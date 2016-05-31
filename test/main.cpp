/*
 * Copyright (C) Andrey Pikas
 */

#include <getopt.h>

#include <gtest/gtest.h>

#include <pruv/log.hpp>
#include <pruv/worker_loop.hpp>
#include "workers_reg.hpp"

int main(int argc, char **argv) {
    const char *worker = nullptr;
    static const option opts[] = {
        {"worker", required_argument, nullptr, 1},
        {0, 0, nullptr, 0}
    };
    opterr = 0;
    for (int c; (c = getopt_long(argc, argv, "", opts, nullptr)) != -1;)
        if (c == 1)
            worker = optarg;

    pruv::openlog(pruv::log_type::JOURNALD, -1);
    if (worker) {
        int r = pruv::worker_loop::setup();
        if (r)
            return r;
        std::unique_ptr<pruv::worker_loop> loop =
            pruv::workers_reg::instance().get(worker);
        if (loop)
            return loop->run();
        else
            return EXIT_FAILURE;
    }
    else {
        testing::InitGoogleTest(&argc, argv);
        return RUN_ALL_TESTS();
    }
}
