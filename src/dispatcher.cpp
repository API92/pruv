/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/dispatcher.hpp>

#include <algorithm>
#include <cassert>
#include <inttypes.h>
#include <memory.h>
#include <string>

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
    assert(uv_is_closing((uv_handle_t *)&tcp_server));

    assert(clients_idle.empty());
    assert(clients_reading.empty());
    assert(clients_scheduling.empty());
    assert(clients_processing.empty());
    assert(clients_writing.empty());

    assert(free_workers.empty());
    assert(in_use_workers.empty());

    assert(req_bufs.empty());
    assert(resp_bufs.empty());
    assert(in_use_bufs.empty());
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
    ok &= start_timer();
    if (!ok)
        stop();
}

void dispatcher::stop() noexcept
{
    assert(loop);
    close_connections(clients_idle);
    close_connections(clients_reading);
    close_connections(clients_scheduling);
    close_connections(clients_processing);
    close_connections(clients_writing);
    stop_server();
    // Don't close timer.
    // It will helps to kill workers with SIGKILL if they not respond.
    while (!free_workers.empty())
        kill_worker(&free_workers.front());
    while (!in_use_workers.empty())
        kill_worker(&in_use_workers.front());
    close_buffers(req_bufs);
    close_buffers(resp_bufs);
    // Don't close in_use_bufs. They used by workers. Wait workers exit.
    worker_args = nullptr;
    worker_name = nullptr;
}

void dispatcher::on_loop_exit() noexcept
{
    assert(loop);
    close_timer();
    close_buffers(req_bufs);
    close_buffers(resp_bufs);
    assert(in_use_bufs.empty());
    loop = nullptr;
}

bool dispatcher::start_server(const char *ip, int port) noexcept
{
    assert(loop);
    int r;
    tcp_server.data = this;

    sockaddr_in addr4;
    sockaddr_in6 addr6;
    sockaddr *addr = nullptr;
    if ((r = uv_ip4_addr(ip, port, &addr4)) < 0) {
        int r2 = uv_ip6_addr(ip, port, &addr6);
        if (r2 < 0) {
            log_uv_error(LOG_EMERG, "dispatcher::start_server uv_ip4_addr", r);
            log_uv_error(LOG_EMERG, "dispatcher::start_server uv_ip6_addr", r2);
            return false;
        }
        else
            addr = (sockaddr *)&addr6;
    }
    else
        addr = (sockaddr *)&addr4;

    close_on_return close_server((uv_handle_t *)&tcp_server, nullptr);
    if ((r = uv_tcp_init(loop, &tcp_server)) < 0) {
        log_uv_error(LOG_EMERG, "dispatcher::start_server uv_tcp_init", r);
        return false;
    }

    if ((r = uv_tcp_bind(&tcp_server, addr, 0)) < 0) {
        log_uv_error(LOG_EMERG, "dispatcher::start_server uv_tcp_bind", r);
        return false;
    }

    struct cb {
        static void on_conn(uv_stream_t *server, int status) {
            dispatcher *d = reinterpret_cast<dispatcher *>(server->data);
            d->on_connection(server, status);
        }
    };

    if ((r = uv_listen((uv_stream_t *)&tcp_server, 100, cb::on_conn)) < 0) {
        log_uv_error(LOG_EMERG, "dispatcher::start_server uv_listen", r);
        return false;
    }

    close_server.h = nullptr;
    log(LOG_NOTICE, "Server started at %s:%d", ip, port);
    return true;
}

void dispatcher::stop_server() noexcept
{
    // Check if server not closing because stop() called on failed start.
    // If server can't be started, uv_close called in start_server.
    if (!uv_is_closing((uv_handle_t *)&tcp_server))
        uv_close((uv_handle_t *)&tcp_server, nullptr);
    log(LOG_NOTICE, "Server stopped");
}

