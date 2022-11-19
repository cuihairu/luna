cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(luna LANGUAGES C)
set(CMAKE_C_STANDARD 11)

# custom modules
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
message(STATUS "CMAKE_MODULE_PATH:${CMAKE_MODULE_PATH}")
# option
option(ENABLE_UNIT_TESTS "Enable unit tests" ON)
message(STATUS "Enable testing :${ENABLE_UNIT_TESTS}")


option(CHECKOUT_GIT_SUBMODULE "Check out git submodules during build" ON)
message(STATUS "CHECKOUT_GIT_SUBMODULE:${CHECKOUT_GIT_SUBMODULE}")
include(FetchSubmodules)

# luajit2
include(ExternalProject)
set(LUAJIT_LIB "${PROJECT_SOURCE_DIR}/deps/luajit2/src/libluajit.a")
if(WIN32)
    set(LUAJIT_LIB "${PROJECT_SOURCE_DIR}/deps/luajit2/src/libluajit-5.1.dll.a")
endif()

message(STATUS "luajit lib:${LUAJIT_LIB}")

ExternalProject_Add(
    luajit
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/deps/luajit2/src"
    DOWNLOAD_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND MACOSX_DEPLOYMENT_TARGET=11.6 make all
    BUILD_IN_SOURCE true
    INSTALL_COMMAND ""
)

include_directories(
    "${PROJECT_SOURCE_DIR}/deps/luajit2/src/"
    "${PROJECT_SOURCE_DIR}/deps/luajit2/src/host/"
    "${PROJECT_SOURCE_DIR}/deps/jemalloc/include/"
)

list(APPEND luna_sources
        src/luna.c
        src/luna.h
        src/luna_main.c
        src/luna_hook.c
        src/luna_hook.h
)

add_executable(luna ${luna_sources})
target_link_libraries(luna ${LUAJIT_LIB})

if (ENABLE_UNIT_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
