/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <pruv/hash_table.hpp>
#include <pruv/shmem_buffer.hpp>

namespace pruv {

class shmem_cache {
public:
    shmem_cache(bool for_write);
    ~shmem_cache();
    shmem_buffer * get(char const *name) noexcept;

private:
    hash_table _name_to_buf;
    bool _for_write;
};

} // namespace pruv
