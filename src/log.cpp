/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/log.hpp>

#include <assert.h>
#include <cstdarg>
#include <cstdio>
#include <memory>

#include <errno.h>
#include <syslog.h>
#include <systemd/sd-journal.h>
#include <unistd.h>

namespace pruv {

namespace {

class logger {
public:
    logger() : max_level(0) {}
    virtual ~logger()
    {
        if (logger_impl == this)
            logger_impl = nullptr;
    }
    virtual void open() const {}
    virtual void close() const {}
    void set_max_level(int level) { max_level = level; }
    void log(int level, const char *format, va_list va) const;

    static logger *logger_impl;

protected:
    virtual void write_log(int level, const char *frmt, va_list va) const = 0;

private:
    int max_level;
};

logger * logger::logger_impl = nullptr;


void logger::log(int level, const char *format, va_list va) const
{
    if (level <= max_level)
        write_log(level, format, va);
}

class stderr_logger : public logger {
public:
    static void write_log_static(int level, const char *fmt, va_list v)
    {
        fprintf(stderr, "<%d> ", level);
        vfprintf(stderr, fmt, v);
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    virtual void write_log(int level, const char *fmt, va_list v) const override
    {
        write_log_static(level, fmt, v);
    }

    static stderr_logger & instance()
    {
        static stderr_logger inst;
        return inst;
    }
};

class syslog_logger : public logger {
public:
    syslog_logger()
    {
        ::openlog("dfsd", LOG_PID, 0);
    }

    virtual void write_log(int level, const char *fmt, va_list v) const override
    {
        ::vsyslog(level, fmt, v);
    }

    virtual void close() const override
    {
        ::closelog();
    }

    ~syslog_logger()
    {
        close();
        usleep(1000);
    }

    static syslog_logger & instance()
    {
        static syslog_logger inst;
        return inst;
    }
};

class journald_logger : public logger {
public:
    virtual void write_log(int level, const char *fmt, va_list v) const override
    {
        sd_journal_printv(level, fmt, v);
    }

    ~journald_logger()
    {
        usleep(1000);
    }

    static journald_logger & instance()
    {
        static journald_logger inst;
        return inst;
    }
};

} // namespace

void openlog(log_type type, int max_level) noexcept
{
    if (type == log_type::STDERR)
        logger::logger_impl = &stderr_logger::instance();
    else if (type == log_type::SYSLOG)
        logger::logger_impl = &syslog_logger::instance();
    else if (type == log_type::JOURNALD)
        logger::logger_impl = &journald_logger::instance();
    assert(logger::logger_impl);
    logger::logger_impl->set_max_level(max_level);
}

void closelog() noexcept
{
    if (!logger::logger_impl)
        return;
    logger::logger_impl->close();
    logger::logger_impl = nullptr;
}

void log(int level, const char *format, ...) noexcept
{
    va_list va;
    va_start(va, format);
    if (logger::logger_impl)
        logger::logger_impl->log(level, format, va);
    else
        stderr_logger::write_log_static(level, format, va);
}

void log_syserr(int level, const char *msg) noexcept
{
    log(level, "%s. Error: %s.", msg, strerror(errno));
}

} // namespace pruv
