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

# ---- Optional CPU-side test for the YUV->RGB shader math --------------------
if(VC_BUILD_TESTS)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    add_executable(ui_yuv_test YuvMathTest.cpp)
    target_link_libraries(ui_yuv_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME ui_yuv_test COMMAND ui_yuv_test)
  endif()
endif()
