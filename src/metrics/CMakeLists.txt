if(NOT VC_BUILD_METRICS)
  return()
endif()

# ---- vc_metrics: structured logging (spdlog) + runtime metrics --------------
find_package(spdlog CONFIG REQUIRED)

add_library(vc_metrics STATIC
  Log.cpp
  Metrics.cpp
)
target_link_libraries(vc_metrics PUBLIC vc_common spdlog::spdlog)
target_include_directories(vc_metrics PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(VC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
  add_executable(metrics_tests test_metrics.cpp)
  target_link_libraries(metrics_tests PRIVATE vc_metrics
                        GTest::gtest GTest::gtest_main)
  add_test(NAME metrics_tests COMMAND metrics_tests)
endif()
