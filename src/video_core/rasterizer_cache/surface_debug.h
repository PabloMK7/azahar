// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "video_core/rasterizer_cache/surface_params.h"

namespace VideoCore {

/**
 * Number the surface viewer gives a surface, never reused by the same rasterizer cache. It is not
 * a SurfaceId, which is the slot of the surface in the cache and is reused once the surface is
 * deleted, so a later surface can have the SurfaceId of an earlier one but not its
 * SurfaceUniqueId. Zero means the surface has none yet.
 */
using SurfaceUniqueId = u32;

/// How a surface was used during a frame. Only tracked while surface debugging is enabled.
enum class SurfaceUsageBits : u32 {
    ColorTarget = 1 << 0,    ///< Bound as the color buffer of a draw.
    DepthTarget = 1 << 1,    ///< Bound as the depth/stencil buffer of a draw.
    Texture = 1 << 2,        ///< Sampled as a texture or texture cube.
    TransferSource = 1 << 3, ///< Source of a display transfer or texture copy.
    TransferDest = 1 << 4,   ///< Destination of a display transfer or texture copy.
    Fill = 1 << 5,           ///< Created by a memory fill.
    CopySource = 1 << 6,     ///< Copied from to validate another surface.
    CopyDest = 1 << 7,       ///< Validated by a copy from another surface.
    Upload = 1 << 8,         ///< Loaded from guest memory.
    Download = 1 << 9,       ///< Flushed to guest memory.
    Display = 1 << 10,       ///< Presented on a screen.
};
DECLARE_ENUM_FLAG_OPERATORS(SurfaceUsageBits);

/// Debug information that the rasterizer cache keeps in every surface.
struct SurfaceDebugTracking {
    u64 created_frame = 0;         ///< Frame when the surface was created or last recycled.
    bool creation_tracked = false; ///< Created or recycled while tracking was enabled.
    u64 last_used_frame = 0;       ///< Last frame the surface was used in, if last_usage is set.
    SurfaceUsageBits last_usage{}; ///< How the surface was used in last_used_frame.
    u32 recycle_count = 0;         ///< Times the surface was recycled from the deletion queue.
    SurfaceUniqueId unique_id = 0; ///< Unique ID given when the surface was first seen.
    u64 uploaded_bytes = 0;        ///< Bytes uploaded to the host texture since creation.
    u64 downloaded_bytes = 0;      ///< Bytes flushed to guest memory since creation.
};

/// Data moved between guest memory and host textures. Only counted while tracking is enabled.
struct SurfaceTransferStats {
    u64 frame = 0;        ///< Frame the stats belong to, for per-frame stats.
    u64 upload_bytes = 0; ///< Guest textures decoded and uploaded to the host GPU.
    u64 uploads = 0;
    u64 custom_upload_bytes = 0; ///< Custom textures uploaded to the host GPU.
    u64 custom_uploads = 0;
    u64 download_bytes = 0; ///< Host textures read back from the host GPU.
    u64 downloads = 0;
    u64 fill_bytes = 0; ///< Fill surfaces written to guest memory by the CPU.
    u64 fill_downloads = 0;

    u64 HostUploadBytes() const {
        return upload_bytes + custom_upload_bytes;
    }

    void Add(const SurfaceTransferStats& other) {
        upload_bytes += other.upload_bytes;
        uploads += other.uploads;
        custom_upload_bytes += other.custom_upload_bytes;
        custom_uploads += other.custom_uploads;
        download_bytes += other.download_bytes;
        downloads += other.downloads;
        fill_bytes += other.fill_bytes;
        fill_downloads += other.fill_downloads;
    }
};

/// A copy of the state of one surface in the rasterizer cache.
struct SurfaceDebugInfo {
    u32 index = 0; ///< Slot index of the surface in the cache.
    SurfaceParams params{};
    u32 flags = 0; ///< SurfaceFlagBits.
    SurfaceDebugTracking
        usage{}; ///< Default contructed for surfaces not seen while tracking was enabled.

    bool sentenced = false;          ///< Is in the deletion queue.
    u64 sentenced_frame = 0;         ///< Frame when the surface was sentenced.
    u64 sentenced_resource_tick = 0; ///< Runtime resource tick when the surface was sentenced.
    bool gpu_in_use = false;       ///< The GPU may still use the surface, it cannot be deleted yet.
    bool recyclable = false;       ///< A new surface with matching parameters can reuse it.
    u64 frames_until_deletion = 0; ///< Zero once the garbage collector may delete it.

    bool fully_invalid = false;
    u32 invalid_bytes = 0;
    std::vector<std::pair<PAddr, PAddr>> invalid_regions;
    u32 dirty_bytes = 0; ///< Bytes of guest memory this surface owns and has not flushed.
    std::vector<std::pair<PAddr, PAddr>> dirty_regions;
    u64 modification_tick = 0;

