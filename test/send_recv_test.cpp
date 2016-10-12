/*
 * Copyright (C) Andrey Pikas
 */

#include <memory>

#include <gtest/gtest.h>
#include <uv.h>

#include "fixtures.hpp"
#include "common_dispatcher.hpp"
#include "workers_reg.hpp"

namespace pruv {

namespace {

struct test_context : common_dispatcher<test_context>::tcp_context {
    size_t exp_req_len = 0;
    size_t exp_resp_len = 0;
    size_t resp_len = 0;
    bool req_end = false;
    bool wait_response = false;
    bool keep_alive = true;

    virtual bool parse_request(shmem_buffer *buf) noexcept override;
    virtual bool get_request(request_meta &r) noexcept override;
    virtual bool inplace_response(const request_meta &r,
            shmem_buffer &buf_in, shmem_buffer &buf_out) noexcept override;

    virtual bool response_ready(const request_meta &r,
            const shmem_buffer &resp_buf) noexcept override;
    virtual bool parse_response(shmem_buffer &buf) noexcept override;
    virtual bool finish_response(const shmem_buffer &buf) noexcept override;
};

bool test_context::parse_request(shmem_buffer *buf) noexcept
{
    if (!buf)
        return true;
    EXPECT_EQ(req_end, wait_response);
    if (req_end)
        return true;
    EXPECT_GE(exp_req_len, buf->data_size());
    req_end = (buf->data_size() == exp_req_len);
    if (!buf->map_offset() && buf->data_size() >= 2 * sizeof(size_t))
        keep_alive = reinterpret_cast<const size_t *>(buf->map_begin())[1];
    return true;
}

bool test_context::get_request(request_meta &r) noexcept
{
    r.pos = 0;
    r.size = exp_req_len;
    static char meta[] =
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789"
            "01234567890123456789012345678901234567890123456789";
    r.meta = meta;
    r.inplace = false;
    if (req_end) {
        if (wait_response)
            return false;
        wait_response = true;
        return true;
    }
    else
        return false;
}

bool test_context::inplace_response(const request_meta &,
        shmem_buffer &, shmem_buffer &) noexcept
{
    EXPECT_TRUE(false);
    return false;
}

bool test_context::response_ready(const request_meta &r,
        const shmem_buffer &resp_buf) noexcept
{
    EXPECT_TRUE(req_end);
    EXPECT_EQ(0U, resp_len);
    resp_len = 0;
    return true;
}

bool test_context::parse_response(shmem_buffer &buf) noexcept
{
    EXPECT_LE(resp_len, buf.data_size());
    resp_len = std::min(buf.data_size(),
            resp_len + size_t(buf.map_end() - buf.map_ptr()));
    EXPECT_GE(exp_resp_len, resp_len);
    return true;
}

bool test_context::finish_response(const shmem_buffer &) noexcept
{
    EXPECT_EQ(resp_len, exp_resp_len);
    req_end = wait_response = false;
    resp_len = 0;
    return keep_alive;
}

struct context {
    uv_loop_t *loop;
    std::unique_ptr<uv_tcp_t> con;
    context *next;
    int keep_alive;
    std::unique_ptr<uv_connect_t> rcon;
    uv_write_t write;
    std::vector<char> req_data;
    std::vector<char> resp_data;
    size_t resp_len;
    size_t exp_resp_len;
    common_dispatcher<test_context> *d;

    virtual ~context() {}
    virtual void create_request() = 0;
    virtual void check_response() const = 0;
};

void connect(context *ctx);
void on_connect(uv_connect_t *req_con, int status);

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
        if (ctx->keep_alive)
            EXPECT_EQ(0, nread);
        else {
            EXPECT_EQ(UV_EOF, nread);
            uv_close((uv_handle_t *)strm, nullptr);
            if (ctx->next)
                connect(ctx->next);
            else
                ctx->d->stop();
        }
    }
    else {
        ASSERT_TRUE(uv_ok(nread));
        ctx->resp_len += nread;
        EXPECT_GE(ctx->exp_resp_len, ctx->resp_len);
        if (ctx->resp_len == ctx->exp_resp_len) {
            ctx->check_response();
            if (ctx->keep_alive) {
                context *n = ctx->next;
                assert(n);
                n->con = std::move(ctx->con);
                n->con->data = n;
                n->rcon = std::move(ctx->rcon);
                n->rcon->data = n;
                on_connect(n->rcon.get(), 0);
            }
        }
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
    ctx->create_request();
    uv_buf_t buf = uv_buf_init(&ctx->req_data.front(), ctx->req_data.size());
    ctx->write.data = ctx;
    (*ctx->d->products.begin())->exp_req_len = ctx->req_data.size();
    (*ctx->d->products.begin())->exp_resp_len = ctx->exp_resp_len;
    ASSERT_TRUE(uv_ok(uv_write(&ctx->write, ctx->rcon->handle, &buf, 1,
                    on_write)));
}

void connect(context *ctx)
{
    ctx->d->set_factory_args();
    ctx->con.reset(new uv_tcp_t);
    ctx->con->data = ctx;
    ASSERT_TRUE(uv_ok(uv_tcp_init(ctx->loop, ctx->con.get())));
    sockaddr_in6 addr;
    EXPECT_TRUE(uv_ok(uv_ip6_addr("::1", 8000, &addr)));
    ctx->rcon.reset(new uv_connect_t());
    ctx->rcon->data = ctx;
    ASSERT_TRUE(uv_ok(uv_tcp_connect(ctx->rcon.get(), ctx->con.get(),
                    (sockaddr *)&addr, on_connect)));
}

} // namespace

