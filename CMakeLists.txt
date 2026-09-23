cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(luna LANGUAGES C)
set(CMAKE_C_STANDARD 11)

# custom modules
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
message(STATUS "CMAKE_MODULE_PATH:${CMAKE_MODULE_PATH}")
# option
option(ENABLE_UNIT_TESTS "Enable unit tests" ON)
message(STATUS "Enable testing :${ENABLE_UNIT_TESTS}")
# git submodules
option(CHECKOUT_GIT_SUBMODULE "Check out git submodules during build" ON)
message(STATUS "CHECKOUT_GIT_SUBMODULE:${CHECKOUT_GIT_SUBMODULE}")
include(FetchSubmodules)

# official Lua 5.4 (deps/lua, pinned tag v5.4.9)
# core sources live at the repo root; exclude the standalone
# interpreter/amalgamation/debug-driver translation units.
file(GLOB LUA_CORE_SOURCES "${PROJECT_SOURCE_DIR}/deps/lua/*.c")
list(FILTER LUA_CORE_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")
list(LENGTH LUA_CORE_SOURCES LUA_CORE_COUNT)
message(STATUS "lua core sources: ${LUA_CORE_COUNT}")

add_library(lualib STATIC ${LUA_CORE_SOURCES})
target_include_directories(lualib PUBLIC "${PROJECT_SOURCE_DIR}/deps/lua")
# libm: implicit on macOS, explicit on Linux (pow/log/...); dl: loadlib.c
if(UNIX)
    target_link_libraries(lualib PUBLIC m ${CMAKE_DL_LIBS})
endif()

include_directories("${PROJECT_SOURCE_DIR}/deps/lua")

list(APPEND luna_sources
        src/luna.c
        src/luna.h
        src/luna_main.c
        src/luna_hook.c
        src/luna_hook.h
)

add_executable(luna ${luna_sources})
target_link_libraries(luna lualib)

if (ENABLE_UNIT_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
