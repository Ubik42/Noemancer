#include "engine/render_reference_scene.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <string>

namespace noemancer {

namespace {

SceneEntityDocument material_sphere(const std::uint32_t column, const std::uint32_t row) {
    constexpr std::array<SceneVector3, 5> colors{{
        {0.82, 0.82, 0.82},
        {0.72, 0.18, 0.10},
        {0.08, 0.34, 0.72},
        {0.10, 0.62, 0.28},
        {0.72, 0.46, 0.08}
    }};
    const auto suffix = std::to_string(row) + "-" + std::to_string(column);
    return SceneEntityDocument{
        .guid = "entity.reference.material-" + suffix,
        .name = "Material " + suffix,
        .parent_guid = "entity.reference.root",
        .transform = SceneTransform{{-3.1 + static_cast<double>(column) * 1.55,
                                     0.8 + static_cast<double>(row) * 1.55, 0.0},
                                    {0.68, 0.68, 0.68}},
        .mesh_renderer = SceneMeshRenderer{"asset.primitive.sphere", true, true, true},
        .pbr_material = ScenePbrMaterial{colors[column],
            static_cast<double>(column) / 4.0,
            0.08 + static_cast<double>(row) * 0.22}
    };
}

SceneEntityDocument color_response_patch(const std::string& guid, const std::string& name,
                                         const SceneVector3 position,
                                         const SceneVector3 emissive_color,
                                         const double emissive_intensity) {
    return SceneEntityDocument{
        .guid = guid, .name = name, .parent_guid = "entity.reference.root",
        .transform = SceneTransform{position, {0.30, 0.18, 0.10}},
        .mesh_renderer = SceneMeshRenderer{"asset.primitive.cube", true, false, false},
        // Black fully-metallic base removes the dielectric F0 and diffuse term,
        // leaving an isolated scene-linear emissive signal for tone-map evidence.
        .pbr_material = ScenePbrMaterial{{0.0, 0.0, 0.0}, 1.0, 1.0, {},
                                         emissive_color, emissive_intensity}
    };
}

} // namespace

SceneDocument make_commercial_raster_reference_scene_document() {
    SceneDocument document{
        .scene_guid = std::string(commercial_raster_reference_scene_guid),
        .name = std::string(commercial_raster_reference_name),
        .source_uri = std::string(commercial_raster_reference_source_uri),
        .entities = {
            {.guid="entity.reference.root", .name="Reference Root"},
            {.guid="entity.camera.editor", .name="Reference Camera", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{0.0, 4.4, 16.5}},
             .camera=SceneCamera{{0.0, 3.8, 0.0}, 43.0, 0.1, 120.0, true}},
            {.guid="entity.sun", .name="Reference Sun", .parent_guid="entity.reference.root",
             .directional_light=SceneDirectionalLight{{-0.46, -1.0, -0.31}, {1.0, 0.93, 0.82}, 1.15, 0.12, true}},
            {.guid="entity.reference.light-warm", .name="Warm Point Light", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{4.2, 2.2, 2.8}},
             .local_light=SceneLocalLight{"point",{1.0,0.28,0.06},420.0,7.5,{0.0,-1.0,0.0},25.0,35.0,0.12,true}},
            {.guid="entity.reference.light-cool", .name="Cool Point Light", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{-4.0, 4.6, 1.4}},
             .local_light=SceneLocalLight{"point",{0.08,0.35,1.0},480.0,8.5,{0.0,-1.0,0.0},25.0,35.0,0.10,true}},
            {.guid="entity.reference.light-spot", .name="Material Grid Spot", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{0.0, 8.0, 4.5}},
             .local_light=SceneLocalLight{"spot",{0.92,0.96,1.0},360.0,14.0,{0.0,-0.82,-0.57},18.0,30.0,0.18,true}},
            {.guid="entity.reference.ground", .name="Neutral Ground", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{0.0, -0.08, 0.0}, {8.0, 1.0, 8.0}},
             .mesh_renderer=SceneMeshRenderer{"asset.primitive.plane", true, false, true},
             .pbr_material=ScenePbrMaterial{{0.18, 0.20, 0.23}, 0.0, 0.78}},
            {.guid="entity.reference.shadow-column", .name="Shadow Cadence", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{-5.4, 2.0, 0.4}, {0.55, 4.0, 0.55}},
             .mesh_renderer=SceneMeshRenderer{"asset.primitive.cube", true, true, true},
             .pbr_material=ScenePbrMaterial{{0.34, 0.36, 0.40}, 0.15, 0.34}},
            // These two ordinary PBR entities are deliberately kept on stable IDs so
            // the reference scene remains a durable authored-material fixture.  The
            // small pin is isolated against the dark upper background/column while
            // the broad panel exercises a weaker, spatially distributed emission.
            {.guid="entity.reference.emissive-warm", .name="Bloom Soft Emissive Area", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{0.4, 7.55, -0.8}, {2.25, 0.42, 0.12}},
             .mesh_renderer=SceneMeshRenderer{"asset.primitive.cube", true, false, true},
             .pbr_material=ScenePbrMaterial{{0.012, 0.032, 0.05}, 0.0, 0.3, {}, {0.08, 0.28, 0.42}, 2.2}},
            {.guid="entity.reference.emissive-cool", .name="Bloom High-Intensity Pin", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{-4.15, 5.75, 1.1}, {0.24, 0.24, 0.24}},
             .mesh_renderer=SceneMeshRenderer{"asset.primitive.sphere", true, false, true},
             .pbr_material=ScenePbrMaterial{{0.004, 0.006, 0.01}, 0.0, 0.22, {}, {1.0, 0.28, 0.025}, 18.0}},
            {.guid="entity.reference.textured-materials", .name="glTF Texture and Alpha Cards", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{5.8, 3.7, 1.2}, {0.75, 0.75, 0.75}},
             .mesh_renderer=SceneMeshRenderer{"asset.test.material-reference", true, true, true}},
            {.guid="entity.reference.depth-near", .name="Depth Near", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{-5.4, 0.55, 3.0}, {0.45, 0.45, 0.45}},
             .mesh_renderer=SceneMeshRenderer{"asset.primitive.sphere", true, true, true},
             .pbr_material=ScenePbrMaterial{{0.8, 0.8, 0.82}, 0.9, 0.14}},
            {.guid="entity.reference.depth-far", .name="Depth Far", .parent_guid="entity.reference.root",
             .transform=SceneTransform{{5.2, 5.2, -4.0}, {0.9, 0.9, 0.9}},
             .mesh_renderer=SceneMeshRenderer{"asset.primitive.sphere", true, true, true},
             .pbr_material=ScenePbrMaterial{{0.06, 0.07, 0.09}, 0.0, 0.92}}
        }
    };
    document.entities.reserve(document.entities.size() + 46U);
    for (std::uint32_t row=0; row<5U; ++row)
        for (std::uint32_t column=0; column<5U; ++column)
            document.entities.push_back(material_sphere(column, row));
    // Keep the AO/material fixture on stable ordinary PBR entities outside the
    // v1.6 Bloom source and halo regions.  The contact block meets the ground,
    // the wall forms a concave corner behind it, and the rear slab supplies a
    // dark, low-frequency background for an enabled/disabled A/B comparison.
    document.entities.push_back({
        .guid="entity.reference.ao-contact-prop", .name="AO Contact Prop", .parent_guid="entity.reference.root",
        .transform=SceneTransform{{4.05, 0.25, -1.0}, {0.48, 0.33, 0.48}},
        .mesh_renderer=SceneMeshRenderer{"asset.primitive.cube", true, false, true},
        .pbr_material=ScenePbrMaterial{{0.24, 0.26, 0.29}, 0.82, 0.18}});
    document.entities.push_back({
        .guid="entity.reference.ao-corner-wall", .name="AO Concave Corner Wall", .parent_guid="entity.reference.root",
        .transform=SceneTransform{{4.05, 1.30, -1.40}, {1.40, 1.38, 0.14}},
        .mesh_renderer=SceneMeshRenderer{"asset.primitive.cube", true, false, true},
        .pbr_material=ScenePbrMaterial{{0.055, 0.065, 0.08}, 0.0, 0.88}});
    document.entities.push_back({
        .guid="entity.reference.ao-dark-backdrop", .name="AO Dark Background", .parent_guid="entity.reference.root",
        .transform=SceneTransform{{4.05, 2.60, -1.58}, {2.0, 2.60, 0.10}},
        .mesh_renderer=SceneMeshRenderer{"asset.primitive.cube", true, false, true},
        .pbr_material=ScenePbrMaterial{{0.012, 0.015, 0.020}, 0.0, 0.96}});
    // Keep the color-response fixture in the open lower-right ground band.  It
    // is emissive-authored for stable channel measurements, starts beyond the
    // AO backdrop/contact ROI, and remains below the existing textured cards.
    constexpr std::array<SceneVector3, 6> neutral_ramp_colors{{
        {0.03, 0.03, 0.03}, {0.08, 0.08, 0.08}, {0.18, 0.18, 0.18},
        {0.36, 0.36, 0.36}, {0.60, 0.60, 0.60}, {0.90, 0.90, 0.90}
    }};
    constexpr std::array<SceneVector3, 6> chromatic_patch_colors{{
        {0.85, 0.03, 0.02}, {0.03, 0.75, 0.05}, {0.02, 0.08, 0.85},
        {0.02, 0.75, 0.78}, {0.78, 0.03, 0.72}, {0.84, 0.76, 0.03}
    }};
    constexpr std::array<std::string_view, 6> chromatic_patch_names{{
        "Red", "Green", "Blue", "Cyan", "Magenta", "Yellow"
    }};
    constexpr std::array<double, 6> hdr_emissive_intensities{{0.125, 0.25, 0.50, 1.0, 2.0, 4.0}};
    for (std::uint32_t column=0; column<6U; ++column) {
        const auto position = SceneVector3{8.2 + static_cast<double>(column) * 0.42, 0.75, -0.7};
        document.entities.push_back(color_response_patch(
            "entity.reference.color-neutral-" + std::to_string(column),
            "Color Neutral Exposure " + std::to_string(column), position,
            neutral_ramp_colors[column], 1.0));
        document.entities.push_back(color_response_patch(
            "entity.reference.color-chromatic-" + std::to_string(column),
            "Color " + std::string(chromatic_patch_names[column]),
            {position.x, 1.35, position.z}, chromatic_patch_colors[column], 0.25));
        document.entities.push_back(color_response_patch(
            "entity.reference.color-hdr-" + std::to_string(column),
            "Color HDR Roll-off " + std::to_string(column),
            {position.x, 1.95, position.z}, {0.45, 0.45, 0.45},
            hdr_emissive_intensities[column]));
    }
    return document;
}

