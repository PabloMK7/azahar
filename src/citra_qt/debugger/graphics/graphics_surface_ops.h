// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <array>
#include <memory>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QWidget>
#include "common/common_types.h"
#include "video_core/rasterizer_cache/surface_debug.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QTimer;
class QTreeView;
class DebugWorkQueue;

namespace Core {
class System;
}

/// The records of the surface operation log. One model is shared by every surface viewer.
class SurfaceOpModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Number,
        Frame,
        Operation,
        Surface,
        Range,
        Bytes,
        SurfaceInfo,
        Details,
        ColumnCount,
    };

    enum class Category {
        Lifetime,
        Pica,
        Coherency,
        Transfers,
        Usage,
        Tools,
        Frames,
        Count,
    };

    /// Returns the shared model, creating it if no viewer holds it.
    static std::shared_ptr<SurfaceOpModel> Get();
    ~SurfaceOpModel() override;

    /// The record of a row. Valid until the log is cleared.
    const VideoCore::SurfaceOpRecord& Record(int row) const;
    void Clear();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    static QString OpName(VideoCore::SurfaceOp op);
    static QString OpDescription(VideoCore::SurfaceOp op);
    static Category OpCategory(VideoCore::SurfaceOp op);
    static QString CategoryName(Category category);
    static QString CategoryDescription(Category category);
    /// The unique ID and slot of a surface.
    static QString SurfaceText(const VideoCore::SurfaceOpSurface& surface);
    /// The format, size and layout of a surface.
    static QString SurfaceInfoText(const VideoCore::SurfaceOpSurface& surface);
    static QString RangeText(const VideoCore::SurfaceOpRecord& record);
    /// What the operation did beyond its surface and range.
    static QString DetailsText(const VideoCore::SurfaceOpRecord& record);
    /// Describes a column and lists the values it can show.
    static QString HeaderToolTip(int section);

signals:
    /// The log started or stopped recording, filled up, or got new records.
    void StateChanged();

private:
    SurfaceOpModel();
    /// Adds the rows of the records appended since the last poll.
    void Poll();

    QTimer* poll_timer;
    int rows = 0;
    bool was_recording = false;
};

class SurfaceOpFilter : public QSortFilterProxyModel {
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    /// An addres shows the operations whose range or surfaces contain it, #N the operations on
    /// the surface with unique ID N, other text is the operations whose name contains it.
    void SetText(const QString& text);
    void SetCategoryShown(SurfaceOpModel::Category category, bool shown);

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

private:
    enum class TextKind {
        None,
        Address,
        UniqueId,
        Name,
    };

    TextKind kind = TextKind::None;
    QString text;
    u64 number = 0;
    std::array<bool, static_cast<std::size_t>(SurfaceOpModel::Category::Count)> shown_categories{
        true, true, true, true, true, true, true};
};

/// Records the operations of the rasterizer cache in order, and lists them.
class SurfaceOpView : public QWidget {
    Q_OBJECT

public:
    explicit SurfaceOpView(Core::System& system, DebugWorkQueue& work_queue,
                           QWidget* parent = nullptr);
    ~SurfaceOpView() override;

public slots:
    void OnEmulationStateChanged(bool running, bool paused);

signals:
    /// Asks to select a surface in the cached surfaces tab.
    void ShowSurface(u32 index, VideoCore::SurfaceUniqueId unique_id);

private:
    void Start();
    void Stop();
    void ClearLog();
    void Save();
    void UpdateState();
    void UpdateDetails();
    const VideoCore::SurfaceOpRecord* SelectedRecord() const;
    /// The source row of the operation the given one is nested in, or -1.
    int ParentRow(int row) const;

    Core::System& system;
    DebugWorkQueue& work_queue;
    std::shared_ptr<SurfaceOpModel> model;
    SurfaceOpFilter* filter;

    QPushButton* start_button;
    QPushButton* stop_button;
    QPushButton* clear_button;
    QPushButton* save_button;
    QCheckBox* record_usage;
    QLabel* status_label;
    QLineEdit* filter_edit;
    QTreeView* table;
    QTextBrowser* details;
    QPushButton* show_surface_button;
    QPushButton* show_other_button;
    QPushButton* filter_surface_button;

    bool running = false;
    /// The table was scrolled to the end before rows were added, so it follows them.
    bool follow_end = true;
};
