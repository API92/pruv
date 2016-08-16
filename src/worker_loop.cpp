/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/worker_loop.hpp>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstring>

#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <pruv/log.hpp>
#include <pruv/shmem_buffer.hpp>
#include <pruv/shmem_cache.hpp>
#include <pruv/termination.hpp>

namespace pruv {

namespace {

void termhdlr(int sig)
{
    set_interruption(sig == SIGINT ? IRQ_INT : IRQ_TERM);
}

} // namespace

int worker_loop::setup() noexcept
{
    struct sigaction sigact;
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = termhdlr;
    sigemptyset(&sigact.sa_mask);
    int r = sigaction(SIGTERM, &sigact, nullptr);
    if (r == -1)
        pruv_log_syserr(LOG_ERR, "worker_loop sigaction(SIGTERM)");
    r = sigaction(SIGINT, &sigact, nullptr);
    if (r == -1)
        pruv_log_syserr(LOG_ERR, "worker_loop sigaction(SIGINT)");
    r = sigaction(SIGHUP, &sigact, nullptr);
    if (r == -1)
        pruv_log_syserr(LOG_ERR, "worker_loop sigaction(SIGHUP)");
    r = prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
    if (r == -1)
        pruv_log_syserr(LOG_ERR, "worker_loop prctl");
    if (getppid() == 1) {
        pruv_log(LOG_ERR, "Orphaned at start. Exit.");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int worker_loop::run() noexcept
{
    for (;;) {
        if (!next_request()) {
            if (interruption_requested())
                break;
            else
                return EXIT_FAILURE;
        }

        int r = handle_request();
        if (interruption_requested())
            break;
        else if (r != EXIT_SUCCESS)
            return r;
        if (!clean_after_request())
            return EXIT_FAILURE;
    }
    pruv_log(LOG_NOTICE, "Terminated.");
    return EXIT_SUCCESS;
}

bool worker_loop::read_line() noexcept
{
    char *dst = ln;
    while (!interruption_requested()) {
        ssize_t r = read(STDIN_FILENO, dst, std::end(ln) - dst);
        if (r == -1) {
            if (errno == EINTR)
                continue;
            else {
                pruv_log_syserr(LOG_ERR, "read(STDIN_FILENO)");
                return false;
            }
        }
        dst += r;
        char *eol = std::find(dst - r, dst, '\n');
        if (eol != dst) {
            *eol = 0;
            return true;
        }
        if (dst >= std::end(ln)) {
            pruv_log(LOG_ERR, "Too large input line");
            return false;
        }
    }
    return false;
}

bool worker_loop::next_request() noexcept
{
    if (!read_line())
        return false;

    size_t buf_in_pos;
    size_t buf_in_len;
    size_t buf_out_file_size;
    int parsed = sscanf(ln, "%255s IN SHM %255s %" SCNuPTR ", %" SCNuPTR
            " OUT SHM %255s %" SCNuPTR, req_protocol, buf_in_name,
            &buf_in_pos, &buf_in_len, buf_out_name, &buf_out_file_size);
    if (parsed != 6) {
        pruv_log(LOG_ERR, "Error parsing \"%s\"", ln);
        return false;
    }

    static const size_t PAGESIZE_MASK = sysconf(_SC_PAGE_SIZE) - 1;

    shmem_buffer *buf_in = buf_in_cache.get(buf_in_name);
    if (!buf_in)
        return false;
    size_t buf_in_base_pos = buf_in_pos & ~PAGESIZE_MASK;
    size_t in_len = buf_in_pos + buf_in_len - buf_in_base_pos;
    size_t buf_in_end_pos = buf_in->map_offset() +
        (buf_in->map_end() - buf_in->map_begin());
    if (!(buf_in->map_offset() <= buf_in_base_pos &&
          buf_in_base_pos + in_len <= buf_in_end_pos)) {
        if (!buf_in->map(buf_in_base_pos, in_len))
            return false;
    }
    buf_in->move_ptr((ptrdiff_t)buf_in_pos - (ptrdiff_t)buf_in->cur_pos());

    shmem_buffer *buf_out = buf_out_cache.get(buf_out_name);
    if (!buf_out)
        return false;
    buf_out->update_file_size(buf_out_file_size);

    request_buf = buf_in;
    request = buf_in->map_ptr();
    request_len = buf_in_len;
    response_buf = buf_out;
    return true;
}

bool worker_loop::send_last_response() noexcept
{
    bool ok = true;
    if (response_buf->map_end() - response_buf->map_begin() >
            (ptrdiff_t)RESPONSE_CHUNK)
        ok &= response_buf->unmap();
    if (!ok) {
        response_buf = nullptr;
        return false;
    }

    ok &= printf("RESP %" PRIuPTR " of %" PRIuPTR " END\n",
            response_buf->data_size(), response_buf->file_size()) >= 0;
    ok &= fflush(stdout) == 0;
    response_buf = nullptr;
    return ok;
}

bool worker_loop::clean_after_request() noexcept
{
    assert(request_buf);
    bool ok = true;
    if (request_buf->map_offset() +
            (request_buf->map_end() - request_buf->map_begin()) > REQUEST_CHUNK)
        ok &= request_buf->unmap();
    request_buf = nullptr;
    return ok;
}

} // namespace pruv
