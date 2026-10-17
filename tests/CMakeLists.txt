FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11)
FetchContent_MakeAvailable(doctest)

add_executable(brir_tests
    test_main.cpp
    test_convolver.cpp
    test_clock_bridge.cpp
    test_renderer.cpp
    test_upmixer.cpp)

target_link_libraries(brir_tests PRIVATE brir_engine doctest::doctest)

add_test(NAME brir_tests COMMAND brir_tests)
