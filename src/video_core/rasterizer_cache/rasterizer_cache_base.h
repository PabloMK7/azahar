// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <deque>
#include <functional>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/icl/interval_map.hpp>
#include <tsl/robin_map.h>

#include "video_core/rasterizer_cache/framebuffer_base.h"
#include "video_core/rasterizer_cache/sampler_params.h"
#include "video_core/rasterizer_cache/surface_debug.h"
#include "video_core/rasterizer_cache/surface_params.h"
#include "video_core/rasterizer_cache/texture_cube.h"

namespace Memory {
class MemorySystem;
}

namespace Pica {
struct RegsInternal;
struct DisplayTransferConfig;
struct MemoryFillConfig;
} // namespace Pica

namespace Pica::Texture {
struct TextureInfo;
}

namespace Settings {
enum class TextureFilter : u32;
}

namespace VideoCore {

enum class ScaleMatch {
    Exact,   ///< Only accept same res scale
    Upscale, ///< Only allow higher scale than params
    Ignore   ///< Accept every scaled res
};

enum class MatchFlags {
    Exact = 1 << 0,       ///< Surface perfectly matches params
    SubRect = 1 << 1,     ///< Surface encompasses params
    Copy = 1 << 2,        ///< Surface that can be used as a copy source
    TexCopy = 1 << 3,     ///< Surface that will match a display transfer "texture copy" parameters
    Reinterpret = 1 << 4, ///< Surface might have different pixel format.
};

DECLARE_ENUM_FLAG_OPERATORS(MatchFlags);

class CustomTexManager;
struct Material;
class RendererBase;
enum class SurfaceFlagBits : u32;

template <class T>
class RasterizerCache {
    /// Address shift for caching surfaces into a hash table
    static constexpr u64 CITRA_PAGEBITS = 18;

    using Runtime = typename T::Runtime;
    using Sampler = typename T::Sampler;
    using Surface = typename T::Surface;
    using Framebuffer = typename T::Framebuffer;
    using DebugScope = typename T::DebugScope;

    using SurfaceMap = boost::icl::interval_map<PAddr, SurfaceId, boost::icl::partial_absorber,
                                                std::less, boost::icl::inplace_plus,
                                                boost::icl::inter_section, SurfaceInterval>;

    using SurfaceRect_Tuple = std::pair<SurfaceId, Common::Rectangle<u32>>;
    using PageMap = boost::icl::interval_map<u32, int>;

public:
    explicit RasterizerCache(Memory::MemorySystem& memory, CustomTexManager& custom_tex_manager,
                             Runtime& runtime, Pica::RegsInternal& regs, RendererBase& renderer);
    ~RasterizerCache();

    /// Notify the cache that a new frame has been queued
    void TickFrame();

    /// Perform hardware accelerated texture copy according to the provided configuration
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config);

    /// Perform hardware accelerated display transfer according to the provided configuration
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config);

    /// Perform hardware accelerated memory fill according to the provided configuration
    bool AccelerateFill(const Pica::MemoryFillConfig& config);

    /// Returns a reference to the surface object assigned to surface_id
    Surface& GetSurface(SurfaceId surface_id);

    /// Returns a reference to the sampler object matching the provided configuration
    Sampler& GetSampler(const Pica::TexturingRegs::TextureConfig& config);
    Sampler& GetSampler(SamplerId sampler_id);

    /// Copy one surface's region to another
    void CopySurface(Surface& src_surface, Surface& dst_surface, SurfaceInterval copy_interval);

    /// Load a texture from 3DS memory to OpenGL and cache it (if not already cached)
    SurfaceId GetSurface(const SurfaceParams& params, ScaleMatch match_res_scale,
                         bool load_if_create, const SurfaceFlagBits& create_initial_flags = {});

    /// Attempt to find a subrect (resolution scaled) of a surface, otherwise loads a texture from
    /// 3DS memory to OpenGL and caches it (if not already cached)
    SurfaceRect_Tuple GetSurfaceSubRect(const SurfaceParams& params, ScaleMatch match_res_scale,
                                        bool load_if_create,
                                        const SurfaceFlagBits& create_initial_flags = {});

    /// Get a surface based on the texture configuration
    Surface& GetTextureSurface(const Pica::TexturingRegs::FullTextureConfig& config);
    SurfaceId GetTextureSurface(const Pica::Texture::TextureInfo& info, u32 max_level = 0);

    /// Get a texture cube based on the texture configuration
    Surface& GetTextureCube(const TextureCubeConfig& config);

    /// Get the color and depth surfaces based on the framebuffer configuration
    FramebufferHelper<T> GetFramebufferSurfaces(bool using_color_fb, bool using_depth_fb);

    /// Get a surface that matches a "texture copy" display transfer config
    SurfaceRect_Tuple GetTexCopySurface(const SurfaceParams& params);

    /// Write any cached resources overlapping the region back to memory (if dirty)
    void FlushRegion(PAddr addr, u32 size, SurfaceId flush_surface = {});

    /// Mark region as being invalidated by region_owner (nullptr if 3DS memory)
    void InvalidateRegion(PAddr addr, u32 size, SurfaceId region_owner = {});

    /// Flush all cached resources tracked by this cache manager
    void FlushAll();

    /// Clear all cached resources tracked by this cache manager
    void ClearAll(bool flush);

