// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <memory>
#include <QAbstractListModel>
#include "video_core/debug_utils/debug_utils.h"

class BreakPointModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum {
        Role_IsEnabled = Qt::UserRole,
    };

    BreakPointModel(std::shared_ptr<Pica::DebugContext> context, QObject* parent);

    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    void OnBreakPointHit(Pica::DebugContext::Event event);
    void OnResumed();

    static QString DebugContextEventToString(Pica::DebugContext::Event event);

private:
    std::weak_ptr<Pica::DebugContext> context_weak;
    bool at_breakpoint;
    Pica::DebugContext::Event active_breakpoint;
};
