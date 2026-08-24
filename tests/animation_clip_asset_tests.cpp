#include "engine/animation_clip_asset.hpp"

#include <iostream>

int main() {
    using noemancer::AnimationClipAssetCodec;
    constexpr auto source = R"({
      "schemaVersion":"noemancer.animation-clip/0.1",
      "assetId":"animation.hero.run",
      "sourceAsset":"model.hero.source",
      "skinIndex":0,
      "animationIndex":2,
      "compression":"ozz_runtime_baseline"
    })";
    const auto parsed = AnimationClipAssetCodec::parse_json(source);
    if (!parsed || parsed.document->asset_id != "animation.hero.run" ||
        parsed.document->source_asset != "model.hero.source" || parsed.document->animation_index != 2U) {
        std::cerr << "Valid Animation Clip descriptor was rejected.\n";
        return 1;
    }
    const auto canonical = AnimationClipAssetCodec::write_canonical_json(*parsed.document);
    const auto round_trip = AnimationClipAssetCodec::parse_json(canonical);
    const auto build_inputs = AnimationClipAssetCodec::build_inputs(*parsed.document);
    if (!round_trip || canonical != AnimationClipAssetCodec::write_canonical_json(*round_trip.document) ||
        build_inputs.size() != 1U || build_inputs.front() != "model.hero.source") {
        std::cerr << "Animation Clip canonical form or Cook build input is unstable.\n";
        return 2;
    }
    const auto unknown = AnimationClipAssetCodec::parse_json(
        R"({"schemaVersion":"noemancer.animation-clip/0.1","assetId":"a","sourceAsset":"b","skinIndex":0,"animationIndex":0,"compression":"ozz_runtime_baseline","runtimePath":"source.fbx"})");
    if (unknown || unknown.code != "animation.clip.unknown-field") {
        std::cerr << "Animation Clip accepted an unknown persistence field.\n";
        return 3;
    }
    const auto invalid_compression = AnimationClipAssetCodec::parse_json(
        R"({"schemaVersion":"noemancer.animation-clip/0.1","assetId":"a","sourceAsset":"b","skinIndex":0,"animationIndex":0,"compression":"acl"})");
    if (invalid_compression || invalid_compression.code != "animation.clip.compression-invalid") {
        std::cerr << "Animation Clip accepted an unproven production codec.\n";
        return 4;
    }
    return 0;
}
