/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

namespace pruv {

enum interruption_type {
    IRQ_NONE = 0, /// interruption not requested
    IRQ_INT =  1,  /// interrupt current request
    IRQ_TERM = 2  /// terminate process
};

void set_interruption(interruption_type type) noexcept;

interruption_type interruption_requested() noexcept;

} // namespace pruv
