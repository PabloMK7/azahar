// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <QBoxLayout>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QTreeView>
#include "citra_qt/debugger/graphics/graphics_surface_cache.h"
#include "citra_qt/debugger/graphics/graphics_surface_ops.h"
#include "core/core.h"

using VideoCore::SurfaceOp;
using VideoCore::SurfaceOpLog;
using VideoCore::SurfaceOpRecord;
using VideoCore::SurfaceOpSurface;

namespace {

/// How often the model looks for new records.
constexpr int POLL_INTERVAL_MS = 100;
/// Records searched backwards for the operation a record is nested in.
constexpr int PARENT_SEARCH_LIMIT = 100000;

static QString Hex(u64 value) {
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
}

static QString Bytes(u64 bytes) {
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
    }
    if (bytes >= 1024) {
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

static bool IsTransfer(SurfaceOp op) {
    return op == SurfaceOp::Upload || op == SurfaceOp::CustomUpload || op == SurfaceOp::Download ||
           op == SurfaceOp::FillDownload;
}

/// Ops where the other surface is the source of the data
bool HasSource(SurfaceOp op) {
    switch (op) {
    case SurfaceOp::Copy:
    case SurfaceOp::Blit:
    case SurfaceOp::Clear:
    case SurfaceOp::Reinterpret:
    case SurfaceOp::TemporaryCopy:
    case SurfaceOp::CubeFaceCopy:
        return true;
    default:
        return false;
    }
}

bool Contains(const SurfaceOpSurface& surface, u64 address) {
    return surface.IsValid() && address >= surface.addr && address < surface.addr + surface.size;
}

std::weak_ptr<SurfaceOpModel> shared_model;

} // Anonymous namespace

std::shared_ptr<SurfaceOpModel> SurfaceOpModel::Get() {
    auto model = shared_model.lock();
    if (!model) {
        model = std::shared_ptr<SurfaceOpModel>(new SurfaceOpModel);
        shared_model = model;
    }
    return model;
}

SurfaceOpModel::SurfaceOpModel() {
    poll_timer = new QTimer(this);
    poll_timer->setInterval(POLL_INTERVAL_MS);
    connect(poll_timer, &QTimer::timeout, this, &SurfaceOpModel::Poll);
    poll_timer->start();
    Poll();
}

SurfaceOpModel::~SurfaceOpModel() = default;

void SurfaceOpModel::Poll() {
    const SurfaceOpLog& log = VideoCore::GetSurfaceOpLog();
    const int size = static_cast<int>(log.Size());
    const bool recording = log.IsRecording();
    const bool changed = size != rows || recording != was_recording;
    if (size > rows) {
        beginInsertRows({}, rows, size - 1);
        rows = size;
        endInsertRows();
    }
    was_recording = recording;
    if (changed) {
        emit StateChanged();
    }
}

void SurfaceOpModel::Clear() {
    beginResetModel();
    VideoCore::GetSurfaceOpLog().Clear();
    rows = 0;
    endResetModel();
    emit StateChanged();
}

const SurfaceOpRecord& SurfaceOpModel::Record(int row) const {
    return VideoCore::GetSurfaceOpLog().At(static_cast<std::size_t>(row));
}

int SurfaceOpModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows;
}

int SurfaceOpModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant SurfaceOpModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows) {
        return {};
    }
    const SurfaceOpRecord& record = Record(index.row());
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Number:
            return index.row();
        case Frame:
            return qulonglong{record.frame};
        case Operation:
            return QString{QString(record.depth * 3, QLatin1Char(' ')) + OpName(record.op)};
        case Surface:
            return SurfaceText(record.surface);
        case Range:
            return RangeText(record);
        case Bytes:
            return IsTransfer(record.op) ? ::Bytes(record.value.bytes) : QString{};
        case SurfaceInfo:
            return SurfaceInfoText(record.surface);
        case Details:
            return DetailsText(record);
        default:
            return {};
        }
    case Qt::ToolTipRole:
        if (index.column() == Operation) {
            return OpDescription(record.op);
        }
        return {};
    case Qt::FontRole:
        if (record.op == SurfaceOp::FrameEnd) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return {};
    case Qt::ForegroundRole:
        if (record.op == SurfaceOp::NotAccelerated) {
            return QColor(Qt::red);
        }
        return {};
    default:
        return {};
    }
}

