/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/http_worker.hpp>

#include <cinttypes>
#include <cstring>

#include <http_parser.h>
#include <falloc/cache.hpp>
#include <pruv/log.hpp>

namespace pruv {

typedef falloc::object_cache<http_worker::header> header_cache;
header_cache::set_stat_interval set_header_stat(0x100000);
typedef falloc::object_cache<http_worker::body_chunk> body_cache;
body_cache::set_stat_interval set_body_stat(0x10000);

http_worker::headers::~headers()
{
    clear();
}

http_worker::header * http_worker::headers::emplace_back(char const *field,
        char const *value) noexcept
{
    header *h = header_cache::alloc_with_new_handler();
    if (h) {
        new (h) header(field, value);
        push_back(h);
    }
    else
        pruv_log(LOG_ERR, "Can't allocate memory for header");
    return h;
}

void http_worker::headers::clear() noexcept
{
    while (!empty()) {
        header *h = &front();
        h->remove_from_list();
        header_cache::free(h);
    }
}

http_worker::body::~body()
{
    clear();
}

http_worker::body_chunk * http_worker::body::emplace_back(char const *data,
        size_t length) noexcept
{
    body_chunk *h = body_cache::alloc_with_new_handler();
    if (h) {
        new (h) body_chunk(data, length);
        push_back(h);
    }
    else
        pruv_log(LOG_ERR, "Can't allocate memory for header");
    return h;
}

void http_worker::body::clear() noexcept
{
    while (!empty()) {
        body_chunk *h = &front();
        h->remove_from_list();
        body_cache::free(h);
    }
}

struct http_worker::req_settings : http_parser_settings {
    req_settings() noexcept {
        memset(this, 0, sizeof(*this));
        on_message_complete = on_message_complete_cb;
        on_url = on_url_cb;
        on_header_field = on_header_field_cb;
        on_header_value = on_header_value_cb;
        on_body = on_body_cb;
    }

    static int on_message_complete_cb(http_parser *parser) noexcept {
        http_worker *w = reinterpret_cast<http_worker *>(parser->data);
        w->_keep_alive = http_should_keep_alive(parser);
        // Returning 1 stops parsing after message end.
        // It needed to detect new message after end of this message.
        return 1;
    }

    static int on_url_cb(http_parser *parser, char const *p, size_t len)
        noexcept {
        http_worker *w = reinterpret_cast<http_worker *>(parser->data);
        w->_url = p;
        size_t end_pos = p - w->get_request() + len;
        if (end_pos >= w->get_request_len()) {
            pruv_log(LOG_ERR, "Parsed URI path too long.");
            return 1;
        }
        w->get_request()[end_pos] = 0;
        return 0;
    }

    static int on_header_field_cb(http_parser *parser, char const *p,
            size_t len) noexcept {
        http_worker *w = reinterpret_cast<http_worker *>(parser->data);
        size_t end_pos = p - w->get_request() + len;
        if (end_pos >= w->get_request_len()) {
            pruv_log(LOG_ERR, "Parsed header field is too long");
            return 1;
        }
        w->get_request()[end_pos] = 0;
        if (!w->_headers.emplace_back(p, nullptr))
            return 1;
        return 0;
    }

    static int on_header_value_cb(http_parser *parser, char const *p,
            size_t len) noexcept {
        http_worker *w = reinterpret_cast<http_worker *>(parser->data);
        size_t end_pos = p - w->get_request() + len;
        if (end_pos >= w->get_request_len()) {
            pruv_log(LOG_ERR, "Parsed header value is too long");
            return 1;
        }
        w->get_request()[end_pos] = 0;
        if (w->_headers.empty()) {
            pruv_log(LOG_WARNING, "Header field not parsed before value");
            return 1;
        }
        w->_headers.back().value = p;
        return 0;
    }

