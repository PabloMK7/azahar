// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeView>
#include <fmt/format.h>
#include "citra_qt/debugger/debug_work_queue.h"
#include "citra_qt/debugger/graphics/graphics_breakpoints_p.h"
#include "citra_qt/debugger/graphics/graphics_surface_cache.h"
#include "common/file_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_cache/surface_base.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

using VideoCore::SurfaceDebugInfo;
using VideoCore::SurfaceUsageBits;

namespace {

/// Number of visible surface views. Surface tracking is on while it is not zero.
std::atomic<int> active_views{0};

/// Snapshots are requested again for this long after a frame step, until the stepped frame ends.
constexpr int STEP_RETRY_INTERVAL_MS = 5;
constexpr int STEP_RETRY_LIMIT = 400;
/// How often Live asks whether enough frames have ended for the next snapshot.
constexpr int LIVE_POLL_INTERVAL_MS = 5;
constexpr int LIVE_DEFAULT_FRAMES = 30;
constexpr int LIVE_MAX_FRAMES = 3600;
constexpr int PENDING_WARNING_MS = 1000;

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

static QString BytesPerSecond(double bytes_per_second) {
    if (bytes_per_second >= 1024 * 1024) {
        return QStringLiteral("%1 MiB/s").arg(bytes_per_second / (1024.0 * 1024.0), 0, 'f', 2);
    }
    if (bytes_per_second >= 1024) {
        return QStringLiteral("%1 KiB/s").arg(bytes_per_second / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B/s").arg(bytes_per_second, 0, 'f', 0);
}

QString FormatText(const VideoCore::SurfaceParams& params) {
    if (params.type == VideoCore::SurfaceType::Fill) {
        return QStringLiteral("-");
    }
    QString text = QString::fromUtf8(VideoCore::PixelFormatAsString(params.pixel_format).data());
    if (params.custom_format != VideoCore::CustomPixelFormat::Invalid) {
        text += QStringLiteral(" (%1)").arg(
            QString::fromUtf8(VideoCore::CustomPixelFormatAsString(params.custom_format).data()));
    }
    return text;
}

static QImage ToImage(const VideoCore::SurfaceDebugImage& surface_image, bool flip) {
    const u32 width = surface_image.width;
    const u32 height = surface_image.height;
    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGBA8888);
    for (u32 y = 0; y < height; y++) {
        const u32 out_y = flip ? height - 1 - y : y;
        std::memcpy(image.scanLine(static_cast<int>(out_y)),
                    &surface_image.rgba[static_cast<std::size_t>(y) * width * 4], width * 4);
    }
    return image;
}

static bool UsedInFrame(const SurfaceDebugInfo& surface, u64 frame) {
    return True(surface.usage.last_usage) && surface.usage.last_used_frame >= frame;
}

} // Anonymous namespace

SurfaceCacheModel::SurfaceCacheModel(QObject* parent) : QAbstractTableModel(parent) {}

void SurfaceCacheModel::SetInfo(const VideoCore::SurfaceCacheDebugInfo& info, u64 usage_frame) {
    beginResetModel();
    surfaces = info.surfaces;
    frame = usage_frame;
    endResetModel();
}

const SurfaceDebugInfo* SurfaceCacheModel::GetSurface(int row) const {
    if (row < 0 || row >= static_cast<int>(surfaces.size())) {
        return nullptr;
    }
    return &surfaces[row];
}

int SurfaceCacheModel::RowOfIndex(u32 index) const {
    const auto it = std::find_if(surfaces.begin(), surfaces.end(),
                                 [index](const SurfaceDebugInfo& s) { return s.index == index; });
    return it == surfaces.end() ? -1 : static_cast<int>(it - surfaces.begin());
}

int SurfaceCacheModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(surfaces.size());
}

int SurfaceCacheModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QString SurfaceCacheModel::StateText(const SurfaceDebugInfo& surface) {
    if (surface.sentenced) {
        return QStringLiteral("Sentenced");
    }
    if (surface.flags & static_cast<u32>(VideoCore::SurfaceFlagBits::Registered)) {
        return QStringLiteral("Registered");
    }
    if (surface.cube_refs > 0) {
        return QStringLiteral("Cube");
    }
    return QStringLiteral("Unregistered");
}

QString SurfaceCacheModel::RecycleText(const SurfaceDebugInfo& surface) {
    if (!surface.sentenced) {
        return {};
    }
    QStringList parts;
    parts << (surface.recyclable ? QStringLiteral("recyclable") : QStringLiteral("not recyclable"));
    if (surface.gpu_in_use) {
        parts << QStringLiteral("GPU busy");
    } else if (surface.frames_until_deletion > 0) {
        parts << QStringLiteral("deleted in %1 frame(s)")
                     .arg(static_cast<int>(surface.frames_until_deletion));
    } else {
        parts << QStringLiteral("deletion pending");
    }
    return parts.join(QStringLiteral(", "));
}

QString SurfaceCacheModel::FlagsText(u32 flags) {
    using VideoCore::SurfaceFlagBits;
    static constexpr std::pair<SurfaceFlagBits, const char*> names[] = {
        {SurfaceFlagBits::Registered, "Registered"},
        {SurfaceFlagBits::Picked, "Picked"},
        {SurfaceFlagBits::Tracked, "Tracked"},
        {SurfaceFlagBits::Custom, "Custom"},
        {SurfaceFlagBits::ShadowSource, "ShadowSource"},
        {SurfaceFlagBits::RenderTarget, "RenderTarget"},
    };
    QStringList parts;
    for (const auto& [bit, name] : names) {
        if (flags & static_cast<u32>(bit)) {
            parts << QString::fromLatin1(name);
        }
    }
    return parts.join(QStringLiteral(", "));
}

QString SurfaceCacheModel::UsageText(SurfaceUsageBits usage) {
    static constexpr std::pair<SurfaceUsageBits, const char*> names[] = {
        {SurfaceUsageBits::ColorTarget, "Color Target"},
        {SurfaceUsageBits::DepthTarget, "Depth Target"},
        {SurfaceUsageBits::Texture, "Texture"},
        {SurfaceUsageBits::TransferSource, "Transfer Source"},
        {SurfaceUsageBits::TransferDest, "Transfer Destination"},
        {SurfaceUsageBits::Fill, "Fill"},
        {SurfaceUsageBits::CopySource, "Copy Source"},
        {SurfaceUsageBits::CopyDest, "Copy Destination"},
        {SurfaceUsageBits::Upload, "Upload"},
        {SurfaceUsageBits::Download, "Download"},
        {SurfaceUsageBits::Display, "Display"},
    };
    QStringList parts;
    for (const auto& [bit, name] : names) {
        if (True(usage & bit)) {
            parts << QString::fromLatin1(name);
        }
    }
    return parts.join(QStringLiteral(", "));
}

QString SurfaceCacheModel::TypeText(const VideoCore::SurfaceParams& params) {
    QString type;
    switch (params.type) {
    case VideoCore::SurfaceType::Color:
        type = QStringLiteral("Color");
        break;
    case VideoCore::SurfaceType::Texture:
        type = QStringLiteral("Texture");
        break;
    case VideoCore::SurfaceType::Depth:
        type = QStringLiteral("Depth");
        break;
    case VideoCore::SurfaceType::DepthStencil:
        type = QStringLiteral("Depth/Stencil");
        break;
    case VideoCore::SurfaceType::Fill:
        return QStringLiteral("Fill");
    default:
        type = QStringLiteral("Invalid");
        break;
    }
    if (params.texture_type == VideoCore::TextureType::CubeMap) {
        type += QStringLiteral(" (cube)");
    }
    return type;
}

