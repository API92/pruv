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
    worker_loop();
    static int setup(int argc, char const * const *argv) noexcept;
    static int argc() noexcept;
    static char const * const * argv() noexcept;


    int run() noexcept;

protected:
    virtual int handle_request() noexcept = 0;

    bool send_last_response() noexcept;
    char * request() const { return _request; }
    size_t request_len() const { return _request_len; }
    shmem_buffer * response_buf() const { return _response_buf; }
    char const * req_meta() const { return _req_meta; }

private:
    virtual bool emit_last_response_cmd() noexcept;
    virtual bool recv_request_cmd(
            char (&buf_in_name)[256], size_t &buf_in_pos, size_t &buf_in_len,
            char (&buf_out_name)[256], size_t &buf_out_file_size,
            char *meta, size_t meta_len) noexcept;

    bool read_line() noexcept;
    bool next_request() noexcept;
    bool clean_after_request() noexcept;

    shmem_cache _buf_in_cache {true};
    shmem_cache _buf_out_cache {true};
    char *_request = nullptr;
    size_t _request_len = 0;
    shmem_buffer *_request_buf = nullptr;
    shmem_buffer *_response_buf = nullptr;

    char _ln[1024];
    char _req_meta[1024];
    char _buf_in_name[256];
    char _buf_out_name[256];

    static int _argc;
    static char const * const *_argv;
};

} // namespace pruv
