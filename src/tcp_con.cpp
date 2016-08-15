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
        pruv_log_uv_err(LOG_ERR, "uv_tcp_init", r);
        close();
        return false;
    }

    if ((r = uv_accept(server, base<uv_stream_t *>())) < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_accept", r);
        close();
        return false;
    }

    pruv_log(LOG_DEBUG, "Connection accepted");

    if ((r = uv_tcp_nodelay(this, 0))) {
        pruv_log_uv_err(LOG_ERR, "uv_tcp_nodelay", r);
        return true;
    }
    return true;
}

void tcp_con::close() noexcept
{
    if (!uv_is_closing(base<uv_handle_t *>()))
        uv_close(base<uv_handle_t *>(), close_cb);
}

void tcp_con::close_cb(uv_handle_t *handle) noexcept
{
    tcp_con *strm = (tcp_con *)handle;
    if (strm->deleter)
        strm->deleter(strm);
}

bool tcp_con::set_tcp_keepalive(int enable, unsigned delay, unsigned interval,
        unsigned cnt) noexcept
{
    bool ret = true;
    int r;
    if ((r = uv_tcp_keepalive(this, 1, 20)) < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_tcp_keepalive", r);
        ret = false;
    }

    uv_os_fd_t fd;
    if ((r = uv_fileno(base<uv_handle_t *>(), &fd)) < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_fileno", r);
        ret = false;
    }

    if ((r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval,
                    sizeof(interval)))) {
        pruv_log_syserr(LOG_ERR, "setsockopt(TCP_KEEPINTVL)");
        ret = false;
    }

    if ((r = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)))) {
        pruv_log_syserr(LOG_ERR, "setsockopt(TCP_KEEPCNT)");
        ret = false;
    }
    return ret;
}

bool tcp_con::read_start(uv_alloc_cb alloc_cb, uv_read_cb read_cb) noexcept
{
    // Start connection reading.
    int r = uv_read_start(base<uv_stream_t *>(), alloc_cb, read_cb);
    if (r < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_read_start", r);
        return false;
    }
    pruv_log(LOG_DEBUG, "reading connection begin");
    return true;
}

bool tcp_con::read_stop() noexcept
{
    int r = uv_read_stop(base<uv_stream_t *>());
    if (r < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_read_stop", r);
        return false;
    }
    pruv_log(LOG_DEBUG, "Reading connection end");
    return true;
}

} // namespace pruv
