cmake_minimum_required(VERSION 3.16...3.26)

# These MUST be set before project(): project() initialises the cache entries
# and configures the toolchain, after which a later set(... CACHE ...) is a
# silent no-op and the binary inherits the build host's SDK (minos 26.0 here),
# refusing to load on older systems. libobs and obs-frontend-api.dylib are
# arm64-only, so a universal build cannot link them either.
#
# 13.0 is the real floor for an OBS 32.2.1 Qt plugin, measured with vtool:
# OBS.app declares LSMinimumSystemVersion 12.0 and libobs/obs-frontend-api are
# minos 12.0, but the bundled QtCore/QtGui/QtWidgets are minos 13.0. Targeting
# anything lower only produces "built for newer version 13.0" linker warnings
# and a bundle that still cannot load Qt below 13.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures")

project(obs-icecast VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── OBS SDK ──────────────────────────────────────────────────────────
# Try system-installed cmake config first (from an OBS build tree).
# If not found, download OBS headers and link against the app bundle.
find_package(libobs QUIET)

if(NOT libobs_FOUND)
    message(STATUS "libobs cmake config not found — downloading OBS headers")

    set(OBS_VERSION "32.2.1" CACHE STRING "OBS version to fetch headers for")

    include(FetchContent)
    FetchContent_Declare(obs-src
        URL "https://github.com/obsproject/obs-studio/archive/refs/tags/${OBS_VERSION}.tar.gz"
    )
    FetchContent_GetProperties(obs-src)
    if(NOT obs-src_POPULATED)
        FetchContent_Populate(obs-src)
    endif()

    # Generate minimal obsconfig.h (normally produced by OBS's cmake)
    file(WRITE "${obs-src_SOURCE_DIR}/libobs/obsconfig.h"
"#pragma once\n\
#define OBS_VERSION \"${OBS_VERSION}\"\n\
#define OBS_DATA_PATH \"../../data\"\n\
#define OBS_INSTALL_PREFIX \"\"\n\
#define OBS_PLUGIN_DESTINATION \"obs-plugins\"\n\
#define OBS_RELATIVE_PREFIX \"../../\"\n\
#define OBS_UNIX_STRUCTURE 0\n\
#define OBS_RELEASE_CANDIDATE 0\n\
#define OBS_BETA 0\n"
    )

    # Create imported libobs target
    add_library(OBS::libobs SHARED IMPORTED)

    if(APPLE)
        set(_libobs_framework "/Applications/OBS.app/Contents/Frameworks/libobs.framework")
        if(NOT EXISTS "${_libobs_framework}")
            message(FATAL_ERROR "OBS.app not found at /Applications/OBS.app — install OBS Studio first")
        endif()
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_LOCATION "${_libobs_framework}/Versions/A/libobs"
            INTERFACE_INCLUDE_DIRECTORIES "${obs-src_SOURCE_DIR}/libobs"
        )
    else()
        message(FATAL_ERROR "Auto-download only supports macOS. Set CMAKE_PREFIX_PATH to your OBS build.")
    endif()
endif()

# ── FFmpeg ───────────────────────────────────────────────────────────
# Use Homebrew FFmpeg for headers at compile time.
# At runtime, OBS's bundled FFmpeg is loaded via @rpath.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)

if(APPLE)
    set(_obs_frameworks "/Applications/OBS.app/Contents/Frameworks")

    # ── FFmpeg ABI guard ─────────────────────────────────────────────
    # We compile against Homebrew headers but link @rpath/libavcodec.dylib
    # from OBS.app. If the majors diverge, AVCodecContext/AVFrame offsets
    # silently disagree and corrupt memory. Fail loudly instead.
    file(STRINGS "${LIBAVCODEC_INCLUDE_DIRS}/libavcodec/version_major.h" _hdr
         REGEX "define LIBAVCODEC_VERSION_MAJOR")
    string(REGEX MATCH "[0-9]+" _hdr_major "${_hdr}")
    execute_process(COMMAND otool -L "${_obs_frameworks}/libavcodec.dylib"
                    OUTPUT_VARIABLE _obs_otool)
    string(REGEX MATCH "libavcodec\\.dylib \\(compatibility version ([0-9]+)"
           _ "${_obs_otool}")
    if(NOT _hdr_major STREQUAL "${CMAKE_MATCH_1}")
        message(FATAL_ERROR
            "FFmpeg major mismatch: headers are libavcodec ${_hdr_major} but "
            "OBS.app ships ${CMAKE_MATCH_1}. Install the matching Homebrew "
            "ffmpeg formula.")
    endif()
    message(STATUS "FFmpeg ABI check: libavcodec major ${_hdr_major} matches OBS")

    # ── simde ────────────────────────────────────────────────────────
    # On arm64 libobs/util/sse-intrin.h includes <simde/x86/sse2.h>. simde is
    # a git submodule in the OBS repo, so the release tarball we fetch does
    # not contain it and the headers must come from Homebrew.
    execute_process(COMMAND brew --prefix simde
                    OUTPUT_VARIABLE _simde_prefix
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT EXISTS "${_simde_prefix}/include/simde/x86/sse2.h")
        message(FATAL_ERROR
            "simde headers not found. libobs needs <simde/x86/sse2.h> on "
            "arm64; run: brew install simde")
    endif()
    set(_simde_include "${_simde_prefix}/include")
    message(STATUS "simde headers: ${_simde_include}")

    # ── Qt ───────────────────────────────────────────────────────────
    # Homebrew supplies headers and moc only. Linkage is against OBS's own
    # frameworks so the recorded install names stay @rpath/… — linking
    # Homebrew Qt would bake /opt/homebrew paths into the bundle and break
    # it on any machine without Homebrew.
    execute_process(COMMAND brew --prefix qt
                    OUTPUT_VARIABLE _qt_prefix
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND CMAKE_PREFIX_PATH "${_qt_prefix}")
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
    set(CMAKE_AUTOMOC ON)

    foreach(_qtlib Core Gui Widgets)
        add_library(obs-qt${_qtlib} SHARED IMPORTED)
        set_target_properties(obs-qt${_qtlib} PROPERTIES
            IMPORTED_LOCATION
                "${_obs_frameworks}/Qt${_qtlib}.framework/Versions/A/Qt${_qtlib}")
    endforeach()

    add_library(obs-frontend-api SHARED IMPORTED)
    set_target_properties(obs-frontend-api PROPERTIES
        IMPORTED_LOCATION "${_obs_frameworks}/obs-frontend-api.dylib")
