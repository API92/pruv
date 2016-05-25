/*
 * Copyright (C) Andrey Pikas
 */

#include "workers_reg.hpp"

#include <cstring>

namespace pruv {

workers_reg::workers_reg()
{
}

workers_reg & workers_reg::instance()
{
    static workers_reg inst;
    return inst;
}

void workers_reg::add(const char *name, std::function<worker_loop * ()> factory)
{
    f.emplace_back(name, std::move(factory));
}

std::unique_ptr<worker_loop> workers_reg::get(const char *name) const
{
    for (const auto &it : f)
        if (!strcmp(it.first, name) && it.second)
            return std::unique_ptr<worker_loop>(it.second());
    return std::unique_ptr<worker_loop>();
}

} // namespace pruv
