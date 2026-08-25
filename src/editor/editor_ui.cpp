#include "editor/editor_ui.hpp"
#include "editor/project_settings_input_map.hpp"
#include "engine/transform_math.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <cstdio>
#include <functional>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <utility>

namespace noemancer {
namespace {

constexpr ImU32 panel_background = IM_COL32(19, 24, 33, 255);
constexpr ImU32 grid_minor = IM_COL32(49, 59, 73, 85);
constexpr ImU32 grid_major = IM_COL32(74, 89, 108, 130);
constexpr ImU32 accent = IM_COL32(84, 178, 255, 255);
constexpr ImVec4 color_accent{0.31F,0.66F,0.98F,1.0F};
constexpr ImVec4 color_success{0.30F,0.78F,0.55F,1.0F};
constexpr ImVec4 color_warning{0.96F,0.67F,0.28F,1.0F};
constexpr ImVec4 color_danger{0.95F,0.35F,0.35F,1.0F};

enum class EditorIcon : std::uint8_t { select,move,rotate,scale,frame,play,stop,pause,resume,save,add,more,tile };

void draw_editor_icon(ImDrawList* draw,const EditorIcon icon,const ImVec2 center,const float size,const ImU32 color) {
    const auto h=size*0.5F;
    constexpr float thickness=1.6F;
    switch(icon) {
    case EditorIcon::select: {
        const ImVec2 points[]={{center.x-h*0.72F,center.y-h},{center.x+h*0.64F,center.y+h*0.28F},
            {center.x+h*0.08F,center.y+h*0.34F},{center.x+h*0.42F,center.y+h},{center.x+h*0.10F,center.y+h*0.96F}};
        draw->AddPolyline(points,5,color,ImDrawFlags_Closed,thickness);break;
    }
    case EditorIcon::move:
        draw->AddLine({center.x-h,center.y},{center.x+h,center.y},color,thickness);
        draw->AddLine({center.x,center.y-h},{center.x,center.y+h},color,thickness);
        draw->AddTriangleFilled({center.x-h,center.y},{center.x-h*0.48F,center.y-h*0.30F},{center.x-h*0.48F,center.y+h*0.30F},color);
        draw->AddTriangleFilled({center.x+h,center.y},{center.x+h*0.48F,center.y-h*0.30F},{center.x+h*0.48F,center.y+h*0.30F},color);
        draw->AddTriangleFilled({center.x,center.y-h},{center.x-h*0.30F,center.y-h*0.48F},{center.x+h*0.30F,center.y-h*0.48F},color);
        draw->AddTriangleFilled({center.x,center.y+h},{center.x-h*0.30F,center.y+h*0.48F},{center.x+h*0.30F,center.y+h*0.48F},color);break;
    case EditorIcon::rotate:
        draw->PathArcTo(center,h*0.82F,-2.65F,2.25F,18);draw->PathStroke(color,0,thickness);
        draw->AddTriangleFilled({center.x-h*0.76F,center.y-h*0.42F},{center.x-h*0.98F,center.y+h*0.05F},
            {center.x-h*0.46F,center.y-h*0.02F},color);break;
    case EditorIcon::scale:
        draw->AddRect({center.x-h*0.85F,center.y-h*0.15F},{center.x+h*0.15F,center.y+h*0.85F},color,0,0,thickness);
        draw->AddLine({center.x-h*0.35F,center.y+h*0.35F},{center.x+h*0.72F,center.y-h*0.72F},color,thickness);
        draw->AddRectFilled({center.x+h*0.38F,center.y-h},{center.x+h,center.y-h*0.38F},color);break;
    case EditorIcon::frame:
        draw->AddLine({center.x-h,center.y-h*0.35F},{center.x-h,center.y-h},color,thickness);draw->AddLine({center.x-h,center.y-h},{center.x-h*0.35F,center.y-h},color,thickness);
        draw->AddLine({center.x+h,center.y-h*0.35F},{center.x+h,center.y-h},color,thickness);draw->AddLine({center.x+h,center.y-h},{center.x+h*0.35F,center.y-h},color,thickness);
        draw->AddLine({center.x-h,center.y+h*0.35F},{center.x-h,center.y+h},color,thickness);draw->AddLine({center.x-h,center.y+h},{center.x-h*0.35F,center.y+h},color,thickness);
        draw->AddLine({center.x+h,center.y+h*0.35F},{center.x+h,center.y+h},color,thickness);draw->AddLine({center.x+h,center.y+h},{center.x+h*0.35F,center.y+h},color,thickness);break;
    case EditorIcon::play: case EditorIcon::resume:
        draw->AddTriangleFilled({center.x-h*0.55F,center.y-h},{center.x+h,center.y},{center.x-h*0.55F,center.y+h},color);break;
    case EditorIcon::stop: draw->AddRectFilled({center.x-h*0.72F,center.y-h*0.72F},{center.x+h*0.72F,center.y+h*0.72F},color,2.0F);break;
    case EditorIcon::pause:
        draw->AddRectFilled({center.x-h*0.65F,center.y-h},{center.x-h*0.15F,center.y+h},color,1.0F);
        draw->AddRectFilled({center.x+h*0.15F,center.y-h},{center.x+h*0.65F,center.y+h},color,1.0F);break;
    case EditorIcon::save:
        draw->AddRect({center.x-h,center.y-h},{center.x+h,center.y+h},color,1.5F,0,thickness);
        draw->AddRectFilled({center.x-h*0.55F,center.y-h},{center.x+h*0.38F,center.y-h*0.28F},color);
        draw->AddRect({center.x-h*0.55F,center.y+h*0.15F},{center.x+h*0.55F,center.y+h},color,1.0F,0,thickness);break;
    case EditorIcon::add:
        draw->AddLine({center.x-h,center.y},{center.x+h,center.y},color,thickness);draw->AddLine({center.x,center.y-h},{center.x,center.y+h},color,thickness);break;
    case EditorIcon::more:
        for(const auto offset:{-0.62F,0.0F,0.62F})draw->AddCircleFilled({center.x+h*offset,center.y},1.6F,color);break;
    case EditorIcon::tile:
        for(int y=0;y<2;++y)for(int x=0;x<2;++x)draw->AddRect({center.x-h+x*h,center.y-h+y*h},
            {center.x-h*0.08F+x*h,center.y-h*0.08F+y*h},color,1.0F,0,thickness);break;
    }
}

bool draw_icon_button(const char* id,const EditorIcon icon,const char* label,const bool active=false,const char* tooltip=nullptr) {
    const auto height=ImGui::GetFrameHeight();
    const auto has_label=label!=nullptr&&label[0]!='\0';
    const auto width=has_label?ImGui::CalcTextSize(label).x+34.0F:height;
    const auto position=ImGui::GetCursorScreenPos();
    const auto pressed=ImGui::InvisibleButton(id,{width,height});
    const auto hovered=ImGui::IsItemHovered();const auto held=ImGui::IsItemActive();
    const auto background=held?ImGuiCol_ButtonActive:hovered?ImGuiCol_ButtonHovered:ImGuiCol_Button;
    auto color=ImGui::GetColorU32(background);
    if(active&&!hovered&&!held)color=ImGui::ColorConvertFloat4ToU32({0.12F,0.27F,0.39F,1.0F});
    ImGui::GetWindowDrawList()->AddRectFilled(position,{position.x+width,position.y+height},color,ImGui::GetStyle().FrameRounding);
    const auto text_color=ImGui::GetColorU32(active?ImVec4{0.76F,0.90F,1.0F,1.0F}:ImGui::GetStyleColorVec4(ImGuiCol_Text));
    const ImVec2 icon_center{position.x+(has_label?14.0F:width*0.5F),position.y+height*0.5F};
    draw_editor_icon(ImGui::GetWindowDrawList(),icon,icon_center,12.0F,text_color);
    if(has_label)ImGui::GetWindowDrawList()->AddText({position.x+27.0F,position.y+(height-ImGui::GetTextLineHeight())*0.5F},text_color,label);
    if(tooltip!=nullptr&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("%s",tooltip);
    return pressed;
}

struct GizmoVec3 { float x{},y{},z{}; };
GizmoVec3 operator-(const GizmoVec3 a,const GizmoVec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
float dot(const GizmoVec3 a,const GizmoVec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
GizmoVec3 cross(const GizmoVec3 a,const GizmoVec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
GizmoVec3 normalize(const GizmoVec3 value){const auto length=std::sqrt(dot(value,value)); return length>0.00001F?GizmoVec3{value.x/length,value.y/length,value.z/length}:GizmoVec3{};}
std::array<float,16> identity_matrix(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
std::array<float,16> view_matrix(const GizmoVec3 eye,const GizmoVec3 target){
    const auto forward=normalize(target-eye),side=normalize(cross(forward,{0,1,0})),up=cross(side,forward); auto value=identity_matrix();
    value[0]=side.x;value[1]=up.x;value[2]=-forward.x;value[4]=side.y;value[5]=up.y;value[6]=-forward.y;
    value[8]=side.z;value[9]=up.z;value[10]=-forward.z;value[12]=-dot(side,eye);value[13]=-dot(up,eye);value[14]=dot(forward,eye);return value;
}
std::array<float,16> perspective_matrix(const float fov,const float aspect,const float near_clip,const float far_clip){
    std::array<float,16> value{};const auto scale=1.0F/std::tan(fov*0.5F);value[0]=scale/aspect;value[5]=scale;
    value[10]=far_clip/(near_clip-far_clip);value[11]=-1;value[14]=near_clip*far_clip/(near_clip-far_clip);return value;
}
std::array<float,16> orthographic_matrix(const float half_height,const float aspect,const float near_clip,const float far_clip){
    auto value=identity_matrix();const auto half_width=half_height*aspect;value[0]=1.0F/half_width;value[5]=1.0F/half_height;
    value[10]=1.0F/(near_clip-far_clip);value[14]=near_clip/(near_clip-far_clip);return value;
}

void apply_editor_style() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 5.0F;
    style.ChildRounding = 5.0F;
    style.FrameRounding = 5.0F;
    style.PopupRounding = 7.0F;
    style.TabRounding = 5.0F;
    style.GrabRounding = 4.0F;
    style.ScrollbarRounding = 8.0F;
    style.WindowBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.WindowPadding = ImVec2(10.0F, 9.0F);
    style.FramePadding = ImVec2(9.0F, 5.0F);
    style.ItemSpacing = ImVec2(8.0F, 7.0F);
    style.ItemInnerSpacing = ImVec2(6.0F,4.0F);
    style.IndentSpacing = 18.0F;
    style.ScrollbarSize = 11.0F;
    auto& colors=style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.88F,0.91F,0.95F,1.0F);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.46F,0.52F,0.61F,1.0F);
    colors[ImGuiCol_WindowBg] = ImVec4(0.043F,0.051F,0.070F,1.0F);
    colors[ImGuiCol_ChildBg] = ImVec4(0.052F,0.062F,0.083F,1.0F);
    colors[ImGuiCol_PopupBg] = ImVec4(0.065F,0.076F,0.100F,0.98F);
    colors[ImGuiCol_Border] = ImVec4(0.15F,0.18F,0.23F,1.0F);
    colors[ImGuiCol_BorderShadow] = ImVec4(0,0,0,0);
    colors[ImGuiCol_FrameBg] = ImVec4(0.080F,0.096F,0.126F,1.0F);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.105F,0.130F,0.170F,1.0F);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.125F,0.160F,0.210F,1.0F);
    colors[ImGuiCol_TitleBg] = ImVec4(0.045F,0.053F,0.071F,1.0F);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.062F,0.075F,0.100F,1.0F);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.035F,0.042F,0.058F,1.0F);
    colors[ImGuiCol_Tab] = ImVec4(0.055F,0.066F,0.088F,1.0F);
    colors[ImGuiCol_TabHovered] = ImVec4(0.105F,0.155F,0.215F,1.0F);
    colors[ImGuiCol_TabSelected] = ImVec4(0.095F,0.125F,0.165F,1.0F);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.068F,0.083F,0.108F,1.0F);
    colors[ImGuiCol_Header] = ImVec4(0.095F,0.125F,0.165F,1.0F);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.125F,0.180F,0.245F,1.0F);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.145F,0.220F,0.305F,1.0F);
    colors[ImGuiCol_Button] = ImVec4(0.082F,0.102F,0.136F,1.0F);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.120F,0.175F,0.235F,1.0F);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.145F,0.225F,0.315F,1.0F);
    colors[ImGuiCol_CheckMark] = color_accent;
    colors[ImGuiCol_SliderGrab] = ImVec4(0.28F,0.57F,0.86F,1.0F);
    colors[ImGuiCol_SliderGrabActive] = color_accent;
    colors[ImGuiCol_Separator] = ImVec4(0.14F,0.17F,0.22F,1.0F);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.28F,0.55F,0.82F,1.0F);
    colors[ImGuiCol_SeparatorActive] = color_accent;
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.18F,0.24F,0.32F,0.35F);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.28F,0.55F,0.82F,0.65F);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.31F,0.66F,0.98F,0.38F);
    colors[ImGuiCol_NavCursor] = color_accent;
}

void draw_status_badge(const char* label,const ImVec4 color) {
    const auto text_size=ImGui::CalcTextSize(label);const auto position=ImGui::GetCursorScreenPos();
    const ImVec2 size{text_size.x+12.0F,ImGui::GetFrameHeight()};
    ImGui::GetWindowDrawList()->AddRectFilled(position,{position.x+size.x,position.y+size.y},
        ImGui::ColorConvertFloat4ToU32({color.x*0.18F,color.y*0.18F,color.z*0.18F,1.0F}),4.0F);
    ImGui::GetWindowDrawList()->AddText({position.x+6.0F,position.y+(size.y-text_size.y)*0.5F},
        ImGui::ColorConvertFloat4ToU32(color),label);
    ImGui::Dummy(size);
}

bool draw_mode_button(const char* id,const EditorIcon icon,const char* label,const bool active) {
    return draw_icon_button(id,icon,label,active,label);
}

bool contains_case_insensitive(const std::string_view value,const std::string_view query) {
    if(query.empty())return true;
    return std::ranges::search(value,query,{},
        [](const char character){return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));},
        [](const char character){return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));}).begin()!=value.end();
}

void draw_panel_heading(const char* title,const char* detail) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    if(detail!=nullptr&&detail[0]!='\0') {
        ImGui::SameLine();
        ImGui::TextDisabled("%s",detail);
    }
}

void draw_empty_panel_state(const char* title,const char* detail) {
    const auto available=ImGui::GetContentRegionAvail();
    ImGui::Dummy({1.0F,std::max(24.0F,available.y*0.2F)});
    const auto title_size=ImGui::CalcTextSize(title);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0F,(available.x-title_size.x)*0.5F));
    ImGui::TextUnformatted(title);
    const auto detail_size=ImGui::CalcTextSize(detail);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0F,(available.x-detail_size.x)*0.5F));
    ImGui::TextDisabled("%s",detail);
}

void draw_isometric_cube(
    ImDrawList* draw_list,
    const ImVec2 center,
    const float size,
    const bool selected) {
    const float half = size * 0.5F;
    const float depth = size * 0.28F;
    const std::array<ImVec2, 8> points{{
        {center.x - half, center.y - half + depth},
        {center.x + half, center.y - half + depth},
        {center.x + half, center.y + half + depth},
        {center.x - half, center.y + half + depth},
        {center.x - half + depth, center.y - half - depth},
        {center.x + half + depth, center.y - half - depth},
        {center.x + half + depth, center.y + half - depth},
        {center.x - half + depth, center.y + half - depth}
    }};

    const ImVec2 top[] = {points[0], points[1], points[5], points[4]};
    const ImVec2 front[] = {points[0], points[4], points[7], points[3]};
    const ImVec2 side[] = {points[1], points[2], points[6], points[5]};
    draw_list->AddConvexPolyFilled(top, 4, IM_COL32(87, 168, 229, 255));
    draw_list->AddConvexPolyFilled(front, 4, IM_COL32(47, 104, 158, 255));
    draw_list->AddConvexPolyFilled(side, 4, IM_COL32(35, 78, 126, 255));
    const ImU32 outline = selected ? IM_COL32(255, 191, 82, 255) : IM_COL32(154, 208, 248, 210);
    for (int index = 0; index < 4; ++index) {
        const int next = (index + 1) % 4;
        draw_list->AddLine(points[index], points[next], outline, selected ? 2.5F : 1.2F);
        draw_list->AddLine(points[index + 4], points[next + 4], outline, selected ? 2.5F : 1.2F);
        draw_list->AddLine(points[index], points[index + 4], outline, selected ? 2.5F : 1.2F);
    }
}

void draw_sphere(
    ImDrawList* draw_list,
    const ImVec2 center,
    const float radius,
    const bool selected) {
    draw_list->AddCircleFilled(center, radius, IM_COL32(199, 103, 133, 255), 48);
    draw_list->AddCircleFilled(
        {center.x - radius * 0.28F, center.y - radius * 0.32F},
        radius * 0.42F,
        IM_COL32(242, 156, 181, 180),
        32);
    draw_list->AddCircle(
        center,
        radius,
        selected ? IM_COL32(255, 191, 82, 255) : IM_COL32(255, 190, 211, 210),
        48,
        selected ? 2.5F : 1.2F);
    draw_list->AddEllipse(
        center,
        {radius, radius * 0.34F},
        IM_COL32(255, 205, 220, 110),
        0.0F,
        40,
        1.0F);
}

} // namespace

EditorUi::EditorUi(World& world, AssetRegistry& assets)
    : model_(world, assets),scripting_status_cache_(model_.scripting_status_json()) {
    reset_viewport_camera();
    synchronize_editor_context_revision();
}

void EditorUi::refresh_visible_state() {
    model_.refresh();
    scripting_status_cache_=model_.scripting_status_json();
    synchronize_editor_context_revision();
}

void EditorUi::refresh_world_model() {
    if(script_compile_busy_)return;
    refresh_visible_state();
}

bool EditorUi::select_asset(const std::string_view asset_id) noexcept {
    const auto* previous=model_.selected_asset();
    const auto previous_id=previous==nullptr?std::string_view{}:std::string_view(previous->id);
    const auto selected=model_.select_asset(asset_id);
    if(!selected)return false;
    animation_graph_focus_frames_=nlohmann::json::parse(
        model_.selected_animation_graph_authoring_json(),nullptr,false).value("valid",false)?3:0;
    const auto* current=model_.selected_asset();
    const auto current_id=current==nullptr?std::string_view{}:std::string_view(current->id);
    if(previous_id!=current_id)mark_editor_context_changed();
    return true;
}

std::string EditorUi::invoke_retained_authoring_action(
    const std::string_view action_id,
    const std::string_view binding_json,
    const std::string_view value_json) {
    constexpr std::size_t maximum_action_id_bytes = 128U;
    constexpr std::size_t maximum_binding_bytes = 16U * 1024U;
    constexpr std::size_t maximum_value_bytes = 16U * 1024U;
    const auto world_action = action_id == "outliner.create-empty" || action_id == "outliner.copy" ||
        action_id == "outliner.duplicate" || action_id == "outliner.paste";
    const auto asset_action = action_id == "asset.import" || action_id == "asset.inspect" ||
        action_id == "asset.build-preview" || action_id == "asset.cook";
    const auto source_revision_before = world_action ? model_.world_revision() : [&] {
        const auto status = nlohmann::json::parse(model_.asset_registry_status_json(), nullptr, false);
        return status.is_object() ? status.value("revision", std::uint64_t{}) : std::uint64_t{};
    }();
    const auto finish = [&](const bool success, std::string code, std::string detail,
                            const std::uint64_t revision_after,
                            const std::string_view entity_id = {},
                            const std::string_view asset_id = {}) {
        last_action_status_ = std::string(action_id.empty() ? "retained action" : action_id) + ": " + detail;
        synchronize_editor_context_revision();
        return nlohmann::json{
            {"schemaVersion", "noemancer.retained-authoring-action-receipt/0.1"},
            {"success", success}, {"code", std::move(code)}, {"detail", std::move(detail)},
            {"actionId", std::string(action_id.substr(0U, maximum_action_id_bytes))},
            {"sourceRevisionBefore", source_revision_before},
            {"sourceRevisionAfter", revision_after}, {"revision", revision_after},
            {"editorContextRevision", editor_context_revision_},
            {"entityId", entity_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(entity_id)},
            {"assetId", asset_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(asset_id)}}.dump();
    };
    const auto fail = [&](std::string code, std::string detail) {
        return finish(false, std::move(code), std::move(detail), source_revision_before);
    };
    if (action_id.empty() || action_id.size() > maximum_action_id_bytes)
        return fail("retained-action.action-id-invalid", "The retained action ID must be non-empty and bounded.");
    if (!world_action && !asset_action)
        return fail("retained-action.unsupported", "This retained action is unavailable or requires explicit confirmation/input.");
    if (binding_json.size() > maximum_binding_bytes || value_json.size() > maximum_value_bytes)
        return fail("retained-action.payload-too-large", "The retained action binding or value exceeds its 16 KiB budget.");
    const auto binding = nlohmann::json::parse(binding_json, nullptr, false);
    const auto value = nlohmann::json::parse(value_json, nullptr, false);
    if (!binding.is_object())
        return fail("retained-action.binding-invalid", "The retained action binding must be a JSON object.");
    if (!value.is_object())
        return fail("retained-action.value-invalid", "The retained action value must be a JSON object.");
    const auto source_revision = binding.find("sourceRevision");
    if (source_revision == binding.end() || !source_revision->is_number_unsigned())
        return fail("retained-action.source-revision-required", "The binding must contain an unsigned sourceRevision.");
    if (source_revision->get<std::uint64_t>() != source_revision_before)
        return fail("retained-action.stale-source-revision", "The retained document is stale; observe the surface again before invoking its action.");
    const auto stable_id = [](const std::string_view id) {
        if (id.empty() || id.size() > 256U) return false;
        return std::ranges::all_of(id, [](const unsigned char character) {
            return character >= 0x80U || std::isalnum(character) != 0 || character == '.' ||
                character == '_' || character == '-' || character == ':' || character == '/';
        });
    };

    EditorSceneAction result;
    std::string entity_id;
    std::string asset_id;
    if (world_action) {
        if (simulation_state_ != EditorSimulationState::edit || script_compile_busy_)
            return fail("retained-action.world-read-only", "Edit World authoring is unavailable while Play or script compilation owns the surface.");
        const auto expected_kind = action_id == "outliner.create-empty" ? "editor-entity-create" :
            action_id == "outliner.paste" ? "editor-entity-paste" : "editor-entity-action";
        if (binding.value("kind", std::string{}) != expected_kind)
            return fail("retained-action.binding-kind-mismatch", "The binding kind does not match this Outliner action.");
        if (expected_kind == std::string_view("editor-entity-action")) {
            entity_id = binding.value("entityId", std::string{});
            if (!stable_id(entity_id))
                return fail("retained-action.entity-id-invalid", "The binding must contain a stable entityId.");
            const auto expected_operation=action_id=="outliner.copy"?"copy":"duplicate";
            if(binding.value("operation",std::string{})!=expected_operation)
                return fail("retained-action.operation-mismatch", "The binding operation does not match this Outliner action.");
            if (model_.selected_object_ids().empty() || model_.selected_object().id != entity_id)
                return fail("retained-action.selection-mismatch", "The bound entity is no longer the primary EditorModel selection.");
        }
        if (action_id == "outliner.create-empty") {
            const auto display_name = value.value("displayName", std::string{"Empty Entity"});
            const auto parent_id = value.value("parentEntityId", std::string{});
            if (display_name.empty() || display_name.size() > 256U ||
                (!parent_id.empty() && !stable_id(parent_id)))
                return fail("retained-action.value-invalid", "Create Empty requires a bounded displayName and optional stable parentEntityId.");
            result = model_.create_empty_entity(display_name, parent_id);
        } else if (action_id == "outliner.copy") result = model_.copy_selected();
        else if (action_id == "outliner.duplicate") result = model_.duplicate_selected();
        else result = model_.paste_copied();
        entity_id = result.entity_id.empty() ? entity_id : result.entity_id;
    } else {
        if (binding.value("kind", std::string{}) != "editor-asset-action")
            return fail("retained-action.binding-kind-mismatch", "The binding kind does not match this Asset Browser action.");
        asset_id = binding.value("assetId", std::string{});
        if (!stable_id(asset_id))
            return fail("retained-action.asset-id-invalid", "The binding must contain a stable assetId.");
        const auto* selected = model_.selected_asset();
        if (selected == nullptr || selected->id != asset_id)
            return fail("retained-action.selection-mismatch", "The bound asset is no longer the EditorModel selection.");
        const auto expected_operation=action_id=="asset.import"?"import":action_id=="asset.inspect"?"inspect":
            action_id=="asset.build-preview"?"build-preview":"cook";
        if(binding.value("operation",std::string{})!=expected_operation)
            return fail("retained-action.operation-mismatch", "The binding operation does not match this Asset Browser action.");
        const auto job = nlohmann::json::parse(model_.active_asset_job_json(), nullptr, false);
        const auto job_state = job.is_object() ? job.value("state", std::string{"idle"}) : std::string{"invalid"};
        if (job_state == "queued" || job_state == "running" || job_state == "cancelling")
            return fail("retained-action.asset-job-busy", "Another Asset Job is active; wait, cancel, or reconcile it before invoking another action.");
        if (action_id == "asset.import") result = model_.import_selected_asset();
        else if (action_id == "asset.inspect") result = model_.inspect_selected_asset();
        else if (action_id == "asset.build-preview") result = model_.generate_selected_asset_thumbnail();
        else {
            const auto target = value.value("targetProfile", std::string{"windows-x64-debug"});
            if (!stable_id(target))
                return fail("retained-action.value-invalid", "Cook requires a stable targetProfile identifier.");
            result = model_.cook_selected_asset(target);
        }
    }
    if (result.success) refresh_visible_state();
    return finish(result.success, result.code, result.detail, result.revision, entity_id, asset_id);
}

void EditorUi::render() {
    poll_script_compile_job();
    ImGuizmo::BeginFrame();
    const auto now=std::chrono::steady_clock::now();
    if(!script_compile_busy_&&(last_model_refresh_.time_since_epoch().count()==0||
       now-last_model_refresh_>=std::chrono::milliseconds(100))) {
        refresh_world_model();evaluate_auto_compile();last_model_refresh_=now;
    }
    const auto& io = ImGui::GetIO();
    if (!script_compile_busy_&&simulation_state_==EditorSimulationState::edit && !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && model_.can_save_scene()) {
        const auto action=model_.save_scene();last_action_status_=action.detail;
        if(action.success)scene_recovery_candidates_json_=model_.scene_recovery_candidates_json(project_context_.root);
    } else if(!script_compile_busy_&&simulation_state_==EditorSimulationState::edit&&!io.WantTextInput&&io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_C,false)) {
        last_action_status_=model_.copy_selected().detail;
    } else if(!script_compile_busy_&&simulation_state_==EditorSimulationState::edit&&!io.WantTextInput&&io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_V,false)&&model_.can_paste()) {
        last_action_status_=model_.paste_copied().detail;
    } else if (!script_compile_busy_&&simulation_state_==EditorSimulationState::edit && !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && model_.can_undo()) {
        last_action_status_ = model_.undo().detail;
        model_.refresh();
    } else if (!script_compile_busy_&&simulation_state_==EditorSimulationState::edit && !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && model_.can_redo()) {
        last_action_status_ = model_.redo().detail;
        model_.refresh();
    }
    if (!layout_initialized_) {
        apply_editor_style();
    }
    if(startup_hub_open_) {
        draw_startup_hub();
        synchronize_editor_context_revision();
        return;
    }
    draw_root_dockspace();
    draw_scene_view();
    draw_animation_graph();
    draw_world_outliner();
    draw_inspector();
    draw_asset_browser();
    draw_console();
    draw_agent_context();
    if(project_settings_open_&&project_input_panel_)project_input_panel_->render();
    if(project_settings_open_&&hybrid_pixel_profile_panel_)hybrid_pixel_profile_panel_->render();
    if(project_settings_open_&&project_ui_panel_)project_ui_panel_->render();
    synchronize_editor_context_revision();
}

void EditorUi::set_engine_status(std::string status_json) {
    engine_status_json_ = std::move(status_json);
}

void EditorUi::set_render_surface(
    const std::uintptr_t texture_id,
    const std::uint32_t width,
    const std::uint32_t height) {
    scene_texture_id_ = texture_id;
    scene_texture_width_ = width;
    scene_texture_height_ = height;
    if(hybrid_pixel_profile_panel_)hybrid_pixel_profile_panel_->set_preview_extent(width,height);
}