endif()

# ── Plugin target ────────────────────────────────────────────────────
add_library(obs-icecast MODULE
    src/plugin-main.c
    src/mp3-encoder.c
    src/icecast-output.c
    src/icy-protocol.c
    src/radioco-dock.cpp
)

target_include_directories(obs-icecast PRIVATE src)

# Link FFmpeg headers only (compile-time), not the libraries.
# At runtime the plugin uses OBS's bundled FFmpeg via @rpath.
target_include_directories(obs-icecast SYSTEM PRIVATE
    ${LIBAVCODEC_INCLUDE_DIRS}
    ${LIBAVUTIL_INCLUDE_DIRS}
)

if(APPLE)
    # Create imported targets for OBS's bundled FFmpeg
    add_library(obs-avcodec SHARED IMPORTED)
    set_target_properties(obs-avcodec PROPERTIES
        IMPORTED_LOCATION "${_obs_frameworks}/libavcodec.dylib"
    )
    add_library(obs-avutil SHARED IMPORTED)
    set_target_properties(obs-avutil PROPERTIES
        IMPORTED_LOCATION "${_obs_frameworks}/libavutil.dylib"
    )

    # Qt headers from Homebrew; the libraries linked are OBS's own.
    # simde satisfies libobs's <simde/x86/sse2.h> include on arm64.
    target_include_directories(obs-icecast SYSTEM PRIVATE
        ${Qt6Core_INCLUDE_DIRS}
        ${Qt6Gui_INCLUDE_DIRS}
        ${Qt6Widgets_INCLUDE_DIRS}
        "${_simde_include}"
    )

    # obs-frontend-api.h moved between OBS majors (UI/obs-frontend-api/ up to
    # OBS 30, frontend/api/ at 32.2.1). Resolve it instead of hardcoding.
    if(DEFINED obs-src_SOURCE_DIR)
        file(GLOB_RECURSE _fe_hdr "${obs-src_SOURCE_DIR}/*/obs-frontend-api.h")
        if(NOT _fe_hdr)
            message(FATAL_ERROR "obs-frontend-api.h not found in fetched OBS source")
        endif()
        list(GET _fe_hdr 0 _fe_hdr0)
        get_filename_component(_fe_dir "${_fe_hdr0}" DIRECTORY)
        message(STATUS "obs-frontend-api.h: ${_fe_hdr0}")
        target_include_directories(obs-icecast SYSTEM PRIVATE "${_fe_dir}")
    endif()

    target_link_libraries(obs-icecast PRIVATE
        OBS::libobs
        obs-avcodec
        obs-avutil
        obs-qtCore
        obs-qtGui
        obs-qtWidgets
        obs-frontend-api
    )

    # Replace the absolute /Applications/OBS.app/... rpath CMake would
    # otherwise bake in, so the bundle works wherever OBS is installed.
    set_target_properties(obs-icecast PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
else()
    target_link_libraries(obs-icecast PRIVATE
        OBS::libobs
        PkgConfig::LIBAVCODEC
        PkgConfig::LIBAVUTIL
    )
endif()

target_compile_definitions(obs-icecast PRIVATE
    PLUGIN_VERSION="${CMAKE_PROJECT_VERSION}"
)

# Remove "lib" prefix from output filename
set_target_properties(obs-icecast PROPERTIES PREFIX "")

# ── macOS .plugin bundle ─────────────────────────────────────────────
if(APPLE)
    set(PLUGIN_VERSION "${CMAKE_PROJECT_VERSION}")
    configure_file(cmake/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist)

    set_target_properties(obs-icecast PROPERTIES
        SUFFIX ""
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist"
    )

    # Copy locale data into the bundle's Resources directory
    add_custom_command(TARGET obs-icecast POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:obs-icecast>/Contents/Resources/locale"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data/locale"
            "$<TARGET_BUNDLE_DIR:obs-icecast>/Contents/Resources/locale"
        COMMENT "Copying locale data into plugin bundle"
    )
endif()

# ── Installation ─────────────────────────────────────────────────────
if(APPLE)
    # Install the complete .plugin bundle
    install(DIRECTORY "${CMAKE_BINARY_DIR}/obs-icecast.plugin"
        DESTINATION .
        USE_SOURCE_PERMISSIONS
    )
else()
    install(TARGETS obs-icecast LIBRARY DESTINATION obs-icecast/bin/64bit)
    install(DIRECTORY data/ DESTINATION obs-icecast/data)
endif()
