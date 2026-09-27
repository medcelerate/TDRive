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
    list(APPEND TDRIVE_SOURCES src/backend_d3d11.cpp src/cuda_interop_win.cpp)
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

# -----------------------------------------------------------------------------
# CPython - the schema endpoint (pythonGetSets) exposed on the node.
# -----------------------------------------------------------------------------
# We build against the Python SDK TouchDesigner ships with itself, so there is
# no external Python dependency. Two things matter here:
#
#   * Py_LIMITED_API + python3.lib = the PEP 384 stable ABI. That binds the
#     plugin to python3.dll rather than python311.dll, so it is not hostage to
#     a single CPython minor release. Everything the schema getter needs
#     (PyGetSetDef, PyDict_*, PyUnicode_FromString, PyFloat_FromDouble,
#     PyBool_FromLong, PyList_*) lives inside the limited API.
#   * customOPInfo.pythonVersion is a separate, softer gate TD checks at load
#     time - see kTDRivePythonVersion in TDRiveTOP.cpp.
#
# Point TD_PYTHON_ROOT at a Python SDK root to override the auto-discovery
# below. Two root layouts are accepted, because CI has no TouchDesigner
# install to borrow the SDK from:
#
#   TD's bundled SDK   <root>/Include/Python.h  + Include/PC/  + lib/x64/python3.lib
#   a stock CPython    <root>/include/Python.h                 + libs/python3.lib
#
# The second is what actions/setup-python lays down, so CI points
# TD_PYTHON_ROOT straight at $env:pythonLocation.
#
# Failing to resolve a root is FATAL, not a warning. A warning here still
# emits a perfectly loadable plugin with the entire Python block compiled
# out - no pythonGetSets, so TD builds no Python class on the node and
# `op('rive1').propertySchema` raises AttributeError. That is exactly how a
# schema-less DLL once got tagged and shipped: the CI runner has no
# TouchDesigner, the glob below found nothing, and the build went green.
# If you genuinely want the plugin without the endpoint, say so out loud
# with -DTDRIVE_PYTHON=OFF.
option(TDRIVE_PYTHON "Expose the Rive property schema through CPython" ON)