void EditorUi::set_retained_inspector_surface(const std::uintptr_t texture_id,const std::uint32_t width,
                                               const std::uint32_t height) {
    retained_inspector_texture_id_=texture_id;
    retained_inspector_texture_width_=width;
    retained_inspector_texture_height_=height;
}

void EditorUi::set_retained_outliner_surface(const std::uintptr_t texture_id,const std::uint32_t width,
                                              const std::uint32_t height) {
    retained_outliner_texture_id_=texture_id;
    retained_outliner_texture_width_=width;
    retained_outliner_texture_height_=height;
    if(texture_id==0U) {
        retained_outliner_canvas_width_=0.0F;
        retained_outliner_canvas_height_=0.0F;
    }
}

void EditorUi::set_retained_asset_browser_surface(const std::uintptr_t texture_id,const std::uint32_t width,
                                                   const std::uint32_t height) {
    retained_asset_browser_texture_id_=texture_id;
    retained_asset_browser_texture_width_=width;
    retained_asset_browser_texture_height_=height;
    if(texture_id==0U) {
        retained_asset_browser_canvas_width_=0.0F;
        retained_asset_browser_canvas_height_=0.0F;
    }
}

void EditorUi::set_render_status(std::string status_json) {
    render_status_json_ = std::move(status_json);
}

void EditorUi::set_input_status(std::string status_json) {
    input_status_json_=std::move(status_json);
}
void EditorUi::set_project_input_capture(ProjectSettingsInputMapCaptureObservation observation) {
    if(project_input_panel_)project_input_panel_->set_capture_observation(std::move(observation));
}
void EditorUi::set_project_input_actions(std::vector<InputActionDefinition> actions,const std::uint64_t revision) {
    project_context_.input_actions=std::move(actions);project_context_.input_revision=revision;
    if(project_input_panel_)project_input_panel_->set_snapshot({project_context_.project_id,project_context_.name,
        project_context_.input_revision,project_context_.input_actions});
}
void EditorUi::set_project_hybrid_pixel_profile(std::optional<HybridPixelProfile> profile,
                                                const std::uint64_t revision,
                                                const bool can_undo,const bool can_redo) {
    project_context_.hybrid_pixel_profile=profile;
    project_context_.hybrid_pixel_profile_revision=revision;
    project_context_.hybrid_pixel_profile_can_undo=can_undo;
    project_context_.hybrid_pixel_profile_can_redo=can_redo;
    if(hybrid_pixel_profile_panel_) {
        hybrid_pixel_profile_panel_->set_snapshot(revision,std::move(profile));
        hybrid_pixel_profile_panel_->set_undo_redo_available(can_undo,can_redo);
    }
}
void EditorUi::set_project_ui_document(std::string document_json,const std::uint64_t revision,
                                       std::string fingerprint,const bool can_undo,const bool can_redo) {
    project_context_.project_ui_document_json=std::move(document_json);
    project_context_.project_ui_revision=revision;
    project_context_.project_ui_fingerprint=std::move(fingerprint);
    project_context_.project_ui_can_undo=can_undo;
    project_context_.project_ui_can_redo=can_redo;
    const ProjectUiAuthoringSnapshot snapshot{project_context_.project_ui_document_json,
        project_context_.project_ui_revision,project_context_.project_ui_fingerprint,
        project_context_.project_ui_can_undo,project_context_.project_ui_can_redo};
    if(project_ui_panel_)project_ui_panel_->set_snapshot(snapshot);
    else project_ui_panel_.emplace(snapshot);
}
std::optional<ProjectSettingsInputMapPanelRequest> EditorUi::consume_project_input_request() {
    return project_input_panel_?project_input_panel_->consume_request():std::nullopt;
}
std::optional<HybridPixelProfilePanelRequest> EditorUi::consume_hybrid_pixel_profile_request() {
    return hybrid_pixel_profile_panel_?hybrid_pixel_profile_panel_->consume_request():std::nullopt;
}
std::optional<ProjectUiAuthoringPanelRequest> EditorUi::consume_project_ui_request() {
    return project_ui_panel_?project_ui_panel_->consume_request():std::nullopt;
}
void EditorUi::set_asset_thumbnail_texture(std::string asset_id,const std::uintptr_t texture_id) {
    if(asset_id.empty())return;
    if(texture_id==0U)asset_thumbnail_textures_.erase(asset_id);
    else asset_thumbnail_textures_.insert_or_assign(std::move(asset_id),texture_id);
}
std::vector<EditorAssetThumbnailArtifact> EditorUi::asset_thumbnail_artifacts() const {
    std::vector<EditorAssetThumbnailArtifact> result;
    const auto document=nlohmann::json::parse(retained_asset_browser_document_json(),nullptr,false);
    if(!document.is_object())return result;
    for(const auto& node:document.value("nodes",nlohmann::json::array())) {
        if(!node.is_object()||node.value("role",std::string{})!="griditem")continue;
        const auto asset=node.value("asset",nlohmann::json::object());
        const auto thumbnail=asset.value("thumbnail",nlohmann::json::object());
        const auto asset_id=asset.value("id",std::string{});
        const auto uri=thumbnail.value("uri",std::string{});
        if(!asset_id.empty()&&thumbnail.value("cached",false)&&!uri.empty())
            result.push_back({asset_id,uri});
    }
    return result;
}
void EditorUi::set_project_context(EditorProjectContext context) {
    project_context_=std::move(context);model_.reset_for_loaded_project();
    if(project_context_.root!="engine://") {
        const auto opened=std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        startup_hub_.set_recent_projects({StartupHubRecentProject{
            project_context_.root,project_context_.name,static_cast<std::uint64_t>(std::max<std::int64_t>(0,opened))}});
        startup_hub_open_=false;
    }
    project_input_panel_.emplace(ProjectSettingsInputMapSnapshot{project_context_.project_id,project_context_.name,
        project_context_.input_revision,project_context_.input_actions});
    hybrid_pixel_profile_panel_.emplace(HybridPixelProfileSnapshot{
        project_context_.hybrid_pixel_profile_revision,project_context_.hybrid_pixel_profile});
    if(!project_context_.project_ui_document_json.empty())project_ui_panel_.emplace(ProjectUiAuthoringSnapshot{
        project_context_.project_ui_document_json,project_context_.project_ui_revision,
        project_context_.project_ui_fingerprint,project_context_.project_ui_can_undo,
        project_context_.project_ui_can_redo});
    else project_ui_panel_.reset();
    if(scene_texture_width_>0U&&scene_texture_height_>0U)
        hybrid_pixel_profile_panel_->set_preview_extent(scene_texture_width_,scene_texture_height_);
    hybrid_pixel_profile_panel_->set_undo_redo_available(project_context_.hybrid_pixel_profile_can_undo,
        project_context_.hybrid_pixel_profile_can_redo);
    scene_recovery_candidates_json_=model_.scene_recovery_candidates_json(project_context_.root);
    if(project_context_.root!="engine://") {
        const auto output=(std::filesystem::path(project_context_.root)/"dist"/(project_context_.name+"-windows-x64")).string();
        std::snprintf(package_output_path_.data(),package_output_path_.size(),"%s",output.c_str());
    }
    synchronize_editor_context_revision();
}

void EditorUi::draw_startup_hub() {
    const ImGuiViewport* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0.0F,0.0F});
    constexpr auto flags=ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoTitleBar|
        ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
        ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("Noemancer Project Hub",nullptr,flags);
    ImGui::PopStyleVar(3);

    const auto extent=ImGui::GetContentRegionAvail();
    const auto left_width=std::clamp(extent.x*0.37F,340.0F,560.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4{0.075F,0.133F,0.184F,1.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{48.0F,44.0F});
    if(ImGui::BeginChild("##startup-brand",{left_width,extent.y},ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) {
        const auto origin=ImGui::GetCursorScreenPos();
        constexpr float mark=92.0F;
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin,{origin.x+mark,origin.y+mark},IM_COL32(20,36,51,255),14.0F);
        const auto gold=IM_COL32(212,161,93,255),green=IM_COL32(110,154,139,255);
        const auto x=origin.x,y=origin.y;
        draw->AddLine({x+24,y+69},{x+24,y+23},gold,9.0F);
        draw->AddLine({x+24,y+23},{x+68,y+69},gold,9.0F);
        draw->AddLine({x+68,y+69},{x+68,y+23},gold,9.0F);
        draw->AddLine({x+18,y+76},{x+74,y+76},green,4.0F);
        ImGui::Dummy({mark,mark+38.0F});
        ImGui::SetWindowFontScale(2.0F);ImGui::TextUnformatted(startup_hub_.brand().title.c_str());ImGui::SetWindowFontScale(1.0F);
        ImGui::TextColored({0.83F,0.63F,0.36F,1.0F},"ENGINE / EDITOR");
        ImGui::Dummy({1.0F,22.0F});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+std::max(220.0F,left_width-96.0F));
        ImGui::TextColored({0.77F,0.83F,0.86F,1.0F},
            "Build worlds with one readable state shared by the editor, runtime, and coding agents.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy({1.0F,std::max(20.0F,extent.y-390.0F)});
        ImGui::TextDisabled("PRE-ALPHA  |  WINDOWS x64");
        ImGui::TextDisabled("Source-first. Agent-readable. Runtime-verifiable.");
    }
    ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();

    ImGui::SameLine(0.0F,0.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4{0.043F,0.051F,0.070F,1.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{44.0F,40.0F});
    if(ImGui::BeginChild("##startup-projects",{0,extent.y},ImGuiChildFlags_None)) {
        ImGui::SetWindowFontScale(1.45F);ImGui::TextUnformatted("Start a project");ImGui::SetWindowFontScale(1.0F);
        ImGui::TextDisabled("Open an existing workspace or create a clean Noemancer project.");
        ImGui::Dummy({1.0F,18.0F});

        if(project_dialog_mode_==0)project_dialog_mode_=2;
        if(ImGui::Button("Open Project",{150.0F,38.0F}))project_dialog_mode_=2;
        ImGui::SameLine();if(ImGui::Button("New Project",{150.0F,38.0F}))project_dialog_mode_=1;
        if(project_context_.root!="engine://") {
            ImGui::SameLine();if(ImGui::Button("Back to Editor",{150.0F,38.0F}))startup_hub_open_=false;
        } else {
            ImGui::SameLine();if(ImGui::Button("Empty Workspace",{150.0F,38.0F}))startup_hub_open_=false;
        }
        ImGui::Dummy({1.0F,14.0F});

        const auto creating=project_dialog_mode_==1;
        ImGui::TextUnformatted(creating?"Create a workspace":"Open a workspace");
        ImGui::TextDisabled(creating?"The target folder must not already contain a project.":
            "Use a project directory or its noemancer.project.json path.");
        if(creating) {
            ImGui::SetNextItemWidth(-1.0F);ImGui::InputText("Project name",project_name_.data(),project_name_.size());
            constexpr const char* presets[]={"Starter 3D","Hybrid Pixel / HD2D"};
            ImGui::SetNextItemWidth(-1.0F);ImGui::Combo("Project preset",&project_preset_index_,presets,2);
            ImGui::TextDisabled(project_preset_index_==1?
                "Pixel-stable 320 x 180 canvas, nearest integer presentation, and shared 2D/3D rendering.":
                "General-purpose scene, C# entry point, HUD, input, animation, and built-in assets.");
        }
        ImGui::SetNextItemWidth(-1.0F);ImGui::InputText("Project path",project_path_.data(),project_path_.size());
        const auto ready=project_path_[0]!='\0'&&(!creating||project_name_[0]!='\0');
        ImGui::BeginDisabled(!ready);
        if(ImGui::Button(creating?"Create and Open":"Open in Editor",{180.0F,38.0F})) {
            project_request_=EditorProjectRequest{creating?EditorProjectCommand::create:EditorProjectCommand::open,
                project_path_.data(),creating?project_name_.data():std::string{},
                creating&&project_preset_index_==1?"hybrid-pixel":"starter"};
        }
        ImGui::EndDisabled();

        ImGui::Dummy({1.0F,24.0F});ImGui::Separator();ImGui::Dummy({1.0F,14.0F});
        ImGui::TextUnformatted("Recent projects");
        const auto& recent=startup_hub_.view().recent_projects;
        if(recent.empty()) {
            ImGui::TextDisabled("No project has been opened in this session yet.");
            ImGui::TextDisabled("Choose a path above; recent workspaces will appear here.");
        }
        for(std::size_t index=0;index<recent.size();++index) {
            const auto& project=recent[index];ImGui::PushID(static_cast<int>(index));
            const auto available=project.status==StartupHubProjectStatus::available;
            ImGui::BeginDisabled(!available);
            if(ImGui::Selectable(project.display_name.c_str(),false,ImGuiSelectableFlags_None,{0,42.0F}))
                project_request_=EditorProjectRequest{EditorProjectCommand::open,project.path,{},"starter"};
            ImGui::EndDisabled();
            ImGui::SameLine(190.0F);ImGui::TextDisabled("%s",project.path.c_str());
            if(!available){ImGui::SameLine();ImGui::TextColored(color_warning,"%s",startup_hub_project_status_name(project.status));}
            ImGui::PopID();
        }
        const auto project_status=nlohmann::json::parse(project_status_json_,nullptr,false);
        if(project_status.is_object()&&!project_status.value("success",true)) {
            ImGui::Dummy({1.0F,12.0F});
            ImGui::TextColored(color_danger,"%s",project_status.value("detail",
                project_status.value("code",std::string{"Project operation failed."})).c_str());
        }
    }
    ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();
    ImGui::End();
}

void EditorUi::set_package_status(const bool busy,std::string status_json) {
    package_busy_=busy;package_status_json_=std::move(status_json);
}

std::optional<EditorPackageRequest> EditorUi::consume_package_request() {
    auto result=std::move(package_request_);package_request_.reset();return result;
}

void EditorUi::set_project_status(std::string status_json) {project_status_json_=std::move(status_json);}
std::optional<EditorProjectRequest> EditorUi::consume_project_request() {
    auto result=std::move(project_request_);project_request_.reset();return result;
}
bool EditorUi::consume_exit_request() noexcept {const auto result=exit_requested_;exit_requested_=false;return result;}
void EditorUi::request_close() {if(model_.scene_dirty())close_dialog_open_=true;else exit_requested_=true;}
std::string EditorUi::compile_scripts(const std::string_view configuration) {
    if(script_compile_busy_)return nlohmann::json{{"schemaVersion","noemancer.script-compile-result/0.1"},{"success",false},
        {"code","scripting.compile-job-busy"},{"configuration",configuration},{"cacheHit",false},{"diagnostics",nlohmann::json::array()}}.dump();
    last_script_compile_json_=model_.compile_scripts_json(configuration);
    scripting_status_cache_=model_.scripting_status_json();
    return last_script_compile_json_;
}

bool EditorUi::begin_compile_scripts(const std::string_view configuration) {
    if(script_compile_busy_||(configuration!="Debug"&&configuration!="Release"))return false;
    const auto config=std::string(configuration);const auto job_id=++script_compile_job_sequence_;
    script_compile_busy_=true;script_compile_started_=std::chrono::steady_clock::now();
    script_compile_job_json_=nlohmann::json{{"schemaVersion","noemancer.editor-script-build-job/0.1"},{"jobId",job_id},
        {"state","running"},{"configuration",config},{"result",nullptr},{"elapsedMilliseconds",0}}.dump();
    script_compile_future_=std::async(std::launch::async,[this,config]{return model_.compile_scripts_json(config);});
    return true;
}

bool EditorUi::script_compile_busy() const noexcept {return script_compile_busy_;}
std::optional<EditorSourceOpenRequest> EditorUi::consume_source_open_request() {
    auto result=std::move(source_open_request_);source_open_request_.reset();return result;
}
std::optional<EditorScriptBuildCompletion> EditorUi::consume_script_build_completion() {
    auto result=std::move(script_build_completion_);script_build_completion_.reset();return result;
}
void EditorUi::set_play_script_reload_status(std::string status_json) {play_script_reload_json_=std::move(status_json);}
void EditorUi::set_auto_compile_scripts(const bool enabled) noexcept {
    auto_compile_scripts_=enabled;
    if(!enabled){auto_compile_candidate_fingerprint_.clear();auto_compile_blocked_fingerprint_.clear();auto_compile_candidate_since_={};}
}
bool EditorUi::auto_compile_scripts() const noexcept {return auto_compile_scripts_;}

void EditorUi::evaluate_auto_compile() {
    if(!auto_compile_scripts_||script_compile_busy_)return;
    const auto status=nlohmann::json::parse(scripting_status_cache_,nullptr,false);
    if(!status.is_object()||!status.value("project",nlohmann::json{}).is_object())return;
    const auto& project=status.at("project");const auto source=project.value("sourceState",nlohmann::json::object());
    if(!project.value("configured",false)||!source.value("needsCompile",false)) {
        auto_compile_candidate_fingerprint_.clear();auto_compile_candidate_since_={};return;
    }
    const auto fingerprint=source.value("observedFingerprint",std::string{});if(fingerprint.empty())return;
    if(fingerprint==auto_compile_blocked_fingerprint_)return;
    if(!auto_compile_blocked_fingerprint_.empty()&&fingerprint!=auto_compile_blocked_fingerprint_)
        auto_compile_blocked_fingerprint_.clear();
    const auto now=std::chrono::steady_clock::now();
    if(fingerprint!=auto_compile_candidate_fingerprint_) {
        auto_compile_candidate_fingerprint_=fingerprint;auto_compile_candidate_since_=now;return;
    }
    if(now-auto_compile_candidate_since_<std::chrono::milliseconds(750))return;
    if(begin_compile_scripts("Debug")) {
        auto job=nlohmann::json::parse(script_compile_job_json_);job["trigger"]="auto-source-change";
        job["sourceFingerprint"]=fingerprint;script_compile_job_json_=job.dump();
        auto_compile_candidate_fingerprint_.clear();auto_compile_candidate_since_={};
    }
}

void EditorUi::poll_script_compile_job() {
    if(!script_compile_busy_||!script_compile_future_.valid())return;
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-script_compile_started_).count();
    auto job=nlohmann::json::parse(script_compile_job_json_);job["elapsedMilliseconds"]=elapsed;
    if(script_compile_future_.wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
        last_script_compile_json_=script_compile_future_.get();script_compile_busy_=false;
        const auto result=nlohmann::json::parse(last_script_compile_json_,nullptr,false);
        const auto success=result.is_object()&&result.value("success",false);
        job["state"]=success?"succeeded":"failed";job["result"]=result;
        const auto trigger=job.value("trigger",std::string{"manual"});const auto fingerprint=job.value("sourceFingerprint",std::string{});
        if(trigger=="auto-source-change"&&!success)auto_compile_blocked_fingerprint_=fingerprint;
        else if(success)auto_compile_blocked_fingerprint_.clear();
        script_build_completion_=EditorScriptBuildCompletion{job.value("configuration",std::string{"Debug"}),trigger,last_script_compile_json_};
        scripting_status_cache_=model_.scripting_status_json();
    }
    script_compile_job_json_=job.dump();
}

bool EditorUi::open_script_source(const std::string_view source_path,const std::uint32_t line,const std::uint32_t column) {
    const auto scripting=nlohmann::json::parse(scripting_status_cache_,nullptr,false);
    if(!scripting.is_object()||!scripting.value("project",nlohmann::json{}).is_object())return false;
    const auto root_text=scripting.at("project").value("projectRoot",std::string{});
    if(root_text.empty()||source_path.empty())return false;
    std::error_code error;
    const auto root=std::filesystem::weakly_canonical(root_text,error);if(error)return false;
    auto source=std::filesystem::path(source_path);if(source.is_relative())source=root/source;
    source=std::filesystem::weakly_canonical(source,error);if(error)return false;
    const auto relative=source.lexically_relative(root);
    if(relative.empty()||relative.begin()->string()==".."||!std::filesystem::is_regular_file(source,error))return false;
    std::ifstream stream(source,std::ios::binary);if(!stream)return false;
    nlohmann::json excerpt=nlohmann::json::array();std::string text;std::uint32_t current=1;
    const auto focus=std::max(line,1U);const auto first=focus>4?focus-4:1U;const auto last=focus+4;
    while(current<=last&&std::getline(stream,text)) {
        if(text.size()>512U)text.resize(512U);
        if(current>=first)excerpt.push_back({{"line",current},{"text",text},{"focus",current==focus}});
        ++current;
    }
    script_source_location_json_=nlohmann::json{{"schemaVersion","noemancer.editor-source-location/0.1"},
        {"path",source.generic_string()},{"projectRelativePath",relative.generic_string()},{"line",focus},
        {"column",std::max(column,1U)},{"excerpt",std::move(excerpt)},{"bounded",true}}.dump();
    return true;
}

std::uint32_t EditorUi::requested_scene_width() const { return requested_scene_width_; }
std::uint32_t EditorUi::requested_scene_height() const { return requested_scene_height_; }
std::uint32_t EditorUi::requested_inspector_width() const noexcept {return requested_inspector_width_;}
std::uint32_t EditorUi::requested_inspector_height() const noexcept {return requested_inspector_height_;}
std::string EditorUi::retained_inspector_document_json() const {return model_.inspector_semantic_ui_document_json();}
std::uint32_t EditorUi::requested_outliner_width() const noexcept {return requested_outliner_width_;}
std::uint32_t EditorUi::requested_outliner_height() const noexcept {return requested_outliner_height_;}
std::string EditorUi::retained_outliner_document_json() const {
    if(simulation_state_==EditorSimulationState::edit)return model_.outliner_semantic_ui_document_json();
    const auto observation=nlohmann::json::parse(play_world_observation_json_,nullptr,false);
    std::vector<EditorObject> objects;
    if(observation.is_object())for(const auto& entity:observation.value("entities",nlohmann::json::array())) {
        if(!entity.is_object())continue;
        const auto id=entity.value("id",std::string{});if(id.empty())continue;
        const auto parent=entity.value("parentId",nlohmann::json(nullptr));
        objects.push_back({.id=id,.name=entity.value("displayName",entity.value("name",id)),
            .kind=entity.value("type",std::string{"entity"}),
            .parent_id=parent.is_string()?parent.get<std::string>():std::string{},
            .revision=entity.value("revision",0ULL)});
    }
    std::vector<std::string> selection;
    if(!play_world_selected_entity_id_.empty())selection.push_back(play_world_selected_entity_id_);
    return model_.outliner_semantic_ui_document_json(EditorOutlinerAuthorityView{
        .authority="play-world-read-only",.simulation_state=simulation_state_==EditorSimulationState::paused?"paused":"playing",
        .writable=false,.world_revision=observation.is_object()?observation.value("revision",0ULL):0ULL,
        .objects=objects,.selected_object_ids=selection,.primary_selected_object_id=play_world_selected_entity_id_});
}
std::uint32_t EditorUi::requested_asset_browser_width() const noexcept {return requested_asset_browser_width_;}
std::uint32_t EditorUi::requested_asset_browser_height() const noexcept {return requested_asset_browser_height_;}
void EditorUi::synchronize_asset_browser_navigation() const {
    const auto visible_query = std::string(asset_browser_filter_.data());
    if (visible_query != asset_browser_query_) {
        asset_browser_query_ = visible_query;
        asset_browser_cursor_ = 0U;
    }
    asset_browser_page_size_ = std::clamp<std::size_t>(asset_browser_page_size_, 1U, 256U);
    const auto projection = nlohmann::json::parse(model_.asset_browser_semantic_ui_document_json({
        .query = asset_browser_query_, .cursor = asset_browser_cursor_,
        .page_limit = asset_browser_page_size_}), nullptr, false);
    if (!projection.is_object()) {
        asset_browser_cursor_ = 0U;
        return;
    }
    const auto revision = projection.value("revision", 0ULL);
    const auto page = projection.value("page", nlohmann::json::object());
    const auto matched = page.value("matched", std::size_t{});
    if (revision != asset_browser_registry_revision_) {
        asset_browser_registry_revision_ = revision;
        if (matched == 0U) asset_browser_cursor_ = 0U;
        else if (asset_browser_cursor_ >= matched)
            asset_browser_cursor_ = ((matched - 1U) / asset_browser_page_size_) * asset_browser_page_size_;
    } else if (asset_browser_cursor_ >= matched && matched > 0U) {
        asset_browser_cursor_ = matched == 0U ? 0U :
            ((matched - 1U) / asset_browser_page_size_) * asset_browser_page_size_;
    }
}
std::string EditorUi::retained_asset_browser_document_json() const {
    synchronize_asset_browser_navigation();
    return model_.asset_browser_semantic_ui_document_json({
        .query=asset_browser_query_,.cursor=asset_browser_cursor_,.page_limit=asset_browser_page_size_});
}
void EditorUi::set_asset_browser_query(const std::string_view query) {
    const auto count = std::min(query.size(), asset_browser_filter_.size() - 1U);
    const auto normalized = std::string(query.substr(0U, count));
    std::fill(asset_browser_filter_.begin(), asset_browser_filter_.end(), '\0');
    std::memcpy(asset_browser_filter_.data(), normalized.data(), normalized.size());
    asset_browser_query_ = normalized;
    asset_browser_cursor_ = 0U;
}
void EditorUi::set_asset_browser_cursor(const std::size_t cursor) noexcept {
    const auto page_size = std::max<std::size_t>(asset_browser_page_size_, 1U);
    asset_browser_cursor_ = (cursor / page_size) * page_size;
}
void EditorUi::set_asset_browser_page_size(const std::size_t page_size) noexcept {
    const auto normalized = std::clamp<std::size_t>(page_size, 1U, 256U);
    if (normalized == asset_browser_page_size_) return;
    asset_browser_page_size_ = normalized;
    asset_browser_cursor_ = 0U;
}
bool EditorUi::asset_browser_next_page() {
    synchronize_asset_browser_navigation();
    const auto projection = nlohmann::json::parse(retained_asset_browser_document_json(), nullptr, false);
    if (!projection.is_object()) return false;
    const auto next = projection.value("page", nlohmann::json::object()).value(
        "nextCursor", nlohmann::json(nullptr));
    if (!next.is_number_unsigned()) return false;
    asset_browser_cursor_ = next.get<std::size_t>();
    return true;
}
bool EditorUi::asset_browser_previous_page() {
    synchronize_asset_browser_navigation();
    if (asset_browser_cursor_ == 0U) return false;
    asset_browser_cursor_ = asset_browser_cursor_ > asset_browser_page_size_ ?
        asset_browser_cursor_ - asset_browser_page_size_ : 0U;
    return true;
}
std::size_t EditorUi::asset_browser_cursor() const noexcept { return asset_browser_cursor_; }
std::size_t EditorUi::asset_browser_page_size() const noexcept { return asset_browser_page_size_; }
float EditorUi::requested_exposure() const { return requested_exposure_; }
void EditorUi::set_exposure(const float exposure) { requested_exposure_=std::clamp(exposure,0.125F,8.0F); }
std::optional<RenderCameraSnapshot> EditorUi::render_camera_override() const {
    return simulation_state_==EditorSimulationState::edit?editor_camera_:std::nullopt;
}

void EditorUi::reset_viewport_camera() {
    const auto camera=model_.viewport_camera();
    if(!camera) { editor_camera_.reset(); return; }
    editor_camera_=RenderCameraSnapshot{"editor.viewport.camera",
        {camera->transform.x,camera->transform.y,camera->transform.z},
        {camera->camera.target_x,camera->camera.target_y,camera->camera.target_z},
        camera->camera.vertical_fov_degrees,camera->camera.near_clip,camera->camera.far_clip,
        camera->camera.projection,camera->camera.orthographic_height};
}

