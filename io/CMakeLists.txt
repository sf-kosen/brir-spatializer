add_library(brir_io STATIC
    src/capture_stream.cpp)

target_include_directories(brir_io PUBLIC include)
target_link_libraries(brir_io PUBLIC brir_engine PRIVATE ole32 avrt)

if(MSVC)
    target_compile_options(brir_io PRIVATE /W4 /permissive-)
endif()