QVariant SurfaceOpModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) {
        return {};
    }
    if (role == Qt::ToolTipRole) {
        return HeaderToolTip(section);
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case Number:
        return QStringLiteral("#");
    case Frame:
        return QStringLiteral("Frame");
    case Operation:
        return QStringLiteral("Operation");
    case Surface:
        return QStringLiteral("Surface");
    case Range:
        return QStringLiteral("Range");
    case Bytes:
        return QStringLiteral("Bytes");
    case SurfaceInfo:
        return QStringLiteral("Surface format");
    case Details:
        return QStringLiteral("Details");
    default:
        return {};
    }
}

QString SurfaceOpModel::HeaderToolTip(int section) {
    switch (section) {
    case Number:
        return QStringLiteral("Order of the operation in the log, bigger means later.");
    case Frame:
        return QStringLiteral("Frame (VBlank) where the operation happened.");
    case Operation:
        return QStringLiteral(
            "What the rasterizer cache did. Indented operations were done as part of the "
            "operation above them with less indentation. Hover an entry for its meaning.");
    case Surface:
        return QStringLiteral(
            "The surface operated on, as #ID (slot N). Unique IDs are not reused while the "
            "surface viewer tracks, and slots are reused once a surface is deleted. For "
            "copies it is the destination, for invalidations the surface that wrote the "
            "range.");
    case Range:
        return QStringLiteral("Guest memory the operation covers.");
    case Bytes:
        return QStringLiteral("Data moved between guest memory and the host GPU.");
    case SurfaceInfo:
        return QStringLiteral("Format, size, layout and resolution scale of the surface.");
    case Details:
        return QStringLiteral(
            "The other surface of the operation (copy source, depth target), and values "
            "such as hashes, fill values and destination addresses.");
    default:
        return {};
    }
}

QString SurfaceOpModel::OpName(SurfaceOp op) {
    switch (op) {
    case SurfaceOp::Create:
        return QStringLiteral("Create");
    case SurfaceOp::Recycle:
        return QStringLiteral("Recycle");
    case SurfaceOp::Register:
        return QStringLiteral("Register");
    case SurfaceOp::Unregister:
        return QStringLiteral("Unregister");
    case SurfaceOp::Sentence:
        return QStringLiteral("Sentence");
    case SurfaceOp::Delete:
        return QStringLiteral("Delete");
    case SurfaceOp::EraseFill:
        return QStringLiteral("Erase fill");
    case SurfaceOp::ScaleUp:
        return QStringLiteral("Scale up");
    case SurfaceOp::UnregisterAll:
        return QStringLiteral("Unregister all");
    case SurfaceOp::ClearAll:
        return QStringLiteral("Clear all");
    case SurfaceOp::MemoryFill:
        return QStringLiteral("Memory fill");
    case SurfaceOp::DisplayTransfer:
        return QStringLiteral("Display transfer");
    case SurfaceOp::TextureCopy:
        return QStringLiteral("Texture copy");
    case SurfaceOp::NotAccelerated:
        return QStringLiteral("Not accelerated");
    case SurfaceOp::Flush:
        return QStringLiteral("Flush");
    case SurfaceOp::Invalidate:
        return QStringLiteral("Invalidate");
    case SurfaceOp::MarkInvalid:
        return QStringLiteral("Mark invalid");
    case SurfaceOp::Validate:
        return QStringLiteral("Validate");
    case SurfaceOp::Copy:
        return QStringLiteral("Copy");
    case SurfaceOp::Blit:
        return QStringLiteral("Blit");
    case SurfaceOp::Clear:
        return QStringLiteral("Clear");
    case SurfaceOp::Reinterpret:
        return QStringLiteral("Reinterpret");
    case SurfaceOp::Upload:
        return QStringLiteral("Upload");
    case SurfaceOp::CustomMatch:
        return QStringLiteral("Custom texture match");
    case SurfaceOp::CustomUpload:
        return QStringLiteral("Custom texture upload");
    case SurfaceOp::GenerateMipmaps:
        return QStringLiteral("Generate mipmaps");
    case SurfaceOp::Download:
        return QStringLiteral("Download");
    case SurfaceOp::FillDownload:
        return QStringLiteral("Fill download");
    case SurfaceOp::TemporaryCopy:
        return QStringLiteral("Temporary copy");
    case SurfaceOp::CubeFaceCopy:
        return QStringLiteral("Cube face copy");
    case SurfaceOp::Draw:
        return QStringLiteral("Draw");
    case SurfaceOp::Texture:
        return QStringLiteral("Texture");
    case SurfaceOp::Display:
        return QStringLiteral("Display");
    case SurfaceOp::Import:
        return QStringLiteral("Import");
    case SurfaceOp::Revert:
        return QStringLiteral("Revert import");
    case SurfaceOp::FrameEnd:
        return QStringLiteral("Frame end");
    default:
        return QStringLiteral("Unknown");
    }
}

