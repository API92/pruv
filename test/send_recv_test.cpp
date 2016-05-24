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
    resp_len = std::min(buf->data_size(),
            resp_len + size_t(buf->map_end() - buf->map_ptr()));
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
    if (!resp->reset_defaults(resp_len))
        return EXIT_FAILURE;
    resp->set_data_size(resp_len);
    for (size_t i = 0; i < resp_len; ++i)
        resp->map_ptr()[i] = i;
    return 0;
}

workers_reg::registrator reg("onerequest", onerequest_worker);

struct context {
    uv_loop_t *loop;
    uv_tcp_t con;
    int last;
    uv_connect_t rcon;
    uv_write_t write;
    size_t req_data[2];
    std::vector<char> resp_data;
    size_t resp_len;
    size_t exp_resp_len;
    common_dispatcher<test_context> *d;
};

void connect(context *ctx);

void alloc(uv_handle_t *h, size_t sz, uv_buf_t *buf)
{
    context *ctx = reinterpret_cast<context *>(h->data);
    ctx->resp_data.resize(ctx->resp_len + sz);
    *buf = uv_buf_init(ctx->resp_data.data() + ctx->resp_len, sz);
}

void on_read(uv_stream_t *strm, ssize_t nread, const uv_buf_t *)
{
    context *ctx = reinterpret_cast<context *>(strm->data);
    if (ctx->resp_len == ctx->exp_resp_len) {
        for (size_t i = 0; i < ctx->resp_len; ++i)
            EXPECT_EQ((char)i, ctx->resp_data[i]);
        EXPECT_EQ(UV_EOF, nread);
        if (ctx->resp_len == ctx->exp_resp_len) {
            uv_close((uv_handle_t *)strm, nullptr);
            if (ctx->last)
                ctx->d->stop();
            else
                connect(ctx + 1);
        }
    }
    else {
        ASSERT_TRUE(uv_ok(nread));
        ctx->resp_len += nread;
        EXPECT_GE(ctx->exp_resp_len, ctx->resp_len);
    }
}

void on_write(uv_write_t *req_w, int status)
{
    ASSERT_TRUE(uv_ok(status));
    context *ctx = reinterpret_cast<context *>(req_w->data);
    ctx->resp_len = 0;
    ASSERT_TRUE(uv_ok(uv_read_start(req_w->handle, alloc, on_read)));
}

void on_connect(uv_connect_t *req_con, int status)
{
    ASSERT_TRUE(uv_ok(status));
    context *ctx = reinterpret_cast<context *>(req_con->data);
    ctx->req_data[0] = sizeof(ctx->req_data[1]);
    ctx->req_data[1] = ctx->exp_resp_len;
    uv_buf_t buf = uv_buf_init((char *)&ctx->req_data, sizeof(ctx->req_data));
    ctx->write.data = ctx;
    ASSERT_TRUE(uv_ok(uv_write(&ctx->write, ctx->rcon.handle, &buf, 1,
                    on_write)));
}

void connect(context *ctx)
{
    ctx->d->set_factory_args(2 * sizeof(size_t), ctx->exp_resp_len);
    ctx->con.data = ctx;
    ASSERT_TRUE(uv_ok(uv_tcp_init(ctx->loop, &ctx->con)));
    sockaddr_in6 addr;
    EXPECT_TRUE(uv_ok(uv_ip6_addr("::1", 8000, &addr)));
    ctx->rcon.data = ctx;
    ASSERT_TRUE(uv_ok(uv_tcp_connect(&ctx->rcon, &ctx->con, (sockaddr *)&addr,
                    on_connect)));
}

} // namespace

TEST_F(nonpersistent, onerequest)
{
    common_dispatcher<test_context> d;
    const char *args[] = {"./pruv_test", "--worker", "onerequest", nullptr};
    d.start(&loop, "::1", 8000, 1, "./pruv_test", args);
    size_t lens[] = {0, 1, 4096, REQUEST_CHUNK, RESPONSE_CHUNK,
        10 * RESPONSE_CHUNK, 123, 10 * RESPONSE_CHUNK + 123};
    std::vector<context> ctxs(std::distance(lens, std::end(lens)));
    for (size_t i = 0; i < ctxs.size(); ++i) {
        ctxs[i].last = (i + 1 == ctxs.size());
        ctxs[i].exp_resp_len = lens[i];
        ctxs[i].d = &d;
        ctxs[i].loop = &loop;
    }
    connect(ctxs.data());
    ASSERT_TRUE(uv_ok(uv_run(&loop, UV_RUN_DEFAULT)));
    d.on_loop_exit();
}

} // namespace pruv
