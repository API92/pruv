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

void log_setup_locations(int enable) noexcept;
int log_locations_enabled() noexcept;

void closelog() noexcept;

void log(int level, const char *format, ...) noexcept;
void log_with_location(int level, const char *loc_format,
        const char *src_format, ...) noexcept;

void log_uv_err(int level, const char *msg, int error) noexcept;
void log_uv_err_with_location(int level, const char *msg, int error,
        const char *func, const char *file, int line) noexcept;

void log_syserr(int level, const char *msg) noexcept;
void log_syserr_with_location(int level, const char *msg,
        const char *func, const char *file, int line) noexcept;

} // namespace pruv

#define pruv_log1(level, format, ...) \
    ::pruv::log_with_location(level, \
            "CODE_FUNC=%s CODE_FILE=%s CODE_LINE=%d " format, format, \
            __PRETTY_FUNCTION__, __FILE__, int(__LINE__), ##__VA_ARGS__)

#define pruv_log(...) pruv_log1(__VA_ARGS__, "")

#define pruv_log_uv_err(level, msg, error) \
    ::pruv::log_uv_err_with_location(level, msg, error, \
            __PRETTY_FUNCTION__, __FILE__, __LINE__)

#define pruv_log_syserr(level, msg) \
    ::pruv::log_syserr_with_location(level, msg, \
            __PRETTY_FUNCTION__, __FILE__, __LINE__)