QString SurfaceOpModel::OpDescription(SurfaceOp op) {
    switch (op) {
    case SurfaceOp::Create:
        return QStringLiteral("A surface was allocated, with a new host texture.");
    case SurfaceOp::Recycle:
        return QStringLiteral(
            "A sentenced surface with the same parameters was reused for a new surface, "
            "keeping its host texture.");
    case SurfaceOp::Register:
        return QStringLiteral(
            "The surface was added to the page table, where draws, transfers and flushes "
            "of its addresses find it.");
    case SurfaceOp::Unregister:
        return QStringLiteral(
            "The surface was removed from the page table, because it became fully "
            "invalid, the CPU wrote to it, or the cache was reset.");
    case SurfaceOp::Sentence:
        return QStringLiteral(
            "The surface was queued for deletion. It can be recycled until the garbage "
            "collector deletes it.");
    case SurfaceOp::Delete:
        return QStringLiteral("The garbage collector deleted the surface and its host texture.");
    case SurfaceOp::EraseFill:
        return QStringLiteral("A fill surface was erased. Fills skip the deletion queue.");
    case SurfaceOp::ScaleUp:
        return QStringLiteral(
            "The host texture was recreated with a larger resolution scale or sample "
            "count, to receive data from a larger surface.");
    case SurfaceOp::UnregisterAll:
        return QStringLiteral(
            "Every surface was flushed and unregistered, after a change of resolution, "
            "texture filter or custom texture settings.");
    case SurfaceOp::ClearAll:
        return QStringLiteral("The cache \"forgot\" every surface, flushing them only if asked.");
    case SurfaceOp::MemoryFill:
        return QStringLiteral(
            "A Pica memory fill. It creates a fill surface that holds the fill value.");
    case SurfaceOp::DisplayTransfer:
        return QStringLiteral("A Pica display transfer, done on the host GPU.");
    case SurfaceOp::TextureCopy:
        return QStringLiteral("A Pica texture copy, done on the host GPU.");
    case SurfaceOp::NotAccelerated:
        return QStringLiteral(
            "The operation above could not be done on the host GPU and fell back to the "
            "software implementation, which flushes and invalidates guest memory.");
    case SurfaceOp::Flush:
        return QStringLiteral(
            "Dirty surfaces in the range wrote their data back to guest memory. Recorded "
            "only when there was something to write.");
    case SurfaceOp::Invalidate:
        return QStringLiteral(
            "The range of guest memory was overwritten. If the Surface column is filled, that "
            "surface drew the range and now holds its newest data. Recorded only when the "
            "range was held by another surface or by none. If it is empty, the CPU, a service "
            "or the software renderer wrote the range. Recorded only when that made a cached "
            "surface out of date.");
    case SurfaceOp::MarkInvalid:
        return QStringLiteral(
            "Part of the surface became invalid. Its host texture no longer holds the "
            "latest data of that range. Recorded only when part of the range was still "
            "valid.");
    case SurfaceOp::Validate:
        return QStringLiteral(
            "Invalid parts of the surface were brought up to date, by copies from other "
            "surfaces, reinterpretation or uploads.");
    case SurfaceOp::Copy:
        return QStringLiteral("Copy from another surface at the same scale, on the host GPU.");
    case SurfaceOp::Blit:
        return QStringLiteral("Scaled copy from another surface, on the host GPU.");
    case SurfaceOp::Clear:
        return QStringLiteral("Cleared with the value of a fill surface, on the host GPU.");
    case SurfaceOp::Reinterpret:
        return QStringLiteral(
            "Copy from a surface of another format, converting the data on the host GPU.");
    case SurfaceOp::Upload:
        return QStringLiteral("Guest memory was decoded and uploaded to the host texture.");
    case SurfaceOp::CustomMatch:
        return QStringLiteral(
            "A custom texture matched the hash of the guest data. It replaces the host "
            "texture once decoded, maybe in a later frame.");
    case SurfaceOp::CustomUpload:
        return QStringLiteral("A custom texture was uploaded and replaced the host texture.");
    case SurfaceOp::GenerateMipmaps:
        return QStringLiteral("Mipmaps were generated from the base level on the host GPU.");
    case SurfaceOp::Download:
        return QStringLiteral(
            "The host texture was read back from the host GPU and encoded to guest "
            "memory.");
    case SurfaceOp::FillDownload:
        return QStringLiteral(
            "The host CPU wrote the pattern of a fill surface to guest memory. The host GPU is "
            "not involved.");
    case SurfaceOp::TemporaryCopy:
        return QStringLiteral(
            "A texture was copied to a temporary surface, because its mipmap chain does "
            "not fit in the cache. The temporary surface is sentenced immediately.");
    case SurfaceOp::CubeFaceCopy:
        return QStringLiteral("A face was copied to a texture cube.");
    case SurfaceOp::Draw:
        return QStringLiteral(
            "A draw with the specified color and depth targets. Without \"Record usage\", draws "
            "are only recorded when work was done in them.");
    case SurfaceOp::Texture:
        return QStringLiteral("The surface was bound as a texture for a draw.");
    case SurfaceOp::Display:
        return QStringLiteral("The surface was presented on a screen.");
    case SurfaceOp::Import:
        return QStringLiteral("The surface viewer replaced the surface with an imported image.");
    case SurfaceOp::Revert:
        return QStringLiteral("The surface viewer removed an imported image.");
    case SurfaceOp::FrameEnd:
        return QStringLiteral("A frame (a VBlank) ended.");
    default:
        return {};
    }
}

