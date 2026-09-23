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
