/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/shmem_buffer.hpp>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pruv/log.hpp>
#include <pruv/random.hpp>

namespace pruv {

namespace {

const long PAGESIZE = sysconf(_SC_PAGE_SIZE);
const size_t PAGESIZE_MASK = PAGESIZE - 1;

} // namespace

shmem_buffer::~shmem_buffer()
{
    if (name_)
        free((void *)name_);
}

bool shmem_buffer::open(const char *name, bool for_write) noexcept
{
    mode_t mode = 0;
    int oflag = O_RDONLY;
    if (for_write)
        oflag |= O_RDWR;
    if (!name) {
        uint64_t rnd[2];
        if (!random_bytes(&rnd, sizeof(rnd))) {
            log(LOG_ERR, "Can't generate random shmem name");
            return false;
        }

        size_t buflen = 50;
        char *new_name = (char *)malloc(buflen);
        if (!new_name) {
            log(LOG_ERR, "shmem_buffer::open not enough memory for name");
            return false;
        }
        int r = snprintf(new_name, buflen, "/pruv-shm-%0.16" PRIx64
                "%0.16" PRIx64, rnd[0], rnd[1]);
        if (r < 0 || r >= (int)buflen) {
            log(LOG_ERR, "shmem_buffer::open error printing random name");
            free(new_name);
            return false;
        }

        name = name_ = new_name;
        oflag |= O_CREAT | O_EXCL;
        mode = S_IRUSR | S_IWUSR;
    }

    fd = shm_open(name, oflag, mode);
    if (fd == -1) {
        log_syserr(LOG_ERR, "shmem_buffer::open shm_open");
        return false;
    }
    log((name_ ? LOG_NOTICE : LOG_DEBUG), "Opened shared memory object %s, "
            "fd = %d", name, fd);

    file_size_ = 0;
    writable = for_write;
    return true;
}

bool shmem_buffer::resize(size_t new_size) noexcept
{
    new_size = (new_size + PAGESIZE_MASK) & ~PAGESIZE_MASK;
    int r;
    for (;;) {
        r = ftruncate(fd, new_size);
        if (!r) {
            file_size_ = new_size;
            return true;
        }
        if (errno == EINTR)
            continue;
        log_syserr(LOG_ERR, "shmem_buffer::resize ftruncate");
        return false;
    }
}

void shmem_buffer::update_file_size(size_t new_file_size) noexcept
{
    assert(fd != -1);
#ifndef NDEBUG
    struct stat prop;
    int r = fstat(fd, &prop);
    assert(!r);
    assert(prop.st_size == (ptrdiff_t)new_file_size);
#endif
    file_size_ = new_file_size;
}

bool shmem_buffer::unmap() noexcept
{
    if (!map_begin_)
        return true;
    if (!munmap(map_begin_, map_end_ - map_begin_)) {
        map_ptr_ = map_end_ = map_begin_ = nullptr;
        map_offset_ = 0;
        return true;
    }

    log_syserr(LOG_ERR, "shmem_buffer::unmap unmap");
    return false;
}

char * shmem_buffer::map_impl(size_t offset, size_t size) const noexcept
{
    int prot = PROT_READ;
    if (writable)
        prot |= PROT_WRITE;
    void *r = mmap(nullptr, size, prot, MAP_SHARED, fd, offset);
    if (r == MAP_FAILED) {
        log_syserr(LOG_ERR, "shmem_buffer::map_impl mmap");
        return nullptr;
    }
    else if (!r) {
        log(LOG_ERR, "shmem_buffer::map_impl mmap returns null pointer");
        // Allow zero page to leak.
        return nullptr;
    }
    return (char *)r;
}

bool shmem_buffer::map(size_t offset, size_t size) noexcept
{
    assert(!(offset & PAGESIZE_MASK));
    size = (size + PAGESIZE_MASK) & ~PAGESIZE_MASK;
    if (map_offset_ == offset && (ptrdiff_t)size == map_end_ - map_begin_) {
        map_ptr_ = map_begin_;
        return true;
    }

    if (!unmap())
        return false;

    char *r = map_impl(offset, size);
    if (!r)
        return false;

    map_ptr_ = map_begin_ = r;
    map_end_ = map_begin_ + size;
    map_offset_ = offset;
    return true;
}

bool shmem_buffer::restart(const char *l, const char *r) noexcept
{
    assert(l <= r);
    if (l == r)
        return unmap();
    assert(map_end_ <= l || r <= map_begin_ ||
           (map_begin_ <= l && r <= map_end_));
    if (!map_offset_ && r - l <= map_end_ - map_begin_) {
        memmove(map_begin_, l, r - l);
        map_ptr_ = map_begin_;
        return true;
    }
    size_t size = (r - l + PAGESIZE_MASK) & ~PAGESIZE_MASK;
    char *dst = map_impl(0, size);
    if (!dst)
        return false;
    // Source and destination can't overlap virtually (because of
    // new mmap'ed region for destination).
    // But check if they overlap phisically. If so memcpy is not allowed.
    // memmove not allowed too because it takes into account only virtual
    // overlapping and can select wrong direction.
    if (map_begin_ <= l && r <= map_end_ &&
        r - l > (ptrdiff_t)map_offset_ + (l - map_begin_)) {
        char *p = dst;
        while (l != r)
            *p++ = *l++;
    }
    else
        memcpy(dst, l, r - l);

    bool res = unmap();
    map_ptr_ = map_begin_ = dst;
    map_end_ = map_begin_ + size;
    return res;
}

bool shmem_buffer::seek(size_t pos) noexcept
{
    size_t len = map_end_ - map_begin_;
    if (map_offset_ <= pos && pos <= map_offset_ + len) {
        move_ptr((ptrdiff_t)pos - (ptrdiff_t)cur_pos());
        return true;
    }
    size_t base_pos = pos & ~PAGESIZE_MASK;
    if (base_pos + len <= pos)
        len += PAGESIZE;
    if (!map(base_pos, len))
        return false;
    move_ptr(pos - base_pos);
    return true;
}

bool shmem_buffer::reset_defaults(size_t default_size) noexcept
{
    if (file_size_ != default_size && !resize(default_size))
        return false;
    return map(0, default_size);
}

bool shmem_buffer::close() noexcept
{
    bool res = true;
    res &= unmap();
    if (name_) {
        if (shm_unlink(name_) == -1) {
            log_syserr(LOG_ERR, "shmem_buffer::close shm_unlink");
            res = false;
        }
        log(LOG_NOTICE, "Unlinked shared memory object %s, fd = %d", name_, fd);
        free((void *)name_);
        name_ = nullptr;
    }

    if (::close(fd) == -1) {
        log_syserr(LOG_ERR, "shmem_buffer::close close");
        res = false;
    }
    log(LOG_DEBUG, "Closed shared memory object, fd = %d", fd);
    fd = -1;
    file_size_ = 0;
    return res;
}

} // namespace pruv