SurfaceOpModel::Category SurfaceOpModel::OpCategory(SurfaceOp op) {
    switch (op) {
    case SurfaceOp::Create:
    case SurfaceOp::Recycle:
    case SurfaceOp::Register:
    case SurfaceOp::Unregister:
    case SurfaceOp::Sentence:
    case SurfaceOp::Delete:
    case SurfaceOp::EraseFill:
    case SurfaceOp::ScaleUp:
    case SurfaceOp::UnregisterAll:
    case SurfaceOp::ClearAll:
        return Category::Lifetime;
    case SurfaceOp::MemoryFill:
    case SurfaceOp::DisplayTransfer:
    case SurfaceOp::TextureCopy:
    case SurfaceOp::NotAccelerated:
        return Category::Pica;
    case SurfaceOp::Flush:
    case SurfaceOp::Invalidate:
    case SurfaceOp::MarkInvalid:
        return Category::Coherency;
    case SurfaceOp::Draw:
    case SurfaceOp::Texture:
    case SurfaceOp::Display:
        return Category::Usage;
    case SurfaceOp::Import:
    case SurfaceOp::Revert:
        return Category::Tools;
    case SurfaceOp::FrameEnd:
        return Category::Frames;
    default:
        return Category::Transfers;
    }
}

QString SurfaceOpModel::CategoryName(Category category) {
    switch (category) {
    case Category::Lifetime:
        return QStringLiteral("Lifetime");
    case Category::Pica:
        return QStringLiteral("Pica");
    case Category::Coherency:
        return QStringLiteral("Flush/invalidate");
    case Category::Transfers:
        return QStringLiteral("Validation");
    case Category::Usage:
        return QStringLiteral("Usage");
    case Category::Tools:
        return QStringLiteral("Viewer");
    case Category::Frames:
        return QStringLiteral("Frames");
    default:
        return {};
    }
}

QString SurfaceOpModel::CategoryDescription(Category category) {
    switch (category) {
    case Category::Lifetime:
        return QStringLiteral(
            "Create, Recycle, Register, Unregister, Sentence, Delete, Erase fill, Scale "
            "up, Unregister all, Clear all");
    case Category::Pica:
        return QStringLiteral("Memory fill, Display transfer, Texture copy, Not accelerated");
    case Category::Coherency:
        return QStringLiteral("Flush, Invalidate, Mark invalid");
    case Category::Transfers:
        return QStringLiteral(
            "Validate, Copy, Blit, Clear, Reinterpret, Upload, Custom texture match and "
            "upload, Generate mipmaps, Download, Fill download, Temporary copy, Cube face "
            "copy");
    case Category::Usage:
        return QStringLiteral("Draw, Texture, Display");
    case Category::Tools:
        return QStringLiteral("Import, Revert import");
    case Category::Frames:
        return QStringLiteral("Frame end");
    default:
        return {};
    }
}

QString SurfaceOpModel::SurfaceText(const SurfaceOpSurface& surface) {
    if (!surface.IsValid()) {
        return {};
    }
    return QStringLiteral("#%1 (slot %2)").arg(surface.unique_id).arg(surface.index);
}

