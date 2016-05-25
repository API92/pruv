/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <cstring>
#include <list>
#include <string>
#include <unordered_map>

#include <pruv/shmem_buffer.hpp>

namespace pruv {

class shmem_cache {
public:
    ~shmem_cache();
    shmem_buffer * get(const char *name) noexcept;

private:

    struct cstr_hash {
        typedef size_t       result_type;
        typedef const char * argument_type;

        size_t operator()(const char *s) const noexcept
        { return std::_Hash_impl::hash(s, strlen(s)); }
    };

    struct cstr_cmp {
        bool operator()(const char *lhs, const char *rhs) const noexcept
        {
            return !strcmp(lhs, rhs);
        }
    };

    std::unordered_map<const char *, shmem_buffer, cstr_hash, cstr_cmp>
        name_to_buf;
    std::list<std::string> name_holder;
};

} // namespace pruv
