/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <memory>

#include <uv.h>

#include <pruv/list_node.hpp>
#include <pruv/process.hpp>
#include <pruv/shmem_buffer.hpp>
#include <pruv/tcp_con.hpp>
#include <pruv/tcp_server.hpp>

namespace pruv {

class dispatcher {
public:
    virtual ~dispatcher();
    /// new_loop, worker_name and worker_args must be valid until stop called.
    void start(uv_loop_t *new_loop, const char *ip, int port,
            size_t workers_max, const char *worker_name,
            const char * const *worker_args) noexcept;
    void stop() noexcept;
    /// Stop timer.
    void on_loop_exit() noexcept;

private:
    struct shmem_buffer_node : shmem_buffer, list_node<shmem_buffer_node> {};

    struct worker_process;

protected:
    /// Buffered tcp connection
    class tcp_context : private tcp_con, list_node<tcp_context> {
    public:
        virtual ~tcp_context() {}
        struct request_meta {
            size_t pos;
            size_t size;
            const char *protocol;
            bool inplace;
            request_meta() : pos(0), size(0), protocol(""), inplace(false) {}
        };

    protected:
        /// Called after reading data into buffer.
        /// Also called with nullptr when releasing previous buffer.
        /// If returns false, connection will be closed.
        virtual bool parse_request(shmem_buffer *buf) noexcept = 0;
        /// Fill r with pos and size of currently parsed request.
        /// Returns if has fully parsed request and switches to the next one.
        virtual bool get_request(request_meta &r) noexcept = 0;
        /// Makes response in buf_out.
        /// If returns false, connection will be closed.
        virtual bool inplace_response(const request_meta &r,
                shmem_buffer &buf_in, shmem_buffer &buf_out) noexcept = 0;

        /// Called after response message received from worker or
        /// when inplace response generated.
        /// If returns false, connection will be closed.
        virtual bool response_ready(const request_meta &r,
                const shmem_buffer &resp_buf) noexcept = 0;
        /// Called before sending data currently mapped in the buf.
        /// If returns false, connection will be closed.
        virtual bool parse_response(shmem_buffer &buf) noexcept = 0;
        /// Called after writing last response chunk.
        /// If returns false, connection will be closed.
        virtual bool finish_response(const shmem_buffer &resp_buf) noexcept = 0;

    private:
        friend dispatcher;
        friend list_node;
        dispatcher * get_dispatcher() const noexcept;
        /// Remove connection from any dispatcher's list.
        /// Return used buffers into dispatcher.
        /// Break reference from worker to this connection.
        void remove_from_dispatcher() noexcept;

        /// Buffer for reading request.
        shmem_buffer_node *read_buffer = nullptr;
        /// Buffers with responses.
        list_node<shmem_buffer_node> resp_buffers;
        /// Worker processing last request. On EOF there is no need to stop it,
        /// but its result must be ignored. Stored in connection to break
        /// reference worker->processed_con on EOF received.
        worker_process *worker = nullptr;
        /// Parameters of last processed request
        request_meta request;
        uv_write_t write_req;
        uint64_t timeout;

#define LIST_ID_MAP(XX) \
        XX(LIST_IDLE) \
        XX(LIST_IO) \
        XX(LIST_SCHEDULING) \
        XX(LIST_PROCESSING)
        enum list_id_enum {
#define XX(x) x,
            LIST_ID_MAP(XX)
#undef XX
        } list_id;
        static constexpr const char * list_names[] = {
#define XX(x) #x,
            LIST_ID_MAP(XX)
#undef XX
        };
    };

    /// Allocate connection structure.
    virtual tcp_context * create_connection() noexcept = 0;
    /// Destruct and free connection structure.
    virtual void free_connection(tcp_context *con) noexcept = 0;

private:
    struct worker_process : public process, list_node<worker_process> {
        /// Connection, whose request this worker process now.
        /// It's needed because on exit connection must be closed.
        tcp_context *processed_con = nullptr;

        /// Buffers with request and response.
        /// When connection closed, but worker still process request, buffers
        /// can't be reused. Therefore buffers owned by worker for processing
        /// time.
        shmem_buffer_node *in_buf = nullptr;
        shmem_buffer_node *out_buf = nullptr;

        /// io_state used to protect single pipe_buf.
        /// Buffer can be used for reading when IO_READ,
        /// for writing when IO_WRITE, and can't be used when IO_IDLE.
        /// Also it allows not stop polling pipe when worker has no work.
        enum {
            IO_IDLE,
            IO_READ,
            IO_WRITE
        } io_state = IO_IDLE;
        /// Time when current operation must be finished.
        uint64_t timeout;

