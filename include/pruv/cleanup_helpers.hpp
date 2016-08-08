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

template<typename T>
class scoped_ptr {
public:
    explicit scoped_ptr(T *p) noexcept : p(p) {}
    ~scoped_ptr() { delete p; }
    operator bool () const noexcept { return p; }
    T * get() const noexcept { return p; }
    T * operator -> () const noexcept { return p; }
    T * release() noexcept
    {
        T * res = p;
        p = nullptr;
        return res;
    }

private:
    scoped_ptr(const scoped_ptr &) = delete;
    scoped_ptr(scoped_ptr &&) = delete;
    void operator = (const scoped_ptr &) = delete;
    void operator = (scoped_ptr &&) = delete;

    T *p;
};

} // namespace pruv
