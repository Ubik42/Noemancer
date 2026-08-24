#include "engine/text_layout.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <unordered_set>

int main() {
    const std::string arabic = "\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";
    const auto shaped = nlohmann::json::parse(noemancer::text_layout_inspect_json({.text=arabic,.locale="ar-SA",.font_size=20.0F}));
    if (!shaped.at("valid").get<bool>() || shaped.at("baseDirection") != "rtl" ||
        shaped.at("visualRunCount") != 1 || shaped.at("visualRuns").at(0).at("direction") != "rtl" ||
        shaped.at("glyphCount").get<unsigned>() == 0U || shaped.at("engines").at("shaping") != "HarfBuzz" ||
        !shaped.at("renderIntegration").at("rendererNeutralPlan").get<bool>() ||
        !shaped.at("renderIntegration").at("retainedGlyphRunConsumer").get<bool>()) {
        std::cerr << "Arabic shaping plan did not expose truthful glyph-run evidence\n"; return 1;
    }
    const auto& glyphs=shaped.at("visualRuns").at(0).at("glyphs");
    if(glyphs.size()<2U||glyphs.front().at("clusterUtf16").get<unsigned>()<=glyphs.back().at("clusterUtf16").get<unsigned>()) {
        std::cerr << "RTL shaping did not preserve descending logical clusters\n"; return 2;
    }
    std::unordered_set<unsigned> shaped_glyph_ids;
    for(const auto& glyph:glyphs) {
        const auto glyph_id=glyph.at("glyphId").get<unsigned>();
        if(glyph_id==0U) { std::cerr << "Arabic shaping resolved to .notdef\n"; return 5; }
        shaped_glyph_ids.insert(glyph_id);
    }
    if(shaped_glyph_ids.size()<4U) { std::cerr << "Arabic contextual forms were not shaped\n"; return 6; }

    const std::string mixed = "Noemancer 42 \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";
    const auto bidi=nlohmann::json::parse(noemancer::text_layout_inspect_json({.text=mixed,.locale="en-US"}));
    if(!bidi.at("valid").get<bool>()||bidi.at("visualRunCount").get<unsigned>()<2U) {
        std::cerr << "Mixed-direction paragraph was not split into visual runs\n"; return 3;
    }

    const std::string cjk="\xE5\xBC\x95\xE6\x93\x8E\xE5\xBC\x80\xE5\x8F\x91\xE7\x95\x8C\xE9\x9D\xA2\xE6\xB5\x8B\xE8\xAF\x95";
    const auto breaks=nlohmann::json::parse(noemancer::text_layout_inspect_json({.text=cjk,.locale="zh-CN"}));
    if(!breaks.at("valid").get<bool>()||breaks.at("lineBreaks").size()<5U||
       breaks.at("engines").at("lineBreaking")!="ICU BreakIterator") {
        std::cerr << "Locale line-break analysis did not expose CJK opportunities\n"; return 4;
    }
    return 0;
}
