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
    for (int c; (c = getopt_long(argc, argv, "", opts, nullptr)) != -1;)
        if (c == 1)
            worker = optarg;

    pruv::openlog(pruv::log_type::STDERR, -1);
    if (worker) {
        pruv::request_handler hdlr = pruv::workers_reg::instance().get(worker);
        if (hdlr)
            return pruv::worker_loop(hdlr);
        else
            return EXIT_FAILURE;
    }
    else {
        testing::InitGoogleTest(&argc, argv);
        return RUN_ALL_TESTS();
    }
}
