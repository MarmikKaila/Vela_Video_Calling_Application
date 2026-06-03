if(NOT VC_BUILD_MEDIA)
  return()
endif()

# ---- vc_media: pipeline wiring + A/V sync -----------------------------------
# Ties together capture, codec, and network into an end-to-end media path.
add_library(vc_media STATIC
  MediaPipeline.cpp
  AVSync.cpp
  ReceiveRouter.cpp
)
target_link_libraries(vc_media
  PUBLIC vc_common vc_codec vc_network vc_capture)
target_include_directories(vc_media PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ---- unit tests -------------------------------------------------------------
if(VC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
  add_executable(media_tests test_media.cpp)
  target_link_libraries(media_tests PRIVATE vc_media vc_common
                        GTest::gtest GTest::gtest_main)
  add_test(NAME media_tests COMMAND media_tests)
endif()
