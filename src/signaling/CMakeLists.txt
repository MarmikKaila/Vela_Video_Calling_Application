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

# DTLS-SRTP needs OpenSSL (DTLS handshake) + libsrtp2 (SRTP). Found via
# pkg-config like the project's other native deps; ensure openssl@3's and srtp's
# pkgconfig dirs are on PKG_CONFIG_PATH.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SRTP REQUIRED IMPORTED_TARGET libsrtp2)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)

add_library(vc_signaling STATIC
  SignalingClient.cpp
  NATTraversal.cpp
  RtpTransport.cpp
  DtlsSrtp.cpp)
target_include_directories(vc_signaling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vc_signaling
  PUBLIC vc_common
  PRIVATE LibJuice::LibJuice
          nlohmann_json::nlohmann_json
          PkgConfig::SRTP
          PkgConfig::OPENSSL)

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

  # DTLS-SRTP: secured loopback handshake + SRTP round trip + fingerprint check.
  add_executable(test_dtls_srtp test_dtls_srtp.cpp)
  target_link_libraries(test_dtls_srtp
    PRIVATE vc_signaling
            vc_common
            GTest::gtest
            GTest::gtest_main)
  add_test(NAME test_dtls_srtp COMMAND test_dtls_srtp)
endif()
