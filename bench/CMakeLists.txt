if(NOT VC_BUILD_BENCH)
  return()
endif()

# Benchmarks need the SFU and the metrics/logging library.
if(NOT TARGET vc_sfu OR NOT TARGET vc_metrics)
  message(FATAL_ERROR
    "VC_BUILD_BENCH requires VC_BUILD_SFU=ON and VC_BUILD_METRICS=ON")
endif()

add_executable(sfu_benchmark sfu_benchmark.cpp)
target_link_libraries(sfu_benchmark PRIVATE vc_sfu vc_network vc_metrics vc_common)
