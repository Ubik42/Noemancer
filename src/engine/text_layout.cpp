#include "engine/text_layout.hpp"

#include <hb-ot.h>
#include <hb.h>
#include <nlohmann/json.hpp>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/uversion.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

namespace noemancer {
namespace {

using Json = nlohmann::json;

bool locale_is_rtl(std::string_view locale) {
    std::string language(locale.substr(0, locale.find_first_of("-_")));
    std::ranges::transform(language, language.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return language == "ar" || language == "fa" || language == "he" || language == "ur";
}

std::filesystem::path default_font_path(std::string_view locale) {
#if defined(_WIN32)
    char* windows_root = nullptr;
    std::size_t windows_root_length = 0;
    static_cast<void>(_dupenv_s(&windows_root, &windows_root_length, "WINDIR"));
    const auto font_root = std::filesystem::path(windows_root ? windows_root : "C:/Windows") / "Fonts";
    std::free(windows_root);
    const bool chinese=locale.starts_with("zh");
    const bool japanese=locale.starts_with("ja");
    const bool korean=locale.starts_with("ko");
    const std::vector<std::filesystem::path> candidates = locale_is_rtl(locale)
        ? std::vector<std::filesystem::path>{font_root / "segoeui.ttf", font_root / "arial.ttf"}
        : chinese ? std::vector<std::filesystem::path>{font_root / "msyh.ttc", font_root / "simhei.ttf"}
        : japanese ? std::vector<std::filesystem::path>{font_root / "YuGothR.ttc", font_root / "msgothic.ttc"}
        : korean ? std::vector<std::filesystem::path>{font_root / "malgun.ttf", font_root / "gulim.ttc"}
        : std::vector<std::filesystem::path>{font_root / "segoeui.ttf", font_root / "arial.ttf"};
    for (const auto& candidate : candidates) if (std::filesystem::exists(candidate)) return candidate;
#elif defined(__APPLE__)
    const std::vector<std::filesystem::path> candidates{
        "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/AppleSDGothicNeo.ttc"};
    for (const auto& candidate : candidates) if (std::filesystem::exists(candidate)) return candidate;
#else
    const std::vector<std::filesystem::path> candidates{
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const auto& candidate : candidates) if (std::filesystem::exists(candidate)) return candidate;
#endif
    return {};
}

std::vector<UChar> utf16_from_utf8(std::string_view text, UErrorCode& status) {
    int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, text.data(), static_cast<int32_t>(text.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) return {};
    status = U_ZERO_ERROR;
    std::vector<UChar> result(static_cast<std::size_t>(length) + 1U);
    u_strFromUTF8(result.data(), length + 1, &length, text.data(), static_cast<int32_t>(text.size()), &status);
    if (U_FAILURE(status)) return {};
    result.resize(static_cast<std::size_t>(length));
    return result;
}

struct HbBlobDeleter { void operator()(hb_blob_t* value) const { if (value) hb_blob_destroy(value); } };
struct HbFaceDeleter { void operator()(hb_face_t* value) const { if (value) hb_face_destroy(value); } };
struct HbFontDeleter { void operator()(hb_font_t* value) const { if (value) hb_font_destroy(value); } };
struct HbBufferDeleter { void operator()(hb_buffer_t* value) const { if (value) hb_buffer_destroy(value); } };
struct BidiDeleter { void operator()(UBiDi* value) const { if (value) ubidi_close(value); } };
struct BreakDeleter { void operator()(UBreakIterator* value) const { if (value) ubrk_close(value); } };

Json failure(std::string code, std::string message, const TextLayoutRequest& request) {
    return {{"schemaVersion", "noemancer.text-layout/0.1"}, {"valid", false}, {"code", std::move(code)},
            {"message", std::move(message)}, {"locale", request.locale}};
}

} // namespace

std::string text_layout_inspect_json(const TextLayoutRequest& request) {
    if (request.text.size() > 64U * 1024U)
        return failure("ui.text.input-too-large", "Text layout inspection is limited to 64 KiB", request).dump();
    if (!std::isfinite(request.font_size) || request.font_size < 4.0F || request.font_size > 512.0F)
        return failure("ui.text.invalid-font-size", "fontSize must be within [4, 512]", request).dump();

    UErrorCode status = U_ZERO_ERROR;
    auto utf16 = utf16_from_utf8(request.text, status);
    if (U_FAILURE(status))
        return failure("ui.text.invalid-utf8", u_errorName(status), request).dump();

    auto font_path = request.font_path.empty() ? default_font_path(request.locale) : std::filesystem::path(request.font_path);
    if (font_path.empty() || !std::filesystem::is_regular_file(font_path))
        return failure("ui.text.font-unavailable", "No readable font was resolved for the locale", request).dump();

    std::unique_ptr<hb_blob_t, HbBlobDeleter> blob(hb_blob_create_from_file_or_fail(font_path.string().c_str()));
    if (!blob) return failure("ui.text.font-load-failed", "HarfBuzz could not load the resolved font", request).dump();
    std::unique_ptr<hb_face_t, HbFaceDeleter> face(hb_face_create(blob.get(), 0));
    std::unique_ptr<hb_font_t, HbFontDeleter> font(hb_font_create(face.get()));
    hb_ot_font_set_funcs(font.get());
    const auto scale = static_cast<int>(std::lround(request.font_size * 64.0F));
    hb_font_set_scale(font.get(), scale, scale);
    hb_font_set_ppem(font.get(), static_cast<unsigned>(std::lround(request.font_size)), static_cast<unsigned>(std::lround(request.font_size)));

    status = U_ZERO_ERROR;
    std::unique_ptr<UBiDi, BidiDeleter> bidi(ubidi_openSized(static_cast<int32_t>(utf16.size()), 0, &status));
    if (U_FAILURE(status) || !bidi) return failure("ui.text.bidi-init-failed", u_errorName(status), request).dump();
    const UBiDiLevel paragraph_level = locale_is_rtl(request.locale) ? UBIDI_DEFAULT_RTL : UBIDI_DEFAULT_LTR;
    ubidi_setPara(bidi.get(), utf16.data(), static_cast<int32_t>(utf16.size()), paragraph_level, nullptr, &status);
    if (U_FAILURE(status)) return failure("ui.text.bidi-analysis-failed", u_errorName(status), request).dump();

    const int32_t run_count = ubidi_countRuns(bidi.get(), &status);
    if (U_FAILURE(status)) return failure("ui.text.bidi-runs-failed", u_errorName(status), request).dump();
    Json runs = Json::array();
    std::uint64_t glyph_count = 0;
    for (int32_t visual_index = 0; visual_index < run_count; ++visual_index) {
        int32_t logical_start = 0;
        int32_t length = 0;
        const auto direction = ubidi_getVisualRun(bidi.get(), visual_index, &logical_start, &length);
        std::unique_ptr<hb_buffer_t, HbBufferDeleter> buffer(hb_buffer_create());
        hb_buffer_add_utf16(buffer.get(), reinterpret_cast<const uint16_t*>(utf16.data()), static_cast<int>(utf16.size()), logical_start, length);
        hb_buffer_set_direction(buffer.get(), direction == UBIDI_RTL ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_language(buffer.get(), hb_language_from_string(request.locale.c_str(), static_cast<int>(request.locale.size())));
        hb_buffer_guess_segment_properties(buffer.get());
        hb_shape(font.get(), buffer.get(), nullptr, 0);

        unsigned count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
        const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer.get(), &count);
        Json glyphs = Json::array();
        float pen_x = 0.0F;
        float pen_y = 0.0F;
        for (unsigned index = 0; index < count; ++index) {
            const float x_advance = static_cast<float>(positions[index].x_advance) / 64.0F;
            const float y_advance = static_cast<float>(positions[index].y_advance) / 64.0F;
            glyphs.push_back({{"glyphId", infos[index].codepoint}, {"clusterUtf16", infos[index].cluster},
                {"x", pen_x}, {"y", pen_y}, {"xOffset", static_cast<float>(positions[index].x_offset) / 64.0F},
                {"yOffset", static_cast<float>(positions[index].y_offset) / 64.0F},
                {"xAdvance", x_advance}, {"yAdvance", y_advance}});
            pen_x += x_advance;
            pen_y += y_advance;
        }
        glyph_count += count;
        runs.push_back({{"visualIndex", visual_index}, {"logicalStartUtf16", logical_start}, {"lengthUtf16", length},
            {"direction", direction == UBIDI_RTL ? "rtl" : "ltr"}, {"advanceX", pen_x}, {"glyphs", std::move(glyphs)}});
    }

    status = U_ZERO_ERROR;
    std::unique_ptr<UBreakIterator, BreakDeleter> line_breaker(ubrk_open(
        UBRK_LINE, request.locale.c_str(), utf16.data(), static_cast<int32_t>(utf16.size()), &status));
    if (U_FAILURE(status) || !line_breaker)
        return failure("ui.text.line-break-init-failed", u_errorName(status), request).dump();
    Json line_breaks = Json::array();
    for (int32_t offset = ubrk_first(line_breaker.get()); offset != UBRK_DONE; offset = ubrk_next(line_breaker.get())) {
        line_breaks.push_back({{"offsetUtf16", offset}, {"ruleStatus", ubrk_getRuleStatus(line_breaker.get())}});
    }

    UVersionInfo icu_version{};
    u_getVersion(icu_version);
    char icu_version_text[U_MAX_VERSION_STRING_LENGTH]{};
    u_versionToString(icu_version, icu_version_text);
    Json result{{"schemaVersion", "noemancer.text-layout/0.1"}, {"valid", true}, {"code", "ok"},
        {"locale", request.locale}, {"fontPath", font_path.generic_string()}, {"fontSize", request.font_size},
        {"textBytes", request.text.size()}, {"utf16Units", utf16.size()},
        {"codePoints", u_countChar32(utf16.data(), static_cast<int32_t>(utf16.size()))},
        {"baseDirection", ubidi_getParaLevel(bidi.get()) % 2U ? "rtl" : "ltr"},
        {"visualRunCount", run_count}, {"glyphCount", glyph_count}, {"visualRuns", std::move(runs)},
        {"lineBreaks", std::move(line_breaks)},
        {"engines", {{"shaping", "HarfBuzz"}, {"harfBuzzVersion", hb_version_string()},
            {"bidirectionalLayout", "ICU UBiDi"}, {"lineBreaking", "ICU BreakIterator"}, {"icuVersion", icu_version_text}}},
        {"renderIntegration", {{"rendererNeutralPlan", true}, {"retainedGlyphRunConsumer", true},
            {"backend", "RmlUi FreeType shaped-atlas adapter"},
            {"next", "cache shaped runs and expose cluster-aware caret selection"}}}};
    return result.dump();
}

} // namespace noemancer
