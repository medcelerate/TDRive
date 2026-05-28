# TDRive: TouchDesigner Custom TOP for rendering .riv files.
#
# Usage:
#   # 1. Build the Rive runtime once (this is slow and not driven by CMake):
#   #    macOS:   ./scripts/build_rive.sh release
#   #    Windows: scripts\build_rive.bat release
#   # 2. Configure + build the plugin:
#   #    cmake -B build -DCMAKE_BUILD_TYPE=Release
#   #    cmake --build build --config Release
#
# Outputs:
#   macOS:   build/TDRiveTOP.plugin    (bundle)
#   Windows: build/Release/TDRiveTOP.dll

cmake_minimum_required(VERSION 3.20)
project(TDRiveTOP LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

# -----------------------------------------------------------------------------
# Rive runtime - prebuilt static libs produced by scripts/build_rive.{sh,bat}.
# -----------------------------------------------------------------------------
# The Rive build script writes its outputs into per-config subdirectories
# under third_party/rive-runtime/renderer/out/<config>/. We pick the matching
# CMAKE_BUILD_TYPE (or Release by default for multi-config generators).
set(RIVE_DIR "${CMAKE_SOURCE_DIR}/third_party/rive-runtime")

if(CMAKE_CONFIGURATION_TYPES)
    # Multi-config generator (Visual Studio, Xcode). Use a generator
    # expression so each config picks its own Rive output directory.
    set(RIVE_OUT_DIR
        "${RIVE_DIR}/renderer/out/$<IF:$<CONFIG:Debug>,debug,release>")
else()
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _rive_cfg)
    if(_rive_cfg STREQUAL "")
        set(_rive_cfg "release")
    endif()
    set(RIVE_OUT_DIR "${RIVE_DIR}/renderer/out/${_rive_cfg}")
endif()

# Glob all the static libs the Rive renderer build produces (librive,
# librive_pls_renderer, librive_decoders, librive_harfbuzz, librive_sheenbidi,
# librive_yoga, libpng, libjpeg, libwebp, libzlib, libluau_vm, libminiaudio,
# libglfw3, ...). We don't enumerate by name because the set shifts whenever
# Rive's premake config changes.
if(WIN32)
    file(GLOB RIVE_LIBS "${RIVE_DIR}/renderer/out/release/*.lib")
else()
    file(GLOB RIVE_LIBS "${RIVE_OUT_DIR}/*.a")
endif()

# Don't hard-fail at configure time on multi-config generators - the glob runs
# before the build directory exists. We emit a warning instead.
if(NOT RIVE_LIBS AND NOT CMAKE_CONFIGURATION_TYPES)
    message(WARNING
        "No Rive static libs found under ${RIVE_OUT_DIR}.\n"
        "Run scripts/build_rive.sh (or build_rive.bat) first.")
endif()

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------
set(TDRIVE_SOURCES
    src/TDRiveTOP.cpp
)

if(APPLE)
    list(APPEND TDRIVE_SOURCES src/backend_metal.mm)
elseif(WIN32)
    list(APPEND TDRIVE_SOURCES src/backend_d3d11.cpp)
else()
    message(FATAL_ERROR "Unsupported platform - macOS and Windows only.")
endif()

# -----------------------------------------------------------------------------
# Plugin target
# -----------------------------------------------------------------------------
if(APPLE)
    # MODULE = loadable bundle, which is what TD wants for a Custom Operator.
    add_library(TDRiveTOP MODULE ${TDRIVE_SOURCES})
    set_target_properties(TDRiveTOP PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/Info.plist"
        XCODE_ATTRIBUTE_WRAPPER_EXTENSION "plugin"
        SUFFIX ""
    )
elseif(WIN32)
    add_library(TDRiveTOP SHARED ${TDRIVE_SOURCES})
    set_target_properties(TDRiveTOP PROPERTIES PREFIX "")
endif()

target_include_directories(TDRiveTOP PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/td_sdk"
    "${RIVE_DIR}/include"
    "${RIVE_DIR}/renderer/include"
)

target_link_libraries(TDRiveTOP PRIVATE ${RIVE_LIBS})

if(APPLE)
    # ObjC++ ARC for backend_metal.mm.
    target_compile_options(TDRiveTOP PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(TDRiveTOP PRIVATE
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
    )
elseif(WIN32)
    target_compile_definitions(TDRiveTOP PRIVATE
        # NOMINMAX is already defined by td_sdk/CPlusPlus_Common.hpp on _WIN32.
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(TDRiveTOP PRIVATE
        d3d11
        dxgi
        dxguid
        d3dcompiler
    )
    # Statically link the MSVC runtime to match how the Rive libs are built.
    set_property(TARGET TDRiveTOP PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# -----------------------------------------------------------------------------
# Optional: rivinfo-style inspector (scripts/inspect_riv.mm on macOS only).
# -----------------------------------------------------------------------------
if(APPLE AND EXISTS "${CMAKE_SOURCE_DIR}/scripts/inspect_riv.mm")
    add_executable(inspect_riv scripts/inspect_riv.mm)
    set_target_properties(inspect_riv PROPERTIES
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
    target_include_directories(inspect_riv PRIVATE
        "${CMAKE_SOURCE_DIR}/td_sdk"
        "${RIVE_DIR}/include"
        "${RIVE_DIR}/renderer/include"
    )
    target_compile_options(inspect_riv PRIVATE -fobjc-arc)
    target_link_libraries(inspect_riv PRIVATE
        ${RIVE_LIBS}
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
    )
endif()
