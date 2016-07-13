/*
 * Copyright (C) Andrey Pikas
 */

#include <deque>
#include <list>
#include <numeric>
#include <string>

#include <gtest/gtest.h>

#include <pruv/log.hpp>
#include <pruv/worker_loop.hpp>
#include "common_dispatcher.hpp"
#include "fixtures.hpp"
#include "workers_reg.hpp"

namespace pruv {
namespace {

bool generate_response(char *request, size_t req_len, shmem_buffer *resp)
{
    if (req_len < sizeof(size_t) + 1) {
        pruv_log(LOG_ERR, "Request is too small");
        return false;
    }
    size_t body_len = *reinterpret_cast<size_t *>(request);
    if (sizeof(size_t) + 1 + body_len != req_len) {
        pruv_log(LOG_ERR, "Request length %" PRIuPTR " is wrong", req_len);
        return false;
    }
    bool keep_alive = request[sizeof(size_t)];
    size_t resp_body_len = 2 * body_len;
    size_t resp_len = sizeof(size_t) + 1 + resp_body_len;
    if (!resp->reset_defaults(resp_len))
        return false;
    resp->set_data_size(resp_len);
    memcpy(resp->map_ptr(), &resp_body_len, sizeof(resp_body_len));
    resp->move_ptr(sizeof(resp_body_len));
    *resp->map_ptr() = keep_alive;
    resp->move_ptr(1);

    char *cp = request + sizeof(size_t) + 1;
    for (size_t i = 0; i < body_len; ++i, ++cp) {
        *resp->map_ptr() = *std::prev(resp->map_ptr()) ^ *cp;
        *std::next(resp->map_ptr()) = *cp;
        resp->move_ptr(2);
    }
    return true;
}

struct pipeline_context : common_dispatcher<pipeline_context>::tcp_context {
    pipeline_context(bool inplace) : inplace(inplace) {}

    virtual bool prepare_for_request(shmem_buffer *buf) noexcept override;
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

    size_t request_pos_ = 0;
    size_t request_len = 0;
    size_t body_len = 0;

    bool inplace;
    bool req_end = true;
    bool keep_alive;
};

bool pipeline_context::validate_request(const shmem_buffer *buf) const noexcept
{
    return buf->data_size() < 1024 * 1024;
}

bool pipeline_context::prepare_for_request(shmem_buffer *buf) noexcept
{
    if (!req_end)
        return true; // from on_end_write_con()
    request_pos_ += request_len; // Skip previous request.
    request_len = 0;
    req_end = false;
    if (!buf) {
        request_pos_ = 0;
        return true;
    }
    return parse_request(buf);
}

bool pipeline_context::parse_request(shmem_buffer *buf) noexcept
{
    EXPECT_FALSE(req_end);

    while (!req_end && request_pos_ + request_len < buf->data_size()) {
        if (!buf->seek(request_pos_ + request_len, REQUEST_CHUNK))
            return false;
        if (request_len < sizeof(body_len)) {
            reinterpret_cast<char *>(&body_len)[request_len] = *buf->map_ptr();
            ++request_len;
            continue;
        }
        size_t rem_len = std::min(buf->data_size() - buf->cur_pos(),
                size_t(buf->map_end() - buf->map_ptr()));
        request_len = std::min(request_len + rem_len,
                body_len + sizeof(size_t) + 1);
        req_end = (body_len + sizeof(size_t) + 1 == request_len);
    }
    return true;
}

size_t pipeline_context::request_size() const noexcept
{
    return req_end ? request_len : 0;
}

size_t pipeline_context::request_pos() const noexcept
{
    return request_pos_;
}

const char * pipeline_context::request_protocol() const noexcept
{
    return "TEST";
}

bool pipeline_context::inplace_response(shmem_buffer *buf_in,
        shmem_buffer *buf_out) noexcept
{
    EXPECT_TRUE(req_end);

    if (inplace) {
        if (!buf_out)
            return true;
        bool map_res = buf_in->map(0, request_pos_ + request_len);
        EXPECT_TRUE(map_res);
        if (!map_res)
            return false;
        bool gen_res = generate_response(buf_in->map_ptr() + request_pos_,
                request_len, buf_out);
        EXPECT_TRUE(gen_res);
        if (!gen_res)
            return false;
        bool move_res = buf_out->seek(0, RESPONSE_CHUNK);
        EXPECT_TRUE(move_res);
        if (!move_res)
            return false;
        return true;
    }
    else {
        EXPECT_TRUE(!buf_out);
        return buf_out;
    }
}

bool pipeline_context::prepare_for_response() noexcept
{
    keep_alive = false;
    return true;
}

bool pipeline_context::parse_response(shmem_buffer *buf) noexcept
{
    if (!buf->map_offset())
        keep_alive = buf->map_begin()[sizeof(size_t)];
    return true;
}

bool pipeline_context::finish_response() noexcept
{
    return keep_alive;
}

struct reqtest {
    std::string request;
    std::string exp_response;
    reqtest *next;
};

void fill_test(reqtest &req, size_t req_len, bool keep_alive)
{
    ASSERT_LE(sizeof(req_len) + 1, req_len);
    size_t len = req_len - sizeof(req_len) - 1;
    req.request.insert(req.request.end(), (char *)&len,
            (char *)&len + sizeof(len));
    req.request.push_back(keep_alive);

    size_t resp_len = 2 * len;
    req.exp_response.insert(req.exp_response.end(), (char *)&resp_len,
            (char *)&resp_len + sizeof(resp_len));
    req.exp_response.push_back(keep_alive);

    for (size_t i = 0; i < len; ++i) {
        char c = rand();
        req.request.push_back(c);
        req.exp_response.push_back(req.exp_response.back() ^ c);
        req.exp_response.push_back(c);
    }
}

void concatenate_requests(reqtest *r, std::string *dest)
{
    for (; r; r = r->next)
        dest->append(r->request);
}

void concatenate_responses(reqtest *r, std::string *dest)
{
    for (; r; r = r->next)
        dest->append(r->exp_response);
}

struct state {
    uv_tcp_t connection;
    uv_timer_t timer;
    std::string send_data;
    size_t sended = 0;
    std::vector<char> recv_buffer;
    size_t received = 0;
    std::string exp_received;
    std::deque<size_t> send_lens;
    std::list<uv_write_t> write_reqs;
    common_dispatcher<pipeline_context> *d = nullptr;
};

struct pipeline : loop_fixture, ::testing::WithParamInterface<bool> {
    static void on_write(uv_write_t *, int status)
    {
        ASSERT_TRUE(uv_ok(status));
    }