QString SurfaceOpModel::SurfaceInfoText(const SurfaceOpSurface& surface) {
    if (!surface.IsValid()) {
        return {};
    }
    if (surface.Type() == VideoCore::SurfaceType::Fill) {
        return QStringLiteral("fill");
    }
    QStringList parts{
        QStringLiteral("%1 %2x%3")
            .arg(QString::fromUtf8(VideoCore::PixelFormatAsString(surface.Format()).data()))
            .arg(surface.width)
            .arg(surface.height),
    };
    if (surface.levels > 1) {
        parts << QStringLiteral("%1 level(s)").arg(surface.levels);
    }
    if (surface.GetTextureType() == VideoCore::TextureType::CubeMap) {
        parts << QStringLiteral("cube");
    }
    parts << (surface.is_tiled ? QStringLiteral("tiled") : QStringLiteral("linear"));
    if (surface.res_scale > 1) {
        parts << QStringLiteral("scale %1").arg(surface.res_scale);
    }
    return parts.join(QStringLiteral(", "));
}

QString SurfaceOpModel::RangeText(const SurfaceOpRecord& record) {
    if (record.start == 0 && record.end == 0xFFFFFFFF) {
        return QStringLiteral("all memory");
    }
    if (record.start == record.end) {
        return record.start == 0 ? QString{} : Hex(record.start);
    }
    return QStringLiteral("%1-%2 (%3)")
        .arg(Hex(record.start), Hex(record.end), ::Bytes(record.end - record.start));
}

QString SurfaceOpModel::DetailsText(const SurfaceOpRecord& record) {
    const auto other = [&record] {
        return QStringLiteral("%1, %2").arg(SurfaceText(record.other),
                                            SurfaceInfoText(record.other));
    };
    switch (record.op) {
    case SurfaceOp::Copy:
    case SurfaceOp::Blit:
    case SurfaceOp::Clear:
    case SurfaceOp::Reinterpret:
    case SurfaceOp::TemporaryCopy:
        return QStringLiteral("from %1").arg(other());
    case SurfaceOp::CubeFaceCopy:
        return QStringLiteral("face %1 from %2").arg(record.value.face).arg(other());
    case SurfaceOp::Draw:
        return record.other.IsValid() ? QStringLiteral("depth target %1").arg(other()) : QString{};
    case SurfaceOp::ScaleUp:
        return QStringLiteral("to %1x").arg(record.value.scale);
    case SurfaceOp::MemoryFill:
        return QStringLiteral("value %1").arg(Hex(record.value.fill_value));
    case SurfaceOp::DisplayTransfer:
    case SurfaceOp::TextureCopy:
        return QStringLiteral("to %1").arg(Hex(record.value.output_addr));
    case SurfaceOp::Invalidate:
        return record.surface.IsValid() ? QStringLiteral("written by the surface")
                                        : QStringLiteral("guest memory changed");
    case SurfaceOp::Flush:
        return record.surface.IsValid() ? QStringLiteral("only this surface") : QString{};
    case SurfaceOp::CustomMatch:
    case SurfaceOp::Import:
    case SurfaceOp::Revert:
        return QStringLiteral("hash %1").arg(record.value.hash, 16, 16, QLatin1Char('0'));
    case SurfaceOp::FrameEnd:
        return QStringLiteral("frame %1 ended").arg(record.value.frame);
    default:
        return {};
    }
}

void SurfaceOpFilter::SetText(const QString& text_) {
    text = text_.trimmed();
    kind = TextKind::None;
    bool ok = false;
    if (text.isEmpty()) {
        kind = TextKind::None;
    } else if (text.startsWith(QLatin1Char('#')) && (number = text.mid(1).toULongLong(&ok), ok)) {
        kind = TextKind::UniqueId;
    } else if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) &&
               (number = text.mid(2).toULongLong(&ok, 16), ok)) {
        kind = TextKind::Address;
    } else {
        kind = TextKind::Name;
    }
    invalidateFilter();
}

void SurfaceOpFilter::SetCategoryShown(SurfaceOpModel::Category category, bool shown) {
    shown_categories[static_cast<std::size_t>(category)] = shown;
    invalidateFilter();
}

bool SurfaceOpFilter::filterAcceptsRow(int source_row, const QModelIndex&) const {
    const auto* model = static_cast<const SurfaceOpModel*>(sourceModel());
    const SurfaceOpRecord& record = model->Record(source_row);
    if (!shown_categories[static_cast<std::size_t>(SurfaceOpModel::OpCategory(record.op))]) {
        return false;
    }
    switch (kind) {
    case TextKind::None:
        return true;
    case TextKind::UniqueId:
        return (record.surface.IsValid() && record.surface.unique_id == number) ||
               (record.other.IsValid() && record.other.unique_id == number);
    case TextKind::Address:
        return (number >= record.start && number < record.end) ||
               Contains(record.surface, number) || Contains(record.other, number);
    case TextKind::Name:
        return SurfaceOpModel::OpName(record.op).contains(text, Qt::CaseInsensitive);
    }
    return true;
}

