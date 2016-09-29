/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <http_parser.h>

#include <pruv/list_node.hpp>
#include <pruv/worker_loop.hpp>

namespace pruv {

class http_worker : public worker_loop {
public:
    struct header : list_node<header> {
        header(char const *f, char const *v) : field(f), value(v) {}
        char const *field;
        char const *value;
    };

    struct headers : list_node<header> {
        ~headers();
        header * emplace_back(char const *field, char const *value) noexcept;
        void clear() noexcept;
    };

    struct body_chunk : list_node<body_chunk> {
        body_chunk(char const *data, size_t length) :
            data(data), length(length) {}
        char const *data;
        size_t length;
    };

    struct body : list_node<body_chunk> {
        ~body();
        body_chunk * emplace_back(char const *data, size_t length) noexcept;
        void clear() noexcept;
    };

    http_method method() const { return _method; }
    char const * url() const { return _url; }
    /// Request headers.
    struct headers const & headers() const { return _headers; }
    /// Request body.
    struct body const & body() const { return _body; }
    bool keep_alive() const { return _keep_alive; }
    void set_keep_alive(bool value) { _keep_alive = value; }

protected:
    virtual int handle_request() noexcept override;
    virtual int do_response() noexcept;

    int send_response(char const *response, size_t length) noexcept;
    int send_empty_response(char const *status_line) noexcept;

    bool start_response(char const *version, char const *status_line) noexcept;
    bool write_header(char const *name, char const *value) noexcept;
    bool complete_headers() noexcept;
    bool write_body(char const *data, size_t length) noexcept;
    bool complete_body() noexcept;

private:
    bool write_response(char const *data, size_t length) noexcept;

    struct req_settings;

    // request info
    http_method _method;
    char const *_url = nullptr;
    struct headers _headers;
    struct body _body;
    bool _keep_alive;

    // response info
    size_t _body_pos;
};

} // namespace pruv
