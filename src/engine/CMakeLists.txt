add_library(noemancer_engine STATIC
    animation_clip_asset.cpp
    animation_graph.cpp
    animation_graph_patch.cpp
    animation_state_machine.cpp
    asset_registry.cpp
    asset_cook_pipeline.cpp
    asset_job_queue.cpp
    asset_repair.cpp
    audio_asset.cpp
    cascaded_shadow.cpp
    clustered_lighting.cpp
    character_motor_2d.cpp
    command_registry.cpp
    content_hash.cpp
    engine_host.cpp
    fbx_asset.cpp
    gameplay_runtime.cpp
    gameplay_ability.cpp
    gltf_inspector.cpp
    gltf_mesh.cpp
    gpu_batch_cache.cpp
    hybrid_pixel_profile.cpp
    hybrid_pixel_post.cpp
    hybrid_pixel_render.cpp
    ibl_cook.cpp
    image_decoder.cpp
    ktx2_cook_adapter.cpp
    linear_dirty_ranges.cpp
    live_editor_session.cpp
    ${lodepng_SOURCE_DIR}/lodepng.cpp
    log.cpp
    managed_debug_protocol.cpp
    managed_debug_transport.cpp
    mesh_runtime_artifact.cpp
    network_replication.cpp
    network_transport.cpp
    package_pipeline.cpp
    play_world_apply.cpp
    pixel_presentation.cpp
    pixel_snapping.cpp
    process_diagnostics.cpp
    project_document.cpp
    project_hybrid_pixel_authoring.cpp
    project_ui_authoring.cpp
    project_ui_authoring_commands.cpp
    project_workspace.cpp
    render_graph.cpp
    render_reference_scene.cpp
    render_world.cpp
    retained_ui_runtime.cpp
    sky_atmosphere.cpp
    sky_environment.cpp
    scene_document.cpp
    semantic_ui.cpp
    semantic_state.cpp
    semantic_2d_character_bake.cpp
    semantic_2d_character_rig.cpp
    scripting_runtime.cpp
    simulation_runtime.cpp
    sprite_asset.cpp
    sprite_atlas_artifact.cpp
    sprite_atlas_page_cache.cpp
    tilemap_asset.cpp
    temporal_aa.cpp
    temporal_history.cpp
    screen_space_reflections.cpp
    screen_space_global_illumination.cpp
    gpu_occlusion_policy.cpp
    texture_streaming_demand.cpp
    texture_streaming_policy.cpp
    text_layout.cpp
    asset_thumbnail.cpp
    asset_workflow.cpp
    visibility_culling.cpp
    vfx_gpu_residency.cpp
    vfx_runtime.cpp
    virtual_file_system.cpp
    vfs_document_reader.cpp
    world.cpp
)

add_library(noemancer::engine ALIAS noemancer_engine)

set(NOEMANCER_LOCAL_DOTNET "${PROJECT_SOURCE_DIR}/_tools/dotnet/dotnet.exe")
if(WIN32 AND EXISTS "${NOEMANCER_LOCAL_DOTNET}")
    set(NOEMANCER_MANAGED_OUTPUT "${PROJECT_BINARY_DIR}/managed/$<CONFIG>")
    add_custom_target(noemancer_managed_host
        COMMAND "${NOEMANCER_LOCAL_DOTNET}" build
            "${PROJECT_SOURCE_DIR}/managed/Noemancer.ManagedHost/Noemancer.ManagedHost.csproj"
            --configuration $<CONFIG>
            --output "${NOEMANCER_MANAGED_OUTPUT}"
            --nologo
        COMMENT "Building the Noemancer managed host for $<CONFIG>"
        VERBATIM)
endif()

target_include_directories(noemancer_engine PUBLIC ${PROJECT_SOURCE_DIR}/src PRIVATE
    ${lodepng_SOURCE_DIR}
    ${rmlui_SOURCE_DIR}/Source/Core/FontEngineDefault)
target_link_libraries(noemancer_engine PRIVATE noemancer_miniaudio)
set(NOEMANCER_GENERATED_FONT_DIR "${PROJECT_BINARY_DIR}/generated/fonts")
file(MAKE_DIRECTORY "${NOEMANCER_GENERATED_FONT_DIR}")
configure_file(
    "${rmlui_SOURCE_DIR}/Samples/assets/LatoLatin-Regular.ttf"
    "${NOEMANCER_GENERATED_FONT_DIR}/LatoLatin-Regular.ttf"
    COPYONLY)
configure_file(
    "${rmlui_SOURCE_DIR}/Samples/assets/LICENSE.txt"
    "${NOEMANCER_GENERATED_FONT_DIR}/RmlUi-Sample-Assets-LICENSE.txt"
    COPYONLY)
