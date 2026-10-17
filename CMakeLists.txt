cmake_minimum_required(VERSION 3.24)
project(BrirSpatializer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BRIR_BUILD_TESTS "Build unit tests" ON)
option(BRIR_BUILD_TOOLS "Build benchmarks and offline tools" ON)
option(BRIR_BUILD_APP "Build the JUCE desktop app" ON)

include(FetchContent)

# Some dependencies declare an old cmake_minimum_required that CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(PFFFT_USE_TYPE_DOUBLE OFF CACHE BOOL "" FORCE)
set(PFFFT_USE_FFTPACK OFF CACHE BOOL "" FORCE)
set(INSTALL_PFFFT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG aa16fd3db58de4ba5dae8b0438440bb9da46b6fa)
FetchContent_MakeAvailable(pffft)

add_subdirectory(engine)
if(WIN32)
    add_subdirectory(io)
endif()

if(BRIR_BUILD_APP AND WIN32)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(app)
endif()

if(BRIR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BRIR_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