void EditorUi::update_viewport_camera_navigation(const bool hovered) {
    if(!hovered||simulation_state_!=EditorSimulationState::edit||!editor_camera_) return;
    auto& camera=*editor_camera_;auto& io=ImGui::GetIO();
    GizmoVec3 eye{camera.position[0],camera.position[1],camera.position[2]};
    GizmoVec3 target{camera.target[0],camera.target[1],camera.target[2]};
    auto offset=eye-target;auto distance=std::max(std::sqrt(dot(offset,offset)),0.25F);
    if(ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        auto yaw=std::atan2(offset.x,offset.z)-io.MouseDelta.x*0.006F;
        auto pitch=std::clamp(std::asin(std::clamp(offset.y/distance,-1.0F,1.0F))+io.MouseDelta.y*0.006F,-1.553F,1.553F);
        const auto horizontal=std::cos(pitch)*distance;
        eye={target.x+std::sin(yaw)*horizontal,target.y+std::sin(pitch)*distance,target.z+std::cos(yaw)*horizontal};
    }
    auto forward=normalize(target-eye);auto right=normalize(cross(forward,{0,1,0}));auto up=normalize(cross(right,forward));
    if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const auto amount=distance*0.0015F;
        const GizmoVec3 delta{right.x*(-io.MouseDelta.x*amount)+up.x*(io.MouseDelta.y*amount),
            right.y*(-io.MouseDelta.x*amount)+up.y*(io.MouseDelta.y*amount),
            right.z*(-io.MouseDelta.x*amount)+up.z*(io.MouseDelta.y*amount)};
        eye={eye.x+delta.x,eye.y+delta.y,eye.z+delta.z};target={target.x+delta.x,target.y+delta.y,target.z+delta.z};
    }
    if(io.MouseWheel!=0.0F) {
        distance=std::clamp(distance*std::exp(-io.MouseWheel*0.14F),0.25F,500.0F);
        eye={target.x-forward.x*distance,target.y-forward.y*distance,target.z-forward.z*distance};
    }
    if(ImGui::IsMouseDown(ImGuiMouseButton_Right)&&!io.WantTextInput) {
        const auto speed=std::max(distance*0.7F,1.0F)*io.DeltaTime*(io.KeyShift?3.0F:1.0F);
        GizmoVec3 move{};
        if(ImGui::IsKeyDown(ImGuiKey_W)) move={move.x+forward.x,move.y+forward.y,move.z+forward.z};
        if(ImGui::IsKeyDown(ImGuiKey_S)) move={move.x-forward.x,move.y-forward.y,move.z-forward.z};
        if(ImGui::IsKeyDown(ImGuiKey_D)) move={move.x+right.x,move.y+right.y,move.z+right.z};
        if(ImGui::IsKeyDown(ImGuiKey_A)) move={move.x-right.x,move.y-right.y,move.z-right.z};
        if(ImGui::IsKeyDown(ImGuiKey_E)) move.y+=1.0F;if(ImGui::IsKeyDown(ImGuiKey_Q)) move.y-=1.0F;
        if(dot(move,move)>0.0F) { move=normalize(move);move={move.x*speed,move.y*speed,move.z*speed};
            eye={eye.x+move.x,eye.y+move.y,eye.z+move.z};target={target.x+move.x,target.y+move.y,target.z+move.z}; }
    }
    if(ImGui::IsKeyPressed(ImGuiKey_F,false)&&!model_.objects().empty()&&model_.selected_object().transform) {
        const auto& selected=*model_.selected_object().transform;target={selected.x,selected.y,selected.z};
        eye={target.x-forward.x*distance,target.y-forward.y*distance,target.z-forward.z*distance};
    }
    camera.position={eye.x,eye.y,eye.z};camera.target={target.x,target.y,target.z};
}

std::optional<ScenePickRequest> EditorUi::consume_scene_pick_request() {
    auto request = scene_pick_request_;
    scene_pick_request_.reset();
    return request;
}

std::optional<ScenePointerPosition> EditorUi::scene_pointer_at(const float window_x, const float window_y) const {
    if (scene_canvas_width_ <= 0.0F || scene_canvas_height_ <= 0.0F ||
        window_x < scene_canvas_x_ || window_y < scene_canvas_y_ ||
        window_x >= scene_canvas_x_ + scene_canvas_width_ || window_y >= scene_canvas_y_ + scene_canvas_height_)
        return std::nullopt;
    const auto u = std::clamp((window_x - scene_canvas_x_) / scene_canvas_width_, 0.0F, 0.999999F);
    const auto v = std::clamp((window_y - scene_canvas_y_) / scene_canvas_height_, 0.0F, 0.999999F);
    return ScenePointerPosition{
        static_cast<std::int32_t>(u * static_cast<float>(scene_texture_width_)),
        static_cast<std::int32_t>(v * static_cast<float>(scene_texture_height_))};
}

std::optional<ScenePointerPosition> EditorUi::retained_inspector_pointer_at(const float window_x,const float window_y) const {
    if(retained_inspector_texture_id_==0||retained_inspector_canvas_width_<=0.0F||retained_inspector_canvas_height_<=0.0F||
       window_x<retained_inspector_canvas_x_||window_y<retained_inspector_canvas_y_||
       window_x>=retained_inspector_canvas_x_+retained_inspector_canvas_width_||
       window_y>=retained_inspector_canvas_y_+retained_inspector_canvas_height_)return std::nullopt;
    return ScenePointerPosition{
        std::clamp(static_cast<std::int32_t>((window_x-retained_inspector_canvas_x_)*retained_inspector_texture_width_/retained_inspector_canvas_width_),0,
                   static_cast<std::int32_t>(retained_inspector_texture_width_-1U)),
        std::clamp(static_cast<std::int32_t>((window_y-retained_inspector_canvas_y_)*retained_inspector_texture_height_/retained_inspector_canvas_height_),0,
                   static_cast<std::int32_t>(retained_inspector_texture_height_-1U))};
}

std::optional<ScenePointerPosition> EditorUi::retained_outliner_pointer_at(const float window_x,const float window_y) const {
    if(retained_outliner_texture_id_==0||retained_outliner_texture_width_==0||retained_outliner_texture_height_==0||
       retained_outliner_canvas_width_<=0.0F||retained_outliner_canvas_height_<=0.0F||
       window_x<retained_outliner_canvas_x_||window_y<retained_outliner_canvas_y_||
       window_x>=retained_outliner_canvas_x_+retained_outliner_canvas_width_||
       window_y>=retained_outliner_canvas_y_+retained_outliner_canvas_height_)return std::nullopt;
    return ScenePointerPosition{
        std::clamp(static_cast<std::int32_t>((window_x-retained_outliner_canvas_x_)*retained_outliner_texture_width_/
            retained_outliner_canvas_width_),0,static_cast<std::int32_t>(retained_outliner_texture_width_-1U)),
        std::clamp(static_cast<std::int32_t>((window_y-retained_outliner_canvas_y_)*retained_outliner_texture_height_/
            retained_outliner_canvas_height_),0,static_cast<std::int32_t>(retained_outliner_texture_height_-1U))};
}

std::optional<ScenePointerPosition> EditorUi::retained_asset_browser_pointer_at(const float window_x,
                                                                                const float window_y) const {
    if(retained_asset_browser_texture_id_==0||retained_asset_browser_texture_width_==0||
       retained_asset_browser_texture_height_==0||retained_asset_browser_canvas_width_<=0.0F||
       retained_asset_browser_canvas_height_<=0.0F||window_x<retained_asset_browser_canvas_x_||
       window_y<retained_asset_browser_canvas_y_||
       window_x>=retained_asset_browser_canvas_x_+retained_asset_browser_canvas_width_||
       window_y>=retained_asset_browser_canvas_y_+retained_asset_browser_canvas_height_)return std::nullopt;
    return ScenePointerPosition{
        std::clamp(static_cast<std::int32_t>((window_x-retained_asset_browser_canvas_x_)*
            retained_asset_browser_texture_width_/retained_asset_browser_canvas_width_),0,
            static_cast<std::int32_t>(retained_asset_browser_texture_width_-1U)),
        std::clamp(static_cast<std::int32_t>((window_y-retained_asset_browser_canvas_y_)*
            retained_asset_browser_texture_height_/retained_asset_browser_canvas_height_),0,
            static_cast<std::int32_t>(retained_asset_browser_texture_height_-1U))};
}

std::optional<SceneWindowPosition> EditorUi::scene_window_at(const std::int32_t scene_x, const std::int32_t scene_y) const {
    if (scene_canvas_width_ <= 0.0F || scene_canvas_height_ <= 0.0F ||
        scene_texture_width_ == 0 || scene_texture_height_ == 0) return std::nullopt;
    const auto u=std::clamp(static_cast<float>(scene_x)/static_cast<float>(scene_texture_width_),0.0F,1.0F);
    const auto v=std::clamp(static_cast<float>(scene_y)/static_cast<float>(scene_texture_height_),0.0F,1.0F);
    return SceneWindowPosition{
        static_cast<std::int32_t>(std::lround(scene_canvas_x_+u*scene_canvas_width_)),
        static_cast<std::int32_t>(std::lround(scene_canvas_y_+v*scene_canvas_height_)),
        scene_canvas_height_/static_cast<float>(scene_texture_height_)};
}

std::optional<SceneWindowPosition> EditorUi::retained_inspector_window_at(const std::int32_t surface_x,
                                                                          const std::int32_t surface_y) const {
    if(retained_inspector_texture_id_==0||retained_inspector_texture_width_==0||retained_inspector_texture_height_==0||
       retained_inspector_canvas_width_<=0.0F||retained_inspector_canvas_height_<=0.0F)return std::nullopt;
    return SceneWindowPosition{
        static_cast<std::int32_t>(std::lround(retained_inspector_canvas_x_+surface_x*retained_inspector_canvas_width_/
            retained_inspector_texture_width_)),
        static_cast<std::int32_t>(std::lround(retained_inspector_canvas_y_+surface_y*retained_inspector_canvas_height_/
            retained_inspector_texture_height_)),
        retained_inspector_canvas_height_/retained_inspector_texture_height_};
}

std::optional<SceneWindowPosition> EditorUi::retained_outliner_window_at(const std::int32_t surface_x,
                                                                         const std::int32_t surface_y) const {
    if(retained_outliner_texture_id_==0||retained_outliner_texture_width_==0||retained_outliner_texture_height_==0||
       retained_outliner_canvas_width_<=0.0F||retained_outliner_canvas_height_<=0.0F||surface_x<0||surface_y<0||
       surface_x>=static_cast<std::int32_t>(retained_outliner_texture_width_)||
       surface_y>=static_cast<std::int32_t>(retained_outliner_texture_height_))return std::nullopt;
    return SceneWindowPosition{
        static_cast<std::int32_t>(std::lround(retained_outliner_canvas_x_+surface_x*retained_outliner_canvas_width_/
            retained_outliner_texture_width_)),
        static_cast<std::int32_t>(std::lround(retained_outliner_canvas_y_+surface_y*retained_outliner_canvas_height_/
            retained_outliner_texture_height_)),
        retained_outliner_canvas_height_/retained_outliner_texture_height_};
}

std::optional<SceneWindowPosition> EditorUi::retained_asset_browser_window_at(const std::int32_t surface_x,
                                                                              const std::int32_t surface_y) const {
    if(retained_asset_browser_texture_id_==0||retained_asset_browser_texture_width_==0||
       retained_asset_browser_texture_height_==0||retained_asset_browser_canvas_width_<=0.0F||
       retained_asset_browser_canvas_height_<=0.0F||surface_x<0||surface_y<0||
       surface_x>=static_cast<std::int32_t>(retained_asset_browser_texture_width_)||
       surface_y>=static_cast<std::int32_t>(retained_asset_browser_texture_height_))return std::nullopt;
    return SceneWindowPosition{
        static_cast<std::int32_t>(std::lround(retained_asset_browser_canvas_x_+surface_x*
            retained_asset_browser_canvas_width_/retained_asset_browser_texture_width_)),
        static_cast<std::int32_t>(std::lround(retained_asset_browser_canvas_y_+surface_y*
            retained_asset_browser_canvas_height_/retained_asset_browser_texture_height_)),
        retained_asset_browser_canvas_height_/retained_asset_browser_texture_height_};
}

bool EditorUi::select_entity(const std::string_view entity_id) {
    if(simulation_state_!=EditorSimulationState::edit) {
        if(!entity_exists_in_play_world(entity_id)||play_world_selected_entity_id_==entity_id)return false;
        play_world_selected_entity_id_=std::string(entity_id);
        mark_editor_context_changed();
        return true;
    }
    const auto before=model_.selected_object_ids();
    const auto selected=model_.select_object(entity_id);
    if(selected&&before!=model_.selected_object_ids())mark_editor_context_changed();
    return selected;
}

void EditorUi::set_simulation_state(const EditorSimulationState state) noexcept {
    if(simulation_state_==state)return;
    simulation_state_=state;
    if(state==EditorSimulationState::edit) {
        play_world_observation_json_.clear();play_world_inspector_json_.clear();play_world_apply_plan_json_.clear();
        play_world_selected_entity_id_.clear();selected_play_world_change_ids_.clear();known_play_world_change_ids_.clear();
    } else if(play_world_selected_entity_id_.empty()&&!model_.objects().empty())play_world_selected_entity_id_=model_.selected_object().id;
    mark_editor_context_changed();
}
void EditorUi::set_play_world_context(std::string observation_json,std::string inspector_json,std::string apply_plan_json) {
    play_world_observation_json_=std::move(observation_json);play_world_inspector_json_=std::move(inspector_json);
    play_world_apply_plan_json_=std::move(apply_plan_json);
    const auto observation=nlohmann::json::parse(play_world_observation_json_,nullptr,false);
    const auto entities=observation.is_object()?observation.value("entities",nlohmann::json::array()):nlohmann::json::array();
    const auto selection_exists=std::ranges::any_of(entities,[&](const nlohmann::json& entity){return entity.value("id",std::string{})==play_world_selected_entity_id_;});
    if(!selection_exists)play_world_selected_entity_id_=entities.empty()?std::string{}:entities.front().value("id",std::string{});
    const auto plan=nlohmann::json::parse(play_world_apply_plan_json_,nullptr,false);
    std::unordered_set<std::string> current;
    if(plan.is_object())for(const auto& change:plan.value("changes",nlohmann::json::array())) {
        const auto id=change.value("changeId",std::string{});if(id.empty())continue;current.insert(id);
        if(!known_play_world_change_ids_.contains(id))selected_play_world_change_ids_.insert(id);
    }
    std::erase_if(selected_play_world_change_ids_,[&](const std::string& id){return !current.contains(id);});
    known_play_world_change_ids_=std::move(current);
    synchronize_editor_context_revision();
}
const std::string& EditorUi::play_world_selected_entity_id() const noexcept {return play_world_selected_entity_id_;}
std::vector<std::string> EditorUi::selected_play_world_change_ids() const {
    std::vector<std::string> result(selected_play_world_change_ids_.begin(),selected_play_world_change_ids_.end());
    std::ranges::sort(result);return result;
}
void EditorUi::set_last_action_status(std::string status) { last_action_status_=std::move(status); }
EditorSimulationState EditorUi::simulation_state() const noexcept { return simulation_state_; }
std::optional<EditorSimulationCommand> EditorUi::consume_simulation_command() {
    auto command=simulation_command_;
    simulation_command_.reset();
    return command;
}
void EditorUi::set_managed_debug_context(std::string events_json,std::string last_action_json) {
    auto accumulated=nlohmann::json::parse(managed_debug_events_json_,nullptr,false);
    auto incoming=nlohmann::json::parse(events_json,nullptr,false);
    auto events=accumulated.is_object()?accumulated.value("events",nlohmann::json::array()):nlohmann::json::array();
    if(incoming.is_object())for(auto& event:incoming.value("events",nlohmann::json::array()))events.push_back(std::move(event));
    constexpr std::size_t event_budget=128U;
    const auto truncated=events.size()>event_budget;
    if(truncated)events.erase(events.begin(),events.begin()+static_cast<nlohmann::json::difference_type>(events.size()-event_budget));
    managed_debug_events_json_=nlohmann::json{{"schemaVersion","noemancer.editor-managed-debug-events/0.1"},
        {"eventCount",events.size()},{"truncated",truncated},{"events",std::move(events)}}.dump();
    if(!last_action_json.empty())managed_debug_last_action_json_=std::move(last_action_json);
}
std::optional<EditorManagedDebugRequest> EditorUi::consume_managed_debug_request() {
    auto request=managed_debug_request_;managed_debug_request_.reset();return request;
}

void EditorUi::mark_editor_context_changed() noexcept {
    ++editor_context_revision_;
    editor_context_signature_.clear();
}

std::string EditorUi::editor_context_signature() const {
    std::string result;
    const auto append=[&](const std::string_view value) {
        result+=std::to_string(value.size());
        result.push_back(':');
        result.append(value);
        result.push_back('|');
    };
    append(simulation_state_==EditorSimulationState::edit?"edit":
        simulation_state_==EditorSimulationState::playing?"playing":"paused");
    append(project_context_.project_id);append(project_context_.root);append(project_context_.startup_scene);
    append(model_.scene_source());append(std::to_string(model_.world_revision()));
    for(const auto& id:model_.selected_object_ids())append(id);
    if(const auto* asset=model_.selected_asset();asset!=nullptr)append(asset->id);
    append(model_.focused_panel());
    append(active_tab_id_?std::string_view(*active_tab_id_):std::string_view{});
    append(startup_hub_open_?"startup-visible":"startup-hidden");
    append(play_world_selected_entity_id_);
    append(last_action_status_);append(script_compile_busy_?"build-busy":"build-idle");
    append(model_.can_undo()?"undo-available":"undo-empty");
    append(model_.can_redo()?"redo-available":"redo-empty");
    auto change_ids=selected_play_world_change_ids();
    for(const auto& id:change_ids)append(id);
    return result;
}

void EditorUi::synchronize_editor_context_revision() {
    auto signature=editor_context_signature();
    if(editor_context_signature_.empty()) {
        editor_context_signature_=std::move(signature);
        return;
    }
    if(signature!=editor_context_signature_) {
        ++editor_context_revision_;
        editor_context_signature_=std::move(signature);
    }
}

void EditorUi::set_focused_panel(const std::string_view panel_id) {
    const auto before=model_.focused_panel();
    const auto before_tab=active_tab_id_;
    model_.set_focused_panel(panel_id);
    if(ImGui::IsWindowDocked())active_tab_id_=std::string(panel_id);
    else active_tab_id_.reset();
    if(before!=model_.focused_panel()||before_tab!=active_tab_id_)mark_editor_context_changed();
}

void EditorUi::prepare_panel_window(const std::string_view panel_id) {
    if(pending_panel_focus_id_!=panel_id)return;
    ImGui::SetNextWindowFocus();
    pending_panel_focus_id_.clear();
}

bool EditorUi::entity_exists_in_play_world(const std::string_view entity_id) const {
    if(entity_id.empty())return false;
    const auto observation=nlohmann::json::parse(play_world_observation_json_,nullptr,false);
    if(!observation.is_object())return false;
    const auto iterator=observation.find("entities");
    if(iterator==observation.end()||!iterator->is_array())return false;
    for(const auto& entity:*iterator) {
        if(!entity.is_object())continue;
        const auto id=entity.find("id");
        if(id!=entity.end()&&id->is_string()&&id->get<std::string>()==entity_id)return true;
    }
    return false;
}

bool EditorUi::apply_entity_selection(const std::vector<std::string>& entity_ids,
                                      const std::string_view primary_entity_id) {
    if(entity_ids.empty()||primary_entity_id.empty())return false;
    if(simulation_state_!=EditorSimulationState::edit) {
        if(entity_ids.size()!=1U)return false;
        if(play_world_selected_entity_id_==entity_ids.front())return false;
        play_world_selected_entity_id_=entity_ids.front();
        return true;
    }
    const auto before=model_.selected_object_ids();
    const auto primary=std::ranges::find(entity_ids,primary_entity_id);
    if(primary==entity_ids.end())return false;
    if(!model_.select_object(primary_entity_id,false))return false;
    for(const auto& id:entity_ids) {
        if(id==primary_entity_id)continue;
        if(!model_.select_object(id,true))return false;
    }
    return before!=model_.selected_object_ids();
}

EditorUiContextSnapshot EditorUi::editor_context_snapshot() const {
    EditorUiContextSnapshot result;
    result.revision=editor_context_revision_;
    result.world_revision=model_.world_revision();
    result.authority=simulation_state_==EditorSimulationState::edit?"edit-world":"play-world-read-only";
    result.simulation_state=simulation_state_==EditorSimulationState::edit?"edit":
        simulation_state_==EditorSimulationState::playing?"playing":"paused";
    result.writable=simulation_state_==EditorSimulationState::edit&&!script_compile_busy_;
    result.project_id=project_context_.project_id;
    result.project_name=project_context_.name;
    result.project_root=project_context_.root;
    result.scene_source=model_.scene_source();
    result.scene_dirty=model_.scene_dirty();
    if(simulation_state_==EditorSimulationState::edit) {
        result.selected_entity_ids=model_.selected_object_ids();
        if(!result.selected_entity_ids.empty())result.primary_selected_entity_id=result.selected_entity_ids.front();
    } else if(!play_world_selected_entity_id_.empty()) {
        result.selected_entity_ids.push_back(play_world_selected_entity_id_);
        result.primary_selected_entity_id=play_world_selected_entity_id_;
    }
    if(const auto* asset=model_.selected_asset();asset!=nullptr)result.selected_asset_id=asset->id;
    result.focused_panel_id=model_.focused_panel();
    result.active_tab_id=active_tab_id_;
    result.last_action_status=last_action_status_;
    result.selected_play_world_change_ids=selected_play_world_change_ids();
    result.scene_can_undo=model_.can_undo();
    result.scene_can_redo=model_.can_redo();
    result.script_build_busy=script_compile_busy_;
    return result;
}

std::string EditorUi::editor_context_snapshot_json() const {
    const auto value=editor_context_snapshot();
    const auto asset_id=value.selected_asset_id.empty()?nlohmann::json(nullptr):nlohmann::json(value.selected_asset_id);
    const auto tab_id=value.active_tab_id? nlohmann::json(*value.active_tab_id):nlohmann::json(nullptr);
    return nlohmann::json{
        {"schemaVersion",value.schema_version},{"revision",value.revision},{"worldRevision",value.world_revision},
        {"authority",value.authority},{"simulationState",value.simulation_state},{"writable",value.writable},
        {"project",{{"id",value.project_id},{"name",value.project_name},{"root",value.project_root}}},
        {"scene",{{"source",value.scene_source},{"dirty",value.scene_dirty}}},
        {"selection",{{"entityIds",value.selected_entity_ids},{"primaryEntityId",value.primary_selected_entity_id.empty()?
            nlohmann::json(nullptr):nlohmann::json(value.primary_selected_entity_id)},{"assetId",asset_id}}},
        {"focus",{{"panelId",value.focused_panel_id},{"activeTabId",tab_id}}},
        {"transaction",{{"lastActionStatus",value.last_action_status},{"selectedPlayWorldChangeIds",value.selected_play_world_change_ids},
            {"canUndo",value.scene_can_undo},{"canRedo",value.scene_can_redo},{"scriptBuildBusy",value.script_build_busy}}}
    }.dump();
}

EditorUiContextApplyReceipt EditorUi::apply_editor_context_intent(const EditorUiContextIntent& intent) {
    synchronize_editor_context_revision();
    EditorUiContextApplyReceipt result{.revision_before=editor_context_revision_,.revision_after=editor_context_revision_};
    const auto fail=[&](std::string code,std::string detail) {
        result.success=false;result.code=std::move(code);result.detail=std::move(detail);result.revision_after=editor_context_revision_;return result;
    };
    if(intent.expected_revision!=editor_context_revision_)
        return fail("editor.context-conflict","The Editor context revision changed; observe it again before applying selection or focus.");
    auto requested_panel=intent.focused_panel_id;
    if(intent.active_tab_id&&!intent.active_tab_id->empty()) {
        if(requested_panel&&!requested_panel->empty()&&*requested_panel!=*intent.active_tab_id)
            return fail("editor.context-focus-conflict","focus.panelId and focus.activeTabId must identify the same visible panel.");
        requested_panel=intent.active_tab_id;
    }
    if(requested_panel) {
        if(requested_panel->empty()||std::ranges::find(model_.panels(),*requested_panel,&EditorPanel::id)==model_.panels().end())
            return fail("editor.context-panel-invalid","The requested Editor panel is not part of the current shell.");
    }
    std::vector<std::string> entity_ids;
    const bool selection_requested=intent.selected_entity_ids.has_value()||intent.primary_selected_entity_id.has_value();
    if(intent.selected_entity_ids)entity_ids=*intent.selected_entity_ids;
    if(intent.primary_selected_entity_id&&intent.primary_selected_entity_id->empty()&&!entity_ids.empty())
        return fail("editor.context-selection-invalid","A primary selection cannot be empty when entity IDs are supplied.");
    if(intent.primary_selected_entity_id&&!intent.primary_selected_entity_id->empty()&&entity_ids.empty())
        entity_ids.push_back(*intent.primary_selected_entity_id);
    if(entity_ids.size()>128U)return fail("editor.context-selection-too-large","The Editor selection is limited to 128 entities.");
    std::unordered_set<std::string> unique_entity_ids;
    for(const auto& id:entity_ids) {
        if(id.empty()||id.size()>128U||!unique_entity_ids.insert(id).second)
            return fail("editor.context-selection-invalid","Entity selection IDs must be unique, non-empty and bounded.");
        if(simulation_state_==EditorSimulationState::edit) {
            if(std::ranges::find(model_.objects(),id,&EditorObject::id)==model_.objects().end())
                return fail("editor.context-entity-not-found","One or more requested Edit World entities do not exist.");
        } else if(!entity_exists_in_play_world(id)) {
            return fail("editor.context-entity-not-found","One or more requested Play World entities do not exist.");
        }
    }
    std::string primary_entity_id;
    if(intent.primary_selected_entity_id)primary_entity_id=*intent.primary_selected_entity_id;
    else if(!entity_ids.empty())primary_entity_id=entity_ids.front();
    if(!primary_entity_id.empty()&&std::ranges::find(entity_ids,primary_entity_id)==entity_ids.end())
        return fail("editor.context-selection-invalid","The primary entity must be included in entityIds.");
    if(intent.selected_entity_ids&&intent.selected_entity_ids->empty()) {
        const auto current_selection=editor_context_snapshot().selected_entity_ids;
        if(!current_selection.empty())return fail("editor.context-selection-clear-unavailable",
            "The current EditorModel exposes revision-bound selection but no destructive clear operation.");
    }
    if(intent.selected_asset_id) {
        if(intent.selected_asset_id->empty()) {
            if(model_.selected_asset()!=nullptr)return fail("editor.context-asset-clear-unavailable",
                "The current EditorModel does not expose an asset-selection clear operation.");
        } else if(!std::ranges::any_of(model_.assets(),[&](const EditorAsset& asset){return asset.id==*intent.selected_asset_id;}))
            return fail("editor.context-asset-not-found","The requested asset is not in the current EditorModel registry.");
    }
    if(intent.dry_run) {
        result.success=true;result.code="editor.context-dry-run";
        result.detail="Editor selection/focus intent validated without changing visible state.";
        return result;
    }
    bool changed=false;
    if(selection_requested&&!entity_ids.empty())changed|=apply_entity_selection(entity_ids,primary_entity_id);
    if(intent.selected_asset_id&&!intent.selected_asset_id->empty()) {
        const auto* before=model_.selected_asset();
        const auto before_id=before==nullptr?std::string_view{}:std::string_view(before->id);
        static_cast<void>(model_.select_asset(*intent.selected_asset_id));
        const auto* after=model_.selected_asset();
        const auto after_id=after==nullptr?std::string_view{}:std::string_view(after->id);
        changed|=before_id!=after_id;
    }
    if(requested_panel&&model_.focused_panel()!=*requested_panel) {
        model_.set_focused_panel(*requested_panel);changed=true;
    }
    if(requested_panel)pending_panel_focus_id_=*requested_panel;
    if(changed)mark_editor_context_changed();
    refresh_visible_state();
    result.success=true;result.code=changed?"ok":"editor.context-no-change";
    result.detail=changed?"Editor selection/focus applied and visible state refreshed.":"Editor context already matched the requested intent.";
    result.revision_after=editor_context_revision_;
    return result;
}

