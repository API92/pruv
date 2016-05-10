/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/log.hpp>

#include <uv.h>

namespace pruv {

void log_uv_error(int level, const char *msg, int error) noexcept
{
    log(level, "%s. Error %s (%s).", msg, uv_strerror(error),
        uv_err_name(error));
}

} // namespace pruv
