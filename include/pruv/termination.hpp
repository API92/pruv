/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

namespace pruv {

void set_interruption() noexcept;

bool interruption_requested() noexcept;

} // namespace pruv
