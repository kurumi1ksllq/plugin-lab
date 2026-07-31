cmake_minimum_required(VERSION 3.24)

# ── Plugin Scanner (standalone exe, crash-safe) ─────────────────────────
# This tool runs in a separate process so VST3 crashes don't take down
# the main Plugin Lab application.
juce_add_console_app(VST3Scanner
    PRODUCT_NAME "VST3Scanner"
)

juce_generate_juce_header(VST3Scanner)

target_sources(VST3Scanner PRIVATE
    VST3Scanner.cpp
)

target_link_libraries(VST3Scanner PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

target_compile_definitions(VST3Scanner PRIVATE
    JUCE_PLUGINHOST_VST3=1
)

if(MSVC)
    target_compile_options(VST3Scanner PRIVATE /W4 /permissive- /WX /utf-8)
endif()

# Copy VST3Scanner.exe next to Plugin Lab.exe after build
add_custom_command(TARGET VST3Scanner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:VST3Scanner>"
        "${CMAKE_CURRENT_BINARY_DIR}/../PluginLab_artefacts/$<CONFIG>/VST3Scanner.exe"
    COMMENT "Copying VST3Scanner to Plugin Lab directory"
)
