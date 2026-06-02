if(NOT VC_BUILD_CODEC)
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

# ---- vc_codec: H.264 (libx264 via FFmpeg) + Opus encode/decode -------------
add_library(vc_codec STATIC
  VideoEncoder.cpp
  VideoDecoder.cpp
  AudioEncoder.cpp
  AudioDecoder.cpp
)

target_link_libraries(vc_codec
  PUBLIC  vc_common
  PRIVATE PkgConfig::FFMPEG PkgConfig::OPUS
)

target_compile_features(vc_codec PUBLIC cxx_std_20)

# ---- Tests -----------------------------------------------------------------
if(VC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)

  add_executable(test_codec test_codec.cpp)
  target_link_libraries(test_codec
    PRIVATE vc_codec vc_common GTest::gtest GTest::gtest_main
  )

  add_test(NAME codec_roundtrip COMMAND test_codec)
endif()
