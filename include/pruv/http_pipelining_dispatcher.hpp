/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <http_parser.h>

#include <pruv/dispatcher.hpp>

namespace pruv {

class http_pipelining_dispatcher : public dispatcher {
protected:
    class http_pipelining_context : public tcp_context {
    public:
        http_pipelining_context() noexcept;

    private:
        virtual bool parse_request(shmem_buffer *buf) noexcept override;
        virtual bool get_request(request_meta &r) noexcept override;
        virtual bool inplace_response(const request_meta &r,
                shmem_buffer &buf_in, shmem_buffer &buf_out) noexcept override;
        virtual bool response_ready(const request_meta &r,
                const shmem_buffer &resp_buf) noexcept override;

        virtual bool parse_response(shmem_buffer &buf) noexcept override;
        virtual bool finish_response(const shmem_buffer &buf) noexcept override;

        void prepare_for_request() noexcept;

        http_parser parser_in;
        http_parser parser_out;
        http_parser_settings settings_in;
        http_parser_settings settings_out;
        size_t request_pos;
        size_t request_len;
        size_t resp_sum_size = 0;
        size_t resp_cnt = 0;
        bool req_end;
        bool wait_response;
        /// Calculated while parsing response.
        /// Used when writing response finished.
        bool keep_alive;
    };

    virtual http_pipelining_context * create_connection() noexcept override;
    virtual void free_connection(tcp_context *con) noexcept override;
};

} // namespace pruv
