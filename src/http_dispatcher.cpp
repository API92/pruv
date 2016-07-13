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

bool http_dispatcher::tcp_http_context::prepare_for_request(shmem_buffer *buf)
    noexcept
{
    if (req_end)
        return !buf; // Protection from pipelining. Reset it after response end.
    assert(!request_len);
    http_parser_init(&parser, HTTP_REQUEST);
    request_len = 0;
    req_end = false;
    return true;
}

bool http_dispatcher::tcp_http_context::validate_request(const shmem_buffer *)
    const noexcept
{
    return true;
}

bool http_dispatcher::tcp_http_context::parse_request(shmem_buffer *buf)
    noexcept
{
    assert(buf);
    if (req_end) // Protection from pipelining. Reset it after sending response.
        return false;
    parser.data = &req_end;
    http_parser_settings settings;
    memset(&settings, 0, sizeof(settings));
    struct cb {
        static int on_msg_compl(http_parser *parser) {
            *reinterpret_cast<bool *>(parser->data) = true;
            // Returning 1 stops parsing after message end.
            // It needed to detect new message after end of this message.
            return 1;
        }
    };
    settings.on_message_complete = cb::on_msg_compl;
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
        pruv_log(LOG_WARNING, "HTTP Upgrade not supported. Close connection.");
        return false;
    }

    request_len = buf->data_size();
    return true;
}

size_t http_dispatcher::tcp_http_context::request_size() const noexcept
{
    return req_end ? request_len : 0;
}

size_t http_dispatcher::tcp_http_context::request_pos() const noexcept
{
    return 0;
}

const char * http_dispatcher::tcp_http_context::request_protocol() const
    noexcept
{
    return "HTTP";
}

bool http_dispatcher::tcp_http_context::inplace_response(shmem_buffer *buf_in,
        shmem_buffer *buf_out) noexcept
{
    assert(!buf_in);
    assert(!buf_out);
    return false;
}

bool http_dispatcher::tcp_http_context::prepare_for_response() noexcept
{
    assert(req_end);
    // Initialize response parser before first chunk of data.
    http_parser_init(&parser, HTTP_RESPONSE);
    keep_alive = false;
    return true;
}

bool http_dispatcher::tcp_http_context::parse_response(shmem_buffer *buf)
    noexcept
{
    assert(buf);
    assert(req_end);
    http_parser_settings parser_settings;
    memset(&parser_settings, 0, sizeof(parser_settings));
    parser.data = &keep_alive;
    struct parser_cb {
        static int on_hdrs_end(http_parser *parser) {
            *reinterpret_cast<bool *>(parser->data) =
                http_should_keep_alive(parser);
            // Returning 1 needed to stop parsing after headers complete.
            return 1;
        }
    };
    parser_settings.on_headers_complete = parser_cb::on_hdrs_end;
    size_t len = std::min(size_t(buf->map_end() - buf->map_ptr()),
                buf->data_size() - buf->cur_pos());
    http_parser_execute(&parser, &parser_settings, buf->map_ptr(), len);
    return true;
}

bool http_dispatcher::tcp_http_context::finish_response() noexcept
{
    // After end of response we can read next request.
    req_end = false;
    request_len = 0;
    return keep_alive;
}

} // namespace pruv
