#pragma once

#include <QWidget>

namespace core::backend { class BackendBridge; }

namespace ui {

/// Read-only runtime overview and explicitly requested controller diagnostics.
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(core::backend::BackendBridge *bridge, QWidget *parent = nullptr);
};

} // namespace ui