std::string commercial_raster_reference_contract_json() {
    using Json = nlohmann::json;
    return Json{
        {"schemaVersion", "noemancer.render-reference-contract/0.1"},
        {"id", commercial_raster_reference_scene_id},
        {"name", commercial_raster_reference_name},
        {"sceneGuid", commercial_raster_reference_scene_guid},
        {"sourceUri", commercial_raster_reference_source_uri},
        {"capture", {{"width",1920},{"height",1080},{"warmupFrames",32},{"captureFrame",64},
                     {"exposureCompensation",1.0},{"renderScale",1.0},{"temporalDebugView","final"}}},
        {"bloomFixture", {
            {"schemaVersion", "noemancer.bloom-quality-fixture/0.1"},
            {"smallHighIntensitySourceId", "entity.reference.emissive-cool"},
            {"largeWeakAreaSourceId", "entity.reference.emissive-warm"},
            {"darkNeighbor", "upper-background-and-shadow-column"},
            {"smallSourceWorldPosition", {-4.15, 5.75, 1.1}},
            {"largeAreaWorldPosition", {0.4, 7.55, -0.8}},
            {"analysisCoordinateSpace", "output-normalized"},
            {"analysisIntent", "small-core-near-halo-far-halo-energy-cap"},
            {"sourceSearchRoi", {{"shape", "rectangle"}, {"x0", 0.24}, {"x1", 0.42},
                                  {"y0", 0.24}, {"y1", 0.53}}},
            {"coreRoi", {{"shape", "disk"}, {"center", "brightest-pixel-in-source-search-roi"},
                         {"radius", {{"minimumPixels", 8.0}, {"minOutputDimensionFraction", 0.012}}}}},
            {"nearHaloRoi", {{"shape", "annulus"}, {"center", "coreRoi"},
                              {"innerRadiusCoreMultiplier", 1.5}, {"outerRadiusCoreMultiplier", 3.5}}},
            {"farHaloRoi", {{"shape", "annulus"}, {"center", "coreRoi"},
                             {"innerRadiusCoreMultiplier", 3.5}, {"outerRadiusCoreMultiplier", 6.5}}},
            {"baselineRoi", {{"shape", "annulus"}, {"center", "coreRoi"},
                              {"innerRadiusCoreMultiplier", 7.5}, {"outerRadiusCoreMultiplier", 11.0}}},
            {"largeWeakAreaRoi", {{"shape", "rectangle"}, {"x0", 0.36}, {"x1", 0.64},
                                   {"y0", 0.13}, {"y1", 0.31}}},
            {"largeWeakAreaBaselineRoi", {{"shape", "rectangle"}, {"x0", 0.36}, {"x1", 0.64},
                                            {"y0", 0.06}, {"y1", 0.12}}}
        }},
        {"materialAoFixture", {
            {"schemaVersion", "noemancer.material-ao-fixture/0.1"},
            {"analysisCoordinateSpace", "output-normalized"},
            {"analysisIntent", "metallic-roughness-gradient-contact-concave-ao-ab"},
            {"comparison", "ao-enabled-minus-disabled-linear-luma"},
            {"abIntent", {
                {"enabledImage", "ao-enabled"}, {"disabledImage", "ao-disabled"},
                {"delta", "enabled-minus-disabled-linear-luma"},
                {"aoExpectedSign", "negative"}, {"controlExpectedSign", "near-zero"}
            }},
            {"materialGradientSourceIds", Json::array({
                "entity.reference.material-0-0", "entity.reference.material-0-4",
                "entity.reference.material-4-0", "entity.reference.material-4-4"})},
            {"contactSourceId", "entity.reference.ao-contact-prop"},
            {"concaveSourceIds", Json::array({"entity.reference.ground", "entity.reference.ao-corner-wall"})},
            {"darkBackgroundSourceId", "entity.reference.ao-dark-backdrop"},
            {"materialGradient", {
                {"metallic", {{"axis", "column"}, {"start", 0.0}, {"end", 1.0}}},
                {"roughness", {{"axis", "row"}, {"start", 0.08}, {"end", 0.96}}}
            }},
            {"rois", {
                {"materialGradient", {{"shape", "rectangle"}, {"x0", 0.25}, {"x1", 0.62},
                                       {"y0", 0.53}, {"y1", 0.86}}},
                {"ao", {{"shape", "rectangle"}, {"x0", 0.63}, {"x1", 0.82},
                         {"y0", 0.52}, {"y1", 0.86}}},
                {"contact", {{"shape", "rectangle"}, {"x0", 0.66}, {"x1", 0.76},
                              {"y0", 0.70}, {"y1", 0.85}}},
                {"concave", {{"shape", "rectangle"}, {"x0", 0.65}, {"x1", 0.82},
                              {"y0", 0.53}, {"y1", 0.71}}},
                {"control", {{"shape", "rectangle"}, {"x0", 0.73}, {"x1", 0.94},
                              {"y0", 0.16}, {"y1", 0.34}}}
            }},
            {"thresholds", {
                {"globalMeanLinearMin", 0.02}, {"globalMeanLinearMax", 0.90},
                {"globalBlackPixelFractionMax", 0.98}, {"globalMeanDeltaAbsMax", 0.025},
                {"globalMeanRatioDeltaMax", 0.08}, {"aoMeanDeltaMin", -0.20},
                {"aoMeanDeltaMax", -0.001}, {"aoNegativeFractionMin", 0.05},
                {"aoEnabledMeanLinearMin", 0.01}, {"aoBlackPixelFractionMax", 0.75},
                {"controlMeanDeltaAbsMax", 0.01}, {"controlP95AbsDeltaMax", 0.025}
            }}
        }},
        {"colorResponseFixture", {
            {"schemaVersion", "noemancer.color-response-fixture/0.1"},
            {"analysisCoordinateSpace", "output-normalized"},
            {"analysisIntent", "neutral-exposure-ramp-rgb-cmy-separation-hdr-shoulder-compression"},
            {"authoring", {
                {"geometry", "asset.primitive.cube"}, {"material", "pbr-emissive-isolated"},
                {"castsShadows", false}, {"receivesShadows", false},
                {"baseColor", Json::array({0.0, 0.0, 0.0})}, {"metallic", 1.0}, {"roughness", 1.0},
                {"neutralEmissiveIntensity", 1.0}, {"chromaticEmissiveIntensity", 0.25},
                {"hdrEmissiveIntensities", Json::array({0.125, 0.25, 0.50, 1.0, 2.0, 4.0})}
            }},
            {"input", {
                {"neutralRampLinearValues", Json::array({0.03, 0.08, 0.18, 0.36, 0.60, 0.90})},
                {"chromaticPatchLabels", Json::array({"red", "green", "blue", "cyan", "magenta", "yellow"})},
                {"chromaticPatchLinearColors", Json::array({
                    Json::array({0.85, 0.03, 0.02}), Json::array({0.03, 0.75, 0.05}),
                    Json::array({0.02, 0.08, 0.85}), Json::array({0.02, 0.75, 0.78}),
                    Json::array({0.78, 0.03, 0.72}), Json::array({0.84, 0.76, 0.03})})},
                {"hdrNeutralBaseColor", Json::array({0.45, 0.45, 0.45})}
            }},
            {"worldLayout", {
                {"xStart", 8.2}, {"xStep", 0.42}, {"columnCount", 6},
                {"neutralY", 0.75}, {"chromaticY", 1.35}, {"hdrY", 1.95}, {"z", -0.7},
                {"scale", Json::array({0.30, 0.18, 0.10})}
            }},
            {"sourceIds", {
                {"neutralRamp", Json::array({
                    "entity.reference.color-neutral-0", "entity.reference.color-neutral-1",
                    "entity.reference.color-neutral-2", "entity.reference.color-neutral-3",
                    "entity.reference.color-neutral-4", "entity.reference.color-neutral-5"})},
                {"chromaticPatches", Json::array({
                    "entity.reference.color-chromatic-0", "entity.reference.color-chromatic-1",
                    "entity.reference.color-chromatic-2", "entity.reference.color-chromatic-3",
                    "entity.reference.color-chromatic-4", "entity.reference.color-chromatic-5"})},
                {"hdrRollOff", Json::array({
                    "entity.reference.color-hdr-0", "entity.reference.color-hdr-1",
                    "entity.reference.color-hdr-2", "entity.reference.color-hdr-3",
                    "entity.reference.color-hdr-4", "entity.reference.color-hdr-5"})}
            }},
            {"expectedOrder", {
                {"neutralRamp", "increasing-linear-luma-left-to-right"},
                {"chromaticPatches", Json::array({"red", "green", "blue", "cyan", "magenta", "yellow"})},
                {"hdrRollOff", "increasing-emissive-input-left-to-right-with-late-step-compression"}
            }},
            {"rois", {
                {"neutralRamp", {{"shape", "rectangle"}, {"x0", 0.83}, {"x1", 0.93},
                                  {"y0", 0.712}, {"y1", 0.727}, {"columnCount", 6},
                                  {"columnHalfWidth", 0.004},
                                  {"columnCenters", Json::array({0.837, 0.855, 0.872, 0.889, 0.907, 0.924})}}},
                {"chromaticPatches", {{"shape", "rectangle"}, {"x0", 0.83}, {"x1", 0.93},
                                        {"y0", 0.669}, {"y1", 0.684}, {"columnCount", 6},
                                        {"columnHalfWidth", 0.004},
                                        {"columnCenters", Json::array({0.837, 0.855, 0.872, 0.889, 0.907, 0.924})}}},
                {"hdrRollOff", {{"shape", "rectangle"}, {"x0", 0.83}, {"x1", 0.93},
                                  {"y0", 0.626}, {"y1", 0.641}, {"columnCount", 6},
                                  {"columnHalfWidth", 0.004},
                                  {"columnCenters", Json::array({0.837, 0.855, 0.872, 0.889, 0.907, 0.924})}}}
            }},
            {"thresholds", {
                {"neutralAdjacentLumaMin", 0.002}, {"neutralChannelSpreadMax", 0.05},
                {"neutralRelativeBiasMax", 0.30},
                {"chromaticDominantChannelMin", 0.20}, {"chromaticDominantAdvantageMin", 0.035},
                {"hdrAdjacentLumaMin", 0.001}, {"hdrLateStepRatioMax", 0.95},
                {"hdrClippedFractionMax", 0.05}
            }}
        }},
        {"quality", {{"meanLumaMin",0.05},{"meanLumaMax",0.72},{"darkPixelFractionMax",0.55},
                     {"brightPixelFractionMax",0.08},{"cpuFrameP95MillisecondsMax",16.67}}},
        {"exercises", Json::array({"material.metallic-roughness","material.emissive","material.base-color-texture",
            "material.normal-texture","material.metallic-roughness-texture","material.occlusion-texture","material.emissive-texture",
            "material.alpha-mask","material.alpha-blend","material.double-sided","lighting.directional","lighting.local-clustered",
            "lighting.point-photometric","lighting.spot-cone","lighting.local-shadow-point","lighting.local-shadow-spot",
            "shadow.cascaded","shadow.directional-cache","environment.ibl","ambient-occlusion","temporal-aa","bloom.multi-scale-dual-filter","auto-exposure",
            "tone-map.aces","depth-range"})},
        {"knownCoverageGaps", Json::array({"render.gpu-driven-submission","texture.demand-and-eviction"})}
    }.dump();
}

} // namespace noemancer
