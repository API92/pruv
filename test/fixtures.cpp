/*
 * Copyright (C) Andrey Pikas
 */

#include "fixtures.hpp"

namespace pruv {

::testing::AssertionResult uv_ok(ptrdiff_t r)
{
    if (r < 0)
        return ::testing::AssertionFailure() << uv_strerror(r) << " " <<
            uv_err_name(r);
    else
        return ::testing::AssertionSuccess();
}

void loop_fixture::SetUp()
{
    ASSERT_TRUE(uv_ok(uv_loop_init(&loop)));
}

void loop_fixture::TearDown()
{
    EXPECT_TRUE(uv_ok(uv_loop_close(&loop)));
}

} // namespace pruv