void dispatcher::spawn_worker() noexcept
{
    assert(loop);
    struct cb {
        static void on_exit(uv_process_t *p, int64_t exit_code, int signal) {
            worker_process *worker = static_cast<worker_process *>(p);
            dispatcher *d = reinterpret_cast<dispatcher *>(worker->owner);
            d->on_worker_exit(worker, exit_code, signal);
        }
    };
    worker_process *worker = new (std::nothrow) worker_process;
    if (!worker) {
        log(LOG_ERR, "dispatcher::spawn_worker not enough memory for worker");
        return;
    }
    struct deleter {
        static void cb(void *w) {
            delete_nothrow(reinterpret_cast<worker_process *>(w));
        }
    };

    if (!worker->start(loop, worker_name, worker_args, this,
                cb::on_exit, deleter::cb))
        // deleter::cb will be called sometime later, or already called.
        return;

    log(LOG_NOTICE, "Worker process %d started.", worker->pid);
    // on_worker_exit callback can be called only on next loop iteration
    // after exit from this function.
    // Therefore allowed to call push_back after worker->start.
    free_workers.push_back(worker);
    ++workers_cnt;

    struct cbr {
        static void alloc(uv_handle_t *h, size_t /*sz*/, uv_buf_t *buf) {
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
        }

        static void read(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf) {
            // process class stores pointer to it in it's pipes data member.
            // s is pipe.
            process *p = reinterpret_cast<process *>(s->data);
            worker_process *w = static_cast<worker_process *>(p);
            reinterpret_cast<dispatcher *>(w->owner)->on_worker_read(
                    w, nread, buf);
        }
    };
    // Start reading worker's stdout forever.
    int r = uv_read_start((uv_stream_t *)&worker->out, cbr::alloc, cbr::read);
    if (r < 0) {
        log_uv_error(LOG_ERR, "dispatcher::spawn_worker uv_read_start", r);
        kill_worker(worker);
        return;
    }
}

void dispatcher::kill_worker(worker_process *w) noexcept
{
    assert(loop);
    assert(!w->empty()); // worker must be in some list
    // Stop reading pipe to not receive eof.
    // on_worker_read assumes loop is not null.
    int r;
    if ((r = uv_read_stop((uv_stream_t *)&w->out)) < 0)
        log_uv_error(LOG_ERR, "dispatcher::kill_worker uv_read_stop", r);
    if ((r = uv_process_kill(w, SIGTERM)) < 0)
        log_uv_error(LOG_ERR, "dispatcher::kill_worker uv_process_kill", r);
    w->remove_from_list();
    w->timeout = uv_now(loop) + KILL_TIMEOUT;
    terminated_workers.push_back(w);
}

void dispatcher::on_worker_exit(worker_process *w, int64_t exit_code, int sig)
    noexcept
{
    log(LOG_NOTICE, "Worker %d exited with code %" PRId64 " caused signal %d.",
            w->pid, exit_code, sig);
    w->remove_from_list();
    if (w->processed_con)
        w->processed_con->remove_from_dispatcher();
    // Buffers may be safely reused only after worker exit.
    if (w->in_buf) {
        return_buffer(w->in_buf, true);
        w->in_buf = nullptr;
    }
    if (w->out_buf) {
        return_buffer(w->out_buf, false);
        w->out_buf = nullptr;
    }
    w->stop();
    --workers_cnt;
    schedule();
}

void dispatcher::on_connection(uv_stream_t *server, int status) noexcept
{
    assert(loop);
    if (status < 0) {
        log_uv_error(LOG_ERR, "on_connection", status);
        return;
    }

    log(LOG_DEBUG, "Connection received. Status = %d.", status);

    tcp_context *con = create_connection();
    if (!con) {
        log(LOG_ERR, "dispatcher::on_connection no memory for connection");
        return;
    }

    struct deleter {
        static void cb(tcp_con *p) {
            tcp_context *con = static_cast<tcp_context *>(p);
            dispatcher *d = reinterpret_cast<dispatcher *>(con->owner);
            d->free_connection(con);
        }
    };

    if (!con->accept(loop, server, this, deleter::cb))
        // deleter::cb will be called sometime later.
        return;

    if (!read_connection(con)) {
        con->remove_from_dispatcher();
        return;
    }

    move_to(tcp_context::LIST_IDLE, con);
}

