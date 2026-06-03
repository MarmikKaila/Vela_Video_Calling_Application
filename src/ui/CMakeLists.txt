if(NOT VC_BUILD_UI)
  return()
endif()

# ---- Qt6 UI module -----------------------------------------------------------
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

# Static library holding the reusable widgets.
add_library(vc_ui STATIC
  VideoWidget.cpp
  MainWindow.cpp
  SettingsDialog.cpp
  MockFrameSource.cpp
)

target_include_directories(vc_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(vc_ui
  PUBLIC
    vc_common
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
)

# ---- Visual-proof demo executable -------------------------------------------
add_executable(ui_demo main.cpp)
target_link_libraries(ui_demo
  PRIVATE
    vc_ui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    vc_common
)

# ---- loopback_call: live camera through the full encode/RTP/jitter/decode path
# Needs the capture + media (+ codec/network) modules too. Built only when those
# targets exist, so a UI-only build still works.
if(TARGET vc_capture AND TARGET vc_media)
  add_executable(loopback_call loopback_call.cpp)
  target_link_libraries(loopback_call PRIVATE
    vc_ui vc_media vc_capture vc_codec vc_network vc_common
    Qt6::Widgets Qt6::OpenGLWidgets)
endif()

# ---- group_call: real cross-laptop call via the SFU --------------------------
# Needs media + capture + audio + signaling (RtpTransport/SignalingClient).
if(TARGET vc_capture AND TARGET vc_media AND TARGET vc_signaling AND TARGET vc_audio)
  add_executable(group_call group_call.cpp)
  target_link_libraries(group_call PRIVATE
    vc_ui vc_media vc_capture vc_codec vc_network vc_audio vc_signaling vc_common
    Qt6::Widgets Qt6::OpenGLWidgets)
endif()

# ---- Optional CPU-side test for the YUV->RGB shader math --------------------
if(VC_BUILD_TESTS)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    add_executable(ui_yuv_test YuvMathTest.cpp)
    target_link_libraries(ui_yuv_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME ui_yuv_test COMMAND ui_yuv_test)
  endif()
endif()
