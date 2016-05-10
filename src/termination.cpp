/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/termination.hpp>

namespace pruv {

namespace {

volatile int interrupted = 0;

} // namespace

void set_interruption() noexcept
{
    interrupted = 1;
}

bool interruption_requested() noexcept
{
    return interrupted;
}

} // namespace pruv
