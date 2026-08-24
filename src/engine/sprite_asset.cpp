#include "engine/sprite_asset.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <utility>
#include <unordered_set>

namespace noemancer {
namespace {
using Json=nlohmann::json;

bool limits_are_valid(const SpriteAssetValidationLimits& limits) noexcept {
    return limits.max_source_bytes > 0U && limits.max_frames > 0U && limits.max_clips > 0U &&
        limits.max_frames_per_clip > 0U && limits.max_total_clip_frame_references > 0U;
}

void error(std::vector<SpriteAssetError>& errors,std::string code,std::string path,std::string message);

bool valid_sha256_identity(const std::string_view value) noexcept {
    constexpr std::string_view prefix = "sha256:";
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 64U) return false;
    return std::ranges::all_of(value.substr(prefix.size()), [](const char character) {
        return std::isxdigit(static_cast<unsigned char>(character)) != 0;
    });
}

void invalid_limits(std::vector<SpriteAssetError>& errors) {
    error(errors, "sprite.invalid-validation-limits", "/limits",
          "Sprite validation limits must all be positive.");
}

void error(std::vector<SpriteAssetError>& errors,std::string code,std::string path,std::string message) {
    errors.push_back({std::move(code),std::move(path),std::move(message)});
}

bool fields(const Json& value,std::initializer_list<std::string_view> allowed,const std::string& path,
            std::vector<SpriteAssetError>& errors) {
    if(!value.is_object()) { error(errors,"sprite.invalid-object",path,"Expected an object."); return false; }
    bool valid=true;
    for(const auto& [name,unused]:value.items()) {
        static_cast<void>(unused);
        bool known=false; for(const auto candidate:allowed) if(name==candidate) { known=true; break; }
        if(!known) { error(errors,"sprite.unknown-field",path+"/"+name,"Unknown field."); valid=false; }
    }
    return valid;
}

bool text_field(const Json& value,const char* name,const std::string& path,std::string& output,
                std::vector<SpriteAssetError>& errors,bool required=true) {
    if(!value.contains(name)) {
        if(required) error(errors,"sprite.missing-field",path+"/"+name,"Required string field is missing.");
        return !required;
    }
    if(!value.at(name).is_string()) { error(errors,"sprite.invalid-string",path+"/"+name,"Expected a string."); return false; }
    output=value.at(name).get<std::string>();
    if(required&&output.empty()) { error(errors,"sprite.empty-string",path+"/"+name,"Value cannot be empty."); return false; }
    return true;
}

bool u32_field(const Json& value,const char* name,const std::string& path,std::uint32_t& output,
               std::vector<SpriteAssetError>& errors,bool positive=false) {
    if(!value.contains(name)||!value.at(name).is_number_unsigned()) {
        error(errors,"sprite.invalid-integer",path+"/"+name,"Expected an unsigned integer."); return false;
    }
    const auto number=value.at(name).get<std::uint64_t>();
    if(number>std::numeric_limits<std::uint32_t>::max()||(positive&&number==0)) {
        error(errors,"sprite.integer-range",path+"/"+name,positive?"Value must be in [1, 2^32-1].":"Value exceeds 32-bit range."); return false;
    }
    output=static_cast<std::uint32_t>(number); return true;
}

bool add_area_checked(const std::uint64_t left, const std::uint64_t right, std::uint64_t& output) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
    output = left + right;
    return true;
}

struct AtlasEvent final {
    std::uint64_t x{};
    std::uint64_t y0{};
    std::uint64_t y1{};
    int delta{};
};

std::uint64_t union_area(const SpriteAssetDocument& document) {
    std::vector<AtlasEvent> events;
    std::vector<std::uint64_t> coordinates;
    events.reserve(document.frames.size() * 2U);
    coordinates.reserve(document.frames.size() * 2U);
    for (const auto& frame : document.frames) {
        const auto x0 = static_cast<std::uint64_t>(frame.x);
        const auto y0 = static_cast<std::uint64_t>(frame.y);
        const auto x1 = x0 + static_cast<std::uint64_t>(frame.width);
        const auto y1 = y0 + static_cast<std::uint64_t>(frame.height);
        events.push_back({x0, y0, y1, 1});
        events.push_back({x1, y0, y1, -1});
        coordinates.push_back(y0);
        coordinates.push_back(y1);
    }
    if (events.empty()) return 0U;
    std::ranges::sort(coordinates);
    coordinates.erase(std::unique(coordinates.begin(), coordinates.end()), coordinates.end());
    std::ranges::sort(events, [](const auto& left, const auto& right) {
        if (left.x != right.x) return left.x < right.x;
        return left.delta > right.delta;
    });

    const auto interval_count = coordinates.size() - 1U;
    if (interval_count == 0U) return 0U;
    std::vector<std::int32_t> cover(interval_count * 4U + 4U);
    std::vector<std::uint64_t> covered(interval_count * 4U + 4U);
    const auto pull = [&](const std::size_t node, const std::size_t left,
                          const std::size_t right) -> void {
        if (cover[node] > 0) {
            covered[node] = coordinates[right + 1U] - coordinates[left];
        } else if (left == right) {
            covered[node] = 0U;
        } else {
            covered[node] = covered[node * 2U] + covered[node * 2U + 1U];
        }
    };
    const auto update = [&](const auto& self, const std::size_t node, const std::size_t left,
                            const std::size_t right, const std::size_t query_left,
                            const std::size_t query_right, const int delta) -> void {
        if (query_left > right || query_right < left || query_left > query_right) return;
        if (query_left <= left && right <= query_right) {
            cover[node] += delta;
            pull(node, left, right);
            return;
        }
        const auto middle = (left + right) / 2U;
        self(self, node * 2U, left, middle, query_left, query_right, delta);
        self(self, node * 2U + 1U, middle + 1U, right, query_left, query_right, delta);
        pull(node, left, right);
    };

    std::uint64_t area{};
    std::uint64_t previous_x = events.front().x;
    std::size_t event_index{};
    while (event_index < events.size()) {
        const auto x = events[event_index].x;
        const auto delta_x = x - previous_x;
        // A valid rectangle set is bounded by textureSize, therefore the
        // union area cannot exceed the representable atlas area.
        area += covered[1U] * delta_x;
        while (event_index < events.size() && events[event_index].x == x) {
            const auto& event = events[event_index];
            const auto first = static_cast<std::size_t>(std::ranges::lower_bound(coordinates, event.y0) - coordinates.begin());
            const auto last_exclusive = static_cast<std::size_t>(std::ranges::lower_bound(coordinates, event.y1) - coordinates.begin());
            if (first < last_exclusive)
                update(update, 1U, 0U, interval_count - 1U, first, last_exclusive - 1U, event.delta);
            ++event_index;
        }
        previous_x = x;
    }
    return area;
}

std::uint64_t layout_hash(const SpriteAssetDocument& document) {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const auto feed = [&](std::uint64_t value, const std::size_t byte_count) {
        for (std::size_t index = 0U; index < byte_count; ++index) {
            hash ^= static_cast<unsigned char>(value & 0xFFU);
            hash *= prime;
            value >>= 8U;
        }
    };
    const auto feed_string = [&](const std::string& value) {
        for (const auto byte : value) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= prime;
        }
        hash ^= 0xFFU;
        hash *= prime;
    };
    feed(document.texture_width, sizeof(document.texture_width));
    feed(document.texture_height, sizeof(document.texture_height));
    for (const auto& frame : document.frames) {
        feed_string(frame.id);
        feed(frame.x, sizeof(frame.x)); feed(frame.y, sizeof(frame.y));
        feed(frame.width, sizeof(frame.width)); feed(frame.height, sizeof(frame.height));
        feed(frame.trim_x, sizeof(frame.trim_x)); feed(frame.trim_y, sizeof(frame.trim_y));
        feed(frame.source_width, sizeof(frame.source_width)); feed(frame.source_height, sizeof(frame.source_height));
    }
    for (const auto& clip : document.clips) {
        feed_string(clip.id);
        feed(clip.looping ? 1U : 0U, 1U);
        for (const auto& clip_frame : clip.frames) {
            feed_string(clip_frame.frame_id);
            feed(clip_frame.duration_ms, sizeof(clip_frame.duration_ms));
        }
    }
    return hash;
}

bool planning_limits_are_valid(const SpriteAtlasPlanningLimits& limits) noexcept {
    return limits.max_frames > 0U && limits.max_pages > 0U && limits.max_page_width > 0U &&
        limits.max_page_height > 0U && limits.max_estimated_cook_bytes > 0U &&
        limits.estimated_bytes_per_pixel > 0U;
}

bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                      std::uint64_t& output) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) return false;
    output = left * right;
    return true;
}

constexpr std::uint64_t atlas_hash_offset = 14695981039346656037ULL;
constexpr std::uint64_t atlas_hash_prime = 1099511628211ULL;

void hash_bytes(std::uint64_t& hash, std::uint64_t value, const std::size_t byte_count) noexcept {
    for (std::size_t index = 0U; index < byte_count; ++index) {
        hash ^= static_cast<unsigned char>(value & 0xFFU);
        hash *= atlas_hash_prime;
        value >>= 8U;
    }
}

void hash_text(std::uint64_t& hash, const std::string_view value) noexcept {
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= atlas_hash_prime;
    }
    hash ^= 0xFFU;
    hash *= atlas_hash_prime;
}

} // namespace

SpriteAssetParseResult SpriteAssetCodec::parse_json(const std::string_view source) {
    return parse_json(source, SpriteAssetValidationLimits{});
}

