# vim: set sts=2 ts=2 sw=2 et:
#
# CS2GLAZ - CMake build.
#
# Port of the AMBuild scripts (configure.py / AMBuildScript / AMBuilder /
# PackageScript) to CMake. Mirrors the cs2ac-ce approach, but the plugin target
# follows cs2glaz's own AMBuilder exactly:
#   - plugin sources are an EXPLICIT list (no GLOB), so baker/tests-only TUs
#     (src/baker/*, src/core/builder.cpp) never leak into the module;
#   - third_party/miniz.c and MaskedOcclusionCulling.cpp are part of the plugin;
#   - tier1/convar.cpp and public/tier0/memoverride.cpp are compiled into the
#     plugin ONLY (not into cs2glaz-protobuf), avoiding duplicate symbols.
#
# Dependencies stay where they were: hl2sdk-cs2/, metamod-source/,
# vendor/funchook/, third_party/.
#
# Windows (from "x64 Native Tools Command Prompt for VS"):
#   cmake -S . -B build-windows-release -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-windows-release --parallel
#
# Linux (Clang + lld):
#   cmake -S . -B build-linux-release -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
#   cmake --build build-linux-release --parallel

cmake_minimum_required(VERSION 3.19)
project(cs2glaz LANGUAGES CXX C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(JsonUtil)
include(CompilerSettings)
include(Protobuf)
include(Package)

# -----------------------------------------------------------------------------
# Platform / toolchain checks (MMSPluginConfig.__init__ in AMBuildScript)
# -----------------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "cs2glaz only builds for x86_64.")
endif()

if(WIN32)
  if(NOT MSVC)
    message(FATAL_ERROR "The Windows build needs the Microsoft C++ compiler.")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The Linux build needs Clang.")
  endif()
else()
  message(FATAL_ERROR "cs2glaz only builds for Windows or Linux.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF) # -fPIC added manually, as before

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# windows.h must not leak min/max macros (they break std::numeric_limits<>::max,
# which is what caused C2589 / C4003 "max" in bvh8_format.cpp).
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

# Static MSVC runtime, configured globally BEFORE any target is created, so CMake
# does not fight a manual /MT (this removes the D9025 "/MD overridden by /MT" spam).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# -----------------------------------------------------------------------------
# Required dependency directories (requireDirectory())
# -----------------------------------------------------------------------------
set(CS2GLAZ_SDK_ROOT         "${CMAKE_SOURCE_DIR}/hl2sdk-cs2")
set(CS2GLAZ_MMS_ROOT         "${CMAKE_SOURCE_DIR}/metamod-source")
set(CS2GLAZ_MANIFESTS_ROOT   "${CS2GLAZ_MMS_ROOT}/hl2sdk-manifests/manifests")
set(CS2GLAZ_FUNCHOOK_ROOT    "${CMAKE_SOURCE_DIR}/vendor/funchook")
set(CS2GLAZ_THIRD_PARTY_ROOT "${CMAKE_SOURCE_DIR}/third_party")

foreach(_pair
    "CS2GLAZ_SDK_ROOT|CS2 SDK"
    "CS2GLAZ_MMS_ROOT|Metamod:Source"
    "CS2GLAZ_MANIFESTS_ROOT|HL2SDK manifests"
    "CS2GLAZ_FUNCHOOK_ROOT|funchook"
    "CS2GLAZ_THIRD_PARTY_ROOT|third_party")
  string(REPLACE "|" ";" _pair_list "${_pair}")
  list(GET _pair_list 0 _var)
  list(GET _pair_list 1 _desc)
  if(NOT IS_DIRECTORY "${${_var}}")
    message(FATAL_ERROR "${_desc} is missing at ${${_var}}.")
  endif()
endforeach()

# Vendored sources that are part of the plugin must exist (mirrors AMBuilder).
if(NOT EXISTS "${CS2GLAZ_THIRD_PARTY_ROOT}/masked_occlusion_culling/MaskedOcclusionCulling.h")
  message(FATAL_ERROR "MaskedOcclusionCulling is missing at ${CS2GLAZ_THIRD_PARTY_ROOT}/masked_occlusion_culling.")
endif()
if(NOT EXISTS "${CS2GLAZ_THIRD_PARTY_ROOT}/masked_occlusion_culling/MaskedOcclusionCulling.cpp")
  message(FATAL_ERROR "MaskedOcclusionCulling.cpp is missing at ${CS2GLAZ_THIRD_PARTY_ROOT}/masked_occlusion_culling.")
endif()
if(NOT EXISTS "${CS2GLAZ_THIRD_PARTY_ROOT}/miniz.c")
  message(FATAL_ERROR "miniz.c is missing at ${CS2GLAZ_THIRD_PARTY_ROOT}/miniz.c.")
endif()

# -----------------------------------------------------------------------------
# Plugin metadata (MMSPluginConfig.loadMetadata())
# -----------------------------------------------------------------------------
file(READ "${CMAKE_SOURCE_DIR}/plugin-metadata.json" _cs2glaz_metadata_json)
foreach(_key name alias display_name description author url log_tag license version)
  cs2glaz_json_string("${_cs2glaz_metadata_json}" _value ${_key})
  if(NOT DEFINED _value)
    message(FATAL_ERROR "plugin-metadata.json is missing: ${_key}.")
  endif()
  string(TOUPPER "${_key}" _key_upper)
  set(CS2GLAZ_META_${_key_upper} "${_value}")
  unset(_value)
endforeach()

if(NOT CS2GLAZ_META_NAME STREQUAL "cs2glaz" OR NOT CS2GLAZ_META_ALIAS STREQUAL "cs2glaz")
  message(FATAL_ERROR "The plugin name and alias must both stay cs2glaz.")
endif()
if(NOT CS2GLAZ_META_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "The CS2GLAZ version must use major.minor.patch format.")
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_gen.h.in"
  "${CMAKE_BINARY_DIR}/versioning/version_gen.h"
  @ONLY
)

# -----------------------------------------------------------------------------
# SDK manifest cs2.json (SdkTarget / SdkHelpers.configureCxx)
# -----------------------------------------------------------------------------
file(READ "${CS2GLAZ_MANIFESTS_ROOT}/cs2.json" _cs2glaz_cs2_manifest_json)

cs2glaz_json_string("${_cs2glaz_cs2_manifest_json}" CS2GLAZ_SDK_CODE code)
cs2glaz_json_string_array("${_cs2glaz_cs2_manifest_json}" _cs2glaz_sdk_include_paths include_paths)
set(CS2GLAZ_SDK_INCLUDE_DIRS "")
foreach(_p ${_cs2glaz_sdk_include_paths})
  list(APPEND CS2GLAZ_SDK_INCLUDE_DIRS "${CS2GLAZ_SDK_ROOT}/${_p}")
endforeach()

cs2glaz_json_string_array("${_cs2glaz_cs2_manifest_json}" CS2GLAZ_SDK_LINUX_DEFINES linux defines)
cs2glaz_json_string_array("${_cs2glaz_cs2_manifest_json}" _cs2glaz_sdk_linux_postlink_libs linux x86_64 postlink_libs)
cs2glaz_json_string_array("${_cs2glaz_cs2_manifest_json}" _cs2glaz_sdk_linux_dynamic_libs linux x86_64 dynamic_libs)
cs2glaz_json_string("${_cs2glaz_cs2_manifest_json}" CS2GLAZ_SDK_LINUX_PROTOC_PATH linux protoc_path)

cs2glaz_json_string_array("${_cs2glaz_cs2_manifest_json}" _cs2glaz_sdk_windows_libs windows x86_64 libs)
cs2glaz_json_string("${_cs2glaz_cs2_manifest_json}" CS2GLAZ_SDK_WINDOWS_PROTOC_PATH windows protoc_path)

# postlink_libs / dynamic_libs / libs in the manifest are relative to sdk['path'].
set(CS2GLAZ_SDK_LINUX_POSTLINK_LIBS "")
foreach(_lib ${_cs2glaz_sdk_linux_postlink_libs})
  list(APPEND CS2GLAZ_SDK_LINUX_POSTLINK_LIBS "${CS2GLAZ_SDK_ROOT}/${_lib}")
endforeach()
set(CS2GLAZ_SDK_LINUX_DYNAMIC_LIBS "")
foreach(_lib ${_cs2glaz_sdk_linux_dynamic_libs})
  list(APPEND CS2GLAZ_SDK_LINUX_DYNAMIC_LIBS "${CS2GLAZ_SDK_ROOT}/${_lib}")
endforeach()
set(CS2GLAZ_SDK_WINDOWS_LIBS "")
foreach(_lib ${_cs2glaz_sdk_windows_libs})
  list(APPEND CS2GLAZ_SDK_WINDOWS_LIBS "${CS2GLAZ_SDK_ROOT}/${_lib}")
endforeach()

# SE_<ENGINE>=<code> for every manifest in hl2sdk-manifests/manifests.
file(GLOB _cs2glaz_all_manifests "${CS2GLAZ_MANIFESTS_ROOT}/*.json")
set(CS2GLAZ_SE_DEFINES "")
foreach(_manifest ${_cs2glaz_all_manifests})
  file(READ "${_manifest}" _m_json)
  cs2glaz_json_string("${_m_json}" _m_define define)
  cs2glaz_json_string("${_m_json}" _m_code code)
  list(APPEND CS2GLAZ_SE_DEFINES "SE_${_m_define}=${_m_code}")
  unset(_m_define)
  unset(_m_code)
endforeach()

# -----------------------------------------------------------------------------
# cs2glaz-protobuf: static lib with generated protobuf sources (createProtobufLibrary()).
# NOTE: convar.cpp / memoverride.cpp are deliberately NOT part of this target.
# -----------------------------------------------------------------------------
cs2glaz_generate_protobuf(CS2GLAZ_PROTOBUF_SOURCES CS2GLAZ_PROTOBUF_GEN_DIR)

add_library(cs2glaz-protobuf STATIC ${CS2GLAZ_PROTOBUF_SOURCES})
add_dependencies(cs2glaz-protobuf cs2glaz-protobuf-gen)
cs2glaz_configure_target(cs2glaz-protobuf)
target_include_directories(cs2glaz-protobuf PRIVATE "${CS2GLAZ_PROTOBUF_GEN_DIR}")

# -----------------------------------------------------------------------------
# cs2glaz: the plugin module (AMBuilder "plugin" target).
#
# Explicit source list, identical to AMBuilder plugin.sources. This is intentional:
# a GLOB over src/ would pull in src/baker/* (main.cpp with int main(), and
# glb_import.cpp which needs cgltf) and src/core/builder.cpp - none of which
# belong in the plugin. convar.cpp / memoverride.cpp live here (and only here).
# -----------------------------------------------------------------------------
set(CS2GLAZ_PLUGIN_SOURCES
  # src/core
  "${CMAKE_SOURCE_DIR}/src/core/bvh8.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/bvh8_format.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/capsule_visibility.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/decoy_logic.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/dynamic_occluders.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/masked_occlusion_stubs.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/map_source.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/signature_scan.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/subprocess.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/smoke_occlusion.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/visibility_sampling.cpp"
  "${CMAKE_SOURCE_DIR}/src/core/vpk.cpp"

  # src/plugin
  "${CMAKE_SOURCE_DIR}/src/plugin/automatic_baker.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/bridge.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/decoys.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/front_decoys.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/selftest.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/game_state.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/ghosts.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/journal.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/occluders.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/phantoms.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/plugin.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/radar_filter.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/runtime_compatibility.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/settings.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/transmit.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/updater.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/visibility_worker.cpp"
  "${CMAKE_SOURCE_DIR}/src/plugin/diagnostics.cpp"

  # third_party (vendored, part of the plugin)
  "${CS2GLAZ_THIRD_PARTY_ROOT}/miniz.c"
  "${CS2GLAZ_THIRD_PARTY_ROOT}/masked_occlusion_culling/MaskedOcclusionCulling.cpp"

  # SDK translation units compiled into the plugin
  "${CS2GLAZ_SDK_ROOT}/tier1/convar.cpp"
  "${CS2GLAZ_SDK_ROOT}/public/tier0/memoverride.cpp"
)

add_library(cs2glaz MODULE ${CS2GLAZ_PLUGIN_SOURCES})
add_dependencies(cs2glaz cs2glaz-protobuf-gen)
cs2glaz_configure_target(cs2glaz)

set_target_properties(cs2glaz PROPERTIES
  PREFIX ""
  OUTPUT_NAME "cs2glaz"
)

target_compile_definitions(cs2glaz PRIVATE
  MINIZ_NO_ARCHIVE_WRITING_APIS
  MINIZ_NO_ZLIB_APIS
  CS2GLAZ_VERSION="${CS2GLAZ_META_VERSION}"
)

# Include dirs mirror AMBuilder configure() + plugin.compiler.cxxincludes.
# src/baker is kept as an include dir (AMBuilder adds it for every target), but
# no baker .cpp is compiled here, so cgltf is NOT required by the plugin.
target_include_directories(cs2glaz PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/core"
  "${CMAKE_SOURCE_DIR}/src/baker"
  "${CMAKE_SOURCE_DIR}/src/plugin"

  "${CS2GLAZ_THIRD_PARTY_ROOT}"

  "${CS2GLAZ_SDK_ROOT}"
  "${CS2GLAZ_SDK_ROOT}/public/entity2"

  "${CS2GLAZ_FUNCHOOK_ROOT}/include"
  "${CS2GLAZ_PROTOBUF_GEN_DIR}"
)

if(EXISTS "${CS2GLAZ_THIRD_PARTY_ROOT}/generated")
  target_include_directories(cs2glaz PRIVATE "${CS2GLAZ_THIRD_PARTY_ROOT}/generated")
endif()

target_link_libraries(cs2glaz PRIVATE cs2glaz-protobuf)

if(WIN32)
  set(_windows_sdk_roots
    "$ENV{WindowsSdkDir}Lib"
    "$ENV{WindowsSdkDir}Lib/$ENV{WindowsSDKVersion}"
    "C:/Program Files (x86)/Windows Kits/10/Lib"
    "C:/Program Files/Windows Kits/10/Lib"
    "D:/Windows Kits/10/Lib"
  )
  find_library(PSAPI_LIB NAMES psapi
    HINTS ${_windows_sdk_roots}
    PATH_SUFFIXES
      "10.0.26100.0/um/x64"
      "10.0.26100.0/um/x86"
      "10.0.26100.0/um/arm64"
      "um/x64"
      "um/x86"
      "um/arm64"
  )
  if(NOT PSAPI_LIB)
    message(FATAL_ERROR "psapi.lib was not found in the installed Windows SDK. Install the Windows 10/11 SDK and retry.")
  endif()

  set(_msvc_delayimp_roots
    "D:/VisualStudio2026/VC/Tools/MSVC"
    "C:/Program Files/Microsoft Visual Studio"
    "C:/Program Files (x86)/Microsoft Visual Studio"
  )
  set(DELAYIMP_LIB "")
  foreach(_msvc_root ${_msvc_delayimp_roots})
    if(EXISTS "${_msvc_root}")
      file(GLOB _msvc_delayimp_candidates "${_msvc_root}/*/lib/x64/delayimp.lib" "${_msvc_root}/*/lib/x86/delayimp.lib" "${_msvc_root}/*/lib/arm64/delayimp.lib")
      if(_msvc_delayimp_candidates)
        list(GET _msvc_delayimp_candidates 0 DELAYIMP_LIB)
        break()
      endif()
    endif()
  endforeach()
  if(NOT DELAYIMP_LIB)
    find_library(DELAYIMP_LIB NAMES delayimp
      HINTS ${_msvc_delayimp_roots}
      PATH_SUFFIXES "*/lib/x64" "*/lib/x86" "*/lib/arm64"
    )
  endif()
  if(NOT DELAYIMP_LIB)
    message(FATAL_ERROR "delayimp.lib was not found in the installed MSVC toolchain. Install the C++ build tools and retry.")
  endif()

  target_link_libraries(cs2glaz PRIVATE
    ${CS2GLAZ_SDK_WINDOWS_LIBS}                                        # protobuf/mathlib/tier0/interfaces from cs2.json
    "${PSAPI_LIB}"
    "${CS2GLAZ_FUNCHOOK_ROOT}/lib/funchook.lib"
    "${CS2GLAZ_FUNCHOOK_ROOT}/lib/distorm.lib"
    "${CS2GLAZ_SDK_ROOT}/lib/public/win64/mathlib.lib"
    "${CS2GLAZ_SDK_ROOT}/lib/public/win64/steam_api64.lib"
    "${DELAYIMP_LIB}"
  )
  target_link_options(cs2glaz PRIVATE "/DELAYLOAD:steam_api64.dll")
else()
  target_link_libraries(cs2glaz PRIVATE
    ${CS2GLAZ_SDK_LINUX_POSTLINK_LIBS}                                 # mathlib.a / interfaces.a / libprotobuf.a
    ${CS2GLAZ_SDK_LINUX_DYNAMIC_LIBS}                                  # libtier0.so
    "${CS2GLAZ_FUNCHOOK_ROOT}/lib/libfunchook.a"
    "${CS2GLAZ_FUNCHOOK_ROOT}/lib/libdistorm.a"
    "${CS2GLAZ_SDK_ROOT}/lib/linux64/mathlib.a"
  )
endif()

# -----------------------------------------------------------------------------
# Packaging (PackageScript)
# -----------------------------------------------------------------------------
cs2glaz_add_package(cs2glaz)