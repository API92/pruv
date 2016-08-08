/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/tcp_server.hpp>

#include <pruv/log.hpp>

namespace pruv {

bool tcp_server::init(uv_loop_t *loop) noexcept
{
    int r = uv_tcp_init(loop, this);
    if (r < 0) {
        pruv_log_uv_err(LOG_EMERG, "uv_tcp_init", r);
        return false;
    }
    return true;
}

bool tcp_server::bind(sockaddr *addr, unsigned int flags) noexcept
{
    int r = uv_tcp_bind(this, addr, 0);
    if (r < 0) {
        pruv_log_uv_err(LOG_EMERG, "uv_tcp_bind", r);
        return false;
    }
    return true;
}

bool tcp_server::listen(int backlog, uv_connection_cb cb) noexcept
{
    int r = uv_listen(base<uv_stream_t *>(), backlog, cb);
    if (r < 0) {
        pruv_log_uv_err(LOG_EMERG, "uv_listen", r);
        return false;
    }
    return true;
}

void tcp_server::close(uv_close_cb close_cb)
{
    if (!uv_is_closing(base<uv_handle_t *>()))
        uv_close(base<uv_handle_t *>(), nullptr);
}

} // namespace pruv
