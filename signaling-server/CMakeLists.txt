if(NOT VC_BUILD_SIGNALING)
  return()
endif()

# Standalone WebSocket signaling server (Phase 3b).
#
# uWebSockets is shipped by vcpkg as the header-only target
# `unofficial::uwebsockets::uwebsockets`, which transitively pulls in the
# static `unofficial::usockets::usockets` (libuSockets.a). The vcpkg build sets
# UWS_NO_ZLIB, so no zlib link is required. nlohmann_json carries the JSON
# message codec.
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# RoomManager is also linked into the test target, so expose it as a small lib.
add_library(vc_signaling_rooms STATIC RoomManager.cpp)
target_include_directories(vc_signaling_rooms PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vc_signaling_rooms
  PUBLIC vc_common
  PRIVATE nlohmann_json::nlohmann_json)

add_executable(signaling_server main.cpp)
target_link_libraries(signaling_server
  PRIVATE vc_signaling_rooms
          vc_common
          unofficial::uwebsockets::uwebsockets
          nlohmann_json::nlohmann_json)