SurfaceOpView::SurfaceOpView(Core::System& system_, DebugWorkQueue& work_queue_, QWidget* parent)
    : QWidget(parent), system{system_}, work_queue{work_queue_}, model{SurfaceOpModel::Get()} {
    filter = new SurfaceOpFilter(this);
    filter->setSourceModel(model.get());

    start_button = new QPushButton(QStringLiteral("Start"));
    start_button->setToolTip(
        QStringLiteral("Start recording the operations of the surface cache."));
    stop_button = new QPushButton(QStringLiteral("Stop"));
    clear_button = new QPushButton(QStringLiteral("Clear"));
    save_button = new QPushButton(QStringLiteral("Save..."));
    save_button->setToolTip(QStringLiteral("Save the operations shown, as tab separated text."));
    record_usage = new QCheckBox(QStringLiteral("Record usage"));
    record_usage->setToolTip(QStringLiteral(
        "Also record every draw, texture bind and presentation. Without it, draws are "
        "recorded only when work was done in them."));
    status_label = new QLabel;
    status_label->setWordWrap(true);

    filter_edit = new QLineEdit;
    filter_edit->setPlaceholderText(
        QStringLiteral("Filter by address (0x...), surface unique ID (#N) or operation"));
    filter_edit->setClearButtonEnabled(true);

    auto buttons_layout = new QHBoxLayout;
    buttons_layout->addWidget(start_button);
    buttons_layout->addWidget(stop_button);
    buttons_layout->addWidget(clear_button);
    buttons_layout->addWidget(record_usage);
    buttons_layout->addStretch();
    buttons_layout->addWidget(save_button);

    auto filter_layout = new QHBoxLayout;
    filter_layout->addWidget(new QLabel(QStringLiteral("Show:")));
    for (std::size_t i = 0; i < static_cast<std::size_t>(SurfaceOpModel::Category::Count); i++) {
        const auto category = static_cast<SurfaceOpModel::Category>(i);
        auto* check = new QCheckBox(SurfaceOpModel::CategoryName(category));
        check->setChecked(true);
        check->setToolTip(SurfaceOpModel::CategoryDescription(category));
        connect(check, &QCheckBox::toggled, this,
                [this, category](bool shown) { filter->SetCategoryShown(category, shown); });
        filter_layout->addWidget(check);
    }
    filter_layout->addStretch();

    table = new QTreeView;
    table->setModel(filter);
    table->setRootIsDecorated(false);
    table->setUniformRowHeights(true);
    table->setAlternatingRowColors(true);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    QFont fixed_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    table->setFont(fixed_font);
    table->header()->setStretchLastSection(true);
    table->setColumnWidth(SurfaceOpModel::Number, 80);
    table->setColumnWidth(SurfaceOpModel::Frame, 60);
    table->setColumnWidth(SurfaceOpModel::Operation, 220);
    table->setColumnWidth(SurfaceOpModel::Surface, 120);
    table->setColumnWidth(SurfaceOpModel::Range, 260);
    table->setColumnWidth(SurfaceOpModel::Bytes, 80);
    table->setColumnWidth(SurfaceOpModel::SurfaceInfo, 230);

    details = new QTextBrowser;
    show_surface_button = new QPushButton(QStringLiteral("Show surface"));
    show_surface_button->setToolTip(QStringLiteral(
        "Select the surface in the cached surfaces tab. Double clicking an operation does the "
        "same."));
    show_other_button = new QPushButton(QStringLiteral("Show other surface"));
    show_other_button->setToolTip(QStringLiteral(
        "Select the other surface (copy source, depth target) in the cached surfaces tab."));
    filter_surface_button = new QPushButton(QStringLiteral("Filter by surface"));
    filter_surface_button->setToolTip(QStringLiteral("Show only the operations on this surface."));
    auto details_buttons = new QHBoxLayout;
    details_buttons->addWidget(show_surface_button);
    details_buttons->addWidget(show_other_button);
    details_buttons->addWidget(filter_surface_button);
    details_buttons->addStretch();
    auto details_widget = new QWidget;
    auto details_layout = new QVBoxLayout(details_widget);
    details_layout->setContentsMargins(0, 0, 0, 0);
    details_layout->addWidget(details);
    details_layout->addLayout(details_buttons);

    auto splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(table);
    splitter->addWidget(details_widget);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    auto layout = new QVBoxLayout(this);
    layout->addLayout(buttons_layout);
    layout->addWidget(status_label);
    layout->addWidget(filter_edit);
    layout->addLayout(filter_layout);
    layout->addWidget(splitter, 1);

    connect(start_button, &QPushButton::clicked, this, &SurfaceOpView::Start);
    connect(stop_button, &QPushButton::clicked, this, &SurfaceOpView::Stop);
    connect(clear_button, &QPushButton::clicked, this, &SurfaceOpView::ClearLog);
    connect(save_button, &QPushButton::clicked, this, &SurfaceOpView::Save);
    connect(record_usage, &QCheckBox::toggled, this, [](bool checked) {
        SurfaceOpLog& log = VideoCore::GetSurfaceOpLog();
        if (log.IsRecording()) {
            log.Start(checked);
        }
    });
    connect(filter_edit, &QLineEdit::textChanged, filter, &SurfaceOpFilter::SetText);
    connect(model.get(), &SurfaceOpModel::StateChanged, this, &SurfaceOpView::UpdateState);
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &SurfaceOpView::UpdateDetails);
    connect(model.get(), &QAbstractItemModel::modelReset, this, &SurfaceOpView::UpdateDetails);

    // New rows are followed while the table is scrolled to its end
    connect(filter, &QAbstractItemModel::rowsAboutToBeInserted, this, [this] {
        const QScrollBar* bar = table->verticalScrollBar();
        follow_end = bar->value() == bar->maximum();
    });
    connect(filter, &QAbstractItemModel::rowsInserted, this, [this] {
        if (follow_end) {
            table->scrollToBottom();
        }
    });

    connect(show_surface_button, &QPushButton::clicked, this, [this] {
        if (const SurfaceOpRecord* record = SelectedRecord()) {
            emit ShowSurface(record->surface.index, record->surface.unique_id);
        }
    });
    connect(show_other_button, &QPushButton::clicked, this, [this] {
        if (const SurfaceOpRecord* record = SelectedRecord()) {
            emit ShowSurface(record->other.index, record->other.unique_id);
        }
    });
    connect(table, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const int row = filter->mapToSource(index).row();
        if (row < 0 || row >= model->rowCount()) {
            return;
        }
        // A draw with only a depth target has it as the other surface
        const SurfaceOpRecord& record = model->Record(row);
        const SurfaceOpSurface& surface = record.surface.IsValid() ? record.surface : record.other;
        if (surface.IsValid()) {
            emit ShowSurface(surface.index, surface.unique_id);
        }
    });
    connect(filter_surface_button, &QPushButton::clicked, this, [this] {
        if (const SurfaceOpRecord* record = SelectedRecord(); record && record->surface.IsValid()) {
            filter_edit->setText(QStringLiteral("#%1").arg(record->surface.unique_id));
        }
    });

    UpdateState();
    UpdateDetails();
}

