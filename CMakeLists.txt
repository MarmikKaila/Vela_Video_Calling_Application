cmake_minimum_required(VERSION 3.21)

# Pick up vcpkg's toolchain automatically if VCPKG_ROOT is set and the caller
# didn't already pass -DCMAKE_TOOLCHAIN_FILE=...
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "vcpkg toolchain")
endif()

project(video_call_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---- Feature switches (each phase flips its module on as it lands) ----------
option(VC_BUILD_CAPTURE   "Build the capture backends"          OFF)
option(VC_BUILD_CODEC     "Build the FFmpeg/Opus codec module"  OFF)
option(VC_BUILD_NETWORK   "Build RTP/jitter/signaling-client"   OFF)
option(VC_BUILD_SFU       "Build the SFU module"                OFF)
option(VC_BUILD_MEDIA     "Build the media pipeline + AVSync"   OFF)
option(VC_BUILD_SIGNALING "Build the standalone signaling srv"  OFF)
option(VC_BUILD_UI        "Build the Qt6 client UI"             OFF)
option(VC_BUILD_TESTS     "Build unit tests (gtest)"            ON)

# ---- Shared contracts: header-only, depended on by every module ------------
add_library(vc_common INTERFACE)
target_include_directories(vc_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(vc_common INTERFACE cxx_std_20)

# Sanity target: compiles every contract header so a green build proves the
# shared seams are well-formed even before any module is implemented.
add_executable(contracts_selfcheck tools/contracts_selfcheck.cpp)
target_link_libraries(contracts_selfcheck PRIVATE vc_common)

# Enable testing BEFORE adding modules so their add_test() calls register.
if(VC_BUILD_TESTS)
  enable_testing()
endif()

# ---- Modules (added by their phase; safe no-ops until the dir has CMake) ----
foreach(_mod IN ITEMS
        src/capture src/codec src/network src/signaling src/sfu src/media src/ui
        signaling-server tests)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_mod}/CMakeLists.txt)
    add_subdirectory(${_mod})
  endif()
endforeach()