set(NOEMANCER_GENERATED_LICENSE_DIR "${PROJECT_BINARY_DIR}/generated/licenses")
file(MAKE_DIRECTORY "${NOEMANCER_GENERATED_LICENSE_DIR}")
configure_file("${harfbuzz_SOURCE_DIR}/COPYING" "${NOEMANCER_GENERATED_LICENSE_DIR}/HarfBuzz-COPYING.txt" COPYONLY)
configure_file("${sdl_SOURCE_DIR}/LICENSE.txt" "${NOEMANCER_GENERATED_LICENSE_DIR}/SDL-LICENSE.txt" COPYONLY)
configure_file("${flecs_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/flecs-LICENSE.txt" COPYONLY)
configure_file("${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" "${NOEMANCER_GENERATED_LICENSE_DIR}/nlohmann-json-LICENSE.txt" COPYONLY)
configure_file("${imgui_SOURCE_DIR}/LICENSE.txt" "${NOEMANCER_GENERATED_LICENSE_DIR}/Dear-ImGui-LICENSE.txt" COPYONLY)
configure_file("${imguizmo_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/ImGuizmo-LICENSE.txt" COPYONLY)
configure_file("${lodepng_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/lodepng-LICENSE.txt" COPYONLY)
configure_file("${libjpeg_turbo_SOURCE_DIR}/LICENSE.md" "${NOEMANCER_GENERATED_LICENSE_DIR}/libjpeg-turbo-LICENSE.md" COPYONLY)
configure_file("${miniaudio_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/miniaudio-LICENSE.txt" COPYONLY)
configure_file("${joltphysics_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/Jolt-Physics-LICENSE.txt" COPYONLY)
configure_file("${freetype_SOURCE_DIR}/docs/FTL.TXT" "${NOEMANCER_GENERATED_LICENSE_DIR}/FreeType-LICENSE.txt" COPYONLY)
configure_file("${rmlui_SOURCE_DIR}/LICENSE.txt" "${NOEMANCER_GENERATED_LICENSE_DIR}/RmlUi-LICENSE.txt" COPYONLY)
configure_file("${rmlui_SOURCE_DIR}/Samples/assets/LICENSE.txt" "${NOEMANCER_GENERATED_LICENSE_DIR}/Lato-Font-LICENSE.txt" COPYONLY)
configure_file("${fastgltf_SOURCE_DIR}/LICENSE.md" "${NOEMANCER_GENERATED_LICENSE_DIR}/fastgltf-LICENSE.md" COPYONLY)
configure_file("${meshoptimizer_SOURCE_DIR}/LICENSE.md" "${NOEMANCER_GENERATED_LICENSE_DIR}/meshoptimizer-LICENSE.md" COPYONLY)
configure_file("${ktx_software_SOURCE_DIR}/LICENSE.md" "${NOEMANCER_GENERATED_LICENSE_DIR}/KTX-Software-LICENSE.md" COPYONLY)
configure_file("${ozz_animation_SOURCE_DIR}/LICENSE.md" "${NOEMANCER_GENERATED_LICENSE_DIR}/ozz-animation-LICENSE.md" COPYONLY)
configure_file("${ufbx_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/ufbx-LICENSE.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/Noemancer-LICENSE.txt" COPYONLY)
if(WIN32)
    configure_file("${icu4c_runtime_SOURCE_DIR}/LICENSE" "${NOEMANCER_GENERATED_LICENSE_DIR}/ICU-LICENSE.txt" COPYONLY)
endif()
file(TO_CMAKE_PATH "${NOEMANCER_GENERATED_FONT_DIR}/LatoLatin-Regular.ttf" NOEMANCER_DEFAULT_FONT_PATH)
target_compile_definitions(noemancer_engine PRIVATE
    NOEMANCER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    NOEMANCER_BINARY_DIR="${PROJECT_BINARY_DIR}"
    NOEMANCER_RMLUI_DEFAULT_FONT="${NOEMANCER_DEFAULT_FONT_PATH}")
target_link_libraries(noemancer_engine
    PUBLIC flecs::flecs_static SDL3::SDL3-static
    PRIVATE fastgltf::fastgltf meshoptimizer ktx noemancer_turbojpeg
    PRIVATE Jolt
    PRIVATE ozz_animation ozz_animation_offline
    PRIVATE noemancer_ufbx
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE RmlUi::Core
    PRIVATE harfbuzz ICU::uc ICU::i18n
)
if(WIN32)
    add_custom_command(TARGET noemancer_engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/tests/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/src/runtime/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NOEMANCER_ICU_RUNTIME_DIR}/icuuc78.dll"
            "${NOEMANCER_ICU_RUNTIME_DIR}/icuin78.dll"
            "${NOEMANCER_ICU_RUNTIME_DIR}/icudt78.dll"
            "${PROJECT_BINARY_DIR}/tests/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NOEMANCER_ICU_RUNTIME_DIR}/icuuc78.dll"
            "${NOEMANCER_ICU_RUNTIME_DIR}/icuin78.dll"
            "${NOEMANCER_ICU_RUNTIME_DIR}/icudt78.dll"
            "${PROJECT_BINARY_DIR}/src/runtime/$<CONFIG>")
endif()
if(WIN32)
    target_link_libraries(noemancer_engine PRIVATE ws2_32 dbghelp)
endif()

if(MSVC)
    target_compile_options(noemancer_engine PRIVATE
        /W4
        /permissive-
        /Zc:preprocessor
        /external:anglebrackets
        /external:W0
    )
    if(NOEMANCER_WARNINGS_AS_ERRORS)
        target_compile_options(noemancer_engine PRIVATE /WX)
    endif()
endif()
