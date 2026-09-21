#pragma once

#include <QMenu>
#include "core/types.h"

namespace core { class MihomoClient; }

namespace ui {

class TrayProxyMenu : public QMenu {
    Q_OBJECT
public:
    explicit TrayProxyMenu(core::MihomoClient *client, QWidget *parent = nullptr);

private:
    void rebuild();
    void choose(const QString &group, const QString &node);
    void reset(const QString &group);
    core::MihomoClient *client_;
    QVector<core::ProxyGroup> groups_;
    QList<QMenu *> submenus_;
};

}  // namespace ui