QVariant SurfaceCacheModel::DisplayData(const SurfaceDebugInfo& surface, int column) const {
    const auto& params = surface.params;
    switch (column) {
    case Index:
        return surface.index;
    case State:
        return StateText(surface);
    case Recycle:
        return RecycleText(surface);
    case Address:
        return Hex(params.addr);
    case End:
        return Hex(params.end);
    case Size:
        return Bytes(params.size);
    case Dimensions:
        if (params.type == VideoCore::SurfaceType::Fill) {
            return QStringLiteral("-");
        }
        if (params.stride != params.width) {
            return QStringLiteral("%1x%2 (stride %3)")
                .arg(params.width)
                .arg(params.height)
                .arg(params.stride);
        }
        return QStringLiteral("%1x%2").arg(params.width).arg(params.height);
    case Format:
        return FormatText(params);
    case Type:
        return TypeText(params);
    case Levels:
        return params.levels;
    case Scale:
        return params.type == VideoCore::SurfaceType::Fill ? QVariant{}
                                                           : QVariant{params.res_scale};
    case Samples:
        return params.sample_count;
    case Tiled:
        return params.is_tiled ? QStringLiteral("Yes") : QStringLiteral("No");
    case Flags:
        return FlagsText(surface.flags);
    case LastUsed:
        if (False(surface.usage.last_usage)) {
            return {};
        }
        if (surface.usage.last_used_frame >= frame) {
            return QStringLiteral("this frame");
        }
        return QStringLiteral("%1 frame(s) ago")
            .arg(static_cast<int>(frame - std::min(frame, surface.usage.last_used_frame)));
    case Usage:
        return UsageText(surface.usage.last_usage);
    case Created:
        return surface.usage.creation_tracked ? QVariant{qulonglong{surface.usage.created_frame}}
                                              : QVariant{};
    case Recycled:
        return surface.usage.recycle_count;
    case Invalid:
        if (surface.fully_invalid) {
            return QStringLiteral("all");
        }
        return surface.invalid_bytes ? Bytes(surface.invalid_bytes) : QString{};
    case Dirty:
        return surface.dirty_bytes ? Bytes(surface.dirty_bytes) : QString{};
    case Uploaded:
        return surface.usage.uploaded_bytes ? Bytes(surface.usage.uploaded_bytes) : QString{};
    case Downloaded:
        return surface.usage.downloaded_bytes ? Bytes(surface.usage.downloaded_bytes) : QString{};
    case HostMemory:
        return surface.host_bytes ? Bytes(surface.host_bytes) : QString{};
    case References: {
        QStringList parts;
        if (surface.framebuffer_refs) {
            parts << QStringLiteral("%1 framebuffer(s)")
                         .arg(static_cast<int>(surface.framebuffer_refs));
        }
        if (surface.cube_refs) {
            parts << QStringLiteral("%1 cube(s)").arg(static_cast<int>(surface.cube_refs));
        }
        return parts.join(QStringLiteral(", "));
    }
    default:
        return {};
    }
}

QVariant SurfaceCacheModel::SortData(const SurfaceDebugInfo& surface, int column) const {
    const auto& params = surface.params;
    switch (column) {
    case Index:
        return surface.index;
    case Recycle:
        return surface.sentenced ? QVariant{qlonglong(surface.frames_until_deletion)}
                                 : QVariant{-1};
    case Address:
        return params.addr;
    case End:
        return params.end;
    case Size:
        return params.size;
    case Dimensions:
        return qulonglong{params.width} * params.height;
    case LastUsed:
        return True(surface.usage.last_usage) ? QVariant{qulonglong{surface.usage.last_used_frame}}
                                              : QVariant{0};
    case Created:
        return qulonglong{surface.usage.created_frame};
    case Recycled:
        return surface.usage.recycle_count;
    case Invalid:
        return surface.invalid_bytes;
    case Dirty:
        return surface.dirty_bytes;
    case Uploaded:
        return qulonglong{surface.usage.uploaded_bytes};
    case Downloaded:
        return qulonglong{surface.usage.downloaded_bytes};
    case HostMemory:
        return qulonglong{surface.host_bytes};
    default:
        return DisplayData(surface, column);
    }
}

QVariant SurfaceCacheModel::data(const QModelIndex& index, int role) const {
    const SurfaceDebugInfo* surface = GetSurface(index.row());
    if (!surface) {
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        return DisplayData(*surface, index.column());
    case SortRole:
        return SortData(*surface, index.column());
    case Qt::ForegroundRole:
        if (surface->sentenced) {
            return QColor(Qt::gray);
        }
        return {};
    case Qt::FontRole:
        if (UsedInFrame(*surface, frame)) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return {};
    case Qt::ToolTipRole:
        if (index.column() == Recycle && surface->sentenced) {
            return QStringLiteral("Sentenced at frame %1 (resource tick %2)")
                .arg(surface->sentenced_frame)
                .arg(surface->sentenced_resource_tick);
        }
        return {};
    default:
        return {};
    }
}

QString SurfaceCacheModel::HeaderToolTip(int section) {
    // Each value a column can show is listed with its meaning
    const auto list = [](const QString& description,
                         std::initializer_list<std::pair<QString, QString>> values) {
        QString html = QStringLiteral("<p>%1</p>").arg(description.toHtmlEscaped());
        if (values.size() != 0) {
            html += QStringLiteral("<table>");
            for (const auto& [value, meaning] : values) {
                html += QStringLiteral("<tr><td><b>%1</b>&nbsp;&nbsp;</td><td>%2</td></tr>")
                            .arg(value.toHtmlEscaped(), meaning.toHtmlEscaped());
            }
            html += QStringLiteral("</table>");
        }
        return html;
    };
    switch (section) {
    case Index:
        return list(QStringLiteral(
                        "Slot of the surface in the cache. A deleted surface frees its slot for a "
                        "new one."),
                    {});
    case State:
        return list(
            QStringLiteral("Where the surface is in its lifetime."),
            {
                {QStringLiteral("Registered"),
                 QStringLiteral("In the page table. Draws, transfers and "
                                "flushes of its addresses find it.")},
                {QStringLiteral("Sentenced"),
                 QStringLiteral("In the deletion queue. A new surface with the same "
                                "parameters can recycle it until it is deleted.")},
                {QStringLiteral("Cube"),
                 QStringLiteral("Not registered, but used by a texture cube (as the "
                                "cube or one of its faces).")},
                {QStringLiteral("Unregistered"),
                 QStringLiteral("Not in the page table, such as a temporary surface made to "
                                "sample a texture whose levels did not match.")},
            });
    case Recycle:
        return list(
            QStringLiteral("For sentenced surfaces, whether they can be reused and when they are "
                           "deleted. Hover a cell for the frame it was sentenced in."),
            {
                {QStringLiteral("recyclable"),
                 QStringLiteral("The GPU has reached the point where the surface "
                                "may be reused by a new surface.")},
                {QStringLiteral("not recyclable"),
                 QStringLiteral("The GPU has not reached the point where the surface "
                                "may be reused by a new surface.")},
                {QStringLiteral("GPU busy"),
                 QStringLiteral("The GPU may still use the surface, so it cannot be "
                                "deleted yet.")},
                {QStringLiteral("deleted in N frames"),
                 QStringLiteral("Frames left before the garbage "
                                "collector deletes it.")},
                {QStringLiteral("deletion pending"),
                 QStringLiteral("The next garbage collection will delete the surface.")},
            });
    case Address:
        return list(QStringLiteral("Start physical address of the surface in guest memory."), {});
    case End:
        return list(QStringLiteral("End physical address of the surface in guest memory."), {});
    case Size:
        return list(
            QStringLiteral("Size of guest memory the surface occupies, all levels included."), {});
    case Dimensions:
        return list(
            QStringLiteral("Width and height in pixels at native resolution."),
            {
                {QStringLiteral("(stride N)"),
                 QStringLiteral("The rows in guest memory are N pixels apart, "
                                "which is not the width.")},
                {QStringLiteral("-"), QStringLiteral("A fill surface, which has no texture.")},
            });
    case Format:
        return list(
            QStringLiteral("Pixel format of the surface in guest memory."),
            {
                {QStringLiteral("(custom format)"),
                 QStringLiteral("The format of the custom texture that "
                                "replaces the surface.")},
                {QStringLiteral("-"), QStringLiteral("A fill surface, which has no format.")},
            });
    case Type:
        return list(
            QStringLiteral("Kind of surface, from its format."),
            {
                {QStringLiteral("Color"),
                 QStringLiteral("A format a render target can have (RGBA8, RGB8, "
                                "RGB5A1, RGB565, RGBA4).")},
                {QStringLiteral("Texture"), QStringLiteral("A format only textures can have.")},
                {QStringLiteral("Depth"), QStringLiteral("A depth buffer (D16, D24).")},
                {QStringLiteral("Depth/Stencil"),
                 QStringLiteral("A depth buffer with stencil (D24S8).")},
                {QStringLiteral("Fill"),
                 QStringLiteral("A memory fill that has not been used as a texture "
                                "yet. It holds the fill pattern only.")},
                {QStringLiteral("(cube)"),
                 QStringLiteral("A cube map, made from six face surfaces.")},
            });
    case Levels:
        return list(QStringLiteral("Number of mipmap levels."), {});
    case Scale:
        return list(QStringLiteral("Resolution scale of the host texture."), {});
    case Samples:
        return list(QStringLiteral("Number of samples per pixel of the host texture."), {});
    case Tiled:
        return list(QStringLiteral("Layout of the pixels in guest memory."),
                    {
                        {QStringLiteral("Yes"),
                         QStringLiteral("Tiled in 8x8 blocks, as the GPU renders and samples.")},
                        {QStringLiteral("No"),
                         QStringLiteral("Linear rows, as the display processes framebuffers.")},
                    });
    case Flags:
        return list(
            QStringLiteral("Flags of the surface. Several can be set at once."),
            {
                {QStringLiteral("Registered"), QStringLiteral("In the page table.")},
                {QStringLiteral("Picked"),
                 QStringLiteral("Already visited by a search of the cache (should never appear).")},
                {QStringLiteral("Tracked"), QStringLiteral("A face of a texture cube. Writes to it "
                                                           "make the cube update.")},
                {QStringLiteral("Custom"), QStringLiteral("The texture is replaced by a custom "
                                                          "texture.")},
                {QStringLiteral("ShadowSource"), QStringLiteral("Sampled as a shadow map.")},
                {QStringLiteral("RenderTarget"), QStringLiteral("Has been a color or depth target "
                                                                "of a draw.")},
            });
    case LastUsed:
        return list(
            QStringLiteral("Last frame the surface was used in, while the surface viewer was "
                           "tracking. Empty if it was not used since."),
            {
                {QStringLiteral("this frame"),
                 QStringLiteral("Used in the frame the snapshot shows. Surfaces used this frame "
                                "are bold in the viewer.")},
                {QStringLiteral("N frames ago"), QStringLiteral("Last used that many frames ago.")},
            });
    case Usage:
        return list(
            QStringLiteral(
                "How the surface was used in the frame of the Last used column. Multiple "
                "can be listed."),
            {
                {QStringLiteral("Color Target"), QStringLiteral("The color buffer of a draw.")},
                {QStringLiteral("Depth Target"),
                 QStringLiteral("The depth (and stencil) buffer of a draw.")},
                {QStringLiteral("Texture"),
                 QStringLiteral("Sampled by a draw, as a texture or a cube map.")},
                {QStringLiteral("Transfer Source"),
                 QStringLiteral("Source of a display transfer or texture "
                                "copy.")},
                {QStringLiteral("Transfer Destination"),
                 QStringLiteral("Destination of a display transfer or "
                                "texture copy.")},
                {QStringLiteral("Fill"), QStringLiteral("Created by a memory fill.")},
                {QStringLiteral("Copy Source"),
                 QStringLiteral("Copy source to make another surface up to "
                                "date.")},
                {QStringLiteral("Copy Destination"),
                 QStringLiteral("Made up to date by a copy from another "
                                "surface.")},
                {QStringLiteral("Upload"), QStringLiteral("Loaded from guest memory.")},
                {QStringLiteral("Download"), QStringLiteral("Written back to guest memory.")},
                {QStringLiteral("Display"), QStringLiteral("Shown on a screen.")},
            });
    case Created:
        return list(QStringLiteral("Frame the surface was created or last recycled. Empty if that "
                                   "happened while the surface viewer was not tracking."),
                    {});
    case Recycled:
        return list(
            QStringLiteral("Times the surface was reused from the deletion queue, counted while "
                           "the surface viewer was tracking."),
            {});
    case Invalid:
        return list(
            QStringLiteral("Amount of guest memory that is newer than the host texture "
                           "(needs invalidation). It is loaded before the surface is used."),
            {
                {QStringLiteral("all"), QStringLiteral("Nothing has been loaded.")},
                {QStringLiteral("size"),
                 QStringLiteral("That much is not loaded. The details list the ranges.")},
            });
    case Dirty:
        return list(
            QStringLiteral(
                "Amount of guest memory that is older than the host texture (needs flushing). It "
                "is written back when the guest reads it. The details list the ranges."),
            {});
    case Uploaded:
        return list(
            QStringLiteral("Bytes copied from guest memory, or from a custom texture, to the host "
                           "texture. Counted since the surface was created or recycled, while the "
                           "surface viewer was tracking."),
            {});
    case Downloaded:
        return list(QStringLiteral(
                        "Bytes copied from the host texture to guest memory. Fill surfaces write "
                        "their pattern to guest memory with the CPU instead. Counted since the "
                        "surface was created or recycled, while the surface viewer was tracking."),
                    {});
    case HostMemory:
        return list(
            QStringLiteral("Estimated memory of the host texture, with every level at its scale "
                           "and sample count."),
            {});
    case References:
        return list(QStringLiteral("Cached objects that use the surface."),
                    {
                        {QStringLiteral("N framebuffers"),
                         QStringLiteral("Framebuffers that have it as color or "
                                        "depth attachment.")},
                        {QStringLiteral("N cubes"),
                         QStringLiteral("Texture cubes that use it as the cube or as a "
                                        "face.")},
                    });
    default:
        return {};
    }
}