if(TDRIVE_PYTHON AND WIN32)
    if(NOT TD_PYTHON_ROOT)
        file(GLOB _td_py_roots
             "C:/Program Files/Derivative/TouchDesigner.*/Samples/CPlusPlus/3rdParty/Python")
        if(_td_py_roots)
            list(SORT _td_py_roots)
            list(REVERSE _td_py_roots)          # newest install first
            list(GET _td_py_roots 0 TD_PYTHON_ROOT)
        endif()
    endif()

    # Resolve headers and import library separately: the two layouts agree on
    # neither directory name, and python3.lib is the gate that actually
    # decides whether this links.
    set(_tdrive_py_inc "")
    set(_tdrive_py_lib "")
    if(TD_PYTHON_ROOT)
        foreach(_cand "Include" "include")
            if(EXISTS "${TD_PYTHON_ROOT}/${_cand}/Python.h")
                set(_tdrive_py_inc "${TD_PYTHON_ROOT}/${_cand}")
                break()
            endif()
        endforeach()
        foreach(_cand "lib/x64" "libs")
            if(EXISTS "${TD_PYTHON_ROOT}/${_cand}/python3.lib")
                set(_tdrive_py_lib "${TD_PYTHON_ROOT}/${_cand}")
                break()
            endif()
        endforeach()
    endif()

    if(_tdrive_py_inc AND _tdrive_py_lib)
        message(STATUS "TDRive: CPython schema endpoint ON")
        message(STATUS "TDRive:   headers     ${_tdrive_py_inc}")
        message(STATUS "TDRive:   python3.lib ${_tdrive_py_lib}")
        target_include_directories(TDRiveTOP PRIVATE "${_tdrive_py_inc}")
        # TD's SDK keeps the Windows pyconfig.h under Include/PC (it ships the
        # CPython source tree's layout); an installed CPython has it beside
        # Python.h and has no PC directory at all.
        if(EXISTS "${_tdrive_py_inc}/PC/pyconfig.h")
            target_include_directories(TDRiveTOP PRIVATE "${_tdrive_py_inc}/PC")
        endif()
        # pyconfig.h emits `#pragma comment(lib, "python3.lib")` once
        # Py_LIMITED_API is defined, so the search path above is all the linker
        # needs - naming the lib again here would be redundant.
        target_link_directories(TDRiveTOP PRIVATE "${_tdrive_py_lib}")
        target_compile_definitions(TDRiveTOP PRIVATE
            TDRIVE_PYTHON
            Py_LIMITED_API=0x030B0000   # 3.11, the floor TD 2023+ ships
        )
    else()
        set(_tdrive_py_why "no TD_PYTHON_ROOT could be found")
        if(TD_PYTHON_ROOT)
            set(_tdrive_py_why "TD_PYTHON_ROOT=${TD_PYTHON_ROOT} is missing")
            if(NOT _tdrive_py_inc)
                string(APPEND _tdrive_py_why " Include/Python.h (or include/Python.h)")
            endif()
            if(NOT _tdrive_py_lib)
                if(NOT _tdrive_py_inc)
                    string(APPEND _tdrive_py_why " and")
                endif()
                string(APPEND _tdrive_py_why " lib/x64/python3.lib (or libs/python3.lib)")
            endif()
        endif()
        message(FATAL_ERROR
            "TDRIVE_PYTHON is ON but no usable Python SDK was found: "
            "${_tdrive_py_why}.\n"
            "Searched for a TouchDesigner install at "
            "C:/Program Files/Derivative/TouchDesigner.*/Samples/CPlusPlus/3rdParty/Python.\n"
            "Fix by pointing -DTD_PYTHON_ROOT=<path> at either a TD Python SDK "
            "or a stock CPython 3.11 install, or build without the schema "
            "endpoint on purpose with -DTDRIVE_PYTHON=OFF.\n"
            "This is fatal by design - see the comment above. A silent "
            "downgrade here ships a plugin whose Python attributes are simply "
            "absent, which is indistinguishable from a broken install.")
    endif()
