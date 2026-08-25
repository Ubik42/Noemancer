#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <array>

namespace noemancer {

struct RetainedUiVertex final {
    std::array<float,2> position{};
    std::uint32_t rgba{};
    std::array<float,2> texcoord{};
};

struct RetainedUiDraw final {
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    bool scissor_enabled{};
    std::array<std::int32_t,4> scissor{};
    std::uint64_t texture_id{};
};

struct RetainedUiTexture final {
    std::uint64_t id{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t revision{};
    std::vector<std::uint8_t> rgba8;
};

struct RetainedUiRenderPacket final {
    std::vector<RetainedUiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RetainedUiDraw> draws;
    std::vector<RetainedUiTexture> textures;
};

enum class RetainedUiKey : std::uint8_t {
    unknown, backspace, tab, enter, escape, home, end, left, right, up, down, insert_key, delete_key,
    a, c, v, x, y, z
};

enum RetainedUiModifier : std::uint32_t {
    retained_ui_modifier_none = 0,
    retained_ui_modifier_ctrl = 1U << 0U,
    retained_ui_modifier_shift = 1U << 1U,
    retained_ui_modifier_alt = 1U << 2U,
    retained_ui_modifier_caps_lock = 1U << 3U,
    retained_ui_modifier_num_lock = 1U << 4U
};

struct RetainedUiKeyboardRequest final {
    bool active{};
    std::int32_t caret_x{};
    std::int32_t caret_y{};
    std::int32_t line_height{};
    std::uint64_t revision{};
};

enum class RetainedUiActionKind : std::uint8_t { invoke, value_changed };

struct RetainedUiActionEvent final {
    std::uint64_t sequence{};
    RetainedUiActionKind kind{RetainedUiActionKind::invoke};
    std::string surface_id;
    std::string document_id;
    std::string node_id;
    std::string action_id;
    std::string binding_json{"{}"};
    std::string value_json;
};

struct RetainedUiImageReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::string image_source;
    std::uint64_t revision{};
    std::size_t resident_bytes{};
};

class RetainedUiRuntime final {
public:
    RetainedUiRuntime();
    ~RetainedUiRuntime();
    RetainedUiRuntime(const RetainedUiRuntime&) = delete;
    RetainedUiRuntime& operator=(const RetainedUiRuntime&) = delete;
    RetainedUiRuntime(RetainedUiRuntime&&) = delete;
    RetainedUiRuntime& operator=(RetainedUiRuntime&&) = delete;

    [[nodiscard]] bool initialize(std::uint32_t width, std::uint32_t height, float density_scale = 1.0F);
    [[nodiscard]] bool load_document(std::string_view document_id, std::string_view rml);
    [[nodiscard]] bool reload_document(std::string_view document_id, std::string_view rml);
    [[nodiscard]] bool update();
    [[nodiscard]] bool render();
    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height, float density_scale);
    [[nodiscard]] bool pointer_move(std::int32_t x, std::int32_t y);
    [[nodiscard]] bool pointer_button(std::uint32_t button, bool pressed);
    [[nodiscard]] bool pointer_leave();
    [[nodiscard]] bool key(RetainedUiKey key, bool pressed, std::uint32_t modifiers = retained_ui_modifier_none);
    [[nodiscard]] bool text_input(std::string_view utf8);
    [[nodiscard]] bool text_composition(std::string_view utf8, std::int32_t cursor, std::int32_t selection_length);
    [[nodiscard]] bool focus_node(std::string_view document_id, std::string_view semantic_node_id);
    [[nodiscard]] RetainedUiKeyboardRequest keyboard_request() const noexcept;
    [[nodiscard]] std::vector<RetainedUiActionEvent> consume_action_events();
    // Registers renderer-neutral RGBA8 pixels behind a stable imageSource.
    // The registry is process-local, bounded, and shared by every named UI surface.
    [[nodiscard]] RetainedUiImageReceipt register_image_rgba8(
        std::string_view image_source, std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> rgba8);
    [[nodiscard]] RetainedUiImageReceipt remove_image(std::string_view image_source);

    [[nodiscard]] bool create_surface(std::string_view surface_id,std::uint32_t width,std::uint32_t height,float density_scale=1.0F);
    [[nodiscard]] bool destroy_surface(std::string_view surface_id);
    [[nodiscard]] bool load_surface_document(std::string_view surface_id,std::string_view document_id,std::string_view rml);
    [[nodiscard]] bool reload_surface_document(std::string_view surface_id,std::string_view document_id,std::string_view rml);
    [[nodiscard]] bool update_surface(std::string_view surface_id);
    [[nodiscard]] bool render_surface(std::string_view surface_id);
    [[nodiscard]] bool resize_surface(std::string_view surface_id,std::uint32_t width,std::uint32_t height,float density_scale);
    [[nodiscard]] bool surface_pointer_move(std::string_view surface_id,std::int32_t x,std::int32_t y);
    [[nodiscard]] bool surface_pointer_button(std::string_view surface_id,std::uint32_t button,bool pressed);
    [[nodiscard]] bool surface_pointer_leave(std::string_view surface_id);
    [[nodiscard]] bool surface_key(std::string_view surface_id,RetainedUiKey key,bool pressed,
                                   std::uint32_t modifiers=retained_ui_modifier_none);
    [[nodiscard]] bool surface_text_input(std::string_view surface_id,std::string_view utf8);
    [[nodiscard]] bool surface_text_composition(std::string_view surface_id,std::string_view utf8,
                                                std::int32_t cursor,std::int32_t selection_length);
    [[nodiscard]] bool focus_surface_node(std::string_view surface_id,std::string_view document_id,
                                          std::string_view semantic_node_id);
    [[nodiscard]] std::string surface_observation_json(std::string_view surface_id,std::string_view document_id) const;
    [[nodiscard]] RetainedUiRenderPacket surface_render_packet(std::string_view surface_id) const;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;
    [[nodiscard]] std::string observation_json(std::string_view document_id) const;
    [[nodiscard]] std::string render_packet_json() const;
    [[nodiscard]] RetainedUiRenderPacket render_packet() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string retained_ui_rml_from_semantic_document(std::string_view semantic_ui_document_json);
[[nodiscard]] std::string retained_ui_text_capabilities_json(
    std::string_view locale = "en-US", std::string_view sample_text = {},
    std::string_view font_path = {}, float font_size = 16.0F);
[[nodiscard]] std::string retained_ui_preview_json(
    std::string_view semantic_ui_document_json,
    std::uint32_t width = 960,
    std::uint32_t height = 720,
    float density_scale = 1.0F);

} // namespace noemancer
