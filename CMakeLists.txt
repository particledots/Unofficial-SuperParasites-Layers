cmake_minimum_required(VERSION 3.22)
project(SuperParasitesLayersPlugin VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  # The GitHub workflow passes -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal build.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.6
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Plugin formats: AU + VST3 (+ Standalone for quick testing).
set(FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND FORMATS AU)
endif()

juce_add_plugin(SuperParasitesLayers
  COMPANY_NAME "particledots"
  PLUGIN_MANUFACTURER_CODE Ptdt
  PLUGIN_CODE SPLy
  FORMATS ${FORMATS}
  PRODUCT_NAME "Unofficial SuperParasites Layers"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  AU_MAIN_TYPE kAudioUnitType_MusicEffect   # lets Logic route MIDI to it as a "MIDI-controlled effect"
  COPY_PLUGIN_AFTER_BUILD TRUE)

# --- DSP core: vendored SuperParasites/stmlib code + our JUCE-free wrapper, built with plain flags ---
add_library(superparasites_core STATIC
  src/SuperParasitesEngine.cpp
  src/LayerAudio.cpp
  third_party/supercell/dsp/correlator.cc
  third_party/supercell/dsp/granular_processor.cc
  third_party/supercell/dsp/kammerl_player.cc
  third_party/supercell/dsp/mu_law.cc
  third_party/supercell/dsp/pvoc/frame_transformation.cc
  third_party/supercell/dsp/pvoc/phase_vocoder.cc
  third_party/supercell/dsp/pvoc/spectral_clouds_transformation.cc
  third_party/supercell/dsp/pvoc/stft.cc
  third_party/supercell/resources.cc
  third_party/stmlib/utils/random.cc
  third_party/stmlib/dsp/units.cc
  third_party/stmlib/dsp/atan.cc)
target_include_directories(superparasites_core PUBLIC src third_party)
target_compile_definitions(superparasites_core PRIVATE TEST=1)
set_target_properties(superparasites_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Some upstream headers rely on <cstdio>/<cstdlib>/<cmath> being pulled in transitively.
if(MSVC)
  target_compile_options(superparasites_core PRIVATE /FIcstdio /FIcstdlib /FIcmath /FIcstring)
else()
  target_compile_options(superparasites_core PRIVATE "SHELL:-include cstdio" "SHELL:-include cstdlib" "SHELL:-include cmath" "SHELL:-include cstring" -Wno-unused-local-typedef -Wno-narrowing)
endif()

# --- The plugin ---
target_sources(SuperParasitesLayers PRIVATE src/PluginProcessor.cpp src/PluginEditor.cpp)
target_compile_definitions(SuperParasitesLayers PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SuperParasitesLayers PRIVATE
  superparasites_core
  juce::juce_audio_utils
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)

# --- Headless test of the DSP wrapper:  cmake --build build --target engine_test && build/engine_test ---
add_executable(engine_test EXCLUDE_FROM_ALL src/engine_test.cpp)
target_link_libraries(engine_test PRIVATE superparasites_core)

# --- Comparison of the 32 kHz and 48 kHz engines:  cmake --build build --target rate_compare_test && build/rate_compare_test ---
add_executable(rate_compare_test EXCLUDE_FROM_ALL tests/rate_compare_test.cpp)
target_link_libraries(rate_compare_test PRIVATE superparasites_core)

# --- Checksums of Live's output, to compare with the HD plug-in's (see the workflow) ---
add_executable(live_dump EXCLUDE_FROM_ALL tests/live_dump.cpp)
target_link_libraries(live_dump PRIVATE superparasites_core)

# --- Tests of the layers:  cmake --build build --target layers_test && build/layers_test ---
add_executable(layers_test EXCLUDE_FROM_ALL tests/layers_test.cpp)
target_link_libraries(layers_test PRIVATE superparasites_core)
