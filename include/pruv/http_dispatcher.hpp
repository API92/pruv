/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <http_parser.h>

#include <pruv/dispatcher.hpp>

namespace pruv {

class http_dispatcher : public dispatcher {
protected:
    class tcp_http_context : public tcp_context {
    public:
        tcp_http_context() noexcept;

    private:
        virtual bool parse_request(shmem_buffer *buf) noexcept override;
        virtual bool get_request(request_meta &r) noexcept override;
        virtual bool inplace_response(const request_meta &r,
                shmem_buffer &buf_in, shmem_buffer &buf_out) noexcept override;
        virtual bool response_ready(shmem_buffer *req_buf,
                const request_meta &r, const shmem_buffer &resp_buf) noexcept
            override;

        virtual bool parse_response(shmem_buffer &buf) noexcept override;
        virtual bool finish_response(const shmem_buffer &buf) noexcept override;

        void prepare_for_request() noexcept;

        http_parser parser;
        size_t request_len;
        bool req_end;
        /// Calculated while parsing response.
        /// Used when writing response finished.
        bool keep_alive;
    };

    virtual tcp_http_context * create_connection() noexcept override;
    virtual void free_connection(tcp_context *con) noexcept override;
};

} // namespace pruv
