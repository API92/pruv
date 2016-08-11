/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/log.hpp>
#include <pruv/shmem_cache.hpp>

namespace pruv {

shmem_cache::~shmem_cache()
{
    for (auto &name_buf : name_to_buf)
        if (name_buf.second.opened())
            name_buf.second.close();
    name_to_buf.clear();
    name_holder.clear();
}

shmem_buffer * shmem_cache::get(const char *name) noexcept
{
    auto it = name_to_buf.find(name);
    if (it != name_to_buf.end() && it->second.opened())
        return &it->second;
    shmem_buffer *buf = nullptr;
    if (it == name_to_buf.end()) {
        name_holder.emplace_back(name);
        buf = &name_to_buf[name_holder.back().data()];
    }
    else
        buf = &it->second;
    if (!buf->open(name, true))
        return nullptr;
    buf->set_data_size(0);
    return buf;
}

} // namespace pruv
