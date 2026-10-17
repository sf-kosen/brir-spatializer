juce_add_gui_app(BrirCinema
    PRODUCT_NAME "BRIR Cinema"
    COMPANY_NAME "mikansubre"
    VERSION ${PROJECT_VERSION})

target_sources(BrirCinema PRIVATE
    src/Main.cpp
    src/MainComponent.cpp)

target_compile_definitions(BrirCinema PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_ASIO=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:BrirCinema,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:BrirCinema,JUCE_VERSION>")

target_link_libraries(BrirCinema
    PRIVATE
        brir_io
        avrt
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

if(MSVC)
    target_compile_options(BrirCinema PRIVATE /utf-8)
endif()