    u32 fill_size = 0;
    std::array<u8, 4> fill_data{};

    bool has_material = false;
    u64 material_hash = 0;
    u32 material_width = 0;
    u32 material_height = 0;
    bool has_normal_map = false;
    bool material_imported = false; ///< The custom texture was imported by a debugging tool.
    std::string material_path;      ///< File of the custom texture.

    u32 framebuffer_refs = 0; ///< Cached framebuffers that use the surface.
    u32 cube_refs = 0; ///< Cached texture cubes that use the surface, as a face or as the cube.

    u64 host_bytes = 0; ///< Estimated host memory used by the texture.
};

/// Counters of surface lifetime events, kept while tracking is enabled.
struct SurfaceDebugCounters {
    u64 created = 0;       ///< Surfaces allocated, fills excluded.
    u64 recycled = 0;      ///< Surfaces reused from the deletion queue.
    u64 sentenced = 0;     ///< Surfaces moved to the deletion queue.
    u64 deleted = 0;       ///< Surfaces deleted by the garbage collector.
    u64 fills_created = 0; ///< Fill surfaces created by memory fills.
    u64 fills_erased = 0;  ///< Fill surfaces erased. They skip the deletion queue.
};

/// A copy of the state of the rasterizer cache.
struct SurfaceCacheDebugInfo {
    u64 frame = 0;
    u64 program_id = 0; ///< Program whose custom textures the cache uses.
    u64 recycle_frames = 0;
    u64 resource_tick = 0;
    u64 resource_free_tick = 0;
    u64 resource_recycle_tick = 0;
    bool tracking = false;
    u64 tracking_frame = 0; ///< Frame when tracking was enabled.
    SurfaceDebugCounters counters{};
    SurfaceTransferStats transfers{}; ///< Totals since tracking was enabled.
    /// Stats of the frames since frame - TRANSFER_HISTORY_FRAMES that moved data, oldest first.
    std::vector<SurfaceTransferStats> transfer_history;
    std::vector<SurfaceDebugInfo> surfaces;
};

/// Frames of transfer stats kept for the snapshots.
constexpr u64 TRANSFER_HISTORY_FRAMES = 60;

/**
 * A surface level read back for debugging. Pixels are in guest memory order, the first row is
 * the first one in memory. Depth surfaces also give the raw depth (and stencil) of each pixel.
 */
struct SurfaceDebugImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;             ///< RGBA8, empty if the read failed.
    std::vector<u32> depth;           ///< Raw depth values, for depth formats.
    u32 depth_bits = 0;               ///< Bits in each depth value.
    std::vector<u8> stencil;          ///< Stencil values, for D24S8.
    bool from_guest_memory = false;   ///< Decoded from guest memory, as the GPU copy cannot be.
    bool from_custom_texture = false; ///< The custom texture that replaces the surface.
    u32 scale = 1;                    ///< Resolution scale the image was read at.
    std::string native_reason;        ///< Why an upscaled read gave the native resolution.
    std::string error;                ///< Why the read failed.
};

/// The outcome of a debugging action on a surface.
struct SurfaceDebugResult {
    std::string error; ///< Why it failed, empty if it succeeded.
};

/// An operation of the rasterizer cache, recorded by debugging tools.
enum class SurfaceOp : u8 {
    // Surface lifetime
    Create,        ///< A surface was allocated.
    Recycle,       ///< A sentenced surface with the same parameters was reused.
    Register,      ///< A surface was added to the page table, where lookups find it.
    Unregister,    ///< A surface was removed from the page table.
    Sentence,      ///< A surface was queued for deletion.
    Delete,        ///< The garbage collector deleted a sentenced surface.
    EraseFill,     ///< A fill surface was erased. Fills skip the deletion queue.
    ScaleUp,       ///< The host texture was recreated with a larger scale or sample count.
    UnregisterAll, ///< Every surface was unregistered, after flushing them.
    ClearAll,      ///< The cache remover every surface without flushing them.

    // Pica operations
    MemoryFill, ///< A memory fill, which creates a fill surface.
    DisplayTransfer,
    TextureCopy,
    NotAccelerated, ///< The operation fell back to the software implementation.

    // Coherency with guest memory
    Flush,       ///< Dirty surfaces in a range were written back to guest memory.
    Invalidate,  ///< A range changed, written by a surface (its owner) or in guest memory.
    MarkInvalid, ///< Part of a surface became invalid.

