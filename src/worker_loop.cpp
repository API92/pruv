/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/worker_loop.hpp>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstring>
#include <iterator>

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
    setup_interruption(sig == SIGINT ? IRQ_INT : IRQ_TERM);
}

} // namespace

int worker_loop::_argc = 0;
char const * const * worker_loop::_argv = nullptr;

worker_loop::worker_loop() {}

int worker_loop::setup(int argc, char const * const *argv) noexcept
{
    _argc = argc;
    _argv = argv;
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

int worker_loop::argc() noexcept
{
    return _argc;
}

char const * const * worker_loop::argv() noexcept
{
    return _argv;
}

int worker_loop::run() noexcept
{
    for (;;) {
        if (!next_request()) {
            if (interruption_requested() == IRQ_TERM)
                break;
            else
                return EXIT_FAILURE;
        }

        int r = handle_request();
        if (interruption_requested() == IRQ_TERM)
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
    char *dst = _ln;
    while (interruption_requested() != IRQ_TERM) {
        ssize_t r = read(STDIN_FILENO, dst, std::end(_ln) - dst);
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
        if (dst >= std::end(_ln)) {
            pruv_log(LOG_ERR, "Too large input line");
            return false;
        }
    }
    return false;
}

bool worker_loop::recv_request_cmd(
        char (&buf_in_name)[256], size_t &buf_in_pos, size_t &buf_in_len,
        char (&buf_out_name)[256], size_t &buf_out_file_size,
        char *meta, size_t meta_len) noexcept
{
    if (!read_line())
        return false;

    int readed;
    int parsed = sscanf(_ln, "IN SHM %255s %" SCNuPTR ", %" SCNuPTR
            " OUT SHM %255s %" SCNuPTR " META %n",
            buf_in_name, &buf_in_pos, &buf_in_len,
            buf_out_name, &buf_out_file_size, &readed);
    if (parsed != 5) {
        pruv_log(LOG_ERR, "Error parsing \"%s\"", _ln);
        return false;
    }

    strncpy(meta, &_ln[readed], meta_len);
    if (meta[meta_len - 1]) {
        pruv_log(LOG_ERR, "Request meta too long.");
        return false;
    }

    return true;
}

bool worker_loop::next_request() noexcept
{
    size_t buf_in_pos;
    size_t buf_in_len;
    size_t buf_out_file_size;
    if (!recv_request_cmd(
                _buf_in_name, buf_in_pos, buf_in_len,
                _buf_out_name, buf_out_file_size,
                _req_meta, sizeof(_req_meta)))
        return false;

    shmem_buffer *buf_in = _buf_in_cache.get(_buf_in_name);
    if (!buf_in)
        return false;
    size_t buf_in_base_pos = buf_in_pos & ~shmem_buffer::PAGE_SIZE_MASK;
    size_t in_len = buf_in_pos + buf_in_len - buf_in_base_pos;
    size_t buf_in_end_pos = buf_in->map_offset() +
        (buf_in->map_end() - buf_in->map_begin());
    if (!(buf_in->map_offset() <= buf_in_base_pos &&
          buf_in_base_pos + in_len <= buf_in_end_pos)) {
        if (!buf_in->map(buf_in_base_pos, in_len))
            return false;
    }
    buf_in->move_ptr((ptrdiff_t)buf_in_pos - (ptrdiff_t)buf_in->cur_pos());

    shmem_buffer *buf_out = _buf_out_cache.get(_buf_out_name);
    if (!buf_out)
        return false;
    buf_out->update_file_size(buf_out_file_size);

    _request_buf = buf_in;
    _request = buf_in->map_ptr();
    _request_len = buf_in_len;
    _response_buf = buf_out;
    return true;
}

bool worker_loop::send_last_response() noexcept
{
    bool ok = true;
    if (_response_buf->map_end() - _response_buf->map_begin() >
            (ptrdiff_t)RESPONSE_CHUNK)
        ok &= _response_buf->unmap();
    if (!ok) {
        _response_buf = nullptr;
        return false;
    }

    ok &= emit_last_response_cmd();
    _response_buf = nullptr;
    return ok;
}

bool worker_loop::emit_last_response_cmd() noexcept
{
    return
        printf("RESP %" PRIuPTR " of %" PRIuPTR " END\n",
            _response_buf->data_size(), _response_buf->file_size()) >= 0 &&
        fflush(stdout) == 0;
}

bool worker_loop::clean_after_request() noexcept
{
    assert(_request_buf);
    bool ok = true;
    if (_request_buf->map_offset() +
        (_request_buf->map_end() - _request_buf->map_begin()) > REQUEST_CHUNK)
        ok &= _request_buf->unmap();
    _request_buf = nullptr;
    return ok;
}

} // namespace pruv