elseif(TDRIVE_PYTHON AND APPLE)
    # macOS needs headers only. TD bundles CPython as an @rpath Python.framework
    # and each TD build ships a different 3.11.x, so rather than link one, the
    # Python symbols are left undefined and bound at load time to the
    # interpreter TD already has loaded.
    #
    # Each allowed symbol is named with -U rather than using a blanket
    # -undefined dynamic_lookup: that would hide a genuinely missing Rive or
    # system symbol until TD loads the plugin, and older ld (macos-14 runner)
    # also stops binding _objc_msgSend to libobjc under it. If you call a new
    # CPython function, the link fails naming it - add it to this list.
    # scripts/verify_python_endpoint.py re-checks the artifact.
    #
    # Accepted roots (TD_PYTHON_ROOT overrides the auto-discovery):
    #   TD's bundle     .../Python.framework/Versions/3.11  (include/python3.11/)
    #   a stock CPython <root>/include/python3.11/Python.h  (actions/setup-python)
    if(NOT TD_PYTHON_ROOT)
        file(GLOB _td_py_roots
             "/Applications/TouchDesigner*.app/Contents/Frameworks/Python.framework/Versions/3.11")
        if(_td_py_roots)
            list(GET _td_py_roots 0 TD_PYTHON_ROOT)
        endif()
    endif()

    set(_tdrive_py_inc "")
    if(TD_PYTHON_ROOT)
        foreach(_cand "include/python3.11" "Headers" "include")
            if(EXISTS "${TD_PYTHON_ROOT}/${_cand}/Python.h")
                set(_tdrive_py_inc "${TD_PYTHON_ROOT}/${_cand}")
                break()
            endif()
        endforeach()
    endif()

    if(_tdrive_py_inc)
        message(STATUS "TDRive: CPython schema endpoint ON")
        message(STATUS "TDRive:   headers ${_tdrive_py_inc}")
        target_include_directories(TDRiveTOP PRIVATE "${_tdrive_py_inc}")
        set(_tdrive_py_symbols
            PyBool_FromLong PyDict_New PyDict_SetItemString PyFloat_FromDouble
            PyList_Append PyList_New PyLong_FromLong PyUnicode_FromString
            _Py_Dealloc)
        foreach(_sym IN LISTS _tdrive_py_symbols)
            target_link_options(TDRiveTOP PRIVATE "LINKER:-U,_${_sym}")
        endforeach()
        target_compile_definitions(TDRiveTOP PRIVATE
            TDRIVE_PYTHON
            Py_LIMITED_API=0x030B0000
        )
    else()
        message(FATAL_ERROR
            "TDRIVE_PYTHON is ON but no Python 3.11 headers were found "
            "(TD_PYTHON_ROOT=${TD_PYTHON_ROOT}).\n"
            "Searched /Applications/TouchDesigner*.app/Contents/Frameworks/"
            "Python.framework/Versions/3.11.\n"
            "Fix by pointing -DTD_PYTHON_ROOT=<path> at a TD Python.framework "
            "version dir or a stock CPython 3.11 install, or build without the "
            "schema endpoint on purpose with -DTDRIVE_PYTHON=OFF.")
    endif()
endif()

# The Rive static libs are built with RTTI disabled (they use an internal
# "lite rtti" scheme). Any rive-derived class we instantiate in plugin TUs
# (e.g. RiveRenderImage) would otherwise emit typeinfo references to symbols
# the libs don't export. Match their flags.
if(MSVC)
    target_compile_options(TDRiveTOP PRIVATE /GR-)
else()
    target_compile_options(TDRiveTOP PRIVATE -fno-rtti)
endif()

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
        # EnumProcessModules/GetModuleBaseName for the cudart runtime loader.
        psapi
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

# -----------------------------------------------------------------------------
# Texture injection round-trip test: drives the platform backend (Metal, or
# D3D11 + the CUDA path on Windows) against tests/texture_io/texture_io.riv.
#   ctest --test-dir build [-C Release]
#   build/texture_io_test tests/texture_io/texture_io.riv [cpu|cuda|all]
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/texture_io/texture_io_test.cpp")
    if(APPLE)
        add_executable(texture_io_test
            tests/texture_io/texture_io_test.cpp
            src/backend_metal.mm)
        # -fno-rtti to match the plugin and the Rive libs (see the note above
        # the TDRiveTOP flags); the backends subclass rive types.
        target_compile_options(texture_io_test PRIVATE
            -fno-rtti
            $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
        target_link_libraries(texture_io_test PRIVATE
            ${RIVE_LIBS}
            "-framework Foundation"
            "-framework CoreFoundation"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework ImageIO"
        )
    elseif(WIN32)
        add_executable(texture_io_test
            tests/texture_io/texture_io_test.cpp
            src/backend_d3d11.cpp
            src/cuda_interop_win.cpp)
        target_compile_options(texture_io_test PRIVATE /GR-)
        target_compile_definitions(texture_io_test PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(texture_io_test PRIVATE
            ${RIVE_LIBS} d3d11 dxgi dxguid d3dcompiler psapi)
        set_property(TARGET texture_io_test PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    target_include_directories(texture_io_test PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${RIVE_DIR}/include"
        "${RIVE_DIR}/renderer/include"
    )
    enable_testing()
    add_test(NAME texture_io
             COMMAND texture_io_test "${CMAKE_SOURCE_DIR}/tests/texture_io/texture_io.riv")
endif()