SurfaceOpView::~SurfaceOpView() = default;

void SurfaceOpView::OnEmulationStateChanged(bool running_, bool paused) {
    const bool started = running_ && !running;
    running = running_;
    // The renderer of a new run starts without tracking
    if (started && VideoCore::GetSurfaceOpLog().IsRecording()) {
        QueueSurfaceDebugTracking(system, work_queue);
    }
    UpdateState();
}

void SurfaceOpView::Start() {
    SurfaceOpLog& log = VideoCore::GetSurfaceOpLog();
    log.Start(record_usage->isChecked());
    QueueSurfaceDebugTracking(system, work_queue);
    UpdateState();
}

void SurfaceOpView::Stop() {
    VideoCore::GetSurfaceOpLog().Stop();
    QueueSurfaceDebugTracking(system, work_queue);
    UpdateState();
}

void SurfaceOpView::ClearLog() {
    model->Clear();
}

void SurfaceOpView::Save() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Surface Operations"), QStringLiteral("surface_operations.tsv"),
        QStringLiteral("Tab separated values (*.tsv *.txt)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::critical(this, QStringLiteral("Save Surface Operations"),
                              QStringLiteral("Could not write %1").arg(path));
        return;
    }
    QTextStream out(&file);
    for (int column = 0; column < SurfaceOpModel::ColumnCount; column++) {
        out << model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString()
            << (column + 1 < SurfaceOpModel::ColumnCount ? '\t' : '\n');
    }
    const int rows = filter->rowCount();
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < SurfaceOpModel::ColumnCount; column++) {
            out << filter->index(row, column).data().toString()
                << (column + 1 < SurfaceOpModel::ColumnCount ? '\t' : '\n');
        }
    }
}

