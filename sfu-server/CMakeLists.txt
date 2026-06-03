# Integrated SFU + signaling server. Built when both the SFU and signaling
# (which provides RtpTransport/NATTraversal) modules are enabled.
if(NOT VC_BUILD_SFU OR NOT VC_BUILD_SIGNALING)
  return()
endif()

find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(sfu_server main.cpp)
target_link_libraries(sfu_server
  PRIVATE vc_sfu
          vc_signaling
          vc_common
          unofficial::uwebsockets::uwebsockets
          nlohmann_json::nlohmann_json)

# Headless end-to-end check: two clients exchange RTP through a running server.
if(VC_BUILD_TESTS)
  add_executable(sfu_smoketest smoketest.cpp)
  target_link_libraries(sfu_smoketest PRIVATE vc_signaling vc_common)
endif()
