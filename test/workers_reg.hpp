/*
 * Copyright (C) Andrey Pikas
 */

#include <utility>
#include <vector>

#include <pruv/worker_loop.hpp>

namespace pruv {

class workers_reg {
public:
    static workers_reg & instance();

    void add(const char *name, request_handler handler);
    request_handler get(const char *name) const;

    struct registrator {
        registrator(const char *name, request_handler handler);
    };

private:
    workers_reg();
    workers_reg(const workers_reg &) = delete;
    workers_reg(workers_reg &&) = delete;
    void operator = (const workers_reg &) = delete;
    void operator = (workers_reg &&) = delete;

    std::vector<std::pair<const char *, request_handler>> hdlrs;
};

} // namespace pruv
