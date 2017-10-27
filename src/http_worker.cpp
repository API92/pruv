/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/http_worker.hpp>

#include <cinttypes>
#include <cstring>

#include <http_parser.h>
#include <pruv/log.hpp>

namespace pruv {

http_worker::headers::~headers()
{
    clear();
}

http_worker::header * http_worker::headers::emplace_back(
        std::string_view field, std::string_view value) noexcept
{
    header *h = new (std::nothrow) header(field, value);
    if (h)
        push_back(h);
    else
        pruv_log(LOG_EMERG, "Can't allocate memory for header");
    return h;
}

void http_worker::headers::clear() noexcept
{
    while (!empty()) {
        header *h = &front();
        h->remove_from_list();
        delete h;
    }
}

http_worker::body::~body()
{
    clear();
}

http_worker::body_chunk * http_worker::body::emplace_back(char const *data,
        size_t length) noexcept
{
    body_chunk *h = new (std::nothrow) body_chunk(data, length);
    if (h)
        push_back(h);
    else
        pruv_log(LOG_EMERG, "Can't allocate memory for header");
    return h;
}

void http_worker::body::clear() noexcept
{
    while (!empty()) {
        body_chunk *h = &front();
        h->remove_from_list();
        delete h;
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
        w->_url = std::string_view(p, len);
        return 0;
    }

    static int on_header_field_cb(http_parser *parser, char const *p,
            size_t len) noexcept {
        http_worker *w = reinterpret_cast<http_worker *>(parser->data);
        if (!w->_headers.emplace_back(
                    std::string_view(p, len),
                    std::string_view(nullptr, 0)))
            return 1;
        return 0;
    }

    static int on_header_value_cb(http_parser *parser, char const *p,
            size_t len) noexcept {
        http_worker *w = reinterpret_cast<http_worker *>(parser->data);
        if (w->_headers.empty()) {
            pruv_log(LOG_WARNING, "Header field not parsed before value");
            return 1;
        }
        w->_headers.back().value = std::string_view(p, len);
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
    static const req_settings settings;
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = this;
    _url = std::string_view(nullptr, 0);
    _headers.clear();
    _keep_alive = false;
    _zt = strstr(req_meta(), "zt=1");
    _body.clear();

    size_t parselen = request_len() - _zt;
    size_t nparsed = http_parser_execute(&parser, &settings,
            request(), parselen);

    if (nparsed != parselen || _url.empty()) {
        pruv_log(LOG_WARNING, "HTTP parsing error");
        return send_empty_response("400 Bad Request");
    }
    _method = static_cast<http_method>(parser.method);
    return do_response();
}

int http_worker::send_empty_response(char const *status_line) noexcept
{
    if (!start_response(u8"HTTP/1.1", status_line))
        return EXIT_FAILURE;
    if (!keep_alive() && !write_header(u8"Connection", u8"close"))
        return EXIT_FAILURE;
    if (!complete_headers())
        return EXIT_FAILURE;
    if (!complete_body())
        return EXIT_FAILURE;
    if (!send_last_response())
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

bool http_worker::write_response(char const *data, size_t length) noexcept
{
    shmem_buffer *buf = response_buf();
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

bool http_worker::start_response(char const *version,
        char const *status_line) noexcept
{
    _body_pos = 0;
    shmem_buffer *buf = response_buf();
    buf->set_data_size(0);
    return
        write_response(version, strlen(version)) &&
        write_response(" ", 1) &&
        write_response(status_line, strlen(status_line)) &&
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
    _body_pos = response_buf()->data_size();
    return true;
}

bool http_worker::write_body(char const *data, size_t length) noexcept
{
    return write_response(data, length);
}

bool http_worker::complete_body() noexcept
{
    shmem_buffer *buf = response_buf();
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
    if (url().size() > 50)
        return send_empty_response("414 Request-URL Too Long");
    char buf[51] = {};
    memcpy(buf, url().data(), url().size());

    char op[20];
    int64_t value;
    int ret;
    if ((ret = sscanf(buf, "%*[/]%[^/]%*[/]%" SCNd64, op, &value)) != 2)
        return send_empty_response("404 Not Found");

    if (!strcmp(op, "double"))
        value <<= 1;
    else if (!strcmp(op, "square"))
        value *= value;
    else
        return send_empty_response("404 Not Found");

    char res[20] = {};
    snprintf(res, sizeof(res), "%" PRId64, value);

    if (!start_response(u8"HTTP/1.1", u8"200 OK"))
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
