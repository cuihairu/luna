project(luna_test LANGUAGES C)
include(FetchCMocka)
list(APPEND luna_test_sources
        test_main.c
)
add_executable(${PROJECT_NAME} ${luna_test_sources})
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)

target_link_libraries(${PROJECT_NAME} PRIVATE
        cmocka-static lualib)
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