struct onerequest_worker : public worker_loop {
    virtual int handle_request() noexcept override
    {
        if (get_request_len() != 3 * sizeof(size_t))
            return EXIT_FAILURE;
        size_t *p = reinterpret_cast<size_t *>(get_request());
        if (p[0] != 2 * sizeof(p[1]))
            return EXIT_FAILURE;
        size_t resp_len = p[2];
        shmem_buffer *resp = get_response_buf();
        if (!resp->reset_defaults(resp_len))
            return EXIT_FAILURE;
        resp->set_data_size(resp_len);
        for (size_t i = 0; i < resp_len; ++i)
            resp->map_ptr()[i] = i;
        return send_last_response() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
};

namespace {
workers_reg::registrator<onerequest_worker> reg1("onerequest");
} // namespace

struct empty_req_context : context {
    virtual void create_request() override
    {
        req_data.resize(3 * sizeof(size_t));
        size_t *dst = (size_t *)req_data.data();
        dst[0] = 2 * sizeof(dst[1]);
        dst[1] = keep_alive;
        dst[2] = exp_resp_len;
    }

    virtual void check_response() const override
    {
        for (size_t i = 0; i < resp_len; ++i)
            EXPECT_EQ((char)i, resp_data[i]);
    }
};

template<typename T, size_t n>
constexpr size_t ar_sz(const T (&)[n]) { return n; }

struct nonpersistent : loop_fixture {};

TEST_F(nonpersistent, varresponses)
{
    common_dispatcher<test_context> d;
    const char *args[] = {"./pruv_test", "--worker", "onerequest", nullptr};
    d.start(&loop, "::1", 8000, 1, "./pruv_test", args);
    size_t lens[] = {0, 1, 4096, REQUEST_CHUNK, RESPONSE_CHUNK,
        10 * RESPONSE_CHUNK, 123, 10 * RESPONSE_CHUNK + 123};
    std::vector<std::unique_ptr<context>> ctxs(ar_sz(lens));
    for (size_t i = 0; i < ctxs.size(); ++i) {
        ctxs[i].reset(new empty_req_context);
        ctxs[i]->next = nullptr;
        if (i)
            ctxs[i - 1]->next = ctxs[i].get();
        ctxs[i]->keep_alive = false;
        ctxs[i]->exp_resp_len = lens[i];
        ctxs[i]->d = &d;
        ctxs[i]->loop = &loop;
    }
    connect(ctxs.front().get());
    ASSERT_TRUE(uv_ok(uv_run(&loop, UV_RUN_DEFAULT)));
    d.on_loop_exit();
}

struct persistent : loop_fixture {};

TEST_F(persistent, varresponses)
{
    common_dispatcher<test_context> d;
    const char *args[] = {"./pruv_test", "--worker", "onerequest", nullptr};
    d.start(&loop, "::1", 8000, 1, "./pruv_test", args);
    size_t lens[] = {1, 4096, REQUEST_CHUNK, RESPONSE_CHUNK,
        10 * RESPONSE_CHUNK, 123, 10 * RESPONSE_CHUNK + 123};
    std::vector<std::unique_ptr<context>> ctxs(ar_sz(lens));
    for (size_t i = 0; i < ctxs.size(); ++i) {
        ctxs[i].reset(new empty_req_context());
        ctxs[i]->next = nullptr;
        if (i)
            ctxs[i - 1]->next = ctxs[i].get();
        ctxs[i]->keep_alive = true;
        ctxs[i]->exp_resp_len = lens[i];
        ctxs[i]->d = &d;
        ctxs[i]->loop = &loop;
    }
    ctxs.back()->keep_alive = false;
    connect(ctxs.front().get());
    ASSERT_TRUE(uv_ok(uv_run(&loop, UV_RUN_DEFAULT)));
    d.on_loop_exit();
}

uint32_t adler32(unsigned char *data, size_t len)
{
    const int MOD_ADLER = 65521;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % MOD_ADLER;
        b = (b + a) % MOD_ADLER;
    }
    return (b << 16) | a;
}

