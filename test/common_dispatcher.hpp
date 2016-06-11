/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <functional>
#include <set>

#include <pruv/dispatcher.hpp>

namespace pruv {

template<typename ProdT>
struct common_dispatcher : dispatcher {
    using dispatcher::tcp_context;

    template<typename ... ArgT>
    common_dispatcher(ArgT ... args)
        : factory([args...]{ return new ProdT(args...); })
    {
    }

    template<typename ... ArgT>
    void set_factory_args(ArgT ... args)
    {
        factory = [args...]{ return new ProdT(args...); };
    }

    virtual tcp_context * create_connection() noexcept override;
    virtual void free_connection(tcp_context *con) noexcept override;

    std::function<ProdT * ()> factory;

    std::set<ProdT *> products;
};

template<typename ProdT>
dispatcher::tcp_context * common_dispatcher<ProdT>::create_connection() noexcept
{
    ProdT *result = factory ? factory() : nullptr;
    if (result)
        products.insert(result);
    return result;
}

template<typename ProdT>
void common_dispatcher<ProdT>::free_connection(tcp_context *con) noexcept
{
    products.erase(dynamic_cast<ProdT *>(con));
    delete con;
}

} // namespace pruv
