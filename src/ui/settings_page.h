#pragma once

#include <QWidget>

#include "app_context.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace ui {

class SettingsSection;

class SettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(const app::Context &context, QWidget *parent = nullptr);

public slots:
    /// Flips the system proxy; also where the "toggle system proxy" hotkey lands.
    void toggleSystemProxy();

private:
    void buildSystemProxy(QVBoxLayout *column);
    void buildStartup(QVBoxLayout *column);
    void buildHotkeys(QVBoxLayout *column);
    void buildChain(QVBoxLayout *column);
    void applySystemProxy(bool enabled);
    void refreshSystemProxy();

    app::Context context_;
    SettingsSection *proxySection_;
    SettingsSection *startupSection_;
    QCheckBox *proxyToggle_;
    QCheckBox *startupToggle_;
    QLineEdit *bypassEdit_;
    QLabel *proxyState_;
    quint16 corePort_ = 0;
};

}  // namespace ui
