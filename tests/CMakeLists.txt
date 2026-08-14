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
    ../source/signal/MultiTone.cpp ../source/signal/SequentialTone.cpp ../source/signal/ToneBurst.cpp ../source/signal/Impulse.cpp
    ../source/signal/FilePlayback.cpp ../source/signal/NoiseGenerator.cpp
    ../source/signal/EnvelopeSignal.cpp
    ../source/capture/AudioBuffer.cpp ../source/capture/SweepRunner.cpp
    ../source/capture/MeasurementSession.cpp
    ../source/capture/ParameterTimeline.cpp
    ../source/scan/ScanEngine.cpp
    ../source/analysis/FreqResponse.cpp ../source/analysis/HarmonicAnalysis.cpp
    ../source/analysis/CompressionCurve.cpp ../source/analysis/Export.cpp
    ../source/analysis/GainReduction.cpp ../source/analysis/TimeConstants.cpp
    ../source/analysis/CompressionFamily.cpp
    ../source/analysis/WavExporter.cpp
    ../source/analysis/WavCaptureReader.cpp
    ../source/analysis/ChildWavAnalyzer.cpp
    ../source/analysis/MeasurementAnalysis.cpp
    ../source/utils/FftHelper.cpp ../source/utils/MathUtils.cpp
    ../source/ipc/CommandParser.cpp
    ../source/ipc/PipeServer.cpp
    ../source/host/PluginManager.cpp
    ../source/host/EditorCrashGuard.cpp
    ../source/host/ChildProcessCoordinator.cpp
    ../source/host/ChildMeasureOrchestrator.cpp
    ../source/replica/ReplicaSpec.cpp
    ../source/replica/ReplicaChain.cpp
    CommandParserStubs.cpp
    TestPlugin.h TestPluginTests.cpp TestCompressorPlugin.h TestCompressorPluginTests.cpp ToneBurstTests.cpp SweepRunnerTests.cpp CaptureBufferTests.cpp FreqResponseTests.cpp ExportTests.cpp
    CommandParserTests.cpp PipeServerTests.cpp
    CommandParserConcurrencyTests.cpp
    CommandParserRoutingTests.cpp
    CommandParserLoadPluginBlacklistTests.cpp
    PluginManagerTests.cpp
    FilePlaybackTests.cpp NoiseGeneratorTests.cpp EnvelopeSignalTests.cpp MultiToneTests.cpp SequentialToneTests.cpp
    GainReductionTests.cpp ScanEngineTests.cpp
    TimeConstantsTests.cpp
    ReplicaSpecTests.cpp
    ReplicaChainTests.cpp
    PluginLabReplicaTests.cpp
    CompressionFamilyTests.cpp
    ComponentVisibilityTests.cpp
    EditorCrashGuardTests.cpp
    WavExporterTests.cpp
    WavCaptureReaderTests.cpp
    ParameterTimelineTests.cpp
    ChildProcessCoordinatorTests.cpp
    PluginHostChildIntegrationTests.cpp
    ChildHostParityTests.cpp
    ChildProtocolTests.cpp
    ChildProcessRestartTests.cpp
    ChildMeasureOrchestratorTests.cpp)
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