    static void on_tick(uv_timer_t *th)
    {
        state *st = reinterpret_cast<state *>(th->data);
        if (st->sended == st->send_data.size())
            return;
        ASSERT_FALSE(st->send_lens.empty());
        size_t chunk_len = st->send_lens.front();
        st->send_lens.pop_front();
        st->write_reqs.emplace_back();
        uv_write_t &wr = st->write_reqs.back();
        uv_buf_t buf = uv_buf_init(&st->send_data[st->sended], chunk_len);
        st->sended += chunk_len;
        ASSERT_TRUE(uv_ok(uv_write(&wr, (uv_stream_t *)&st->connection,
                        &buf, 1, on_write)));
    }

    static void alloc_cb(uv_handle_t *h, size_t sz, uv_buf_t *buf)
    {
        state *st = reinterpret_cast<state *>(h->data);
        st->recv_buffer.resize(st->received + sz);
        *buf = uv_buf_init(st->recv_buffer.data() + st->received, sz);
    }

    static void read_cb(uv_stream_t *s, ssize_t nread, const uv_buf_t *)
    {
        ASSERT_TRUE(uv_ok(nread));
        state *st = reinterpret_cast<state *>(s->data);
        st->received += nread;
        st->recv_buffer.resize(st->received);

        if (st->received >= st->exp_received.size()) {
            uv_close((uv_handle_t *)&st->timer, nullptr);
            uv_close((uv_handle_t *)&st->connection, nullptr);
            st->d->stop();
        }
    }

    static void on_connect(uv_connect_t *conreq, int status)
    {
        ASSERT_TRUE(uv_ok(status));
        state *st = reinterpret_cast<state *>(conreq->handle->data);
        st->timer.data = st;
        ASSERT_TRUE(uv_ok(uv_timer_init(st->connection.loop, &st->timer)));
        ASSERT_TRUE(uv_ok(uv_timer_start(&st->timer, on_tick, 0, 200)));
        ASSERT_TRUE(uv_ok(uv_read_start((uv_stream_t *)&st->connection,
                        alloc_cb, read_cb)));
    }
};

struct redundant_worker : public worker_loop {
    virtual int handle_request() noexcept override
    {
        if (!generate_response(get_request(), get_request_len(),
                    get_response_buf()))
            return EXIT_FAILURE;
        return send_last_response() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
};

workers_reg::registrator<redundant_worker> reg("redundantxor");

TEST_P(pipeline, test_1)
{
    size_t lens[] = {9, 10, REQUEST_CHUNK - 1, REQUEST_CHUNK,
        REQUEST_CHUNK + 1, REQUEST_CHUNK + 9, 10 * REQUEST_CHUNK};
    size_t sz = sizeof(lens) / sizeof(*lens);
    std::vector<reqtest> tests(sz);
    for (size_t i = 0; i < sz; ++i)
        fill_test(tests[i], lens[i], i + 1 < sz);
    for (size_t i = 1; i < sz; ++i)
        tests[i - 1].next = &tests[i];

    state st;
    concatenate_requests(&tests[0], &st.send_data);
    concatenate_responses(&tests[0], &st.exp_received);
    st.send_lens = {10, 9, 2 * REQUEST_CHUNK - 1, 3 * REQUEST_CHUNK};
    st.send_lens.push_back(st.send_data.size() -
            std::accumulate(st.send_lens.begin(), st.send_lens.end(), 0));

    ASSERT_TRUE(uv_ok(uv_tcp_init(&loop, &st.connection)));

    common_dispatcher<pipeline_context> d(GetParam());
    st.d = &d;
    const char *args[] = {"./pruv_test", "--worker", "redundantxor", nullptr};
    d.start(&loop, "::1", 8000, 1, "./pruv_test", args);

    sockaddr_in6 addr;
    ASSERT_TRUE(uv_ok(uv_ip6_addr("::1", 8000, &addr)));
    uv_connect_t conreq;
    st.connection.data = &st;
    ASSERT_TRUE(uv_ok(uv_tcp_connect(&conreq, &st.connection,
                    (sockaddr *)&addr, pipeline::on_connect)));

    EXPECT_TRUE(uv_ok(uv_run(&loop, UV_RUN_DEFAULT)));
    d.on_loop_exit();
    EXPECT_TRUE(std::equal(
            st.exp_received.begin(), st.exp_received.end(),
            st.recv_buffer.begin(), st.recv_buffer.end()));
}

INSTANTIATE_TEST_CASE_P(inworker, pipeline, ::testing::Values(false));

INSTANTIATE_TEST_CASE_P(inplace, pipeline, ::testing::Values(true));

} // namespace
} // namespace pruv
