/*
 * Copyright (C) Andrey Pikas
 */

#include <gtest/gtest.h>
#include <uv.h>

#include "fixtures.hpp"
#include "common_dispatcher.hpp"
#include "workers_reg.hpp"

namespace pruv {

struct nonpersistent : loop_fixture {};

namespace {

struct test_context : common_dispatcher<test_context>::tcp_context {
    size_t exp_req_len;
    size_t exp_resp_len;
    size_t resp_len = 0;
    int cnt = 0;
    bool req_end = true;

    test_context(size_t exp_req_len, size_t exp_resp_len)
        : exp_req_len(exp_req_len), exp_resp_len(exp_resp_len) {}

    virtual bool prepare_for_request(shmem_buffer *buf) noexcept override;
    virtual bool validate_request(const shmem_buffer *buf) const noexcept
        override;
    virtual bool parse_request(shmem_buffer *buf) noexcept override;
    virtual size_t request_size() const noexcept override;
    virtual size_t request_pos() const noexcept override;
    virtual const char * request_protocol() const noexcept override;
    virtual bool inplace_response(shmem_buffer *buf) noexcept override;
    virtual bool prepare_for_response() noexcept override;
    virtual bool parse_response(shmem_buffer *buf) noexcept override;
    virtual bool finish_response() noexcept override;
};

bool test_context::prepare_for_request(shmem_buffer *buf) noexcept
{
    if (!buf)
        return true;
    EXPECT_TRUE(req_end);
    req_end = false;
    ++cnt;
    EXPECT_EQ(1, cnt);
    EXPECT_EQ(0, buf->cur_pos());
    return true;
}

bool test_context::validate_request(const shmem_buffer *buf) const noexcept
{
    EXPECT_GE(exp_req_len, buf->data_size());
    return true;
}

bool test_context::parse_request(shmem_buffer *buf) noexcept
{
    EXPECT_GE(exp_req_len, buf->data_size());
    req_end = (buf->data_size() == exp_req_len);
    return true;
}

size_t test_context::request_size() const noexcept
{
    return req_end ? exp_req_len : 0;
}

size_t test_context::request_pos() const noexcept
{
    EXPECT_TRUE(req_end);
    return 0;
}

const char * test_context::request_protocol() const noexcept
{
    return "HTTP";
}

bool test_context::inplace_response(shmem_buffer *buf) noexcept
{
    EXPECT_EQ(nullptr, buf);
    EXPECT_TRUE(req_end);
    return false;
}

bool test_context::prepare_for_response() noexcept
{
    EXPECT_TRUE(req_end);
    EXPECT_EQ(0, resp_len);
    return true;
}

bool test_context::parse_response(shmem_buffer *buf) noexcept
{
    EXPECT_LT(resp_len, buf->data_size());
    resp_len = buf->data_size();
    EXPECT_GE(exp_resp_len, resp_len);
    return true;
}

bool test_context::finish_response() noexcept
{
    EXPECT_EQ(resp_len, exp_resp_len);
    req_end = false;
    return false; // close connection
}

int onerequest_worker(char *req, size_t req_len, shmem_buffer *resp)
{
    if (req_len != 2 * sizeof(size_t))
        return EXIT_FAILURE;
    size_t *p = reinterpret_cast<size_t *>(req);
    if (p[0] != sizeof(p[1]))
        return EXIT_FAILURE;
    size_t resp_len = p[1];
    if (!resp->reset_defaults(RESPONSE_CHUNK))
        return EXIT_FAILURE;
    resp->set_data_size(resp_len);
    for (size_t i = 0; i < resp_len; ++i)
        resp->map_ptr()[i] = i;
    return 0;
}

workers_reg::registrator reg("onerequest_worker", onerequest_worker);

struct requests {
    uv_connect_t con;
    uv_write_t write;
    size_t req_data[2];
    char resp_data[512];
    size_t resp_len;
    dispatcher *d;
};

template<typename T, int n>
constexpr size_t size(T (&)[n]) { return n; }

void alloc(uv_handle_t *h, size_t, uv_buf_t *buf)
{
    requests *req = reinterpret_cast<requests *>(h->data);
    *buf = uv_buf_init(req->resp_data + req->resp_len,
            size(req->resp_data) - req->resp_len);
}

void on_read(uv_stream_t *strm, ssize_t nread, const uv_buf_t *)
{
    requests *req = reinterpret_cast<requests *>(strm->data);
    if (req->resp_len == 256)
        EXPECT_EQ(UV_EOF, nread);
    else {
        ASSERT_TRUE(uv_ok(nread));
        req->resp_len += nread;
        EXPECT_GE(256, req->resp_len);
        if (req->resp_len == 256) {
            uv_close((uv_handle_t *)strm, nullptr);
            req->d->stop();
        }
    }
}

void on_write(uv_write_t *req_w, int status)
{
    ASSERT_TRUE(uv_ok(status));
    requests *req = reinterpret_cast<requests *>(req_w->data);
    req->resp_len = 0;
    ASSERT_TRUE(uv_ok(uv_read_start(req_w->handle, alloc, on_read)));
}

void on_connect(uv_connect_t *req_con, int status)
{
    ASSERT_TRUE(uv_ok(status));
    requests *req = reinterpret_cast<requests *>(req_con->data);
    req->req_data[0] = sizeof(req->req_data[1]);
    req->req_data[1] = 256;
    uv_buf_t buf = uv_buf_init((char *)&req->req_data, sizeof(req->req_data));
    req->write.data = req;
    ASSERT_TRUE(uv_ok(uv_write(&req->write, req->con.handle, &buf, 1,
                    on_write)));
}

} // namespace

TEST_F(nonpersistent, onerequest)
{
    common_dispatcher<test_context> d(2 * sizeof(size_t), 256);
    const char *args[] = {"./pruv_test", "--worker", "onerequest_worker", nullptr};
    d.start(&loop, "::1", 8000, 1, "./pruv_test", args);
    requests req;
    req.d = &d;
    uv_tcp_t con;
    con.data = &req;
    ASSERT_TRUE(uv_ok(uv_tcp_init(&loop, &con)));
    sockaddr_in6 addr;
    EXPECT_TRUE(uv_ok(uv_ip6_addr("::1", 8000, &addr)));
    req.con.data = &req;
    ASSERT_TRUE(uv_ok(uv_tcp_connect(&req.con, &con, (sockaddr *)&addr,
                    on_connect)));
    ASSERT_TRUE(uv_ok(uv_run(&loop, UV_RUN_DEFAULT)));
    d.on_loop_exit();
}

} // namespace pruv