bool dispatcher::read_connection(tcp_context *con) noexcept
{
    struct cb {
        static void alloc(uv_handle_t *h, size_t size, uv_buf_t *buf) {
            tcp_context *strm = static_cast<tcp_context *>(tcp_con::from(h));
            dispatcher *d = reinterpret_cast<dispatcher *>(strm->owner);
            d->read_con_alloc_cb(strm, size, buf);
        }

        static void read(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf) {
            tcp_context *strm = static_cast<tcp_context *>(tcp_con::from(s));
            dispatcher *d = reinterpret_cast<dispatcher *>(strm->owner);
            d->read_con_cb(strm, nread, buf);
        }
    };

    // Start connection reading.
    int r = uv_read_start(con->base<uv_stream_t *>(), cb::alloc, cb::read);
    if (r < 0) {
        log_uv_error(LOG_ERR, "dispatcher::read_connection uv_read_start", r);
        return false;
    }
    log(LOG_DEBUG, "dispatcher::read_connection reading connection begin");
    return true;
}

void dispatcher::read_con_alloc_cb(tcp_context *con, size_t suggested_size,
        uv_buf_t *buf) noexcept
{
    assert(loop);
    *buf = uv_buf_init(nullptr, 0);

    if (!con->read_buffer) {
        // Start new request reading.
        con->read_buffer = get_buffer(true);
        if (!con->read_buffer)
            return;
        if (!con->prepare_for_request(con->read_buffer))
            return;
    }
    assert(!con->empty()); // connection must be in some list
    assert(con->list_id == tcp_context::LIST_IDLE ||
           con->list_id == tcp_context::LIST_READING ||
           con->list_id == tcp_context::LIST_WRITING);
    if (con->list_id == tcp_context::LIST_WRITING)
        // May be client don't read response because it's sending requests now.
        // Delay sending therefore.
        move_to(tcp_context::LIST_WRITING, con);
    else
        // Update timeout for reading only if connection isn't writed now.
        // Because writing is more important.
        move_to(tcp_context::LIST_READING, con);

    shmem_buffer_node *sh_buf = con->read_buffer;
    if (sh_buf->map_ptr() == sh_buf->map_end()) {
        // Mapped shared memory is full. Map other chunk.
        size_t next_offs = sh_buf->cur_pos();
        if (next_offs == sh_buf->file_size() &&
            !sh_buf->resize(next_offs + suggested_size))
            return;
        size_t sz = std::min(sh_buf->file_size() - next_offs, suggested_size);
        if (!sh_buf->map(next_offs, sz))
            return;
    }
    *buf = uv_buf_init(sh_buf->map_ptr(),
            sh_buf->map_end() - sh_buf->map_ptr());
}

void dispatcher::read_con_cb(tcp_context *con, ssize_t nread, const uv_buf_t *)
    noexcept
{
    assert(loop);
    if (nread < 0) {
        log_uv_error(nread == UV_EOF ? LOG_DEBUG : LOG_ERR,
                "dispatcher::read_con_cb", nread);
        con->remove_from_dispatcher();
    }
    else if (nread > 0) {
        if (!con->parse_request(con->read_buffer, nread)) {
            con->remove_from_dispatcher();
            return;
        }

        // Fully received message to be processed.
        if (con->request_size()) {
            log(LOG_DEBUG, "Request message end (%" PRIuPTR " bytes).",
                    con->request_size());
            // Stop reading for scheduling and processing (or writing ready
            // responses).
            int r = uv_read_stop(con->base<uv_stream_t *>());
            if (r < 0) {
                log_uv_error(LOG_ERR, "dispatcher::read_con_cb uv_read_stop",
                        r);
                con->remove_from_dispatcher();
                return;
            }
            log(LOG_DEBUG, "dispatcher::read_con_cb reading connection end");
            assert(con->list_id == tcp_context::LIST_READING ||
                   con->list_id == tcp_context::LIST_WRITING);
            respond_or_enqueue(con);
            schedule();
        }
    }
}

void dispatcher::respond_or_enqueue(tcp_context *con) noexcept
{
    assert(con);
    assert(con->list_id == tcp_context::LIST_READING ||
           // in LIST_PROCESSING only after end of processing
           con->list_id == tcp_context::LIST_PROCESSING ||
           con->list_id == tcp_context::LIST_SCHEDULING ||
           con->list_id == tcp_context::LIST_WRITING);
    if (con->resp_buffers_num >= RESPONSES_MAXDEPTH) {
        // Don't process new request if responses queue is too large.
        // It will be done after sending all responses.
        move_to(tcp_context::LIST_WRITING, con);
        return;
    }

    if (!con->inplace_response(nullptr)) {
        move_to(tcp_context::LIST_SCHEDULING, con);
        return;
    }

    shmem_buffer_node *buf = get_buffer(false);
    if (!buf) {
        con->remove_from_dispatcher();
        return;
    }

    buf->remove_from_list(); // was in in_use_bufs list
    con->resp_buffers.push_back(buf);
    ++con->resp_buffers_num;
    move_to(tcp_context::LIST_WRITING, con);
    if (!con->inplace_response(buf)) {
        con->remove_from_dispatcher();
        return;
    }
    if (con->resp_buffers_num == 1)
        write_con(con);
}

