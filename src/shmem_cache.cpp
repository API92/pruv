/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/shmem_cache.hpp>

#include <cstring> // strcmp, strlen
#include <functional> // hash

#include <pruv/log.hpp>

namespace pruv {

namespace {

struct entry : hash_table::entry {
    shmem_buffer shm;
    char name[50];

    bool equal(char const *rhs) const noexcept
    {
        return !strcmp(name, rhs);
    }
};

size_t cstr_hash(char const *s, size_t len) noexcept
{
    return std::_Hash_impl::hash(s, len);
}

} // namepspace


shmem_cache::~shmem_cache()
{
    name_to_buf.clear([](hash_table::entry *base_e) {
        entry *e = static_cast<entry *>(base_e);
        if (e->shm.opened())
            e->shm.close();
        delete e;
    });
}

shmem_buffer * shmem_cache::get(char const *name) noexcept
{
    size_t name_len = strlen(name);
    hash_table::iterator it = name_to_buf.find(cstr_hash(name, name_len),
        [name](hash_table::entry const *e) {
            return static_cast<entry const *>(e)->equal(name);
        });

    if (it && it.get<entry>()->shm.opened())
        return &it.get<entry>()->shm;

    if (!it) {
        if (name_len >= sizeof(entry::name) / sizeof(*entry::name)) {
            pruv_log(LOG_ERR, "Name is too long.");
            return nullptr;
        }

        entry *e = new (std::nothrow) entry;
        if (!e) {
            pruv_log(LOG_EMERG, "Can't allocate memory for cache entry.");
            return nullptr;
        }
        e->hash = cstr_hash(name, name_len);
        memcpy(e->name, name, name_len + 1);

        it = name_to_buf.insert(e);
        if (!it) {
            pruv_log(LOG_EMERG, "Can't insert cache entry.");
            return nullptr;
        }
    }

    entry *e = it.get<entry>();
    if (!e->shm.open(e->name, true))
        return nullptr;
    e->shm.set_data_size(0);
    return &e->shm;
}

} // namespace pruv
