/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <pruv/hash_table.hpp>
#include <pruv/shmem_buffer.hpp>

namespace pruv {

class shmem_cache {
public:
    ~shmem_cache();
    shmem_buffer * get(char const *name) noexcept;

private:
    hash_table name_to_buf;
};

} // namespace pruv
