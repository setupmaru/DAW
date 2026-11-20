cmake_minimum_required(VERSION 3.22)
project(DAW VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ASIO (Windows only). JUCE 8 bundles the ASIO SDK headers (Steinberg ASIO
# License or GPLv3; used here under GPLv3, compatible with this project's
# AGPLv3), so nothing else is needed. DAW_ASIO_SDK_DIR
# optionally points at another copy of the SDK instead.
if(WIN32)
    set(DAW_ASIO_DEFAULT ON)
else()
    set(DAW_ASIO_DEFAULT OFF)
endif()
option(DAW_ENABLE_ASIO "Enable ASIO audio devices (Windows only)" ${DAW_ASIO_DEFAULT})
set(DAW_ASIO_SDK_DIR "" CACHE PATH "Optional: an external Steinberg ASIO SDK (contains common/ and host/) instead of JUCE's bundled headers")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.17-security
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

enable_testing()

# --- Source lists. Later tasks only add files to these lists. -------------
# Core sources: compiled into BOTH DAW and DAWTests (no static lib, to avoid
# linking JUCE modules twice).
set(DAW_CORE_SOURCES
    src/audio/ChainProcessor.cpp
    src/audio/AudioEngine.cpp
    src/plugins/PluginChain.cpp
    # src/audio/...
)
# App-only sources (DAW only).
set(DAW_APP_SOURCES
    src/Main.cpp
    src/MainComponent.cpp
    src/plugins/PluginScanner.cpp
    src/plugins/PluginEditorWindow.cpp
    src/ui/ChainListComponent.cpp
    src/ui/LevelMeter.cpp
    src/ui/PresetStore.cpp
)
# Test sources (DAWTests only).
set(DAW_TEST_SOURCES
    tests/TestMain.cpp
    tests/SmokeTest.cpp
    tests/ChainProcessorTests.cpp
    tests/AudioEngineTests.cpp
    tests/PluginChainTests.cpp
    tests/PluginScannerTests.cpp
    # App-only, also compiled here so its queueing logic can be tested
    # without VST3 plugins (an empty format manager).
    src/plugins/PluginScanner.cpp
    tests/PresetStoreTests.cpp
    tests/AutosaveGateTests.cpp
    # App-only file handling for presets; tested in a temporary folder.
    src/ui/PresetStore.cpp
)

# --- Shared settings -------------------------------------------------------
set(DAW_JUCE_LIBRARIES
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_gui_extra)

set(DAW_ASIO_DEFINITIONS "")
set(DAW_ASIO_INCLUDES "")
if(DAW_ENABLE_ASIO AND WIN32)
    set(DAW_ASIO_DEFINITIONS JUCE_ASIO=1)

    if(DAW_ASIO_SDK_DIR)
        if(NOT EXISTS "${DAW_ASIO_SDK_DIR}/common/iasiodrv.h")
            message(FATAL_ERROR "DAW_ASIO_SDK_DIR does not look like an ASIO SDK: ${DAW_ASIO_SDK_DIR}/common/iasiodrv.h not found")
        endif()
        list(APPEND DAW_ASIO_DEFINITIONS JUCE_ASIO_USE_EXTERNAL_SDK=1)
        set(DAW_ASIO_INCLUDES "${DAW_ASIO_SDK_DIR}/common" "${DAW_ASIO_SDK_DIR}")
    endif()
elseif(DAW_ENABLE_ASIO)
    message(STATUS "DAW_ENABLE_ASIO ignored: ASIO is Windows only")
endif()

function(daw_configure_target target)
    target_include_directories(${target} PRIVATE src ${DAW_ASIO_INCLUDES})
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_compile_definitions(${target} PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        ${DAW_ASIO_DEFINITIONS})
    target_link_libraries(${target} PRIVATE
        ${DAW_JUCE_LIBRARIES}
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endfunction()

# --- App -------------------------------------------------------------------
juce_add_gui_app(DAW PRODUCT_NAME "DAW")
target_sources(DAW PRIVATE ${DAW_CORE_SOURCES} ${DAW_APP_SOURCES})
daw_configure_target(DAW)

# --- Tests -----------------------------------------------------------------
juce_add_console_app(DAWTests PRODUCT_NAME "DAWTests")
target_sources(DAWTests PRIVATE ${DAW_CORE_SOURCES} ${DAW_TEST_SOURCES})
daw_configure_target(DAWTests)
# Tests pump the message loop with MessageManager::runDispatchLoopUntil().
target_compile_definitions(DAWTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
add_test(NAME DAWTests COMMAND DAWTests)
