#include "engine/image_decoder.hpp"

#include <lodepng.h>
#include <turbojpeg.h>

#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>

namespace noemancer {

DecodedImage decode_image_rgba8(const std::span<const std::byte> encoded,
                                const std::string_view format_hint) {
    std::string format(format_hint);
    std::ranges::transform(format, format.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (format == "png" || format == ".png" || format == "image/png")
        return decode_png_rgba8(encoded);
    if (format == "jpg" || format == ".jpg" || format == "jpeg" || format == ".jpeg" ||
        format == "image/jpeg") return decode_jpeg_rgba8(encoded);
    DecodedImage result;
    result.code = "image.unsupported-format";
    result.detail = "The engine image adapter supports PNG and JPEG RGBA8 decode for this path.";
    return result;
}

DecodedImage decode_jpeg_rgba8(const std::span<const std::byte> encoded) {
    DecodedImage result;
    if (encoded.empty()) {
        result.code = "image.empty";
        result.detail = "JPEG payload is empty.";
        return result;
    }
    auto* decoder = tj3Init(TJINIT_DECOMPRESS);
    if (decoder == nullptr) {
        result.code = "image.jpeg-decoder-unavailable";
        result.detail = "libjpeg-turbo could not create a decompressor.";
        return result;
    }
    const auto fail = [&](const std::string_view code) {
        result.code = std::string(code);
        const auto* error = tj3GetErrorStr(decoder);
        result.detail = error != nullptr ? error : "libjpeg-turbo rejected the JPEG payload.";
        tj3Destroy(decoder);
        return result;
    };
    if (tj3DecompressHeader(decoder,
            reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) != 0)
        return fail("image.jpeg-invalid-header");
    const int width = tj3Get(decoder, TJPARAM_JPEGWIDTH);
    const int height = tj3Get(decoder, TJPARAM_JPEGHEIGHT);
    constexpr std::uint64_t maximum_pixels = 64ULL * 1024ULL * 1024ULL;
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint64_t>(width) > maximum_pixels / static_cast<std::uint64_t>(height)) {
        tj3Destroy(decoder);
        result.code = "image.invalid-dimensions";
        result.detail = "JPEG dimensions are empty or exceed the 64-megapixel decode budget.";
        return result;
    }
    const auto bytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ULL;
    if (bytes > std::numeric_limits<std::size_t>::max()) {
        tj3Destroy(decoder);
        result.code = "image.too-large";
        result.detail = "JPEG RGBA8 output exceeds the decoder address space.";
        return result;
    }
    result.rgba8.resize(static_cast<std::size_t>(bytes));
    if (tj3Decompress8(decoder,
            reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(),
            result.rgba8.data(), 0, TJPF_RGBA) != 0) {
        result.rgba8.clear();
        return fail("image.jpeg-decode-failed");
    }
    tj3Destroy(decoder);
    result.valid = true;
    result.code = "ok";
    result.detail = "JPEG decoded by libjpeg-turbo to tightly packed RGBA8.";
    result.width = static_cast<std::uint32_t>(width);
    result.height = static_cast<std::uint32_t>(height);
    return result;
}

DecodedImage decode_png_rgba8(const std::span<const std::byte> encoded) {
    DecodedImage result;
    if (encoded.empty()) {
        result.code = "image.empty";
        result.detail = "PNG payload is empty.";
        return result;
    }
    if (encoded.size() > std::numeric_limits<std::size_t>::max() / 4U) {
        result.code = "image.too-large";
        result.detail = "PNG payload exceeds the decoder address space.";
        return result;
    }
    unsigned width = 0U;
    unsigned height = 0U;
    const unsigned error = lodepng::decode(
        result.rgba8,
        width,
        height,
        reinterpret_cast<const unsigned char*>(encoded.data()),
        encoded.size(),
        LCT_RGBA,
        8U);
    if (error != 0U) {
        result.code = "image.png-decode-failed";
        result.detail = lodepng_error_text(error);
        result.rgba8.clear();
        return result;
    }
    if (width == 0U || height == 0U ||
        static_cast<std::uint64_t>(width) * height * 4ULL != result.rgba8.size()) {
        result.code = "image.invalid-dimensions";
        result.detail = "Decoded PNG dimensions do not match its RGBA8 payload.";
        result.rgba8.clear();
        return result;
    }
    result.valid = true;
    result.code = "ok";
    result.detail = "PNG decoded to tightly packed RGBA8.";
    result.width = width;
    result.height = height;
    return result;
}

EncodedPng encode_png_rgba8(const std::uint32_t width, const std::uint32_t height,
                            const std::span<const std::uint8_t> rgba8) {
    EncodedPng result;
    if (width == 0U || height == 0U ||
        static_cast<std::uint64_t>(width) * height * 4ULL != rgba8.size()) {
        result.code = "image.invalid-dimensions";
        result.detail = "RGBA8 payload does not match the requested PNG dimensions.";
        return result;
    }
    const unsigned error = lodepng::encode(result.bytes, rgba8.data(), width, height, LCT_RGBA, 8U);
    if (error != 0U) {
        result.code = "image.png-encode-failed";
        result.detail = lodepng_error_text(error);
        result.bytes.clear();
        return result;
    }
    result.valid = true;
    result.code = "ok";
    result.detail = "RGBA8 encoded as PNG.";
    return result;
}

DecodedHdrImage decode_radiance_hdr(const std::span<const std::byte> encoded) {
    DecodedHdrImage result;
    if (encoded.empty()) { result.code="image.empty"; result.detail="Radiance HDR payload is empty."; return result; }
    const auto* bytes=reinterpret_cast<const std::uint8_t*>(encoded.data());
    std::size_t cursor{};
    const auto read_line=[&](std::string& value) {
        if (cursor>=encoded.size()) return false;
        const auto start=cursor;
        while (cursor<encoded.size() && bytes[cursor]!='\n') ++cursor;
        value.assign(reinterpret_cast<const char*>(bytes+start),cursor-start);
        if (!value.empty() && value.back()=='\r') value.pop_back();
        if (cursor<encoded.size()) ++cursor;
        return true;
    };
    std::string header;
    if (!read_line(header) || (header!="#?RADIANCE" && header!="#?RGBE")) {
        result.code="image.hdr-invalid-signature"; result.detail="Expected a Radiance RGBE signature."; return result;
    }
    bool format_found=false;
    while (read_line(header) && !header.empty()) if (header=="FORMAT=32-bit_rle_rgbe") format_found=true;
    if (!format_found || !read_line(header)) {
        result.code="image.hdr-invalid-header"; result.detail="Radiance HDR FORMAT or resolution line is missing."; return result;
    }
    char y_axis{},x_axis{},y_sign{},x_sign{}; unsigned height{},width{};
    std::istringstream resolution(header);
    resolution>>y_sign>>y_axis>>height>>x_sign>>x_axis>>width;
    if (!resolution || (y_axis!='Y'&&y_axis!='y') || (x_axis!='X'&&x_axis!='x') || width==0 || height==0 ||
        static_cast<std::uint64_t>(width)*height>268435456ULL) {
        result.code="image.hdr-invalid-dimensions"; result.detail="Radiance HDR resolution is invalid or unsupported."; return result;
    }
    std::vector<std::uint8_t> rgbe(static_cast<std::size_t>(width)*height*4U);
    for (unsigned scanline=0;scanline<height;++scanline) {
        auto* destination=rgbe.data()+static_cast<std::size_t>(scanline)*width*4U;
        if (cursor+4U>encoded.size()) { result.code="image.hdr-truncated"; result.detail="HDR pixel stream ended before a scanline."; return result; }
        const bool rle=width>=8U && width<=32767U && bytes[cursor]==2U && bytes[cursor+1U]==2U &&
            ((static_cast<unsigned>(bytes[cursor+2U])<<8U)|bytes[cursor+3U])==width;
        if (!rle) {
            const auto count=static_cast<std::size_t>(width)*4U;
            if (cursor+count>encoded.size()) { result.code="image.hdr-truncated"; result.detail="Flat RGBE scanline is truncated."; return result; }
            std::memcpy(destination,bytes+cursor,count); cursor+=count; continue;
        }
        cursor+=4U;
        for (unsigned channel=0;channel<4U;++channel) {
            unsigned written{};
            while (written<width) {
                if (cursor>=encoded.size()) { result.code="image.hdr-truncated"; result.detail="RLE packet is truncated."; return result; }
                const auto count=bytes[cursor++];
                if (count>128U) {
                    const unsigned run=count-128U;
                    if (run==0U || written+run>width || cursor>=encoded.size()) { result.code="image.hdr-invalid-rle"; result.detail="Invalid HDR RLE run."; return result; }
                    const auto value=bytes[cursor++];
                    for (unsigned i=0;i<run;++i) destination[(written+i)*4U+channel]=value;
                    written+=run;
                } else {
                    if (count==0U || written+count>width || cursor+count>encoded.size()) { result.code="image.hdr-invalid-rle"; result.detail="Invalid HDR RLE literal."; return result; }
                    for (unsigned i=0;i<count;++i) destination[(written+i)*4U+channel]=bytes[cursor+i];
                    cursor+=count; written+=count;
                }
            }
        }
    }
    result.rgba32f.resize(static_cast<std::size_t>(width)*height*4U);
    for (unsigned source_y=0;source_y<height;++source_y) for (unsigned source_x=0;source_x<width;++source_x) {
        const unsigned x=x_sign=='+'?source_x:width-1U-source_x;
        const unsigned y=y_sign=='-'?source_y:height-1U-source_y;
        const auto source=(static_cast<std::size_t>(source_y)*width+source_x)*4U;
        const auto target=(static_cast<std::size_t>(y)*width+x)*4U;
        const auto exponent=rgbe[source+3U];
        const float scale=exponent?std::ldexp(1.0F,static_cast<int>(exponent)-136):0.0F;
        result.rgba32f[target]=(static_cast<float>(rgbe[source])+0.5F)*scale;
        result.rgba32f[target+1U]=(static_cast<float>(rgbe[source+1U])+0.5F)*scale;
        result.rgba32f[target+2U]=(static_cast<float>(rgbe[source+2U])+0.5F)*scale;
        result.rgba32f[target+3U]=1.0F;
    }
    result.valid=true; result.code="ok"; result.detail="Radiance RGBE decoded to linear RGBA32F.";
    result.width=width; result.height=height;
    return result;
}

} // namespace noemancer