QVariant SurfaceCacheModel::headerData(int section, Qt::Orientation orientation, int role) const {
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
    case Index:
        return QStringLiteral("Slot");
    case State:
        return QStringLiteral("State");
    case Recycle:
        return QStringLiteral("Recycle");
    case Address:
        return QStringLiteral("Address");
    case End:
        return QStringLiteral("End");
    case Size:
        return QStringLiteral("Size");
    case Dimensions:
        return QStringLiteral("Dimensions");
    case Format:
        return QStringLiteral("Format");
    case Type:
        return QStringLiteral("Type");
    case Levels:
        return QStringLiteral("Levels");
    case Scale:
        return QStringLiteral("Scale");
    case Samples:
        return QStringLiteral("Samples");
    case Tiled:
        return QStringLiteral("Tiled");
    case Flags:
        return QStringLiteral("Flags");
    case LastUsed:
        return QStringLiteral("Last used");
    case Usage:
        return QStringLiteral("Usage");
    case Created:
        return QStringLiteral("Created");
    case Recycled:
        return QStringLiteral("Recycled");
    case Invalid:
        return QStringLiteral("Invalid");
    case Dirty:
        return QStringLiteral("Dirty");
    case Uploaded:
        return QStringLiteral("Uploaded");
    case Downloaded:
        return QStringLiteral("Downloaded");
    case HostMemory:
        return QStringLiteral("Host memory");
    case References:
        return QStringLiteral("References");
    default:
        return {};
    }
}

void SurfaceCacheFilter::SetText(const QString& text_) {
    text = text_.trimmed();
    invalidateFilter();
}

void SurfaceCacheFilter::SetShowSentenced(bool show) {
    show_sentenced = show;
    invalidateFilter();
}

void SurfaceCacheFilter::SetUsedOnly(bool used) {
    used_only = used;
    invalidateFilter();
}

