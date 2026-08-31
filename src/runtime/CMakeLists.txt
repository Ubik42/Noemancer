add_executable(noemancer
    asset_vfs_catalog.cpp
    asset_thumbnail_gpu_cache.cpp
    application.cpp
    game_persistence_store.cpp
    gpu_batch_resource_identity.cpp
    gpu_pass_timestamp_adapter.cpp
    input_source_adapter.cpp
    live_editor_transport.cpp
    audio_output_backend.cpp
    miniaudio_render_graph.cpp
    miniaudio_vfs_adapter.cpp
    native_raytracing_capability_adapter.cpp
    native_raytracing_execution_adapter.cpp
    native_d3d12_raytracing_executor.cpp
    native_d3d12_raytracing_context.cpp
    native_vulkan_raytracing_executor.cpp
    native_vulkan_raytracing_context.cpp
    package_vfs_bootstrap.cpp
    main.cpp
    performance_evidence.cpp
    runtime_texture_upload.cpp
    shader_artifact_contract.cpp
    texture_resource_table.cpp
    scene_renderer.cpp
    retained_ui_gpu_adapter.cpp
    source_editor_service.cpp
    windows_package_service.cpp
    windows_runtime_dependencies.cpp
    vfs_asset_reader.cpp
)
target_include_directories(noemancer PRIVATE
    ${SDL3_SOURCE_DIR}/src/video/khronos)
target_link_libraries(noemancer PRIVATE noemancer::editor noemancer::engine noemancer_imgui noemancer_miniaudio nlohmann_json::nlohmann_json)
if(TARGET noemancer_managed_host)
    add_dependencies(noemancer noemancer_managed_host)
endif()

