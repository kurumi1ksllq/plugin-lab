# ── Unit test infrastructure: Catch2 v3 + standalone console target ─────────
include(FetchContent)
FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
# Note: Catch2 v3.8.0's extras/ contains Catch.cmake (not Catch2.cmake),
# which provides catch_discover_tests.
include(Catch)

juce_add_console_app(unit_tests)
juce_generate_juce_header(unit_tests)
target_sources(unit_tests PRIVATE
    ../source/signal/SignalGenerator.cpp ../source/signal/SineSweep.cpp
    ../source/signal/MultiTone.cpp ../source/signal/ToneBurst.cpp ../source/signal/Impulse.cpp
    ../source/signal/FilePlayback.cpp ../source/signal/NoiseGenerator.cpp
    ../source/signal/EnvelopeSignal.cpp
    ../source/capture/AudioBuffer.cpp ../source/capture/SweepRunner.cpp
    ../source/capture/MeasurementSession.cpp
    ../source/scan/ScanEngine.cpp
    ../source/analysis/FreqResponse.cpp ../source/analysis/HarmonicAnalysis.cpp
    ../source/analysis/CompressionCurve.cpp ../source/analysis/Export.cpp
    ../source/utils/FftHelper.cpp ../source/utils/MathUtils.cpp
    ../source/ipc/CommandParser.cpp
    ../source/ipc/PipeServer.cpp
    ../source/host/PluginManager.cpp
    CommandParserStubs.cpp
    TestPlugin.h TestPluginTests.cpp TestCompressorPlugin.h TestCompressorPluginTests.cpp ToneBurstTests.cpp SweepRunnerTests.cpp FreqResponseTests.cpp ExportTests.cpp
    CommandParserTests.cpp PipeServerTests.cpp
    FilePlaybackTests.cpp NoiseGeneratorTests.cpp EnvelopeSignalTests.cpp
    ScanEngineTests.cpp)
target_link_libraries(unit_tests PRIVATE
    juce::juce_audio_basics juce::juce_audio_processors juce::juce_audio_utils juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags
    Catch2::Catch2WithMain)
if(MSVC)
    target_compile_options(unit_tests PRIVATE /W4 /permissive- /WX /utf-8)
endif()
if(WIN32)
    target_compile_definitions(unit_tests PRIVATE
        JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1)
endif()
catch_discover_tests(unit_tests)
