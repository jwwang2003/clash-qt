#pragma once

#include <QToolButton>

#include "core/process/core_process.h"

class QMenu;

namespace ui {

/// Opens the core's bundled web dashboard. Only a core started from here
/// serves /ui, so the button follows CoreProcess's state.
class DashboardButton : public QToolButton {
    Q_OBJECT

public:
    explicit DashboardButton(core::CoreProcess *coreProcess, QWidget *parent = nullptr);

private:
    void openDashboard();
    void chooseBrowser(const QString &browserId);
    void rebuildMenu();
    void applyCoreState(core::CoreState state);

    core::CoreProcess *coreProcess_;
    QMenu *menu_;
    QString browserId_;
};

}  // namespace ui
