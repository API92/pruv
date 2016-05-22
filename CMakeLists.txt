project(pruv)
cmake_minimum_required(VERSION 2.8)

include_directories(include)

if (NOT Threads_FOUND)
    find_package(Threads REQUIRED)
endif()

if (NOT Systemd_FOUND)
    set(Systemd_DIR .)
    find_package(Systemd REQUIRED)
    include_directories(${SYSTEMD_INCLUDE_DIRS})
    link_directories(${SYSTEMD_LIBRARY_DIRS})
endif()

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
    src/termination.cpp
    src/worker_loop.cpp
)

add_executable(${PROJECT_NAME}d
    daemon/main.cpp
)

add_executable(${PROJECT_NAME}_test
    test/fixtures.cpp
    test/fixtures.hpp
    test/main.cpp
    test/send_recv_test.cpp
    test/common_dispatcher.hpp
    test/workers_reg.cpp
    test/workers_reg.hpp
)

target_link_libraries(${PROJECT_NAME} http_parser)
target_link_libraries(${PROJECT_NAME} rt)
target_link_libraries(${PROJECT_NAME} ${SYSTEMD_LIBRARIES})
target_link_libraries(${PROJECT_NAME} uv)

target_link_libraries(${PROJECT_NAME}d http_parser)
target_link_libraries(${PROJECT_NAME}d ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME}d rt)
target_link_libraries(${PROJECT_NAME}d ${SYSTEMD_LIBRARIES})
target_link_libraries(${PROJECT_NAME}d uv)
add_dependencies(${PROJECT_NAME}d ${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME}_test gtest)
target_link_libraries(${PROJECT_NAME}_test ${PROJECT_NAME})
target_link_libraries(${PROJECT_NAME}_test rt)
target_link_libraries(${PROJECT_NAME}_test ${SYSTEMD_LIBRARIES})
target_link_libraries(${PROJECT_NAME}_test ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(${PROJECT_NAME}_test uv)
add_dependencies(${PROJECT_NAME}_test ${PROJECT_NAME})

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_EXTENSIONS OFF)

set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${PROJECT_NAME}_test PROPERTY CXX_STANDARD_REQUIRED ON)