void dispatcher::schedule() noexcept
{
    assert(loop);
    while (!clients_scheduling.empty() &&
           clients_scheduling.front().inplace_response(nullptr)) {
        tcp_context *con = &clients_scheduling.front();
        respond_or_enqueue(con);
        assert(con->empty() || con->list_id == tcp_context::LIST_WRITING);
    }

    if (clients_scheduling.empty() ||
        (workers_cnt >= workers_max && free_workers.empty()))
        return;

    if (free_workers.empty()) {
        spawn_worker();
        if (free_workers.empty()) {
            // Сan't serve any request if spawning worker failed.
            log(LOG_ERR, "dispatcher::schedule no worker for request. "
                "Close connections.");
            close_connections(clients_scheduling);
            return;
        }
    }

    shmem_buffer_node * resp_buf = get_buffer(false);
    if (!resp_buf) {
        // Сan't serve any request if opening buffer failed.
        log(LOG_ERR, "dispatcher::schedule no buffer for response. "
                "Close connections.");
        close_connections(clients_scheduling);
        return;
    }

    tcp_context *con = nullptr;
    // take worker for request
    worker_process &w = *free_workers.begin();
    assert(w.io_state == worker_process::IO_IDLE);
    // pipe_buf_ptr moved to start when state became IO_IDLE
    assert(w.pipe_buf_ptr == w.pipe_buf);

    // make request params to send into worker
    int req_len = 0;
    while (!clients_scheduling.empty()) {
        con = &clients_scheduling.front();
        req_len = snprintf(w.pipe_buf, sizeof(w.pipe_buf),
            "%s IN SHM %s %" PRIuPTR " OUT SHM %s %" PRIuPTR "\n",
            con->request_protocol(), con->read_buffer->name(),
            con->request_size(), resp_buf->name(), resp_buf->file_size());
        if (req_len >= 0 && req_len < (int)sizeof(w.pipe_buf))
            break;
        log(LOG_ERR, "dispatcher::schedule can't snprintf request params");
        con->remove_from_dispatcher();
        con = nullptr;
    }
    if (!con) {
        return_buffer(resp_buf, false);
        return;
    }
    assert(con->request_size());

    // Connect request and worker.
    w.remove_from_list();
    w.processed_con = con;
    w.in_buf = con->read_buffer;
    con->read_buffer = nullptr; // buffer now owned by worker
    w.out_buf = resp_buf; // buffer owned by worker for processing time
    con->worker = &w;
    w.timeout = uv_now(loop) + PROCESSING_TIMEOUT;
    in_use_workers.push_back(&w);
    move_to(tcp_context::LIST_PROCESSING, con); // Reading already was stopped.

    struct cb {
        static void write(uv_write_t *req, int status) {
            // This callback may be called after worker death.
            // But worker_process structure will alive while pipe's
            // structures alive.
            worker_process *w = reinterpret_cast<worker_process *>(req->data);
            dispatcher *d = reinterpret_cast<dispatcher *>(w->owner);
            if (status != 0) {
                log_uv_error(LOG_ERR, "dispatcher::schedule cb::write", status);
                if (w->processed_con)
                    w->processed_con->remove_from_dispatcher();
                d->kill_worker(w);
                return;
            }
            log(LOG_DEBUG, "dispatcher::schedule cb::write request sent to "
                    "worker");
            assert(w->io_state == worker_process::IO_WRITE);
            // Before writing pipe_buf_ptr was at start. Writing don't move it.
            assert(w->pipe_buf_ptr == w->pipe_buf);
            w->io_state = worker_process::IO_READ;
        }
    };

    // Send request to worker.
    w.write_req.data = &w;
    w.io_state = worker_process::IO_WRITE;
    uv_buf_t buf = uv_buf_init(w.pipe_buf, req_len);
    int r = uv_write(&w.write_req, (uv_stream_t *)&w.in, &buf, 1, cb::write);
    if (r < 0) {
        log_uv_error(LOG_ERR, "dispatcher::schedule uv_write", r);
        kill_worker(&w);
        con->remove_from_dispatcher();
    }
}

