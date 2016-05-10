/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/process.hpp>

#include <cassert>
#include <memory.h>

#include <pruv/cleanup_helpers.hpp>
#include <pruv/log.hpp>

namespace pruv {

process::process() noexcept
{
    memset(this, 0, sizeof(*this));
    data = this;
    in.data = this;
    out.data = this;
}

bool process::start(uv_loop_t *loop, const char *file, const char * const *args,
        void *owner, uv_exit_cb on_exit, void (*deleter)(void *)) noexcept
{
    assert(loop);
    this->owner = owner;
    this->deleter = deleter;

    int r;
    ++wait_close;
    close_on_return close_in((uv_handle_t *)&in, close_cb);
    if ((r = uv_pipe_init(loop, &in, 0)) < 0) {
        log_uv_error(LOG_ERR, "process::start uv_pipe_init in", r);
        if (deleter)
            deleter(this);
        return false;
    }

    ++wait_close;
    close_on_return close_out((uv_handle_t *)&out, close_cb);
    if ((r = uv_pipe_init(loop, &out, 0)) < 0) {
        log_uv_error(LOG_ERR, "process::start uv_pipe_init out", r);
        return false;
    }

    uv_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.exit_cb = on_exit;
    options.file = file;
    // http://pubs.opengroup.org/onlinepubs/009604499/functions/exec.html
    // RATIONALE section describes, why args can't be const.
    // It is only due to limitation of the ISO C.
    // In fact args is completely constant.
    options.args = const_cast<char **>(args);
    options.flags = UV_PROCESS_WINDOWS_HIDE;
    options.stdio_count = 2;
    uv_stdio_container_t stdio[2];
    options.stdio = stdio;
    options.stdio[0].flags = uv_stdio_flags(UV_CREATE_PIPE | UV_READABLE_PIPE);
    options.stdio[0].data.stream = (uv_stream_t *)&in;
    options.stdio[1].flags = uv_stdio_flags(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    options.stdio[1].data.stream = (uv_stream_t *)&out;

    ++wait_close;
    close_on_return close_this_proc((uv_handle_t *)this, close_cb);
    if ((r = uv_spawn(loop, (uv_process_t *)this, &options)) < 0) {
        log_uv_error(LOG_ERR, "process::start uv_spawn", r);
        return false;
    }

    close_in.h = close_out.h = close_this_proc.h = nullptr;
    return true;
}

void process::stop() noexcept
{
    uv_close((uv_handle_t *)this, close_cb);
    uv_close((uv_handle_t *)&out, close_cb);
    uv_close((uv_handle_t *)&in, close_cb);
}

void process::close_cb(uv_handle_t *handle) noexcept
{
    process *p = reinterpret_cast<process *>(handle->data);
    if (!--p->wait_close && p->deleter)
        p->deleter(p);
}

} // namespace pruv
