if(NOT VC_BUILD_AUDIO)
  return()
endif()

# ---- vc_audio: microphone capture + speaker playback ------------------------
set(_audio_sources AudioIO.cpp)
if(APPLE)
  list(APPEND _audio_sources MacAudio.mm)
endif()

add_library(vc_audio STATIC ${_audio_sources})
target_link_libraries(vc_audio PUBLIC vc_common)
target_include_directories(vc_audio PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(APPLE)
  set_source_files_properties(MacAudio.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(vc_audio PUBLIC
      "-framework Foundation"
      "-framework AVFoundation"
      "-framework AudioToolbox"
      "-framework CoreAudio")
endif()

# ---- audio_loopback: mic -> Opus encode -> Opus decode -> speaker -----------
# A standalone checkpoint that the full audio path works before any networking.
# Needs the Opus codec module.
if(APPLE AND TARGET vc_codec)
  add_executable(audio_loopback audio_loopback.cpp)
  target_link_libraries(audio_loopback PRIVATE vc_audio vc_codec vc_common)
endif()
