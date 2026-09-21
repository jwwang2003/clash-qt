#pragma once

#include <QWidget>

namespace core { class MihomoClient; }

namespace ui {

/// Read-only runtime overview and explicitly requested controller diagnostics.
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(core::MihomoClient *client, QWidget *parent = nullptr);
};

} // namespace ui