SpriteAssetParseResult SpriteAssetCodec::parse_json(
    const std::string_view source, const SpriteAssetValidationLimits& limits) {
    SpriteAssetParseResult result;
    if (!limits_are_valid(limits)) {
        invalid_limits(result.errors);
        return result;
    }
    if (source.size() > limits.max_source_bytes) {
        error(result.errors, "sprite.source-too-large", "/",
              "Sprite asset JSON exceeds the bounded source-size limit.");
        return result;
    }
    const auto input=Json::parse(source,nullptr,false);
    if(input.is_discarded()||!input.is_object()) {
        error(result.errors,"sprite.invalid-json","/","Sprite asset must be a JSON object."); return result;
    }
    fields(input,{"schema","assetId","textureAsset","textureSize","pixelsPerUnit","sampling","alphaMode","material","frames","clips","provenance"},"",result.errors);
    const auto frames_input = input.find("frames");
    if (frames_input != input.end() && frames_input->is_array() && frames_input->size() > limits.max_frames) {
        error(result.errors, "sprite.frame-count-limit", "/frames",
              "Frame count exceeds the bounded SpriteAssetValidationLimits.max_frames limit.");
        return result;
    }
    const auto clips_input = input.find("clips");
    if (clips_input != input.end() && clips_input->is_array()) {
        if (clips_input->size() > limits.max_clips) {
            error(result.errors, "sprite.clip-count-limit", "/clips",
                  "Clip count exceeds the bounded SpriteAssetValidationLimits.max_clips limit.");
            return result;
        }
        std::size_t total_clip_frames{};
        for (std::size_t index = 0U; index < clips_input->size(); ++index) {
            const auto& clip = clips_input->at(index);
            if (!clip.is_object()) continue;
            const auto clip_frames = clip.find("frames");
            if (clip_frames == clip.end() || !clip_frames->is_array()) continue;
            if (clip_frames->size() > limits.max_frames_per_clip) {
                error(result.errors, "sprite.clip-frame-count-limit",
                      "/clips/" + std::to_string(index) + "/frames",
                      "Clip frame count exceeds the bounded per-clip limit.");
                return result;
            }
            if (clip_frames->size() > limits.max_total_clip_frame_references -
                                      std::min(total_clip_frames, limits.max_total_clip_frame_references)) {
                error(result.errors, "sprite.total-clip-frame-count-limit", "/clips",
                      "Total clip frame references exceed the bounded production limit.");
                return result;
            }
            total_clip_frames += clip_frames->size();
        }
    }
    SpriteAssetDocument document;
    text_field(input,"schema","",document.schema,result.errors);
    text_field(input,"assetId","",document.asset_id,result.errors);
    text_field(input,"textureAsset","",document.texture_asset,result.errors);
    if(input.contains("textureSize")&&input.at("textureSize").is_array()&&input.at("textureSize").size()==2) {
        const auto& size=input.at("textureSize");
        if(size[0].is_number_unsigned()&&size[1].is_number_unsigned()) {
            const auto width=size[0].get<std::uint64_t>(),height=size[1].get<std::uint64_t>();
            if(width>0&&height>0&&width<=std::numeric_limits<std::uint32_t>::max()&&height<=std::numeric_limits<std::uint32_t>::max()) {
                document.texture_width=static_cast<std::uint32_t>(width);document.texture_height=static_cast<std::uint32_t>(height);
            } else error(result.errors,"sprite.invalid-texture-size","/textureSize","Texture dimensions must be positive 32-bit integers.");
        } else error(result.errors,"sprite.invalid-texture-size","/textureSize","Texture size must contain unsigned integers.");
    } else error(result.errors,"sprite.invalid-texture-size","/textureSize","Texture size must be [width,height].");
    if(input.contains("pixelsPerUnit")&&input.at("pixelsPerUnit").is_number()) document.pixels_per_unit=input.at("pixelsPerUnit").get<float>();
    else error(result.errors,"sprite.invalid-pixels-per-unit","/pixelsPerUnit","pixelsPerUnit must be a number.");
    text_field(input,"sampling","",document.sampling,result.errors);
    text_field(input,"alphaMode","",document.alpha_mode,result.errors);
    if(input.contains("material")) {
        const auto& value=input.at("material");
        if(fields(value,{"normalTextureAsset","emissiveMaskTextureAsset","depthTextureAsset","normalStrength",
                         "emissiveColor","emissiveIntensity","depthBias","shadingModel","metallic","roughness",
                         "receivesShadows","castsShadows"},"/material",result.errors)) {
            SpriteMaterialChannels material;
            text_field(value,"normalTextureAsset","/material",material.normal_texture_asset,result.errors,false);
            text_field(value,"emissiveMaskTextureAsset","/material",material.emissive_mask_texture_asset,result.errors,false);
            text_field(value,"depthTextureAsset","/material",material.depth_texture_asset,result.errors,false);
            const auto number=[&](const char* name,float& target,const float fallback) {
                target=fallback;
                if(value.contains(name)) {
                    if(value.at(name).is_number())target=value.at(name).get<float>();
                    else error(result.errors,"sprite.invalid-number",std::string("/material/")+name,"Expected a number.");
                }
            };
            number("normalStrength",material.normal_strength,1.0F);
            number("emissiveIntensity",material.emissive_intensity,0.0F);
            number("depthBias",material.depth_bias,0.0F);
            text_field(value,"shadingModel","/material",material.shading_model,result.errors,false);
            number("metallic",material.metallic,0.0F);
            number("roughness",material.roughness,0.8F);
            const auto boolean=[&](const char* name,bool& target,const bool fallback) {
                target=fallback;
                if(value.contains(name)) {
                    if(value.at(name).is_boolean())target=value.at(name).get<bool>();
                    else error(result.errors,"sprite.invalid-boolean",std::string("/material/")+name,"Expected a boolean.");
                }
            };
            boolean("receivesShadows",material.receives_shadows,true);
            boolean("castsShadows",material.casts_shadows,true);
            if(value.contains("emissiveColor")&&value.at("emissiveColor").is_array()&&value.at("emissiveColor").size()==3&&
               std::ranges::all_of(value.at("emissiveColor"),[](const auto& channel){return channel.is_number();})) {
                material.emissive_r=value.at("emissiveColor")[0].get<float>();material.emissive_g=value.at("emissiveColor")[1].get<float>();
                material.emissive_b=value.at("emissiveColor")[2].get<float>();
            } else if(value.contains("emissiveColor")) error(result.errors,"sprite.invalid-color","/material/emissiveColor","Expected three numeric channels.");
            document.material=std::move(material);
        }
    }

    if(!input.contains("frames")||!input.at("frames").is_array()) error(result.errors,"sprite.invalid-frames","/frames","frames must be an array.");
    else for(std::size_t index=0;index<input.at("frames").size();++index) {
        const auto path="/frames/"+std::to_string(index);const auto& value=input.at("frames").at(index);
        if(!fields(value,{"id","rect","trimOffset","sourceSize","pivot","collisionProfile"},path,result.errors)) continue;
        SpriteFrame frame;text_field(value,"id",path,frame.id,result.errors);
        auto pair=[&](const char* name,std::uint32_t& first,std::uint32_t& second,bool positive) {
            if(!value.contains(name)||!value.at(name).is_array()||value.at(name).size()!=2||
               !value.at(name)[0].is_number_unsigned()||!value.at(name)[1].is_number_unsigned()) {
                error(result.errors,"sprite.invalid-pair",path+"/"+name,"Expected two unsigned integers.");return;
            }
            const auto a=value.at(name)[0].get<std::uint64_t>(),b=value.at(name)[1].get<std::uint64_t>();
            if(a>std::numeric_limits<std::uint32_t>::max()||b>std::numeric_limits<std::uint32_t>::max()||(positive&&(a==0||b==0))) {
                error(result.errors,"sprite.integer-range",path+"/"+name,"Values are outside the supported range.");return;
            }
            first=static_cast<std::uint32_t>(a);second=static_cast<std::uint32_t>(b);
        };
        if(value.contains("rect")&&value.at("rect").is_array()&&value.at("rect").size()==4&&
           value.at("rect")[0].is_number_unsigned()&&value.at("rect")[1].is_number_unsigned()&&
           value.at("rect")[2].is_number_unsigned()&&value.at("rect")[3].is_number_unsigned()) {
            const auto& rect=value.at("rect");
            const auto x=rect[0].get<std::uint64_t>(),y=rect[1].get<std::uint64_t>(),w=rect[2].get<std::uint64_t>(),h=rect[3].get<std::uint64_t>();
            if(x<=std::numeric_limits<std::uint32_t>::max()&&y<=std::numeric_limits<std::uint32_t>::max()&&w>0&&h>0&&
               w<=std::numeric_limits<std::uint32_t>::max()&&h<=std::numeric_limits<std::uint32_t>::max()) {
                frame.x=static_cast<std::uint32_t>(x);frame.y=static_cast<std::uint32_t>(y);
                frame.width=static_cast<std::uint32_t>(w);frame.height=static_cast<std::uint32_t>(h);
            } else error(result.errors,"sprite.invalid-rect",path+"/rect","Frame rect must fit positive 32-bit dimensions.");
        } else error(result.errors,"sprite.invalid-rect",path+"/rect","rect must be [x,y,width,height].");
        pair("trimOffset",frame.trim_x,frame.trim_y,false);pair("sourceSize",frame.source_width,frame.source_height,true);
        if(value.contains("pivot")&&value.at("pivot").is_array()&&value.at("pivot").size()==2&&
           value.at("pivot")[0].is_number()&&value.at("pivot")[1].is_number()) {
            frame.pivot_x=value.at("pivot")[0].get<float>();frame.pivot_y=value.at("pivot")[1].get<float>();
        } else error(result.errors,"sprite.invalid-pivot",path+"/pivot","pivot must be [x,y].");
        text_field(value,"collisionProfile",path,frame.collision_profile,result.errors,false);
        document.frames.push_back(std::move(frame));
    }

    if(!input.contains("clips")||!input.at("clips").is_array()) error(result.errors,"sprite.invalid-clips","/clips","clips must be an array.");
    else for(std::size_t clip_index=0;clip_index<input.at("clips").size();++clip_index) {
        const auto path="/clips/"+std::to_string(clip_index);const auto& value=input.at("clips").at(clip_index);
        if(!fields(value,{"id","looping","frames"},path,result.errors)) continue;
        SpriteClip clip;text_field(value,"id",path,clip.id,result.errors);
        if(value.contains("looping")&&value.at("looping").is_boolean())clip.looping=value.at("looping").get<bool>();
        else error(result.errors,"sprite.invalid-looping",path+"/looping","looping must be a boolean.");
        if(!value.contains("frames")||!value.at("frames").is_array()) error(result.errors,"sprite.invalid-clip-frames",path+"/frames","Clip frames must be an array.");
        else for(std::size_t frame_index=0;frame_index<value.at("frames").size();++frame_index) {
            const auto frame_path=path+"/frames/"+std::to_string(frame_index);const auto& item=value.at("frames").at(frame_index);
            if(!fields(item,{"frame","durationMs","event"},frame_path,result.errors))continue;
            SpriteClipFrame frame;text_field(item,"frame",frame_path,frame.frame_id,result.errors);
            u32_field(item,"durationMs",frame_path,frame.duration_ms,result.errors,true);
            text_field(item,"event",frame_path,frame.event,result.errors,false);clip.frames.push_back(std::move(frame));
        }
        document.clips.push_back(std::move(clip));
    }
    if(!input.contains("provenance")||!input.at("provenance").is_object()) error(result.errors,"sprite.invalid-provenance","/provenance","provenance must be an object.");
    else {
        const auto& value=input.at("provenance");fields(value,{"sourceUri","sourceSha256","generator","license"},"/provenance",result.errors);
        text_field(value,"sourceUri","/provenance",document.provenance.source_uri,result.errors);
        text_field(value,"sourceSha256","/provenance",document.provenance.source_sha256,result.errors);
        text_field(value,"generator","/provenance",document.provenance.generator,result.errors);
        text_field(value,"license","/provenance",document.provenance.license,result.errors);
    }
    auto semantic=validate(document, limits);result.errors.insert(result.errors.end(),semantic.begin(),semantic.end());
    if(result.errors.empty())result.document=std::move(document);return result;
}