bool SurfaceCacheFilter::filterAcceptsRow(int source_row, const QModelIndex&) const {
    const auto* model = static_cast<const SurfaceCacheModel*>(sourceModel());
    const SurfaceDebugInfo* surface = model->GetSurface(source_row);
    if (!surface) {
        return false;
    }
    if (!show_sentenced && surface->sentenced) {
        return false;
    }
    if (used_only && !UsedInFrame(*surface, model->Frame())) {
        return false;
    }
    if (text.isEmpty()) {
        return true;
    }

    // An address matches the surfaces that contain it, anything else matches the row text.
    bool is_number = false;
    const u32 address = text.toUInt(&is_number, 16);
    if (is_number && address >= surface->params.addr && address < surface->params.end) {
        return true;
    }
    for (int column = 0; column < SurfaceCacheModel::ColumnCount; column++) {
        const QString cell = model->index(source_row, column).data().toString();
        if (cell.contains(text, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

SurfaceCacheView::SurfaceCacheView(Core::System& system_, DebugWorkQueue& work_queue_,
                                   QWidget* parent)
    : QWidget(parent), system{system_}, work_queue{work_queue_} {
    model = new SurfaceCacheModel(this);
    filter = new SurfaceCacheFilter(this);
    filter->setSourceModel(model);
    filter->setSortRole(SurfaceCacheModel::SortRole);
    filter->SetShowSentenced(true);

    summary_label = new QLabel;
    summary_label->setTextFormat(Qt::RichText);
    summary_label->setWordWrap(true);
    summary_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_label = new QLabel;
    status_label->setWordWrap(true);

    filter_edit = new QLineEdit;
    filter_edit->setPlaceholderText(QStringLiteral("Filter by address or text"));
    filter_edit->setClearButtonEnabled(true);
    show_sentenced = new QCheckBox(QStringLiteral("Sentenced"));
    show_sentenced->setChecked(true);
    used_only = new QCheckBox(QStringLiteral("Used this frame"));
    live_refresh = new QCheckBox(QStringLiteral("Live"));
    live_refresh->setToolTip(QStringLiteral("Refresh automatically while the emulation runs"));
    live_frames = new QSpinBox;
    live_frames->setRange(1, LIVE_MAX_FRAMES);
    live_frames->setValue(LIVE_DEFAULT_FRAMES);
    const auto update_live_text = [this](int frames) {
        const QString text = QStringLiteral("every %1 frame(s)").arg(frames);
        const QString number = QString::number(frames);
        const qsizetype position = text.indexOf(number);
        if (position < 0) {
            live_frames->setPrefix({});
            live_frames->setSuffix(QLatin1Char(' ') + text);
            return;
        }
        live_frames->setPrefix(text.left(position));
        live_frames->setSuffix(text.mid(position + number.size()));
    };
    update_live_text(LIVE_DEFAULT_FRAMES);
    connect(live_frames, &QSpinBox::valueChanged, this, update_live_text);
    live_frames->setToolTip(QStringLiteral("Emulated frames between live refreshes"));
    refresh_button = new QPushButton(QStringLiteral("Refresh"));

    table = new QTreeView;
    table->setModel(filter);
    table->setRootIsDecorated(false);
    table->setUniformRowHeights(true);
    table->setAlternatingRowColors(true);
    table->setSortingEnabled(true);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->sortByColumn(SurfaceCacheModel::Address, Qt::AscendingOrder);
    table->header()->setSectionResizeMode(QHeaderView::Interactive);

    details = new QTextBrowser;
    details->setOpenLinks(false);

    preview_level = new QSpinBox;
    preview_level->setPrefix(QStringLiteral("Level "));
    preview_level->setRange(0, 0);
    preview_mode = new QComboBox;
    preview_mode->addItems({QStringLiteral("Color"), QStringLiteral("Color (no alpha)"),
                            QStringLiteral("Alpha"), QStringLiteral("Depth (normalized)"),
                            QStringLiteral("Stencil")});
    preview_zoom = new QComboBox;
    preview_zoom->addItems({QStringLiteral("1x"), QStringLiteral("2x"), QStringLiteral("4x")});
    preview_flip = new QCheckBox(QStringLiteral("Flip"));
    preview_flip->setToolTip(
        QStringLiteral("Flip the surface vertically to match natural orientation"));
    preview_upscaled = new QCheckBox(QStringLiteral("Upscaled"));
    preview_upscaled->setToolTip(
        QStringLiteral("Read the texture at its resolution scale, as the GPU "
                       "samples it"));
    preview_save = new QPushButton(QStringLiteral("Save PNG"));
    save_upscaled = new QPushButton(QStringLiteral("Save upscaled PNG"));
    save_upscaled->setToolTip(
        QStringLiteral("Save the selected level at its resolution scale, with its "
                       "alpha"));
    open_manual = new QPushButton(QStringLiteral("Open in manual tab"));
    import_button = new QPushButton(QStringLiteral("Import PNG..."));
    import_button->setToolTip(QStringLiteral(
        "Replace the texture with a PNG of the same size, as a custom texture does"));
    import_upscaled_button = new QPushButton(QStringLiteral("Import upscaled PNG..."));
    import_upscaled_button->setToolTip(QStringLiteral(
        "Replace the texture with a larger PNG of the same aspect ratio, as a custom "
        "texture does"));
    revert_button = new QPushButton(QStringLiteral("Revert import"));
    revert_button->setToolTip(QStringLiteral("Load the texture from guest memory again"));
    save_pack_button = new QPushButton(QStringLiteral("Save to pack"));
    save_pack_button->setToolTip(QStringLiteral(
        "Save the imported PNG, or the guest texture of the selected level, to the custom "
        "texture pack of the title with the name the dumper gives it"));
    preview_label = new QLabel;
    preview_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    preview_status = new QLabel;
    preview_status->setWordWrap(true);

    live_timer = new QTimer(this);
    live_timer->setInterval(LIVE_POLL_INTERVAL_MS);

    auto* preview_scroll = new QScrollArea;
    preview_scroll->setWidget(preview_label);
    preview_scroll->setWidgetResizable(true);
    preview_scroll->setBackgroundRole(QPalette::Dark);

    auto* preview_controls = new QHBoxLayout;
    preview_controls->addWidget(preview_level);
    preview_controls->addWidget(preview_mode);
    preview_controls->addWidget(preview_zoom);
    preview_controls->addWidget(preview_flip);
    preview_controls->addWidget(preview_upscaled);
    preview_controls->addStretch();
    auto* preview_buttons = new QHBoxLayout;
    preview_buttons->addWidget(open_manual);
    preview_buttons->addWidget(preview_save);
    preview_buttons->addWidget(save_upscaled);
    preview_buttons->addStretch();
    auto* custom_buttons = new QHBoxLayout;
    custom_buttons->addWidget(import_button);
    custom_buttons->addWidget(import_upscaled_button);
    custom_buttons->addWidget(revert_button);
    custom_buttons->addWidget(save_pack_button);
    custom_buttons->addStretch();

    auto* preview_widget = new QWidget;
    auto* preview_layout = new QVBoxLayout(preview_widget);
    preview_layout->setContentsMargins(0, 0, 0, 0);
    preview_layout->addLayout(preview_controls);
    preview_layout->addWidget(preview_scroll, 1);
    preview_layout->addWidget(preview_status);
    preview_layout->addLayout(preview_buttons);
    preview_layout->addLayout(custom_buttons);

    auto* bottom_splitter = new QSplitter(Qt::Horizontal);
    bottom_splitter->addWidget(details);
    bottom_splitter->addWidget(preview_widget);

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(table);
    splitter->addWidget(bottom_splitter);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    auto* filter_layout = new QHBoxLayout;
    filter_layout->addWidget(filter_edit, 1);
    filter_layout->addWidget(show_sentenced);
    filter_layout->addWidget(used_only);
    filter_layout->addWidget(live_refresh);
    filter_layout->addWidget(live_frames);
    filter_layout->addWidget(refresh_button);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(summary_label);
    layout->addLayout(filter_layout);
    layout->addWidget(splitter, 1);
    layout->addWidget(status_label);

    connect(filter_edit, &QLineEdit::textChanged, filter, &SurfaceCacheFilter::SetText);
    connect(show_sentenced, &QCheckBox::toggled, filter, &SurfaceCacheFilter::SetShowSentenced);
    connect(used_only, &QCheckBox::toggled, filter, &SurfaceCacheFilter::SetUsedOnly);
    connect(live_refresh, &QCheckBox::toggled, this,
            [this] { OnEmulationStateChanged(running, paused); });
    connect(refresh_button, &QPushButton::clicked, this, &SurfaceCacheView::Refresh);
    connect(live_timer, &QTimer::timeout, this, &SurfaceCacheView::RefreshLive);
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &SurfaceCacheView::OnSelectionChanged);
    connect(preview_level, qOverload<int>(&QSpinBox::valueChanged), this,
            &SurfaceCacheView::RequestPreview);
    connect(preview_mode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &SurfaceCacheView::UpdatePreviewImage);
    connect(preview_zoom, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &SurfaceCacheView::UpdatePreviewImage);
    connect(preview_flip, &QCheckBox::toggled, this, &SurfaceCacheView::UpdatePreviewImage);
    connect(preview_save, &QPushButton::clicked, this, &SurfaceCacheView::SavePreview);
    connect(preview_upscaled, &QCheckBox::toggled, this, &SurfaceCacheView::RequestPreview);
    connect(save_upscaled, &QPushButton::clicked, this, &SurfaceCacheView::SaveUpscaled);
    connect(import_button, &QPushButton::clicked, this, [this] { ImportPng(false); });
    connect(import_upscaled_button, &QPushButton::clicked, this, [this] { ImportPng(true); });
    connect(revert_button, &QPushButton::clicked, this, &SurfaceCacheView::RevertImport);
    connect(save_pack_button, &QPushButton::clicked, this, &SurfaceCacheView::SaveToPack);
    connect(open_manual, &QPushButton::clicked, this, [this] {
        if (const SurfaceDebugInfo* surface = SelectedSurface()) {
            emit OpenInManual(surface->params.addr, surface->params.width, surface->params.height,
                              surface->params.pixel_format);
        }
    });

    UpdateSummary();
    UpdateDetails();
    UpdatePreviewImage();
}

SurfaceCacheView::~SurfaceCacheView() {
    if (active) {
        active_views--;
    }
}

void QueueSurfaceDebugTracking(Core::System& system, DebugWorkQueue& work_queue) {
    if (!system.IsPoweredOn()) {
        return;
    }
    work_queue.Queue([&system] {
        if (!system.IsPoweredOn()) {
            return;
        }
        // Decided when the work runs, as views may have changed in the meantime
        const bool enabled = active_views > 0 || VideoCore::GetSurfaceOpLog().IsRecording();
        system.GPU().Renderer().Rasterizer()->SetSurfaceDebugTracking(enabled);
    });
}

void SurfaceCacheView::SetActive(bool active_) {
    if (active == active_) {
        return;
    }
    active = active_;
    if (active) {
        active_views++;
        QueueSurfaceDebugTracking(system, work_queue);
        Refresh();
    } else {
        active_views--;
        QueueSurfaceDebugTracking(system, work_queue);
    }
    OnEmulationStateChanged(running, paused);
}

void SurfaceCacheView::OnEmulationStateChanged(bool running_, bool paused_) {
    const bool started = running_ && !running;
    const bool stopped = !running_ && running;
    const bool paused_now = running_ && paused_ && !(running && paused);
    running = running_;
    paused = paused_;

    if (!running) {
        snapshot_pending = false;
        preview_pending = false;
        refresh_again = false;
        wait_for_frame_after.reset();
    }
    if (stopped) {
        ClearSnapshot();
    }
    if (active && started) {
        QueueSurfaceDebugTracking(system, work_queue);
    }
    if (active && paused_now) {
        Refresh();
    }

    const bool live = active && running && !paused && live_refresh->isChecked();
    if (live && !live_timer->isActive()) {
        live_timer->start();
    } else if (!live) {
        live_timer->stop();
    }
    UpdateStatus();
}

void SurfaceCacheView::UpdateStatus() {
    if (!running) {
        status_label->setText(QStringLiteral("Emulation is not running."));
        return;
    }
    if (!has_info) {
        status_label->setText(
            paused || at_breakpoint
                ? QStringLiteral("Waiting for data...")
                : QStringLiteral("Pause the emulation or use Live to see the cached surfaces."));
        return;
    }
    if (at_breakpoint) {
        const QString event = BreakPointModel::DebugContextEventToString(breakpoint_event);
        status_label->setText(
            snapshot_before_flush
                ? QStringLiteral("Stopped at Pica breakpoint \"%1\" in frame %2.")
                      .arg(event)
                      .arg(usage_frame)
                : QStringLiteral(
                      "Stopped at Pica breakpoint \"%1\" in frame %2. Taken after the breakpoint "
                      "flushed the caches, so nothing is dirty.")
                      .arg(event)
                      .arg(usage_frame));
    } else if (paused) {
        status_label->setText(QStringLiteral("Paused at the end of frame %1.").arg(usage_frame));
    } else if (live_timer->isActive()) {
        status_label->setText(QStringLiteral("Live, frame %1.").arg(usage_frame));
    } else {
        status_label->setText(
            QStringLiteral(
                "Showing frame %1. Pause the emulation or use Live to see the current state.")
                .arg(usage_frame));
    }
}

void SurfaceCacheView::CaptureAtBreakpoint() {
    if (!active || !system.IsPoweredOn()) {
        return;
    }
    auto info = system.GPU().Renderer().Rasterizer()->GetSurfaceDebugInfo();
    if (!info) {
        return;
    }
    QMetaObject::invokeMethod(
        this, [this, info = std::move(*info)]() mutable { OnSnapshot(std::move(info), true); },
        Qt::QueuedConnection);
}

void SurfaceCacheView::OnBreakpoint(bool hit, Pica::DebugContext::Event event) {
    at_breakpoint = hit;
    breakpoint_event = event;
    if (hit) {
        // The snapshot from CaptureAtBreakpoint replaces any step in progress.
        wait_for_frame_after.reset();
    }
    UpdateStatus();
}

void SurfaceCacheView::SelectSurface(u32 index, VideoCore::SurfaceUniqueId unique_id) {
    pending_selection.emplace(index, unique_id);
    if (!running) {
        pending_selection.reset();
        status_label->setText(QStringLiteral("Emulation is not running."));
        return;
    }
    // A view that becomes active refreshes on its own
    if (active) {
        Refresh();
    }
}

bool SurfaceCacheView::SelectPendingSurface() {
    const auto [index, unique_id] = *pending_selection;
    pending_selection.reset();
    const int row = model->RowOfIndex(index);
    if (row < 0 || model->GetSurface(row)->usage.unique_id != unique_id) {
        return false;
    }
    QModelIndex proxy_index = filter->mapFromSource(model->index(row, 0));
    if (!proxy_index.isValid()) {
        // Hidden by the filters
        filter_edit->clear();
        show_sentenced->setChecked(true);
        used_only->setChecked(false);
        proxy_index = filter->mapFromSource(model->index(row, 0));
    }
    table->selectionModel()->setCurrentIndex(proxy_index, QItemSelectionModel::ClearAndSelect |
                                                              QItemSelectionModel::Rows);
    table->scrollTo(proxy_index);
    return true;
}

void SurfaceCacheView::OnFrameStepped() {
    if (!active) {
        return;
    }
    // The snapshot must be taken at the end of the stepped frame, not before the step starts.
    wait_for_frame_after = has_info ? last_info.frame : 0;
    stale_retries = 0;
    Refresh();
}

void SurfaceCacheView::Refresh() {
    RequestSnapshot(0, 0);
}

void SurfaceCacheView::RefreshLive() {
    // The pending request is answered first
    if (snapshot_pending) {
        return;
    }
    if (!has_info) {
        RequestSnapshot(0, 0);
        return;
    }
    RequestSnapshot(last_info.frame, static_cast<u64>(live_frames->value()));
}

void SurfaceCacheView::RequestSnapshot(u64 since_frame, u64 min_frames) {
    if (!active || !system.IsPoweredOn()) {
        return;
    }
    if (snapshot_pending) {
        refresh_again = true;
        return;
    }
    snapshot_pending = true;
    work_queue.Queue([this, &system = system, since_frame, min_frames] {
        std::optional<VideoCore::SurfaceCacheDebugInfo> info;
        bool too_early = false;
        if (system.IsPoweredOn()) {
            auto& renderer = system.GPU().Renderer();
            auto* rasterizer = renderer.Rasterizer();
            if (active_views > 0) {
                rasterizer->SetSurfaceDebugTracking(true);
            }
            // The cache is only copied once enough frames have ended. A frame number below
            // since_frame means the renderer was recreated, so the snapshot is taken.
            const u64 frame = renderer.GetCurrentFrame();
            too_early = frame >= since_frame && frame - since_frame < min_frames;
            if (!too_early) {
                info = rasterizer->GetSurfaceDebugInfo();
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, info = std::move(info), too_early]() mutable {
                snapshot_pending = false;
                if (refresh_again) {
                    refresh_again = false;
                    QTimer::singleShot(0, this, &SurfaceCacheView::Refresh);
                }
                if (too_early) {
                    return;
                }
                if (!info) {
                    status_label->setText(
                        QStringLiteral("The current renderer has no surface cache."));
                    return;
                }
                OnSnapshot(std::move(*info), false);
            },
            Qt::QueuedConnection);
    });

    // Live requests wait for the frames to end, which is not worth a warning
    if (min_frames != 0) {
        return;
    }
    QTimer::singleShot(PENDING_WARNING_MS, this, [this] {
        if (snapshot_pending) {
            status_label->setText(
                QStringLiteral("Waiting for the emulation thread. Snapshots are taken at "
                               "the end of a frame or at a Pica breakpoint, not while "
                               "the CPU debugger holds the emulation."));
        }
    });
}

void SurfaceCacheView::ClearSnapshot() {
    last_info = {};
    has_info = false;
    usage_frame = 0;
    model->SetInfo(last_info, usage_frame);
    preview.reset();
    details_index.reset();
    UpdateSummary();
    UpdateDetails();
    UpdatePreviewImage();
}

void SurfaceCacheView::OnSnapshot(VideoCore::SurfaceCacheDebugInfo info,
                                  bool before_breakpoint_flush) {
    if (!running) {
        return;
    }
    if (wait_for_frame_after && !before_breakpoint_flush) {
        if (info.frame <= *wait_for_frame_after && paused && stale_retries < STEP_RETRY_LIMIT) {
            stale_retries++;
            QTimer::singleShot(STEP_RETRY_INTERVAL_MS, this, &SurfaceCacheView::Refresh);
            return;
        }
        wait_for_frame_after.reset();
    }

    const SurfaceDebugInfo* selected = SelectedSurface();
    const std::optional<u32> selected_index =
        selected ? std::optional<u32>{selected->index} : std::nullopt;

    const bool first = !has_info;
    last_info = std::move(info);
    has_info = true;
    // The renderer counts a frame as soon as it ends, so a snapshot taken at the end of a frame
    // sees usage stamped with the previous frame number.
    usage_frame = before_breakpoint_flush || at_breakpoint || last_info.frame == 0
                      ? last_info.frame
                      : last_info.frame - 1;
    const int table_x = table->horizontalScrollBar()->value();
    const int table_y = table->verticalScrollBar()->value();
    model->SetInfo(last_info, usage_frame);
    if (first) {
        table->header()->resizeSections(QHeaderView::ResizeToContents);
    }

    if (selected_index) {
        const int row = model->RowOfIndex(*selected_index);
        if (row >= 0) {
            const QModelIndex index = filter->mapFromSource(model->index(row, 0));
            table->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
        }
    }
    if (!first) {
        table->doItemsLayout();
        table->horizontalScrollBar()->setValue(table_x);
        table->verticalScrollBar()->setValue(table_y);
    }

    snapshot_before_flush = before_breakpoint_flush;
    UpdateStatus();
    if (pending_selection) {
        const VideoCore::SurfaceUniqueId unique_id = pending_selection->second;
        if (!SelectPendingSurface()) {
            status_label->setText(
                QStringLiteral("Surface #%1 is no longer in the cache.").arg(unique_id));
        }
    }
    UpdateSummary();
    UpdateDetails();
    RequestPreview();
}

const SurfaceDebugInfo* SurfaceCacheView::SelectedSurface() const {
    const QModelIndex index = table->selectionModel()->currentIndex();
    if (!index.isValid()) {
        return nullptr;
    }
    return model->GetSurface(filter->mapToSource(index).row());
}

void SurfaceCacheView::OnSelectionChanged() {
    const SurfaceDebugInfo* surface = SelectedSurface();
    {
        const QSignalBlocker blocker(preview_level);
        preview_level->setRange(0, surface ? static_cast<int>(surface->params.levels) - 1 : 0);
    }
    if (surface && surface->index != preview_index) {
        preview.reset();
        const bool depth = surface->params.type == VideoCore::SurfaceType::Depth ||
                           surface->params.type == VideoCore::SurfaceType::DepthStencil;
        const QSignalBlocker blocker(preview_mode);
        if (depth && preview_mode->currentIndex() < 3) {
            preview_mode->setCurrentIndex(3);
        } else if (!depth && preview_mode->currentIndex() >= 3) {
            preview_mode->setCurrentIndex(0);
        }
    }
    UpdateDetails();
    RequestPreview();
}

void SurfaceCacheView::RequestPreview() {
    const SurfaceDebugInfo* surface = SelectedSurface();
    if (!surface) {
        preview.reset();
        UpdatePreviewImage();
        return;
    }
    if (preview_pending || !system.IsPoweredOn()) {
        return;
    }
    if (surface->params.type == VideoCore::SurfaceType::Fill) {
        preview.reset();
        preview_index = surface->index;
        UpdatePreviewImage();
        return;
    }

    preview_pending = true;
    const u32 index = surface->index;
    const VideoCore::SurfaceParams params = surface->params;
    const u32 level = static_cast<u32>(preview_level->value());
    const bool upscaled = preview_upscaled->isChecked();
    preview_read_level = level;
    preview_read_upscaled = upscaled;
    work_queue.Queue([this, &system = system, index, params, level, upscaled] {
        VideoCore::SurfaceDebugImage image;
        if (system.IsPoweredOn()) {
            image = system.GPU().Renderer().Rasterizer()->ReadSurfaceForDebug(index, params, level,
                                                                              upscaled);
        } else {
            image.error = "Emulation is not running";
        }
        QMetaObject::invokeMethod(
            this,
            [this, image = std::move(image), index]() mutable {
                preview_pending = false;
                OnPreview(std::move(image), index);
            },
            Qt::QueuedConnection);
    });
}

void SurfaceCacheView::OnPreview(VideoCore::SurfaceDebugImage image, u32 index) {
    if (!running) {
        return;
    }
    preview = std::move(image);
    preview_index = index;
    UpdatePreviewImage();

    // The selection, level or scale may have changed while the read was pending.
    const SurfaceDebugInfo* surface = SelectedSurface();
    if (surface && (surface->index != index ||
                    preview_read_level != static_cast<u32>(preview_level->value()) ||
                    preview_read_upscaled != preview_upscaled->isChecked())) {
        RequestPreview();
    }
}

void SurfaceCacheView::UpdatePreviewImage() {
    const SurfaceDebugInfo* surface = SelectedSurface();
    preview_image = {};
    open_manual->setEnabled(surface != nullptr);

    if (surface && surface->params.type == VideoCore::SurfaceType::Fill) {
        const auto& fill = surface->fill_data;
        QStringList bytes;
        for (u32 i = 0; i < surface->fill_size; i++) {
            bytes << QStringLiteral("%1").arg(fill[i], 2, 16, QLatin1Char('0'));
        }
        preview_status->setText(QStringLiteral("Fill surface with the %1-byte pattern %2")
                                    .arg(surface->fill_size)
                                    .arg(bytes.join(QLatin1Char(' '))));
    } else if (!surface || !preview || preview_index != surface->index) {
        preview_status->setText(surface ? QStringLiteral("Reading the surface...") : QString{});
    } else if (!preview->error.empty()) {
        preview_status->setText(QString::fromStdString(preview->error));
    } else {
        const u32 width = preview->width;
        const u32 height = preview->height;
        const int mode = preview_mode->currentIndex();
        QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);

        u32 depth_min = 0;
        u32 depth_max = 0;
        if (!preview->depth.empty()) {
            const auto [min_it, max_it] =
                std::minmax_element(preview->depth.begin(), preview->depth.end());
            depth_min = *min_it;
            depth_max = *max_it;
        }

        for (u32 y = 0; y < height; y++) {
            const u32 out_y = preview_flip->isChecked() ? height - 1 - y : y;
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(out_y)));
            for (u32 x = 0; x < width; x++) {
                const u32 index = y * width + x;
                const u8* rgba = &preview->rgba[index * 4];
                const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
                const int background = checker ? 0xCC : 0x99;
                QRgb color{};
                switch (mode) {
                case 0: {
                    const int alpha = rgba[3];
                    const auto blend = [&](int c) {
                        return (c * alpha + background * (255 - alpha)) / 255;
                    };
                    color = qRgb(blend(rgba[0]), blend(rgba[1]), blend(rgba[2]));
                    break;
                }
                case 1:
                    color = qRgb(rgba[0], rgba[1], rgba[2]);
                    break;
                case 2:
                    color = qRgb(rgba[3], rgba[3], rgba[3]);
                    break;
                case 3: {
                    int value = rgba[0];
                    if (!preview->depth.empty()) {
                        const u32 range = std::max(depth_max - depth_min, 1u);
                        value = static_cast<int>(
                            static_cast<u64>(preview->depth[index] - depth_min) * 255 / range);
                    }
                    color = qRgb(value, value, value);
                    break;
                }
                case 4: {
                    const int value = preview->stencil.empty() ? 0 : preview->stencil[index];
                    color = qRgb(value, value, value);
                    break;
                }
                }
                line[x] = color;
            }
        }
        preview_image = image;

        QString status = QStringLiteral("%1x%2").arg(width).arg(height);
        if (!preview->depth.empty()) {
            status += QStringLiteral(", depth from %1 to %2 (of %3)")
                          .arg(Hex(depth_min))
                          .arg(Hex(depth_max))
                          .arg(Hex((1ull << preview->depth_bits) - 1));
        }
        if (preview->from_custom_texture) {
            status += QStringLiteral(", custom texture");
        } else if (preview->from_guest_memory) {
            status += QStringLiteral(", decoded from guest memory");
        } else if (preview->scale > 1) {
            status += QStringLiteral(", read back from the GPU at %1x scale").arg(preview->scale);
        } else {
            status += QStringLiteral(", read back from the GPU at native resolution");
            if (preview_read_upscaled && !preview->native_reason.empty()) {
                status +=
                    QStringLiteral(" (%1)").arg(QString::fromStdString(preview->native_reason));
            }
        }
        preview_status->setText(status);
    }

    preview_save->setEnabled(!preview_image.isNull());
    save_upscaled->setEnabled(surface != nullptr &&
                              surface->params.type != VideoCore::SurfaceType::Fill &&
                              !save_upscaled_pending && system.IsPoweredOn());
    UpdateActionButtons();
    if (preview_image.isNull()) {
        preview_label->setPixmap({});
        return;
    }
    const int zoom = 1 << preview_zoom->currentIndex();
    preview_label->setPixmap(QPixmap::fromImage(
        preview_image.scaled(preview_image.width() * zoom, preview_image.height() * zoom,
                             Qt::IgnoreAspectRatio, Qt::FastTransformation)));
}

