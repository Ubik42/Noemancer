cmake_minimum_required(VERSION 3.28)

project(Noemancer VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NOEMANCER_WARNINGS_AS_ERRORS "Treat engine warnings as errors" ON)

include(FetchContent)
include(ExternalProject)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.14
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=<SOURCE_DIR>
        -DPATCH_FILE=${PROJECT_SOURCE_DIR}/cmake/patches/sdl-3.4.14-gpu-timestamps.patch
        -P ${PROJECT_SOURCE_DIR}/cmake/apply_patch_if_needed.cmake
        COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=<SOURCE_DIR>
        -DPATCH_FILE=${PROJECT_SOURCE_DIR}/cmake/patches/sdl-3.4.14-gpu-native-device-access.patch
        -P ${PROJECT_SOURCE_DIR}/cmake/apply_patch_if_needed.cmake
)

set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_PIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG v4.1.6
    GIT_SHALLOW TRUE
)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9-docking
    GIT_SHALLOW TRUE
)

# MIT immediate-mode transform gizmo. Pinned release; Noemancer owns the
# transaction, semantic observation and scene persistence layers around it.
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 1.10
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake
)

FetchContent_Declare(
    lodepng
    GIT_REPOSITORY https://github.com/lvandeve/lodepng.git
    GIT_TAG ed6fe5825c6a4fbb7f58ab35a4231c7543cd452a
    GIT_SHALLOW FALSE
    SOURCE_SUBDIR _no_cmake
)

# libjpeg-turbo is the SIMD-accelerated JPEG decode kernel behind the
# engine-owned RGBA8 image adapter. Pin the official release archive and hash;
# no TurboJPEG types cross the public asset or scene boundary.
FetchContent_Declare(
    libjpeg_turbo
    URL https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz
    URL_HASH SHA256=6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e
    SOURCE_SUBDIR _no_cmake
)

# miniaudio is the production decode/device/graph kernel behind Noemancer's
# plain-data audio contracts. The first adapter slice replaces format parsing;
# device and graph ownership migrate separately without exposing ma_* types.
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake
)

# Standards-compliant glTF semantics and production mesh optimization kernels
# remain private implementation dependencies behind Noemancer-owned records.
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG v0.9.0
    GIT_SHALLOW TRUE
)

set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v1.2
    GIT_SHALLOW TRUE
)

# KTX-Software provides the production KTX2 container and Basis encoder. The
# runtime upload helpers and CLI/test applications are unnecessary in-engine.
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "" FORCE)
set(KTX_FEATURE_ETC_UNPACK OFF CACHE BOOL "" FORCE)
# libktx's common texture object implementation still references the KTX1
# constructor even when Noemancer only emits KTX2, so keep that library slice.
set(KTX_FEATURE_KTX1 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_KTX2 ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ktx_software
    GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
    GIT_TAG v4.4.2
    GIT_SHALLOW TRUE
)

# Jolt is an implementation detail behind Noemancer's stable physics contract.
# Do not let its standalone defaults replace the host project's compiler/runtime flags.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
)

# ozz-animation is the data-oriented skeletal animation kernel. Noemancer owns
# source asset validation and conversion; only validated runtime objects cross
# this dependency boundary.
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    ozz_animation
    GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git
    GIT_TAG 0.17.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG v0.23.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmake
)

# FreeType provides RmlUi's required font-engine ABI. Shaping, fallback and
# localization remain engine-owned layers and are integrated separately.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG VER-2-14-3
    GIT_SHALLOW TRUE
)

# Text shaping is engine-owned rather than hidden inside locale resources.
# HarfBuzz is built from a pinned source release. ICU's upstream Windows build
# is distributed as an official MSVC archive; other platforms use a packaged
# ICU so the ABI matches their system toolchain.
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU_DEMO OFF CACHE STRING "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    harfbuzz
    GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
    GIT_TAG 14.3.1
    GIT_SHALLOW TRUE
)

if(WIN32)
    FetchContent_Declare(
        icu4c_runtime
        URL https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-Win64-MSVC2022.zip
        URL_HASH SHA256=446b671f9437227daa79e221d4521d75793f9ecd65ac44c06e34dd848f201ac2
    )
endif()

# RmlUi is a retained document/layout implementation behind Noemancer's stable
# Semantic UI contract. The upstream sample backends own their window and GPU
# device, so the engine links only the core and provides its own adapters.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    rmlui
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG 6.2
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=<SOURCE_DIR>
        -DPATCH_FILE=${PROJECT_SOURCE_DIR}/cmake/patches/rmlui-6.2-harfbuzz-icu.patch
        -P ${PROJECT_SOURCE_DIR}/cmake/apply_patch_if_needed.cmake
)

