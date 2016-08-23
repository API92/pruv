/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/dispatcher.hpp>

#include <algorithm>
#include <cassert>
#include <inttypes.h>

#include <unistd.h>

#include <pruv/cleanup_helpers.hpp>
#include <pruv/log.hpp>

namespace pruv {

///
/// dispatcher
///

dispatcher::~dispatcher()
{
    assert(!loop);
    assert(!workers_cnt);
    assert(!worker_name);
    assert(!worker_args);

    assert(clients_idle.empty());
    assert(clients_io.empty());
    assert(clients_scheduling.empty());
    assert(clients_processing.empty());

    assert(free_workers.empty());
    assert(in_use_workers.empty());

    assert(req_bufs.empty());
    assert(resp_bufs.empty());
}

void dispatcher::set_timeouts(bool enable) noexcept
{
    timeouts_enabled = enable;
}

void dispatcher::start(uv_loop_t *new_loop, const char *ip, int port,
        size_t workers_max, const char *worker_name,
        const char * const *worker_args) noexcept
{
    loop = new_loop;
    this->worker_name = worker_name;
    this->workers_max = workers_max;
    this->worker_args = worker_args;
    bool ok = true;
    ok &= start_server(ip, port);
    ok &= start_timer(); // Initialize timer before stop it.
    if (!ok)
        stop();
}

void dispatcher::stop() noexcept
{
    assert(loop);
    close_connections(clients_idle);
    close_connections(clients_io);
    close_connections(clients_scheduling);
    close_connections(clients_processing);
    stop_server();
    // Don't close timer.
    // It will helps to kill workers with SIGKILL if they not respond.
    while (!free_workers.empty())
        kill_worker(&free_workers.front());
    while (!in_use_workers.empty())
        kill_worker(&in_use_workers.front());
    close_buffers(req_bufs);
    close_buffers(resp_bufs);
    worker_args = nullptr;
    worker_name = nullptr;
}

void dispatcher::on_loop_exit() noexcept
{
    assert(loop);
    close_timer();
    close_buffers(req_bufs);
    close_buffers(resp_bufs);
    loop = nullptr;
}

bool dispatcher::start_server(const char *ip, int port) noexcept
{
    assert(loop);
    server.base<uv_handle_t *>()->data = this;
    if (!server.init(loop)) {
        server.close(nullptr);
        return false;
    }

    int r;
    sockaddr_in addr4;
    sockaddr_in6 addr6;
    sockaddr *addr = nullptr;
    if ((r = uv_ip4_addr(ip, port, &addr4)) < 0) {
        int r2 = uv_ip6_addr(ip, port, &addr6);
        if (r2 < 0) {
            pruv_log_uv_err(LOG_EMERG, "uv_ip4_addr", r);
            pruv_log_uv_err(LOG_EMERG, "uv_ip6_addr", r2);
            return false;
        }
        else
            addr = (sockaddr *)&addr6;
    }
    else
        addr = (sockaddr *)&addr4;

    auto on_conn = [](uv_stream_t *server, int status) {
        dispatcher *d = reinterpret_cast<dispatcher *>(server->data);
        d->on_connection(server, status);
    };

    if (!server.bind(addr, 0) || !server.listen(16384, on_conn)) {
        server.close(nullptr);
        return false;
    }
    pruv_log(LOG_NOTICE, "Server started at %s:%d", ip, port);
    return true;
}

void dispatcher::stop_server() noexcept
{
    server.close(nullptr);
    pruv_log(LOG_NOTICE, "Server stopped");
}

void dispatcher::spawn_worker() noexcept
{
    assert(loop);
    worker_process *worker = new (std::nothrow) worker_process;
    if (!worker) {
        pruv_log(LOG_ERR, "Not enough memory for worker");
        return;
    }
    auto delcb = [](void *w) { delete reinterpret_cast<worker_process *>(w); };
    auto on_exit = [](uv_process_t *p, int64_t exit_code, int signal) {
        worker_process *worker = static_cast<worker_process *>(p);
        dispatcher *d = reinterpret_cast<dispatcher *>(worker->owner);
        d->on_worker_exit(worker, exit_code, signal);
    };

    if (!worker->start(loop, worker_name, worker_args, this, on_exit, delcb))
        // deleter will be called sometime later, or already called.
        return;

    pruv_log(LOG_NOTICE, "Worker process %d started.", worker->pid);
    // on_worker_exit callback can be called only on next loop iteration
    // after exit from this function.
    // Therefore allowed to call push_back after worker->start.
    free_workers.push_back(worker);
    ++workers_cnt;

    auto alloc_cb = [](uv_handle_t *h, size_t /*sz*/, uv_buf_t *buf) {
        // process class stores pointer to it in it's pipes data member.
        // h is pipe.
        process *p = reinterpret_cast<process *>(h->data);
        worker_process *w = static_cast<worker_process *>(p);
        // io_state may be some other, then IO_READ if worker writes
        // trash to stdout or dies (and we receive EOF).
        if (w->io_state != worker_process::IO_READ ||
            w->pipe_buf_ptr >= std::end(w->pipe_buf)) {
            *buf = uv_buf_init(nullptr, 0);
            return;
        }
        *buf = uv_buf_init(w->pipe_buf_ptr,
                std::end(w->pipe_buf) - w->pipe_buf_ptr);
    };

    auto read_cb = [](uv_stream_t *s, ssize_t nread, const uv_buf_t *buf) {
        // process class stores pointer to it in it's pipes data member.
        // s is pipe.
        process *p = reinterpret_cast<process *>(s->data);
        worker_process *w = static_cast<worker_process *>(p);
        reinterpret_cast<dispatcher *>(w->owner)->on_worker_read(
                w, nread, buf);
    };
    // Start reading worker's stdout forever.
    int r = uv_read_start((uv_stream_t *)&worker->out, alloc_cb, read_cb);
    if (r < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_read_start", r);
        return kill_worker(worker);
    }
}

void dispatcher::kill_worker(worker_process *w) noexcept
{
    assert(loop);
    assert(!w->empty()); // worker must be in some list
    if (w->processed_con)
        w->processed_con->remove_from_dispatcher();
    // Stop reading pipe to not receive eof,
    // because on_worker_read assumes loop is not null.
    int r;
    if ((r = uv_read_stop((uv_stream_t *)&w->out)) < 0)
        pruv_log_uv_err(LOG_ERR, "uv_read_stop", r);
    if (!w->exited && (r = uv_process_kill(w, SIGTERM)) < 0)
        pruv_log_uv_err(LOG_ERR, "uv_process_kill", r);
    w->remove_from_list();
    w->timeout = uv_now(loop) + KILL_TIMEOUT;
    terminated_workers.push_back(w);
}

void dispatcher::on_worker_exit(worker_process *w, int64_t exit_code, int sig)
    noexcept
{
    pruv_log(LOG_NOTICE, "Worker %d exited with code %" PRId64
            " caused signal %d.", w->pid, exit_code, sig);
    w->exited = true;
    w->remove_from_list();
    if (w->processed_con)
        w->processed_con->remove_from_dispatcher();
    // Buffers may be safely reused only after worker exit.
    if (w->in_buf)
        return_buffer(&w->in_buf, true);
    if (w->out_buf)
        return_buffer(&w->out_buf, false);
    w->stop();
    --workers_cnt;
    schedule();
}

void dispatcher::on_connection(uv_stream_t *server, int status) noexcept
{
    assert(loop);
    if (status < 0)
        return pruv_log_uv_err(LOG_ERR, "on_connection", status);

    pruv_log(LOG_DEBUG, "Connection received. Status = %d.", status);

    tcp_context *con = create_connection();
    if (!con)
        return pruv_log(LOG_ERR, "No memory for connect");

    auto deleter = [](tcp_con *p) {
        tcp_context *con = static_cast<tcp_context *>(p);
        con->get_dispatcher()->free_connection(con);
    };

    if (!con->accept(loop, server, this, deleter) || !con->read_start())
        return con->remove_from_dispatcher(); // deleter will be called later.

    move_to(tcp_context::LIST_IDLE, con);
}

void dispatcher::read_con_alloc_cb(tcp_context *con, size_t /*suggested_size*/,
        uv_buf_t *b) noexcept
{
    assert(loop);
    *b = uv_buf_init(nullptr, 0);

    // Start new request reading.
    if (!con->read_buffer && !(con->read_buffer = get_buffer(true)))
        return;

    shmem_buffer_node *sh_buf = con->read_buffer;
    if (!sh_buf->seek(sh_buf->data_size(), REQUEST_CHUNK))
        return;
    *b = uv_buf_init(sh_buf->map_ptr(), sh_buf->map_end() - sh_buf->map_ptr());
}

void dispatcher::read_con_cb(tcp_context *con, ssize_t nread, const uv_buf_t *)
    noexcept
{
    assert(loop);
    if (nread < 0) {
        pruv_log_uv_err(nread == UV_EOF ? LOG_DEBUG : LOG_ERR, "", nread);
        return con->remove_from_dispatcher();
    }

    con->read_buffer->set_data_size(con->read_buffer->data_size() + nread);
    if (!con->parse_request(con->read_buffer))
        return con->remove_from_dispatcher();

    if (con->list_id == tcp_context::LIST_IDLE ||
        con->list_id == tcp_context::LIST_IO) {
        move_to(tcp_context::LIST_IO, con);
        if (con->get_request(con->request)) {
            // Fully received message to be processed.
            pruv_log(LOG_DEBUG, "Request message parsed (%" PRIuPTR " bytes "
                    "starting from %" PRIuPTR " byte).",
                    con->request.size, con->request.pos);
            respond_or_enqueue(con);
            schedule();
        }
    }

    if (con->list_id != tcp_context::LIST_SCHEDULING && con->read_buffer &&
        con->request.pos >= con->read_buffer->data_size()) {
        /// There is no not parsed data now.
        return_buffer(&con->read_buffer, true);
        if (!con->parse_request(nullptr))
            con->remove_from_dispatcher();
        else if (con->list_id == tcp_context::LIST_IO &&
                con->resp_buffers.empty())
            move_to(tcp_context::LIST_IDLE, con);
    }
}

void dispatcher::respond_or_enqueue(tcp_context *con) noexcept
{
    assert(con);
    if (!con->request.inplace)
        return move_to(tcp_context::LIST_SCHEDULING, con);

    shmem_buffer_node *buf = get_buffer(false);
    if (!buf || !con->read_buffer)
        return con->remove_from_dispatcher();

    con->resp_buffers.push_back(buf);
    move_to(tcp_context::LIST_IO, con);
    if (!con->inplace_response(con->request, *con->read_buffer, *buf) ||
        !con->response_ready(con->request, *buf))
        return con->remove_from_dispatcher();


    if (con->request.pos + con->request.size >= con->read_buffer->data_size()) {
        return_buffer(&con->read_buffer, true);
        if (!con->parse_request(nullptr))
            return con->remove_from_dispatcher();
    }

    if (con->resp_buffers.one_element())
        write_con(con);
}

void dispatcher::schedule() noexcept
{
    assert(loop);
    if (clients_scheduling.empty() ||
        (workers_cnt >= workers_max && free_workers.empty()))
        return;

    if (free_workers.empty()) {
        spawn_worker();
        if (free_workers.empty()) {
            // Сan't serve any request if spawning worker failed.
            pruv_log(LOG_ERR, "No worker for request. Close connections.");
            return close_connections(clients_scheduling);
        }
    }

    shmem_buffer_node *resp_buf = get_buffer(false);
    if (!resp_buf) {
        // Сan't serve any request if opening buffer failed.
        pruv_log(LOG_ERR, "No buffer for response. Close connections.");
        return close_connections(clients_scheduling);
    }

    // take worker for request
    worker_process &w = free_workers.front();
    assert(w.io_state == worker_process::IO_IDLE);
    // pipe_buf_ptr moved to start when state became IO_IDLE
    assert(w.pipe_buf_ptr == w.pipe_buf);

    // make request params to send into worker
    tcp_context *con = nullptr;
    int req_len = 0;
    while (!clients_scheduling.empty()) {
        con = &clients_scheduling.front();
        if (con->read_buffer) {
            req_len = snprintf(w.pipe_buf, sizeof(w.pipe_buf),
                "%s IN SHM %s %" PRIuPTR ", %" PRIuPTR
                " OUT SHM %s %" PRIuPTR "\n", con->request.protocol,
                con->read_buffer->name(), con->request.pos, con->request.size,
                resp_buf->name(), resp_buf->file_size());
            if (req_len >= 0 && req_len < (int)sizeof(w.pipe_buf))
                break;
        }
        pruv_log(LOG_ERR, "Can't snprintf request params");
        con->remove_from_dispatcher();
        con = nullptr;
    }
    if (!con)
        return return_buffer(&resp_buf, false);

    // Connect request and worker.
    w.remove_from_list();
    w.processed_con = con;
    w.in_buf = con->read_buffer;
    w.out_buf = resp_buf; // buffer owned by worker for processing time
    con->worker = &w;
    w.timeout = uv_now(loop) + PROCESSING_TIMEOUT;
    in_use_workers.push_back(&w);
    move_to(tcp_context::LIST_PROCESSING, con);

    auto write_cb = [](uv_write_t *req, int status) {
        // This callback may be called after worker death.
        // But worker_process structure will alive while pipe's
        // structures alive.
        worker_process *w = reinterpret_cast<worker_process *>(req->data);
        dispatcher *d = reinterpret_cast<dispatcher *>(w->owner);
        if (status != 0) {
            pruv_log_uv_err(LOG_ERR, "write_cb", status);
            return d->kill_worker(w);
        }
        pruv_log(LOG_DEBUG, "request sent to worker");
        assert(w->io_state == worker_process::IO_WRITE);
        // Before writing pipe_buf_ptr was at start. Writing don't move it.
        assert(w->pipe_buf_ptr == w->pipe_buf);
        w->io_state = worker_process::IO_READ;
    };

    // Send request to worker.
    w.write_req.data = &w;
    w.io_state = worker_process::IO_WRITE;
    uv_buf_t buf = uv_buf_init(w.pipe_buf, req_len);
    int r = uv_write(&w.write_req, (uv_stream_t *)&w.in, &buf, 1, write_cb);
    if (r < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_write", r);
        kill_worker(&w); // Connection will be closed here too
    }
}

void dispatcher::on_worker_read(worker_process *w, ssize_t nread,
        const uv_buf_t *buf) noexcept
{
    assert(loop); // Because uv_read_stop was called in kill_process.
    if (nread < 0) {
        pruv_log_uv_err(LOG_ERR, "nread", nread);
        return kill_worker(w);
    }

    if (w->io_state != worker_process::IO_READ) {
        pruv_log(LOG_ERR, "Worker not in read state");
        return kill_worker(w);
    }

    w->pipe_buf_ptr += nread;

    // Read until end of line.
    if (!nread || buf->base[nread - 1] != '\n')
        return;

    // Now response from worker fully received.
    pruv_log(LOG_DEBUG, "Response received from worker");

    // Parse response length.
    buf->base[nread - 1] = 0;
    size_t resp_len;
    size_t resp_file_size;
    if (sscanf(w->pipe_buf, "RESP %" SCNuPTR " of %" SCNuPTR " END",
                &resp_len, &resp_file_size) != 2) {
        pruv_log(LOG_ERR, "sscanf can't parse response \"%s\".", buf->base);
        return kill_worker(w);
    }
    pruv_log(LOG_DEBUG, "Response of %" PRIuPTR " bytes ready", resp_len);

    assert(w->in_buf);
    assert(w->out_buf);

    // To reduce number of ftruncate syscals transfer changed size of shared
    // memory object through pipe.
    w->out_buf->update_file_size(resp_file_size);
    tcp_context *con = nullptr;
    if (w->processed_con) {
        con = w->processed_con;
        w->processed_con = nullptr;
        con->worker = nullptr;
        assert(con->list_id == tcp_context::LIST_PROCESSING);

        assert(!con->read_buffer || con->read_buffer == w->in_buf);
        if (con->read_buffer &&
            con->request.pos + con->request.size >= w->in_buf->data_size())
            return_buffer(&con->read_buffer, true);
        w->in_buf = nullptr;

        assert(!w->out_buf->cur_pos());
        assert(w->out_buf->map_ptr() != w->out_buf->map_end());
        w->out_buf->set_data_size(resp_len);
        con->resp_buffers.push_back(w->out_buf);
        w->out_buf = nullptr;
    }
    else {
        // Connection was closed before worker processing finished.
        return_buffer(&w->in_buf, true);
        return_buffer(&w->out_buf, false);
    }

    w->io_state = worker_process::IO_IDLE;
    w->pipe_buf_ptr = w->pipe_buf;
    w->remove_from_list();
    free_workers.push_back(w);

    if (con) {
        // May be some part of new request was readed with previous one (but
        // not parsed yet). If so parse it.
        if (con->response_ready(con->request, con->resp_buffers.back()) &&
            con->parse_request(con->read_buffer)) {
            // If resp_buffers size becames 1 now then writing not started yet.
            // If resp_buffers size > 1 then writing of some other buffer
            // already in progress.
            // move_to LIST_IO because if do so and response is of zero
            // size then write_con() can move connection to LIST_IDLE.
            move_to(tcp_context::LIST_IO, con);
            if (con->resp_buffers.one_element())
                write_con(con);
            if (con->get_request(con->request))
                // With previous request the second request was fully readed too
                // and was parsed in parse_request().
                // Process it now or when responses queue will become empty.
                respond_or_enqueue(con);
        }
        else
            con->remove_from_dispatcher();
    }
    schedule();
}

void dispatcher::write_con(tcp_context *con) noexcept
{
    assert(loop);
    assert(!con->resp_buffers.empty());
    assert(!con->empty()); // must be in some list
    // Writing response for one response and scheduling/processing the second
    // response can be at the same time. In this case connection is in
    // LIST_SCHEDULING/LIST_PROCESSING.
    assert(con->list_id != tcp_context::LIST_IDLE);
    if (con->list_id == tcp_context::LIST_IO)
        move_to(tcp_context::LIST_IO, con);

    shmem_buffer_node *buf = &con->resp_buffers.front();
    if (buf->map_ptr() == buf->map_end()) {
        // Mapped chunk was fully written. Map next chunk.
        size_t map_size = std::min(RESPONSE_CHUNK,
                buf->data_size() - buf->cur_pos());
        if (!buf->map(buf->cur_pos(), map_size))
            return con->remove_from_dispatcher();
    }

    if (!con->parse_response(*buf))
        return con->remove_from_dispatcher();

    auto write_cb = [](uv_write_t *r, int status) {
        tcp_context *con = static_cast<tcp_context *>(tcp_con::from(r->handle));
        if (con->resp_buffers.empty())
            return; // Connection was closed and buffers was returned to pool.
        if (status < 0) {
            pruv_log_uv_err(LOG_ERR, "", status);
            return con->remove_from_dispatcher();
        }
        size_t chunk_size = (size_t)r->data;
        shmem_buffer_node &buf = con->resp_buffers.front();
        buf.move_ptr(chunk_size);
        pruv_log(LOG_DEBUG, "Response chunk of %" PRIuPTR " bytes written",
                chunk_size);
        if (buf.cur_pos() >= buf.data_size())
            // Do it in callback to protect from infinite recursion
            // on_end_write_con -> respond_or_enqueue -> ... for empty response.
            return con->get_dispatcher()->on_end_write_con(con);
        con->get_dispatcher()->write_con(con);
    };

    size_t chunk_size = std::min(size_t(buf->map_end() - buf->map_ptr()),
            buf->data_size() - buf->cur_pos()); // Can be 0 for empty response
    uv_buf_t wbuf = uv_buf_init(buf->map_ptr(), chunk_size);
    con->write_req.data = (void *)chunk_size;
    int r = uv_write(&con->write_req, con->base<uv_stream_t *>(), &wbuf, 1,
            write_cb);
    if (r < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_write", r);
        return con->remove_from_dispatcher();
    }
}

void dispatcher::on_end_write_con(tcp_context *con) noexcept
{
    assert(loop);
    assert(con->list_id != tcp_context::LIST_IDLE);
    pruv_log(LOG_DEBUG, "Response sended");
    if (con->finish_response(con->resp_buffers.front()) &&
        con->parse_request(con->read_buffer)) {
        return_buffer(con->resp_buffers.front(), false);
        if (!con->resp_buffers.empty())
            return write_con(con);
        if (con->list_id != tcp_context::LIST_IO)
            return; // Connection scheduled or processed now. Can't ask request.
        // If new request became available after finish_response() and
        // parse_request() then now it's allowed to ask it and to schedule it.
        if (con->get_request(con->request)) {
            respond_or_enqueue(con);
            schedule();
        }
        else if (con->read_buffer)
            // Connection has partially readed message.
            move_to(tcp_context::LIST_IO, con);
        else
            // Connection is inactive.
            move_to(tcp_context::LIST_IDLE, con);
    }
    else
        con->remove_from_dispatcher();
}

constexpr const char * dispatcher::tcp_context::list_names[];

void dispatcher::move_to(tcp_context::list_id_enum dst, tcp_context *con)
    noexcept
{
    assert(loop);
    if (!con->empty())
        con->remove_from_list();
    con->list_id = dst;
    if (dst == tcp_context::LIST_IO) {
        con->timeout = uv_now(loop) + IO_TIMEOUT;
        clients_io.push_back(con);
    }
    else if (dst == tcp_context::LIST_SCHEDULING)
        clients_scheduling.push_back(con);
    else if (dst == tcp_context::LIST_PROCESSING)
        clients_processing.push_back(con);
    else if (dst == tcp_context::LIST_IDLE) {
        con->timeout = uv_now(loop) + IDLE_TIMEOUT;
        clients_idle.push_back(con);
    }
    pruv_log(LOG_DEBUG, "Connection moved to list %s",
            tcp_context::list_names[dst]);
}

dispatcher::shmem_buffer_node *
dispatcher::get_buffer(bool for_request) noexcept
{
    assert(loop);
    list_node<shmem_buffer_node> &list = (for_request ? req_bufs : resp_bufs);
    if (!list.empty()) {
        shmem_buffer_node *buf = &list.front();
        buf->remove_from_list();
        assert(buf->data_size() == 0);
        assert(!buf->cur_pos());
        return buf;
    }

    scoped_ptr<shmem_buffer_node> buf(new (std::nothrow) shmem_buffer_node);
    if (!buf) {
        pruv_log(LOG_ERR, "No memory for shmem_buffer_node");
        return nullptr;
    }
    if (!buf->open(nullptr, true))
        return nullptr;
    if (!buf->reset_defaults(for_request ? REQUEST_CHUNK : RESPONSE_CHUNK)) {
        buf->close();
        return nullptr;
    }
    return buf.release();
}

void dispatcher::return_buffer(shmem_buffer_node &buf, bool for_request)
    noexcept
{
    if (!buf.empty())
        buf.remove_from_list();
    if (!buf.reset_defaults(for_request ? REQUEST_CHUNK : RESPONSE_CHUNK)) {
        buf.close();
        delete &buf;
        return;
    }
    assert(!buf.cur_pos()); // after reset_defaults
    buf.set_data_size(0);
    (for_request ? req_bufs : resp_bufs).push_front(&buf);
}

void dispatcher::return_buffer(shmem_buffer_node **buf, bool for_request)
    noexcept
{
    return_buffer(**buf, for_request);
    *buf = nullptr;
}

void dispatcher::close_buffers(list_node<shmem_buffer_node> &buf_list) noexcept
{
    assert(loop);
    while (!buf_list.empty()) {
        shmem_buffer_node *buf = &buf_list.front();
        buf->remove_from_list();
        buf->close();
        // tcp_stream and worker, which uses this buffer, must be closed before
        // and must not use buffer in close_cb.
        delete buf;
    }
}

bool dispatcher::start_timer() noexcept
{
    assert(loop);
    int r;
    close_on_return close_timer((uv_handle_t *)&timer, nullptr);
    if ((r = uv_timer_init(loop, &timer)) < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_timer_init", r);
        return false;
    }

    auto timeout_cb = [](uv_timer_t *t) {
        reinterpret_cast<dispatcher *>(t->data)->on_timer_tick();
    };

    timer.data = this;
    if ((r = uv_timer_start(&timer, timeout_cb, 0, TIMER_PERIOD)) < 0) {
        pruv_log_uv_err(LOG_ERR, "uv_timer_start", r);
        return false;
    }

    uv_unref((uv_handle_t *)&timer);
    close_timer.h = nullptr;
    return true;
}

void dispatcher::close_timer() noexcept
{
    assert(loop);
    // When start_timer() failed, timer was closed there.
    // But close_timer() calling too, therefore check if timer not closing.
    if (!uv_is_closing((uv_handle_t *)&timer))
        uv_close((uv_handle_t *)&timer, nullptr);
    // Make one loop iteration to remove timer from loop.
    uv_run(loop, UV_RUN_NOWAIT);
}

void dispatcher::on_timer_tick() noexcept
{
    if (!timeouts_enabled)
        return;
    assert(loop);
    uint64_t now = uv_now(loop);
    for (const worker_process &w : terminated_workers) {
        if (w.timeout > now)
            break;
        assert(!terminated_workers.front().exited);
        int r = uv_process_kill(&terminated_workers.front(), SIGKILL);
        if (r < 0)
            pruv_log_uv_err(LOG_ERR, "uv_process_kill", r);
    }
    while (!in_use_workers.empty() && in_use_workers.front().timeout <= now)
        kill_worker(&in_use_workers.front());
    close_old_connections(clients_idle);
    close_old_connections(clients_io);
}

void dispatcher::close_connections(list_node<tcp_context> &list) noexcept
{
    while (!list.empty())
        list.front().remove_from_dispatcher();
}

void dispatcher::close_old_connections(list_node<tcp_context> &list) noexcept
{
    assert(loop);
    uint64_t now = uv_now(loop);
    while (!list.empty() && list.front().timeout <= now)
        list.front().remove_from_dispatcher();
}

///
/// tcp_context
///

dispatcher * dispatcher::tcp_context::get_dispatcher() const noexcept
{
    return reinterpret_cast<dispatcher *>(owner);
}

void dispatcher::tcp_context::remove_from_dispatcher() noexcept
{
    while (!resp_buffers.empty())
        get_dispatcher()->return_buffer(resp_buffers.front(), false);

    if (worker)
        worker->processed_con = nullptr;
    else if (read_buffer) // Can return buffer only if worker not use it.
        get_dispatcher()->return_buffer(&read_buffer, true);

    if (!empty()) // may be not in any list (for example, in schedule)
        remove_from_list();
    close();
    pruv_log(LOG_DEBUG, "Connection closed.");
}

bool dispatcher::tcp_context::read_start()
{
    auto alloc_cb = [](uv_handle_t *h, size_t size, uv_buf_t *buf) {
        tcp_context *strm = static_cast<tcp_context *>(tcp_con::from(h));
        strm->get_dispatcher()->read_con_alloc_cb(strm, size, buf);
    };
    auto read_cb = [](uv_stream_t *s, ssize_t nread, const uv_buf_t *buf) {
        tcp_context *strm = static_cast<tcp_context *>(tcp_con::from(s));
        strm->get_dispatcher()->read_con_cb(strm, nread, buf);
    };
    return tcp_con::read_start(alloc_cb, read_cb);
}

} // namespace pruv
