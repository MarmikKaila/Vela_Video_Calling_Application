if(NOT VC_BUILD_SIGNALING)
  return()
endif()

# Client-side signaling + NAT traversal (Phase 3b).
#
# libjuice is shipped by vcpkg as the static target `LibJuice::LibJuice`
# (libjuice.a, compiled with JUICE_STATIC; pulls in Threads). The WebSocket
# client is implemented in-tree (SignalingClient.cpp) over a raw POSIX socket
# because the vcpkg uWebSockets package ships only a stub WebSocket *client*;
# the server side uses the real uWebSockets server. nlohmann_json carries the
# JSON wire format.
find_package(LibJuice CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(vc_signaling STATIC
  SignalingClient.cpp
  NATTraversal.cpp
  RtpTransport.cpp)
target_include_directories(vc_signaling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vc_signaling
  PUBLIC vc_common
  PRIVATE LibJuice::LibJuice
          nlohmann_json::nlohmann_json)

if(VC_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
  find_package(unofficial-uwebsockets CONFIG REQUIRED)

  add_executable(test_signaling test_signaling.cpp)
  # The end-to-end test starts an in-process uWS signaling server, so it links
  # both the room manager (server routing) and the client library.
  target_include_directories(test_signaling
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../signaling-server)
  target_link_libraries(test_signaling
    PRIVATE vc_signaling
            vc_signaling_rooms
            vc_common
            unofficial::uwebsockets::uwebsockets
            nlohmann_json::nlohmann_json
            GTest::gtest
            GTest::gtest_main)

  add_test(NAME test_signaling COMMAND test_signaling)

  # RtpTransport round trip: two ICE agents on loopback, no STUN, no server.
  add_executable(test_rtp_transport test_rtp_transport.cpp)
  target_link_libraries(test_rtp_transport
    PRIVATE vc_signaling
            vc_common
            GTest::gtest
            GTest::gtest_main)
  add_test(NAME test_rtp_transport COMMAND test_rtp_transport)
endif()