std::vector<SpriteAssetError> SpriteAssetCodec::validate(const SpriteAssetDocument& document) {
    return validate(document, SpriteAssetValidationLimits{});
}

std::vector<SpriteAssetError> SpriteAssetCodec::validate(
    const SpriteAssetDocument& document, const SpriteAssetValidationLimits& limits) {
    std::vector<SpriteAssetError> errors;
    if (!limits_are_valid(limits)) {
        invalid_limits(errors);
        return errors;
    }
    bool count_limit_exceeded = false;
    if (document.frames.size() > limits.max_frames) {
        error(errors, "sprite.frame-count-limit", "/frames",
              "Frame count exceeds the bounded SpriteAssetValidationLimits.max_frames limit.");
        return errors;
    }
    if (document.clips.size() > limits.max_clips) {
        error(errors, "sprite.clip-count-limit", "/clips",
              "Clip count exceeds the bounded SpriteAssetValidationLimits.max_clips limit.");
        return errors;
    }
    std::size_t total_clip_frame_references{};
    for (std::size_t index = 0U; index < document.clips.size(); ++index) {
        const auto& clip = document.clips[index];
        if (clip.frames.size() > limits.max_frames_per_clip) {
            error(errors, "sprite.clip-frame-count-limit", "/clips/" + std::to_string(index) + "/frames",
                  "Clip frame count exceeds the bounded per-clip limit.");
            count_limit_exceeded = true;
        }
        if (clip.frames.size() > limits.max_total_clip_frame_references -
                                  std::min(total_clip_frame_references, limits.max_total_clip_frame_references)) {
            error(errors, "sprite.total-clip-frame-count-limit", "/clips",
                  "Total clip frame references exceed the bounded production limit.");
            count_limit_exceeded = true;
            total_clip_frame_references = limits.max_total_clip_frame_references;
        } else {
            total_clip_frame_references += clip.frames.size();
        }
    }
    if (count_limit_exceeded) return errors;
    if(document.schema!="noemancer.sprite-asset/0.1"&&document.schema!="noemancer.sprite-asset/0.2")
        error(errors,"sprite.unsupported-schema","/schema","Expected noemancer.sprite-asset/0.1 or noemancer.sprite-asset/0.2.");
    if(document.schema=="noemancer.sprite-asset/0.1"&&document.material)
        error(errors,"sprite.material-requires-v0.2","/material","Material channels require noemancer.sprite-asset/0.2.");
    if(document.asset_id.empty())error(errors,"sprite.empty-asset-id","/assetId","Asset ID cannot be empty.");
    if(document.texture_asset.empty())error(errors,"sprite.empty-texture-asset","/textureAsset","Texture asset cannot be empty.");
    if(document.texture_width==0||document.texture_height==0)error(errors,"sprite.invalid-texture-size","/textureSize","Texture dimensions must be positive.");
    if(!std::isfinite(document.pixels_per_unit)||document.pixels_per_unit<=0.0F)error(errors,"sprite.invalid-pixels-per-unit","/pixelsPerUnit","pixelsPerUnit must be finite and positive.");
    if(document.sampling!="nearest"&&document.sampling!="linear")error(errors,"sprite.invalid-sampling","/sampling","sampling must be nearest or linear.");
    if(document.alpha_mode!="cutout"&&document.alpha_mode!="blend")error(errors,"sprite.invalid-alpha-mode","/alphaMode","alphaMode must be cutout or blend.");
    if(document.material) {
        const auto& material=*document.material;
        if(!std::isfinite(material.normal_strength)||material.normal_strength<0.0F||material.normal_strength>4.0F)
            error(errors,"sprite.invalid-normal-strength","/material/normalStrength","normalStrength must be finite in [0,4].");
        if(!std::isfinite(material.emissive_r)||!std::isfinite(material.emissive_g)||!std::isfinite(material.emissive_b)||
           material.emissive_r<0.0F||material.emissive_g<0.0F||material.emissive_b<0.0F||
           material.emissive_r>100.0F||material.emissive_g>100.0F||material.emissive_b>100.0F)
            error(errors,"sprite.invalid-emissive-color","/material/emissiveColor","Emissive channels must be finite in [0,100].");
        if(!std::isfinite(material.emissive_intensity)||material.emissive_intensity<0.0F||material.emissive_intensity>100.0F)
            error(errors,"sprite.invalid-emissive-intensity","/material/emissiveIntensity","emissiveIntensity must be finite in [0,100].");
        if(!std::isfinite(material.depth_bias)||material.depth_bias<0.0F||material.depth_bias>0.01F)
            error(errors,"sprite.invalid-depth-bias","/material/depthBias","depthBias must be finite in [0,0.01] device-depth units.");
        if(material.shading_model!="lit"&&material.shading_model!="unlit")
            error(errors,"sprite.invalid-shading-model","/material/shadingModel","shadingModel must be lit or unlit.");
        if(!std::isfinite(material.metallic)||material.metallic<0.0F||material.metallic>1.0F)
            error(errors,"sprite.invalid-metallic","/material/metallic","metallic must be finite in [0,1].");
        if(!std::isfinite(material.roughness)||material.roughness<0.045F||material.roughness>1.0F)
            error(errors,"sprite.invalid-roughness","/material/roughness","roughness must be finite in [0.045,1].");
    }
    if(document.frames.empty())error(errors,"sprite.empty-frames","/frames","At least one frame is required.");
    std::unordered_set<std::string> frame_ids;
    for(std::size_t index=0;index<document.frames.size();++index) {
        const auto& frame=document.frames[index];const auto path="/frames/"+std::to_string(index);
        if(frame.id.empty()||!frame_ids.insert(frame.id).second)error(errors,"sprite.duplicate-frame-id",path+"/id","Frame IDs must be non-empty and unique.");
        if(frame.width==0||frame.height==0||static_cast<std::uint64_t>(frame.x)+frame.width>document.texture_width||
           static_cast<std::uint64_t>(frame.y)+frame.height>document.texture_height)
            error(errors,"sprite.frame-out-of-bounds",path+"/rect","Frame rect must stay inside textureSize.");
        if(frame.source_width<frame.width||frame.source_height<frame.height||
           static_cast<std::uint64_t>(frame.trim_x)+frame.width>frame.source_width||
           static_cast<std::uint64_t>(frame.trim_y)+frame.height>frame.source_height)
            error(errors,"sprite.invalid-trim",path,"Trimmed pixels must fit sourceSize.");
        if(!std::isfinite(frame.pivot_x)||!std::isfinite(frame.pivot_y)||frame.pivot_x<0.0F||frame.pivot_x>1.0F||frame.pivot_y<0.0F||frame.pivot_y>1.0F)
            error(errors,"sprite.invalid-pivot",path+"/pivot","Pivot coordinates must be finite normalized values.");
    }
    std::unordered_set<std::string> clip_ids;
    for(std::size_t index=0;index<document.clips.size();++index) {
        const auto& clip=document.clips[index];const auto path="/clips/"+std::to_string(index);
        if(clip.id.empty()||!clip_ids.insert(clip.id).second)error(errors,"sprite.duplicate-clip-id",path+"/id","Clip IDs must be non-empty and unique.");
        if(clip.frames.empty())error(errors,"sprite.empty-clip",path+"/frames","A clip must contain at least one frame.");
        for(std::size_t frame_index=0;frame_index<clip.frames.size();++frame_index) {
            const auto& frame=clip.frames[frame_index];const auto frame_path=path+"/frames/"+std::to_string(frame_index);
            if(!frame_ids.contains(frame.frame_id))error(errors,"sprite.unknown-frame",frame_path+"/frame","Clip references an unknown frame ID.");
            if(frame.duration_ms==0||frame.duration_ms>60000)error(errors,"sprite.invalid-duration",frame_path+"/durationMs","durationMs must be in [1,60000].");
        }
    }
    return errors;
}