void dispatcher::on_worker_read(worker_process *w, ssize_t nread,
        const uv_buf_t *buf) noexcept
{
    assert(loop); // Because uv_read_stop was called in kill_process.
    if (nread < 0) {
        log_uv_error(LOG_ERR, "dispatcher::on_worker_read nread", nread);
        kill_worker(w);
        return;
    }

    if (w->io_state != worker_process::IO_READ) {
        log(LOG_ERR, "dispatcher::on_worker_read worker not in read state");
        kill_worker(w);
        return;
    }

    w->pipe_buf_ptr += nread;

    // Read until end of line.
    if (!nread || buf->base[nread - 1] != '\n')
        return;

    // Now response from worker fully received.
    log(LOG_DEBUG, "dispatcher::on_worker_read response received from worker");

    // Parse response length.
    buf->base[nread - 1] = 0;
    size_t resp_len;
    size_t resp_file_size;
    if (sscanf(w->pipe_buf, "RESP %" SCNuPTR " of %" SCNuPTR " END",
                &resp_len, &resp_file_size) != 2) {
        log(LOG_ERR, "dispatcher::on_worker_read sscanf can't parse response "
                "\"%s\".", buf->base);
        kill_worker(w);
        return;
    }
    log(LOG_DEBUG, "dispatcher::on_worker_read response of %" PRIuPTR " bytes "
            "ready", resp_len);

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

        // May be some part of new request was readed with previous one (but
        // not parsed yet). If so parse it. Else simply reset request_size().
        if (w->in_buf->cur_pos() < w->in_buf->data_size())
            con->read_buffer = w->in_buf;
        else
            return_buffer(w->in_buf, true);
        w->in_buf = nullptr;
        if (!con->prepare_for_request(con->read_buffer)) {
            con->remove_from_dispatcher();
            con = nullptr;
        }

        assert(!w->out_buf->map_offset());
        assert(w->out_buf->map_ptr() == w->out_buf->map_begin());
        assert(w->out_buf->map_begin() != w->out_buf->map_end());
        assert(!w->out_buf->empty()); // Was in in_use_bufs list.
        w->out_buf->set_data_size(resp_len);
        w->out_buf->remove_from_list();
        con->resp_buffers.push_back(w->out_buf);
        ++con->resp_buffers_num;
        w->out_buf = nullptr;
        // If resp_buffers_num becames 1 now then writing not started yet.
        // If resp_buffers_num > 1 then writing of some other buffer already
        // in progress.
        if (con->resp_buffers_num == 1)
            write_con(con);
    }
    else {
        // Connection was closed before worker processing finished.
        return_buffer(w->out_buf, false);
        w->out_buf = nullptr;
    }

    w->io_state = worker_process::IO_IDLE;
    w->pipe_buf_ptr = w->pipe_buf;
    w->remove_from_list();
    free_workers.push_back(w);

    if (con) {
        // Reading now stopped, because connection was in processing list.
        if (con->read_buffer && con->request_size())
            // With previous request the second request was fully readed too
            // and was parsed in prepare_for_request().
            // Process it now or when responses queue will become empty.
            respond_or_enqueue(con);
        else {
            move_to(tcp_context::LIST_WRITING, con);
            // After request processed start reading next.
            if (!read_connection(con)) {
                con->remove_from_dispatcher();
                con = nullptr;
            }
        }
    }
    schedule();
}