if(WIN32)
    target_link_libraries(noemancer PRIVATE shell32 psapi bcrypt advapi32)
    set(NOEMANCER_DXC_VERSION "1.9.2607")
    set(NOEMANCER_DXC_ARCHIVE "${PROJECT_SOURCE_DIR}/_tools/dxc_2026_07_29.zip")
    set(NOEMANCER_DXC_ROOT "${PROJECT_SOURCE_DIR}/_tools/dxc-${NOEMANCER_DXC_VERSION}")
    set(NOEMANCER_DXC_EXECUTABLE "${NOEMANCER_DXC_ROOT}/bin/x64/dxc.exe")
    if(NOT EXISTS "${NOEMANCER_DXC_EXECUTABLE}")
        file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/_tools")
        file(DOWNLOAD
            "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/dxc_2026_07_29.zip"
            "${NOEMANCER_DXC_ARCHIVE}"
            EXPECTED_HASH "SHA256=A1DFB116BA3EEAE6A1582291B53A8E7BF65AD760676BD3194685C8F7367CD241"
            TLS_VERIFY ON
            STATUS NOEMANCER_DXC_DOWNLOAD_STATUS)
        list(GET NOEMANCER_DXC_DOWNLOAD_STATUS 0 NOEMANCER_DXC_DOWNLOAD_CODE)
        if(NOT NOEMANCER_DXC_DOWNLOAD_CODE EQUAL 0)
            list(GET NOEMANCER_DXC_DOWNLOAD_STATUS 1 NOEMANCER_DXC_DOWNLOAD_MESSAGE)
            message(FATAL_ERROR "Unable to download pinned DXC: ${NOEMANCER_DXC_DOWNLOAD_MESSAGE}")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${NOEMANCER_DXC_ARCHIVE}" DESTINATION "${NOEMANCER_DXC_ROOT}")
    endif()
    set(NOEMANCER_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(NOEMANCER_SHADER_OUTPUTS)
    foreach(shader IN ITEMS scene_lit.vert scene_gpu_driven.vert scene_lit.frag sprite.vert sprite.frag sprite_shadow.vert shadow_depth.vert shadow_depth.frag tone_map.vert tone_map.frag gtao.frag ao_denoise.frag ao_composite.frag aerial_perspective.frag sky_atmosphere_analytic.frag auto_exposure.frag bloom_downsample.frag bloom_upsample.frag fxaa.vert fxaa.frag taa.vert taa.frag temporal_denoise.frag ssr_hiz_trace.frag ssr_temporal_resolve.frag ssr_composite.frag ssgi_hiz_gather.frag ssgi_spatial_resolve.frag ssgi_temporal_resolve.frag ssgi_composite.frag retained_ui.vert retained_ui.frag gpu_visibility.comp gpu_occlusion.comp depth_pyramid_seed.comp depth_pyramid_reduce.comp vfx_sim.comp vfx_spawn.comp vfx_group.comp vfx_sort_alpha.comp vfx_billboard.vert vfx_billboard.frag sky_atmosphere.vert sky_atmosphere.frag sky_atmosphere_transmittance.comp sky_atmosphere_multi_scattering.comp sky_atmosphere_sky_view.comp sky_atmosphere_camera_volume.comp)
        if(shader MATCHES "\\.vert$")
            set(profile vs_6_0)
        elseif(shader MATCHES "\\.comp$")
            set(profile cs_6_0)
        else()
            set(profile ps_6_0)
        endif()
        set(source "${PROJECT_SOURCE_DIR}/assets/shaders/${shader}.hlsl")
        set(dxil_output "${NOEMANCER_SHADER_OUTPUT_DIR}/${shader}.dxil")
        set(spirv_output "${NOEMANCER_SHADER_OUTPUT_DIR}/${shader}.spv")
        add_custom_command(
            OUTPUT "${dxil_output}" "${spirv_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${NOEMANCER_SHADER_OUTPUT_DIR}"
            COMMAND "${NOEMANCER_DXC_EXECUTABLE}" -Zpc -E main -T ${profile} -O3 -Qstrip_debug -Qstrip_reflect -Fo "${dxil_output}" "${source}"
            COMMAND "${NOEMANCER_DXC_EXECUTABLE}" -spirv -Zpc -fspv-target-env=vulkan1.1 -E main -T ${profile} -O3 -Fo "${spirv_output}" "${source}"
            DEPENDS "${source}"
            VERBATIM)
        list(APPEND NOEMANCER_SHADER_OUTPUTS "${dxil_output}" "${spirv_output}")
    endforeach()
    find_program(NOEMANCER_PWSH_EXECUTABLE NAMES pwsh REQUIRED)
    set(NOEMANCER_SHADER_SOURCE_CONTRACT
        "${PROJECT_SOURCE_DIR}/assets/shaders/shader-artifact-contract.json")
    set(NOEMANCER_SHADER_MANIFEST
        "${NOEMANCER_SHADER_OUTPUT_DIR}/shader-artifact-manifest.json")
    set(NOEMANCER_SHADER_REFLECTION
        "${NOEMANCER_SHADER_OUTPUT_DIR}/shader-artifact-reflection.json")
    add_custom_command(
        OUTPUT "${NOEMANCER_SHADER_MANIFEST}" "${NOEMANCER_SHADER_REFLECTION}"
        COMMAND "${NOEMANCER_PWSH_EXECUTABLE}" -NoLogo -NoProfile
            -File "${PROJECT_SOURCE_DIR}/scripts/generate-shader-artifact-manifest.ps1"
            -SourceContract "${NOEMANCER_SHADER_SOURCE_CONTRACT}"
            -SourceRoot "${PROJECT_SOURCE_DIR}/assets/shaders"
            -ArtifactRoot "${NOEMANCER_SHADER_OUTPUT_DIR}"
            -OutputPath "${NOEMANCER_SHADER_MANIFEST}"
            -CompilerPath "${NOEMANCER_DXC_EXECUTABLE}"
            -CompilerVersion "${NOEMANCER_DXC_VERSION}"
        COMMAND "${NOEMANCER_PWSH_EXECUTABLE}" -NoLogo -NoProfile
            -File "${PROJECT_SOURCE_DIR}/scripts/verify-shader-artifact-reflection.ps1"
            -ManifestPath "${NOEMANCER_SHADER_MANIFEST}"
            -CompilerPath "${NOEMANCER_DXC_EXECUTABLE}"
            -OutputPath "${NOEMANCER_SHADER_REFLECTION}"
            -Quiet
        DEPENDS ${NOEMANCER_SHADER_OUTPUTS}
            "${NOEMANCER_SHADER_SOURCE_CONTRACT}"
            "${PROJECT_SOURCE_DIR}/scripts/generate-shader-artifact-manifest.ps1"
            "${PROJECT_SOURCE_DIR}/scripts/verify-shader-artifact-reflection.ps1"
        VERBATIM)
    add_custom_target(noemancer_shaders DEPENDS ${NOEMANCER_SHADER_OUTPUTS}
        "${NOEMANCER_SHADER_MANIFEST}" "${NOEMANCER_SHADER_REFLECTION}")
    add_dependencies(noemancer noemancer_shaders)
    file(TO_CMAKE_PATH "${NOEMANCER_SHADER_OUTPUT_DIR}" NOEMANCER_SHADER_OUTPUT_DIR_NORMALIZED)
    target_compile_definitions(noemancer PRIVATE NOEMANCER_SHADER_DIR="${NOEMANCER_SHADER_OUTPUT_DIR_NORMALIZED}")
endif()

if(MSVC)
    target_compile_options(noemancer PRIVATE /W4 /permissive- /external:anglebrackets /external:W0)
    if(NOEMANCER_WARNINGS_AS_ERRORS)
        target_compile_options(noemancer PRIVATE /WX)
    endif()
endif()
