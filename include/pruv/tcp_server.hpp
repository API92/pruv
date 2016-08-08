/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <uv.h>

namespace pruv {

class tcp_server : private uv_tcp_t {
public:
    bool init(uv_loop_t *loop) noexcept;
    bool bind(sockaddr *addr, unsigned int flags) noexcept;
    bool listen(int backlog, uv_connection_cb cb) noexcept;
    /// Can be called only after init().
    void close(uv_close_cb close_cb);

    template<typename T>
    T base() { return reinterpret_cast<T>(this); }

    template<typename T>
    static tcp_server * from(T *h) { return reinterpret_cast<tcp_server *>(h); }

};

} // namespace pruv