struct hashrequest_worker : public worker_loop {
    virtual int handle_request() noexcept override
    {
        if (get_request_len() < 2 * sizeof(size_t))
            return EXIT_FAILURE;
        size_t *p = reinterpret_cast<size_t *>(get_request());
        if (p[0] + 2 * sizeof(size_t) != get_request_len())
            return EXIT_FAILURE;
        size_t resp_len = 4;
        shmem_buffer *resp = get_response_buf();
        if (!resp->reset_defaults(resp_len))
            return EXIT_FAILURE;
        resp->set_data_size(resp_len);
        unsigned char *body = (unsigned char *)get_request() +
            2 * sizeof(size_t);
        *reinterpret_cast<uint32_t *>(resp->map_ptr()) = adler32(body, p[0]);
        return send_last_response() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
};

namespace {
workers_reg::registrator<hashrequest_worker> reg2("hashrequest");
} // namespace

struct hash_req_context : context {
    size_t gen_req_len;
    uint32_t hash;

    virtual void create_request() override
    {
        req_data.resize(2 * sizeof(size_t) + gen_req_len);
        size_t *hdr = (size_t *)req_data.data();
        hdr[0] = gen_req_len;
        hdr[1] = keep_alive;
        char *body = req_data.data() + 2 * sizeof(size_t);
        for (size_t i = 0; i < gen_req_len; ++i)
            body[i] = 0;
        hash = adler32((unsigned char *)body, gen_req_len);
    }

    virtual void check_response() const override
    {
        EXPECT_EQ(hash, *reinterpret_cast<const uint32_t *>(resp_data.data()));
    }
};

TEST_F(nonpersistent, varrequests)
{
    common_dispatcher<test_context> d;
    const char *args[] = {"./pruv_test", "--worker", "hashrequest", nullptr};
    d.start(&loop, "::1", 8000, 1, "./pruv_test", args);
    size_t hdr = 2 * sizeof(size_t);
    size_t lens[] = {0, 1, 4096 - hdr, REQUEST_CHUNK - hdr, REQUEST_CHUNK,
        RESPONSE_CHUNK - hdr, RESPONSE_CHUNK,
        10 * RESPONSE_CHUNK - hdr, 123, 10 * RESPONSE_CHUNK + 123};
    std::vector<std::unique_ptr<hash_req_context>> ctxs(ar_sz(lens));
    for (size_t i = 0; i < ctxs.size(); ++i) {
        ctxs[i].reset(new hash_req_context());
        ctxs[i]->next = nullptr;
        if (i)
            ctxs[i - 1]->next = ctxs[i].get();
        ctxs[i]->keep_alive = false;
        ctxs[i]->gen_req_len = lens[i];
        ctxs[i]->exp_resp_len = 4;
        ctxs[i]->d = &d;
        ctxs[i]->loop = &loop;
    }
    connect(ctxs.front().get());
    ASSERT_TRUE(uv_ok(uv_run(&loop, UV_RUN_DEFAULT)));
    d.on_loop_exit();
}

TEST_F(persistent, varrequests)
{
    common_dispatcher<test_context> d;
    const char *args[] = {"./pruv_test", "--worker", "hashrequest", nullptr};
    d.start(&loop, "::1", 8000, 1, "./pruv_test", args);
    size_t hdr = 2 * sizeof(size_t);
    size_t lens[] = {0, 1, 4096 - hdr, REQUEST_CHUNK - hdr, REQUEST_CHUNK,
        RESPONSE_CHUNK - hdr, RESPONSE_CHUNK,
        10 * RESPONSE_CHUNK - hdr, 123, 10 * RESPONSE_CHUNK + 123};
    std::vector<std::unique_ptr<hash_req_context>> ctxs(ar_sz(lens));
    for (size_t i = 0; i < ctxs.size(); ++i) {
        ctxs[i].reset(new hash_req_context());
        ctxs[i]->next = nullptr;
        if (i)
            ctxs[i - 1]->next = ctxs[i].get();
        ctxs[i]->keep_alive = true;
        ctxs[i]->gen_req_len = lens[i];
        ctxs[i]->exp_resp_len = 4;
        ctxs[i]->d = &d;
        ctxs[i]->loop = &loop;
    }
    ctxs.back()->keep_alive = false;
    connect(ctxs.front().get());
    ASSERT_TRUE(uv_ok(uv_run(&loop, UV_RUN_DEFAULT)));
    d.on_loop_exit();
}

} // namespace pruv
