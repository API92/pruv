project(pruv)
cmake_minimum_required(VERSION 2.8)

set(Systemd_DIR .)
find_package(Systemd REQUIRED)
include_directories(${SYSTEMD_INCLUDE_DIRS})
include_directories(include)
link_directories(${SYSTEMD_LIBRARY_DIRS})

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

target_link_libraries(${PROJECT_NAME}d http_parser)
target_link_libraries(${PROJECT_NAME}d pruv)
target_link_libraries(${PROJECT_NAME}d rt)
target_link_libraries(${PROJECT_NAME}d ${SYSTEMD_LIBRARIES})
target_link_libraries(${PROJECT_NAME}d uv)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME}d PROPERTY CXX_STANDARD_REQUIRED ON)
