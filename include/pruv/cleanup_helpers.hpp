/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <uv.h>

namespace pruv {

struct close_on_return {
    close_on_return(uv_handle_t *handle, uv_close_cb cb) : h(handle), cb(cb) {}
    ~close_on_return()
    {
        if (h)
            uv_close(h, cb);
    }

    uv_handle_t *h;
    uv_close_cb cb;
};

} // namespace pruv
