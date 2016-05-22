/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <functional>

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

    virtual tcp_context * create_connection() noexcept override;
    virtual void free_connection(tcp_context *con) noexcept override;

    std::function<tcp_context * ()> factory;
};

template<typename ProdT>
dispatcher::tcp_context * common_dispatcher<ProdT>::create_connection() noexcept
{
    return factory();
}

template<typename ProdT>
void common_dispatcher<ProdT>::free_connection(tcp_context *con) noexcept
{
    delete con;
}

} // namespace pruv
