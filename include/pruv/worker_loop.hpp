/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <pruv/shmem_buffer.hpp>
#include <pruv/shmem_cache.hpp>
#include <pruv/termination.hpp>

namespace pruv {

class worker_loop {
public:
    static int setup() noexcept;
    int run() noexcept;

protected:
    virtual int handle_request() noexcept = 0;

    bool send_last_response() noexcept;
    char * get_request() const { return request; }
    size_t get_request_len() const { return request_len; }
    shmem_buffer * get_response_buf() const { return response_buf; }
    char const * get_req_meta() const { return req_meta; }

private:
    bool read_line() noexcept;
    bool next_request() noexcept;
    bool clean_after_request() noexcept;

    shmem_cache buf_in_cache;
    shmem_cache buf_out_cache;
    char *request = nullptr;
    size_t request_len = 0;
    shmem_buffer *request_buf = nullptr;
    shmem_buffer *response_buf = nullptr;

    char ln[256];
    char req_meta[256];
    char buf_in_name[256];
    char buf_out_name[256];
};

} // namespace pruv
