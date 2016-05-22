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

void workers_reg::add(const char *name, request_handler hdlr)
{
    hdlrs.emplace_back(name, hdlr);
}

request_handler workers_reg::get(const char *name) const
{
    for (const std::pair<const char *, request_handler> &it : hdlrs)
        if (!strcmp(it.first, name))
            return it.second;
    return nullptr;
}

workers_reg::registrator::registrator(const char *name, request_handler hdlr)
{
    workers_reg::instance().add(name, hdlr);
}

} // namespace pruv
