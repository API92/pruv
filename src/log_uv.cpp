/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/log.hpp>

#include <uv.h>

namespace pruv {

void log_uv_err(int level, const char *msg, int error) noexcept
{
    log(level, "%s. Error: %s (%s).", msg, uv_strerror(error),
        uv_err_name(error));
}

void log_uv_err_with_location(int level, const char *msg, int error,
        const char *func, const char *file, int line) noexcept
{
    log_with_location(level,
            "CODE_FUNC=%s CODE_FILE=%s CODE_LINE=%d. %s. Error %s (%s).",
            "%s. Error: %s (%s).",
            func, file, line, msg, uv_strerror(error), uv_err_name(error));
}

} // namespace pruv
