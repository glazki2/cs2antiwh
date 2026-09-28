# vim: set sts=2 ts=2 sw=2 et:
#
# Common compiler/target configuration shared by cs2glaz-protobuf and cs2glaz.
# Mirrors AMBuildScript + SdkHelpers.configureCxx.

function(cs2glaz_configure_target target)
  target_compile_features(${target} PRIVATE cxx_std_20)

  target_compile_definitions(${target} PRIVATE
    ${CS2GLAZ_SE_DEFINES}
    SOURCE_ENGINE=${CS2GLAZ_SDK_CODE}
    GAME_DLL
    RAD_TELEMETRY_DISABLED
    X64BITS
    PLATFORM_64BITS
    META_IS_SOURCE2
  )

  target_include_directories(${target} PRIVATE
    "${CMAKE_BINARY_DIR}/versioning"
    "${CMAKE_SOURCE_DIR}"
    ${CS2GLAZ_SDK_INCLUDE_DIRS}
    "${CS2GLAZ_MMS_ROOT}/core"
    "${CS2GLAZ_MMS_ROOT}/third_party/khook/include"
  )

  # IMPORTANT: convar.cpp / memoverride.cpp are NOT added here. They are listed
  # explicitly in the cs2glaz plugin target only. Putting them here would also
  # compile them into cs2glaz-protobuf, and since the plugin links that static
  # lib, the linker would see duplicate definitions (LNK2005).

  if(MSVC)
    target_compile_definitions(${target} PRIVATE
      _CRT_SECURE_NO_DEPRECATE
      _CRT_SECURE_NO_WARNINGS
      _CRT_NONSTDC_NO_DEPRECATE
      _HAS_EXCEPTIONS=0
      _ITERATOR_DEBUG_LEVEL=0
      WIN32
      _WINDOWS
      COMPILER_MSVC
      COMPILER_MSVC64
      WIN64
    )

    # No /MT here: the static CRT is selected globally via
    # CMAKE_MSVC_RUNTIME_LIBRARY in CMakeLists.txt. Adding /MT on top of CMake's
    # default /MD produced "D9025: overriding /MD with /MT" warnings.
    target_compile_options(${target} PRIVATE
      /W3 /WX
      /wd4005 /wd4018 /wd4099 /wd4146 /wd4244 /wd4267 /wd5033 /wd5048
      /std:c++20 /Oy-
      /experimental:deterministic
      /pathmap:${CMAKE_SOURCE_DIR}=cs2glaz
    )

    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/TP>)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      target_compile_definitions(${target} PRIVATE DEBUG _DEBUG)
      target_compile_options(${target} PRIVATE /Od)
    else()
      target_compile_options(${target} PRIVATE /O2)
    endif()
  else()
    # Clang / GCC (Linux).
    target_compile_definitions(${target} PRIVATE
      ${CS2GLAZ_SDK_LINUX_DEFINES}
      _GNU_SOURCE
    )

    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
      -fno-exceptions -fno-rtti
      -msse4.1 -fno-strict-aliasing -pthread
      -fPIC -fvisibility=hidden
    )

    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
      $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-delete-non-virtual-dtor>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-non-virtual-dtor>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-overloaded-virtual>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-offsetof>
      $<$<COMPILE_LANGUAGE:CXX>:-Wno-parentheses>
    )

    # NOTE: -fuse-ld=lld requires the lld package in the container
    # (apt-get install -y lld). Without it the link step fails with
    # "invalid linker name in argument '-fuse-ld=lld'".
    target_link_options(${target} PRIVATE
      -lm -fuse-ld=lld -Wl,--no-gnu-unique -static-libstdc++ -lgcc_eh -pthread
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      target_compile_definitions(${target} PRIVATE DEBUG _DEBUG)
      target_compile_options(${target} PRIVATE -O0 -g3)
    else()
      target_compile_definitions(${target} PRIVATE NDEBUG)
      target_compile_options(${target} PRIVATE -O3)
      target_link_options(${target} PRIVATE -Wl,-s)
    endif()
  endif()
endfunction()