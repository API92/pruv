/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <uv.h>

namespace pruv {

class tcp_con : private uv_tcp_t {
public:
    bool accept(uv_loop_t *loop, uv_stream_t *server, void *owner,
            void (*deleter)(tcp_con *)) noexcept;
    void close() noexcept;

    template<typename T>
    T base() { return reinterpret_cast<T>(this); }

    template<typename T>
    static tcp_con * from(T *h) { return reinterpret_cast<tcp_con *>(h); }

    void *owner = nullptr;

private:
    static void close_cb(uv_handle_t *handle) noexcept;

    void (*deleter)(tcp_con *) = nullptr;
};

} // namespace pruv
