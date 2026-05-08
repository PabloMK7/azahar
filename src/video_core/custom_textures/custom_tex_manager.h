// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "common/thread_worker.h"
#include "video_core/custom_textures/material.h"
#include "video_core/rasterizer_interface.h"

namespace Core {
class System;
}

namespace FileUtil {
struct FSTEntry;
}

namespace VideoCore {

class SurfaceParams;

struct AsyncUpload {
    const Material* material;
    std::function<bool()> func;
};

#ifdef ENABLE_SURFACE_VIEWER
/// A texture loaded from a PNG by the debugging tool, before it is given to the manager.
struct ImportedMaterial {
    std::unique_ptr<CustomTexture> texture;
    Material material;
};

/// The options of the pack in the load directory of a title.
struct CustomPackOptions {
    bool has_config = false;   ///< The pack has a pack.json.
    bool needs_config = false; ///< The pack is new, so saving to it writes the pack.json template.
    bool skip_mipmap = true;
    bool flip_png_files = true;
    bool use_new_hash = false;
};
#endif

class CustomTexManager {
public:
    explicit CustomTexManager(Core::System& system);
    ~CustomTexManager();

    /// Processes queued texture uploads
    void TickFrame();

    /// Searches the load directory assigned to program_id for any custom textures and loads them
    void FindCustomTextures();

    /// Reads the pack configuration file
    bool ReadConfig(u64 title_id, bool options_only = false);

    /// Saves the pack configuration file template to the dump directory if it doesn't exist.
    void PrepareDumping(u64 title_id);

    /// Preloads all registered custom textures
    void PreloadTextures(const std::atomic_bool& stop_run,
                         const VideoCore::DiskResourceLoadCallback& callback);

    /// Saves the provided pixel data described by params to disk as png
    void DumpTexture(const SurfaceParams& params, u32 level, std::span<u8> data, u64 data_hash);

    /// Returns the material assigned to the provided data hash
    Material* GetMaterial(u64 data_hash);

    /// Decodes the textures in material to a consumable format and uploads it.
    bool Decode(Material* material, std::function<bool()>&& upload);

    /// True when mipmap uploads should be skipped (legacy packs only)
    bool SkipMipmaps() const noexcept {
        return skip_mipmap;
    }

    /// Returns true if the pack uses the new hashing method.
    bool UseNewHash() const noexcept {
        return use_new_hash;
    }

#ifdef ENABLE_SURFACE_VIEWER
    u64 GetProgramId() const;

    /// Loads a PNG for a debugging tool. Returns null and sets error if it cannot be read.
    std::unique_ptr<ImportedMaterial> LoadImportedMaterial(const std::string& path,
                                                           std::string& error);

    /// Takes an imported material and uses it in place of the guest data with hash. The material
    /// stays allocated until the manager is destroyed, as surfaces may still use it.
    Material* AddImportedMaterial(u64 data_hash, std::unique_ptr<ImportedMaterial> imported);

    void RemoveImportedMaterial(const Material* material);

    void ClearImportedMaterials();

    /// Returns the imported material that replaces the guest data with hash, if any.
    Material* FindImportedMaterial(u64 data_hash) const {
        if (imported_map.empty()) {
            return nullptr;
        }
        const auto it = imported_map.find(data_hash);
        return it != imported_map.end() ? it->second : nullptr;
    }

    bool HasImportedMaterials() const noexcept {
        return !imported_map.empty();
    }

    bool IsImportedMaterial(const Material* material) const;

    /// Reads the options of the pack in the load directory of the running title.
    std::optional<CustomPackOptions> ReadPackOptions(std::string& error) const;

    /// Saves an RGBA8 texture, in the row order of a decoded texture, to the pack in the load
    /// directory of the running title with the name the dumper gives it. Returns the path.
    std::optional<std::string> SaveToPack(const SurfaceParams& params, u32 level, u64 data_hash,
                                          std::span<const u8> rgba, u32 width, u32 height,
                                          const CustomPackOptions& options, std::string& error);
#else
    /// Without the surface viewer, nothing imports materials.
    Material* FindImportedMaterial([[maybe_unused]] u64 data_hash) const {
        return nullptr;
    }

    bool HasImportedMaterials() const noexcept {
        return false;
    }
#endif

private:
    /// Parses the custom texture filename (hash, material type, etc).
    bool ParseFilename(const FileUtil::FSTEntry& file, CustomTexture* texture);

    /// Returns a vector of all custom texture files.
    std::vector<FileUtil::FSTEntry> GetTextures(u64 title_id);

    /// Creates the thread workers.
    void CreateWorkers();

private:
    Core::System& system;
    Frontend::ImageInterface& image_interface;
    std::unordered_set<u64> dumped_textures;
    std::unordered_map<u64, std::unique_ptr<Material>> material_map;
    std::unordered_map<std::string, std::vector<u64>> path_to_hash_map;
    std::vector<std::unique_ptr<CustomTexture>> custom_textures;
    std::list<AsyncUpload> async_uploads;
#ifdef ENABLE_SURFACE_VIEWER
    std::vector<std::unique_ptr<ImportedMaterial>> imported_materials;
    std::unordered_map<u64, Material*> imported_map;
#endif
    std::unique_ptr<Common::ThreadWorker> workers;
    bool textures_loaded{false};
    bool async_custom_loading{true};
    bool skip_mipmap{false};
    bool flip_png_files{true};
    bool use_new_hash{true};
};

} // namespace VideoCore
