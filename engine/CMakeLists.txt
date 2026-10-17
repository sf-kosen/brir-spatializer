add_library(brir_engine STATIC
    src/fft.cpp
    src/convolver.cpp
    src/spsc_ring.cpp
    src/clock_bridge.cpp
    src/limiter.cpp
    src/renderer.cpp)

target_include_directories(brir_engine PUBLIC include)
target_link_libraries(brir_engine PRIVATE PFFFT)
target_compile_features(brir_engine PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(brir_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(brir_engine PRIVATE -Wall -Wextra)
endif()
