#pragma once

#include <QMenu>
#include "core/types.h"

namespace core::backend { class BackendBridge; }

namespace ui {

class TrayProxyMenu : public QMenu {
    Q_OBJECT
public:
    explicit TrayProxyMenu(core::backend::BackendBridge *backend, QWidget *parent = nullptr);

private:
    void rebuild();
    void choose(const QString &group, const QString &node);
    void reset(const QString &group);
    core::backend::BackendBridge *backend_;
    QVector<core::ProxyGroup> groups_;
    QList<QMenu *> submenus_;
};

}  // namespace ui
