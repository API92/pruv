/*
 * Copyright (C) Andrey Pikas
 */

#pragma once

#include <gtest/gtest.h>
#include <uv.h>

namespace pruv {

::testing::AssertionResult uv_ok(ptrdiff_t r);

struct loop_fixture : ::testing::Test {
    virtual void SetUp() override;
    virtual void TearDown() override;

    uv_loop_t loop;
};

} // namespace pruv