# SweepRunner.cpp guards plugin calls with catch(...) (block C task 1);
# /EHa lets it also intercept SEH faults, matching the production target.
set_source_files_properties(../source/capture/SweepRunner.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# EditorCrashGuard.cpp touches plugin-DLL editor create/delete paths; /EHa
# matches the production target (block C task 2).
set_source_files_properties(../source/host/EditorCrashGuard.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# ChildHostParityTests.cpp loads and processes a REAL VST3 plugin in-process
# (T3, D2b acceptance); /EHa matches PluginHostChild.cpp's plugin-path
# protection so an SEH fault in the plugin DLL is intercepted, not a crash.
set_source_files_properties(ChildHostParityTests.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# PluginLabReplicaTests.cpp loads and processes the REAL replica VST3
# in-process (issue #27 T4); /EHa matches ChildHostParityTests.cpp so an SEH
# fault in the plugin DLL is intercepted, not a crash.
set_source_files_properties(PluginLabReplicaTests.cpp PROPERTIES
    COMPILE_FLAGS "/EHa"
)

# ── TestChildProcess: stdin/stdout JSON-line double for the
#    ChildProcessCoordinator tests (block D1c) ──────────────────────────────
# Plain exe on purpose: it only uses Win32 console I/O, no JUCE. The exe path
# is baked into unit_tests as TEST_CHILD_EXE (absolute at generation time);
# add_dependencies guarantees it is built before unit_tests so the compiled
# path always exists.
add_executable(TestChildProcess TestChildProcess.cpp)
if(MSVC)
    target_compile_options(TestChildProcess PRIVATE /W4 /permissive- /WX /utf-8)
endif()
add_dependencies(unit_tests TestChildProcess)
target_compile_definitions(unit_tests PRIVATE
    TEST_CHILD_EXE="$<TARGET_FILE:TestChildProcess>")

# The real out-of-process host child (block D1a+D1b, root CMakeLists.txt)
# backs the D1d integration tests. The target is defined in the parent scope
# before add_subdirectory(tests), so $<TARGET_FILE:PluginHostChild> resolves;
# add_dependencies guarantees the real child is built before unit_tests.
add_dependencies(unit_tests PluginHostChild)
target_compile_definitions(unit_tests PRIVATE
    PLUGIN_HOST_CHILD_EXE="$<TARGET_FILE:PluginHostChild>"
    CHILD_PARITY_PLUGIN="C:/Program Files/Common Files/VST3/magic.CURVE.vst3"
    # SuicidePlugin artifact (tests/CMakeLists.txt juce_add_plugin below):
    #   build/tests/SuicidePlugin_artefacts/<CONFIG>/VST3/SuicidePlugin.vst3
    # A .vst3 is a DIRECTORY — tests use exists() not existsAsFile().
    # Forward slashes throughout (CMake normalizes; avoids MSVC C4129).
    SUICIDE_PLUGIN="${CMAKE_CURRENT_BINARY_DIR}/SuicidePlugin_artefacts/$<CONFIG>/VST3/SuicidePlugin.vst3"
    # PluginLabReplica artifact (root CMakeLists.txt juce_add_plugin):
    #   build/PluginLabReplica_artefacts/<CONFIG>/VST3/PluginLabReplica.vst3
    # A .vst3 is a DIRECTORY — tests use exists() not existsAsFile().
    # Forward slashes throughout (CMake normalizes; avoids MSVC C4129).
    REPLICA_PLUGIN="${CMAKE_CURRENT_BINARY_DIR}/../PluginLabReplica_artefacts/$<CONFIG>/VST3/PluginLabReplica.vst3")
catch_discover_tests(unit_tests)

# ── SuicidePlugin: crash-injection VST3 fixture (block D3b / ADR-D-2) ───────
# A real VST3 whose crash_mode parameter (0=passthrough / 1=ExitProcess(1) /
# 2=abort()) lets the D3b auto-restart tests kill the child process on demand.
# Built standalone (NOT into unit_tests — a single-process crash would kill the
# test runner); the child host loads it via VST3PluginFormat and arms the crash
# through the parameter interface. Artifact layout (JUCE):
#   build/tests/SuicidePlugin_artefacts/<CONFIG>/VST3/SuicidePlugin.vst3/
#       Contents/x86_64-win/SuicidePlugin.vst3
juce_add_plugin(SuicidePlugin
    PLUGIN_MANUFACTURER_CODE PLab
    PLUGIN_CODE Scid
    PRODUCT_NAME "SuicidePlugin"
    FORMATS VST3)

target_sources(SuicidePlugin PRIVATE
    SuicidePlugin/SuicidePlugin.cpp
    SuicidePlugin/SuicidePlugin.h)

target_link_libraries(SuicidePlugin PUBLIC
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

target_compile_definitions(SuicidePlugin PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0)

if(MSVC)
    target_compile_options(SuicidePlugin PRIVATE /W4 /permissive- /WX /utf-8)
endif()

# The restart tests spawn the REAL child which loads the REAL SuicidePlugin
# VST3 through VST3PluginFormat — guarantee the fixture is built before
# unit_tests runs (same pattern as TestChildProcess / PluginHostChild).
add_dependencies(unit_tests SuicidePlugin)

# PluginLabReplicaTests.cpp loads the REAL replica VST3 in-process through
# VST3PluginFormat — guarantee the fixture is built before unit_tests runs
# (same pattern as SuicidePlugin).
add_dependencies(unit_tests PluginLabReplica)
