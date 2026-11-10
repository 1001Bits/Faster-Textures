cmake_minimum_required(VERSION 3.23)

project(FasterTextures VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
    message(FATAL_ERROR "in-source builds are not allowed")
endif()

# ── Dependencies, all under extern/ ─────────────────────────────────────────
# CommonLibSSE-NG (SE, AE and VR in one build). vcpkg is extern/vcpkg, see
# CMakePresets.json.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(COMMONLIBSSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG")
add_subdirectory("${COMMONLIBSSE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/commonlibsse-ng")
include("${COMMONLIBSSE_DIR}/cmake/CommonLibSSE.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Lz4.cmake")

# ── The plugin ──────────────────────────────────────────────────────────────
add_commonlibsse_plugin(
    ${PROJECT_NAME}
    AUTHOR "Asciimov"
    USE_ADDRESS_LIBRARY
    SOURCES
        src/main.cpp
        src/SourceIdentity.h
        src/GpuCompletion.h
        src/Logging.cpp
        src/Logging.h
        src/PCH.h
        src/ArchiveTail.h
        src/MipTail.h
        src/Streamer.cpp
        src/Streamer.h
        src/TailCache.h
        src/TextureFormat.h
)

find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

target_include_directories(${PROJECT_NAME} PRIVATE
    "${COMMONLIBSSE_DIR}/include"
    src
    # Faster Archives' archive format code, as FasterArchives/<header>: extern/FasterArchives
    # holds the src folder of https://github.com/1001Bits/Faster-Archives.
    "${CMAKE_CURRENT_SOURCE_DIR}/extern"
    "${SIMPLEINI_INCLUDE_DIRS}"
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_link_libraries(${PROJECT_NAME} PRIVATE bcrypt ftx_lz4)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

if(MSVC)
    # Symbols in every build, so a crash log names functions.
    target_compile_options(${PROJECT_NAME} PRIVATE /Z7 /utf-8)
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:NOICF)
endif()

# ── Deploy ──────────────────────────────────────────────────────────────────
# Set DEPLOY_DIR to a mod folder (MO2/Vortex) or a game's Data folder and every
# build copies the DLL and its PDB there. Empty by default: nothing is copied.
set(DEPLOY_DIR "" CACHE PATH "Folder the plugin is copied into after each build")

if(DEPLOY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}/SKSE/Plugins"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DEPLOY_DIR}/SKSE/Plugins/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DEPLOY_DIR}/SKSE/Plugins/"
        VERBATIM)
endif()

# ── Archive ─────────────────────────────────────────────────────────────────
# The DLL and the default ini, laid out as they go into Data.
set(ZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/zip")
file(MAKE_DIRECTORY "${ZIP_DIR}/SKSE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${ZIP_DIR}/SKSE/Plugins/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/package/${PROJECT_NAME}.ini" "${ZIP_DIR}/SKSE/Plugins/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/package/${PROJECT_NAME}.esp" "${ZIP_DIR}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/SEQ" "${ZIP_DIR}/SEQ"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/MCM" "${ZIP_DIR}/MCM"
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-${PROJECT_VERSION}.7z" --format=7zip -- .
    WORKING_DIRECTORY "${ZIP_DIR}"
    VERBATIM)
