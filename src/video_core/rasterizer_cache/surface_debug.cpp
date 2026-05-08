// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <cstring>
#include "common/color.h"
#include "video_core/rasterizer_cache/surface_debug.h"
#include "video_core/texture/texture_decode.h"
#include "video_core/utils.h"

namespace VideoCore {

void SurfaceOpLog::Start(bool usage) {
    std::scoped_lock lock{mutex};
    if (count.load(std::memory_order_relaxed) >= MAX_RECORDS) {
        full = true;
        return;
    }
    full = false;
    record_usage = usage;
    recording = true;
}

void SurfaceOpLog::Stop() {
    recording = false;
}

void SurfaceOpLog::Clear() {
    std::scoped_lock lock{mutex};
    for (auto& chunk : chunks) {
        chunk.reset();
    }
    count.store(0, std::memory_order_release);
    full = false;
}

void SurfaceOpLog::Append(const SurfaceOpRecord& record) {
    std::scoped_lock lock{mutex};
    if (!recording.load(std::memory_order_relaxed)) {
        return;
    }
    const std::size_t index = count.load(std::memory_order_relaxed);
    if (index >= MAX_RECORDS) {
        full = true;
        recording = false;
        return;
    }
    auto& chunk = chunks[index / CHUNK_RECORDS];
    if (!chunk) {
        chunk = std::make_unique<SurfaceOpRecord[]>(CHUNK_RECORDS);
    }
    chunk[index % CHUNK_RECORDS] = record;
    count.store(index + 1, std::memory_order_release);
}

void SurfaceOpLog::RecordFrameEnd(u64 frame) {
    Append({
        .frame = frame,
        .value = {.frame = frame},
        .op = SurfaceOp::FrameEnd,
    });
}

SurfaceOpLog& GetSurfaceOpLog() {
    static SurfaceOpLog log;
    return log;
}

SurfaceDebugImage DecodeSurfaceForDebug(const SurfaceParams& params, std::span<const u8> data) {
    SurfaceDebugImage image{
        .width = params.width,
        .height = params.height,
    };

    const PixelFormat format = params.pixel_format;
    const u32 bpp = GetFormatBpp(format);
    const u32 bytes_per_pixel = bpp / 8;
    const bool is_texture_format = format <= PixelFormat::ETC1A4;
    const bool is_depth_format =
        format == PixelFormat::D16 || format == PixelFormat::D24 || format == PixelFormat::D24S8;
    if (!is_texture_format && !is_depth_format) {
        image.error = "Unsupported pixel format";
        return image;
    }
    if (!params.is_tiled && (bpp < 8 || format > PixelFormat::RGBA4) && !is_depth_format) {
        image.error = "Unsupported linear pixel format";
        return image;
    }
    if (static_cast<u64>(params.stride) * params.height * bpp / 8 > data.size()) {
        image.error = "Surface data is too small";
        return image;
    }

    const u32 pixel_count = params.width * params.height;
    image.rgba.resize(pixel_count * 4);
    if (is_depth_format) {
        image.depth.resize(pixel_count);
        image.depth_bits = format == PixelFormat::D16 ? 16 : 24;
        if (format == PixelFormat::D24S8) {
            image.stencil.resize(pixel_count);
        }
    }

    Pica::Texture::TextureInfo info{};
    if (is_texture_format) {
        info.width = params.width;
        info.height = params.height;
        info.format = static_cast<Pica::TexturingRegs::TextureFormat>(format);
        info.stride = Pica::Texture::CalculateTileSize(info.format) * (params.stride / 8);
    }

    for (u32 y = 0; y < params.height; y++) {
        for (u32 x = 0; x < params.width; x++) {
            const u32 index = y * params.width + x;
            Common::Vec4<u8> color{0, 0, 0, 255};

            if (is_texture_format && params.is_tiled) {
                color = Pica::Texture::LookupTexture(data.data(), x, y, info);
            } else {
                const u32 offset = params.is_tiled ? GetMortonOffset(x, y, bytes_per_pixel) +
                                                         (y & ~7u) * params.stride * bytes_per_pixel
                                                   : (y * params.stride + x) * bytes_per_pixel;
                const u8* pixel = data.data() + offset;
                switch (format) {
                case PixelFormat::RGBA8:
                    color = Common::Color::DecodeRGBA8(pixel);
                    break;
                case PixelFormat::RGB8:
                    color = Common::Color::DecodeRGB8(pixel);
                    break;
                case PixelFormat::RGB5A1:
                    color = Common::Color::DecodeRGB5A1(pixel);
                    break;
                case PixelFormat::RGB565:
                    color = Common::Color::DecodeRGB565(pixel);
                    break;
                case PixelFormat::RGBA4:
                    color = Common::Color::DecodeRGBA4(pixel);
                    break;
                case PixelFormat::D16:
                    image.depth[index] = Common::Color::DecodeD16(pixel);
                    break;
                case PixelFormat::D24:
                    image.depth[index] = Common::Color::DecodeD24(pixel);
                    break;
                case PixelFormat::D24S8: {
                    const auto depth_stencil = Common::Color::DecodeD24S8(pixel);
                    image.depth[index] = depth_stencil.x;
                    image.stencil[index] = static_cast<u8>(depth_stencil.y);
                    break;
                }
                default:
                    break;
                }
            }

            if (is_depth_format) {
                const u8 value = static_cast<u8>(image.depth[index] >> (image.depth_bits - 8));
                color = {value, value, value, 255};
            }
            std::memcpy(&image.rgba[index * 4], color.AsArray(), 4);
        }
    }

    return image;
}

} // namespace VideoCore