void SurfaceCacheView::UpdateDetails() {
    const SurfaceDebugInfo* surface = SelectedSurface();
    if (!surface) {
        details_index.reset();
        details->setHtml(QStringLiteral("Select a surface to see its details."));
        return;
    }
    const auto& params = surface->params;
    QString html = QStringLiteral("<table>");
    const auto row = [&html](const QString& name, const QString& value) {
        html += QStringLiteral("<tr><td><b>%1</b>&nbsp;&nbsp;</td><td>%2</td></tr>")
                    .arg(name.toHtmlEscaped(), value.toHtmlEscaped());
    };
    const auto regions = [](const std::vector<std::pair<PAddr, PAddr>>& list) {
        constexpr std::size_t max_regions = 32;
        QStringList parts;
        for (std::size_t i = 0; i < std::min(list.size(), max_regions); i++) {
            parts << QStringLiteral("%1-%2").arg(Hex(list[i].first), Hex(list[i].second));
        }
        if (list.size() > max_regions) {
            parts << QStringLiteral("... (%1 more)").arg(list.size() - max_regions);
        }
        return parts.join(QStringLiteral(", "));
    };

    row(QStringLiteral("Slot"), QString::number(surface->index));
    if (surface->usage.unique_id != 0) {
        row(QStringLiteral("Unique ID"), QString::number(surface->usage.unique_id));
    }
    row(QStringLiteral("State"), SurfaceCacheModel::StateText(*surface));
    if (surface->sentenced) {
        row(QStringLiteral("Recycle"), SurfaceCacheModel::RecycleText(*surface));
        // OpenGL can recycle a surface at any tick and reports the largest one
        const bool any_tick = last_info.resource_recycle_tick == std::numeric_limits<u64>::max();
        row(QStringLiteral("Sentenced"),
            QStringLiteral("frame %1, resource tick %2 (free tick %3, recycle tick %4)")
                .arg(surface->sentenced_frame)
                .arg(surface->sentenced_resource_tick)
                .arg(last_info.resource_free_tick)
                .arg(any_tick ? QStringLiteral("any")
                              : QString::number(last_info.resource_recycle_tick)));
    }
    row(QStringLiteral("Range"),
        QStringLiteral("%1-%2 (%3)").arg(Hex(params.addr), Hex(params.end), Bytes(params.size)));
    row(QStringLiteral("Size"), QStringLiteral("%1x%2, stride %3, %4 level(s)")
                                    .arg(params.width)
                                    .arg(params.height)
                                    .arg(params.stride)
                                    .arg(params.levels));
    row(QStringLiteral("Scaled"), QStringLiteral("%1x%2 (scale %3, %4 sample(s))")
                                      .arg(params.GetScaledWidth())
                                      .arg(params.GetScaledHeight())
                                      .arg(params.res_scale)
                                      .arg(params.sample_count));
    row(QStringLiteral("Format"), FormatText(params));
    row(QStringLiteral("Type"), SurfaceCacheModel::TypeText(params));
    row(QStringLiteral("Tiled"), params.is_tiled ? QStringLiteral("yes") : QStringLiteral("no"));
    row(QStringLiteral("Flags"),
        QStringLiteral("%1 (%2)").arg(SurfaceCacheModel::FlagsText(surface->flags),
                                      Hex(surface->flags)));
    if (True(surface->usage.last_usage)) {
        row(QStringLiteral("Last used"),
            QStringLiteral("frame %1 as %2")
                .arg(surface->usage.last_used_frame)
                .arg(SurfaceCacheModel::UsageText(surface->usage.last_usage)));
    } else {
        row(QStringLiteral("Last used"), QStringLiteral("not used since tracking started"));
    }
    if (surface->usage.creation_tracked) {
        row(QStringLiteral("Created"),
            QStringLiteral("frame %1").arg(surface->usage.created_frame));
    }
    row(QStringLiteral("Recycled"),
        QStringLiteral("%1 time(s)").arg(static_cast<int>(surface->usage.recycle_count)));
    row(QStringLiteral("Modification tick"), QString::number(surface->modification_tick));
    row(QStringLiteral("Invalid"),
        surface->fully_invalid ? QStringLiteral("all")
                               : QStringLiteral("%1 %2").arg(Bytes(surface->invalid_bytes),
                                                             regions(surface->invalid_regions)));
    row(QStringLiteral("Dirty"),
        QStringLiteral("%1 %2").arg(Bytes(surface->dirty_bytes), regions(surface->dirty_regions)));
    if (params.type == VideoCore::SurfaceType::Fill) {
        row(QStringLiteral("Fill size"), QString::number(surface->fill_size));
    }
    if (surface->has_material) {
        row(QStringLiteral("Custom texture"),
            QStringLiteral("hash %1, %2x%3%4")
                .arg(QStringLiteral("%1").arg(surface->material_hash, 16, 16, QLatin1Char('0')))
                .arg(surface->material_width)
                .arg(surface->material_height)
                .arg(surface->has_normal_map ? QStringLiteral(", normal map") : QString{}));
        row(QStringLiteral("Custom texture file"),
            surface->material_imported
                ? QStringLiteral("imported from %1")
                      .arg(QString::fromStdString(surface->material_path).toHtmlEscaped())
                : QString::fromStdString(surface->material_path).toHtmlEscaped());
    }
    row(QStringLiteral("References"), QStringLiteral("%1 framebuffer(s), %2 cube(s)")
                                          .arg(surface->framebuffer_refs)
                                          .arg(surface->cube_refs));
    row(QStringLiteral("Uploaded"), Bytes(surface->usage.uploaded_bytes));
    row(QStringLiteral("Downloaded"), Bytes(surface->usage.downloaded_bytes));
    row(QStringLiteral("Host memory"), Bytes(surface->host_bytes));
    html += QStringLiteral("</table>");

    // A refresh of the same surface keeps the scroll position, which setHtml resets
    const bool same_surface = details_index == surface->index;
    const int details_x = details->horizontalScrollBar()->value();
    const int details_y = details->verticalScrollBar()->value();
    details->setHtml(html);
    details_index = surface->index;
    if (same_surface) {
        details->horizontalScrollBar()->setValue(details_x);
        details->verticalScrollBar()->setValue(details_y);
    }
}

