/*
 * Copyright (C) Andrey Pikas
 */

#include <memory>
#include <utility>
#include <vector>

#include <pruv/worker_loop.hpp>

namespace pruv {

class workers_reg {
public:
    static workers_reg & instance();

    void add(const char *name, std::function<worker_loop * ()> factory);
    std::unique_ptr<worker_loop> get(const char *name) const;

    template<typename T>
    struct registrator {
        template<typename ... TArg>
        registrator(const char *name, TArg ... args)
        {
            workers_reg::instance().add(name,
                    [args...]{ return new T(args...); });
        }
    };

private:
    workers_reg();
    workers_reg(const workers_reg &) = delete;
    workers_reg(workers_reg &&) = delete;
    void operator = (const workers_reg &) = delete;
    void operator = (workers_reg &&) = delete;

    std::vector<std::pair<const char *, std::function<worker_loop * ()>>> f;
};

} // namespace pruv
