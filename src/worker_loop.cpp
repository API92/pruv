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
    static thread_local size_t buf_in_len;
    static thread_local char buf_out_name[256];
    static thread_local size_t buf_out_file_size;

    int parsed = sscanf(s,
            "HTTP IN SHM %255s %" SCNuPTR " OUT SHM %255s %" SCNuPTR,
            buf_in_name, &buf_in_len, buf_out_name, &buf_out_file_size);
    if (parsed != 4) {
        pruv::log(LOG_ERR, "Error parsing \"%s\"", s);
        return EXIT_FAILURE;
    }

    static shmem_cache buf_in_cache;
    static shmem_cache buf_out_cache;

    shmem_buffer *buf_in = buf_in_cache.get(buf_in_name);
    if (!buf_in)
        return EXIT_FAILURE;
    assert(!buf_in->map_offset());
    if (buf_in->map_end() - buf_in->map_begin() < (ptrdiff_t)buf_in_len &&
        !buf_in->map(0, buf_in_len))
        return EXIT_FAILURE;
    buf_in->move_ptr(buf_in->map_begin() - buf_in->map_ptr());

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
    if (buf_in_len > REQUEST_CHUNK)
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

void termhdlr(int)
{
    pruv::set_interruption();
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
        pruv::log_syserr(LOG_ERR, "main sigaction(SIGTERM)");
    r = sigaction(SIGINT, &sigact, nullptr);
    if (r == -1)
        pruv::log_syserr(LOG_ERR, "main sigaction(SIGINT)");
    r = sigaction(SIGHUP, &sigact, nullptr);
    if (r == -1)
        pruv::log_syserr(LOG_ERR, "main sigaction(SIGHUP)");

    for (;;) {
        char *dst = ln;
        while (!pruv::interruption_requested()) {
            ssize_t r = read(STDIN_FILENO, dst, std::end(ln) - dst);
            if (r == -1) {
                if (errno == EINTR)
                    continue;
                else {
                    pruv::log_syserr(LOG_ERR, "main read(STDIN_FILENO)");
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
                pruv::log(LOG_ERR, "Too large input line");
                return EXIT_FAILURE;
            }
        }
        if (pruv::interruption_requested())
            break;

        r = process_input(ln, handler);
        if (r != EXIT_SUCCESS)
            return r;
    }
    pruv::log(LOG_NOTICE, "Terminated.");
    return EXIT_SUCCESS;
}

} // namespace pruv
