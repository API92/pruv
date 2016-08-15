project(pruv)
cmake_minimum_required(VERSION 2.8)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules")

include_directories(include)

if (NOT Threads_FOUND)
    find_package(Threads REQUIRED)
endif()

if (NOT Systemd_FOUND)
    find_package(Systemd REQUIRED)
endif()
include_directories(${SYSTEMD_INCLUDE_DIRS})

if (NOT Libuv_FOUND)
    find_package(Libuv REQUIRED)
endif()
include_directories(${LIBUV_INCLUDE_DIR})

if (NOT HttpParser_FOUND)
    find_package(HttpParser REQUIRED)
endif()
include_directories(${HTTP_PARSER_INCLUDE_DIR})

if (NOT Gtest_FOUND)
    find_package(Gtest REQUIRED)
endif()
include_directories(${GTEST_INCLUDE_DIR})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -pedantic")

add_library(${PROJECT_NAME}
    include/pruv/dispatcher.hpp
    include/pruv/cleanup_helpers.hpp
    include/pruv/http_dispatcher.hpp
    include/pruv/http_pipelining_dispatcher.hpp
    include/pruv/list_node.hpp
    include/pruv/log.hpp
    include/pruv/process.hpp
    include/pruv/random.hpp
    include/pruv/shmem_buffer.hpp
    include/pruv/shmem_cache.hpp
    include/pruv/tcp_con.hpp
    include/pruv/tcp_server.hpp
    include/pruv/termination.hpp
    include/pruv/worker_loop.hpp
    src/dispatcher.cpp
    src/http_pipelining_dispatcher.cpp
    src/http_dispatcher.cpp
    src/log.cpp
    src/log_uv.cpp
    src/process.cpp
    src/random.cpp
    src/shmem_buffer.cpp
    src/shmem_cache.cpp
    src/tcp_con.cpp
    src/tcp_server.cpp
    src/termination.cpp
    src/worker_loop.cpp
)

target_compile_options(${PROJECT_NAME} PUBLIC -fno-exceptions -fno-rtti)

add_executable(${PROJECT_NAME}d
    daemon/main.cpp
)

add_executable(${PROJECT_NAME}_test
    test/common_dispatcher.hpp
    test/fixtures.cpp
    test/fixtures.hpp
    test/main.cpp
    test/send_recv_test.cpp
    test/pipelining_test.cpp
    test/workers_reg.cpp
    test/workers_reg.hpp
)

target_link_libraries(${PROJECT_NAME} ${HTTP_PARSER_LIBRARY})
target_link_libraries(${PROJECT_NAME} rt)
target_link_libraries(${PROJECT_NAME} ${SYSTEMD_LIBRARIES})
target_link_libraries(${PROJECT_NAME} ${LIBUV_LIBRARY})

target_link_libraries(${PROJECT_NAME}d ${HTTP_PARSER_LIBRARY})
target_link_libraries(${PROJECT_NAME}d ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME}d rt)
target_link_libraries(${PROJECT_NAME}d ${SYSTEMD_LIBRARIES})
target_link_libraries(${PROJECT_NAME}d ${LIBUV_LIBRARY})
add_dependencies(${PROJECT_NAME}d ${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME}_test ${GTEST_LIBRARIES})
target_link_libraries(${PROJECT_NAME}_test ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME}_test rt)
target_link_libraries(${PROJECT_NAME}_test ${SYSTEMD_LIBRARIES})
target_link_libraries(${PROJECT_NAME}_test ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(${PROJECT_NAME}_test ${LIBUV_LIBRARY})
add_dependencies(${PROJECT_NAME}_test ${PROJECT_NAME})

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_STANDARD_REQUIRED ON)

set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_STANDARD_REQUIRED ON)
