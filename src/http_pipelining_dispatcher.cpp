/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/http_pipelining_dispatcher.hpp>

#include <memory.h>

#include <pruv/cleanup_helpers.hpp>
#include <pruv/log.hpp>

namespace pruv {

http_pipelining_dispatcher::http_pipelining_context *
http_pipelining_dispatcher::create_connection() noexcept
{
    return new (std::nothrow) http_pipelining_context;
}

void http_pipelining_dispatcher::free_connection(tcp_context *con) noexcept
{
    delete con;
}

http_pipelining_dispatcher::http_pipelining_context::http_pipelining_context()
    noexcept :
    request_pos(0), req_end(false), wait_response(false), keep_alive(false)
{
    prepare_for_request();
    http_parser_init(&parser_out, HTTP_RESPONSE);
}

void http_pipelining_dispatcher::http_pipelining_context::prepare_for_request()
    noexcept
{
    request_pos += request_len;
    request_len = 0;
    http_parser_init(&parser_in, HTTP_REQUEST);
}

bool http_pipelining_dispatcher::http_pipelining_context::parse_request(
        shmem_buffer *buf) noexcept
{
    if (!buf) {
        request_pos = request_len = 0;
        return true;
    }
    if (buf->data_size() >= 1024 * 1024)
        return false;
    if (req_end)
        return true;

    // Settings for request parser
    struct settings : http_parser_settings {
        settings() {
            memset(this, 0, sizeof(*this));
            on_message_complete = cb;
        }
        static int cb(http_parser *parser) {
            *reinterpret_cast<bool *>(parser->data) = true;
            // Returning 1 stops parsing after message end.
            // It needed to detect new message after end of this message.
            return 1;
        }
    } static const settings_in;
    parser_in.data = &req_end;


    while (!req_end && request_pos + request_len < buf->data_size()) {
        if (!buf->seek(request_pos + request_len, REQUEST_CHUNK))
            return false;
        size_t len = std::min(buf->data_size() - buf->cur_pos(),
                size_t(buf->map_end() - buf->map_ptr()));
        assert(len);
        size_t nparsed = http_parser_execute(&parser_in, &settings_in,
                buf->map_ptr(), len);
        pruv_log(LOG_DEBUG, "Parsed %" PRIuPTR " bytes of %" PRIuPTR " starting"
                " from %" PRIuPTR, nparsed, len, request_pos + request_len);
        if (!req_end && nparsed != len) {
            pruv_log(LOG_WARNING, "HTTP parsing error.");
            return false;
        }
        if (parser_in.upgrade) {
            pruv_log(LOG_WARNING, "HTTP Upgrade not supported. "
                    "Close connection.");
            return false;
        }
        request_len += nparsed;
    }
    return true;
}

bool http_pipelining_dispatcher::http_pipelining_context::inplace_response(
    const request_meta &, shmem_buffer &, shmem_buffer &) noexcept
{
    return false;
}

bool http_pipelining_dispatcher::http_pipelining_context::get_request(
        request_meta &r) noexcept
{
    r.pos = request_pos;
    r.size = request_len;
    r.meta = nullptr;
    r.inplace = false;
    if (req_end && !wait_response) {
        wait_response = true;
        prepare_for_request();
        return true;
    }
    else
        return false;
}

bool http_pipelining_dispatcher::http_pipelining_context::response_ready(
        const request_meta &, const shmem_buffer &resp_buf) noexcept
{
    if ((resp_sum_size += resp_buf.data_size()) >= 10 * 1024 * 1024)
        return false;
    if (++resp_cnt > 10)
        return false;
    req_end = wait_response = false;
    return true;
}

bool http_pipelining_dispatcher::http_pipelining_context::parse_response(
        shmem_buffer &buf) noexcept
{
    // Settings for response parser
    struct settings : http_parser_settings {
        settings() {
            memset(this, 0, sizeof(*this));
            on_headers_complete = cb;
        }
        static int cb(http_parser *parser) {
            *reinterpret_cast<bool *>(parser->data) =
                http_should_keep_alive(parser);
            // Returning 1 needed to stop parsing after headers complete.
            return 1;
        };
    } static const settings_out;
    parser_out.data = &keep_alive;

    size_t len = std::min(size_t(buf.map_end() - buf.map_ptr()),
                buf.data_size() - buf.cur_pos());
    http_parser_execute(&parser_out, &settings_out, buf.map_ptr(), len);
    return true;
}

bool http_pipelining_dispatcher::http_pipelining_context::finish_response(
        const shmem_buffer &buf) noexcept
{
    resp_sum_size -= buf.data_size();
    --resp_cnt;
    if (keep_alive) {
        http_parser_init(&parser_out, HTTP_RESPONSE);
        keep_alive = false;
        return true;
    }
    return false;
}

} // namespace pruv
