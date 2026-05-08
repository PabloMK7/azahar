// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <vector>
#include <QAbstractTableModel>
#include <QImage>
#include <QSortFilterProxyModel>
#include <QWidget>
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/rasterizer_cache/surface_debug.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTextBrowser;
class QTimer;
class QTreeView;

namespace Core {
class System;
}

namespace VideoCore {
class RasterizerInterface;
}

class DebugWorkQueue;

/// Enables surface tracking on the emulation thread while a surface viewer is visible or the
/// operation log records, and disables it otherwise.
void QueueSurfaceDebugTracking(Core::System& system, DebugWorkQueue& work_queue);

class SurfaceCacheModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Index,
        State,
        Recycle,
        Address,
        End,
        Size,
        Dimensions,
        Format,
        Type,
        Levels,
        Scale,
        Samples,
        Tiled,
        Flags,
        LastUsed,
        Usage,
        Created,
        Recycled,
        Invalid,
        Dirty,
        Uploaded,
        Downloaded,
        HostMemory,
        References,
        ColumnCount,
    };

    static constexpr int SortRole = Qt::UserRole;

    explicit SurfaceCacheModel(QObject* parent = nullptr);

    void SetInfo(const VideoCore::SurfaceCacheDebugInfo& info, u64 usage_frame);
    const VideoCore::SurfaceDebugInfo* GetSurface(int row) const;
    int RowOfIndex(u32 index) const;
    u64 Frame() const {
        return frame;
    }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    static QString StateText(const VideoCore::SurfaceDebugInfo& surface);
    static QString RecycleText(const VideoCore::SurfaceDebugInfo& surface);
    static QString FlagsText(u32 flags);
    static QString UsageText(VideoCore::SurfaceUsageBits usage);
    static QString TypeText(const VideoCore::SurfaceParams& params);
    static QString HeaderToolTip(int section);

private:
    QVariant DisplayData(const VideoCore::SurfaceDebugInfo& surface, int column) const;
    QVariant SortData(const VideoCore::SurfaceDebugInfo& surface, int column) const;

    std::vector<VideoCore::SurfaceDebugInfo> surfaces;
    u64 frame = 0;
};

class SurfaceCacheFilter : public QSortFilterProxyModel {
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void SetText(const QString& text);
    void SetShowSentenced(bool show);
    void SetUsedOnly(bool used);

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

private:
    QString text;
    bool show_sentenced = true;
    bool used_only = false;
};

/// Lists every surface in the rasterizer cache, with its state and a preview.
class SurfaceCacheView : public QWidget {
    Q_OBJECT

public:
    using SurfaceAction = std::function<VideoCore::SurfaceDebugResult(
        VideoCore::RasterizerInterface&, const VideoCore::SurfaceDebugInfo&)>;

    explicit SurfaceCacheView(Core::System& system, DebugWorkQueue& work_queue,
                              QWidget* parent = nullptr);
    ~SurfaceCacheView() override;

    /// Enables surface tracking while the view is visible, and requests a refresh.
    void SetActive(bool active);

    void OnFrameStepped();

    /// Takes a snapshot while a Pica breakpoint is hit, before the caches are flushed. Called
    /// from the emulation thread.
    void CaptureAtBreakpoint();

    void OnBreakpoint(bool hit, Pica::DebugContext::Event event);

    /// Selects the surface in slot index with the unique ID unique_id in the next snapshot, which
    /// is requested if the view is active.
    void SelectSurface(u32 index, VideoCore::SurfaceUniqueId unique_id);

public slots:
    void OnEmulationStateChanged(bool running, bool paused);
    void Refresh();

signals:
    void OpenInManual(u32 address, u32 width, u32 height, VideoCore::PixelFormat format);

private:
    /// Asks the emulation thread for a snapshot at the end of a frame. If min_frames is not zero,
    /// it is only taken once that many frames have ended since since_frame.
    void RequestSnapshot(u64 since_frame, u64 min_frames);
    /// Requests the next live snapshot, once the frames set for Live have ended.
    void RefreshLive();
    /// Empties the view, so nothing of a finished run is shown.
    void ClearSnapshot();
    void OnSnapshot(VideoCore::SurfaceCacheDebugInfo info, bool before_breakpoint_flush);
    void UpdateStatus();
    void OnSelectionChanged();
    void RequestPreview();
    void OnPreview(VideoCore::SurfaceDebugImage image, u32 index);
    void UpdatePreviewImage();
    void UpdateDetails();
    void UpdateSummary();
    /// Lines of the summary about data moved between guest memory and the host GPU.
    QStringList TransferSummary() const;
    void SavePreview();
    /// Reads the selected level at its resolution scale and saves it as a PNG.
    void SaveUpscaled();
    void OnSaveUpscaled(VideoCore::SurfaceDebugImage image, u32 index, u32 level);
    void UpdateActionButtons();
    void ImportPng(bool upscaled);
    void RevertImport();
    void SaveToPack();
    /// Runs an action on the selected surface on the emulation thread, then refreshes.
    void RunSurfaceAction(SurfaceAction action);
    /// Directory where the dumper writes the textures of the title, or its pack if there is none.
    QString CustomTextureDir() const;
    const VideoCore::SurfaceDebugInfo* SelectedSurface() const;
    /// Selects the surface of pending_selection in the snapshot. Returns false if it is not
    /// there.
    bool SelectPendingSurface();

    Core::System& system;
    DebugWorkQueue& work_queue;
    SurfaceCacheModel* model;
    SurfaceCacheFilter* filter;

    QLabel* summary_label;
    QLabel* status_label;
    QLineEdit* filter_edit;
    QCheckBox* show_sentenced;
    QCheckBox* used_only;
    QCheckBox* live_refresh;
    QSpinBox* live_frames;
    QPushButton* refresh_button;
    QTreeView* table;
    QTextBrowser* details;
    std::optional<u32> details_index;

    QSpinBox* preview_level;
    QComboBox* preview_mode;
    QComboBox* preview_zoom;
    QCheckBox* preview_flip;
    QCheckBox* preview_upscaled;
    QPushButton* preview_save;
    QPushButton* save_upscaled;
    QPushButton* open_manual;
    QPushButton* import_button;
    QPushButton* import_upscaled_button;
    QPushButton* revert_button;
    QPushButton* save_pack_button;
    QLabel* preview_label;
    QLabel* preview_status;

    /// Polls for the next live snapshot while the emulation runs.
    QTimer* live_timer;

    VideoCore::SurfaceCacheDebugInfo last_info;
    u64 usage_frame = 0; ///< Frame whose surface usage the snapshot shows.
    bool has_info = false;
    std::optional<VideoCore::SurfaceDebugImage> preview;
    u32 preview_index = 0;
    /// Level and scale of the pending or last preview read.
    u32 preview_read_level = 0;
    bool preview_read_upscaled = false;
    QImage preview_image;

    std::atomic_bool active = false;
    bool running = false;
    bool at_breakpoint = false;
    Pica::DebugContext::Event breakpoint_event{};
    bool snapshot_before_flush = false;
    bool paused = false;
    bool snapshot_pending = false;
    bool preview_pending = false;
    bool save_upscaled_pending = false;
    bool action_pending = false;
    /// Another snapshot is taken when the pending one arrives.
    bool refresh_again = false;
    /// After a frame step, snapshots taken before this frame are discarded and requested again.
    std::optional<u64> wait_for_frame_after;
    /// Slot index and unique ID of the surface SelectSurface selects in the next snapshot.
    std::optional<std::pair<u32, VideoCore::SurfaceUniqueId>> pending_selection;
    int stale_retries = 0;
};
