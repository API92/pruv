/*
 * Copyright (C) Andrey Pikas
 */

#include <pruv/termination.hpp>

namespace pruv {

namespace {

volatile interruption_type irq = IRQ_NONE;

} // namespace

void set_interruption(interruption_type type) noexcept
{
    if (type == IRQ_NONE || type > irq)
        irq = type;
}

interruption_type interruption_requested() noexcept
{
    return irq;
}

} // namespace pruv