std::string EditorUi::apply_editor_context_intent_json(const std::string_view request_json) {
    EditorUiContextApplyReceipt receipt;
    const auto document=nlohmann::json::parse(request_json,nullptr,false);
    if(!document.is_object()) {
        receipt={false,"editor.context-request-invalid","The context intent must be a JSON object.",editor_context_revision_,editor_context_revision_};
    } else if(!document.contains("expectedRevision")||!document.at("expectedRevision").is_number_unsigned()) {
        receipt={false,"editor.context-revision-required","A numeric expectedRevision is required for a context intent.",editor_context_revision_,editor_context_revision_};
    } else {
        EditorUiContextIntent intent;
        intent.expected_revision=document.at("expectedRevision").get<std::uint64_t>();
        if(document.contains("dryRun")&&!document.at("dryRun").is_boolean())
            receipt={false,"editor.context-dry-run-invalid","dryRun must be a boolean.",editor_context_revision_,editor_context_revision_};
        else intent.dry_run=document.value("dryRun",false);
        const auto selection=document.find("selection");
        if(selection!=document.end()&&!selection->is_object())
            receipt={false,"editor.context-selection-invalid","selection must be an object.",editor_context_revision_,editor_context_revision_};
        else if(selection!=document.end()) {
            const auto entity_ids=selection->find("entityIds");
            if(entity_ids!=selection->end()&&!entity_ids->is_array())
                receipt={false,"editor.context-selection-invalid","selection.entityIds must be an array.",editor_context_revision_,editor_context_revision_};
            else {
                bool valid=true;
                if(entity_ids!=selection->end()) {
                    std::vector<std::string> ids;
                    for(const auto& value:*entity_ids) {
                        if(!value.is_string()){valid=false;break;}
                        ids.push_back(value.get<std::string>());
                    }
                    if(valid)intent.selected_entity_ids=std::move(ids);
                }
                const auto primary=selection->find("primaryEntityId");
                if(primary!=selection->end()) {
                    if(!primary->is_string())valid=false;
                    else intent.primary_selected_entity_id=primary->get<std::string>();
                }
                const auto asset=selection->find("assetId");
                if(asset!=selection->end()) {
                    if(!asset->is_string())valid=false;
                    else intent.selected_asset_id=asset->get<std::string>();
                }
                if(!valid)receipt={false,"editor.context-selection-invalid","Selection fields have invalid JSON types.",editor_context_revision_,editor_context_revision_};
            }
        }
        if(receipt.code.empty()) {
            const auto focus=document.find("focus");
            if(focus!=document.end()&&!focus->is_object())receipt={false,"editor.context-focus-invalid","focus must be an object.",editor_context_revision_,editor_context_revision_};
            else if(focus!=document.end()) {
                const auto panel=focus->find("panelId");
                if(panel!=focus->end()) {
                    if(!panel->is_string())receipt={false,"editor.context-focus-invalid","focus.panelId must be a string.",editor_context_revision_,editor_context_revision_};
                    else intent.focused_panel_id=panel->get<std::string>();
                }
                const auto tab=focus->find("activeTabId");
                if(receipt.code.empty()&&tab!=focus->end()&&!tab->is_null()) {
                    if(!tab->is_string())receipt={false,"editor.context-focus-invalid","focus.activeTabId must be a string or null.",editor_context_revision_,editor_context_revision_};
                    else intent.active_tab_id=tab->get<std::string>();
                }
            }
        }
        if(receipt.code.empty())receipt=apply_editor_context_intent(intent);
    }
    const auto context=nlohmann::json::parse(editor_context_snapshot_json(),nullptr,false);
    return nlohmann::json{{"schemaVersion","noemancer.editor-context-receipt/0.1"},{"success",receipt.success},
        {"code",receipt.code},{"detail",receipt.detail},{"revisionBefore",receipt.revision_before},
        {"revisionAfter",receipt.revision_after},{"context",context}}.dump();
}

std::string EditorUi::semantic_snapshot_json() const {
    auto snapshot=nlohmann::json::parse(model_.semantic_snapshot_json());
    snapshot["startupHub"]=nlohmann::json::parse(startup_hub_.semantic_snapshot_json(),nullptr,false);
    snapshot["startupHub"]["visible"]=startup_hub_open_;
    snapshot["simulation"]={{"state",simulation_state_==EditorSimulationState::edit?"edit":
        simulation_state_==EditorSimulationState::playing?"playing":"paused"},
        {"editWorldWritable",simulation_state_==EditorSimulationState::edit&&!script_compile_busy_},
        {"availableCommands",simulation_state_==EditorSimulationState::edit?(script_compile_busy_?nlohmann::json::array():nlohmann::json{"play"}):
            simulation_state_==EditorSimulationState::playing?nlohmann::json{"pause","stop","apply-back-and-stop"}:
                nlohmann::json{"resume","step","stop","apply-back-and-stop"}},
        {"applyBack",{{"transaction","diff-preview-then-atomic-commit"},{"conflictPolicy","base-revision-and-scene-fingerprint"},
            {"undoable",true},{"transientRuntimeStateExcluded",true}}}};
    snapshot["simulation"]["runtimeSelection"]=play_world_selected_entity_id_.empty()?nlohmann::json(nullptr):nlohmann::json(play_world_selected_entity_id_);
    snapshot["simulation"]["runtimeWorld"]=play_world_observation_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(play_world_observation_json_,nullptr,false);
    snapshot["simulation"]["runtimeInspector"]=play_world_inspector_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(play_world_inspector_json_,nullptr,false);
    snapshot["simulation"]["applyBack"]["plan"]=play_world_apply_plan_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(play_world_apply_plan_json_,nullptr,false);
    snapshot["simulation"]["applyBack"]["selectedChangeIds"]=selected_play_world_change_ids();
    snapshot["project"]={{"id",project_context_.project_id},{"name",project_context_.name},{"root",project_context_.root},
        {"startupScene",project_context_.startup_scene},{"scriptProject",project_context_.script_project},
        {"assetRoots",project_context_.asset_roots}};
    snapshot["project"]["hybridPixelProfile"]=nullptr;
    if(project_context_.hybrid_pixel_profile) {
        const auto encoded=HybridPixelProfileCodec::write_canonical_json(*project_context_.hybrid_pixel_profile);
        snapshot["project"]["hybridPixelProfile"]=nlohmann::json::parse(encoded);
    }
    const ProjectSettingsInputMapViewModel input_map{project_context_.project_id,project_context_.name,
        project_context_.input_revision,project_context_.input_actions};
    snapshot["projectSettingsInputMap"]=nlohmann::json::parse(input_map.authoring_json(),nullptr,false);
    if(project_input_panel_) {
        const auto panel=project_input_panel_->state();
        const auto capture_state=panel.capture.state==ProjectSettingsInputMapCaptureState::armed?"armed":
            panel.capture.state==ProjectSettingsInputMapCaptureState::captured?"captured":
            panel.capture.state==ProjectSettingsInputMapCaptureState::cancelled?"cancelled":"idle";
        snapshot["projectSettingsInputMap"]["panel"]={{"open",project_settings_open_},
            {"selectedActionId",panel.selected_action_id},{"selectedBindingId",panel.selected_binding_id},
            {"capture",{{"state",capture_state},{"requestId",panel.capture.request_id},
                {"source",panel.capture.source},{"device",panel.capture.device}}},
            {"pendingRequest",panel.has_pending_request},{"lastError",panel.last_error}};
    }
    if(hybrid_pixel_profile_panel_) {
        auto profile_panel=nlohmann::json::parse(
            hybrid_pixel_profile_panel_->semantic_state_json(),nullptr,false);
        if(profile_panel.is_object()) {
            profile_panel["open"]=project_settings_open_;
            snapshot["projectSettingsHybridPixelProfile"]=std::move(profile_panel);
        }
    }
    if(project_ui_panel_) {
        auto project_ui=nlohmann::json::parse(project_ui_panel_->semantic_snapshot_json(),nullptr,false);
        if(project_ui.is_object()) {
            project_ui["open"]=project_settings_open_;
            snapshot["projectUiAuthoring"]=std::move(project_ui);
        }
    }
    const auto input_status=nlohmann::json::parse(input_status_json_,nullptr,false);
    snapshot["inputSources"]=input_status.is_discarded()?nlohmann::json(nullptr):input_status;
    const bool edit_authority=simulation_state_==EditorSimulationState::edit;
    const auto& runtime_world=snapshot["simulation"]["runtimeWorld"];
    const auto& runtime_inspector=snapshot["simulation"]["runtimeInspector"];
    const auto chrome_entity_count=edit_authority?model_.objects().size():
        (runtime_world.is_object()?runtime_world.value("entities",nlohmann::json::array()).size():0U);
    const auto chrome_selected_entities=edit_authority?nlohmann::json(model_.selected_object_ids()):
        (play_world_selected_entity_id_.empty()?nlohmann::json::array():nlohmann::json::array({play_world_selected_entity_id_}));
    const auto chrome_selected_entity=edit_authority?
        (model_.objects().empty()?nlohmann::json(nullptr):nlohmann::json(model_.selected_object().id)):
        (play_world_selected_entity_id_.empty()?nlohmann::json(nullptr):nlohmann::json(play_world_selected_entity_id_));
    const auto chrome_component_count=edit_authority?model_.inspector_sections().size():
        (runtime_inspector.is_object()?runtime_inspector.value("sections",nlohmann::json::array()).size():0U);
    const auto chrome_asset_job=nlohmann::json::parse(model_.active_asset_job_json(),nullptr,false);
    const auto* chrome_selected_asset=model_.selected_asset();
    snapshot["editorChrome"]={{"schemaVersion","noemancer.editor-chrome/0.1"},
        {"visualSystem","modern-precision-tool/0.1"},{"shell","imgui-dockspace"},
        {"retainedMigration","panel-by-panel-semantic-ui"},
        {"regions",nlohmann::json::array({
            {{"id","editor.command-bar"},{"role","toolbar"},{"currentProject",project_context_.name},
                {"currentScene",model_.scene_source()},{"sceneDirty",model_.scene_dirty()},
                {"simulationState",simulation_state_==EditorSimulationState::edit?"edit":simulation_state_==EditorSimulationState::playing?"playing":"paused"},
                {"scriptBuildBusy",script_compile_busy_}},
            {{"id","editor.scene-view"},{"role","main-workspace"}},
            {{"id","editor.world-outliner"},{"role","hierarchy"},
                {"authority",simulation_state_==EditorSimulationState::edit?"edit-world":"play-world-read-only"},
                {"query",std::string(outliner_filter_.data())},{"entityCount",chrome_entity_count},
                {"selectedEntityIds",chrome_selected_entities}},
            {{"id","editor.inspector"},{"role","inspector"},
                {"authority",simulation_state_==EditorSimulationState::edit&&!script_compile_busy_?"editable":"read-only"},
                {"query",std::string(inspector_filter_.data())},
                {"selectedEntityId",chrome_selected_entity},{"componentCount",chrome_component_count}},
            {{"id","editor.asset-browser"},{"role","asset-workspace"},
                {"query",std::string(asset_browser_filter_.data())},{"assetCount",model_.assets().size()},
                {"selectedAssetId",chrome_selected_asset==nullptr?nlohmann::json(nullptr):nlohmann::json(chrome_selected_asset->id)},
                {"activeJob",chrome_asset_job.is_discarded()?nlohmann::json(nullptr):chrome_asset_job}},
            {{"id","editor.animation-graph"},{"role","node-editor"},
                {"authority",simulation_state_==EditorSimulationState::edit&&!script_compile_busy_?"asset-source-editable":"asset-source-read-only"},
                {"visibleAssetId",animation_graph_asset_id_.empty()?nlohmann::json(nullptr):nlohmann::json(animation_graph_asset_id_)},
                {"fingerprint",animation_graph_fingerprint_.empty()?nlohmann::json(nullptr):nlohmann::json(animation_graph_fingerprint_)},
                {"focusTarget",animation_graph_canvas_.selection().empty()?nlohmann::json(nullptr):
                    nlohmann::json(animation_graph_canvas_.selection().front())},
                {"diagnostic",animation_graph_inline_diagnostic_.empty()?nlohmann::json(nullptr):nlohmann::json(animation_graph_inline_diagnostic_)},
                {"canvas",animation_graph_document_?nlohmann::json::parse(animation_graph_canvas_.projection_json(),nullptr,false):nlohmann::json(nullptr)}},
            {{"id","editor.bottom-tools"},{"role","tab-list"},{"tabs",nlohmann::json::array({"Asset Browser","Console","Agent Context"})}},
            {{"id","editor.status-bar"},{"role","status"},{"message",last_action_status_}}
        })}};
    if(animation_graph_document_&&snapshot.contains("animationGraphAuthoring")&&snapshot.at("animationGraphAuthoring").is_object()) {
        snapshot["animationGraphAuthoring"]["canvas"]=nlohmann::json::parse(animation_graph_canvas_.projection_json(),nullptr,false);
        snapshot["animationGraphAuthoring"]["writable"]=simulation_state_==EditorSimulationState::edit&&!script_compile_busy_;
        snapshot["animationGraphAuthoring"]["diagnostic"]=animation_graph_inline_diagnostic_.empty()?
            nlohmann::json(nullptr):nlohmann::json(animation_graph_inline_diagnostic_);
    }
    const auto retained_inspector=edit_authority?model_.inspector_semantic_ui_document_json():
        semantic_ui_document_from_inspector(play_world_inspector_json_);
    const auto retained_outliner=retained_outliner_document_json();
    const auto retained_asset_browser=retained_asset_browser_document_json();
    snapshot["editorChrome"]["retainedPanels"]={{"schemaVersion","noemancer.editor-retained-panels/0.1"},
        {"outliner",nlohmann::json::parse(retained_outliner,nullptr,false)},
        {"outlinerSurface",{{"surfaceId","editor.world-outliner"},
            {"visible",retained_outliner_texture_id_!=0&&retained_outliner_texture_width_>0&&
                retained_outliner_texture_height_>0},
            {"renderer","RmlUi/SDL_GPU"},{"width",retained_outliner_texture_width_},
            {"height",retained_outliner_texture_height_},{"coordinateRoute","dock-window-to-surface"},
            {"input",nlohmann::json::array({"pointer","keyboard","text","ime"})},
            {"controls",nlohmann::json::array({"search","tree","multi-selection","context-menu","drag-drop"})}}},
        {"assetBrowser",nlohmann::json::parse(retained_asset_browser,nullptr,false)},
        {"assetBrowserSurface",{{"surfaceId","editor.asset-browser.collection"},
            {"visible",retained_asset_browser_texture_id_!=0&&retained_asset_browser_texture_width_>0&&
                retained_asset_browser_texture_height_>0},
            {"renderer","RmlUi/SDL_GPU"},{"width",retained_asset_browser_texture_width_},
            {"height",retained_asset_browser_texture_height_},{"coordinateRoute","dock-window-to-surface"},
            {"input",nlohmann::json::array({"pointer","keyboard"})},
            {"controls",nlohmann::json::array({"grid","asset-card","single-selection","thumbnail","scroll"})}}},
        {"inspector",nlohmann::json::parse(retained_inspector,nullptr,false)},
        {"inspectorSurface",{{"surfaceId","editor.inspector"},{"visible",retained_inspector_texture_id_!=0},
            {"renderer","RmlUi/SDL_GPU"},{"width",retained_inspector_texture_width_},{"height",retained_inspector_texture_height_},
            {"coordinateRoute","dock-window-to-surface"},{"input",nlohmann::json::array({"pointer","keyboard","text","ime"})},
            {"controls",nlohmann::json::array({"text","asset","number","range","checkbox","select"})}}},
        {"actionBridge",{{"schemaVersion","noemancer.retained-ui-actions/0.1"},
            {"dispatch","runtime-adapter-to-domain-plan-apply-receipt"},{"revisionBound",true},
            {"boundedQueueCapacity",128}}}};
    snapshot["scripting"]=nlohmann::json::parse(scripting_status_cache_);
    snapshot["scripting"]["lastCompile"]=last_script_compile_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(last_script_compile_json_,nullptr,false);
    snapshot["scripting"]["sourceLocation"]=script_source_location_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(script_source_location_json_,nullptr,false);
    snapshot["scripting"]["debugEvents"]=nlohmann::json::parse(managed_debug_events_json_,nullptr,false);
    snapshot["scripting"]["debugLastAction"]=managed_debug_last_action_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(managed_debug_last_action_json_,nullptr,false);
    snapshot["scripting"]["buildJob"]=script_compile_job_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(script_compile_job_json_,nullptr,false);
    snapshot["scripting"]["playReload"]=play_script_reload_json_.empty()?nlohmann::json(nullptr):
        nlohmann::json::parse(play_script_reload_json_,nullptr,false);
    snapshot["scripting"]["autoBuild"]={{"enabled",auto_compile_scripts_},{"debounceMilliseconds",750},
        {"candidateFingerprint",auto_compile_candidate_fingerprint_.empty()?nlohmann::json(nullptr):nlohmann::json(auto_compile_candidate_fingerprint_)}};
    snapshot["sceneLifecycle"]={{"newScene",{{"available",simulation_state_==EditorSimulationState::edit&&!script_compile_busy_},
        {"template","asset-free-root-camera-sun"},{"saveMode","save-as"}}},
        {"recovery",scene_recovery_candidates_json_.empty()?nlohmann::json(nullptr):
            nlohmann::json::parse(scene_recovery_candidates_json_,nullptr,false)}};
    snapshot["assetJob"]=nlohmann::json::parse(model_.active_asset_job_json(),nullptr,false);
    snapshot["lastActionStatus"]=last_action_status_;
    snapshot["editorContext"]=nlohmann::json::parse(editor_context_snapshot_json(),nullptr,false);
    nlohmann::json pending_cells=nlohmann::json::array();for(std::size_t index=0;index<std::min<std::size_t>(tile_stroke_edits_.size(),64);++index) {
        const auto& edit=tile_stroke_edits_[index];pending_cells.push_back({{"x",edit.x},{"y",edit.y},{"operation",edit.tile_id?"paint":"erase"},
            {"tileId",edit.tile_id?nlohmann::json(*edit.tile_id):nlohmann::json(nullptr)}});
    }
    nlohmann::json palette_variants=nlohmann::json::array();for(std::size_t mask=0;mask<palette_rule_enabled_.size();++mask)
        if(palette_rule_enabled_[mask])palette_variants.push_back({{"mask",mask},{"frame",palette_rule_frames_[mask].data()}});
    snapshot["paletteRuleEditor"]={{"command","asset.tile-palette.autotile"},{"tileId",palette_rule_tile_id_},
        {"baseFingerprint",palette_rule_fingerprint_},{"autotileGroup",palette_rule_group_.data()},{"neighborBits",{{"north",1},{"east",2},{"south",4},{"west",8}}},
        {"variants",std::move(palette_variants)},{"transaction","preview-then-atomic-commit"}};
    snapshot["viewport"]={{"transformTool",gizmo_mode_==GizmoMode::select?"select":gizmo_mode_==GizmoMode::translate?"translate":
        gizmo_mode_==GizmoMode::rotate?"rotate":gizmo_mode_==GizmoMode::scale?"scale":"tilemap"},
        {"transactionActive",gizmo_was_using_},{"previewEntityId",gizmo_preview_entity_},
        {"implementation","ImGuizmo 1.10 + Noemancer transaction adapter"},
        {"camera",editor_camera_?nlohmann::json{{"id",editor_camera_->entity_id},{"position",editor_camera_->position},
            {"target",editor_camera_->target},{"projection",editor_camera_->projection},{"verticalFovDegrees",editor_camera_->vertical_fov_degrees},
            {"source","editor-ephemeral"},{"mutatesScene",false}}:nlohmann::json(nullptr)},
        {"navigation",{{"orbit","RMB drag"},{"pan","MMB drag"},{"dolly","mouse wheel"},
            {"fly","RMB + WASD/QE"},{"speedBoost","Shift"},{"frameSelected","F"}}},
        {"tileBrush",{{"active",gizmo_mode_==GizmoMode::tilemap},{"strokeActive",tile_stroke_active_},
            {"layerId",tile_brush_layer_id_},{"tileId",tile_brush_tile_id_},{"mode",tile_brush_erase_?"erase":"paint"},
            {"shape",tile_brush_shape_==TileBrushShape::brush?"brush":tile_brush_shape_==TileBrushShape::rectangle?"rectangle":"flood"},
            {"hoverCell",tile_brush_hover_cell_?nlohmann::json::array({tile_brush_hover_cell_->at(0),tile_brush_hover_cell_->at(1)}):nlohmann::json(nullptr)},
            {"regionAnchor",tile_region_anchor_?nlohmann::json::array({tile_region_anchor_->at(0),tile_region_anchor_->at(1)}):nlohmann::json(nullptr)},
            {"regionEnd",tile_region_end_?nlohmann::json::array({tile_region_end_->at(0),tile_region_end_->at(1)}):nlohmann::json(nullptr)},
            {"pendingCellCount",tile_stroke_edits_.size()},{"pendingCells",std::move(pending_cells)},
            {"pendingCellsTruncated",tile_stroke_edits_.size()>64},{"gridRadiusCells",8},{"preview","projected-grid-and-cell-ghost"}}}};
    return snapshot.dump();
}