    // Validation and data transfers
    Validate,        ///< Invalid parts of a surface were made valid.
    Copy,            ///< Copy from another surface.
    Blit,            ///< Scaled copy from another surface.
    Clear,           ///< Clear with the value of a fill surface.
    Reinterpret,     ///< Copy from a surface of another format.
    Upload,          ///< Guest memory decoded and uploaded to the host texture.
    CustomMatch,     ///< A custom texture matched the guest data. It is uploaded once decoded.
    CustomUpload,    ///< A custom texture replaced the host texture.
    GenerateMipmaps, ///< Mipmaps were generated from the base level.
    Download,        ///< The host texture was read back and encoded to guest memory.
    FillDownload,    ///< A fill surface was written to guest memory by the CPU.
    TemporaryCopy,   ///< Copy to a temporary texture for a mipmap chain the cache cannot hold.
    CubeFaceCopy,    ///< A face was copied to a texture cube.

    // Usage, recorded only when asked
    Draw, ///< A draw with a color and/or a depth target.
    Texture,
    Display, ///< A surface was presented on a screen.

    // Debugging tools
    Import, ///< A debugging tool replaced a surface with an image.
    Revert, ///< A debugging tool removed an imported image.

    FrameEnd, ///< A frame (a VBlank) ended.
    Count,
};

/// A surface as recorded in a SurfaceOpRecord.
struct SurfaceOpSurface {
    static constexpr u32 NoIndex = 0xFFFFFFFF;

    u32 index = NoIndex;           ///< Slot index, NoIndex if there is no surface.
    SurfaceUniqueId unique_id = 0; ///< SurfaceDebugTracking::unique_id.
    PAddr addr = 0;
    u32 size = 0;
    u16 width = 0;
    u16 height = 0;
    static constexpr u8 InvalidFormat = 0xFF;

    // The enums are stored in bytes to keep records small
    u8 format = InvalidFormat;
    u8 type = static_cast<u8>(SurfaceType::Invalid);
    u8 texture_type = static_cast<u8>(TextureType::Texture2D);
    u8 res_scale = 1;
    u8 levels = 1;
    bool is_tiled = false;

    bool IsValid() const {
        return index != NoIndex;
    }
    PixelFormat Format() const {
        return format == InvalidFormat ? PixelFormat::Invalid : static_cast<PixelFormat>(format);
    }
    SurfaceType Type() const {
        return static_cast<SurfaceType>(type);
    }
    TextureType GetTextureType() const {
        return static_cast<TextureType>(texture_type);
    }
};

/// Used by SurfaceOpRecord, which member of the enum is relevant depends on the SurfaceOp.
union SurfaceOpValue {
    u64 bytes;
    u64 hash;
    u32 fill_value;
    PAddr output_addr;
    u32 scale;
    u32 face;
    u64 frame;
};

struct SurfaceOpRecord {
    u64 frame = 0;
    SurfaceOpValue value{};
    PAddr start = 0; ///< Guest memory range of the operation.
    PAddr end = 0;
    SurfaceOpSurface surface{}; ///< The surface operated on, the destination of copies.
    SurfaceOpSurface other{};   ///< The source of copies, the depth target of draws.
    SurfaceOp op = SurfaceOp::Count;
    u8 depth = 0; ///< Nesting level. An operation is part of the previous one with a lower depth.
};

#ifdef ENABLE_SURFACE_VIEWER
/**
 * Operations of the rasterizer cache recorded for debugging. Records are appended by the
 * emulation thread and read without locking by one other thread, which must not read past Size().
 */
class SurfaceOpLog {
public:
    static constexpr std::size_t CHUNK_RECORDS = 1 << 16;
    static constexpr std::size_t MAX_CHUNKS = 32;
    static constexpr std::size_t MAX_RECORDS = CHUNK_RECORDS * MAX_CHUNKS;

    bool IsRecording() const {
        return recording.load(std::memory_order_relaxed);
    }
    /// Whether draws, texture binds and presentations are recorded too.
    bool RecordsUsage() const {
        return record_usage.load(std::memory_order_relaxed);
    }

    bool IsFull() const {
        return full.load(std::memory_order_relaxed);
    }

    void Start(bool usage);
    void Stop();
    void Clear();

    void Append(const SurfaceOpRecord& record);
    void RecordFrameEnd(u64 frame);

    std::size_t Size() const {
        return count.load(std::memory_order_acquire);
    }
    const SurfaceOpRecord& At(std::size_t index) const {
        return chunks[index / CHUNK_RECORDS][index % CHUNK_RECORDS];
    }

private:
    std::mutex mutex;
    std::array<std::unique_ptr<SurfaceOpRecord[]>, MAX_CHUNKS> chunks;
    std::atomic<std::size_t> count{};
    std::atomic_bool recording{};
    std::atomic_bool record_usage{};
    std::atomic_bool full{};
};

/// The log of the surface viewer, shared by every renderer.
SurfaceOpLog& GetSurfaceOpLog();

/// Decodes a single level of a surface in guest format to a debug image.
SurfaceDebugImage DecodeSurfaceForDebug(const SurfaceParams& level_params,
                                        std::span<const u8> data);
#endif

} // namespace VideoCore