#ifdef ENABLE_SURFACE_VIEWER
    void SetDebugTracking(bool enabled);

    /// Returns a copy of the state of every surface in the cache
    SurfaceCacheDebugInfo GetDebugInfo() const;

    /// Reads back a level of a surface for debugging, at its resolution scale if upscaled is true.
    /// Fails if the surface at index no longer matches params or cannot be read back.
    SurfaceDebugImage ReadSurfaceForDebug(u32 index, const SurfaceParams& params, u32 level,
                                          bool upscaled);

    /// Replaces the texture of a surface with a PNG, as a custom texture does. The PNG has the
    /// size of the surface, or a larger size with its aspect ratio if upscaled is true. It is kept
    /// for the guest data of the surface until it is reverted.
    SurfaceDebugResult ImportSurfaceForDebug(u32 index, const SurfaceParams& params,
                                             const std::string& path, bool upscaled);

    /// Stops replacing a surface with an imported PNG and reloads it from guest memory.
    SurfaceDebugResult RevertSurfaceImportForDebug(u32 index, const SurfaceParams& params);

    /// Saves a level of a surface to the custom texture pack of the running title. Saves the
    /// imported PNG if there is one, and the guest texture otherwise.
    SurfaceDebugResult SaveSurfaceToPackForDebug(u32 index, const SurfaceParams& params, u32 level);
#endif

    /// Records how a surface was used in the current frame, while debug tracking is enabled
    void MarkDebugUsage([[maybe_unused]] SurfaceId surface_id,
                        [[maybe_unused]] SurfaceUsageBits usage) {
#ifdef ENABLE_SURFACE_VIEWER
        if (debug_tracking) [[unlikely]] {
            RecordDebugUsage(slot_surfaces[surface_id], usage);
        }
#endif
    }

    /// Ends the draw that GetFramebufferSurfaces began in the operation log. Called once the
    /// draw has invalidated its targets.
    void EndDebugDraw() {
#ifdef ENABLE_SURFACE_VIEWER
        if (debug_draw_scope) [[unlikely]] {
            debug_draw_scope = false;
            EndDebugScope(false);
        }
#endif
    }