void dispatcher::write_con(tcp_context *con) noexcept
{
    assert(loop);
    assert(!con->resp_buffers.empty());
    shmem_buffer_node *buf = &con->resp_buffers.front();
    // initialize for response before first chunk of data
    if (!buf->cur_pos() && !con->prepare_for_response()) {
        con->remove_from_dispatcher();
        return;
    }

    if (buf->cur_pos() >= buf->data_size()) {
        on_end_write_con(con);
        return;
    }

    assert(!con->empty()); // must be in some list
    // Writing response for one response and scheduling/processing the second
    // response can be at the same time. In this case connection is in
    // LIST_SCHEDULING/LIST_PROCESSING.
    assert(con->list_id == tcp_context::LIST_SCHEDULING ||
           con->list_id == tcp_context::LIST_PROCESSING ||
           con->list_id == tcp_context::LIST_WRITING);
    if (con->list_id == tcp_context::LIST_WRITING)
        move_to(tcp_context::LIST_WRITING, con);

    if (buf->map_ptr() == buf->map_end()) {
        // Mapped chunk was fully written. Map next chunk.
        size_t map_size = std::min(RESPONSE_CHUNK,
                buf->data_size() - buf->cur_pos());
        if (!buf->map(buf->cur_pos(), map_size)) {
            con->remove_from_dispatcher();
            return;
        }
    }

    if (!con->parse_response(buf)) {
        con->remove_from_dispatcher();
        return;
    }

    size_t chunk_size = std::min(size_t(buf->map_end() - buf->map_ptr()),
            buf->data_size() - buf->cur_pos());

    struct cb {
        static void write(uv_write_t *req, int status) {
            tcp_context *con = static_cast<tcp_context *>(
                    tcp_con::from(req->handle));
            size_t chunk_size = (size_t)req->data;
            if (status < 0) {
                log_uv_error(LOG_ERR, "dispatcher::write_con cb::write",
                        status);
                con->remove_from_dispatcher();
                return;
            }
            con->resp_buffers.front().move_ptr(chunk_size);
            log(LOG_DEBUG, "Response chunk of %" PRIuPTR " bytes written",
                    chunk_size);
            reinterpret_cast<dispatcher *>(con->owner)->write_con(con);
        }
    };

    uv_buf_t wbuf = uv_buf_init(buf->map_ptr(), chunk_size);
    con->write_req.data = (void *)chunk_size;
    int r = uv_write(&con->write_req, con->base<uv_stream_t *>(), &wbuf, 1,
            cb::write);
    if (r < 0) {
        log_uv_error(LOG_ERR, "dispatcher::write_con uv_write", r);
        con->remove_from_dispatcher();
        return;
    }
}

void dispatcher::on_end_write_con(tcp_context *con) noexcept
{
    assert(loop);
    log(LOG_DEBUG, "dispatcher::on_end_write_con response sended");
    if (con->finish_response()) {
        return_buffer(&con->resp_buffers.front(), false);
        --con->resp_buffers_num;

        assert(con->list_id == tcp_context::LIST_SCHEDULING ||
               con->list_id == tcp_context::LIST_PROCESSING ||
               con->list_id == tcp_context::LIST_WRITING);
        if (!con->resp_buffers.empty())
            write_con(con);
        else if (con->list_id == tcp_context::LIST_WRITING) {
            // Connection not scheduled nor processed now.
            // Therefore it's available to schedule it or start reading.

            if (con->read_buffer && con->request_size()) {
                // Connection has fully readed request message but it's not in
                // scheduling or processing list. It's because of schedule()
                // wasn't called because of resp_buffers list was too large.
                // Now resp_buffers list is empty, so call schedule() here.
                // Reading stopped at this moment.
                respond_or_enqueue(con);
                schedule();
            }
            else {
                if (con->read_buffer)
                    // Connection has partially readed message.
                    move_to(tcp_context::LIST_READING, con);
                else
                    // Connection is inactive.
                    move_to(tcp_context::LIST_IDLE, con);
                if (!read_connection(con)) // it's idempotent
                    con->remove_from_dispatcher();
            }
        }
    }
    else
        con->remove_from_dispatcher();
}

void dispatcher::close_connections(list_node<tcp_context> &list) noexcept
{
    for (auto it = list.begin(); it != list.end();) {
        tcp_context::iterator con = it++;
        con->remove_from_dispatcher();
    }
}

