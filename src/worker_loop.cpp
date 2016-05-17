/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/worker_loop.hpp>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstring>

#include <signal.h>
#include <unistd.h>

#include <pruv/log.hpp>
#include <pruv/shmem_buffer.hpp>
#include <pruv/shmem_cache.hpp>
#include <pruv/termination.hpp>

namespace pruv {

int process_input(const char *s, request_handler handler)
{
    static thread_local char buf_in_name[256];
    static thread_local size_t buf_in_pos;
    static thread_local size_t buf_in_len;
    static thread_local char buf_out_name[256];
    static thread_local size_t buf_out_file_size;

    int parsed = sscanf(s, "HTTP IN SHM %255s %" SCNuPTR ", %" SCNuPTR
            " OUT SHM %255s %" SCNuPTR, buf_in_name,
            &buf_in_pos, &buf_in_len, buf_out_name, &buf_out_file_size);
    if (parsed != 5) {
        log(LOG_ERR, "Error parsing \"%s\"", s);
        return EXIT_FAILURE;
    }

    static shmem_cache buf_in_cache;
    static shmem_cache buf_out_cache;
    static const size_t PAGESIZE_MASK = sysconf(_SC_PAGE_SIZE) - 1;

    shmem_buffer *buf_in = buf_in_cache.get(buf_in_name);
    if (!buf_in)
        return EXIT_FAILURE;
    size_t buf_in_base_pos = buf_in_pos & ~PAGESIZE_MASK;
    size_t in_len = buf_in_pos + buf_in_len - buf_in_base_pos;
    size_t buf_in_end_pos = buf_in->map_offset() +
        (buf_in->map_end() - buf_in->map_begin());
    if (!(buf_in->map_offset() <= buf_in_base_pos &&
          buf_in_base_pos + in_len <= buf_in_end_pos)) {
        if (!buf_in->map(buf_in_base_pos, in_len))
            return EXIT_FAILURE;
    }
    if (!buf_in->seek(buf_in_pos, in_len))
        return EXIT_FAILURE;

    shmem_buffer *buf_out = buf_out_cache.get(buf_out_name);
    if (!buf_out)
        return EXIT_FAILURE;
    buf_out->update_file_size(buf_out_file_size);

    int r = EXIT_FAILURE;
    try {
        r = handler(buf_in->map_ptr(), buf_in_len, buf_out);
    }
    catch (...) {}

    bool ok = true;
    if (buf_in_pos + buf_in_len > REQUEST_CHUNK)
        ok &= buf_in->unmap();
    if (buf_out->map_end() - buf_out->map_begin() > (ptrdiff_t)RESPONSE_CHUNK)
        ok &= buf_out->unmap();
    if (!ok)
        return EXIT_FAILURE;

    printf("RESP %" PRIuPTR " of %" PRIuPTR " END\n",
            buf_out->data_size(), buf_out->file_size());
    fflush(stdout);
    return r;
}

namespace {

void termhdlr(int sig)
{
    set_interruption(sig == SIGINT ? IRQ_INT : IRQ_TERM);
}

} // namespace

int worker_loop(request_handler handler)
{
    static thread_local char ln[256];

    struct sigaction sigact;
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = termhdlr;
    sigemptyset(&sigact.sa_mask);
    int r = sigaction(SIGTERM, &sigact, nullptr);
    if (r == -1)
        log_syserr(LOG_ERR, "main sigaction(SIGTERM)");
    r = sigaction(SIGINT, &sigact, nullptr);
    if (r == -1)
        log_syserr(LOG_ERR, "main sigaction(SIGINT)");
    r = sigaction(SIGHUP, &sigact, nullptr);
    if (r == -1)
        log_syserr(LOG_ERR, "main sigaction(SIGHUP)");

    for (;;) {
        char *dst = ln;
        while (interruption_requested() != IRQ_TERM) {
            ssize_t r = read(STDIN_FILENO, dst, std::end(ln) - dst);
            if (r == -1) {
                if (errno == EINTR)
                    continue;
                else {
                    log_syserr(LOG_ERR, "main read(STDIN_FILENO)");
                    return EXIT_FAILURE;
                }
            }
            dst += r;
            char *eol = std::find(dst - r, dst, '\n');
            if (eol != dst) {
                *eol = 0;
                break;
            }
            if (dst >= std::end(ln)) {
                log(LOG_ERR, "Too large input line");
                return EXIT_FAILURE;
            }
        }
        if (interruption_requested() == IRQ_TERM)
            break;

        r = process_input(ln, handler);
        if (r != EXIT_SUCCESS)
            return r;
        if (interruption_requested() == IRQ_INT) {
            log(LOG_DEBUG, "Interrupted.");
            set_interruption(IRQ_NONE);
        }
    }
    log(LOG_NOTICE, "Terminated.");
    return EXIT_SUCCESS;
}

} // namespace pruv