private:
    /**
     * Records an operation in the operation log of the surface viewer while it records. The
     * operations recorded until the scope closes are nested in it. A lazy scope is recorded only
     * if something is recorded in it or if it is forced, so that the many calls that do nothing
     * do not fill the log.
     */
    class DebugOpScope {
    public:
#ifdef ENABLE_SURFACE_VIEWER
        DebugOpScope(RasterizerCache& cache_, SurfaceOp op, PAddr start, PAddr end,
                     Surface* surface = nullptr, Surface* other = nullptr,
                     SurfaceOpValue value = {}, bool lazy = false)
            : cache{cache_} {
            if (cache.debug_tracking) [[unlikely]] {
                open = cache.BeginDebugScope(op, start, end, surface, other, value, lazy);
            }
        }
        ~DebugOpScope() {
            if (open) [[unlikely]] {
                if (expect_success && !succeeded) {
                    cache.RecordDebugOp(SurfaceOp::NotAccelerated, 0, 0);
                }
                cache.EndDebugScope(force);
            }
        }

        void Force() {
            force = true;
        }

        void ExpectSuccess() {
            expect_success = true;
        }

        void Succeed() {
            succeeded = true;
        }

    private:
        RasterizerCache& cache;
        bool open = false;
        bool force = false;
        bool expect_success = false;
        bool succeeded = false;
#else
        DebugOpScope(RasterizerCache&, SurfaceOp, PAddr, PAddr, Surface* = nullptr,
                     Surface* = nullptr, SurfaceOpValue = {}, bool = false) {}
        void Force() {}
        void ExpectSuccess() {}
        void Succeed() {}
#endif
        DebugOpScope(const DebugOpScope&) = delete;
        DebugOpScope& operator=(const DebugOpScope&) = delete;
    };

    /// Records an operation in the operation log of the surface viewer, while it records.
    void RecordDebugOp([[maybe_unused]] SurfaceOp op, [[maybe_unused]] PAddr start,
                       [[maybe_unused]] PAddr end, [[maybe_unused]] Surface* surface = nullptr,
                       [[maybe_unused]] Surface* other = nullptr,
                       [[maybe_unused]] SurfaceOpValue value = {}) {
#ifdef ENABLE_SURFACE_VIEWER
        if (debug_tracking) [[unlikely]] {
            WriteDebugOp(op, start, end, surface, other, value);
        }
#endif
    }

    /// Counts data moved between guest memory and a host texture, and records the operation.
    /// op is Upload, CustomUpload, Download or FillDownload.
    void RecordDebugTransfer([[maybe_unused]] SurfaceOp op, [[maybe_unused]] Surface& surface,
                             [[maybe_unused]] PAddr start, [[maybe_unused]] PAddr end,
                             [[maybe_unused]] u64 bytes) {
#ifdef ENABLE_SURFACE_VIEWER
        if (debug_tracking) [[unlikely]] {
            CountDebugTransfer(op, surface, start, end, bytes);
        }
#endif
    }

    /// Iterate over all page indices in a range
    template <typename Func>
    void ForEachPage(PAddr addr, std::size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> CITRA_PAGEBITS;
        for (u64 page = addr >> CITRA_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Iterates over all the surfaces in a region calling func
    template <typename Func>
    void ForEachSurfaceInRegion(PAddr addr, std::size_t size, Func&& func);

    /// Get the best surface match (and its match type) for the given flags
    template <MatchFlags find_flags>
    SurfaceId FindMatch(const SurfaceParams& params, ScaleMatch match_scale_type,
                        std::optional<SurfaceInterval> validate_interval = std::nullopt);

    /// Queues a surface for deletion. It can be recycled until the garbage collector deletes it.
    void SentenceSurface(SurfaceId surface_id);

    /// Unregisters sentenced surfaces that have surpassed the destruction threshold.
    void RunGarbageCollector();

    /// Removes any framebuffers that reference the provided surface_id.
    void RemoveFramebuffers(SurfaceId surface_id);

    /// Removes any references of the provided surface id from cached texture cubes.
    void RemoveTextureCubeFace(SurfaceId surface_id);

    /// Computes the hash of the provided texture data.
    u64 ComputeHash(const SurfaceParams& load_info, std::span<u8> upload_data);

    u64 ComputeHash(const SurfaceParams& load_info, std::span<u8> upload_data, bool use_new_hash);

    void ReplaceWithMaterial(SurfaceId surface_id, Material* material, u32 level,
                             bool generate_mipmaps);

#ifdef ENABLE_SURFACE_VIEWER
    /// Finds the surface at index for a debugging action. Sets error if it changed since the
    /// snapshot that gave params.
    Surface* FindSurfaceForDebug(u32 index, const SurfaceParams& params, std::string& error);

    /// Reads back a level of an upscaled color surface at its resolution scale
    SurfaceDebugImage ReadScaledSurfaceForDebug(Surface& surface, u32 level);
#endif

    /// Update surface's texture for given region when necessary
    void ValidateSurface(SurfaceId surface, PAddr addr, u32 size);

    /// Copies pixel data in interval from the guest VRAM to the host GPU surface
    void UploadSurface(Surface& surface, SurfaceInterval interval);

    /// Uploads a custom texture identified with hash to the target surface
    bool UploadCustomSurface(SurfaceId surface_id, SurfaceInterval interval);

    /// Copies pixel data in interval from the host GPU surface to the guest VRAM
    void DownloadSurface(Surface& surface, SurfaceInterval interval);

    /// Downloads a fill surface to guest VRAM
    void DownloadFillSurface(Surface& surface, SurfaceInterval interval);

    /// Attempt to find a reinterpretable surface in the cache and use it to copy for validation
    bool ValidateByReinterpretation(Surface& surface, SurfaceParams params,
                                    const SurfaceInterval& interval);

    /// Create a new surface
    SurfaceId CreateSurface(const SurfaceParams& params, const SurfaceFlagBits& initial_flags = {});

    /// Register surface into the cache
    void RegisterSurface(SurfaceId surface);

    /// Remove surface from the cache
    void UnregisterSurface(SurfaceId surface);

    /// Unregisters all surfaces from the cache
    void UnregisterAll();

    /// Increase/decrease the number of surface in pages touching the specified region
    void UpdatePagesCachedCount(PAddr addr, u32 size, int delta);

    /// Records how a surface was used in the current frame, while debug tracking is enabled
    void MarkDebugUsage([[maybe_unused]] Surface& surface,
                        [[maybe_unused]] SurfaceUsageBits usage) {
#ifdef ENABLE_SURFACE_VIEWER
        if (debug_tracking) [[unlikely]] {
            RecordDebugUsage(surface, usage);
        }
#endif
    }

#ifdef ENABLE_SURFACE_VIEWER
    void RecordDebugUsage(Surface& surface, SurfaceUsageBits usage);

    /// Describes a surface for the operation log, giving it a unique ID if it has none
    SurfaceOpSurface DescribeForDebugOp(Surface* surface);

    void WriteOpenDebugScopes(SurfaceOpLog& log);

    /// Appends an operation to the log, after the scopes it is nested in
    void WriteDebugOp(SurfaceOp op, PAddr start, PAddr end, Surface* surface, Surface* other,
                      SurfaceOpValue value);

    /// Opens a scope of the operation log. Returns false if the log is not recording.
    bool BeginDebugScope(SurfaceOp op, PAddr start, PAddr end, Surface* surface, Surface* other,
                         SurfaceOpValue value, bool lazy);

    /// Closes the innermost scope, recording it if it is forced and was not recorded yet
    void EndDebugScope(bool force);

    void CountDebugTransfer(SurfaceOp op, Surface& surface, PAddr start, PAddr end, u64 bytes);

    /// Returns true for the placeholder surfaces bound in place of missing textures
    static bool IsNullSurface(SurfaceId surface_id) {
        return surface_id == NULL_SURFACE_ID || surface_id == NULL_SURFACE_CUBE_ID;
    }
#endif

private:
    /// Frames a sentenced surface is kept for recycling after the runtime is done with it.
    static constexpr u64 RECYCLE_FRAMES = 9;

    struct SentencedSurface {
        SurfaceId surface_id;
        u64 resource_tick; ///< Runtime resource tick when the surface was sentenced.
        u64 frame;         ///< Renderer frame when the surface was sentenced.
    };

    Memory::MemorySystem& memory;
    CustomTexManager& custom_tex_manager;
    Runtime& runtime;
    Pica::RegsInternal& regs;
    RendererBase& renderer;
    std::unordered_map<TextureCubeConfig, TextureCube> texture_cube_cache;
    tsl::robin_pg_map<u64, std::vector<SurfaceId>, Common::IdentityHash<u64>> page_table;
    std::unordered_map<FramebufferParams, FramebufferId> framebuffers;
    std::unordered_map<SamplerParams, SamplerId> samplers;
    std::list<SentencedSurface> sentenced;
    Common::SlotVector<Surface> slot_surfaces;
    Common::SlotVector<Sampler> slot_samplers;
    Common::SlotVector<Framebuffer> slot_framebuffers;
    SurfaceMap dirty_regions;
    PageMap cached_pages;
    u32 resolution_scale_factor;
    u8 sample_count;
    FramebufferParams fb_params;
    Settings::TextureFilter filter;
    bool dump_textures;
    bool use_custom_textures;
#ifdef ENABLE_SURFACE_VIEWER
    struct OpenDebugOp {
        SurfaceOpRecord record;
        bool written;
    };

    bool debug_tracking = false;
    SurfaceDebugCounters debug_counters;
    u64 debug_tracking_frame = 0;
    SurfaceUniqueId debug_next_unique_id = 1;
    SurfaceTransferStats debug_transfers;
    /// Stats of the last frames that moved data, oldest first, the current frame last.
    std::deque<SurfaceTransferStats> debug_transfer_history;
    std::vector<OpenDebugOp> debug_scopes; ///< Open scopes of the operation log, outermost first.
    bool debug_draw_scope = false;
#endif
};

} // namespace VideoCore
