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

size_t const shmem_buffer::PAGE_SIZE = sysconf(_SC_PAGE_SIZE);
size_t const shmem_buffer::PAGE_SIZE_MASK = PAGE_SIZE - 1;

shmem_buffer::shmem_buffer() noexcept
{
}

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
            pruv_log(LOG_ERR, "Can't generate random shmem name");
            return false;
        }

        size_t buflen = 50;
        char *new_name = (char *)malloc(buflen);
        if (!new_name) {
            pruv_log(LOG_EMERG, "Not enough memory for name");
            return false;
        }
        int r = snprintf(new_name, buflen, "/pruv-shm-%.16" PRIx64
                "%.16" PRIx64, rnd[0], rnd[1]);
        if (r < 0 || r >= (int)buflen) {
            pruv_log(LOG_ERR, "Error printing random name");
            free(new_name);
            return false;
        }

        name = name_ = new_name;
        oflag |= O_CREAT | O_EXCL;
        mode = S_IRUSR | S_IWUSR;
    }

    fd = shm_open(name, oflag, mode);
    if (fd == -1) {
        pruv_log_syserr(LOG_ERR, "shmem_buffer::open shm_open");
        return false;
    }
    pruv_log((name_ ? LOG_NOTICE : LOG_DEBUG), "Opened shared memory object %s,"
            " fd = %d", name, fd);

    file_size_ = 0;
    writable = for_write;
    return true;
}

bool shmem_buffer::resize(size_t new_size) noexcept
{
    new_size = (new_size + PAGE_SIZE_MASK) & ~PAGE_SIZE_MASK;
    int r;
    for (;;) {
        r = ftruncate(fd, new_size);
        if (!r) {
            file_size_ = new_size;
            return true;
        }
        if (errno == EINTR)
            continue;
        pruv_log_syserr(LOG_ERR, "ftruncate");
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

    pruv_log_syserr(LOG_ERR, "unmap");
    return false;
}

char * shmem_buffer::map_impl(size_t offset, size_t size) const noexcept
{
    int prot = PROT_READ;
    if (writable)
        prot |= PROT_WRITE;
    void *r = mmap(nullptr, size, prot, MAP_SHARED, fd, offset);
    if (r == MAP_FAILED) {
        pruv_log_syserr(LOG_ERR, "mmap");
        return nullptr;
    }
    else if (!r) {
        pruv_log(LOG_ERR, "mmap returns null pointer");
        // Allow zero page to leak.
        return nullptr;
    }
    return (char *)r;
}

bool shmem_buffer::map(size_t offset, size_t size) noexcept
{
    assert(!(offset & PAGE_SIZE_MASK));
    size = (size + PAGE_SIZE_MASK) & ~PAGE_SIZE_MASK;
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

bool shmem_buffer::seek(size_t pos, size_t segment_size) noexcept
{
    if (map_offset_ <= pos && pos < map_offset_ + (map_end_ - map_begin_)) {
        move_ptr((ptrdiff_t)pos - (ptrdiff_t)cur_pos());
        return true;
    }
    size_t base_pos = pos & ~PAGE_SIZE_MASK;
    if (base_pos + segment_size <= pos)
        segment_size += PAGE_SIZE;
    if (base_pos > file_size_)
        return false;
    if (base_pos == file_size_ && !resize(base_pos + segment_size))
        return false;
    if (base_pos + segment_size > file_size_)
        segment_size = file_size_ - base_pos;
    if (!map(base_pos, segment_size))
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
            pruv_log_syserr(LOG_ERR, "shm_unlink");
            res = false;
        }
        pruv_log(LOG_NOTICE, "Unlinked shared memory object %s, fd = %d",
                 name_, fd);
        free((void *)name_);
        name_ = nullptr;
    }

    if (fd != -1) {
        if (::close(fd) == -1) {
            pruv_log_syserr(LOG_ERR, "close");
            res = false;
        }
        pruv_log(LOG_DEBUG, "Closed shared memory object, fd = %d", fd);
    }
    fd = -1;
    file_size_ = 0;
    return res;
}

} // namespace pruv