void SurfaceCacheView::UpdateSummary() {
    if (!has_info) {
        summary_label->setText(QStringLiteral("No data yet."));
        return;
    }
    u32 surfaces = 0;
    u32 registered = 0;
    u32 sentenced = 0;
    u32 gpu_busy = 0;
    u32 recyclable = 0;
    u32 pending_deletion = 0;
    u32 used = 0;
    u32 fills = 0;
    u32 fills_used = 0;
    u64 host_bytes = 0;
    for (const SurfaceDebugInfo& surface : last_info.surfaces) {
        // Fill surfaces only hold a pattern, are never sentenced and are erased directly
        if (surface.params.type == VideoCore::SurfaceType::Fill) {
            fills++;
            fills_used += UsedInFrame(surface, usage_frame) ? 1 : 0;
            continue;
        }
        surfaces++;
        host_bytes += surface.host_bytes;
        if (UsedInFrame(surface, usage_frame)) {
            used++;
        }
        if (surface.sentenced) {
            sentenced++;
            gpu_busy += surface.gpu_in_use ? 1 : 0;
            recyclable += surface.recyclable ? 1 : 0;
            pending_deletion += !surface.gpu_in_use && surface.frames_until_deletion == 0;
        } else if (surface.flags & static_cast<u32>(VideoCore::SurfaceFlagBits::Registered)) {
            registered++;
        }
    }
    const auto& counters = last_info.counters;
    const QString indent = QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;");
    const QStringList lines{
        QStringLiteral("Frame %1:").arg(usage_frame),
        indent + QStringLiteral(
                     "Surfaces: total: %1, registered: %2, used this frame: %3. Sentenced: %4 ("
                     "recyclable: %5, GPU busy: %6, pending deletion: %7). Host memory "
                     "~%8.")
                     .arg(surfaces)
                     .arg(registered)
                     .arg(used)
                     .arg(sentenced)
                     .arg(recyclable)
                     .arg(gpu_busy)
                     .arg(pending_deletion)
                     .arg(Bytes(host_bytes)),
        indent +
            QStringLiteral("Fills: total: %1, used this frame: %2.").arg(fills).arg(fills_used),
        QStringLiteral("Since tracking started:"),
        indent + QStringLiteral("Surfaces: created: %1, recycled: %2, sentenced: %3, deleted: %4.")
                     .arg(counters.created)
                     .arg(counters.recycled)
                     .arg(counters.sentenced)
                     .arg(counters.deleted),
        indent + QStringLiteral("Fills: created: %1, erased: %2.")
                     .arg(counters.fills_created)
                     .arg(counters.fills_erased),
    };
    summary_label->setText((lines + TransferSummary()).join(QStringLiteral("<br>")));
}