std::string SpriteAssetCodec::write_canonical_json(const SpriteAssetDocument& document) {
    Json out={{"schema",document.schema},{"assetId",document.asset_id},{"textureAsset",document.texture_asset},
        {"textureSize",Json::array({document.texture_width,document.texture_height})},{"pixelsPerUnit",document.pixels_per_unit},
        {"sampling",document.sampling},{"alphaMode",document.alpha_mode},{"frames",Json::array()},{"clips",Json::array()},
        {"provenance",{{"sourceUri",document.provenance.source_uri},{"sourceSha256",document.provenance.source_sha256},
            {"generator",document.provenance.generator},{"license",document.provenance.license}}}};
    if(document.material) {
        const auto& material=*document.material;
        out["material"]={{"normalTextureAsset",material.normal_texture_asset},
            {"emissiveMaskTextureAsset",material.emissive_mask_texture_asset},{"depthTextureAsset",material.depth_texture_asset},
            {"normalStrength",material.normal_strength},{"emissiveColor",Json::array({material.emissive_r,material.emissive_g,material.emissive_b})},
            {"emissiveIntensity",material.emissive_intensity},{"depthBias",material.depth_bias},
            {"shadingModel",material.shading_model},{"metallic",material.metallic},{"roughness",material.roughness},
            {"receivesShadows",material.receives_shadows},{"castsShadows",material.casts_shadows}};
    }
    for(const auto& frame:document.frames)out["frames"].push_back({{"id",frame.id},{"rect",Json::array({frame.x,frame.y,frame.width,frame.height})},
        {"trimOffset",Json::array({frame.trim_x,frame.trim_y})},{"sourceSize",Json::array({frame.source_width,frame.source_height})},
        {"pivot",Json::array({frame.pivot_x,frame.pivot_y})},{"collisionProfile",frame.collision_profile}});
    for(const auto& clip:document.clips) {
        Json value={{"id",clip.id},{"looping",clip.looping},{"frames",Json::array()}};
        for(const auto& frame:clip.frames)value["frames"].push_back({{"frame",frame.frame_id},{"durationMs",frame.duration_ms},{"event",frame.event}});
        out["clips"].push_back(std::move(value));
    }
    return out.dump(2)+"\n";
}