FetchContent_MakeAvailable(SDL flecs nlohmann_json imgui imguizmo lodepng libjpeg_turbo miniaudio fastgltf meshoptimizer ktx_software JoltPhysics ozz_animation ufbx freetype harfbuzz)
set(NOEMANCER_LIBJPEG_TURBO_BINARY_DIR "${FETCHCONTENT_BASE_DIR}/libjpeg_turbo-external-build")
ExternalProject_Add(noemancer_libjpeg_turbo_external
    SOURCE_DIR "${libjpeg_turbo_SOURCE_DIR}"
    BINARY_DIR "${NOEMANCER_LIBJPEG_TURBO_BINARY_DIR}"
    CMAKE_ARGS
        -DENABLE_SHARED=OFF
        -DENABLE_STATIC=ON
        -DWITH_TURBOJPEG=ON
        -DWITH_TOOLS=OFF
        -DWITH_TESTS=OFF
        -DWITH_CRT_DLL=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target turbojpeg-static
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_ALWAYS 1
)
add_library(noemancer_turbojpeg STATIC IMPORTED GLOBAL)
add_dependencies(noemancer_turbojpeg noemancer_libjpeg_turbo_external)
set_target_properties(noemancer_turbojpeg PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${libjpeg_turbo_SOURCE_DIR}/src")
if(MSVC)
    set_target_properties(noemancer_turbojpeg PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
        IMPORTED_LOCATION_DEBUG "${NOEMANCER_LIBJPEG_TURBO_BINARY_DIR}/Debug/turbojpeg-static.lib"
        IMPORTED_LOCATION_RELEASE "${NOEMANCER_LIBJPEG_TURBO_BINARY_DIR}/Release/turbojpeg-static.lib"
        IMPORTED_LOCATION_RELWITHDEBINFO "${NOEMANCER_LIBJPEG_TURBO_BINARY_DIR}/RelWithDebInfo/turbojpeg-static.lib"
        IMPORTED_LOCATION_MINSIZEREL "${NOEMANCER_LIBJPEG_TURBO_BINARY_DIR}/MinSizeRel/turbojpeg-static.lib")
else()
    set_target_properties(noemancer_turbojpeg PROPERTIES
        IMPORTED_LOCATION "${NOEMANCER_LIBJPEG_TURBO_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype-interface)
endif()
if(WIN32)
    FetchContent_MakeAvailable(icu4c_runtime)
    add_library(noemancer_icu_uc SHARED IMPORTED GLOBAL)
    set_target_properties(noemancer_icu_uc PROPERTIES
        IMPORTED_IMPLIB "${icu4c_runtime_SOURCE_DIR}/lib64/icuuc.lib"
        IMPORTED_LOCATION "${icu4c_runtime_SOURCE_DIR}/bin64/icuuc78.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${icu4c_runtime_SOURCE_DIR}/include")
    add_library(noemancer_icu_i18n SHARED IMPORTED GLOBAL)
    set_target_properties(noemancer_icu_i18n PROPERTIES
        IMPORTED_IMPLIB "${icu4c_runtime_SOURCE_DIR}/lib64/icuin.lib"
        IMPORTED_LOCATION "${icu4c_runtime_SOURCE_DIR}/bin64/icuin78.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${icu4c_runtime_SOURCE_DIR}/include"
        INTERFACE_LINK_LIBRARIES noemancer_icu_uc)
    add_library(ICU::uc ALIAS noemancer_icu_uc)
    add_library(ICU::i18n ALIAS noemancer_icu_i18n)
    set(NOEMANCER_ICU_RUNTIME_DIR "${icu4c_runtime_SOURCE_DIR}/bin64")
else()
    find_package(ICU 78.3 REQUIRED COMPONENTS uc i18n)
endif()
FetchContent_MakeAvailable(rmlui)
target_link_libraries(rmlui_core PRIVATE harfbuzz ICU::uc ICU::i18n)

add_library(noemancer_ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(noemancer_ufbx PUBLIC ${ufbx_SOURCE_DIR})
target_compile_definitions(noemancer_ufbx PRIVATE
    UFBX_NO_SUBDIVISION
    UFBX_NO_TESSELLATION
    UFBX_NO_GEOMETRY_CACHE
    UFBX_NO_FORMAT_OBJ
)
if(MSVC)
    target_compile_options(noemancer_ufbx PRIVATE /W0)
endif()

add_library(noemancer_miniaudio STATIC src/third_party/miniaudio_impl.cpp)
target_include_directories(noemancer_miniaudio PUBLIC ${miniaudio_SOURCE_DIR})
if(MSVC)
    target_compile_options(noemancer_miniaudio PRIVATE /W0)
endif()

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
