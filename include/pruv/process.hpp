/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <uv.h>

namespace pruv {

struct process : uv_process_t {
    process() noexcept;
    bool start(uv_loop_t *loop, const char *file, const char * const *args,
            void *owner, uv_exit_cb on_exit, void (*deleter)(void *)) noexcept;
    void stop() noexcept;
    static void close_cb(uv_handle_t *handle) noexcept;

    void *owner;
    uv_pipe_t in;
    uv_pipe_t out;
    int wait_close;
    void (*deleter)(void *);
};

} // namespace pruv