    static int on_body_cb(http_parser *parser, char const *p, size_t len)
        noexcept {
        http_worker *w = reinterpret_cast<http_worker *>(parser->data);
        if (!w->_body.emplace_back(p, len))
            return 1;
        return 0;
    }
};

int http_worker::handle_request() noexcept
{
    if (interruption_requested())
        return send_error_response();

    static const req_settings settings;
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = this;
    _url = nullptr;
    _headers.clear();
    _keep_alive = false;

    size_t nparsed = http_parser_execute(&parser, &settings,
            get_request(), get_request_len());

    if (nparsed != get_request_len() || !_url) {
        pruv_log(LOG_WARNING, "HTTP parsing error");
        return send_error_response();
    }
    _method = static_cast<http_method>(parser.method);
    return do_response();
}

int http_worker::send_error_response() noexcept
{
    static char const resp[] =
        u8"HTTP/1.1 400 Bad Request\r\n"
        u8"Content-Length: 14\r\n"
        u8"Content-Type: text/html; charset=utf-8\r\n"
        u8"Connection: close\r\n"
        u8"\r\n"
        u8"Bad Request!\r\n"
    ;
    static const size_t resp_len = strlen(resp);
    return send_response(resp, resp_len);
}

int http_worker::send_response(char const *response, size_t length) noexcept
{
    shmem_buffer *buf_out = get_response_buf();
    assert(buf_out);
    buf_out->seek(0, RESPONSE_CHUNK);
    buf_out->set_data_size(length);

    if (buf_out->map_end() - buf_out->map_begin() < (ptrdiff_t)length)
        if (!buf_out->reset_defaults(buf_out->data_size()))
            return EXIT_FAILURE;
    assert(!buf_out->cur_pos());
    memcpy(buf_out->map_ptr(), response, buf_out->data_size());
    return send_last_response() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool http_worker::write_response(char const *data, size_t length) noexcept
{
    shmem_buffer *buf = get_response_buf();
    while (length) {
        if (!buf->seek(buf->data_size(), RESPONSE_CHUNK))
            return false;
        size_t n = std::min<size_t>(length, buf->map_end() - buf->map_ptr());
        memcpy(buf->map_ptr(), data, n);
        buf->move_ptr(n);
        buf->set_data_size(buf->data_size() + n);
        length -= n;
    }
    return true;
}

bool http_worker::start_response(char const *status_line) noexcept
{
    _body_pos = 0;
    shmem_buffer *buf = get_response_buf();
    buf->set_data_size(0);
    return write_response(status_line, strlen(status_line)) &&
        write_response("\r\n", 2);
}

bool http_worker::write_header(char const *name, char const *value) noexcept
{
    return
        write_response(name, strlen(name)) && write_response(": ", 2) &&
        write_response(value, strlen(value)) && write_response("\r\n", 2);
}

bool http_worker::complete_headers() noexcept
{
    // Prepare space for writing content-length value in complete_body().
    if (!write_header("Content-Length", "     ""     ""     ""     ") ||
        !write_response("\r\n", 2))
        return false;
    _body_pos = get_response_buf()->data_size();
    return true;
}

bool http_worker::write_body(char const *data, size_t length) noexcept
{
    return write_response(data, length);
}

bool http_worker::complete_body() noexcept
{
    shmem_buffer *buf = get_response_buf();
    if (buf->data_size() < _body_pos)
        return false;
    size_t content_length = buf->data_size() - _body_pos;
    char s[21];
    int n = snprintf(s, sizeof(s), "%" PRIdPTR, content_length);
    if (n < 0 || n >= (int)sizeof(s))
        return false;
    size_t w = 0;
    while (n) {
        if (!buf->seek(_body_pos - 24 + w, RESPONSE_CHUNK))
            return false;
        size_t c = std::min<size_t>(n, buf->map_end() - buf->map_ptr());
        memcpy(buf->map_ptr(), s + w, c);
        w += c;
        n -= c;
    }
    return true;
}

int http_worker::do_response() noexcept
{
    char op[20];
    int64_t value;
    int ret;
    if ((ret = sscanf(url(), "%*[/]%[^/]%*[/]%" SCNd64, op, &value)) != 2)
        return send_error_response();

    if (!strcmp(op, "double"))
        value <<= 1;
    else if (!strcmp(op, "square"))
        value *= value;
    else
        return send_error_response();

    char res[20] = {};
    snprintf(res, sizeof(res), "%" PRId64, value);

    if (!start_response(u8"HTTP/1.1 200 OK"))
        return EXIT_FAILURE;
    if (!write_header(u8"Content-Type", u8"text/html; charset=utf-8"))
        return EXIT_FAILURE;
    if (!keep_alive() && !write_header(u8"Connection", u8"close"))
        return EXIT_FAILURE;
    if (!complete_headers())
        return EXIT_FAILURE;
    if (!write_body(res, strlen(res)) || !write_body("\r\n", 2))
        return EXIT_FAILURE;
    if (!complete_body())
        return EXIT_FAILURE;
    if (!send_last_response())
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

} // namespace pruv