void SurfaceOpView::UpdateState() {
    const SurfaceOpLog& log = VideoCore::GetSurfaceOpLog();
    const bool recording = log.IsRecording();
    const int rows = model->rowCount();
    start_button->setEnabled(!recording);
    stop_button->setEnabled(recording);
    clear_button->setEnabled(rows != 0);
    save_button->setEnabled(rows != 0);

    QString status;
    if (recording) {
        status = QStringLiteral("Recording.");
    } else if (log.IsFull()) {
        status =
            QStringLiteral("Stopped because the log is full. Save or clear it to record more.");
    } else {
        status = QStringLiteral("Not recording.");
    }
    if (rows != 0) {
        status +=
            QLatin1Char(' ') + QStringLiteral("%4 operation(s) (%1), frames %2 to %3.")
                                   .arg(::Bytes(static_cast<u64>(rows) * sizeof(SurfaceOpRecord)))
                                   .arg(model->Record(0).frame)
                                   .arg(model->Record(rows - 1).frame)
                                   .arg(rows);
    }
    if (recording && !running) {
        status += QLatin1Char(' ') + QStringLiteral("Emulation is not running.");
    }
    status_label->setText(status);
}

const SurfaceOpRecord* SurfaceOpView::SelectedRecord() const {
    const QModelIndex index = table->selectionModel()->currentIndex();
    if (!index.isValid()) {
        return nullptr;
    }
    const int row = filter->mapToSource(index).row();
    if (row < 0 || row >= model->rowCount()) {
        return nullptr;
    }
    return &model->Record(row);
}

int SurfaceOpView::ParentRow(int row) const {
    const u8 depth = model->Record(row).depth;
    if (depth == 0) {
        return -1;
    }
    for (int i = row - 1; i >= std::max(0, row - PARENT_SEARCH_LIMIT); i--) {
        if (model->Record(i).depth < depth) {
            return i;
        }
    }
    return -1;
}

void SurfaceOpView::UpdateDetails() {
    const SurfaceOpRecord* record = SelectedRecord();
    show_surface_button->setEnabled(record && record->surface.IsValid());
    show_other_button->setEnabled(record && record->other.IsValid());
    filter_surface_button->setEnabled(record && record->surface.IsValid());
    if (!record) {
        details->setHtml(QStringLiteral("Select an operation to see its details."));
        return;
    }

    QString html = QStringLiteral("<p><b>%1</b>: %2</p><table>")
                       .arg(SurfaceOpModel::OpName(record->op).toHtmlEscaped(),
                            SurfaceOpModel::OpDescription(record->op).toHtmlEscaped());
    const auto row = [&html](const QString& name, const QString& value) {
        if (!value.isEmpty()) {
            html += QStringLiteral("<tr><td><b>%1</b>&nbsp;&nbsp;</td><td>%2</td></tr>")
                        .arg(name.toHtmlEscaped(), value.toHtmlEscaped());
        }
    };
    const auto surface = [](const SurfaceOpSurface& s) {
        if (!s.IsValid()) {
            return QString{};
        }
        return QStringLiteral("%1, %2, %3-%4 (%5)")
            .arg(SurfaceOpModel::SurfaceText(s), SurfaceOpModel::SurfaceInfoText(s), Hex(s.addr),
                 Hex(s.addr + s.size), ::Bytes(s.size));
    };

    const int source_row = filter->mapToSource(table->selectionModel()->currentIndex()).row();
    row(QStringLiteral("Number"), QString::number(source_row));
    row(QStringLiteral("Frame"), QString::number(record->frame));
    if (const int parent = ParentRow(source_row); parent >= 0) {
        const SurfaceOpRecord& parent_record = model->Record(parent);
        row(QStringLiteral("Part of"),
            QStringLiteral("#%1 %2 %3")
                .arg(parent)
                .arg(SurfaceOpModel::OpName(parent_record.op),
                     SurfaceOpModel::SurfaceText(parent_record.surface)));
    }
    row(QStringLiteral("Range"), SurfaceOpModel::RangeText(*record));
    if (IsTransfer(record->op)) {
        row(QStringLiteral("Bytes"), ::Bytes(record->value.bytes));
    }
    const bool is_owner = record->op == SurfaceOp::Invalidate;
    row(is_owner
            ? QStringLiteral("Owner")
            : (HasSource(record->op) ? QStringLiteral("Destination") : QStringLiteral("Surface")),
        surface(record->surface));
    row(HasSource(record->op) ? QStringLiteral("Source")
                              : (record->op == SurfaceOp::Draw ? QStringLiteral("Depth target")
                                                               : QStringLiteral("Other")),
        surface(record->other));
    row(QStringLiteral("Details"), SurfaceOpModel::DetailsText(*record));
    html += QStringLiteral("</table>");
    details->setHtml(html);
}
