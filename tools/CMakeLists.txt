add_executable(bench_convolver bench/bench_convolver.cpp)
target_link_libraries(bench_convolver PRIVATE brir_engine)

if(WIN32)
    add_executable(capture_probe capture_probe/capture_probe.cpp)
    target_link_libraries(capture_probe PRIVATE brir_io)
endif()

if(WIN32 AND BRIR_BUILD_APP)
    juce_add_console_app(output_probe PRODUCT_NAME "output_probe")
    target_sources(output_probe PRIVATE output_probe/output_probe.cpp)
    target_compile_definitions(output_probe PRIVATE JUCE_WASAPI=1 JUCE_DIRECTSOUND=0 JUCE_ASIO=0 JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(output_probe PRIVATE brir_io juce::juce_audio_devices
        PUBLIC juce::juce_recommended_config_flags)
    target_compile_options(output_probe PRIVATE /utf-8)
endif()