void EditorUi::draw_root_dockspace() {
    static char scene_path_buffer[512]{};
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("Noemancer Editor", nullptr, flags);
    ImGui::PopStyleVar(3);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if(ImGui::MenuItem("Project Hub..."))startup_hub_open_=true;
            ImGui::Separator();
            ImGui::BeginDisabled(model_.scene_dirty()||simulation_state_!=EditorSimulationState::edit||script_compile_busy_);
            if(ImGui::MenuItem("New Project...")) {project_dialog_mode_=1;project_preset_index_=0;project_path_[0]='\0';project_name_[0]='\0';ImGui::OpenPopup("Project Workspace");}
            if(ImGui::MenuItem("Open Project...")) {project_dialog_mode_=2;project_path_[0]='\0';project_name_[0]='\0';ImGui::OpenPopup("Project Workspace");}
            ImGui::EndDisabled();
            ImGui::Separator();
            if(ImGui::MenuItem("New Scene...",nullptr,false,simulation_state_==EditorSimulationState::edit&&!script_compile_busy_)) {
                std::snprintf(new_scene_name_.data(),new_scene_name_.size(),"%s","Untitled Scene");ImGui::OpenPopup("New Scene");
            }
            if (ImGui::MenuItem("Open Scene...", "Ctrl+O",false,simulation_state_==EditorSimulationState::edit&&!script_compile_busy_)) {
                scene_path_buffer[0]='\0';
                ImGui::OpenPopup("Open Scene Source");
            }
            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, model_.can_save_scene()&&simulation_state_==EditorSimulationState::edit&&!script_compile_busy_)) {
                const auto action=model_.save_scene();last_action_status_=action.detail;
                if(action.success)scene_recovery_candidates_json_=model_.scene_recovery_candidates_json(project_context_.root);
            }
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S",false,simulation_state_==EditorSimulationState::edit&&!script_compile_busy_)) {
                std::snprintf(scene_path_buffer,sizeof(scene_path_buffer),"%s",model_.scene_source().c_str());
                ImGui::OpenPopup("Save Scene As Source");
            }
            if(ImGui::MenuItem("Recovery Candidates...",nullptr,false,project_context_.root!="engine://"&&
                simulation_state_==EditorSimulationState::edit&&!script_compile_busy_)) {
                scene_recovery_candidates_json_=model_.scene_recovery_candidates_json(project_context_.root);
                ImGui::OpenPopup("Scene Recovery Candidates");
            }
            ImGui::Separator();
            if(ImGui::MenuItem("Package Game...",nullptr,false,project_context_.root!="engine://"&&!package_busy_))
                package_panel_open_=true;
            ImGui::Separator();
            if(ImGui::MenuItem("Exit"))request_close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, model_.can_undo()&&simulation_state_==EditorSimulationState::edit&&!script_compile_busy_)) {
                last_action_status_ = model_.undo().detail;
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, model_.can_redo()&&simulation_state_==EditorSimulationState::edit&&!script_compile_busy_)) {
                last_action_status_ = model_.redo().detail;
            }
            ImGui::Separator();
            if(ImGui::MenuItem("Project Settings...",nullptr,project_settings_open_,
                project_context_.root!="engine://"))project_settings_open_=!project_settings_open_;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Reset Layout");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.048F,0.058F,0.078F,1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12.0F,7.0F});
    if(ImGui::BeginChild("##editor-command-bar",{0,45.0F},ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar)) {
        const auto marker_position=ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(marker_position,{marker_position.x+18.0F,marker_position.y+18.0F},accent,5.0F);
        ImGui::GetWindowDrawList()->AddCircleFilled({marker_position.x+9.0F,marker_position.y+9.0F},3.0F,IM_COL32(225,245,255,255));
        ImGui::Dummy({24.0F,18.0F});ImGui::SameLine();
        ImGui::TextUnformatted(project_context_.name.empty()?"Noemancer":project_context_.name.c_str());
        ImGui::SameLine();ImGui::TextDisabled("/");ImGui::SameLine();
        std::string scene_label=model_.scene_source().empty()?"Untitled Scene":model_.scene_source();
        if(const auto separator=scene_label.find_last_of("/\\");separator!=std::string::npos)scene_label=scene_label.substr(separator+1);
        ImGui::TextDisabled("%s%s",scene_label.c_str(),model_.scene_dirty()?"  * UNSAVED":"");

        ImGui::SameLine(0,20.0F);
        if(simulation_state_==EditorSimulationState::edit) {
            ImGui::BeginDisabled(script_compile_busy_);
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.11F,0.34F,0.25F,1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.14F,0.44F,0.32F,1.0F));
            if(draw_icon_button("##play",EditorIcon::play,"PLAY",false,"Enter Play World"))simulation_command_=EditorSimulationCommand::play;
            ImGui::PopStyleColor(2);ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.42F,0.16F,0.17F,1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.56F,0.19F,0.20F,1.0F));
            if(draw_icon_button("##stop",EditorIcon::stop,"STOP",false,"Stop Play World"))simulation_command_=EditorSimulationCommand::stop;
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if(simulation_state_==EditorSimulationState::playing) {
                if(draw_icon_button("##pause",EditorIcon::pause,"PAUSE",false,"Pause Play World"))simulation_command_=EditorSimulationCommand::pause;
            } else if(draw_icon_button("##resume",EditorIcon::resume,"RESUME",false,"Resume Play World"))simulation_command_=EditorSimulationCommand::resume;
        }
        ImGui::SameLine();
        draw_status_badge(simulation_state_==EditorSimulationState::edit?"EDIT WORLD":
            simulation_state_==EditorSimulationState::playing?"PLAY WORLD":"PAUSED",
            simulation_state_==EditorSimulationState::edit?color_accent:
            simulation_state_==EditorSimulationState::playing?color_success:color_warning);
        if(script_compile_busy_) {ImGui::SameLine();draw_status_badge("BUILDING C#",color_warning);}

        constexpr float right_reserve=205.0F;
        if(ImGui::GetCursorPosX()<ImGui::GetWindowWidth()-right_reserve)ImGui::SameLine(ImGui::GetWindowWidth()-right_reserve);
        ImGui::BeginDisabled(!model_.can_save_scene()||simulation_state_!=EditorSimulationState::edit||script_compile_busy_);
        if(draw_icon_button("##save",EditorIcon::save,"SAVE",false,"Save Scene (Ctrl+S)")) {const auto action=model_.save_scene();last_action_status_=action.detail;}
        ImGui::EndDisabled();ImGui::SameLine();
        ImGui::TextDisabled("%.0f FPS",ImGui::GetIO().Framerate);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();ImGui::PopStyleColor();

    if(ImGui::BeginPopupModal("Project Workspace",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto creating=project_dialog_mode_==1;
        ImGui::TextUnformatted(creating?"Create Noemancer Project":"Open Noemancer Project");
        ImGui::TextDisabled(creating?"Choose an absolute path that does not exist. A scene, assets folder and C# project are created atomically.":
            "Enter a project directory or noemancer.project.json path.");
        if(creating) {
            ImGui::SetNextItemWidth(620.0F);ImGui::InputText("Project name",project_name_.data(),project_name_.size());
            constexpr const char* presets[]={"Starter 3D","Hybrid Pixel / HD2D"};
            ImGui::SetNextItemWidth(620.0F);ImGui::Combo("Project preset",&project_preset_index_,presets,2);
            ImGui::TextDisabled(project_preset_index_==1?
                "Pixel-stable 320 x 180 canvas with nearest integer presentation.":
                "General-purpose scene, C# entry point, HUD, input, animation, and built-in assets.");
        }
        ImGui::SetNextItemWidth(620.0F);ImGui::InputText("Project path",project_path_.data(),project_path_.size());
        const auto ready=project_path_[0]!='\0'&&(!creating||project_name_[0]!='\0');
        ImGui::BeginDisabled(!ready);
        if(ImGui::Button(creating?"Create & Open":"Open")) {
            project_request_=EditorProjectRequest{creating?EditorProjectCommand::create:EditorProjectCommand::open,
                project_path_.data(),creating?project_name_.data():std::string{},
                creating&&project_preset_index_==1?"hybrid-pixel":"starter"};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(close_dialog_open_)ImGui::OpenPopup("Unsaved Scene");
    if(ImGui::BeginPopupModal("Unsaved Scene",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        close_dialog_open_=false;
        ImGui::TextUnformatted("The current scene contains unsaved changes.");
        if(ImGui::Button("Save & Exit")) {
            const auto action=model_.save_scene();last_action_status_=action.detail;
            if(action.success){exit_requested_=true;ImGui::CloseCurrentPopup();}
        }
        ImGui::SameLine();if(ImGui::Button("Discard & Exit")){exit_requested_=true;ImGui::CloseCurrentPopup();}
        ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(ImGui::BeginPopupModal("New Scene",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Create an asset-free scene with a Root, Main Camera and Sun.");
        ImGui::TextDisabled("The new scene stays unsaved until File > Save Scene As.");
        ImGui::SetNextItemWidth(420.0F);ImGui::InputText("Scene name",new_scene_name_.data(),new_scene_name_.size());
        ImGui::BeginDisabled(new_scene_name_[0]=='\0');
        if(ImGui::Button("Create")) {
            const auto action=model_.new_scene(new_scene_name_.data());last_action_status_=action.detail;
            if(action.success){reset_viewport_camera();ImGui::CloseCurrentPopup();}
        }
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();
        if(model_.scene_dirty())ImGui::TextDisabled("Save or undo current changes before replacing the active scene.");
        ImGui::EndPopup();
    }
    if(ImGui::BeginPopupModal("Scene Recovery Candidates",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        auto candidates=nlohmann::json::parse(scene_recovery_candidates_json_,nullptr,false);
        if(!candidates.is_object())candidates=nlohmann::json::object();
        ImGui::TextUnformatted("Validated recovery sidecars under the active project's scenes folder.");
        ImGui::TextDisabled("Recover loads a candidate as unsaved changes. Saving is a separate confirmation.");
        if(ImGui::SmallButton("Refresh")) {
            scene_recovery_candidates_json_=model_.scene_recovery_candidates_json(project_context_.root);
            candidates=nlohmann::json::parse(scene_recovery_candidates_json_,nullptr,false);
        }
        ImGui::SameLine();ImGui::TextDisabled("%zu candidate(s)%s",static_cast<std::size_t>(candidates.value("count",0U)),
            candidates.value("truncated",false)?" (bounded list)":"");
        ImGui::Separator();
        const auto items=candidates.value("items",nlohmann::json::array());
        if(items.empty())ImGui::TextDisabled("No divergent recovery candidate is available.");
        for(std::size_t index=0;index<items.size();++index) {
            const auto& item=items[index];ImGui::PushID(static_cast<int>(index));
            ImGui::Text("%s",item.value("name",std::string{"Unnamed Scene"}).c_str());
            ImGui::TextDisabled("%s  |  %llu bytes",item.value("targetPath",std::string{}).c_str(),
                static_cast<unsigned long long>(item.value("bytes",0ULL)));ImGui::SameLine();
            if(ImGui::SmallButton("Recover")) {
                const auto action=model_.recover_scene(project_context_.root,item.value("recoveryPath",std::string{}));
                last_action_status_=action.detail;if(action.success){reset_viewport_camera();ImGui::CloseCurrentPopup();}
            }
            ImGui::Separator();ImGui::PopID();
        }
        if(ImGui::Button("Close"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    const auto project_status=nlohmann::json::parse(project_status_json_,nullptr,false);
    if(project_status.is_object()&&!project_status.value("success",true))
        last_action_status_=project_status.value("detail",project_status.value("code",std::string{"Project operation failed."}));

    if (ImGui::BeginPopupModal("Open Scene Source",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Absolute scene JSON path");
        ImGui::SetNextItemWidth(620.0F);
        ImGui::InputText("##open-scene-path",scene_path_buffer,sizeof(scene_path_buffer));
        if (ImGui::Button("Open") && scene_path_buffer[0]!='\0') {
            const auto action=model_.open_scene(scene_path_buffer);
            last_action_status_=action.detail;
            if (action.success) { reset_viewport_camera(); ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save Scene As Source",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("New absolute scene JSON path");
        ImGui::SetNextItemWidth(620.0F);
        ImGui::InputText("##save-as-scene-path",scene_path_buffer,sizeof(scene_path_buffer));
        if (ImGui::Button("Save As") && scene_path_buffer[0]!='\0') {
            const auto action=model_.save_scene_as(scene_path_buffer,false);
            last_action_status_=action.detail;
            if (action.success) {scene_recovery_candidates_json_=model_.scene_recovery_candidates_json(project_context_.root);ImGui::CloseCurrentPopup();}
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::TextDisabled("Existing files are never overwritten without an explicit Agent request.");
        ImGui::EndPopup();
    }
    if(package_panel_open_) {
        ImGui::SetNextWindowSize({680.0F,330.0F},ImGuiCond_FirstUseEver);
        if(ImGui::Begin("Package Game",&package_panel_open_)) {
            ImGui::TextUnformatted("Standalone Player Distribution");
            ImGui::TextDisabled("The package is staged beside the destination and committed with one atomic directory rename.");
            ImGui::SetNextItemWidth(-1.0F);ImGui::InputText("Output directory",package_output_path_.data(),package_output_path_.size());
            constexpr const char* profiles[]={"Windows x64 Release","Windows x64 Debug"};
            ImGui::SetNextItemWidth(260.0F);ImGui::Combo("Game Profile",&package_profile_index_,profiles,2);
            const auto can_package=project_context_.root!="engine://"&&package_output_path_[0]!='\0'&&!package_busy_;
            ImGui::BeginDisabled(!can_package);
            if(ImGui::Button("Validate Plan"))package_request_=EditorPackageRequest{package_output_path_.data(),
                package_profile_index_==0?"windows-x64-release":"windows-x64-debug",true};
            ImGui::SameLine();if(ImGui::Button("Build Distribution"))package_request_=EditorPackageRequest{package_output_path_.data(),
                package_profile_index_==0?"windows-x64-release":"windows-x64-debug",false};
            ImGui::EndDisabled();
            if(package_busy_) {ImGui::SameLine();ImGui::TextColored({0.42F,0.65F,0.95F,1.0F},"PACKAGING...");}
            const auto status=nlohmann::json::parse(package_status_json_,nullptr,false);
            if(status.is_object()) {
                const auto success=status.value("success",false);const auto code=status.value("code",std::string{});
                ImGui::Separator();ImGui::TextColored(success?ImVec4{0.42F,0.78F,0.58F,1.0F}:ImVec4{0.95F,0.38F,0.32F,1.0F},
                    "%s  %s",success?"READY":"FAILED",code.c_str());
                ImGui::TextWrapped("%s",status.value("detail",std::string{}).c_str());
                const auto plan=status.value("plan",nlohmann::json(nullptr));
                if(plan.is_object())ImGui::TextDisabled("%zu files  |  %zu assets  |  %s",plan.value("entries",nlohmann::json::array()).size(),
                    plan.value("assetClosure",nlohmann::json::array()).size(),plan.value("planId",std::string{}).c_str());
            }
        }
        ImGui::End();
    }

    const ImGuiID dockspace_id = ImGui::GetID("Noemancer Editor Dockspace");
    constexpr auto dockspace_flags=static_cast<ImGuiDockNodeFlags>(
        static_cast<int>(ImGuiDockNodeFlags_PassthruCentralNode)|static_cast<int>(ImGuiDockNodeFlags_NoWindowMenuButton));
    ImGui::DockSpace(dockspace_id, ImVec2(0.0F, -27.0F), dockspace_flags);

    if (!layout_initialized_) {
        layout_initialized_ = true;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID center = dockspace_id;
        const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17F, nullptr, &center);
        const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25F, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22F, nullptr, &center);
        ImGui::DockBuilderDockWindow("World Outliner", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Asset Browser", bottom);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Agent Context", bottom);
        ImGui::DockBuilderDockWindow("Animation Graph", center);
        ImGui::DockBuilderDockWindow("Scene View", center);
        ImGui::DockBuilderFinish(dockspace_id);
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.035F,0.042F,0.057F,1.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{10.0F,4.0F});
    if(ImGui::BeginChild("##editor-status-bar",{0,0},ImGuiChildFlags_Borders,ImGuiWindowFlags_NoScrollbar)) {
        const auto status_color=last_action_status_.find("fail")!=std::string::npos||last_action_status_.find("error")!=std::string::npos?
            color_danger:ImVec4{0.55F,0.62F,0.72F,1.0F};
        ImGui::TextColored(status_color,"%s",last_action_status_.c_str());
        const auto right_text=script_compile_busy_?"C# build in progress":"Ready";
        const auto right_width=ImGui::CalcTextSize(right_text).x;
        if(ImGui::GetCursorPosX()<ImGui::GetWindowWidth()-right_width-12.0F) {
            ImGui::SameLine(ImGui::GetWindowWidth()-right_width-12.0F);ImGui::TextDisabled("%s",right_text);
        }
    }
    ImGui::EndChild();ImGui::PopStyleVar();ImGui::PopStyleColor();
    ImGui::End();
}

void EditorUi::draw_scene_view() {
    prepare_panel_window("editor.panel.scene");
    ImGui::Begin("Scene View", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))set_focused_panel("editor.panel.scene");

    ImGui::TextDisabled("TOOLS");ImGui::SameLine();
    if (draw_mode_button("##tool-select",EditorIcon::select,"SELECT",gizmo_mode_==GizmoMode::select)) gizmo_mode_=GizmoMode::select;
    ImGui::SameLine();
    if (draw_mode_button("##tool-move",EditorIcon::move,"MOVE",gizmo_mode_==GizmoMode::translate)) gizmo_mode_=GizmoMode::translate;
    ImGui::SameLine();
    if (draw_mode_button("##tool-rotate",EditorIcon::rotate,"ROTATE",gizmo_mode_==GizmoMode::rotate)) gizmo_mode_=GizmoMode::rotate;
    ImGui::SameLine();
    if (draw_mode_button("##tool-scale",EditorIcon::scale,"SCALE",gizmo_mode_==GizmoMode::scale)) gizmo_mode_=GizmoMode::scale;
    const auto tilemap_authoring=nlohmann::json::parse(model_.selected_tilemap_authoring_json(),nullptr,false);
    if(tilemap_authoring.is_object()&&tilemap_authoring.value("valid",false)) {
        ImGui::SameLine();if(draw_mode_button("##tool-tile",EditorIcon::tile,"TILE BRUSH",gizmo_mode_==GizmoMode::tilemap))gizmo_mode_=GizmoMode::tilemap;
    }
    ImGui::SameLine(0,16.0F);
    if(draw_icon_button("##frame-all",EditorIcon::frame,"FRAME ALL",false,"Frame all scene content")) reset_viewport_camera();
    if(simulation_state_!=EditorSimulationState::edit) {
        ImGui::SameLine();ImGui::BeginDisabled(selected_play_world_change_ids_.empty());
        if(ImGui::Button("APPLY SELECTED"))simulation_command_=EditorSimulationCommand::apply_back_and_stop;
        ImGui::EndDisabled();
        if(simulation_state_==EditorSimulationState::paused) {ImGui::SameLine();if(ImGui::Button("STEP"))simulation_command_=EditorSimulationCommand::step;}
    }
    ImGui::SameLine();ImGui::TextDisabled("LIT  |  GRID");
    ImGui::SetNextItemWidth(120.0F);
    ImGui::SameLine();ImGui::SliderFloat("##scene-exposure",&requested_exposure_,0.125F,8.0F,"EV %.2f",ImGuiSliderFlags_Logarithmic);
    ImGui::Separator();

    const ImVec2 canvas_position = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    canvas_size.x = std::max(canvas_size.x, 64.0F);
    canvas_size.y = std::max(canvas_size.y, 64.0F);
    requested_scene_width_ = static_cast<std::uint32_t>(canvas_size.x);
    requested_scene_height_ = static_cast<std::uint32_t>(canvas_size.y);

    if (scene_texture_id_ != 0) {
        ImGui::Image(
            static_cast<ImTextureID>(scene_texture_id_),
            canvas_size,
            ImVec2(0.0F, 0.0F),
            ImVec2(1.0F, 1.0F));
        const auto image_min = ImGui::GetItemRectMin();
        const auto image_size = ImGui::GetItemRectSize();
        scene_canvas_x_ = image_min.x;
        scene_canvas_y_ = image_min.y;
        scene_canvas_width_ = image_size.x;
        scene_canvas_height_ = image_size.y;
        const bool image_clicked=ImGui::IsItemClicked(ImGuiMouseButton_Left);
        update_viewport_camera_navigation(ImGui::IsItemHovered());
        handle_tilemap_brush(image_min.x,image_min.y,image_size.x,image_size.y,ImGui::IsItemHovered());
        const auto canvas_end = ImGui::GetItemRectMax();
        draw_transform_gizmo(image_min.x,image_min.y,image_size.x,image_size.y);
        if (image_clicked && gizmo_mode_!=GizmoMode::tilemap && !ImGuizmo::IsOver() && scene_texture_width_ > 0 && scene_texture_height_ > 0) {
            const auto mouse = ImGui::GetMousePos();
            const float u = std::clamp((mouse.x - image_min.x) / image_size.x, 0.0F, 0.999999F);
            const float v = std::clamp((mouse.y - image_min.y) / image_size.y, 0.0F, 0.999999F);
            scene_pick_request_ = ScenePickRequest{
                static_cast<std::uint32_t>(u * static_cast<float>(scene_texture_width_)),
                static_cast<std::uint32_t>(v * static_cast<float>(scene_texture_height_))};
        }
        auto* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddText(
            {canvas_position.x + 14.0F, canvas_position.y + 14.0F},
            IM_COL32(220, 232, 246, 220),
            "SCENE  |  SHADED");
        const ImVec2 axis_center{canvas_end.x - 42.0F, canvas_position.y + 42.0F};
        draw_list->AddLine(axis_center, {axis_center.x + 22.0F, axis_center.y}, IM_COL32(235, 82, 82, 255), 2.0F);
        draw_list->AddLine(axis_center, {axis_center.x, axis_center.y - 22.0F}, IM_COL32(91, 214, 122, 255), 2.0F);
        draw_list->AddLine(axis_center, {axis_center.x - 14.0F, axis_center.y + 14.0F}, IM_COL32(79, 145, 240, 255), 2.0F);
        ImGui::End();
        return;
    }

    ImGui::InvisibleButton("scene-canvas", canvas_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    update_viewport_camera_navigation(ImGui::IsItemHovered());
    scene_canvas_x_ = canvas_position.x;
    scene_canvas_y_ = canvas_position.y;
    scene_canvas_width_ = canvas_size.x;
    scene_canvas_height_ = canvas_size.y;
    auto* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 canvas_end{canvas_position.x + canvas_size.x, canvas_position.y + canvas_size.y};
    draw_list->AddRectFilled(canvas_position, canvas_end, panel_background);

    const float horizon = canvas_position.y + canvas_size.y * 0.60F;
    for (int index = -12; index <= 12; ++index) {
        const float t = static_cast<float>(index) / 12.0F;
        const float base_x = canvas_position.x + canvas_size.x * (0.5F + t * 0.65F);
        const float top_x = canvas_position.x + canvas_size.x * (0.5F + t * 0.08F);
        draw_list->AddLine(
            {base_x, canvas_end.y},
            {top_x, horizon},
            index % 4 == 0 ? grid_major : grid_minor,
            index % 4 == 0 ? 1.2F : 1.0F);
    }
    for (int index = 0; index <= 12; ++index) {
        const float normalized = static_cast<float>(index) / 12.0F;
        const float perspective = normalized * normalized;
        const float y = horizon + perspective * (canvas_end.y - horizon);
        draw_list->AddLine(
            {canvas_position.x, y},
            {canvas_end.x, y},
            index % 4 == 0 ? grid_major : grid_minor,
            index % 4 == 0 ? 1.2F : 1.0F);
    }

    draw_isometric_cube(
        draw_list,
        {canvas_position.x + canvas_size.x * 0.43F, horizon - 20.0F},
        std::clamp(canvas_size.y * 0.18F, 52.0F, 96.0F),
        !model_.objects().empty() && model_.selected_object().id == "entity.demo-cube");
    draw_sphere(
        draw_list,
        {canvas_position.x + canvas_size.x * 0.64F, horizon - 3.0F},
        std::clamp(canvas_size.y * 0.085F, 28.0F, 48.0F),
        false);

    draw_list->AddText(
        {canvas_position.x + 14.0F, canvas_position.y + 14.0F},
        IM_COL32(179, 194, 214, 255),
        "Bootstrap Scene Document  |  live World + procedural preview");
    draw_list->AddText(
        {canvas_position.x + 14.0F, canvas_end.y - 26.0F},
        IM_COL32(117, 136, 160, 255),
        "RMB orbit/fly  |  MMB pan  |  Wheel dolly  |  F frame selected");

    const ImVec2 axis_center{canvas_end.x - 42.0F, canvas_position.y + 42.0F};
    draw_list->AddLine(axis_center, {axis_center.x + 22.0F, axis_center.y}, IM_COL32(235, 82, 82, 255), 2.0F);
    draw_list->AddLine(axis_center, {axis_center.x, axis_center.y - 22.0F}, IM_COL32(91, 214, 122, 255), 2.0F);
    draw_list->AddLine(axis_center, {axis_center.x - 14.0F, axis_center.y + 14.0F}, IM_COL32(79, 145, 240, 255), 2.0F);
    draw_list->AddText({axis_center.x + 24.0F, axis_center.y - 7.0F}, IM_COL32(235, 82, 82, 255), "X");
    draw_list->AddText({axis_center.x - 4.0F, axis_center.y - 38.0F}, IM_COL32(91, 214, 122, 255), "Y");
    draw_list->AddText({axis_center.x - 28.0F, axis_center.y + 13.0F}, IM_COL32(79, 145, 240, 255), "Z");

    ImGui::End();
}

void EditorUi::handle_tilemap_brush(const float x,const float y,const float width,const float height,const bool hovered) {
    tile_brush_hover_cell_.reset();
    if(gizmo_mode_!=GizmoMode::tilemap||simulation_state_!=EditorSimulationState::edit||!editor_camera_) {
        tile_stroke_active_=false;tile_stroke_edits_.clear();tile_region_anchor_.reset();tile_region_end_.reset();return;
    }
    const auto authoring=nlohmann::json::parse(model_.selected_tilemap_authoring_json(),nullptr,false);
    if(!authoring.is_object()||!authoring.value("valid",false)||authoring.value("bindings",nlohmann::json::array()).empty()) {
        tile_stroke_active_=false;tile_stroke_edits_.clear();tile_region_anchor_.reset();tile_region_end_.reset();return;
    }
    const auto bindings=authoring.at("bindings");const auto& binding=bindings.at(0);const auto& camera=*editor_camera_;
    const auto position=binding.at("position"),scale=binding.at("scale"),rotation=binding.at("rotation");
    const auto rotate=[](const GizmoVec3 value,const float qx,const float qy,const float qz,const float qw) {
        const auto tx=2.0F*(qy*value.z-qz*value.y),ty=2.0F*(qz*value.x-qx*value.z),tz=2.0F*(qx*value.y-qy*value.x);
        return GizmoVec3{value.x+qw*tx+(qy*tz-qz*ty),value.y+qw*ty+(qz*tx-qx*tz),value.z+qw*tz+(qx*ty-qy*tx)};
    };
    const auto safe_scale=[](const float value) {return std::abs(value)>=0.0001F?value:(value<0.0F?-0.0001F:0.0001F);};
    const auto qx=rotation.at(0).get<float>(),qy=rotation.at(1).get<float>(),qz=rotation.at(2).get<float>(),qw=rotation.at(3).get<float>();
    const auto scale_x=safe_scale(scale.at(0).get<float>()),scale_y=safe_scale(scale.at(1).get<float>());
    const auto plane_origin=GizmoVec3{position.at(0).get<float>(),position.at(1).get<float>(),position.at(2).get<float>()};
    GizmoVec3 eye{camera.position[0],camera.position[1],camera.position[2]},target{camera.target[0],camera.target[1],camera.target[2]};
    const auto forward=normalize(target-eye),right=normalize(cross(forward,{0,1,0})),up=normalize(cross(right,forward));
    const auto aspect=width/std::max(height,1.0F);const auto cell_size=authoring.at("map").at("cellSize");
    const auto cell_width=cell_size.at(0).get<float>(),cell_height=cell_size.at(1).get<float>();
    if(hovered) {
        const auto mouse=ImGui::GetMousePos();const auto ndc_x=2.0F*(mouse.x-x)/std::max(width,1.0F)-1.0F;
        const auto ndc_y=1.0F-2.0F*(mouse.y-y)/std::max(height,1.0F);
        GizmoVec3 ray_origin=eye,ray_direction=forward;
        if(camera.projection=="orthographic") {
            const auto half_height=camera.orthographic_height*0.5F,half_width=half_height*aspect;
            ray_origin={eye.x+right.x*ndc_x*half_width+up.x*ndc_y*half_height,
                eye.y+right.y*ndc_x*half_width+up.y*ndc_y*half_height,
                eye.z+right.z*ndc_x*half_width+up.z*ndc_y*half_height};
        } else {
            const auto tangent=std::tan(camera.vertical_fov_degrees*0.0087266463F);
            ray_direction=normalize({forward.x+right.x*ndc_x*tangent*aspect+up.x*ndc_y*tangent,
                forward.y+right.y*ndc_x*tangent*aspect+up.y*ndc_y*tangent,
                forward.z+right.z*ndc_x*tangent*aspect+up.z*ndc_y*tangent});
        }
        const auto plane_normal=rotate({0,0,1},qx,qy,qz,qw);const auto denominator=dot(ray_direction,plane_normal);
        if(std::abs(denominator)>0.00001F) {
            const auto distance=dot(plane_origin-ray_origin,plane_normal)/denominator;
            if(distance>=0.0F) {
                const auto point=GizmoVec3{ray_origin.x+ray_direction.x*distance,ray_origin.y+ray_direction.y*distance,ray_origin.z+ray_direction.z*distance};
                const auto local_rotated=rotate(point-plane_origin,-qx,-qy,-qz,qw);
                tile_brush_hover_cell_=std::array<std::int32_t,2>{static_cast<std::int32_t>(std::floor(local_rotated.x/scale_x/cell_width)),
                    static_cast<std::int32_t>(std::floor(local_rotated.y/scale_y/cell_height))};
            }
        }
    }
    if(tile_brush_hover_cell_&&hovered&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        tile_stroke_active_=true;tile_stroke_edits_.clear();tile_stroke_base_fingerprint_=authoring.value("fingerprint",std::string{});
        tile_region_anchor_=tile_brush_hover_cell_;tile_region_end_=tile_brush_hover_cell_;
        if(tile_brush_shape_==TileBrushShape::flood) {
            const auto preview=nlohmann::json::parse(model_.apply_selected_tilemap_region("flood",tile_brush_layer_id_,*tile_region_anchor_,std::nullopt,
                tile_brush_erase_?std::nullopt:std::optional<std::string>{tile_brush_tile_id_},tile_brush_flip_x_,tile_brush_flip_y_,tile_stroke_base_fingerprint_,true),nullptr,false);
            const auto result=preview.value("result",nlohmann::json::object());if(result.value("success",false))
                for(const auto& edit:result.at("plan").value("edits",nlohmann::json::array()))tile_stroke_edits_.push_back({edit.at("cell").at(0).get<std::int32_t>(),
                    edit.at("cell").at(1).get<std::int32_t>(),edit.value("operation",std::string{})=="paint"?
                        std::optional<std::string>{edit.at("tileId").get<std::string>()}:std::nullopt,edit.value("flipX",false),edit.value("flipY",false)});
            else {tile_stroke_active_=false;last_action_status_="Flood preview rejected: "+result.value("code",std::string("unknown"));}
        }
    }
    if(tile_stroke_active_&&ImGui::IsMouseDown(ImGuiMouseButton_Left)&&tile_brush_hover_cell_) {
        tile_region_end_=tile_brush_hover_cell_;
        if(tile_brush_shape_==TileBrushShape::brush) {
            auto cell_x=tile_stroke_edits_.empty()?tile_brush_hover_cell_->at(0):tile_stroke_edits_.back().x;
            auto cell_y=tile_stroke_edits_.empty()?tile_brush_hover_cell_->at(1):tile_stroke_edits_.back().y;
            const auto target_x=tile_brush_hover_cell_->at(0),target_y=tile_brush_hover_cell_->at(1);
            const auto delta_x=std::abs(target_x-cell_x),step_x=cell_x<target_x?1:-1;
            const auto delta_y=-std::abs(target_y-cell_y),step_y=cell_y<target_y?1:-1;auto error=delta_x+delta_y;
            while(tile_stroke_edits_.size()<4096) {
                if(std::ranges::none_of(tile_stroke_edits_,[&](const TilemapCellEdit& edit){return edit.x==cell_x&&edit.y==cell_y;}))
                    tile_stroke_edits_.push_back({cell_x,cell_y,tile_brush_erase_?std::nullopt:std::optional<std::string>{tile_brush_tile_id_},
                        tile_brush_flip_x_,tile_brush_flip_y_});
                if(cell_x==target_x&&cell_y==target_y)break;const auto doubled_error=2*error;
                if(doubled_error>=delta_y){error+=delta_y;cell_x+=step_x;}if(doubled_error<=delta_x){error+=delta_x;cell_y+=step_y;}
            }
        } else if(tile_brush_shape_==TileBrushShape::rectangle&&tile_region_anchor_) {
            tile_stroke_edits_.clear();const auto minimum_x=std::min(tile_region_anchor_->at(0),tile_region_end_->at(0));
            const auto maximum_x=std::max(tile_region_anchor_->at(0),tile_region_end_->at(0));const auto minimum_y=std::min(tile_region_anchor_->at(1),tile_region_end_->at(1));
            const auto maximum_y=std::max(tile_region_anchor_->at(1),tile_region_end_->at(1));
            for(auto cell_y=minimum_y;cell_y<=maximum_y&&tile_stroke_edits_.size()<4096;++cell_y)for(auto cell_x=minimum_x;cell_x<=maximum_x&&tile_stroke_edits_.size()<4096;++cell_x)
                tile_stroke_edits_.push_back({cell_x,cell_y,tile_brush_erase_?std::nullopt:std::optional<std::string>{tile_brush_tile_id_},tile_brush_flip_x_,tile_brush_flip_y_});
        }
    }

    const auto overlay_center=tile_brush_hover_cell_?tile_brush_hover_cell_:
        (tile_stroke_edits_.empty()?std::optional<std::array<std::int32_t,2>>{}:
            std::optional<std::array<std::int32_t,2>>{{tile_stroke_edits_.back().x,tile_stroke_edits_.back().y}});
    if(overlay_center) {
        const auto world_point=[&](const float local_x,const float local_y) {
            const auto transformed=rotate({local_x*scale_x,local_y*scale_y,0},qx,qy,qz,qw);
            return GizmoVec3{plane_origin.x+transformed.x,plane_origin.y+transformed.y,plane_origin.z+transformed.z};
        };
        const auto project=[&](const GizmoVec3 point)->std::optional<ImVec2> {
            const auto relative=point-eye;const auto depth=dot(relative,forward);float ndc_x{},ndc_y{};
            if(camera.projection=="orthographic") {
                const auto half_height=std::max(camera.orthographic_height*0.5F,0.0001F);
                ndc_x=dot(relative,right)/(half_height*aspect);ndc_y=dot(relative,up)/half_height;
            } else {
                if(depth<=std::max(camera.near_clip,0.0001F))return std::nullopt;
                const auto tangent=std::tan(camera.vertical_fov_degrees*0.0087266463F);
                ndc_x=dot(relative,right)/(depth*tangent*aspect);ndc_y=dot(relative,up)/(depth*tangent);
            }
            if(!std::isfinite(ndc_x)||!std::isfinite(ndc_y))return std::nullopt;
            return ImVec2{x+(ndc_x+1.0F)*0.5F*width,y+(1.0F-ndc_y)*0.5F*height};
        };
        auto* draw_list=ImGui::GetWindowDrawList();draw_list->PushClipRect({x,y},{x+width,y+height},true);
        constexpr std::int32_t radius=8;const auto center_x=overlay_center->at(0),center_y=overlay_center->at(1);
        for(std::int32_t line=-radius;line<=radius+1;++line) {
            const auto vertical_x=static_cast<float>(center_x+line)*cell_width;
            const auto va=project(world_point(vertical_x,static_cast<float>(center_y-radius)*cell_height));
            const auto vb=project(world_point(vertical_x,static_cast<float>(center_y+radius+1)*cell_height));
            if(va&&vb)draw_list->AddLine(*va,*vb,line==0||line==1?IM_COL32(120,205,255,185):IM_COL32(100,165,205,90),line==0||line==1?1.5F:1.0F);
            const auto horizontal_y=static_cast<float>(center_y+line)*cell_height;
            const auto ha=project(world_point(static_cast<float>(center_x-radius)*cell_width,horizontal_y));
            const auto hb=project(world_point(static_cast<float>(center_x+radius+1)*cell_width,horizontal_y));
            if(ha&&hb)draw_list->AddLine(*ha,*hb,line==0||line==1?IM_COL32(120,205,255,185):IM_COL32(100,165,205,90),line==0||line==1?1.5F:1.0F);
        }
        const auto draw_cell=[&](const std::int32_t cell_x,const std::int32_t cell_y,const ImU32 fill,const ImU32 outline) {
            const std::array local{world_point(static_cast<float>(cell_x)*cell_width,static_cast<float>(cell_y)*cell_height),
                world_point(static_cast<float>(cell_x+1)*cell_width,static_cast<float>(cell_y)*cell_height),
                world_point(static_cast<float>(cell_x+1)*cell_width,static_cast<float>(cell_y+1)*cell_height),
                world_point(static_cast<float>(cell_x)*cell_width,static_cast<float>(cell_y+1)*cell_height)};
            std::array<ImVec2,4> screen{};for(std::size_t index=0;index<local.size();++index) {const auto point=project(local[index]);if(!point)return;screen[index]=*point;}
            draw_list->AddConvexPolyFilled(screen.data(),4,fill);draw_list->AddPolyline(screen.data(),4,outline,ImDrawFlags_Closed,2.0F);
        };
        for(const auto& edit:tile_stroke_edits_)draw_cell(edit.x,edit.y,edit.tile_id?IM_COL32(63,170,255,70):IM_COL32(255,88,96,70),
            edit.tile_id?IM_COL32(84,190,255,210):IM_COL32(255,105,110,220));
        if(tile_brush_hover_cell_)draw_cell(tile_brush_hover_cell_->at(0),tile_brush_hover_cell_->at(1),
            tile_brush_erase_?IM_COL32(255,70,80,85):IM_COL32(75,190,255,95),tile_brush_erase_?IM_COL32(255,120,125,255):IM_COL32(130,220,255,255));
        const auto label_point=project(world_point(static_cast<float>(center_x)*cell_width,static_cast<float>(center_y+1)*cell_height));
        if(label_point) {const auto label="cell ["+std::to_string(center_x)+", "+std::to_string(center_y)+"]  "+
            (tile_brush_erase_?"erase":tile_brush_tile_id_.empty()?"select a tile":"paint "+tile_brush_tile_id_);
            draw_list->AddText({label_point->x+6.0F,label_point->y-20.0F},IM_COL32(225,242,255,255),label.c_str());}
        draw_list->PopClipRect();
    }
    if(tile_stroke_active_&&ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        tile_stroke_active_=false;
        if(tile_brush_shape_==TileBrushShape::brush&&!tile_stroke_edits_.empty()&&!tile_brush_layer_id_.empty()&&(tile_brush_erase_||!tile_brush_tile_id_.empty())) {
            const auto preview=nlohmann::json::parse(model_.apply_selected_tilemap_stroke(tile_brush_layer_id_,tile_stroke_edits_,tile_stroke_base_fingerprint_,true),nullptr,false);
            if(preview.is_object()&&preview.value("ok",false)&&preview.at("result").value("success",false)) {
                const auto committed=nlohmann::json::parse(model_.apply_selected_tilemap_stroke(tile_brush_layer_id_,tile_stroke_edits_,tile_stroke_base_fingerprint_,false),nullptr,false);
                const auto result=committed.value("result",nlohmann::json::object());last_action_status_=result.value("success",false)?
                    "Tilemap stroke committed: "+std::to_string(result.at("receipt").value("changedCellCount",0U))+" cell(s).":
                    "Tilemap stroke rejected: "+result.value("code",std::string("unknown"));
            } else last_action_status_="Tilemap stroke preview was rejected.";
        } else if(tile_brush_shape_!=TileBrushShape::brush&&tile_region_anchor_&&tile_region_end_&&!tile_brush_layer_id_.empty()&&(tile_brush_erase_||!tile_brush_tile_id_.empty())) {
            const auto shape=tile_brush_shape_==TileBrushShape::rectangle?"rectangle":"flood";
            const auto second=tile_brush_shape_==TileBrushShape::rectangle?tile_region_end_:std::optional<std::array<std::int32_t,2>>{};
            const auto tile_id=tile_brush_erase_?std::nullopt:std::optional<std::string>{tile_brush_tile_id_};
            const auto preview=nlohmann::json::parse(model_.apply_selected_tilemap_region(shape,tile_brush_layer_id_,*tile_region_anchor_,second,tile_id,
                tile_brush_flip_x_,tile_brush_flip_y_,tile_stroke_base_fingerprint_,true),nullptr,false);const auto preview_result=preview.value("result",nlohmann::json::object());
            if(preview.value("ok",false)&&preview_result.value("success",false)) {const auto committed=nlohmann::json::parse(model_.apply_selected_tilemap_region(shape,
                tile_brush_layer_id_,*tile_region_anchor_,second,tile_id,tile_brush_flip_x_,tile_brush_flip_y_,tile_stroke_base_fingerprint_,false),nullptr,false);
                const auto result=committed.value("result",nlohmann::json::object());last_action_status_=result.value("success",false)?
                    std::string(shape)+" committed: "+std::to_string(result.at("receipt").value("changedCellCount",0U))+" cell(s).":
                    std::string(shape)+" rejected: "+result.value("code",std::string("unknown"));}
            else last_action_status_=std::string(shape)+" preview rejected: "+preview_result.value("code",std::string("unknown"));
        }
        tile_stroke_edits_.clear();tile_region_anchor_.reset();tile_region_end_.reset();
    }
}

void EditorUi::draw_transform_gizmo(const float x,const float y,const float width,const float height) {
    if(simulation_state_!=EditorSimulationState::edit||gizmo_mode_==GizmoMode::select||gizmo_mode_==GizmoMode::tilemap||model_.objects().empty()) return;
    const auto& object=model_.selected_object(); if(!object.transform) return;
    if(!editor_camera_) return;const auto& camera=*editor_camera_;
    const auto& transform=*object.transform;
    if(gizmo_preview_entity_!=object.id||!gizmo_was_using_) {
        gizmo_preview_entity_=object.id;
        const auto euler=euler_degrees_from_quaternion({transform.rotation_x,transform.rotation_y,transform.rotation_z,transform.rotation_w});
        float translation[3]{transform.x,transform.y,transform.z},rotation[3]{static_cast<float>(euler.x),static_cast<float>(euler.y),static_cast<float>(euler.z)};
        float scale[3]{transform.scale_x,transform.scale_y,transform.scale_z};
        ImGuizmo::RecomposeMatrixFromComponents(translation,rotation,scale,gizmo_preview_matrix_.data());
    }
    const auto view=view_matrix({camera.position[0],camera.position[1],camera.position[2]},
        {camera.target[0],camera.target[1],camera.target[2]});
    const auto aspect=width/std::max(height,1.0F);
    const auto projection=camera.projection=="orthographic"?
        orthographic_matrix(camera.orthographic_height*0.5F,aspect,camera.near_clip,camera.far_clip):
        perspective_matrix(camera.vertical_fov_degrees*0.0174532925F,aspect,camera.near_clip,camera.far_clip);
    ImGuizmo::SetDrawlist();ImGuizmo::SetRect(x,y,width,height);ImGuizmo::SetOrthographic(camera.projection=="orthographic");
    const auto operation=gizmo_mode_==GizmoMode::translate?ImGuizmo::TRANSLATE:gizmo_mode_==GizmoMode::rotate?ImGuizmo::ROTATE:ImGuizmo::SCALE;
    ImGuizmo::Manipulate(view.data(),projection.data(),operation,
        ImGuizmo::WORLD,gizmo_preview_matrix_.data());
    const bool using_now=ImGuizmo::IsUsing();
    if(gizmo_was_using_&&!using_now) {
        float translation[3]{},rotation_degrees[3]{},scale[3]{};ImGuizmo::DecomposeMatrixToComponents(gizmo_preview_matrix_.data(),translation,rotation_degrees,scale);
        const auto rotation=quaternion_from_euler_degrees({rotation_degrees[0],rotation_degrees[1],rotation_degrees[2]});
        const auto result=model_.set_selected_transform({translation[0],translation[1],translation[2],scale[0],scale[1],scale[2],
            rotation.x,rotation.y,rotation.z,rotation.w});
        last_action_status_=result.detail;
    }
    gizmo_was_using_=using_now;
}

void EditorUi::draw_world_outliner() {
    prepare_panel_window("editor.panel.outliner");
    ImGui::Begin("World Outliner");
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))set_focused_panel("editor.panel.outliner");
    const bool edit_world=simulation_state_==EditorSimulationState::edit;
    if(retained_outliner_texture_id_!=0&&retained_outliner_texture_width_>0&&
       retained_outliner_texture_height_>0) {
        const auto available=ImGui::GetContentRegionAvail();
        const auto draw_width=std::max(1.0F,available.x);
        const auto draw_height=std::max(1.0F,available.y);
        const auto origin=ImGui::GetCursorScreenPos();
        retained_outliner_canvas_x_=origin.x;retained_outliner_canvas_y_=origin.y;
        retained_outliner_canvas_width_=draw_width;retained_outliner_canvas_height_=draw_height;
        const auto scale=ImGui::GetIO().DisplayFramebufferScale;
        requested_outliner_width_=static_cast<std::uint32_t>(std::clamp(std::lround(draw_width*scale.x),192L,2048L));
        requested_outliner_height_=static_cast<std::uint32_t>(std::clamp(std::lround(draw_height*scale.y),160L,4096L));
        ImGui::Image(static_cast<ImTextureID>(retained_outliner_texture_id_),{draw_width,draw_height});
        ImGui::End();
        return;
    }
    retained_outliner_canvas_width_=0.0F;
    retained_outliner_canvas_height_=0.0F;
    const bool authoring=edit_world&&!script_compile_busy_;
    const auto runtime_observation=edit_world?nlohmann::json::object():nlohmann::json::parse(play_world_observation_json_,nullptr,false);
    const auto object_count=edit_world?model_.objects().size():
        (runtime_observation.is_object()?runtime_observation.value("entities",nlohmann::json::array()).size():0U);
    const auto count_label=std::to_string(object_count)+(object_count==1?" entity":" entities");
    draw_panel_heading(edit_world?"Edit World":"Play World",count_label.c_str());
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-ImGui::GetFrameHeight()*2.25F);
    ImGui::BeginDisabled(!authoring);
    if(draw_icon_button("##create-entity",EditorIcon::add,"",false,"Create empty entity"))last_action_status_=model_.create_empty_entity().detail;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if(draw_icon_button("##entity-actions",EditorIcon::more,"",false,"Entity actions"))ImGui::OpenPopup("Entity actions");
    static char rename_buffer[128]{};
    bool request_rename{};
    bool request_delete{};
    if(ImGui::BeginPopup("Entity actions")) {
        const bool has_selection=authoring&&!model_.objects().empty();
        ImGui::BeginDisabled(!has_selection);
        if(ImGui::MenuItem("Duplicate","Ctrl+D"))last_action_status_=model_.duplicate_selected().detail;
        if(ImGui::MenuItem("Copy","Ctrl+C"))last_action_status_=model_.copy_selected().detail;
        if(ImGui::MenuItem("Rename","F2")) {
            std::snprintf(rename_buffer,sizeof(rename_buffer),"%s",model_.selected_object().name.c_str());
            request_rename=true;
        }
        if(ImGui::MenuItem("Delete","Delete"))request_delete=true;
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!authoring||!model_.can_paste());
        if(ImGui::MenuItem("Paste","Ctrl+V"))last_action_status_=model_.paste_copied().detail;
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if(request_rename)ImGui::OpenPopup("Rename selected entity");
    if(request_delete)ImGui::OpenPopup("Delete selected entity?");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##outliner-filter","Search name, type, or ID",outliner_filter_.data(),outliner_filter_.size());
    if (ImGui::BeginPopupModal("Rename selected entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputTextWithHint("##entity-name", "Display name", rename_buffer, sizeof(rename_buffer));
        if (ImGui::Button("Rename") && rename_buffer[0] != '\0') {
            last_action_status_ = model_.rename_selected(rename_buffer).detail;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete selected entity?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %zu selected entity(s) and all of their children?",model_.selected_object_ids().size());
        ImGui::TextDisabled("References from other entities will be validated before commit.");
        if (ImGui::Button("Delete recursively")) {
            last_action_status_ = model_.delete_selected(true).detail;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();

    if(!edit_world) {
        const auto& observation=runtime_observation;
        const auto entities=observation.is_object()?observation.value("entities",nlohmann::json::array()):nlohmann::json::array();
        draw_status_badge("RUNTIME / READ ONLY",color_warning);
        std::unordered_set<std::string> visible_ids;
        const std::string_view query{outliner_filter_.data()};
        for(const auto& entity:entities) {
            const auto id=entity.value("id",std::string{});
            const auto name=entity.value("displayName",entity.value("name",id));
            if(!contains_case_insensitive(name,query)&&!contains_case_insensitive(id,query)&&
                !contains_case_insensitive(entity.value("type",std::string{"entity"}),query))continue;
            auto current=id;
            while(!current.empty()&&visible_ids.insert(current).second) {
                const auto match=std::ranges::find_if(entities,[&](const nlohmann::json& candidate){return candidate.value("id",std::string{})==current;});
                if(match==entities.end())break;
                const auto parent=match->value("parentId",nlohmann::json(nullptr));current=parent.is_string()?parent.get<std::string>():std::string{};
            }
        }
        if(ImGui::TreeNodeEx("Play World",ImGuiTreeNodeFlags_DefaultOpen|ImGuiTreeNodeFlags_SpanAvailWidth)) {
            std::function<void(const std::string&)> draw_runtime_children=[&](const std::string& parent_id) {
                for(const auto& entity:entities) {
                    const auto parent=entity.value("parentId",nlohmann::json(nullptr));
                    const auto current_parent=parent.is_string()?parent.get<std::string>():std::string{};
                    if(current_parent!=parent_id)continue;
                    const auto id=entity.value("id",std::string{});const auto name=entity.value("displayName",entity.value("name",id));
                    if(!query.empty()&&!visible_ids.contains(id))continue;
                    const bool children=std::ranges::any_of(entities,[&](const nlohmann::json& candidate){
                        const auto value=candidate.value("parentId",nlohmann::json(nullptr));return value.is_string()&&value.get<std::string>()==id;});
                    ImGui::PushID(id.c_str());const auto flags=ImGuiTreeNodeFlags_SpanAvailWidth|
                        (play_world_selected_entity_id_==id?ImGuiTreeNodeFlags_Selected:0)|
                        (children?ImGuiTreeNodeFlags_DefaultOpen:ImGuiTreeNodeFlags_Leaf|ImGuiTreeNodeFlags_NoTreePushOnOpen);
                    const auto open=ImGui::TreeNodeEx(name.c_str(),flags);
                    if(ImGui::IsItemClicked()&&play_world_selected_entity_id_!=id) {
                        play_world_selected_entity_id_=id;
                        mark_editor_context_changed();
                    }
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s\n%s\nruntime revision %llu",id.c_str(),
                        entity.value("type",std::string{"entity"}).c_str(),static_cast<unsigned long long>(entity.value("revision",0ULL)));
                    if(children&&open){draw_runtime_children(id);ImGui::TreePop();}ImGui::PopID();
                }
            };
            draw_runtime_children("");ImGui::TreePop();
        }
        ImGui::End();return;
    }

    std::optional<std::pair<std::string,std::string>> pending_reparent;
    const auto scene_path=std::filesystem::path(model_.scene_source());
    const auto scene_name=scene_path.empty()?std::string{"Untitled Scene"}:scene_path.stem().string();
    const auto& objects=model_.objects();
    std::unordered_set<std::string> visible_ids;
    const std::string_view query{outliner_filter_.data()};
    for(const auto& object:objects) {
        if(!contains_case_insensitive(object.name,query)&&!contains_case_insensitive(object.kind,query)&&!contains_case_insensitive(object.id,query))continue;
        auto current=object.id;
        while(!current.empty()&&visible_ids.insert(current).second) {
            const auto match=std::ranges::find(objects,current,&EditorObject::id);
            current=match==objects.end()?std::string{}:match->parent_id;
        }
    }
    if (ImGui::TreeNodeEx((scene_name+"##scene-root").c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (authoring && ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("NOEMANCER_ENTITY_ID"))
                pending_reparent = {std::string(static_cast<const char*>(payload->Data)),std::string{}};
            ImGui::EndDragDropTarget();
        }
        std::function<void(const std::string&)> draw_children = [&](const std::string& parent_id) {
            for (std::size_t index = 0; index < objects.size(); ++index) {
                const auto& object = objects[index];
                if (object.parent_id != parent_id) continue;
                if(!query.empty()&&!visible_ids.contains(object.id))continue;
                const bool has_children = std::ranges::any_of(objects, [&](const EditorObject& candidate) {
                    return candidate.parent_id == object.id;
                });
                ImGui::PushID(object.id.c_str());
                const auto flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                    (model_.is_object_selected(object.id) ? ImGuiTreeNodeFlags_Selected : 0) |
                    (has_children ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                const bool open = ImGui::TreeNodeEx(object.name.c_str(), flags);
                if (ImGui::IsItemClicked()) {
                    const auto before=model_.selected_object_ids();
                    static_cast<void>(model_.select_object(index,ImGui::GetIO().KeyCtrl));
                    if(before!=model_.selected_object_ids())mark_editor_context_changed();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%s", object.id.c_str(), object.kind.c_str());
                }
                if (authoring && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("NOEMANCER_ENTITY_ID", object.id.c_str(), object.id.size()+1);
                    ImGui::Text("Move %s", object.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (authoring && ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("NOEMANCER_ENTITY_ID"))
                        pending_reparent = {std::string(static_cast<const char*>(payload->Data)),object.id};
                    ImGui::EndDragDropTarget();
                }
                if (has_children && open) {
                    draw_children(object.id);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        };
        draw_children("");
        if(objects.empty())ImGui::TextDisabled("No entities. Use + to create the first one.");
        else if(!query.empty()&&visible_ids.empty())ImGui::TextDisabled("No entities match this search.");
        ImGui::TreePop();
    }
    if (authoring && pending_reparent) last_action_status_ = model_.reparent_entity(pending_reparent->first,pending_reparent->second).detail;
    ImGui::End();
}

void EditorUi::draw_inspector() {
    prepare_panel_window("editor.panel.inspector");
    ImGui::Begin("Inspector");
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))set_focused_panel("editor.panel.inspector");
    if(simulation_state_!=EditorSimulationState::edit) {
        const auto inspector=nlohmann::json::parse(play_world_inspector_json_,nullptr,false);
        draw_panel_heading("Runtime Inspector","live values");
        draw_status_badge("RUNTIME / READ ONLY",color_warning);
        ImGui::Separator();
        if(!inspector.is_object()||!inspector.value("valid",false))draw_empty_panel_state("Nothing selected","Select an entity in Play World to inspect it.");
        else {
            const auto entity=inspector.value("entity",nlohmann::json::object());
            ImGui::TextUnformatted(entity.value("name",std::string{"Runtime Entity"}).c_str());
            ImGui::TextDisabled("%s",entity.value("id",std::string{}).c_str());
            for(const auto& section:inspector.value("sections",nlohmann::json::array())) {
                const auto id=section.value("id",std::string{});const auto label=section.value("label",std::string{"Component"});
                if(!ImGui::CollapsingHeader((label+"##runtime-"+id).c_str(),ImGuiTreeNodeFlags_DefaultOpen))continue;
                for(const auto& property:section.value("properties",nlohmann::json::array())) {
                    const auto value=property.value("value",nlohmann::json(nullptr));const auto display=value.is_string()?value.get<std::string>():value.dump();
                    ImGui::LabelText(property.value("label",std::string{"Value"}).c_str(),"%s",display.c_str());
                }
            }
        }
        ImGui::SeparatorText("Apply Back Preview");
        const auto plan=nlohmann::json::parse(play_world_apply_plan_json_,nullptr,false);
        const auto changes=plan.is_object()?plan.value("changes",nlohmann::json::array()):nlohmann::json::array();
        ImGui::TextDisabled("%zu authorable change(s) · %zu selected · transient state excluded",changes.size(),selected_play_world_change_ids_.size());
        if(ImGui::SmallButton("Select All"))for(const auto& change:changes)if(const auto id=change.value("changeId",std::string{});!id.empty())selected_play_world_change_ids_.insert(id);
        ImGui::SameLine();if(ImGui::SmallButton("Select None"))selected_play_world_change_ids_.clear();
        for(const auto& change:changes) {
            const auto id=change.value("changeId",std::string{});if(id.empty())continue;
            bool selected=selected_play_world_change_ids_.contains(id);
            const auto label=change.value("entityId",std::string{})+" · "+change.value("field",std::string{});
            ImGui::PushID(id.c_str());if(ImGui::Checkbox(label.c_str(),&selected)) {
                if(selected)selected_play_world_change_ids_.insert(id);else selected_play_world_change_ids_.erase(id);
            }
            if(ImGui::IsItemHovered()) {
                auto before=change.value("before",nlohmann::json(nullptr)).dump();auto after=change.value("after",nlohmann::json(nullptr)).dump();
                if(before.size()>256U)before.resize(256U);if(after.size()>256U)after.resize(256U);
                ImGui::SetTooltip("Before: %s\nAfter: %s",before.c_str(),after.c_str());
            }
            ImGui::PopID();
        }
        ImGui::BeginDisabled(selected_play_world_change_ids_.empty());
        if(ImGui::Button("Apply Selected & Stop"))simulation_command_=EditorSimulationCommand::apply_back_and_stop;
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Stop and Discard"))simulation_command_=EditorSimulationCommand::stop;
        ImGui::End();return;
    }
    if (model_.objects().empty()) {
        draw_panel_heading("Inspector","authoring properties");
        draw_empty_panel_state("Nothing selected","Select an entity in the World Outliner.");
        ImGui::End();
        return;
    }
    const auto& object = model_.selected_object();
    const bool authoring=simulation_state_==EditorSimulationState::edit&&!script_compile_busy_;
    const auto component_count=std::to_string(model_.inspector_sections().size())+" components";
    ImGui::TextUnformatted(object.name.c_str());
    ImGui::SameLine();
    draw_status_badge(authoring?"EDITABLE":"BUILDING",authoring?color_success:color_warning);
    ImGui::TextDisabled("%s  |  %s  |  %s",object.id.c_str(),object.kind.c_str(),component_count.c_str());
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##inspector-filter","Filter components or properties",inspector_filter_.data(),inspector_filter_.size());
    ImGui::BeginDisabled(!authoring);
    if(ImGui::Button("+ Add Component",{-1.0F,0.0F}))ImGui::OpenPopup("Add Component Menu");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("Add Component Menu")) {
        static char component_filter[64]{};
        ImGui::SetNextItemWidth(240.0F);
        ImGui::InputTextWithHint("##component-filter", "Search components...", component_filter, sizeof(component_filter));
        ImGui::Separator();
        struct ComponentOffer { std::string_view name; std::string_view hint; };
        static constexpr std::array<ComponentOffer,11> components{{
            {"Transform","Foundation for spatial, render, camera and physics components."},
            {"Velocity","Recommended with a dynamic RigidBody."},
            {"RigidBody","Add one collider shape for physical collision."},
            {"BoxCollider","Only one collider shape is allowed per entity."},
            {"SphereCollider","Only one collider shape is allowed per entity."},
            {"CapsuleCollider","Only one collider shape is allowed per entity."},
            {"Camera","Transform is recommended for scene placement."},
            {"DirectionalLight","Provides direct and ambient scene lighting."},
            {"MeshRenderer","Transform is recommended; PbrMaterial controls appearance."},
            {"PbrMaterial","Used by MeshRenderer; texture fields accept stable asset IDs."},
            {"ManagedScript","Attach a project C# ScriptBehaviour by stable type and instance ID."}
        }};
        const auto contains_filter = [&](const std::string_view value) {
            std::string lhs(value), rhs(component_filter);
            std::ranges::transform(lhs,lhs.begin(),[](const unsigned char c){ return static_cast<char>(std::tolower(c)); });
            std::ranges::transform(rhs,rhs.begin(),[](const unsigned char c){ return static_cast<char>(std::tolower(c)); });
            return lhs.find(rhs) != std::string::npos;
        };
        for (const auto& component : components) {
            if (!contains_filter(component.name)) continue;
            const bool present = std::ranges::any_of(model_.inspector_sections(),[&](const InspectorSection& section){
                return section.component == component.name;
            });
            if (ImGui::MenuItem(component.name.data(),present?"Added":nullptr,false,!present)) {
                last_action_status_ = model_.add_component(component.name).detail;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",component.hint.data());
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    if(retained_inspector_texture_id_!=0) {
        const auto available=ImGui::GetContentRegionAvail();
        const auto draw_width=std::max(1.0F,available.x);
        const auto draw_height=std::max(1.0F,available.y);
        const auto origin=ImGui::GetCursorScreenPos();
        retained_inspector_canvas_x_=origin.x;retained_inspector_canvas_y_=origin.y;
        retained_inspector_canvas_width_=draw_width;retained_inspector_canvas_height_=draw_height;
        const auto scale=ImGui::GetIO().DisplayFramebufferScale;
        requested_inspector_width_=static_cast<std::uint32_t>(std::clamp(std::lround(draw_width*scale.x),192L,2048L));
        requested_inspector_height_=static_cast<std::uint32_t>(std::clamp(std::lround(draw_height*scale.y),160L,4096L));
        ImGui::Image(static_cast<ImTextureID>(retained_inspector_texture_id_),{draw_width,draw_height});
        ImGui::End();
        return;
    }
    retained_inspector_canvas_width_=0.0F;retained_inspector_canvas_height_=0.0F;
    std::optional<std::pair<std::string,std::string>> pending_change;
    static std::unordered_map<std::string,std::array<char,256>> text_buffers;
    std::size_t visible_section_count{};
    const std::string_view inspector_query{inspector_filter_.data()};
    for(const auto& section:model_.inspector_sections()) {
        const bool property_matches=std::ranges::any_of(section.properties,[&](const InspectorProperty& property){
            return contains_case_insensitive(property.label,inspector_query)||contains_case_insensitive(property.id,inspector_query);
        });
        if(!contains_case_insensitive(section.label,inspector_query)&&!contains_case_insensitive(section.component,inspector_query)&&!property_matches)continue;
        ++visible_section_count;
        const auto flags=section.default_expanded?ImGuiTreeNodeFlags_DefaultOpen:ImGuiTreeNodeFlags_None;
        const auto expanded=ImGui::CollapsingHeader((section.label+"##"+section.id).c_str(),flags);
        if(section.component!="SemanticIdentity"&&ImGui::BeginPopupContextItem()) {
            if(ImGui::MenuItem(("Remove "+section.component).c_str(),nullptr,false,authoring)) {
                last_action_status_=model_.remove_component(section.component).detail;
                ImGui::EndPopup();
                ImGui::End();
                return;
            }
            ImGui::EndPopup();
        }
        if(!expanded) continue;
        ImGui::PushID(section.id.c_str());
        for(const auto& property:section.properties) {
            ImGui::PushID(property.id.c_str());
            const auto value=nlohmann::json::parse(property.value_json,nullptr,false);
            bool changed=false;
            nlohmann::json next=value;
            if(property.control=="label"||!property.editable) {
                const auto display=value.is_string()?value.get<std::string>():value.dump();
                ImGui::LabelText(property.label.c_str(),"%s",display.c_str());
            } else if(property.control=="drag"&&(property.value_type=="vector3"||property.value_type=="color3")) {
                std::array<float,3> data{value.value("x",0.0F),value.value("y",0.0F),value.value("z",0.0F)};
                changed=ImGui::DragFloat3(property.label.c_str(),data.data(),static_cast<float>(property.step));
                if(changed) next={{"x",data[0]},{"y",data[1]},{"z",data[2]}};
            } else if(property.control=="color") {
                std::array<float,3> data{value.value("x",0.0F),value.value("y",0.0F),value.value("z",0.0F)};
                changed=ImGui::ColorEdit3(property.label.c_str(),data.data(),ImGuiColorEditFlags_Float);
                if(changed) next={{"x",data[0]},{"y",data[1]},{"z",data[2]}};
            } else if(property.control=="slider"||property.control=="drag") {
                float data=value.is_number()?value.get<float>():0.0F;
                changed=property.control=="slider"?ImGui::SliderFloat(property.label.c_str(),&data,static_cast<float>(property.minimum),static_cast<float>(property.maximum)):
                    ImGui::DragFloat(property.label.c_str(),&data,static_cast<float>(property.step),
                        property.has_minimum?static_cast<float>(property.minimum):-std::numeric_limits<float>::max(),
                        property.has_maximum?static_cast<float>(property.maximum):std::numeric_limits<float>::max());
                if(changed) next=data;
            } else if(property.control=="checkbox") {
                bool data=value.is_boolean()&&value.get<bool>(); changed=ImGui::Checkbox(property.label.c_str(),&data); if(changed) next=data;
            } else if(property.control=="combo") {
                const auto current=value.is_string()?value.get<std::string>():std::string{};
                if(ImGui::BeginCombo(property.label.c_str(),current.c_str())) {
                    for(const auto& option:property.options) if(ImGui::Selectable(option.c_str(),option==current)) { next=option; changed=true; }
                    ImGui::EndCombo();
                }
            } else if(property.control=="asset"||property.control=="text") {
                auto& buffer=text_buffers[property.id];
                if(!ImGui::IsAnyItemActive()) { const auto current=value.is_string()?value.get<std::string>():std::string{};
                    std::memset(buffer.data(),0,buffer.size()); std::memcpy(buffer.data(),current.data(),std::min(current.size(),buffer.size()-1)); }
                if(ImGui::InputText(property.label.c_str(),buffer.data(),buffer.size(),ImGuiInputTextFlags_EnterReturnsTrue)) { next=std::string(buffer.data()); changed=true; }
            } else if(property.control=="json") {
                static std::unordered_map<std::string,std::array<char,1024>> json_buffers;
                auto& buffer=json_buffers[property.id];
                if(!ImGui::IsAnyItemActive()) {const auto current=value.is_discarded()?std::string{"{}"}:value.dump();
                    std::memset(buffer.data(),0,buffer.size());std::memcpy(buffer.data(),current.data(),std::min(current.size(),buffer.size()-1));}
                if(ImGui::InputTextMultiline(property.label.c_str(),buffer.data(),buffer.size(),ImVec2(-1.0F,72.0F),
                    ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CtrlEnterForNewLine)) {
                    const auto parsed=nlohmann::json::parse(buffer.data(),nullptr,false);if(!parsed.is_discarded()){next=parsed;changed=true;}
                }
            }
            if(!property.unit.empty()&&ImGui::IsItemHovered()) ImGui::SetTooltip("Unit: %s",property.unit.c_str());
            if(changed&&!property.property.empty()) pending_change=std::pair{property.property,next.dump()};
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if(visible_section_count==0)draw_empty_panel_state("No matching components","Clear the Inspector search to show all properties.");
    if(authoring&&pending_change) {
        const auto receipt=model_.set_selected_property(pending_change->first,pending_change->second);
        last_action_status_=receipt.detail;
    }
    ImGui::End();
}

void EditorUi::draw_animation_graph() {
    const auto authoring=nlohmann::json::parse(model_.selected_animation_graph_authoring_json(),nullptr,false);
    const auto valid=authoring.is_object()&&authoring.value("valid",false);
    const auto asset_id=valid?authoring.at("asset").value("id",std::string{}):std::string{};
    const auto fingerprint=valid?authoring.value("fingerprint",std::string{}):std::string{};
    if(valid&&(asset_id!=animation_graph_asset_id_||fingerprint!=animation_graph_fingerprint_)) {
        if(!animation_graph_drag_node_id_.empty()) {
            animation_graph_drag_node_id_.clear();animation_graph_drag_asset_id_.clear();
            animation_graph_drag_fingerprint_.clear();
            animation_graph_inline_diagnostic_="Source changed while a node was being dragged; the stale preview was cancelled.";
        }
        const auto parsed=AnimationGraphCodec::parse_json(authoring.at("document").dump());
        if(parsed) {
            const bool same_asset=asset_id==animation_graph_asset_id_;
            const auto old_projection=animation_graph_canvas_.project();
            const auto old_selection=animation_graph_canvas_.selection();
            animation_graph_document_=std::move(*parsed.document);
            animation_graph_canvas_.bind(&*animation_graph_document_);
            if(same_asset) {
                static_cast<void>(animation_graph_canvas_.set_view(old_projection.pan_x,old_projection.pan_y,old_projection.zoom));
                for(const auto& node_id:old_selection)static_cast<void>(animation_graph_canvas_.select_node(node_id,true));
            }
            animation_graph_asset_id_=asset_id;animation_graph_fingerprint_=fingerprint;
            animation_graph_layer_drafts_.clear();animation_graph_mask_drafts_.clear();
            for(const auto& layer:animation_graph_document_->layers)animation_graph_layer_drafts_[layer.id]=layer.weight;
            for(const auto& mask:animation_graph_document_->masks)for(const auto& joint:mask.joints)
                animation_graph_mask_drafts_[mask.id+"\n"+joint.name]=joint.weight;
            if(!same_asset)animation_graph_focus_frames_=3;
        }
    } else if(!valid&&!animation_graph_asset_id_.empty()) {
        animation_graph_document_.reset();animation_graph_canvas_.bind(nullptr);
        animation_graph_asset_id_.clear();animation_graph_fingerprint_.clear();animation_graph_drag_node_id_.clear();
        animation_graph_drag_asset_id_.clear();animation_graph_drag_fingerprint_.clear();
    }

    prepare_panel_window("editor.panel.animation-graph");
    if(animation_graph_focus_frames_>0)ImGui::SetNextWindowFocus();
    ImGui::Begin("Animation Graph");
    if(animation_graph_focus_frames_>0) {
        ImGui::SetWindowFocus();
        set_focused_panel("editor.panel.animation-graph");
        --animation_graph_focus_frames_;
    }
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))set_focused_panel("editor.panel.animation-graph");
    if(!animation_graph_document_) {
        draw_panel_heading("Animation Graph","visual authoring");
        draw_empty_panel_state("Select an Animation Graph","Choose an AnimationGraph asset in the Asset Browser to edit nodes, layers, and masks.");
        ImGui::End();return;
    }
    auto projection=animation_graph_canvas_.project();
    const bool topology_writable=simulation_state_==EditorSimulationState::edit&&!script_compile_busy_;
    const auto apply_graph_operations=[&](std::vector<AnimationGraphPatchOperation> operations,
                                           const std::string_view expected=std::string_view{}) {
        const auto response=nlohmann::json::parse(model_.apply_selected_animation_graph_patch(
            operations,expected.empty()?std::string_view{animation_graph_fingerprint_}:expected,false),nullptr,false);
        const auto result=response.is_object()?response.value("result",nlohmann::json::object()):nlohmann::json::object();
        const auto success=result.value("success",false);
        const auto detail=result.value("detail",result.value("code",std::string{"Animation Graph edit failed."}));
        last_action_status_=detail;animation_graph_inline_diagnostic_=success?std::string{}:detail;
        return success;
    };
    const auto selected_nodes=[&]() {
        std::vector<const AnimationGraphNode*> result;
        for(const auto& selected_id:animation_graph_canvas_.selection()) {
            const auto found=std::ranges::find(animation_graph_document_->nodes,selected_id,&AnimationGraphNode::id);
            if(found!=animation_graph_document_->nodes.end())result.push_back(&*found);
        }
        return result;
    };
    draw_panel_heading(animation_graph_document_->asset_id.c_str(),"SOURCE AUTHORITY");
    ImGui::SameLine();draw_status_badge(("r "+animation_graph_fingerprint_.substr(0,std::min<std::size_t>(8,animation_graph_fingerprint_.size()))).c_str(),color_accent);
    if(ImGui::SmallButton("Frame Graph"))animation_graph_canvas_.reset_view_to_document();
    ImGui::SameLine();ImGui::BeginDisabled(!topology_writable);
    if(ImGui::SmallButton("+ Node"))ImGui::OpenPopup("Create Animation Graph Node");
    ImGui::SameLine();
    const auto selection=selected_nodes();
    ImGui::BeginDisabled(selection.size()!=1U);
    if(ImGui::SmallButton("Delete"))apply_graph_operations({AnimationGraphPatchOperation::delete_node(selection.front()->id)});
    ImGui::EndDisabled();ImGui::EndDisabled();
    ImGui::SameLine();ImGui::TextDisabled(topology_writable?
        "Middle-drag pans  |  Wheel zooms  |  topology and layout share one source transaction":"READ ONLY while Playing or building C#");
    if(ImGui::BeginPopupModal("Create Animation Graph Node",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        static constexpr const char* kinds="Clip\0State Machine\0Blend 1D\0";
        ImGui::Combo("Kind",&animation_graph_create_kind_,kinds);
        ImGui::InputText("Stable node ID",animation_graph_new_node_id_.data(),animation_graph_new_node_id_.size());
        if(animation_graph_create_kind_==0)ImGui::InputText("Clip asset ID",animation_graph_new_node_asset_.data(),animation_graph_new_node_asset_.size());
        else if(animation_graph_create_kind_==1)ImGui::InputText("State-machine asset ID",animation_graph_new_node_asset_.data(),animation_graph_new_node_asset_.size());
        else {
            ImGui::InputText("Float parameter",animation_graph_new_node_parameter_.data(),animation_graph_new_node_parameter_.size());
            ImGui::TextDisabled("Select exactly two Clip nodes before creating a Blend 1D node.");
        }
        const auto can_create=animation_graph_new_node_id_[0]!='\0'&&topology_writable;
        ImGui::BeginDisabled(!can_create);
        if(ImGui::Button("Create")) {
            std::vector<AnimationGraphPatchOperation> operations;
            const std::string node_id=animation_graph_new_node_id_.data();
            if(animation_graph_create_kind_==0)operations.push_back(AnimationGraphPatchOperation::create_clip_node(
                node_id,animation_graph_new_node_asset_.data()));
            else if(animation_graph_create_kind_==1)operations.push_back(AnimationGraphPatchOperation::create_state_machine_node(
                node_id,animation_graph_new_node_asset_.data()));
            else {
                const auto children=selected_nodes();
                if(children.size()!=2U||children[0]->kind!="clip"||children[1]->kind!="clip")
                    animation_graph_inline_diagnostic_="Blend 1D creation requires exactly two selected terminal Clip nodes.";
                else operations.push_back(AnimationGraphPatchOperation::create_blend_1d_node(node_id,
                    animation_graph_new_node_parameter_.data(),{{children[0]->id,0.0F},{children[1]->id,1.0F}}));
            }
            if(!operations.empty()) {
                float next_x=80.0F;for(const auto& layout:animation_graph_document_->editor.nodes)next_x=std::max(next_x,layout.x+220.0F);
                operations.push_back(AnimationGraphPatchOperation::set_node_position(node_id,next_x,120.0F));
                if(apply_graph_operations(std::move(operations))) {
                    std::fill(animation_graph_new_node_id_.begin(),animation_graph_new_node_id_.end(),'\0');
                    std::fill(animation_graph_new_node_asset_.begin(),animation_graph_new_node_asset_.end(),'\0');
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(!animation_graph_inline_diagnostic_.empty())ImGui::TextColored(color_danger,"%s",animation_graph_inline_diagnostic_.c_str());
    ImGui::Separator();
    if(!projection.valid) {
        ImGui::TextColored(color_danger,"%s",projection.code.c_str());ImGui::TextWrapped("%s",projection.detail.c_str());
        ImGui::End();return;
    }

    const auto available=ImGui::GetContentRegionAvail();
    const float details_width=std::clamp(available.x*0.28F,260.0F,380.0F);
    const ImVec2 canvas_size{std::max(180.0F,available.x-details_width-8.0F),std::max(180.0F,available.y)};
    ImGui::BeginChild("##animation-graph-canvas",canvas_size,ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const auto origin=ImGui::GetCursorScreenPos();const auto mouse=ImGui::GetIO().MousePos;
    const auto hovered=ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if(hovered&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle,0.0F))
        static_cast<void>(animation_graph_canvas_.pan_by(ImGui::GetIO().MouseDelta.x,ImGui::GetIO().MouseDelta.y));
    if(hovered&&ImGui::GetIO().MouseWheel!=0.0F) {
        const auto next_zoom=std::clamp(projection.zoom*std::pow(1.12F,ImGui::GetIO().MouseWheel),0.25F,2.5F);
        static_cast<void>(animation_graph_canvas_.set_view(projection.pan_x,projection.pan_y,next_zoom));
    }
    projection=animation_graph_canvas_.project();
    auto* draw=ImGui::GetWindowDrawList();const auto clip_min=origin;const ImVec2 clip_max{origin.x+canvas_size.x,origin.y+canvas_size.y};
    draw->PushClipRect(clip_min,clip_max,true);draw->AddRectFilled(clip_min,clip_max,panel_background);
    const float grid_step=64.0F*projection.zoom;
    if(grid_step>=12.0F) {
        const auto x_start=std::fmod(projection.pan_x,grid_step);const auto y_start=std::fmod(projection.pan_y,grid_step);
        for(float x=x_start;x<canvas_size.x;x+=grid_step)draw->AddLine({origin.x+x,origin.y},{origin.x+x,clip_max.y},grid_minor);
        for(float y=y_start;y<canvas_size.y;y+=grid_step)draw->AddLine({origin.x,origin.y+y},{clip_max.x,origin.y+y},grid_minor);
    }
    const auto position_of=[&](const AnimationGraphCanvasNodeBounds& node) {
        const auto x=node.node_id==animation_graph_drag_node_id_?animation_graph_drag_x_:node.x;
        const auto y=node.node_id==animation_graph_drag_node_id_?animation_graph_drag_y_:node.y;
        return ImVec2{origin.x+projection.pan_x+x*projection.zoom,origin.y+projection.pan_y+y*projection.zoom};
    };
    for(const auto& edge:projection.blend_edges) {
        const ImVec2 start{origin.x+projection.pan_x+edge.from_x*projection.zoom,
            origin.y+projection.pan_y+edge.from_y*projection.zoom};
        const ImVec2 end{origin.x+projection.pan_x+edge.to_x*projection.zoom,
            origin.y+projection.pan_y+edge.to_y*projection.zoom};
        const auto bend=std::max(42.0F,std::abs(end.x-start.x)*0.42F);
        draw->AddBezierCubic(start,{start.x+bend,start.y},{end.x-bend,end.y},end,IM_COL32(93,137,184,210),2.0F);
    }
    for(const auto& node:projection.nodes) {
        const auto top_left=position_of(node);const ImVec2 size{node.width*projection.zoom,node.height*projection.zoom};
        ImGui::SetCursorScreenPos(top_left);ImGui::PushID(node.node_id.c_str());
        ImGui::InvisibleButton("##node",size,ImGuiButtonFlags_MouseButtonLeft);
        const auto node_hovered=ImGui::IsItemHovered();
        if(ImGui::IsItemClicked())static_cast<void>(animation_graph_canvas_.select_node(node.node_id,ImGui::GetIO().KeyCtrl));
        if(topology_writable&&ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Left,2.0F)) {
            if(animation_graph_drag_node_id_.empty()) {
                animation_graph_drag_node_id_=node.node_id;
                animation_graph_drag_asset_id_=animation_graph_asset_id_;
                animation_graph_drag_fingerprint_=animation_graph_fingerprint_;
                animation_graph_drag_offset_x_=mouse.x-top_left.x;animation_graph_drag_offset_y_=mouse.y-top_left.y;
            }
            if(animation_graph_drag_node_id_==node.node_id) {
                animation_graph_drag_x_=(mouse.x-origin.x-projection.pan_x-animation_graph_drag_offset_x_)/projection.zoom;
                animation_graph_drag_y_=(mouse.y-origin.y-projection.pan_y-animation_graph_drag_offset_y_)/projection.zoom;
            }
        }
        const auto selected=std::ranges::find(animation_graph_canvas_.selection(),node.node_id)!=animation_graph_canvas_.selection().end();
        const auto fill=selected?IM_COL32(38,88,126,255):(node_hovered?IM_COL32(40,52,70,255):IM_COL32(29,38,52,255));
        draw->AddRectFilled(top_left,{top_left.x+size.x,top_left.y+size.y},fill,7.0F);
        draw->AddRect(top_left,{top_left.x+size.x,top_left.y+size.y},selected?accent:IM_COL32(82,98,120,255),7.0F,0,selected?2.2F:1.0F);
        draw->AddRectFilled(top_left,{top_left.x+size.x,top_left.y+24.0F*projection.zoom},
            node.kind=="blend-1d"?IM_COL32(102,70,150,255):node.kind=="state-machine"?IM_COL32(48,103,118,255):IM_COL32(48,76,111,255),7.0F);
        draw->AddText({top_left.x+10.0F,top_left.y+5.0F},IM_COL32_WHITE,node.kind.c_str());
        draw->AddText({top_left.x+10.0F,top_left.y+31.0F*projection.zoom},IM_COL32(218,226,239,255),node.node_id.c_str());
        ImGui::PopID();
    }
    for(const auto& port:projection.ports) {
        const ImVec2 center{origin.x+projection.pan_x+port.x*projection.zoom,
            origin.y+projection.pan_y+port.y*projection.zoom};
        const auto radius=std::clamp(port.radius*projection.zoom,4.0F,9.0F);
        draw->AddCircleFilled(center,radius,port.direction=="input"?IM_COL32(170,119,220,255):IM_COL32(84,177,205,255));
        draw->AddCircle(center,radius,IM_COL32(225,235,247,255),0,1.0F);
        ImGui::SetCursorScreenPos({center.x-radius-2.0F,center.y-radius-2.0F});
        ImGui::PushID((port.node_id+"/"+port.port_id).c_str());
        ImGui::InvisibleButton("##port",{radius*2.0F+4.0F,radius*2.0F+4.0F});
        if(ImGui::IsItemClicked())static_cast<void>(animation_graph_canvas_.select_node(port.node_id,ImGui::GetIO().KeyCtrl));
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s %s port\nCtrl-click to add its node to the topology selection",
            port.kind.c_str(),port.direction.c_str());
        ImGui::PopID();
    }
    if(!animation_graph_drag_node_id_.empty()&&ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if(topology_writable&&animation_graph_drag_asset_id_==animation_graph_asset_id_&&
           animation_graph_drag_fingerprint_==animation_graph_fingerprint_)
            apply_graph_operations({AnimationGraphPatchOperation::set_node_position(animation_graph_drag_node_id_,
                animation_graph_drag_x_,animation_graph_drag_y_)},animation_graph_drag_fingerprint_);
        else animation_graph_inline_diagnostic_="Node layout preview was cancelled because its source revision or edit authority changed.";
        animation_graph_drag_node_id_.clear();animation_graph_drag_asset_id_.clear();animation_graph_drag_fingerprint_.clear();
    }
    draw->PopClipRect();
    // Absolute node placement changes the cursor without contributing a layout item.
    // Submit the canvas extent before ending the child so Dear ImGui can validate and
    // retain the window's content bounds in Debug builds.
    ImGui::SetCursorScreenPos(origin);ImGui::Dummy(canvas_size);
    ImGui::EndChild();

    ImGui::SameLine();ImGui::BeginChild("##animation-graph-details",{0,canvas_size.y},ImGuiChildFlags_Borders);
    ImGui::TextUnformatted("TOPOLOGY");ImGui::Separator();
    const AnimationGraphNode* selected_blend=nullptr;const AnimationGraphNode* selected_clip=nullptr;
    if(selection.size()==2U)for(const auto* node:selection) {
        if(node->kind=="blend-1d")selected_blend=node;
        else if(node->kind=="clip")selected_clip=node;
    }
    if(selected_blend&&selected_clip) {
        ImGui::TextWrapped("%s  ->  %s",selected_clip->id.c_str(),selected_blend->id.c_str());
        ImGui::SetNextItemWidth(-1.0F);ImGui::InputFloat("Threshold",&animation_graph_connection_threshold_,0.05F,0.25F,"%.3f");
        const auto edge=std::ranges::find(selected_blend->children,selected_clip->id,&AnimationGraphBlendPoint::node_id);
        const bool connected=edge!=selected_blend->children.end();
        ImGui::BeginDisabled(!topology_writable||connected);
        if(ImGui::Button("Connect Clip",{-1.0F,0.0F}))apply_graph_operations({
            AnimationGraphPatchOperation::connect_blend_1d_child(selected_blend->id,selected_clip->id,
                animation_graph_connection_threshold_)});
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!topology_writable||!connected||selected_blend->children.size()<=2U);
        if(ImGui::Button("Disconnect Clip",{-1.0F,0.0F}))apply_graph_operations({
            AnimationGraphPatchOperation::disconnect_blend_1d_child(selected_blend->id,selected_clip->id)});
        ImGui::EndDisabled();
        if(connected&&selected_blend->children.size()<=2U)ImGui::TextDisabled("A Blend 1D node must retain at least two children.");
    } else ImGui::TextDisabled("Select one Blend 1D node and one Clip node to author an edge.");
    ImGui::Spacing();
    ImGui::TextUnformatted("LAYERS");ImGui::Separator();
    for(std::size_t layer_index=0;layer_index<animation_graph_document_->layers.size();++layer_index) {
        const auto& layer=animation_graph_document_->layers[layer_index];
        ImGui::PushID(layer.id.c_str());ImGui::Text("%s",layer.id.c_str());ImGui::SameLine();
        ImGui::TextDisabled("%s / %s",layer.mode.c_str(),layer.root_node.c_str());
        auto& draft=animation_graph_layer_drafts_[layer.id];ImGui::SetNextItemWidth(-1.0F);
        ImGui::BeginDisabled(!topology_writable||layer_index==0U);ImGui::SliderFloat("Weight",&draft,0.0F,1.0F,"%.2f");
        const auto weight_committed=topology_writable&&layer_index!=0U&&ImGui::IsItemDeactivatedAfterEdit();ImGui::EndDisabled();
        if(weight_committed) {
            const auto operation=AnimationGraphPatchOperation::set_layer_weight(layer.id,draft);
            const auto response=nlohmann::json::parse(model_.apply_selected_animation_graph_patch({operation},animation_graph_fingerprint_,false),nullptr,false);
            const auto result=response.is_object()?response.value("result",nlohmann::json::object()):nlohmann::json::object();
            last_action_status_=result.value("detail",result.value("code",std::string{"Animation Graph layer edit failed."}));
        }
        if(layer_index==0U)ImGui::TextDisabled("Base layer is the full-weight reference pose.");
        if(!layer.weight_parameter.empty())ImGui::TextDisabled("Parameter: %s",layer.weight_parameter.c_str());
        if(!layer.mask_id.empty())ImGui::TextDisabled("Mask: %s",layer.mask_id.c_str());
        if(!layer.sync_group.empty())ImGui::TextDisabled("Sync: %s",layer.sync_group.c_str());
        ImGui::Separator();ImGui::PopID();
    }
    ImGui::TextUnformatted("MASKS");ImGui::Separator();
    if(animation_graph_document_->masks.empty())ImGui::TextDisabled("No named masks");
    for(const auto& mask:animation_graph_document_->masks) {
        if(ImGui::TreeNode(mask.id.c_str(),"%s  (%s descendants)",mask.id.c_str(),mask.include_descendants?"include":"exclude")) {
            for(const auto& joint:mask.joints) {
                const auto key=mask.id+"\n"+joint.name;auto& draft=animation_graph_mask_drafts_[key];
                ImGui::PushID(key.c_str());ImGui::SetNextItemWidth(-1.0F);ImGui::BeginDisabled(!topology_writable);
                ImGui::SliderFloat(joint.name.c_str(),&draft,0.0F,1.0F,"%.2f");
                if(topology_writable&&ImGui::IsItemDeactivatedAfterEdit()) {
                    const auto operation=AnimationGraphPatchOperation::set_mask_joint_weight(mask.id,joint.name,draft);
                    const auto response=nlohmann::json::parse(model_.apply_selected_animation_graph_patch({operation},animation_graph_fingerprint_,false),nullptr,false);
                    const auto result=response.is_object()?response.value("result",nlohmann::json::object()):nlohmann::json::object();
                    last_action_status_=result.value("detail",result.value("code",std::string{"Animation Graph mask edit failed."}));
                }
                ImGui::EndDisabled();ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();ImGui::End();
}

void EditorUi::draw_asset_browser() {
    prepare_panel_window("editor.panel.assets");
    ImGui::Begin("Asset Browser");
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))set_focused_panel("editor.panel.assets");
    if(const auto reconciliation=model_.reconcile_active_asset_job())last_action_status_=reconciliation->detail;
    const auto registry=nlohmann::json::parse(model_.asset_registry_status_json(),nullptr,false);
    const auto registry_revision=registry.is_object()?registry.value("revision",0ULL):0ULL;
    const auto source_root_count=registry.is_object()?registry.value("sourceRoots",nlohmann::json::array()).size():0U;
    const auto asset_count_label=std::to_string(model_.assets().size())+(model_.assets().size()==1?" asset":" assets");
    draw_panel_heading("Project Assets",asset_count_label.c_str());
    ImGui::SameLine();
    const auto revision_label="REGISTRY r"+std::to_string(registry_revision);
    draw_status_badge(revision_label.c_str(),color_accent);
    ImGui::TextDisabled("%s  |  %zu source root(s)",project_context_.root.c_str(),source_root_count);
    ImGui::SetNextItemWidth(std::max(120.0F,ImGui::GetContentRegionAvail().x-82.0F));
    if(ImGui::InputTextWithHint("##asset-filter","Search name, type, state, or ID",asset_browser_filter_.data(),asset_browser_filter_.size()))
        set_asset_browser_query(asset_browser_filter_.data());
    ImGui::SameLine();
    if(ImGui::Button("Rescan",{-1.0F,0.0F}))last_action_status_=model_.refresh_assets().detail;
    ImGui::BeginDisabled(model_.selected_asset()==nullptr);
    if(ImGui::Button("Import"))last_action_status_=model_.import_selected_asset().detail;
    ImGui::SameLine();if(ImGui::Button("Inspect"))last_action_status_=model_.inspect_selected_asset().detail;
    ImGui::SameLine();if(ImGui::Button("Build Preview"))last_action_status_=model_.generate_selected_asset_thumbnail().detail;
    ImGui::SameLine();if(ImGui::Button("Cook Selected"))last_action_status_=model_.cook_selected_asset().detail;
    ImGui::EndDisabled();
    const auto browser_document=nlohmann::json::parse(retained_asset_browser_document_json(),nullptr,false);
    const auto browser_page=browser_document.is_object()?browser_document.value("page",nlohmann::json::object()):nlohmann::json::object();
    const auto browser_matched=browser_page.value("matched",std::size_t{});
    const auto browser_returned=browser_page.value("returned",std::size_t{});
    const auto browser_first=browser_returned==0U?0U:asset_browser_cursor_+1U;
    const auto browser_last=asset_browser_cursor_+browser_returned;
    ImGui::BeginDisabled(asset_browser_cursor_==0U);
    if(ImGui::SmallButton("Previous Page"))static_cast<void>(asset_browser_previous_page());
    ImGui::EndDisabled();ImGui::SameLine();
    ImGui::TextDisabled("%zu-%zu of %zu",browser_first,browser_last,browser_matched);ImGui::SameLine();
    ImGui::BeginDisabled(!browser_page.value("truncated",false));
    if(ImGui::SmallButton("Next Page"))static_cast<void>(asset_browser_next_page());
    ImGui::EndDisabled();
    const auto asset_job=nlohmann::json::parse(model_.active_asset_job_json(),nullptr,false);
    if(asset_job.is_object()&&asset_job.value("valid",false)) {
        const auto state=asset_job.value("state",std::string{"unknown"});
        const auto progress=std::clamp(asset_job.value("progress",0.0F),0.0F,1.0F);
        ImGui::SeparatorText("Background work");
        const auto job_label=asset_job.value("kind",std::string{"job"})+" / "+state;
        draw_panel_heading(job_label.c_str(),asset_job.value("stage",std::string{}).c_str());
        ImGui::ProgressBar(progress,ImVec2(-1.0F,0.0F),asset_job.value("stage",std::string{}).c_str());
        if(state=="queued"||state=="running") {
            if(ImGui::SmallButton("Cancel Job"))last_action_status_=model_.cancel_active_asset_job().detail;
        } else if(state=="failed") {
            if(ImGui::SmallButton("Retry Job"))last_action_status_=model_.retry_active_asset_job().detail;
        }
        if((state=="succeeded"||state=="failed"||state=="cancelled")&&!asset_job.value("detail",std::string{}).empty())
            ImGui::TextWrapped("%s",asset_job.value("detail",std::string{}).c_str());
        const auto artifacts=asset_job.value("artifacts",nlohmann::json::array());
        if(artifacts.is_array()&&!artifacts.empty()) {
            ImGui::TextDisabled("Artifacts: %zu",artifacts.size());
            for(const auto& artifact:artifacts) if(artifact.is_string()) ImGui::TextWrapped("%s",artifact.get<std::string>().c_str());
        }
    }
    ImGui::Separator();

    if(retained_asset_browser_texture_id_!=0&&retained_asset_browser_texture_width_>0&&
       retained_asset_browser_texture_height_>0) {
        const auto available=ImGui::GetContentRegionAvail();
        const auto draw_width=std::max(1.0F,available.x);
        const auto preferred_height=std::clamp(available.y*0.45F,120.0F,360.0F);
        const auto draw_height=std::max(1.0F,std::min(available.y,preferred_height));
        const auto origin=ImGui::GetCursorScreenPos();
        retained_asset_browser_canvas_x_=origin.x;retained_asset_browser_canvas_y_=origin.y;
        retained_asset_browser_canvas_width_=draw_width;retained_asset_browser_canvas_height_=draw_height;
        const auto scale=ImGui::GetIO().DisplayFramebufferScale;
        requested_asset_browser_width_=static_cast<std::uint32_t>(
            std::clamp(std::lround(draw_width*scale.x),320L,4096L));
        requested_asset_browser_height_=static_cast<std::uint32_t>(
            std::clamp(std::lround(draw_height*scale.y),96L,2048L));
        ImGui::Image(static_cast<ImTextureID>(retained_asset_browser_texture_id_),{draw_width,draw_height});
    } else {
    retained_asset_browser_canvas_width_=0.0F;
    retained_asset_browser_canvas_height_=0.0F;
    constexpr float card_width=144.0F;
    std::vector<std::size_t> visible_assets;
    if(browser_document.is_object())for(const auto& node:browser_document.value("nodes",nlohmann::json::array())) {
        if(!node.is_object()||node.value("role",std::string{})!="griditem")continue;
        const auto asset_id=node.value("asset",nlohmann::json::object()).value("id",std::string{});
        const auto found=std::ranges::find(model_.assets(),asset_id,&EditorAsset::id);
        if(found!=model_.assets().end())visible_assets.push_back(static_cast<std::size_t>(std::distance(model_.assets().begin(),found)));
    }
    const auto column_count=std::clamp(static_cast<int>((ImGui::GetContentRegionAvail().x+ImGui::GetStyle().ItemSpacing.x)/
        (card_width+ImGui::GetStyle().ItemSpacing.x)),1,8);
    if(visible_assets.empty())draw_empty_panel_state(model_.assets().empty()?"No assets yet":"No matching assets",
        model_.assets().empty()?"Add files to a project asset root, then rescan.":"Clear or refine the asset search.");
    else if(ImGui::BeginTable("asset-grid",column_count,ImGuiTableFlags_SizingFixedSame|ImGuiTableFlags_PadOuterX)) {
    for(const auto index:visible_assets) {
        const auto& asset=model_.assets().at(index);
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(index));
        ImGui::BeginGroup();
        const char* preview = asset.thumbnail_cached ? "[ Preview Ready ]" : "[ Build Preview ]";
        const bool selected = index == model_.selected_asset_index();
        std::uint32_t preview_hash=2166136261U;for(const auto value:asset.content_hash){preview_hash^=static_cast<unsigned char>(value);preview_hash*=16777619U;}
        const auto channel=[&](const int shift){return 0.14F+static_cast<float>((preview_hash>>shift)&0x7FU)/420.0F;};
        const auto resident=asset_thumbnail_textures_.find(asset.id);
        if(resident!=asset_thumbnail_textures_.end()&&resident->second!=0U) {
            const auto top_left=ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(resident->second),{card_width,52.0F});
            if(ImGui::IsItemClicked())static_cast<void>(select_asset(asset.id));
            if(selected)ImGui::GetWindowDrawList()->AddRect(top_left,{top_left.x+card_width,top_left.y+52.0F},
                IM_COL32(76,170,232,255),3.0F,0,2.0F);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,selected?ImVec4(0.16F,0.32F,0.46F,1.0F):ImVec4(channel(0),channel(8),channel(16),1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(channel(0)+0.08F,channel(8)+0.08F,channel(16)+0.08F,1.0F));
            if(ImGui::Button(preview,{card_width,52.0F}))static_cast<void>(select_asset(asset.id));
            ImGui::PopStyleColor(2);
        }
        ImGui::TextUnformatted(asset.name.c_str());
        ImGui::TextDisabled("%s  |  %s%s",asset.kind.c_str(),asset.import_state.c_str(),asset.thumbnail_cached?"  |  cached":"");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "%s\n%s\n%s\n%s",
                asset.id.c_str(),
                asset.source.c_str(),
                asset.content_hash.c_str(),
                asset.license.c_str());
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndTable();
    }
    }
    if(const auto* selected=model_.selected_asset()) {
        ImGui::Separator();ImGui::Text("Selected: %s",selected->name.c_str());ImGui::SameLine();
        ImGui::TextDisabled("%s  |  %s  |  %s",selected->id.c_str(),selected->import_state.c_str(),selected->license.c_str());
        ImGui::TextDisabled("Preview: %s  |  %s",selected->thumbnail_strategy.c_str(),
            selected->thumbnail_cached?selected->thumbnail_uri.c_str():"not cached");
        const auto repairs=nlohmann::json::parse(model_.selected_asset_repair_json(),nullptr,false);
        if(repairs.is_object()) {
            std::size_t selected_diagnostics{};
            for(const auto& diagnostic:repairs.value("diagnostics",nlohmann::json::array()))
                if(diagnostic.value("assetId",std::string{})==selected->id||diagnostic.value("dependencyId",std::string{})==selected->id) {
                    if(selected_diagnostics++==0U)ImGui::TextUnformatted("Diagnostics / Repair");
                    ImGui::BulletText("%s: %s",diagnostic.value("code",std::string{"asset.issue"}).c_str(),
                        diagnostic.value("detail",std::string{}).c_str());
                }
            for(const auto& action:repairs.value("plan",nlohmann::json::object()).value("actions",nlohmann::json::array())) {
                if(action.value("assetId",std::string{})!=selected->id&&action.value("relatedAssetId",std::string{})!=selected->id)continue;
                const auto action_id=action.value("actionId",std::string{});const auto label=action.value("kind",std::string{"repair"})+"##"+action_id;
                if(ImGui::SmallButton(label.c_str()))last_action_status_=model_.execute_selected_asset_repair(action_id).detail;
                ImGui::SameLine();
            }
            if(!repairs.value("plan",nlohmann::json::object()).value("actions",nlohmann::json::array()).empty())ImGui::NewLine();
        }
        const auto authoring=nlohmann::json::parse(model_.selected_tilemap_authoring_json(),nullptr,false);
        if(authoring.is_object()&&authoring.value("valid",false)&&ImGui::CollapsingHeader("Tilemap Authoring",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Git-reviewable sparse chunks  |  %s",authoring.value("fingerprint",std::string{}).c_str());
            const auto layers=authoring.value("layers",nlohmann::json::array());
            if(tile_brush_layer_id_.empty()&&!layers.empty())tile_brush_layer_id_=layers.at(0).value("id",std::string{});
            if(ImGui::BeginCombo("Layer",tile_brush_layer_id_.c_str())) {
                for(const auto& layer:layers) {const auto id=layer.value("id",std::string{});
                    if(ImGui::Selectable(id.c_str(),id==tile_brush_layer_id_))tile_brush_layer_id_=id;}
                ImGui::EndCombo();
            }
            if(ImGui::RadioButton("Paint",!tile_brush_erase_))tile_brush_erase_=false;ImGui::SameLine();
            if(ImGui::RadioButton("Erase",tile_brush_erase_))tile_brush_erase_=true;ImGui::SameLine();
            ImGui::Checkbox("Flip X",&tile_brush_flip_x_);ImGui::SameLine();ImGui::Checkbox("Flip Y",&tile_brush_flip_y_);
            if(ImGui::RadioButton("Brush",tile_brush_shape_==TileBrushShape::brush))tile_brush_shape_=TileBrushShape::brush;ImGui::SameLine();
            if(ImGui::RadioButton("Rectangle",tile_brush_shape_==TileBrushShape::rectangle))tile_brush_shape_=TileBrushShape::rectangle;ImGui::SameLine();
            if(ImGui::RadioButton("Flood",tile_brush_shape_==TileBrushShape::flood))tile_brush_shape_=TileBrushShape::flood;
            const auto tiles=authoring.at("palette").value("tiles",nlohmann::json::array());
            if(tile_brush_tile_id_.empty()&&!tiles.empty())tile_brush_tile_id_=tiles.at(0).value("id",std::string{});
            for(std::size_t tile_index=0;tile_index<tiles.size();++tile_index) {
                const auto id=tiles.at(tile_index).value("id",std::string{});const auto collision=tiles.at(tile_index).value("collision",std::string{});
                if(tile_index%4!=0)ImGui::SameLine();
                const auto selected_tile=id==tile_brush_tile_id_;if(selected_tile)ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.16F,0.32F,0.46F,1.0F));
                if(ImGui::Button((id+"\n"+collision).c_str(),{112,44})) {tile_brush_tile_id_=id;tile_brush_erase_=false;palette_rule_tile_id_.clear();}
                if(selected_tile)ImGui::PopStyleColor();
            }
            ImGui::TextDisabled("Brush uses asset.tilemap.stroke; rectangle/flood use asset.tilemap.region. Every gesture previews then commits once.");
            if(!tile_brush_tile_id_.empty()&&ImGui::CollapsingHeader("Autotile Rule")) {
                const auto palette=authoring.at("palette");const auto palette_fingerprint=palette.value("fingerprint",std::string{});
                const auto selected_tile=std::ranges::find_if(tiles,[&](const auto& tile){return tile.value("id",std::string{})==tile_brush_tile_id_;});
                if(selected_tile!=tiles.end()&&(palette_rule_tile_id_!=tile_brush_tile_id_||palette_rule_fingerprint_!=palette_fingerprint)) {
                    palette_rule_tile_id_=tile_brush_tile_id_;palette_rule_fingerprint_=palette_fingerprint;palette_rule_group_.fill(0);
                    palette_rule_enabled_.fill(false);for(auto& frame:palette_rule_frames_)frame.fill(0);
                    const auto autotile=selected_tile->value("autotile",nlohmann::json::object());
                    const auto group=autotile.value("group",std::string{});std::snprintf(palette_rule_group_.data(),palette_rule_group_.size(),"%s",group.c_str());
                    for(const auto& variant:autotile.value("variants",nlohmann::json::array())) {const auto mask=variant.value("mask",-1);
                        if(mask>=0&&mask<16){palette_rule_enabled_[mask]=true;const auto frame=variant.value("frame",std::string{});
                            std::snprintf(palette_rule_frames_[mask].data(),palette_rule_frames_[mask].size(),"%s",frame.c_str());}}
                }
                ImGui::TextDisabled("N/E/S/W bits: 1 / 2 / 4 / 8. Toggle exact masks; unmatched masks use the Tile base frame.");
                ImGui::SetNextItemWidth(240.0F);ImGui::InputText("Autotile Group",palette_rule_group_.data(),palette_rule_group_.size());
                const auto base_frame=selected_tile!=tiles.end()?selected_tile->value("frame",std::string{}):std::string{};
                for(std::size_t mask=0;mask<16;++mask) {
                    ImGui::PushID(static_cast<int>(mask));if(mask%4!=0)ImGui::SameLine();
                    const auto label=std::to_string(mask)+" "+(mask&1?"N":"-")+(mask&2?"E":"-")+(mask&4?"S":"-")+(mask&8?"W":"-");
                    if(palette_rule_enabled_[mask])ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.16F,0.32F,0.46F,1.0F));
                    if(ImGui::Button(label.c_str(),{105,30})) {palette_rule_enabled_[mask]=!palette_rule_enabled_[mask];
                        if(palette_rule_enabled_[mask]&&palette_rule_frames_[mask][0]=='\0')std::snprintf(palette_rule_frames_[mask].data(),palette_rule_frames_[mask].size(),"%s",base_frame.c_str());}
                    if(palette_rule_enabled_[mask])ImGui::PopStyleColor();ImGui::PopID();
                }
                for(std::size_t mask=0;mask<16;++mask)if(palette_rule_enabled_[mask]) {ImGui::PushID(100+static_cast<int>(mask));
                    ImGui::SetNextItemWidth(300.0F);const auto label="Mask "+std::to_string(mask)+" frame";
                    ImGui::InputText(label.c_str(),palette_rule_frames_[mask].data(),palette_rule_frames_[mask].size());ImGui::PopID();}
                if(ImGui::Button("Apply Autotile Rule")) {
                    std::vector<TileAutotileVariant> variants;for(std::size_t mask=0;mask<16;++mask)if(palette_rule_enabled_[mask])
                        variants.push_back({static_cast<std::uint8_t>(mask),palette_rule_frames_[mask].data()});
                    const auto preview=nlohmann::json::parse(model_.apply_selected_tile_palette_autotile(tile_brush_tile_id_,palette_rule_group_.data(),
                        variants,palette_rule_fingerprint_,true),nullptr,false);const auto result=preview.value("result",nlohmann::json::object());
                    if(preview.value("ok",false)&&result.value("success",false)) {const auto committed=nlohmann::json::parse(
                        model_.apply_selected_tile_palette_autotile(tile_brush_tile_id_,palette_rule_group_.data(),variants,palette_rule_fingerprint_,false),nullptr,false);
                        const auto committed_result=committed.value("result",nlohmann::json::object());last_action_status_=committed_result.value("code",std::string("palette edit failed"));}
                    else last_action_status_=result.value("code",std::string("palette preview failed"));
                }
            }
        }
    }
    ImGui::End();
}

void EditorUi::draw_console() {
    prepare_panel_window("editor.panel.console");
    ImGui::Begin("Console");
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))set_focused_panel("editor.panel.console");
    const auto scripting=nlohmann::json::parse(scripting_status_cache_,nullptr,false);
    const auto project=scripting.is_object()&&scripting.value("project",nlohmann::json{}).is_object()?
        scripting.at("project"):nlohmann::json::object();
    const auto host=scripting.is_object()&&scripting.value("host",nlohmann::json{}).is_object()?
        scripting.at("host"):nlohmann::json::object();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent),"C# Scripts");ImGui::SameLine();
    ImGui::TextDisabled("%s",project.value("scriptProject",std::string("not configured")).c_str());
    const auto can_compile=!project.value("scriptProject",std::string{}).empty();
    ImGui::SameLine();ImGui::BeginDisabled(!can_compile);
    if(ImGui::SmallButton("Open C# Project"))source_open_request_=EditorSourceOpenRequest{
        project.value("scriptProject",std::string{}),1U,1U};
    ImGui::EndDisabled();
    ImGui::SameLine();auto auto_build=auto_compile_scripts_;
    if(ImGui::Checkbox("Auto Build",&auto_build))set_auto_compile_scripts(auto_build);
    ImGui::BeginDisabled(!can_compile||script_compile_busy_);
    if(ImGui::Button("Build Debug"))static_cast<void>(begin_compile_scripts("Debug"));
    ImGui::SameLine();if(ImGui::Button("Build Release"))static_cast<void>(begin_compile_scripts("Release"));
    ImGui::EndDisabled();
    ImGui::SameLine();ImGui::Text("Host: %s",host.value("status",std::string("not-initialized")).c_str());
    if(scripting.contains("debugAttach")&&scripting.at("debugAttach").is_object()) {
        const auto& debug=scripting.at("debugAttach");
        ImGui::TextDisabled("Adapter %s | source symbols %s",
            debug.value("adapterReady",false)?"ready":"install/configure",
            debug.value("symbolsAvailable",false)?"ready":"build Debug first");
        ImGui::TextDisabled("Player distribution: %s",package_output_path_[0]=='\0'?"package a Debug Game Profile first":package_output_path_.data());
        const auto status=scripting.value("debugSession",nlohmann::json::object());
        const auto session=status.is_object()?status.value("session",nlohmann::json(nullptr)):nlohmann::json(nullptr);
        const auto state=session.is_object()?session.value("state",std::string{"not-started"}):std::string{"not-started"};
        const auto active=session.is_object()&&session.value("active",false);
        ImGui::Text("DAP Session: %s",state.c_str());
        ImGui::BeginDisabled(!debug.value("adapterReady",false)||package_output_path_[0]=='\0'||active);
        if(ImGui::Button("Launch Packaged Player"))managed_debug_request_=EditorManagedDebugRequest{
            .command=EditorManagedDebugCommand::start_attach,.package_output_path=package_output_path_.data(),
            .source_path=managed_debug_source_path_.data(),
            .line=static_cast<std::uint32_t>(std::max(1,managed_debug_breakpoint_line_))};
        ImGui::EndDisabled();
        ImGui::SameLine();ImGui::BeginDisabled(!active);
        if(ImGui::Button("Pause"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::pause,.thread_id=managed_debug_thread_id_};
        ImGui::SameLine();if(ImGui::Button("Continue"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::continue_execution,.thread_id=managed_debug_thread_id_};
        ImGui::SameLine();if(ImGui::Button("Stop Debugger"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::stop};
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(360.0F);ImGui::InputText("Breakpoint Source",managed_debug_source_path_.data(),managed_debug_source_path_.size());
        ImGui::SameLine();ImGui::SetNextItemWidth(80.0F);ImGui::InputInt("Line",&managed_debug_breakpoint_line_);
        ImGui::SameLine();ImGui::BeginDisabled(!active||managed_debug_source_path_[0]=='\0');
        if(ImGui::Button("Set Breakpoint"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::set_breakpoint,
            .source_path=managed_debug_source_path_.data(),.line=static_cast<std::uint32_t>(std::max(1,managed_debug_breakpoint_line_))};
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(90.0F);ImGui::InputScalar("Thread",ImGuiDataType_U64,&managed_debug_thread_id_);
        ImGui::SameLine();ImGui::BeginDisabled(!active);
        if(ImGui::Button("Step Over"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::step_over,.thread_id=managed_debug_thread_id_};
        ImGui::SameLine();if(ImGui::Button("Step In"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::step_in,.thread_id=managed_debug_thread_id_};
        ImGui::SameLine();if(ImGui::Button("Step Out"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::step_out,.thread_id=managed_debug_thread_id_};
        ImGui::SameLine();if(ImGui::Button("Threads / Stack"))managed_debug_request_=EditorManagedDebugRequest{.command=EditorManagedDebugCommand::refresh_stack,.thread_id=managed_debug_thread_id_};
        ImGui::EndDisabled();
        const auto last_action=nlohmann::json::parse(managed_debug_last_action_json_,nullptr,false);
        if(last_action.is_object())ImGui::TextWrapped("Last DAP action: %s (%s)",last_action.value("command",last_action.value("operation",std::string{})).c_str(),
            last_action.value("code",std::string{}).c_str());
        const auto event_document=nlohmann::json::parse(managed_debug_events_json_,nullptr,false);
        const auto events=event_document.is_object()?event_document.value("events",nlohmann::json::array()):nlohmann::json::array();
        if(!events.empty()&&ImGui::TreeNodeEx("Debugger Events",ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto first=events.size()>16U?events.size()-16U:0U;
            for(std::size_t index=first;index<events.size();++index)ImGui::TextDisabled("%s  %s",events[index].value("event",std::string{}).c_str(),events[index].value("body",nlohmann::json::object()).dump().c_str());
            ImGui::TreePop();
        }
    }
    if(script_compile_busy_) {
        const auto job=nlohmann::json::parse(script_compile_job_json_,nullptr,false);
        ImGui::TextColored(ImVec4{0.42F,0.65F,0.95F,1.0F},"BUILD RUNNING  %s  %lld ms",
            job.value("configuration",std::string{}).c_str(),static_cast<long long>(job.value("elapsedMilliseconds",0LL)));
    }
    if(project.contains("sourceState")&&project.at("sourceState").is_object()) {
        const auto& source=project.at("sourceState");
        const auto stale=source.value("needsCompile",false);
        ImGui::TextColored(stale?ImVec4{0.95F,0.72F,0.28F,1.0F}:ImVec4{0.42F,0.78F,0.58F,1.0F},
            "Source: %s (%s)",stale?"changed - build required":"up to date",source.value("configuration",std::string("Debug")).c_str());
    }
    if(project.contains("sourceDocuments")&&project.at("sourceDocuments").is_object()) {
        const auto& documents=project.at("sourceDocuments");const auto items=documents.value("items",nlohmann::json::array());
        const auto label="Project Sources ("+std::to_string(documents.value("count",items.size()))+")";
        if(ImGui::TreeNode(label.c_str())) {
            for(std::size_t index=0;index<items.size();++index) {
                const auto path=items[index].value("path",std::string{});ImGui::PushID(static_cast<int>(index));
                if(ImGui::Selectable(path.c_str()))static_cast<void>(open_script_source(path,1U,1U));
                ImGui::SameLine();if(ImGui::SmallButton("Open")) {
                    if(open_script_source(path,1U,1U)) {
                        const auto location=nlohmann::json::parse(script_source_location_json_);
                        source_open_request_=EditorSourceOpenRequest{location.value("path",std::string{}),1U,1U};
                    }
                }
                ImGui::PopID();
            }
            if(documents.value("truncated",false))ImGui::TextDisabled("Source list truncated; open the project for the complete tree.");
            ImGui::TreePop();
        }
    }
    if(!last_script_compile_json_.empty()) {
        const auto compile=nlohmann::json::parse(last_script_compile_json_,nullptr,false);
        if(compile.is_object()) {
            const auto success=compile.value("success",false);
            ImGui::TextColored(success?ImVec4{0.42F,0.78F,0.58F,1.0F}:ImVec4{0.95F,0.38F,0.32F,1.0F},
                "%s  %s",success?"BUILD SUCCEEDED":"BUILD FAILED",compile.value("configuration",std::string{}).c_str());
            ImGui::SameLine();ImGui::TextDisabled("cache %s (%s)",compile.value("cacheHit",false)?"hit":"miss",
                compile.value("cacheScope",std::string("none")).c_str());
            std::size_t diagnostic_index{};
            for(const auto& diagnostic:compile.value("diagnostics",nlohmann::json::array())) {
                const auto severity=diagnostic.value("severity",std::string("info"));
                const auto label=diagnostic.value("code",std::string{})+" "+diagnostic.value("file",std::string{})+":"+
                    std::to_string(diagnostic.value("line",0))+":"+std::to_string(diagnostic.value("column",0))+" "+
                    diagnostic.value("message",std::string{})+"##script-diagnostic-"+std::to_string(diagnostic_index++);
                ImGui::PushStyleColor(ImGuiCol_Text,severity=="error"?ImVec4{0.95F,0.38F,0.32F,1.0F}:ImVec4{0.95F,0.72F,0.28F,1.0F});
                if(ImGui::Selectable(label.c_str()))static_cast<void>(open_script_source(diagnostic.value("file",std::string{}),
                    diagnostic.value("line",1U),diagnostic.value("column",1U)));
                ImGui::PopStyleColor();
            }
        }
    }
    if(!script_source_location_json_.empty()) {
        const auto location=nlohmann::json::parse(script_source_location_json_,nullptr,false);
        if(location.is_object()&&ImGui::CollapsingHeader("Source Context",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("%s:%u:%u",location.value("projectRelativePath",std::string{}).c_str(),
                location.value("line",1U),location.value("column",1U));
            if(ImGui::SmallButton("Open in Code Editor"))source_open_request_=EditorSourceOpenRequest{
                location.value("path",std::string{}),location.value("line",1U),location.value("column",1U)};
            ImGui::SameLine();if(ImGui::SmallButton("Use as Breakpoint")) {
                const auto path=location.value("path",std::string{});
                std::snprintf(managed_debug_source_path_.data(),managed_debug_source_path_.size(),"%s",path.c_str());
                managed_debug_breakpoint_line_=static_cast<int>(location.value("line",1U));
            }
            for(const auto& source_line:location.value("excerpt",nlohmann::json::array()))
                ImGui::TextColored(source_line.value("focus",false)?ImVec4{0.95F,0.72F,0.28F,1.0F}:ImVec4{0.72F,0.76F,0.82F,1.0F},
                    "%4u %s",source_line.value("line",0U),source_line.value("text",std::string{}).c_str());
        }
    }
    const auto play_reload=nlohmann::json::parse(play_script_reload_json_,nullptr,false);
    if(play_reload.is_object())ImGui::TextColored(play_reload.value("success",false)?ImVec4{0.42F,0.78F,0.58F,1.0F}:
        ImVec4{0.95F,0.38F,0.32F,1.0F},"Play C# hot reload: %s",play_reload.value("detail",std::string{}).c_str());
    if(host.contains("lastManagedResult")&&host.at("lastManagedResult").is_object()) {
        const auto& result=host.at("lastManagedResult");
        ImGui::TextDisabled("Session %s | generation %d | %s.%s",host.value("sessionId",std::string{}).c_str(),
            result.value("loadGeneration",0),result.value("typeName",std::string{}).c_str(),result.value("callback",std::string{}).c_str());
        for(const auto& retired:result.value("retiredLoadContexts",nlohmann::json::array()))
            ImGui::TextDisabled("Retired generation %d: %s",retired.value("Generation",0),retired.value("Collected",false)?"collected":"pending GC");
        if(result.contains("migration")&&result.at("migration").is_object()) {
            const auto& migration=result.at("migration");
            ImGui::Text("State migration: %d restored, %d failed",migration.value("restoredCount",0),migration.value("failedCount",0));
            for(const auto& member:migration.value("members",nlohmann::json::array()))
                ImGui::TextDisabled("%s  %s%s%s",member.value("status",std::string{}).c_str(),member.value("key",std::string{}).c_str(),
                    member.value("detail",std::string{}).empty()?"":"  ",member.value("detail",std::string{}).c_str());
        }
    }
    ImGui::Separator();
    ImGui::TextColored({0.42F, 0.78F, 0.58F, 1.0F}, "INFO");
    ImGui::SameLine();
    ImGui::TextUnformatted("Editor shell initialized on SDL3 + SDL_GPU");
    ImGui::TextColored({0.42F, 0.65F, 0.95F, 1.0F}, "TRACE");
    ImGui::SameLine();
    ImGui::Text("Bootstrap scene: %zu live World entities", model_.objects().size());
    ImGui::TextColored({0.42F, 0.78F, 0.58F, 1.0F}, "READY");
    ImGui::SameLine();
    ImGui::TextUnformatted("Focused query, delta, plan/apply receipt and undo/redo");
    ImGui::End();
}

void EditorUi::draw_agent_context() {
    prepare_panel_window("editor.panel.agent-context");
    ImGui::Begin("Agent Context");
    if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))set_focused_panel("editor.panel.agent-context");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "Semantic Observation Preview");
    ImGui::TextWrapped(
        "Panels and the live ECS World now share stable identities, revisions and source anchors with Agent tools.");
    ImGui::Separator();
    const auto focused_panel=model_.focused_panel();
    ImGui::Text("Focused panel: %s",focused_panel=="editor.panel.animation-graph"?"Animation Graph":"Scene View");
    ImGui::Text("Selected object: %s", model_.selected_object().name.c_str());
    ImGui::TextDisabled("%s", model_.selected_object().id.c_str());
    ImGui::TextDisabled("World revision: %llu", static_cast<unsigned long long>(model_.world_revision()));
    ImGui::TextWrapped("Last transaction: %s", last_action_status_.c_str());
    if (ImGui::TreeNode("Focused observation bundle")) {
        ImGui::TextWrapped("%s", model_.focused_observation_json().c_str());
        ImGui::TreePop();
    }
    if (const auto* asset = model_.selected_asset(); asset != nullptr) {
        ImGui::Separator();
        ImGui::Text("Selected asset: %s", asset->name.c_str());
        ImGui::TextDisabled("%s", asset->id.c_str());
        if (ImGui::TreeNode("Imported asset metadata")) {
            ImGui::TextWrapped("%s", model_.selected_asset_inspection_json().c_str());
            ImGui::TreePop();
        }
    }
    if (ImGui::TreeNode("Raw snapshot")) {
        ImGui::TextWrapped("%s", semantic_snapshot_json().c_str());
        ImGui::TreePop();
    }
    if (!render_status_json_.empty() && ImGui::TreeNode("Renderer observation")) {
        ImGui::TextWrapped("%s", render_status_json_.c_str());
        ImGui::TreePop();
    }
    if (!engine_status_json_.empty() && ImGui::TreeNode("Engine module graph")) {
        ImGui::TextWrapped("%s", engine_status_json_.c_str());
        ImGui::TreePop();
    }
    ImGui::End();
}

} // namespace noemancer
