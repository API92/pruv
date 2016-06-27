/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/tcp_con.hpp>

#include <pruv/cleanup_helpers.hpp>
#include <pruv/log.hpp>

namespace pruv {

bool tcp_con::accept(uv_loop_t *loop, uv_stream_t *server, void *owner,
        void (*deleter)(tcp_con *)) noexcept
{
    this->owner = owner;
    this->deleter = deleter;

    int r;
    if ((r = uv_tcp_init(loop, this)) < 0) {
        log_uv_err(LOG_ERR, "tcp_stream::accept uv_tcp_init", r);
        close();
        return false;
    }

    if ((r = uv_accept(server, base<uv_stream_t *>())) < 0) {
        log_uv_err(LOG_ERR, "tcp_stream::accept uv_accept", r);
        close();
        return false;
    }

    log(LOG_DEBUG, "tcp_stream::accept Connection accepted");

    if ((r = uv_tcp_keepalive(this, 1, 20)) < 0) {
        log_uv_err(LOG_ERR, "tcp_con::accept uv_tcp_keepalive", r);
        return true;
    }

    if ((r = uv_tcp_nodelay(this, 0))) {
        log_uv_err(LOG_ERR, "tcp_con::accept uv_tcp_nodelay", r);
        return true;
    }

    uv_os_fd_t fd;
    if ((r = uv_fileno(base<uv_handle_t *>(), &fd)) < 0) {
        log_uv_err(LOG_ERR, "tcp_con::accept uv_fileno", r);
        return true;
    }

    const unsigned intvl = 5;
    if ((r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl))))
        log_syserr(LOG_ERR, "tcp_con::accept setsockopt(TCP_KEEPINTVL)");

    const unsigned cnt = 4;
    if ((r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt))))
        log_syserr(LOG_ERR, "tcp_con::accept setsockopt(TCP_KEEPCNT)");
    return true;
}

void tcp_con::close() noexcept
{
    uv_close(base<uv_handle_t *>(), close_cb);
}

void tcp_con::close_cb(uv_handle_t *handle) noexcept
{
    tcp_con *strm = (tcp_con *)handle;
    if (strm->deleter)
        strm->deleter(strm);
}

} // namespace pruv
