add_library(noemancer_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
)

target_include_directories(noemancer_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imguizmo_SOURCE_DIR}/src
)
target_link_libraries(noemancer_imgui PUBLIC SDL3::SDL3-static)

add_library(noemancer_editor STATIC
    animation_graph_canvas.cpp
    editor_model.cpp
    editor_ui.cpp
    hybrid_pixel_profile_panel.cpp
    project_settings_input_map.cpp
    project_settings_input_map_panel.cpp
    project_ui_authoring_panel.cpp
    recent_workspace_store.cpp
    startup_hub.cpp
)

add_library(noemancer::editor ALIAS noemancer_editor)

target_include_directories(noemancer_editor PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noemancer_editor
    PUBLIC noemancer::engine
    PRIVATE noemancer_imgui nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(noemancer_editor PRIVATE
        /W4
        /utf-8
        /permissive-
        /Zc:preprocessor
        /external:anglebrackets
        /external:W0
    )
    if(NOEMANCER_WARNINGS_AS_ERRORS)
        target_compile_options(noemancer_editor PRIVATE /WX)
    endif()
endif()
