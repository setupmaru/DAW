cmake_minimum_required(VERSION 3.22)
project(DAW VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DAW_ENABLE_ASIO "Enable ASIO audio driver (Windows only, requires DAW_ASIO_SDK_DIR)" OFF)
set(DAW_ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK (contains common/ and host/)")

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
    # src/audio/...
    # src/plugins/PluginChain.cpp
)
# App-only sources (DAW only).
set(DAW_APP_SOURCES
    src/Main.cpp
    src/MainComponent.cpp
    # src/plugins/PluginScanner.cpp
    # src/plugins/PluginEditorWindow.cpp
    # src/ui/...
)
# Test sources (DAWTests only).
set(DAW_TEST_SOURCES
    tests/TestMain.cpp
    tests/SmokeTest.cpp
    tests/ChainProcessorTests.cpp
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
if(DAW_ENABLE_ASIO)
    if(NOT DAW_ASIO_SDK_DIR)
        message(FATAL_ERROR "DAW_ENABLE_ASIO=ON requires -DDAW_ASIO_SDK_DIR=<path to ASIO SDK>")
    endif()
    set(DAW_ASIO_DEFINITIONS JUCE_ASIO=1)
    set(DAW_ASIO_INCLUDES "${DAW_ASIO_SDK_DIR}/common" "${DAW_ASIO_SDK_DIR}")
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
add_test(NAME DAWTests COMMAND DAWTests)
