#include "engine/ibl_cook.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace noemancer {
namespace {

constexpr float pi = 3.14159265359F;
constexpr std::array<std::uint8_t, 8> magic{'N','M','I','B','L','0','0','2'};
constexpr std::uint32_t endian_marker = 0x01020304U;
constexpr std::uint32_t maximum_resolution = 4096U;
constexpr std::uint32_t maximum_samples = 16384U;
constexpr std::uint64_t maximum_payload_bytes = 1024ULL * 1024ULL * 1024ULL;

struct Vec3 { float x{}; float y{}; float z{}; };
Vec3 operator+(const Vec3 a, const Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 operator-(const Vec3 a, const Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 operator*(const Vec3 value, const float scale) { return {value.x*scale,value.y*scale,value.z*scale}; }
float dot(const Vec3 a, const Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(const Vec3 a, const Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 normalize(const Vec3 value) {
    const float length=std::sqrt(std::max(dot(value,value),1.0e-20F));
    return value*(1.0F/length);
}

std::uint16_t float_to_half(const float value) {
    std::uint32_t bits{}; std::memcpy(&bits,&value,sizeof(bits));
    const std::uint32_t sign=(bits>>16U)&0x8000U;
    const int exponent=static_cast<int>((bits>>23U)&0xffU)-127+15;
    const std::uint32_t mantissa=bits&0x7fffffU;
    if (exponent<=0) {
        if (exponent<-10) return static_cast<std::uint16_t>(sign);
        const auto shifted=(mantissa|0x800000U)>>static_cast<std::uint32_t>(1-exponent);
        return static_cast<std::uint16_t>(sign+((shifted+0x1000U)>>13U));
    }
    if (exponent>=31) return static_cast<std::uint16_t>(sign|0x7c00U);
    return static_cast<std::uint16_t>(sign|(static_cast<std::uint32_t>(exponent)<<10U)|((mantissa+0x1000U)>>13U));
}

float radical_inverse(std::uint32_t bits) {
    bits=(bits<<16U)|(bits>>16U); bits=((bits&0x55555555U)<<1U)|((bits&0xaaaaaaaaU)>>1U);
    bits=((bits&0x33333333U)<<2U)|((bits&0xccccccccU)>>2U);
    bits=((bits&0x0f0f0f0fU)<<4U)|((bits&0xf0f0f0f0U)>>4U);
    bits=((bits&0x00ff00ffU)<<8U)|((bits&0xff00ff00U)>>8U);
    return static_cast<float>(bits)*2.3283064365386963e-10F;
}

Vec3 cube_direction(const std::uint32_t face,const std::uint32_t x,const std::uint32_t y,const std::uint32_t size) {
    const float u=2.0F*(static_cast<float>(x)+0.5F)/static_cast<float>(size)-1.0F;
    const float v=2.0F*(static_cast<float>(y)+0.5F)/static_cast<float>(size)-1.0F;
    switch(face) {
    case 0:return normalize({1.0F,-v,-u}); case 1:return normalize({-1.0F,-v,u});
    case 2:return normalize({u,1.0F,v}); case 3:return normalize({u,-1.0F,-v});
    case 4:return normalize({u,-v,1.0F}); default:return normalize({-u,-v,-1.0F});
    }
}

Vec3 procedural_radiance(const Vec3 direction) {
    const float sky=std::clamp(direction.y*0.5F+0.5F,0.0F,1.0F);
    const float horizon=std::pow(1.0F-std::abs(direction.y),3.0F);
    Vec3 color{0.025F+(0.06F-0.025F)*sky+0.16F*horizon,
        0.022F+(0.15F-0.022F)*sky+0.18F*horizon,0.018F+(0.38F-0.018F)*sky+0.22F*horizon};
    const Vec3 sun_direction=normalize({0.55F,0.72F,0.35F});
    const float sun=std::pow(std::max(dot(direction,sun_direction),0.0F),256.0F);
    return color+Vec3{1.0F,0.78F,0.48F}*sun;
}

Vec3 sample_hdr(const DecodedHdrImage* image,const Vec3 direction) {
    if (!image || !image->valid) return procedural_radiance(direction);
    const float u=std::atan2(direction.z,direction.x)/(2.0F*pi)+0.5F;
    const float v=std::acos(std::clamp(direction.y,-1.0F,1.0F))/pi;
    const float px=u*static_cast<float>(image->width)-0.5F,py=v*static_cast<float>(image->height)-0.5F;
    const int x0=static_cast<int>(std::floor(px));
    const int y0=std::clamp(static_cast<int>(std::floor(py)),0,static_cast<int>(image->height)-1);
    const int y1=std::min(y0+1,static_cast<int>(image->height)-1);
    const auto wrap=[&](const int x){const int width=static_cast<int>(image->width);return (x%width+width)%width;};
    const float tx=px-std::floor(px),ty=py-std::floor(py);
    const auto pixel=[&](const int x,const int y){const auto offset=(static_cast<std::size_t>(y)*image->width+static_cast<unsigned>(wrap(x)))*4U;
        return Vec3{image->rgba32f[offset],image->rgba32f[offset+1U],image->rgba32f[offset+2U]};};
    const Vec3 top=pixel(x0,y0)*(1.0F-tx)+pixel(x0+1,y0)*tx;
    const Vec3 bottom=pixel(x0,y1)*(1.0F-tx)+pixel(x0+1,y1)*tx;
    return top*(1.0F-ty)+bottom*ty;
}

Vec3 tangent_to_world(const Vec3 sample,const Vec3 normal) {
    const Vec3 tangent=normalize(cross(std::abs(normal.y)<0.999F?Vec3{0,1,0}:Vec3{1,0,0},normal));
    const Vec3 bitangent=cross(normal,tangent);
    return normalize(tangent*sample.x+bitangent*sample.y+normal*sample.z);
}

bool valid_profile(const IblCookProfile& value) {
    if (!value.specular_resolution || !value.specular_mip_levels || !value.irradiance_resolution ||
        !value.brdf_lut_resolution || !value.specular_samples || !value.irradiance_samples || !value.brdf_lut_samples) return false;
    if (value.specular_resolution>maximum_resolution || value.irradiance_resolution>maximum_resolution ||
        value.brdf_lut_resolution>maximum_resolution || value.specular_samples>maximum_samples ||
        value.irradiance_samples>maximum_samples || value.brdf_lut_samples>maximum_samples) return false;
    const auto maximum_mips=1U+static_cast<std::uint32_t>(std::floor(std::log2(value.specular_resolution)));
    return value.specular_mip_levels<=maximum_mips;
}

std::uint64_t fnv1a(const std::span<const std::byte> bytes) {
    std::uint64_t hash=14695981039346656037ULL;
    for (const auto byte:bytes) { hash^=std::to_integer<std::uint8_t>(byte); hash*=1099511628211ULL; }
    return hash;
}

template <typename T> void append_scalar(std::vector<std::byte>& bytes,const T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto offset=bytes.size(); bytes.resize(offset+sizeof(T)); std::memcpy(bytes.data()+offset,&value,sizeof(T));
}
void append_string(std::vector<std::byte>& bytes,const std::string& value) {
    append_scalar(bytes,static_cast<std::uint32_t>(value.size()));
    const auto offset=bytes.size(); bytes.resize(offset+value.size()); std::memcpy(bytes.data()+offset,value.data(),value.size());
}
template <typename T> void append_vector(std::vector<std::byte>& bytes,const std::vector<T>& value) {
    append_scalar(bytes,static_cast<std::uint64_t>(value.size()));
    const auto byte_count=value.size()*sizeof(T); const auto offset=bytes.size(); bytes.resize(offset+byte_count);
    if (byte_count) std::memcpy(bytes.data()+offset,value.data(),byte_count);
}
template <typename T> bool read_scalar(const std::span<const std::byte> bytes,std::size_t& cursor,T& value) {
    if (cursor>bytes.size() || bytes.size()-cursor<sizeof(T)) return false;
    std::memcpy(&value,bytes.data()+cursor,sizeof(T)); cursor+=sizeof(T); return true;
}
bool read_string(const std::span<const std::byte> bytes,std::size_t& cursor,std::string& value) {
    std::uint32_t size{}; if (!read_scalar(bytes,cursor,size) || size>65536U || bytes.size()-cursor<size) return false;
    value.assign(reinterpret_cast<const char*>(bytes.data()+cursor),size); cursor+=size; return true;
}
template <typename T> bool read_vector(const std::span<const std::byte> bytes,std::size_t& cursor,std::vector<T>& value) {
    std::uint64_t count{}; if (!read_scalar(bytes,cursor,count) || count>maximum_payload_bytes/sizeof(T)) return false;
    const auto byte_count=static_cast<std::size_t>(count)*sizeof(T);
    if (bytes.size()-cursor<byte_count) return false;
    value.resize(static_cast<std::size_t>(count)); if (byte_count) std::memcpy(value.data(),bytes.data()+cursor,byte_count);
    cursor+=byte_count; return true;
}

std::vector<std::byte> serialize(const IblCookProduct& product) {
    std::vector<std::byte> bytes; bytes.reserve(256U+(product.specular_rgba16f.size()+product.irradiance_rgba16f.size()+product.brdf_lut_rg16f.size())*2U);
    for (const auto value:magic) bytes.push_back(static_cast<std::byte>(value));
    append_scalar(bytes,endian_marker); append_string(bytes,product.source_id); append_string(bytes,product.source_fingerprint);
    const auto& p=product.profile;
    append_scalar(bytes,p.specular_resolution); append_scalar(bytes,p.specular_mip_levels);
    append_scalar(bytes,p.irradiance_resolution); append_scalar(bytes,p.brdf_lut_resolution);
    append_scalar(bytes,p.specular_samples); append_scalar(bytes,p.irradiance_samples); append_scalar(bytes,p.brdf_lut_samples);
    append_vector(bytes,product.specular_rgba16f); append_vector(bytes,product.irradiance_rgba16f); append_vector(bytes,product.brdf_lut_rg16f);
    append_scalar(bytes,fnv1a(bytes)); return bytes;
}

IblCookProduct deserialize(const std::span<const std::byte> bytes,const std::string& fingerprint,const IblCookProfile& profile) {
    IblCookProduct result; result.code="ibl.cache-invalid"; result.detail="IBL cache header or payload is invalid.";
    if (bytes.size()<magic.size()+sizeof(std::uint32_t)+sizeof(std::uint64_t) || bytes.size()>maximum_payload_bytes) return result;
    for (std::size_t i=0;i<magic.size();++i) if (std::to_integer<std::uint8_t>(bytes[i])!=magic[i]) return result;
    std::uint64_t stored_hash{}; std::memcpy(&stored_hash,bytes.data()+bytes.size()-sizeof(stored_hash),sizeof(stored_hash));
    if (stored_hash!=fnv1a(bytes.first(bytes.size()-sizeof(stored_hash)))) { result.code="ibl.cache-checksum"; result.detail="IBL cache checksum does not match."; return result; }
    std::size_t cursor=magic.size(); std::uint32_t endian{};
    if (!read_scalar(bytes,cursor,endian) || endian!=endian_marker || !read_string(bytes,cursor,result.source_id) ||
        !read_string(bytes,cursor,result.source_fingerprint)) return result;
    auto& p=result.profile;
    if (!read_scalar(bytes,cursor,p.specular_resolution)||!read_scalar(bytes,cursor,p.specular_mip_levels)||
        !read_scalar(bytes,cursor,p.irradiance_resolution)||!read_scalar(bytes,cursor,p.brdf_lut_resolution)||
        !read_scalar(bytes,cursor,p.specular_samples)||!read_scalar(bytes,cursor,p.irradiance_samples)||!read_scalar(bytes,cursor,p.brdf_lut_samples)||
        !read_vector(bytes,cursor,result.specular_rgba16f)||!read_vector(bytes,cursor,result.irradiance_rgba16f)||
        !read_vector(bytes,cursor,result.brdf_lut_rg16f)||cursor!=bytes.size()-sizeof(stored_hash)) return result;
    if (result.source_fingerprint!=fingerprint || ibl_profile_fingerprint(p)!=ibl_profile_fingerprint(profile)) {
        result.code="ibl.cache-stale"; result.detail="IBL cache source or profile fingerprint is stale."; return result;
    }
    std::uint64_t specular_texels{}; for (std::uint32_t level=0;level<p.specular_mip_levels;++level) {
        const auto size=std::max(1U,p.specular_resolution>>level); specular_texels+=static_cast<std::uint64_t>(size)*size*6ULL;
    }
    const auto irradiance_texels=static_cast<std::uint64_t>(p.irradiance_resolution)*p.irradiance_resolution*6ULL;
    const auto lut_texels=static_cast<std::uint64_t>(p.brdf_lut_resolution)*p.brdf_lut_resolution;
    if (result.specular_rgba16f.size()!=specular_texels*4ULL || result.irradiance_rgba16f.size()!=irradiance_texels*4ULL ||
        result.brdf_lut_rg16f.size()!=lut_texels*2ULL) return result;
    result.valid=true; result.code="ok"; result.detail="Versioned Split-Sum IBL cache decoded and verified."; return result;
}

std::string hash_string(const std::string& value) {
    const auto bytes=std::as_bytes(std::span(value.data(),value.size()));
    std::ostringstream stream; stream<<std::hex<<std::setfill('0')<<std::setw(16)<<fnv1a(bytes); return stream.str();
}

} // namespace

std::string ibl_profile_fingerprint(const IblCookProfile& p) {
    return "split-sum-ggx/2:"+std::to_string(p.specular_resolution)+"x"+std::to_string(p.specular_mip_levels)+":"+
        std::to_string(p.irradiance_resolution)+":"+std::to_string(p.brdf_lut_resolution)+":"+
        std::to_string(p.specular_samples)+":"+std::to_string(p.irradiance_samples)+":"+std::to_string(p.brdf_lut_samples);
}

IblCookProduct cook_split_sum_ibl(const DecodedHdrImage* source,std::string source_id,std::string source_fingerprint,const IblCookProfile& profile) {
    IblCookProduct result; result.source_id=std::move(source_id); result.source_fingerprint=std::move(source_fingerprint); result.profile=profile;
    if (!valid_profile(profile)) { result.code="ibl.invalid-profile"; result.detail="IBL Cook dimensions, mip count or sample counts are invalid."; return result; }
    if (source && (!source->valid || !source->width || !source->height || source->rgba32f.size()!=static_cast<std::uint64_t>(source->width)*source->height*4ULL)) {
        result.code="ibl.invalid-source"; result.detail="HDR source dimensions do not match its linear RGBA32F payload."; return result;
    }
    for (std::uint32_t level=0;level<profile.specular_mip_levels;++level) {
        const auto size=std::max(1U,profile.specular_resolution>>level);
        const float roughness=profile.specular_mip_levels>1U?static_cast<float>(level)/static_cast<float>(profile.specular_mip_levels-1U):0.0F;
        for (std::uint32_t face=0;face<6U;++face) for (std::uint32_t y=0;y<size;++y) for (std::uint32_t x=0;x<size;++x) {
            const Vec3 normal=cube_direction(face,x,y,size),view=normal; Vec3 integrated{}; float weight_sum{};
            for (std::uint32_t sample=0;sample<profile.specular_samples;++sample) {
                const float xi_x=static_cast<float>(sample)/static_cast<float>(profile.specular_samples),xi_y=radical_inverse(sample);
                const float alpha=std::max(roughness*roughness,0.001F),alpha2=alpha*alpha,phi=2.0F*pi*xi_x;
                const float cos_theta=std::sqrt((1.0F-xi_y)/(1.0F+(alpha2-1.0F)*xi_y));
                const float sin_theta=std::sqrt(std::max(0.0F,1.0F-cos_theta*cos_theta));
                const Vec3 half=tangent_to_world({std::cos(phi)*sin_theta,std::sin(phi)*sin_theta,cos_theta},normal);
                const Vec3 light=normalize(half*(2.0F*dot(view,half))-view); const float n_dot_l=std::max(dot(normal,light),0.0F);
                if (n_dot_l>0.0F) { integrated=integrated+sample_hdr(source,light)*n_dot_l; weight_sum+=n_dot_l; }
            }
            integrated=integrated*(1.0F/std::max(weight_sum,0.0001F));
            result.specular_rgba16f.insert(result.specular_rgba16f.end(),{float_to_half(integrated.x),float_to_half(integrated.y),float_to_half(integrated.z),float_to_half(1.0F)});
        }
    }
    for (std::uint32_t face=0;face<6U;++face) for (std::uint32_t y=0;y<profile.irradiance_resolution;++y) for (std::uint32_t x=0;x<profile.irradiance_resolution;++x) {
        const Vec3 normal=cube_direction(face,x,y,profile.irradiance_resolution); Vec3 integrated{};
        for (std::uint32_t sample=0;sample<profile.irradiance_samples;++sample) {
            const float xi_x=static_cast<float>(sample)/static_cast<float>(profile.irradiance_samples),xi_y=radical_inverse(sample),radius=std::sqrt(xi_y),phi=2.0F*pi*xi_x;
            integrated=integrated+sample_hdr(source,tangent_to_world({radius*std::cos(phi),radius*std::sin(phi),std::sqrt(1.0F-xi_y)},normal));
        }
        integrated=integrated*(1.0F/static_cast<float>(profile.irradiance_samples));
        result.irradiance_rgba16f.insert(result.irradiance_rgba16f.end(),{float_to_half(integrated.x),float_to_half(integrated.y),float_to_half(integrated.z),float_to_half(1.0F)});
    }
    result.brdf_lut_rg16f.reserve(static_cast<std::size_t>(profile.brdf_lut_resolution)*profile.brdf_lut_resolution*2U);
    for (std::uint32_t y=0;y<profile.brdf_lut_resolution;++y) for (std::uint32_t x=0;x<profile.brdf_lut_resolution;++x) {
        const float n_dot_v=(static_cast<float>(x)+0.5F)/static_cast<float>(profile.brdf_lut_resolution);
        const float roughness=(static_cast<float>(y)+0.5F)/static_cast<float>(profile.brdf_lut_resolution);
        const Vec3 view{std::sqrt(std::max(0.0F,1.0F-n_dot_v*n_dot_v)),0.0F,n_dot_v}; float scale{},bias{};
        for (std::uint32_t sample=0;sample<profile.brdf_lut_samples;++sample) {
            const float xi_x=static_cast<float>(sample)/static_cast<float>(profile.brdf_lut_samples),xi_y=radical_inverse(sample);
            const float alpha=std::max(roughness*roughness,0.001F),alpha2=alpha*alpha,phi=2.0F*pi*xi_x;
            const float cos_theta=std::sqrt((1.0F-xi_y)/(1.0F+(alpha2-1.0F)*xi_y));
            const float sin_theta=std::sqrt(std::max(0.0F,1.0F-cos_theta*cos_theta));
            const Vec3 half{std::cos(phi)*sin_theta,std::sin(phi)*sin_theta,cos_theta};
            const Vec3 light=normalize(half*(2.0F*dot(view,half))-view);
            const float n_dot_l=std::max(light.z,0.0F),n_dot_h=std::max(half.z,0.0F),v_dot_h=std::max(dot(view,half),0.0F);
            if (n_dot_l<=0.0F) continue;
            const float k=roughness*roughness*0.5F;
            const auto geometry=[k](const float cosine){return cosine/(cosine*(1.0F-k)+k);};
            const float g_visible=geometry(n_dot_v)*geometry(n_dot_l)*v_dot_h/std::max(n_dot_h*n_dot_v,0.0001F);
            const float fresnel=std::pow(1.0F-v_dot_h,5.0F); scale+=(1.0F-fresnel)*g_visible; bias+=fresnel*g_visible;
        }
        result.brdf_lut_rg16f.push_back(float_to_half(scale/static_cast<float>(profile.brdf_lut_samples)));
        result.brdf_lut_rg16f.push_back(float_to_half(bias/static_cast<float>(profile.brdf_lut_samples)));
    }
    result.valid=true; result.code="ok"; result.detail="Split-Sum GGX IBL textures cooked deterministically on the CPU."; return result;
}

IblCacheResult load_or_cook_split_sum_ibl(const std::filesystem::path& cache_root,const DecodedHdrImage* source,
    std::string source_id,std::string source_fingerprint,const IblCookProfile& profile) {
    IblCacheResult result;
    const auto key=hash_string(source_fingerprint+":"+ibl_profile_fingerprint(profile));
    result.artifact_path=cache_root/key/"split-sum-ggx-v2.nmibl";
    std::ifstream input(result.artifact_path,std::ios::binary|std::ios::ate);
    if (input) {
        const auto size=input.tellg();
        if (size>0 && static_cast<std::uint64_t>(size)<=maximum_payload_bytes) {
            std::vector<std::byte> bytes(static_cast<std::size_t>(size)); input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()),size);
            result.product=deserialize(bytes,source_fingerprint,profile);
            if (result.product.valid) { result.cache_hit=true; result.artifact_bytes=bytes.size(); return result; }
            result.cache_rebuilt=true;
        }
    }
    input.close();
    const auto start=std::chrono::steady_clock::now();
    result.product=cook_split_sum_ibl(source,std::move(source_id),std::move(source_fingerprint),profile);
    result.cook_microseconds=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    if (!result.product.valid) return result;
    const auto bytes=serialize(result.product); std::error_code error;
    std::filesystem::create_directories(result.artifact_path.parent_path(),error);
    if (error) { result.product.valid=false; result.product.code="ibl.cache-directory"; result.product.detail=error.message(); return result; }
    auto temporary=result.artifact_path; temporary+=".tmp";
    { std::ofstream output(temporary,std::ios::binary|std::ios::trunc); output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())); output.flush();
      if (!output) { result.product.valid=false; result.product.code="ibl.cache-write"; result.product.detail="Failed to write the temporary IBL artifact."; return result; } }
    if (std::filesystem::exists(result.artifact_path,error)) std::filesystem::remove(result.artifact_path,error);
    error.clear(); std::filesystem::rename(temporary,result.artifact_path,error);
    if (error) { std::filesystem::remove(temporary); result.product.valid=false; result.product.code="ibl.cache-commit"; result.product.detail=error.message(); return result; }
    result.artifact_bytes=bytes.size(); return result;
}

} // namespace noemancer
