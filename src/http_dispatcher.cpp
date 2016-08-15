/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/http_dispatcher.hpp>

#include <cassert>
#include <memory.h>

#include <pruv/cleanup_helpers.hpp>
#include <pruv/log.hpp>

namespace pruv {

http_dispatcher::tcp_http_context * http_dispatcher::create_connection()
    noexcept
{
    return new (std::nothrow) tcp_http_context;
}

void http_dispatcher::free_connection(tcp_context *con) noexcept
{
    delete con;
}

http_dispatcher::tcp_http_context::tcp_http_context() noexcept
{
    prepare_for_request();
}

void http_dispatcher::tcp_http_context::prepare_for_request() noexcept
{
    http_parser_init(&parser, HTTP_REQUEST);
    request_len = 0;
    req_end = false;
}

bool http_dispatcher::tcp_http_context::parse_request(shmem_buffer *buf)
    noexcept
{
    if (req_end) // Protection from pipelining. Reset it after sending response.
        return false;
    if (!buf)
        return true;

    struct req_settings : http_parser_settings {
        req_settings() {
            memset(this, 0, sizeof(*this));
            on_message_complete = [](http_parser *parser) {
                *reinterpret_cast<bool *>(parser->data) = true;
                // Returning 1 stops parsing after message end.
                // It needed to detect new message after end of this message.
                return 1;
            };
        }
    } static const settings;

    parser.data = &req_end;
    buf->seek(request_len, REQUEST_CHUNK);
    size_t len = buf->data_size() - buf->cur_pos();
    size_t nparsed = http_parser_execute(&parser, &settings,
            buf->map_ptr(), len);
    pruv_log(LOG_DEBUG, "Parsed %" PRIuPTR " bytes of %" PRIuPTR, nparsed, len);
    if (nparsed != len) {
        pruv_log(LOG_WARNING, "HTTP parsing error.");
        return false;
    }
    if (parser.upgrade) {
        pruv_log(LOG_WARNING, "HTTP Upgrade not supported. Disconnecting.");
        return false;
    }

    request_len = buf->data_size();
    return true;
}

bool http_dispatcher::tcp_http_context::get_request(request_meta &r) noexcept
{
    r.pos = 0;
    r.size = request_len;
    r.inplace = false;
    r.protocol = "HTTP";
    return req_end;
}

bool http_dispatcher::tcp_http_context::inplace_response(const request_meta &,
        shmem_buffer &, shmem_buffer &) noexcept
{
    return false;
}

bool http_dispatcher::tcp_http_context::response_ready(const request_meta &,
        const shmem_buffer &resp_buf) noexcept
{
    assert(req_end);
    // Initialize response parser before first chunk of data.
    http_parser_init(&parser, HTTP_RESPONSE);
    return true;
}

bool http_dispatcher::tcp_http_context::parse_response(shmem_buffer &buf)
    noexcept
{
    assert(req_end);
    http_parser_settings parser_settings;
    memset(&parser_settings, 0, sizeof(parser_settings));
    parser.data = &keep_alive;

    parser_settings.on_headers_complete = [](http_parser *parser) {
        *reinterpret_cast<bool *>(parser->data) =
            http_should_keep_alive(parser);
        // Returning 1 needed to stop parsing after headers complete.
        return 1;
    };
    size_t len = std::min(size_t(buf.map_end() - buf.map_ptr()),
                buf.data_size() - buf.cur_pos());
    http_parser_execute(&parser, &parser_settings, buf.map_ptr(), len);
    return true;
}

bool http_dispatcher::tcp_http_context::finish_response(const shmem_buffer &)
    noexcept
{
    // After end of response we can read next request.
    prepare_for_request();
    if (keep_alive) {
        keep_alive = false;
        return true;
    }
    return false;
}

} // namespace pruv
