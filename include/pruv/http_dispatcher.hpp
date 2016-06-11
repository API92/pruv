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
        virtual bool prepare_for_request(shmem_buffer *) noexcept override;
        virtual bool validate_request(const shmem_buffer *buf) const noexcept
            override;
        virtual bool parse_request(shmem_buffer *buf) noexcept override;
        virtual size_t request_size() const noexcept override;
        virtual size_t request_pos() const noexcept override;
        virtual const char * request_protocol() const noexcept override;
        virtual bool inplace_response(shmem_buffer *buf_in,
                shmem_buffer *buf_out) noexcept override;
        virtual bool prepare_for_response() noexcept override;
        virtual bool parse_response(shmem_buffer *buf) noexcept override;
        virtual bool finish_response() noexcept override;

        http_parser parser;
        size_t request_len = 0;
        bool req_end = false;
        /// Calculated while parsing response.
        /// Used when writing response finished.
        bool keep_alive;
    };

    virtual tcp_http_context * create_connection() noexcept override;
    virtual void free_connection(tcp_context *con) noexcept override;
};

} // namespace pruv
