#pragma once

#include <string>
#include <string_view>

namespace noemancer {

struct TextLayoutRequest final {
    std::string text;
    std::string locale{"en-US"};
    std::string font_path;
    float font_size{16.0F};
};

// Produces an engine-owned, renderer-neutral Unicode layout plan. Glyph IDs,
// clusters, advances, visual runs and legal line-break opportunities are kept
// separate from RmlUi so editor, game UI, headless tools and future renderers
// all observe the same text semantics.
[[nodiscard]] std::string text_layout_inspect_json(const TextLayoutRequest& request);

} // namespace noemancer
