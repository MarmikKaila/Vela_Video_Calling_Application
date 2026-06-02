if(NOT VC_BUILD_NETWORK)
  return()
endif()

# vc_network — RTP packetization + jitter buffer (Phase 3a). Pure protocol
# logic; depends only on the header-only shared contracts in vc_common.
add_library(vc_network STATIC
  RTPHandler.cpp
  JitterBuffer.cpp
)
target_include_directories(vc_network PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(vc_network PUBLIC cxx_std_20)
target_link_libraries(vc_network PUBLIC vc_common)

if(VC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)

  add_executable(test_rtp_handler test_rtp_handler.cpp)
  target_link_libraries(test_rtp_handler PRIVATE vc_network GTest::gtest GTest::gtest_main)
  add_test(NAME rtp_handler COMMAND test_rtp_handler)

  add_executable(test_jitter_buffer test_jitter_buffer.cpp)
  target_link_libraries(test_jitter_buffer PRIVATE vc_network GTest::gtest GTest::gtest_main)
  add_test(NAME jitter_buffer COMMAND test_jitter_buffer)
endif()