std::vector<std::string> SpriteAssetCodec::asset_dependencies(const SpriteAssetDocument& document) {
    std::vector<std::string> result;
    result.reserve(4);
    const auto append=[&](const std::string& asset_id) {
        if(!asset_id.empty())result.push_back(asset_id);
    };
    append(document.texture_asset);
    if(document.material) {
        append(document.material->normal_texture_asset);
        append(document.material->emissive_mask_texture_asset);
        append(document.material->depth_texture_asset);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}

SpriteAssetProductionReport SpriteAssetCodec::production_report(const SpriteAssetDocument& document) {
    return production_report(document, SpriteAssetValidationLimits{});
}

SpriteAssetProductionReport SpriteAssetCodec::production_report(
    const SpriteAssetDocument& document, const SpriteAssetValidationLimits& limits) {
    SpriteAssetProductionReport report;
    report.frame_count = document.frames.size();
    report.clip_count = document.clips.size();
    report.atlas_page_count = 1U;
    report.atlas_area = static_cast<std::uint64_t>(document.texture_width) *
        static_cast<std::uint64_t>(document.texture_height);
    report.diagnostics = validate(document, limits);
    if (!report.diagnostics.empty()) {
        report.code = report.diagnostics.front().code;
        return report;
    }

    std::unordered_set<std::string> referenced_frames;
    for (const auto& clip : document.clips) {
        report.max_clip_frame_count = std::max(report.max_clip_frame_count, clip.frames.size());
        report.total_clip_frame_references += clip.frames.size();
        for (const auto& clip_frame : clip.frames) referenced_frames.insert(clip_frame.frame_id);
    }
    report.unique_referenced_frame_count = referenced_frames.size();
    report.unreferenced_frame_count = report.frame_count - report.unique_referenced_frame_count;
    for (const auto& frame : document.frames) {
        const auto area = static_cast<std::uint64_t>(frame.width) *
            static_cast<std::uint64_t>(frame.height);
        if (!add_area_checked(report.frame_area_sum, area, report.frame_area_sum)) {
            report.frame_area_sum = std::numeric_limits<std::uint64_t>::max();
            error(report.diagnostics, "sprite.production-area-overflow", "/frames",
                  "The aggregate frame area exceeds the bounded 64-bit production report.");
            break;
        }
    }
    report.occupied_area = union_area(document);
    report.free_area = report.atlas_area >= report.occupied_area
        ? report.atlas_area - report.occupied_area : 0U;
    report.overlap_area = report.frame_area_sum >= report.occupied_area &&
            report.frame_area_sum != std::numeric_limits<std::uint64_t>::max()
        ? report.frame_area_sum - report.occupied_area : 0U;
    report.layout_fingerprint = layout_hash(document);
    report.valid = report.diagnostics.empty();
    report.code = report.valid ? "ok" : report.diagnostics.front().code;
    return report;
}

SpriteAtlasPlanningReport SpriteAssetCodec::plan_atlas_pages(
    const SpriteAssetDocument& document, const SpriteAtlasPlanningOptions& options) {
    static const std::vector<std::string> no_changed_frames;
    return plan_atlas_pages(document, options, no_changed_frames);
}

SpriteAtlasPlanningReport SpriteAssetCodec::plan_atlas_pages(
    const SpriteAssetDocument& document, const SpriteAtlasPlanningOptions& options,
    const std::vector<std::string>& changed_frame_ids) {
    SpriteAtlasPlanningReport report;
    report.page_width = options.page_width;
    report.page_height = options.page_height;
    report.padding = options.padding;
    report.frame_count = document.frames.size();

    const auto fail = [&](const std::string_view code, const std::string_view path,
                          const std::string_view message) {
        error(report.diagnostics, std::string(code), std::string(path), std::string(message));
        report.code = std::string(code);
        report.valid = false;
    };

    if (!planning_limits_are_valid(options.limits)) {
        fail("sprite.atlas-invalid-planning-limits", "/planning/limits",
             "Atlas planning limits must all be positive.");
        return report;
    }
    if (options.page_width == 0U || options.page_height == 0U) {
        fail("sprite.atlas-invalid-page-size", "/planning/pageSize",
             "Atlas page dimensions must be positive.");
        return report;
    }
    if (options.page_width > options.limits.max_page_width ||
        options.page_height > options.limits.max_page_height) {
        fail("sprite.atlas-page-size-limit", "/planning/pageSize",
             "Atlas page dimensions exceed the bounded planning limit.");
        return report;
    }
    if (static_cast<std::uint64_t>(options.padding) * 2U + 1U > options.page_width ||
        static_cast<std::uint64_t>(options.padding) * 2U + 1U > options.page_height) {
        fail("sprite.atlas-padding-too-large", "/planning/padding",
             "Padding leaves no room for a positive frame on the requested page.");
        return report;
    }
    if (document.frames.size() > options.limits.max_frames) {
        fail("sprite.atlas-frame-limit", "/frames",
             "Frame count exceeds the bounded atlas planning limit.");
        return report;
    }

    const auto page_area_result = [&]() -> std::optional<std::uint64_t> {
        std::uint64_t value{};
        if (!checked_multiply(options.page_width, options.page_height, value)) return std::nullopt;
        return value;
    }();
    if (!page_area_result) {
        fail("sprite.atlas-area-overflow", "/planning/pageSize",
             "Atlas page area cannot be represented by the planning report.");
        return report;
    }
    const auto page_area = *page_area_result;
    std::uint64_t page_bytes{};
    if (!checked_multiply(page_area, options.limits.estimated_bytes_per_pixel, page_bytes)) {
        fail("sprite.atlas-cook-size-overflow", "/planning/estimatedBytesPerPixel",
             "Atlas page byte estimate cannot be represented by the planning report.");
        return report;
    }

    const auto validation_errors = validate(document);
    if (!validation_errors.empty()) {
        report.diagnostics = validation_errors;
        report.code = validation_errors.front().code;
        return report;
    }

    if (!checked_multiply(document.texture_width, document.texture_height,
                          report.single_atlas_cook_pixels) ||
        !checked_multiply(report.single_atlas_cook_pixels,
                          options.limits.estimated_bytes_per_pixel,
                          report.single_atlas_cook_bytes)) {
        fail("sprite.atlas-cook-size-overflow", "/textureSize",
             "Single-atlas byte estimate cannot be represented by the planning report.");
        return report;
    }

    // The planner uses a stable frame-ID order and a deterministic shelf pack.
    // It is intentionally a planning model, not a claim about an encoder or a
    // particular renderer's atlas allocator.
    std::vector<std::size_t> order(document.frames.size());
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::sort(order, [&document](const std::size_t left, const std::size_t right) {
        if (document.frames[left].id != document.frames[right].id)
            return document.frames[left].id < document.frames[right].id;
        return left < right;
    });

    struct PageAccum final {
        std::uint64_t occupied_area{};
        std::vector<std::size_t> placement_indices;
    };
    std::vector<PageAccum> pages;
    pages.emplace_back();
    report.placements.reserve(document.frames.size());

    std::uint32_t cursor_x = options.padding;
    std::uint32_t cursor_y = options.padding;
    std::uint32_t row_height{};
    std::size_t page_index{};
    for (const auto frame_index : order) {
        const auto& frame = document.frames[frame_index];
        const auto padded_width = static_cast<std::uint64_t>(frame.width) +
            static_cast<std::uint64_t>(options.padding) * 2U;
        const auto padded_height = static_cast<std::uint64_t>(frame.height) +
            static_cast<std::uint64_t>(options.padding) * 2U;
        if (padded_width > options.page_width || padded_height > options.page_height) {
            fail("sprite.atlas-frame-too-large", "/frames/" + std::to_string(frame_index) + "/rect",
                 "Frame plus padding does not fit within one atlas page.");
            return report;
        }

        if (static_cast<std::uint64_t>(cursor_x) + frame.width + options.padding > options.page_width) {
            cursor_x = options.padding;
            const auto next_y = static_cast<std::uint64_t>(cursor_y) + row_height + options.padding;
            cursor_y = static_cast<std::uint32_t>(next_y);
            row_height = 0U;
        }
        if (static_cast<std::uint64_t>(cursor_y) + frame.height + options.padding > options.page_height) {
            if (pages.size() >= options.limits.max_pages) {
                fail("sprite.atlas-page-limit", "/planning/maxPages",
                     "Frame placement would exceed the bounded atlas page limit.");
                return report;
            }
            pages.emplace_back();
            ++page_index;
            cursor_x = options.padding;
            cursor_y = options.padding;
            row_height = 0U;
        }

        const auto placement_index = report.placements.size();
        report.placements.push_back(SpriteAtlasFramePlacement{
            .frame_id = frame.id,
            .page_index = static_cast<std::uint32_t>(page_index),
            .x = cursor_x,
            .y = cursor_y,
            .width = frame.width,
            .height = frame.height});
        pages[page_index].placement_indices.push_back(placement_index);
        const auto frame_area = static_cast<std::uint64_t>(frame.width) * frame.height;
        if (!add_area_checked(pages[page_index].occupied_area, frame_area,
                              pages[page_index].occupied_area)) {
            fail("sprite.atlas-area-overflow", "/frames",
                 "Page occupancy cannot be represented by the planning report.");
            return report;
        }
        cursor_x = static_cast<std::uint32_t>(static_cast<std::uint64_t>(cursor_x) +
                                              frame.width + options.padding);
        row_height = std::max(row_height, frame.height);
    }

    report.page_count = pages.size();
    report.pages.reserve(pages.size());
    for (std::size_t index = 0U; index < pages.size(); ++index) {
        const auto& accumulated = pages[index];
        SpriteAtlasPageReport page;
        page.page_index = static_cast<std::uint32_t>(index);
        page.width = options.page_width;
        page.height = options.page_height;
        page.frame_count = accumulated.placement_indices.size();
        page.occupied_area = accumulated.occupied_area;
        page.free_area = page_area >= page.occupied_area ? page_area - page.occupied_area : 0U;
        // The shelf pack places non-overlapping rectangles by construction.
        // Keeping this value explicit makes the no-overlap invariant visible
        // to downstream diagnostics without coupling the plan to a renderer.
        page.overlap_area = 0U;
        page.estimated_cook_pixels = page_area;
        page.estimated_cook_bytes = page_bytes;
        page.layout_fingerprint = atlas_hash_offset;
        hash_bytes(page.layout_fingerprint, page.page_index, sizeof(page.page_index));
        hash_bytes(page.layout_fingerprint, page.width, sizeof(page.width));
        hash_bytes(page.layout_fingerprint, page.height, sizeof(page.height));
        for (const auto placement_index : accumulated.placement_indices) {
            const auto& placement = report.placements[placement_index];
            hash_text(page.layout_fingerprint, placement.frame_id);
            hash_bytes(page.layout_fingerprint, placement.x, sizeof(placement.x));
            hash_bytes(page.layout_fingerprint, placement.y, sizeof(placement.y));
            hash_bytes(page.layout_fingerprint, placement.width, sizeof(placement.width));
            hash_bytes(page.layout_fingerprint, placement.height, sizeof(placement.height));
        }
        report.pages.push_back(page);
    }
    if (!checked_multiply(report.page_count, page_area, report.planned_cook_pixels) ||
        !checked_multiply(report.planned_cook_pixels, options.limits.estimated_bytes_per_pixel,
                          report.planned_cook_bytes)) {
        fail("sprite.atlas-cook-size-overflow", "/planning",
             "Planned atlas byte estimate cannot be represented by the planning report.");
        return report;
    }
    if (report.planned_cook_bytes > options.limits.max_estimated_cook_bytes) {
        fail("sprite.atlas-cook-budget", "/planning/maxEstimatedCookBytes",
             "Planned atlas byte estimate exceeds the bounded cook budget.");
        return report;
    }

    std::vector<std::string> unique_changed = changed_frame_ids;
    std::ranges::sort(unique_changed);
    unique_changed.erase(std::unique(unique_changed.begin(), unique_changed.end()), unique_changed.end());
    report.changed_frame_count = unique_changed.size();
    for (const auto& changed_id : unique_changed) {
        const auto placement = std::ranges::find(report.placements, changed_id,
                                                   &SpriteAtlasFramePlacement::frame_id);
        if (placement == report.placements.end()) {
            fail("sprite.atlas-unknown-frame", "/planning/changedFrameIds",
                 "Changed frame IDs must refer to frames in the current document.");
            return report;
        }
        if (std::ranges::find(report.affected_page_indices, placement->page_index) ==
            report.affected_page_indices.end()) {
            report.affected_page_indices.push_back(placement->page_index);
        }
    }
    if (!checked_multiply(report.affected_page_indices.size(), page_area,
                          report.incremental_cook_pixels) ||
        !checked_multiply(report.incremental_cook_pixels, options.limits.estimated_bytes_per_pixel,
                          report.incremental_cook_bytes)) {
        fail("sprite.atlas-cook-size-overflow", "/planning/changedFrameIds",
             "Incremental atlas byte estimate cannot be represented by the planning report.");
        return report;
    }

    report.layout_fingerprint = atlas_hash_offset;
    hash_bytes(report.layout_fingerprint, options.page_width, sizeof(options.page_width));
    hash_bytes(report.layout_fingerprint, options.page_height, sizeof(options.page_height));
    hash_bytes(report.layout_fingerprint, options.padding, sizeof(options.padding));
    for (const auto& placement : report.placements) {
        hash_text(report.layout_fingerprint, placement.frame_id);
        hash_bytes(report.layout_fingerprint, placement.page_index, sizeof(placement.page_index));
        hash_bytes(report.layout_fingerprint, placement.x, sizeof(placement.x));
        hash_bytes(report.layout_fingerprint, placement.y, sizeof(placement.y));
        hash_bytes(report.layout_fingerprint, placement.width, sizeof(placement.width));
        hash_bytes(report.layout_fingerprint, placement.height, sizeof(placement.height));
    }
    for (const auto& page : report.pages) {
        hash_bytes(report.layout_fingerprint, page.page_index, sizeof(page.page_index));
        hash_bytes(report.layout_fingerprint, page.occupied_area, sizeof(page.occupied_area));
        hash_bytes(report.layout_fingerprint, page.free_area, sizeof(page.free_area));
        hash_bytes(report.layout_fingerprint, page.overlap_area, sizeof(page.overlap_area));
        hash_bytes(report.layout_fingerprint, page.layout_fingerprint, sizeof(page.layout_fingerprint));
    }
    report.valid = true;
    report.code = "ok";
    return report;
}

std::string sprite_atlas_plan_json(
    const SpriteAssetDocument& document, const SpriteAtlasPlanningOptions& options,
    const std::vector<std::string>& changed_frame_ids) {
    const auto report = SpriteAssetCodec::plan_atlas_pages(document, options, changed_frame_ids);

    Json page_items = Json::array();
    const auto page_emit_count = std::min(report.pages.size(), sprite_atlas_plan_max_projected_pages);
    for (std::size_t index = 0U; index < page_emit_count; ++index) {
        const auto& page = report.pages[index];
        page_items.push_back({
            {"index", page.page_index},
            {"size", Json::array({page.width, page.height})},
            {"frameCount", page.frame_count},
            {"occupiedArea", page.occupied_area},
            {"freeArea", page.free_area},
            {"overlapArea", page.overlap_area},
            {"estimatedCookPixels", page.estimated_cook_pixels},
            {"estimatedCookBytes", page.estimated_cook_bytes},
            {"layoutFingerprint", page.layout_fingerprint}});
    }

    Json placement_items = Json::array();
    const auto placement_emit_count = std::min(report.placements.size(),
                                               sprite_atlas_plan_max_projected_placements);
    for (std::size_t index = 0U; index < placement_emit_count; ++index) {
        const auto& placement = report.placements[index];
        placement_items.push_back({
            {"frameId", placement.frame_id},
            {"pageIndex", placement.page_index},
            {"rect", Json::array({placement.x, placement.y, placement.width, placement.height})}});
    }

    Json diagnostics = Json::array();
    for (const auto& issue : report.diagnostics) {
        diagnostics.push_back({
            {"code", issue.code}, {"path", issue.path}, {"message", issue.message}});
    }

    const auto planned_placement_count = report.placements.size();
    const auto placement_truncated = planned_placement_count > placement_emit_count;
    const auto page_truncated = report.pages.size() > page_emit_count;
    const Json output{
        {"schemaVersion", "noemancer.sprite-atlas-plan/0.1"},
        {"valid", report.valid},
        {"code", report.code},
        {"frameCount", report.frame_count},
        {"changedFrameCount", report.changed_frame_count},
        {"options", {
            {"pageSize", Json::array({options.page_width, options.page_height})},
            {"padding", options.padding},
            {"estimatedBytesPerPixel", options.limits.estimated_bytes_per_pixel},
            {"limits", {
                {"maxFrames", options.limits.max_frames},
                {"maxPages", options.limits.max_pages},
                {"maxPageSize", Json::array({options.limits.max_page_width, options.limits.max_page_height})},
                {"maxEstimatedCookBytes", options.limits.max_estimated_cook_bytes}}}}},
        {"pages", {
            {"total", report.page_count},
            {"available", report.pages.size()},
            {"maxItems", sprite_atlas_plan_max_projected_pages},
            {"emitted", page_emit_count},
            {"truncated", page_truncated},
            {"items", std::move(page_items)}}},
        {"placements", {
            {"total", report.frame_count},
            {"planned", planned_placement_count},
            {"maxItems", sprite_atlas_plan_max_projected_placements},
            {"emitted", placement_emit_count},
            {"omitted", report.frame_count > placement_emit_count
                ? report.frame_count - placement_emit_count : 0U},
            {"truncated", placement_truncated || report.frame_count > placement_emit_count},
            {"items", std::move(placement_items)}}},
        {"affectedPages", report.affected_page_indices},
        {"cookEstimate", {
            {"fullPlan", {
                {"pixels", report.planned_cook_pixels},
                {"bytes", report.planned_cook_bytes}}},
            {"incremental", {
                {"affectedPageCount", report.affected_page_indices.size()},
                {"pixels", report.incremental_cook_pixels},
                {"bytes", report.incremental_cook_bytes}}},
            {"singleAtlasBaseline", {
                {"pageCount", 1U},
                {"pixels", report.single_atlas_cook_pixels},
                {"bytes", report.single_atlas_cook_bytes}}}}},
        {"layoutFingerprint", report.layout_fingerprint},
        {"diagnostics", std::move(diagnostics)},
        {"scope", "deterministic atlas planning estimates only; not encoded file size, GPU upload cost, or runtime rendering performance"}};
    return output.dump();
}

std::string sprite_pressure_report_json(const std::uint32_t frame_count,const std::uint32_t clip_count,
    const std::uint32_t frames_per_clip,const std::uint32_t atlas_columns,const std::uint32_t frame_edge,
    const std::uint32_t planned_page_edge,const std::uint32_t planned_padding,
    const std::uint32_t changed_frame_index) {
    constexpr std::uint64_t maximum_references=1000000U;
    const auto reference_count=static_cast<std::uint64_t>(clip_count)*frames_per_clip;
    if(frame_count==0||frame_count>16384||clip_count==0||clip_count>256||frames_per_clip==0||
       reference_count>maximum_references||atlas_columns==0||atlas_columns>256||frame_edge==0||frame_edge>256||
       planned_page_edge==0||planned_page_edge>8192||planned_padding>256||
       (changed_frame_index!=std::numeric_limits<std::uint32_t>::max()&&changed_frame_index>=frame_count)) {
        return Json{{"schemaVersion","noemancer.sprite-production-pressure/0.1"},{"valid",false},
            {"code","sprite.pressure.invalid-budget"},{"limits",{{"maximumFrames",16384},{"maximumClips",256},
            {"maximumClipFrameReferences",maximum_references},{"maximumAtlasColumns",256},{"maximumFrameEdge",256},
            {"maximumPlannedPageEdge",8192},{"maximumPlannedPadding",256}}}}.dump();
    }
    const auto atlas_rows=(static_cast<std::uint64_t>(frame_count)+atlas_columns-1U)/atlas_columns;
    const auto atlas_width=static_cast<std::uint64_t>(atlas_columns)*frame_edge;
    const auto atlas_height=atlas_rows*frame_edge;
    if(atlas_width>std::numeric_limits<std::uint32_t>::max()||atlas_height>std::numeric_limits<std::uint32_t>::max())
        return Json{{"schemaVersion","noemancer.sprite-production-pressure/0.1"},{"valid",false},
            {"code","sprite.pressure.atlas-dimensions-overflow"}}.dump();
    SpriteAssetDocument document;document.schema="noemancer.sprite-asset/0.2";document.asset_id="sprite.pressure";
    document.texture_asset="texture.pressure";document.texture_width=static_cast<std::uint32_t>(atlas_width);
    document.texture_height=static_cast<std::uint32_t>(atlas_height);document.frames.reserve(frame_count);
    for(std::uint32_t index=0;index<frame_count;++index) {
        const auto id="frame."+std::to_string(index);const auto x=(index%atlas_columns)*frame_edge;
        const auto y=(index/atlas_columns)*frame_edge;
        document.frames.push_back(SpriteFrame{.id=id,.x=x,.y=y,.width=frame_edge,.height=frame_edge,
            .source_width=frame_edge,.source_height=frame_edge});
    }
    document.clips.reserve(clip_count);
    for(std::uint32_t clip_index=0;clip_index<clip_count;++clip_index) {
        SpriteClip clip;clip.id="clip."+std::to_string(clip_index);clip.frames.reserve(frames_per_clip);
        for(std::uint32_t index=0;index<frames_per_clip;++index)
            clip.frames.push_back(SpriteClipFrame{.frame_id="frame."+std::to_string((static_cast<std::uint64_t>(clip_index)*frames_per_clip+index)%frame_count),.duration_ms=83});
        document.clips.push_back(std::move(clip));
    }
    const auto report=SpriteAssetCodec::production_report(document);
    SpriteAtlasPlanningOptions planning_options;
    planning_options.page_width=planned_page_edge;planning_options.page_height=planned_page_edge;
    planning_options.padding=planned_padding;
    std::vector<std::string> changed_frames;
    if(changed_frame_index!=std::numeric_limits<std::uint32_t>::max())
        changed_frames.push_back("frame."+std::to_string(changed_frame_index));
    const auto plan=SpriteAssetCodec::plan_atlas_pages(document,planning_options,changed_frames);
    Json diagnostics=Json::array();for(const auto& issue:report.diagnostics)
        diagnostics.push_back({{"code",issue.code},{"path",issue.path},{"message",issue.message}});
    constexpr std::size_t maximum_pressure_page_summaries=128U;
    Json page_summaries=Json::array();for(std::size_t index=0;
        index<std::min(plan.pages.size(),maximum_pressure_page_summaries);++index) {
        const auto& page=plan.pages[index];page_summaries.push_back({{"index",page.page_index},{"frameCount",page.frame_count},
            {"occupiedArea",page.occupied_area},{"freeArea",page.free_area},
            {"layoutFingerprint",page.layout_fingerprint}});
    }
    return Json{{"schemaVersion","noemancer.sprite-production-pressure/0.1"},{"valid",report.valid},{"code",report.code},
        {"workload",{{"frames",report.frame_count},{"clips",report.clip_count},
            {"totalClipFrameReferences",report.total_clip_frame_references},{"uniqueReferencedFrames",report.unique_referenced_frame_count},
            {"unreferencedFrames",report.unreferenced_frame_count},{"maximumFramesPerClip",report.max_clip_frame_count}}},
        {"atlas",{{"policy","single-texture-atlas"},{"pageCount",report.atlas_page_count},{"width",document.texture_width},
            {"height",document.texture_height},{"area",report.atlas_area},{"frameArea",report.frame_area_sum},
            {"occupiedArea",report.occupied_area},{"freeArea",report.free_area},{"overlapArea",report.overlap_area},
            {"layoutFingerprint",report.layout_fingerprint}}},
        {"pagePlan",{{"valid",plan.valid},{"code",plan.code},{"pageEdge",planned_page_edge},
            {"padding",planned_padding},{"pageCount",plan.page_count},{"pages",std::move(page_summaries)},
            {"pageSummariesEmitted",std::min(plan.pages.size(),maximum_pressure_page_summaries)},
            {"pageSummariesTruncated",plan.pages.size()>maximum_pressure_page_summaries},
            {"affectedPages",plan.affected_page_indices},
            {"fullCookBytes",plan.planned_cook_bytes},{"incrementalCookBytes",plan.incremental_cook_bytes},
            {"singleAtlasBaselineBytes",plan.single_atlas_cook_bytes},
            {"layoutFingerprint",plan.layout_fingerprint}}},{"diagnostics",std::move(diagnostics)},
        {"scope","deterministic-source-layout-and-reference-pressure-not-gpu-timing"}}.dump();
}

SpritePageBindingUpdateResult SpriteAssetLibrary::replace_page_bindings(
    const std::string_view asset_id, const std::vector<SpriteRuntimePageBinding>& bindings,
    const std::optional<std::uint64_t> expected_revision) {
    SpritePageBindingUpdateResult result;
    const auto asset_iterator = assets_.find(std::string(asset_id));
    const auto current_state = page_bindings_.find(std::string(asset_id));
    result.revision = current_state == page_bindings_.end() ? 0U : current_state->second.revision;
    result.binding_count = current_state == page_bindings_.end() ? 0U : current_state->second.bindings.size();
    const auto fail = [&](const std::string_view code, const std::string_view path,
                          const std::string_view message) {
        const auto first = result.diagnostics.empty();
        error(result.diagnostics, std::string(code), std::string(path), std::string(message));
        if (first) result.code = std::string(code);
        result.success = false;
    };
    if (asset_iterator == assets_.end()) {
        fail("sprite.asset-not-found", "/assetId", "Cannot bind pages for an unregistered SpriteAsset.");
        return result;
    }
    if (expected_revision && *expected_revision != result.revision) {
        fail("sprite.page-binding-revision-conflict", "/revision",
             "Expected page binding revision does not match the active overlay.");
        return result;
    }
    if (bindings.size() > sprite_runtime_binding_max_entries) {
        fail("sprite.page-binding-count-limit", "/bindings",
             "Page binding count exceeds the bounded runtime overlay limit.");
        return result;
    }

    std::unordered_set<std::string> seen_frame_ids;
    std::unordered_map<std::string, SpriteRuntimePageBinding> candidate;
    candidate.reserve(bindings.size());
    for (std::size_t index = 0U; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        const auto path = "/bindings/" + std::to_string(index);
        bool valid = true;
        if (binding.sprite_asset_id != asset_id) {
            fail("sprite.page-binding-asset-mismatch", path + "/spriteAssetId",
                 "Binding spriteAssetId must match the registration target.");
            valid = false;
        }
        if (binding.frame_id.empty()) {
            fail("sprite.page-binding-empty-frame", path + "/frameId",
                 "Binding frameId cannot be empty.");
            valid = false;
        } else if (!seen_frame_ids.insert(binding.frame_id).second) {
            fail("sprite.page-binding-duplicate-frame", path + "/frameId",
                 "A frame may occur at most once in a page binding batch.");
            valid = false;
        }
        const auto frame = std::ranges::find(asset_iterator->second.frames, binding.frame_id,
                                             &SpriteFrame::id);
        if (frame == asset_iterator->second.frames.end()) {
            fail("sprite.page-binding-unknown-frame", path + "/frameId",
                 "Binding frameId is not present in the registered SpriteAsset.");
            valid = false;
        }
        if (binding.derived_texture_asset_id.empty()) {
            fail("sprite.page-binding-empty-texture", path + "/derivedTextureAssetId",
                 "Derived texture asset ID cannot be empty.");
            valid = false;
        }
        if (binding.page_fingerprint.empty()) {
            fail("sprite.page-binding-empty-page-fingerprint", path + "/pageFingerprint",
                 "Page fingerprint cannot be empty.");
            valid = false;
        } else if (!valid_sha256_identity(binding.page_fingerprint)) {
            fail("sprite.page-binding-unsupported-page-fingerprint", path + "/pageFingerprint",
                 "Page fingerprint must be sha256: followed by exactly 64 hexadecimal digits.");
            valid = false;
        }
        if (binding.page_width == 0U || binding.page_height == 0U) {
            fail("sprite.page-binding-invalid-page-size", path + "/pageSize",
                 "Bound page dimensions must be positive.");
            valid = false;
        } else if (binding.page_width > sprite_runtime_binding_max_page_dimension ||
                   binding.page_height > sprite_runtime_binding_max_page_dimension) {
            fail("sprite.page-binding-page-size-limit", path + "/pageSize",
                 "Bound page dimensions exceed the runtime overlay limit.");
            valid = false;
        }
        if (binding.width == 0U || binding.height == 0U) {
            fail("sprite.page-binding-invalid-rect", path + "/rect",
                 "Bound page-local rect dimensions must be positive.");
            valid = false;
        } else if (static_cast<std::uint64_t>(binding.x) + binding.width > binding.page_width ||
                   static_cast<std::uint64_t>(binding.y) + binding.height > binding.page_height) {
            fail("sprite.page-binding-rect-out-of-bounds", path + "/rect",
                 "Bound page-local rect must stay inside page dimensions.");
            valid = false;
        }
        if (valid) candidate.insert_or_assign(binding.frame_id, binding);
    }
    if (!result.diagnostics.empty()) {
        return result;
    }

    const auto bindings_equal = [](const auto& left, const auto& right) noexcept {
        return left.sprite_asset_id == right.sprite_asset_id && left.frame_id == right.frame_id &&
            left.derived_texture_asset_id == right.derived_texture_asset_id &&
            left.page_index == right.page_index && left.page_width == right.page_width &&
            left.page_height == right.page_height && left.x == right.x && left.y == right.y &&
            left.width == right.width && left.height == right.height &&
            left.layout_fingerprint == right.layout_fingerprint && left.page_fingerprint == right.page_fingerprint;
    };
    bool unchanged = current_state != page_bindings_.end() &&
        current_state->second.bindings.size() == candidate.size();
    if (unchanged) {
        for (const auto& [frame_id, binding] : candidate) {
            const auto current = current_state->second.bindings.find(frame_id);
            if (current == current_state->second.bindings.end() || !bindings_equal(current->second, binding)) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged || (candidate.empty() && current_state == page_bindings_.end())) {
        result.success = true;
        result.code = "ok";
        return result;
    }
    if (current_state != page_bindings_.end() &&
        current_state->second.revision == std::numeric_limits<std::uint64_t>::max()) {
        fail("sprite.page-binding-revision-overflow", "/revision",
             "Page binding revision cannot advance beyond uint64 range.");
        return result;
    }

    const auto next_revision = current_state == page_bindings_.end() ? 1U : current_state->second.revision + 1U;
    if (current_state == page_bindings_.end()) {
        RuntimePageBindingState state;
        state.revision = next_revision;
        state.bindings = std::move(candidate);
        page_bindings_.emplace(std::string(asset_id), std::move(state));
    } else {
        // Validation and equality checks are complete before this swap, so a
        // rejected item can never partially replace the active overlay.
        current_state->second.revision = next_revision;
        current_state->second.bindings.swap(candidate);
    }
    result.success = true;
    result.code = "ok";
    result.revision = next_revision;
    result.binding_count = bindings.size();
    return result;
}

SpritePageBindingUpdateResult SpriteAssetLibrary::register_page_bindings(
    const std::string_view asset_id, const std::vector<SpriteRuntimePageBinding>& bindings,
    const std::optional<std::uint64_t> expected_revision) {
    return replace_page_bindings(asset_id, bindings, expected_revision);
}

SpritePageBindingUpdateResult SpriteAssetLibrary::clear_page_bindings(
    const std::string_view asset_id, const std::optional<std::uint64_t> expected_revision) {
    return replace_page_bindings(asset_id, {}, expected_revision);
}

bool SpriteAssetLibrary::register_asset(SpriteAssetDocument document) {
    if(!SpriteAssetCodec::validate(document).empty())return false;
    const auto id=document.asset_id;
    assets_.insert_or_assign(id,std::move(document));
    // A new authoring document may have different frame geometry.  Drop the
    // runtime overlay so no stale page-local rect can leak into resolution.
    page_bindings_.erase(id);
    return true;
}

const SpriteAssetDocument* SpriteAssetLibrary::find(const std::string_view asset_id) const noexcept {
    const auto found=assets_.find(std::string(asset_id));return found==assets_.end()?nullptr:&found->second;
}

const SpriteRuntimePageBinding* SpriteAssetLibrary::find_page_binding(
    const std::string_view asset_id, const std::string_view frame_id) const noexcept {
    const auto state = page_bindings_.find(std::string(asset_id));
    if (state == page_bindings_.end()) return nullptr;
    const auto binding = state->second.bindings.find(std::string(frame_id));
    return binding == state->second.bindings.end() ? nullptr : &binding->second;
}

std::optional<SpriteResolvedFrame> SpriteAssetLibrary::resolve_frame(
    const std::string_view asset_id,const std::string_view frame_id) const {
    const auto* asset=find(asset_id);if(asset==nullptr)return std::nullopt;
    const auto frame=std::ranges::find(asset->frames,frame_id,&SpriteFrame::id);
    if(frame==asset->frames.end())return std::nullopt;
    SpriteResolvedFrame resolved{asset->asset_id,{},asset->texture_asset,asset->texture_width,
        asset->texture_height,asset->pixels_per_unit,asset->sampling,asset->alpha_mode,asset->material,*frame,0,{}};
    if (const auto* binding = find_page_binding(asset_id, frame_id); binding != nullptr) {
        resolved.texture_asset = binding->derived_texture_asset_id;
        resolved.texture_width = binding->page_width;
        resolved.texture_height = binding->page_height;
        resolved.frame.x = binding->x;
        resolved.frame.y = binding->y;
        resolved.frame.width = binding->width;
        resolved.frame.height = binding->height;
    }
    return resolved;
}

std::optional<SpriteResolvedFrame> SpriteAssetLibrary::resolve(const SpritePlaybackState& state) const {
    const auto* asset=find(state.asset_id);if(asset==nullptr)return std::nullopt;
    const auto clip=std::ranges::find(asset->clips,state.clip_id,&SpriteClip::id);
    if(clip==asset->clips.end()||state.frame_index>=clip->frames.size())return std::nullopt;
    const auto& clip_frame=clip->frames[state.frame_index];
    const auto frame=std::ranges::find(asset->frames,clip_frame.frame_id,&SpriteFrame::id);
    if(frame==asset->frames.end())return std::nullopt;
    SpriteResolvedFrame resolved{asset->asset_id,clip->id,asset->texture_asset,asset->texture_width,
        asset->texture_height,asset->pixels_per_unit,asset->sampling,asset->alpha_mode,asset->material,*frame,
        clip_frame.duration_ms,clip_frame.event};
    if (const auto* binding = find_page_binding(state.asset_id, clip_frame.frame_id); binding != nullptr) {
        resolved.texture_asset = binding->derived_texture_asset_id;
        resolved.texture_width = binding->page_width;
        resolved.texture_height = binding->page_height;
        resolved.frame.x = binding->x;
        resolved.frame.y = binding->y;
        resolved.frame.width = binding->width;
        resolved.frame.height = binding->height;
    }
    return resolved;
}

SpritePlaybackResult SpriteAssetLibrary::advance(SpritePlaybackState& state,const double delta_seconds) const {
    SpritePlaybackResult result{.code="sprite.playback-invalid"};
    if(!std::isfinite(delta_seconds)||delta_seconds<0.0||delta_seconds>10.0) {
        result.code="sprite.invalid-delta";return result;
    }
    const auto* asset=find(state.asset_id);
    if(asset==nullptr){result.code="sprite.asset-not-found";return result;}
    const auto clip=std::ranges::find(asset->clips,state.clip_id,&SpriteClip::id);
    if(clip==asset->clips.end()){result.code="sprite.clip-not-found";return result;}
    if(state.frame_index>=clip->frames.size()){result.code="sprite.frame-index-out-of-range";return result;}
    result.success=true;result.code="ok";result.frame_id=clip->frames[state.frame_index].frame_id;
    state.last_event.clear();
    if(!state.playing||delta_seconds==0.0)return result;
    state.elapsed_in_frame_ms+=delta_seconds*1000.0;
    constexpr std::size_t transition_limit=1024;
    while(state.playing&&result.transitions<transition_limit) {
        const auto duration=static_cast<double>(clip->frames[state.frame_index].duration_ms);
        if(state.elapsed_in_frame_ms+1e-9<duration)break;
        state.elapsed_in_frame_ms-=duration;++result.transitions;result.frame_changed=true;
        if(state.frame_index+1<clip->frames.size())++state.frame_index;
        else if(clip->looping){state.frame_index=0;++state.completed_loops;result.looped=true;}
        else {state.frame_index=clip->frames.size()-1;state.elapsed_in_frame_ms=0.0;state.playing=false;}
        const auto& entered=clip->frames[state.frame_index];
        if(!entered.event.empty()){result.event=entered.event;state.last_event=entered.event;}
    }
    if(result.transitions==transition_limit&&state.playing){result.success=false;result.code="sprite.transition-limit";}
    result.frame_id=clip->frames[state.frame_index].frame_id;return result;
}

std::string SpriteAssetLibrary::observe_json(const SpritePlaybackState& state) const {
    Json out={{"schemaVersion","noemancer.sprite-playback-observation/0.1"},{"assetId",state.asset_id},
        {"clipId",state.clip_id},{"frameIndex",state.frame_index},{"elapsedInFrameMs",state.elapsed_in_frame_ms},
        {"playing",state.playing},{"completedLoops",state.completed_loops},{"lastEvent",state.last_event},
        {"valid",false},{"code","sprite.asset-not-found"},{"frame",nullptr},{"pageBinding",nullptr}};
    const auto* asset=find(state.asset_id);if(asset==nullptr)return out.dump();
    const auto clip=std::ranges::find(asset->clips,state.clip_id,&SpriteClip::id);
    if(clip==asset->clips.end()){out["code"]="sprite.clip-not-found";return out.dump();}
    if(state.frame_index>=clip->frames.size()){out["code"]="sprite.frame-index-out-of-range";return out.dump();}
    const auto resolved=resolve(state);if(!resolved){out["code"]="sprite.frame-not-found";return out.dump();}
    const auto& frame=resolved->frame;
    out["material"]=nullptr;
    if(resolved->material) {
        const auto& material=*resolved->material;
        out["material"]={{"normalTextureAsset",material.normal_texture_asset},
            {"emissiveMaskTextureAsset",material.emissive_mask_texture_asset},{"depthTextureAsset",material.depth_texture_asset},
            {"normalStrength",material.normal_strength},{"emissiveColor",Json::array({material.emissive_r,material.emissive_g,material.emissive_b})},
            {"emissiveIntensity",material.emissive_intensity},{"depthBias",material.depth_bias},
            {"shadingModel",material.shading_model},{"metallic",material.metallic},{"roughness",material.roughness},
            {"receivesShadows",material.receives_shadows},{"castsShadows",material.casts_shadows}};
    }
    out["valid"]=true;out["code"]="ok";out["frame"]={{"id",frame.id},{"rect",Json::array({frame.x,frame.y,frame.width,frame.height})},
        {"trimOffset",Json::array({frame.trim_x,frame.trim_y})},{"sourceSize",Json::array({frame.source_width,frame.source_height})},
        {"pivot",Json::array({frame.pivot_x,frame.pivot_y})},{"collisionProfile",frame.collision_profile},
        {"durationMs",resolved->duration_ms},{"event",resolved->event}};
    if (const auto* binding = find_page_binding(state.asset_id, frame.id); binding != nullptr) {
        out["pageBinding"] = {
            {"spriteAssetId", binding->sprite_asset_id},
            {"frameId", binding->frame_id},
            {"derivedTextureAssetId", binding->derived_texture_asset_id},
            {"pageIndex", binding->page_index},
            {"pageSize", Json::array({binding->page_width, binding->page_height})},
            {"rect", Json::array({binding->x, binding->y, binding->width, binding->height})},
            {"layoutFingerprint", binding->layout_fingerprint},
            {"pageFingerprint", binding->page_fingerprint}};
    }
    return out.dump();
}

std::string SpriteAssetLibrary::observe_page_bindings_json(const std::string_view asset_id) const {
    Json out={{"schemaVersion","noemancer.sprite-page-binding-observation/0.1"},
        {"assetId",asset_id},{"valid",false},{"code","sprite.asset-not-found"},
        {"revision",0U},{"bindingCount",0U},{"bindings",{{"total",0U},
            {"maxItems",sprite_runtime_binding_max_observation_entries},{"emitted",0U},
            {"truncated",false},{"items",Json::array()}}},
        {"diagnostics",Json::array()},
        {"scope","runtime-only renderer-neutral page bindings; not persisted authoring data or GPU performance"}};
    if (find(asset_id) == nullptr) {
        out["diagnostics"] = Json::array({{{"code","sprite.asset-not-found"},
            {"path","/assetId"},{"message","Cannot observe bindings for an unregistered SpriteAsset."}}});
        return out.dump();
    }
    const auto state = page_bindings_.find(std::string(asset_id));
    if (state == page_bindings_.end()) {
        out["valid"] = true;
        out["code"] = "ok";
        out["bindings"] = {
            {"total",0U}, {"maxItems",sprite_runtime_binding_max_observation_entries},
            {"emitted",0U}, {"truncated",false}, {"items",Json::array()}};
        return out.dump();
    }

    std::vector<const SpriteRuntimePageBinding*> ordered;
    ordered.reserve(state->second.bindings.size());
    for (const auto& [unused_frame_id, binding] : state->second.bindings) {
        static_cast<void>(unused_frame_id);
        ordered.push_back(&binding);
    }
    std::ranges::sort(ordered, [](const auto* left, const auto* right) {
        return left->frame_id < right->frame_id;
    });
    const auto emit_count = std::min(ordered.size(), sprite_runtime_binding_max_observation_entries);
    Json items = Json::array();
    for (std::size_t index = 0U; index < emit_count; ++index) {
        const auto& binding = *ordered[index];
        items.push_back({
            {"spriteAssetId", binding.sprite_asset_id},
            {"frameId", binding.frame_id},
            {"derivedTextureAssetId", binding.derived_texture_asset_id},
            {"pageIndex", binding.page_index},
            {"pageSize", Json::array({binding.page_width, binding.page_height})},
            {"rect", Json::array({binding.x, binding.y, binding.width, binding.height})},
            {"layoutFingerprint", binding.layout_fingerprint},
            {"pageFingerprint", binding.page_fingerprint}});
    }
    out["valid"] = true;
    out["code"] = "ok";
    out["revision"] = state->second.revision;
    out["bindingCount"] = state->second.bindings.size();
    out["bindings"] = {
        {"total", state->second.bindings.size()},
        {"maxItems", sprite_runtime_binding_max_observation_entries},
        {"emitted", emit_count},
        {"truncated", ordered.size() > emit_count},
        {"items", std::move(items)}};
    return out.dump();
}

} // namespace noemancer
