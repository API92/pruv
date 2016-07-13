/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/random.hpp>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <pruv/log.hpp>

namespace pruv {

bool random_bytes(void *dst, size_t len) noexcept
{
    int rnd;
    for (;;) {
        rnd = open("/dev/urandom", O_CLOEXEC);
        if (rnd == -1) {
            if (errno == EINTR)
                continue;
            pruv_log_syserr(LOG_ERR, "open /dev/urandom");
            return false;
        }
        break;
    }

    size_t readed = 0;
    while (readed < len) {
        ssize_t r = read(rnd, (char *)dst + readed, len - readed);
        if (r > 0)
            readed += r;
        if (r > 0 || (r == -1 && errno == EINTR))
            continue;
        if (r == 0)
            pruv_log(LOG_ERR, "read(/dev/urandom) readed 0 bytes");
        else
            pruv_log_syserr(LOG_ERR, "random_bytes read");
        break;
    }

    for (;;) {
        int r = close(rnd);
        if (r == -1 && EINTR)
            continue;
        if (r == -1)
            pruv_log_syserr(LOG_ERR, "random_bytes close(/dev/urandom)");
        break;
    }

    return len == readed;
}

} // namespace pruv
