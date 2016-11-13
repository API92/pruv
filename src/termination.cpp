/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/termination.hpp>

#include <atomic>
#include <cassert>

namespace pruv {

namespace {

static std::atomic<interruption_type> irq(IRQ_NONE);

} // namespace

void setup_interruption(interruption_type type) noexcept
{
    assert(type != IRQ_NONE);
    interruption_type old = irq.load();
    while (type >= old && !irq.compare_exchange_weak(old, type)) {}
}

void clear_interruption() noexcept
{
    interruption_type old = irq.load();
    if (old == IRQ_INT)
        irq.compare_exchange_strong(old, IRQ_NONE);
}

interruption_type interruption_requested() noexcept
{
    return irq.load();
}

} // namespace pruv
