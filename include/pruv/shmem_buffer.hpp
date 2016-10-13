/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <cassert>
#include <cstddef>

namespace pruv {

class shmem_buffer {
public:
    ~shmem_buffer();
    /// Open existing (if name not null) or create new (if name is null)
    /// shared memory object.
    bool open(const char *name, bool for_write) noexcept;
    /// Resize shared memory object. new_size automatically aligned.
    bool resize(size_t new_size) noexcept;
    /// Store new_file_size as a file size of this object.
    /// Usefull if shared memory object size was changed elsewhere without
    /// this object.
    void update_file_size(size_t new_file_size) noexcept;
    bool unmap() noexcept;
    /// Unmaps previous mapped region and maps new.
    /// offset must be aligned to page size.
    /// size automatically aligned.
    bool map(size_t offset, size_t size) noexcept;
    /// As if calls resize(default_size) and map(0, default_size).
    bool reset_defaults(size_t default_size) noexcept;
    /// Maps region which contains pos.
    bool seek(size_t pos, size_t segment_size) noexcept;
    /// Unmaps memory, closes file descriptor and (if was created in open)
    /// removes shared memory object from system.
    bool close() noexcept;

    const char * map_begin() const { return map_begin_; }
    char * map_ptr() const { return map_ptr_; }
    const char * map_end() const { return map_end_; }
    size_t map_offset() const { return map_offset_; }
    size_t file_size() const { return file_size_; }
    size_t data_size() const { return data_size_; }
    const char * name() const { return name_; }
    size_t cur_pos() const noexcept
    {
        return map_offset_ + (map_ptr_ - map_begin_);
    }
    bool opened() const { return fd != -1; }

    void move_ptr(ptrdiff_t dif)
    {
        assert(map_begin_ <= map_ptr_ + dif && map_ptr_ + dif <= map_end_);
        map_ptr_ += dif;
    }
    void set_data_size(size_t value) { data_size_ = value; }

    static size_t const PAGE_SIZE;
    static size_t const PAGE_SIZE_MASK;

private:
    char * map_impl(size_t offset, size_t size) const noexcept;

    char *map_begin_ = nullptr;
    char *map_ptr_ = nullptr;
    char *map_end_ = nullptr;
    size_t map_offset_ = 0;
    size_t file_size_ = 0;
    size_t data_size_ = 0;
    const char *name_ = nullptr;
    int fd = -1;
    bool writable = false;
};

constexpr size_t REQUEST_CHUNK = 64 * 1024;
constexpr size_t RESPONSE_CHUNK = 128 * 1024;

} // namespace pruv
