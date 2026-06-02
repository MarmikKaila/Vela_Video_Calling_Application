if(NOT VC_BUILD_SFU)
  return()
endif()

# ---- vc_sfu: the Selective Forwarding Unit ----------------------------------
# Depends on vc_network for the RtpPacket wire type (forwarded verbatim).
add_library(vc_sfu STATIC
  Participant.cpp
  Room.cpp
  SFUServer.cpp
)
target_link_libraries(vc_sfu PUBLIC vc_common vc_network)
target_include_directories(vc_sfu PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ---- unit tests -------------------------------------------------------------
if(VC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
  add_executable(sfu_tests test_sfu.cpp)
  target_link_libraries(sfu_tests PRIVATE vc_sfu vc_common vc_network
                        GTest::gtest GTest::gtest_main)
  add_test(NAME sfu_tests COMMAND sfu_tests)
endif()