QStringList SurfaceCacheView::TransferSummary() const {
    using VideoCore::SurfaceTransferStats;
    const QString indent = QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;");
    const QChar space = QLatin1Char(' ');
    const auto count = [](u64 value) {
        return static_cast<int>(std::min<u64>(value, std::numeric_limits<int>::max()));
    };
    const auto describe = [&](const SurfaceTransferStats& stats) {
        QStringList description{
            QStringLiteral("Uploaded %1 in %2 upload(s).")
                .arg(Bytes(stats.upload_bytes))
                .arg(count(stats.uploads)),
        };
        if (stats.custom_uploads != 0) {
            description << QStringLiteral("Uploaded %1 of custom textures in %2 upload(s).")
                               .arg(Bytes(stats.custom_upload_bytes))
                               .arg(count(stats.custom_uploads));
        }
        description << QStringLiteral("Downloaded %1 in %2 download(s).")
                           .arg(Bytes(stats.download_bytes))
                           .arg(count(stats.downloads));
        if (stats.fill_downloads != 0) {
            description << QStringLiteral("Wrote %1 of fills.").arg(Bytes(stats.fill_bytes));
        }
        return description.join(space);
    };

    // The window ends at the frame the usage is shown for, and starts when tracking started
    // if that is less than an emulated second ago
    const u64 end = usage_frame;
    const u64 tracked = end >= last_info.tracking_frame ? end - last_info.tracking_frame + 1 : 1;
    const u64 window = std::min<u64>(VideoCore::TRANSFER_HISTORY_FRAMES, tracked);
    SurfaceTransferStats sum{};
    SurfaceTransferStats frame_stats{};
    SurfaceTransferStats peak_upload{};
    SurfaceTransferStats peak_download{};
    for (const SurfaceTransferStats& stats : last_info.transfer_history) {
        if (stats.frame > end || stats.frame + window <= end) {
            continue;
        }
        sum.Add(stats);
        if (stats.frame == end) {
            frame_stats = stats;
        }
        if (stats.HostUploadBytes() > peak_upload.HostUploadBytes()) {
            peak_upload = stats;
        }
        if (stats.download_bytes > peak_download.download_bytes) {
            peak_download = stats;
        }
    }

    const double per_second = SCREEN_REFRESH_RATE / static_cast<double>(window);
    QString uploads = QStringLiteral("Uploads: %2 per second, %1.")
                          .arg(BytesPerSecond(sum.HostUploadBytes() * per_second))
                          .arg((sum.uploads + sum.custom_uploads) * per_second, 0, 'f', 1);
    if (sum.custom_uploads != 0) {
        uploads += space + QStringLiteral("Custom textures: %1.")
                               .arg(BytesPerSecond(sum.custom_upload_bytes * per_second));
    }
    if (peak_upload.HostUploadBytes() != 0) {
        uploads += space + QStringLiteral("Peak: %1 in frame %2.")
                               .arg(Bytes(peak_upload.HostUploadBytes()))
                               .arg(peak_upload.frame);
    }
    QString downloads = QStringLiteral("Downloads: %2 per second, %1.")
                            .arg(BytesPerSecond(sum.download_bytes * per_second))
                            .arg(sum.downloads * per_second, 0, 'f', 1);
    if (peak_download.download_bytes != 0) {
        downloads += space + QStringLiteral("Peak: %1 in frame %2.")
                                 .arg(Bytes(peak_download.download_bytes))
                                 .arg(peak_download.frame);
    }

    QStringList lines{
        QStringLiteral("Host GPU transfers per emulated second, over the last %1 frame(s):")
            .arg(count(window)),
        indent + uploads,
        indent + downloads,
    };
    if (sum.fill_downloads != 0) {
        lines << indent + QStringLiteral("Fills written to guest memory by the CPU: %1.")
                              .arg(BytesPerSecond(sum.fill_bytes * per_second));
    }
    lines << indent + QStringLiteral("Frame %1:").arg(end) + space + describe(frame_stats);
    lines << indent + QStringLiteral("Since tracking started:") + space +
                 describe(last_info.transfers);
    return lines;
}

