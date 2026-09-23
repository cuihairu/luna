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
target_link_libraries(luna_test_repl PRIVATE cmocka-static lualib luna_kernel)
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

