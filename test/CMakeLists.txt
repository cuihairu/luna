project(luna_test LANGUAGES C)
include(FetchCMocka)

list(APPEND luna_test_sources
        test_main.c
)
add_executable(${PROJECT_NAME} ${luna_test_sources})
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)

target_link_libraries(${PROJECT_NAME} PRIVATE
        cmocka-static lualib luna_kernel)
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})

# REPL session: evaluation, multi-line chunks, SIGINT abort
add_executable(luna_test_repl luna_test_repl.c)
target_link_libraries(luna_test_repl PRIVATE cmocka-static lualib lpeg luna_kernel)
add_test(NAME repl COMMAND luna_test_repl)

# CLI: script / eval / interactive launch modes through the real binary
add_executable(luna_test_cli luna_test_cli.c)
target_link_libraries(luna_test_cli PRIVATE cmocka-static lualib luna_kernel)
target_compile_definitions(luna_test_cli PRIVATE
        LUNA_BIN="$<TARGET_FILE:luna>"
        LUNA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME cli COMMAND luna_test_cli)

# Tab completion: chains, fields, metatables, plugin sources
add_executable(luna_test_complete luna_test_complete.c)
target_link_libraries(luna_test_complete PRIVATE cmocka-static lualib luna_kernel)
add_test(NAME complete COMMAND luna_test_complete)

# Introspection: repr, help/whos, ?-sugar
add_executable(luna_test_introspect luna_test_introspect.c)
target_link_libraries(luna_test_introspect PRIVATE cmocka-static lualib lpeg luna_kernel)
add_test(NAME introspect COMMAND luna_test_introspect)

# Syntax highlighting: scintillua/LPeg rendering, extension point,
# no-color degradation, colored session output
add_executable(luna_test_highlight luna_test_highlight.c)
target_link_libraries(luna_test_highlight PRIVATE cmocka-static lualib lpeg luna_kernel)
target_compile_definitions(luna_test_highlight PRIVATE
        LUNA_TEST_LEXERS_DIR="${CMAKE_BINARY_DIR}/luna_modules/lexers")
add_test(NAME highlight COMMAND luna_test_highlight)

# Magic commands: %time/%timeit/%hist/... and the register extension point
add_executable(luna_test_magic luna_test_magic.c)
target_link_libraries(luna_test_magic PRIVATE cmocka-static lualib lpeg luna_kernel)
add_test(NAME magic COMMAND luna_test_magic)

# Module system: luna_modules resolution, manifests, stdlib modules
add_executable(luna_test_modules luna_test_modules.c)
target_link_libraries(luna_test_modules PRIVATE cmocka-static lualib lfs luasocket lua_zlib)
if(LUNA_HAVE_OPENSSL)
    target_link_libraries(luna_test_modules PRIVATE luaossl)
    target_compile_definitions(luna_test_modules PRIVATE LUNA_HAVE_OPENSSL=1)
endif()
target_compile_definitions(luna_test_modules PRIVATE
        LUNA_TEST_MODULES_DIR="${CMAKE_BINARY_DIR}/luna_modules"
        FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/modules")
add_test(NAME modules COMMAND luna_test_modules)

# Plugin system: discovery order, manifests, injection, isolation
add_executable(luna_test_plugins luna_test_plugins.c)
target_link_libraries(luna_test_plugins PRIVATE cmocka-static lualib lpeg lfs luna_kernel)
target_compile_definitions(luna_test_plugins PRIVATE
        LUNA_TEST_PLUGIN_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        LUNA_TEST_MODULES_DIR="${CMAKE_BINARY_DIR}/luna_modules")
add_test(NAME plugins COMMAND luna_test_plugins)

# Attach channel: unix socket serve, framed replies, error isolation
add_executable(luna_test_serve luna_test_serve.c)
target_link_libraries(luna_test_serve PRIVATE cmocka-static lualib lpeg luasocket luna_kernel)
# the idle-wake test drives the real binary on a pty
target_link_libraries(luna_test_serve PRIVATE util)
target_compile_definitions(luna_test_serve PRIVATE
        LUNA_BINARY="$<TARGET_FILE:luna>")
add_test(NAME serve COMMAND luna_test_serve)


# Opt-in event loop: timers/immediates, keep-alive, serve poll, ^C
add_executable(luna_test_loop luna_test_loop.c)
target_link_libraries(luna_test_loop PRIVATE cmocka-static lualib luna_loop luna_kernel Threads::Threads)
add_test(NAME loop COMMAND luna_test_loop)

# Package management: install → luna.lock → reproduce-from-lock, driven
# through the real binary; the online install step needs network
add_executable(luna_test_rocks luna_test_rocks.c)
target_link_libraries(luna_test_rocks PRIVATE cmocka-static lualib luna_kernel)
target_compile_definitions(luna_test_rocks PRIVATE LUNA_BIN="$<TARGET_FILE:luna>")
add_test(NAME rocks COMMAND luna_test_rocks)
set_tests_properties(rocks PROPERTIES TIMEOUT 300)