void SurfaceCacheView::SavePreview() {
    if (preview_image.isNull()) {
        return;
    }
    // The Color view blends alpha over a checkerboard, so it saves the pixels with their alpha
    // instead. The other views save what they show.
    const bool real_alpha = preview_mode->currentIndex() == 0 && preview && preview->error.empty();
    const QImage image = real_alpha ? ToImage(*preview, preview_flip->isChecked()) : preview_image;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Surface"),
        QStringLiteral("surface_%1_frame%2.png").arg(preview_index).arg(usage_frame),
        QStringLiteral("Portable Network Graphic (*.png)"));
    if (!path.isEmpty()) {
        image.save(path, "PNG");
    }
}

void SurfaceCacheView::SaveUpscaled() {
    const SurfaceDebugInfo* surface = SelectedSurface();
    if (!surface || save_upscaled_pending || !system.IsPoweredOn()) {
        return;
    }
    save_upscaled_pending = true;
    save_upscaled->setEnabled(false);
    const u32 index = surface->index;
    const VideoCore::SurfaceParams params = surface->params;
    const u32 level = static_cast<u32>(preview_level->value());
    work_queue.Queue([this, &system = system, index, params, level] {
        VideoCore::SurfaceDebugImage image;
        if (system.IsPoweredOn()) {
            image = system.GPU().Renderer().Rasterizer()->ReadSurfaceForDebug(index, params, level,
                                                                              true);
        } else {
            image.error = "Emulation is not running";
        }
        QMetaObject::invokeMethod(
            this,
            [this, &system, image = std::move(image), index, level]() mutable {
                save_upscaled_pending = false;
                const SurfaceDebugInfo* selected = SelectedSurface();
                save_upscaled->setEnabled(selected != nullptr &&
                                          selected->params.type != VideoCore::SurfaceType::Fill &&
                                          system.IsPoweredOn());
                OnSaveUpscaled(std::move(image), index, level);
            },
            Qt::QueuedConnection);
    });
}

void SurfaceCacheView::OnSaveUpscaled(VideoCore::SurfaceDebugImage image, u32 index, u32 level) {
    if (!image.error.empty()) {
        QMessageBox::warning(this, QStringLiteral("Surface Viewer"),
                             QString::fromStdString(image.error));
        return;
    }
    const QString path =
        QFileDialog::getSaveFileName(this, QStringLiteral("Save Upscaled Surface"),
                                     QStringLiteral("surface_%1_level%2_frame%3_%4x.png")
                                         .arg(index)
                                         .arg(level)
                                         .arg(usage_frame)
                                         .arg(image.scale),
                                     QStringLiteral("Portable Network Graphic (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    if (!ToImage(image, preview_flip->isChecked()).save(path, "PNG")) {
        QMessageBox::warning(this, QStringLiteral("Surface Viewer"),
                             QStringLiteral("Unable to write %1").arg(path));
    }
}

void SurfaceCacheView::UpdateActionButtons() {
    const SurfaceDebugInfo* surface = SelectedSurface();
    const bool can_act = surface && !action_pending && system.IsPoweredOn();
    if (!can_act) {
        import_button->setEnabled(false);
        import_upscaled_button->setEnabled(false);
        revert_button->setEnabled(false);
        save_pack_button->setEnabled(false);
        return;
    }
    const auto& params = surface->params;
    const bool color = params.type == VideoCore::SurfaceType::Color ||
                       params.type == VideoCore::SurfaceType::Texture;
    const bool render_target =
        (surface->flags & static_cast<u32>(VideoCore::SurfaceFlagBits::RenderTarget)) != 0;
    const bool custom = params.custom_format != VideoCore::CustomPixelFormat::Invalid &&
                        (surface->flags & static_cast<u32>(VideoCore::SurfaceFlagBits::Custom));
    const bool imported = custom && surface->material_imported;
    const bool replaceable = color && !render_target && !surface->sentenced &&
                             params.texture_type == VideoCore::TextureType::Texture2D;

    import_button->setEnabled(replaceable);
    import_upscaled_button->setEnabled(replaceable);
    revert_button->setEnabled(imported);
    save_pack_button->setEnabled(color && (!custom || (imported && preview_level->value() == 0)));
}

QString SurfaceCacheView::CustomTextureDir() const {
    if (!has_info) {
        return {};
    }
    const auto title_dir = [this](FileUtil::UserPath user_path) {
        return fmt::format("{}textures/{:016X}/", FileUtil::GetUserPath(user_path),
                           last_info.program_id);
    };
    std::string path = title_dir(FileUtil::UserPath::DumpDir);
    if (!FileUtil::Exists(path)) {
        path = title_dir(FileUtil::UserPath::LoadDir);
    }
    return QString::fromStdString(path);
}

void SurfaceCacheView::ImportPng(bool upscaled) {
    const QString path = QFileDialog::getOpenFileName(
        this, upscaled ? QStringLiteral("Import Upscaled PNG") : QStringLiteral("Import PNG"),
        CustomTextureDir(), QStringLiteral("Portable Network Graphic (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    RunSurfaceAction(
        [file = path.toStdString(), upscaled](VideoCore::RasterizerInterface& rasterizer,
                                              const SurfaceDebugInfo& surface) {
            return rasterizer.ImportSurfaceForDebug(surface.index, surface.params, file, upscaled);
        });
}

void SurfaceCacheView::RevertImport() {
    RunSurfaceAction(
        [](VideoCore::RasterizerInterface& rasterizer, const SurfaceDebugInfo& surface) {
            return rasterizer.RevertSurfaceImportForDebug(surface.index, surface.params);
        });
}

void SurfaceCacheView::SaveToPack() {
    const u32 level = static_cast<u32>(preview_level->value());
    RunSurfaceAction(
        [level](VideoCore::RasterizerInterface& rasterizer, const SurfaceDebugInfo& surface) {
            return rasterizer.SaveSurfaceToPackForDebug(surface.index, surface.params, level);
        });
}

void SurfaceCacheView::RunSurfaceAction(SurfaceAction action) {
    const SurfaceDebugInfo* surface = SelectedSurface();
    if (!surface || action_pending || !system.IsPoweredOn()) {
        return;
    }
    action_pending = true;
    UpdateActionButtons();

    work_queue.Queue([this, &system = system, info = *surface, action = std::move(action)] {
        VideoCore::SurfaceDebugResult result;
        if (system.IsPoweredOn()) {
            result = action(*system.GPU().Renderer().Rasterizer(), info);
        } else {
            result.error = "Emulation is not running";
        }
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)] {
                action_pending = false;
                if (!result.error.empty()) {
                    QMessageBox::warning(this, QStringLiteral("Surface Viewer"),
                                         QString::fromStdString(result.error));
                }
                UpdateActionButtons();
                Refresh();
            },
            Qt::QueuedConnection);
    });
}
