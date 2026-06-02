if(NOT VC_BUILD_CAPTURE)
  return()
endif()

# ---- vc_capture: the platform capture backends + factory --------------------
set(_capture_sources
    CaptureFactory.cpp
    LinuxCapture.cpp
    WindowsCapture.cpp
)

if(APPLE)
  list(APPEND _capture_sources MacCapture.mm)
endif()

add_library(vc_capture STATIC ${_capture_sources})
target_link_libraries(vc_capture PUBLIC vc_common)
target_include_directories(vc_capture PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(APPLE)
  # ARC for the Objective-C++ backend.
  set_source_files_properties(MacCapture.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(vc_capture PUBLIC
      "-framework Foundation"
      "-framework AVFoundation"
      "-framework CoreMedia"
      "-framework CoreVideo"
      "-framework CoreFoundation"
      "-framework Accelerate")
endif()

# ---- capture_dump CLI tool --------------------------------------------------
add_executable(capture_dump ${CMAKE_SOURCE_DIR}/tools/capture_dump.cpp)
target_link_libraries(capture_dump PRIVATE vc_capture vc_common)

# ---- unit tests -------------------------------------------------------------
if(VC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
  add_executable(capture_tests test_capture.cpp)
  target_link_libraries(capture_tests PRIVATE vc_capture vc_common
                        GTest::gtest GTest::gtest_main)
  add_test(NAME capture_tests COMMAND capture_tests)
endif()
