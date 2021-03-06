/*
 * Copyright (C) Andrey Pikas
 */

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <errno.h>
#include <getopt.h>
#include <unistd.h>

#include <uv.h>

#include <pruv/http_pipelining_dispatcher.hpp>
#include <pruv/http_worker.hpp>
#include <pruv/log.hpp>
#include <pruv/termination.hpp>

std::unique_ptr<pruv::dispatcher> dispatcher;

void stop_handler(uv_signal_t * /*handle*/, int signum)
{
    pruv_log(LOG_NOTICE, "Received signal %d", signum);
    if (dispatcher)
        dispatcher->stop();
}

int parse_int_arg(const char *s, const char *optname)
{
    char *endptr = nullptr;
    unsigned long res = strtol(s, &endptr, 0);
    if ((res == ULONG_MAX && errno == ERANGE) || res > INT_MAX) {
        pruv_log(LOG_EMERG, "Options \"%s\" value \"%s\" is out of range",
                optname, s);
        exit(EXIT_FAILURE);
    }

    if (*endptr) {
        pruv_log(LOG_EMERG, "Options \"%s\" has non unsigned integer value",
                optname);
        exit(EXIT_FAILURE);
    }

    return (int)res;
}

int main(int argc, char * const *argv)
{
    uv_disable_stdio_inheritance();

    int daemon_or_worker = 0;
    int disable_timeouts = 0;
    int log_level = LOG_INFO;
    int log_locations = 1;
    const char *listen_addr = "::";
    int listen_port = 8000;
    int workers_num = 1;
    const char *worker_exe = argv[0];
    std::vector<const char *> worker_args;
    const option opts[] = {
        {"daemon", no_argument, &daemon_or_worker, 1},
        {"worker", no_argument, &daemon_or_worker, 2},
        {"notimeouts", no_argument, &disable_timeouts, 1},
        {"loglevel", required_argument, &log_level, LOG_INFO},
        {"nologlocations", no_argument, &log_locations, 0},
        {"listen-addr", required_argument, nullptr, 1},
        {"listen-port", required_argument, &listen_port, 8000},
        {"workers-num", required_argument, &workers_num, 1},
        {"worker-executable", required_argument, nullptr, 2},
        {"worker-arg", required_argument, nullptr, 3},
        {0, 0, nullptr, 0}
    };

    for (;;) {
        int opt_idx = 0;
        int c = getopt_long(argc, argv, "", opts, &opt_idx);
        if (c == -1)
            break;
        if (c == 0) {
            if (optarg)
                *opts[opt_idx].flag = parse_int_arg(optarg, opts[opt_idx].name);
        }
        else if (c == 1)
            listen_addr = optarg;
        else if (c == 2)
            worker_exe = optarg;
        else if (c == 3)
            worker_args.push_back(optarg);
        else if (c != '?') {
            pruv_log(LOG_EMERG, "Unknown option");
            exit(EXIT_FAILURE);
        }
    }
    pruv::openlog(
        (daemon_or_worker ? pruv::log_type::JOURNALD : pruv::log_type::STDERR),
        log_level);
    pruv::log_setup_locations(log_locations);

    if (daemon_or_worker == 2) {
        int r = pruv::worker_loop::setup(argc, argv);
        if (r)
            return r;
        std::unique_ptr<pruv::http_worker> loop(
                new (std::nothrow) pruv::http_worker);
        if (!loop) {
            pruv_log(LOG_EMERG, "No memory for worker loop object.");
            return EXIT_FAILURE;
        }
        return loop->run();
    }

    if (worker_exe == argv[0]) {
        worker_args.push_back("--worker");
        worker_args.push_back("--loglevel");
        static const char levels[] = "0\0001\0002\0003\0004\0005\0006\0007\000";
        worker_args.push_back(levels + 2 * std::min(7, std::max(0, log_level)));
    }
    worker_args.insert(worker_args.begin(), worker_exe);
    worker_args.push_back(nullptr);

    if (daemon_or_worker == 1) {
        pruv_log(LOG_NOTICE, "Begin daemon initialization.");

        pid_t pid = fork();
        if (pid < 0) {
            pruv_log(LOG_EMERG, "fork failed at start. errno = %d.", errno);
            return EXIT_FAILURE;
        }
        if (pid > 0)
            return EXIT_SUCCESS;
        umask(0);
        pid_t sid = setsid();
        if (sid < 0)
            pruv_log(LOG_ERR, "setsid failed at start. errno = %d.", errno);

        pruv_log(LOG_NOTICE, "Daemon started.");
    }

    int r;
    uv_loop_t loop;
    if ((r = uv_loop_init(&loop)) < 0) {
        pruv::log_uv_err(LOG_EMERG, "uv_loop_init main loop", r);
        return EXIT_FAILURE;
    }

    uv_signal_t sig[3];
    int signum[3] = {SIGTERM, SIGINT, SIGHUP};
    for (size_t i = 0; i < sizeof(sig) / sizeof(*sig); ++i)
        if ((r = uv_signal_init(&loop, &sig[i])) < 0) {
            pruv::log_uv_err(LOG_ERR, "uv_signal_init SIGTERM handler", r);
            uv_close((uv_handle_t *)&sig[i], nullptr);
        }
        else if ((r = uv_signal_start(&sig[i], stop_handler, signum[i])) < 0) {
            pruv::log_uv_err(LOG_ERR, "uv_signal_start SIGTERM handler", r);
            uv_close((uv_handle_t *)&sig[i], nullptr);
        }
        else
            uv_unref((uv_handle_t *)&sig[i]);

    dispatcher.reset(new pruv::http_pipelining_dispatcher);
    if (disable_timeouts)
        dispatcher->set_timeouts(!disable_timeouts);
    dispatcher->start(&loop, listen_addr, listen_port,
            workers_num, worker_exe, worker_args.data());

    uv_run(&loop, UV_RUN_DEFAULT);

    dispatcher->on_loop_exit();

    for (uv_signal_t & sigi : sig) {
        if ((r = uv_signal_stop(&sigi)) < 0)
            pruv::log_uv_err(LOG_ERR, "uv_signal_stop in SIGTERM handler", r);
        uv_close((uv_handle_t *)&sigi, nullptr);
    }
    // Do one loop iteration to remove sigterm handler from loop.
    uv_run(&loop, UV_RUN_NOWAIT);

    if ((r = uv_loop_close(&loop)) < 0)
        pruv::log_uv_err(LOG_ERR, "uv_loop_close main loop", r);

    if (daemon_or_worker == 1)
        pruv_log(LOG_NOTICE, "Daemon stopped.");

    return EXIT_SUCCESS;
}
