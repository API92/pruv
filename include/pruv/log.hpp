/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <syslog.h>

namespace pruv {

enum class log_type {
    STDERR,
    SYSLOG,
    JOURNALD
};

void openlog(log_type type, int max_level) noexcept;

void closelog() noexcept;

void log(int level, const char *format, ...) noexcept;

void log_uv_error(int level, const char *msg, int error) noexcept;

void log_syserr(int level, const char *msg) noexcept;

} // namespace pruv
