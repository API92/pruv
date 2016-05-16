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
    delete_nothrow(con);
}

bool http_pipelining_dispatcher::http_pipelining_context::prepare_for_request(
        shmem_buffer *buf) noexcept
{
    if (!req_end)
        return true; // from on_end_write_con()
    http_parser_init(&parser_in, HTTP_REQUEST);
    request_pos_ += request_len; // Skip previous request.
    request_len = 0;
    req_end = false;
    if (!buf) {
        request_pos_ = 0;
        return true;
    }

    while (request_pos_ + request_len < buf->data_size()) {
        if (!buf->seek(request_pos_ + request_len))
            return false;
        size_t len = std::min(buf->data_size() - buf->cur_pos(),
                size_t(buf->map_end() - buf->map_ptr()));
        assert(len);
        buf->move_ptr(len);
        if (!parse_request(buf, len))
            return false;
        if (req_end)
            break;
    }
    return buf->seek(buf->data_size());
}

bool http_pipelining_dispatcher::http_pipelining_context::parse_request(
        shmem_buffer *buf, size_t len) noexcept
{
    assert(!req_end);
    parser_in.data = &req_end;
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
    ptrdiff_t diff = buf->cur_pos() - request_pos_ - request_len;
    size_t nparsed = http_parser_execute(&parser_in, &settings,
            buf->map_ptr() - diff, len);
    log(LOG_DEBUG, "Parsed %" PRIu64 " bytes of %" PRId64,
            (uint64_t)nparsed, (int64_t)len);
    if (!req_end && nparsed != len) {
        log(LOG_WARNING, "HTTP parsing error.");
        return false;
    }
    if (parser_in.upgrade) {
        log(LOG_WARNING, "HTTP Upgrade not supported. Close connection.");
        return false;
    }

    request_len += nparsed;
    return true;
}

size_t http_pipelining_dispatcher::http_pipelining_context::request_size() const
    noexcept
{
    return req_end ? request_len : 0;
}

size_t http_pipelining_dispatcher::http_pipelining_context::request_pos() const
    noexcept
{
    return request_pos_;
}

const char *
http_pipelining_dispatcher::http_pipelining_context::request_protocol() const
    noexcept
{
    return "HTTP";
}

bool http_pipelining_dispatcher::http_pipelining_context::inplace_response(
        shmem_buffer *buf) noexcept
{
    assert(!buf);
    return buf;
}

bool http_pipelining_dispatcher::http_pipelining_context::prepare_for_response()
    noexcept
{
    http_parser_init(&parser_out, HTTP_RESPONSE);
    keep_alive = false;
    return true;
}

bool http_pipelining_dispatcher::http_pipelining_context::parse_response(
        shmem_buffer *buf) noexcept
{
    http_parser_settings parser_settings;
    memset(&parser_settings, 0, sizeof(parser_settings));
    parser_out.data = &keep_alive;
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
    http_parser_execute(&parser_out, &parser_settings, buf->map_ptr(), len);
    return true;
}

bool http_pipelining_dispatcher::http_pipelining_context::finish_response()
    noexcept
{
    return keep_alive;
}

} // namespace pruv