void dispatcher::move_to(tcp_context::list_id_enum dst, tcp_context *con)
    noexcept
{
    assert(loop);
    if (!con->empty())
        con->remove_from_list();
    con->list_id = dst;
    const char *dst_name = "";
    if (dst == tcp_context::LIST_IDLE) {
        con->timeout = uv_now(loop) + IDLE_TIMEOUT;
        clients_idle.push_back(con);
        dst_name = "LIST_IDLE";
    }
    else if (dst == tcp_context::LIST_READING) {
        con->timeout = uv_now(loop) + READ_TIMEOUT;
        clients_reading.push_back(con);
        dst_name = "LIST_READING";
    }
    else if (dst == tcp_context::LIST_SCHEDULING) {
        clients_scheduling.push_back(con);
        dst_name = "LIST_SCHEDULING";
    }
    else if (dst == tcp_context::LIST_PROCESSING) {
        clients_processing.push_back(con);
        dst_name = "LIST_PROCESSING";
    }
    else if (dst == tcp_context::LIST_WRITING) {
        con->timeout = uv_now(loop) + WRITE_TIMEOUT;
        clients_writing.push_back(con);
        dst_name = "LIST_WRITING";
    }
    log(LOG_DEBUG, "dispatcher::move_to connection moved to list %s", dst_name);
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
        assert(buf->map_ptr() == buf->map_begin());
        in_use_bufs.push_front(buf);
        return buf;
    }

    shmem_buffer_node *buf = new (std::nothrow) shmem_buffer_node;
    if (!buf) {
        log(LOG_ERR, "dispatcher::get_buffer no memory for shmem_buffer_node");
        return nullptr;
    }
    if (!buf->open(nullptr, true)) {
        delete_nothrow(buf);
        return nullptr;
    }
    if (!buf->reset_defaults(for_request ? REQUEST_CHUNK : RESPONSE_CHUNK)) {
        buf->close();
        delete_nothrow(buf);
        return nullptr;
    }
    buf->set_data_size(0);
    in_use_bufs.push_front(buf);
    return buf;
}

void dispatcher::return_buffer(shmem_buffer_node *buf, bool for_request)
    noexcept
{
    buf->remove_from_list(); // was in in_use_bufs or connection's resp_buffers
    if (!buf->reset_defaults(for_request ? REQUEST_CHUNK : RESPONSE_CHUNK)) {
        buf->close();
        delete_nothrow(buf);
        return;
    }
    assert(buf->map_ptr() == buf->map_begin()); // after reset_defaults
    buf->set_data_size(0);
    (for_request ? req_bufs : resp_bufs).push_front(buf);
}

void dispatcher::close_buffers(list_node<shmem_buffer_node> &buf_list) noexcept
{
    assert(loop);
    for (auto it = buf_list.begin(); it != buf_list.end();) {
        shmem_buffer_node::iterator buf = it++;
        buf->remove_from_list();
        buf->close();
        // tcp_stream and worker, which uses this buffer, must be closed before
        // and must not use buffer in close_cb.
        delete_nothrow(&*buf);
    }
}

bool dispatcher::start_timer() noexcept
{
    assert(loop);
    int r;
    close_on_return close_timer((uv_handle_t *)&timer, nullptr);
    if ((r = uv_timer_init(loop, &timer)) < 0) {
        log_uv_error(LOG_ERR, "dispatcher::start_timer uv_timer_init", r);
        return false;
    }

    struct cb {
        static void timeout(uv_timer_t *t) {
            reinterpret_cast<dispatcher *>(t->data)->on_timer_tick();
        }
    };

    timer.data = this;
    if ((r = uv_timer_start(&timer, cb::timeout, 0, TIMER_PERIOD)) < 0) {
        log_uv_error(LOG_ERR, "dispatcher::start_timer uv_timer_start", r);
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
    assert(loop);
    uint64_t now = uv_now(loop);
    while (!in_use_workers.empty() && in_use_workers.front().timeout <= now)
        kill_worker(&in_use_workers.front());
    for (const worker_process &w : terminated_workers) {
        if (w.timeout > now)
            break;
        uv_process_kill(&terminated_workers.front(), SIGKILL);
    }
    close_old_connections(clients_idle);
    close_old_connections(clients_reading);
    close_old_connections(clients_writing);
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

void dispatcher::tcp_context::remove_from_dispatcher() noexcept
{
    dispatcher *d = reinterpret_cast<dispatcher *>(owner);
    if (read_buffer) {
        d->return_buffer(read_buffer, true);
        read_buffer = nullptr;
    }
    while (!resp_buffers.empty()) {
        d->return_buffer(&resp_buffers.front(), false);
        --resp_buffers_num;
    }
    if (worker)
        worker->processed_con = nullptr;
    if (!empty()) // may be not in any list (for example, in schedule)
        remove_from_list();
    close();
    log(LOG_DEBUG, "Connection closed.");
}

} // namespace pruv
