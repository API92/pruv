/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <pruv/shmem_buffer.hpp>

namespace pruv {

typedef int (*request_handler)(char *request, size_t request_len,
        shmem_buffer *response_buf);

int worker_loop(request_handler handler);

} // namespace pruv