        /// Buffer for request to worker and for response from it.
        char pipe_buf[256];
        /// Pointer inside to pipe_buf for reading response by chunks.
        char *pipe_buf_ptr = pipe_buf;
        /// Request for writing task into worker's pipe.
        uv_write_t write_req;
        /// Because waitpid() called before on_worker_exit(), it's must be
        /// stored that kill is not allowed inside on_worker_exit().
        bool exited = false;
    };

    /// Start listening ip:port and initialize callbacks for accepting.
    bool start_server(const char *ip, int port) noexcept;
    /// Stop listening.
    void stop_server() noexcept;

    /// Spawn worker process and put it into free_workers list.
    void spawn_worker() noexcept;
    /// Kill worker and stop reading it's pipe. Worker must be in some list.
    void kill_worker(worker_process *w) noexcept;
    /// Release resources used by worker and close waiting connection.
    void on_worker_exit(worker_process *w, int64_t exit_code, int signal)
        noexcept;
    /// Accepts connection and initializes callbacks for reading data.
    void on_connection(uv_stream_t *server, int status) noexcept;
    /// Prepare buffer for reading data from connection.
    void read_con_alloc_cb(tcp_context *con, size_t suggested_size,
            uv_buf_t *buf) noexcept;
    /// Process data readed from connection.
    void read_con_cb(tcp_context *con, ssize_t nread, const uv_buf_t *buf)
        noexcept;
    /// If inplace response can be done then respond with it
    /// else enqueue for scheduling request to worker.
    void respond_or_enqueue(tcp_context *con) noexcept;
    /// Take one request and one worker and send request into worker.
    void schedule() noexcept;
    /// Read response from workers stdout pipe and enqueue it for sending.
    void on_worker_read(worker_process *w, ssize_t nread, const uv_buf_t *buf)
        noexcept;
    /// Write data from con->resp_buffers.front() into connection by chunks.
    void write_con(tcp_context *con) noexcept;
    /// Called after writing last chunk of response to connection.
    /// Prepares connection for reading next request.
    void on_end_write_con(tcp_context *con) noexcept;

    /// Remove connection from current list and push_back to specified.
    /// Update connections timeout.
    void move_to(tcp_context::list_id_enum dst, tcp_context *con) noexcept;

    /// Take buffer from cache or create new.
    shmem_buffer_node * get_buffer(bool for_request) noexcept;
    /// Return buffer into cache. Remove buffer from its current list.
    void return_buffer(shmem_buffer_node &buf, bool for_request) noexcept;
    /// Return buffer into cache. Remove buffer from its current list.
    void return_buffer(shmem_buffer_node **buf, bool for_request) noexcept;
    /// Close all shared memory objects in list and free memory.
    /// Must be called only when there is no references to any buffer
    /// in the buf_list.
    void close_buffers(list_node<shmem_buffer_node> &buf_list) noexcept;

    bool start_timer() noexcept;
    void close_timer() noexcept;
    /// Make cleanup routines. Check connections and workers timeouts.
    void on_timer_tick() noexcept;
    /// Close all connections in list and release its resources.
    void close_connections(list_node<tcp_context> &list) noexcept;
    /// Close timed out connections in list. list sorted by timeout.
    void close_old_connections(list_node<tcp_context> &list) noexcept;


    static constexpr unsigned IDLE_TIMEOUT = 30'000;
    static constexpr unsigned IO_TIMEOUT = 10'000;
    static constexpr unsigned PROCESSING_TIMEOUT = 10'000;
    static constexpr unsigned KILL_TIMEOUT = 10'000;
    static constexpr unsigned TIMER_PERIOD = 5'000;

    uv_loop_t *loop = nullptr;
    const char *worker_name = nullptr;
    const char * const *worker_args = nullptr;
    size_t workers_cnt = 0;
    size_t workers_max = 0;

    tcp_server server;
    uv_timer_t timer;

    /// Connections in this list are inactive.
    list_node<tcp_context> clients_idle;
    /// Connections in this list are readed (has some not processed data)
    /// or with partially writed response.
    list_node<tcp_context> clients_io;
    /// Connections with fully read request without worker.
    /// Connections may be readed and writed, but parsing stopped for its.
    list_node<tcp_context> clients_scheduling;
    /// Connections waits response from worker.
    /// Connections may be readed and writed, but parsing stopped for its.
    list_node<tcp_context> clients_processing;

    /// Idle workers without job.
    list_node<worker_process> free_workers;
    /// Workers serving some request now.
    list_node<worker_process> in_use_workers;
    /// Workers to which SIGTERM signal is sent.
    list_node<worker_process> terminated_workers;

    /// Free buffers for reading request.
    list_node<shmem_buffer_node> req_bufs;
    /// Free buffers for writing response.
    list_node<shmem_buffer_node> resp_bufs;
};

} // namespace pruv
