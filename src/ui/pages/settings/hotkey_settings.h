#pragma once

#include <QVector>
#include <QWidget>

class QGridLayout;
class QKeySequenceEdit;
class QLabel;

namespace platform {
class Hotkeys;
}

namespace ui {

/// Hotkey ids, shared with whoever acts on platform::Hotkeys::triggered.
namespace hotkey {
constexpr auto kToggleWindow = "window.toggle";
constexpr auto kToggleProxy = "proxy.toggle";
constexpr auto kCycleMode = "mode.cycle";
}  // namespace hotkey

/// The action / shortcut table. A binding the OS refuses is reported on its own
/// row: another app already owning the combination is the usual cause, and the
/// only fix is picking a different one.
class HotkeySettings : public QWidget {
    Q_OBJECT

public:
    explicit HotkeySettings(platform::Hotkeys *hotkeys, QWidget *parent = nullptr);

private:
    struct Row {
        QString id;
        QKeySequenceEdit *edit;
        QLabel *error;
    };

    void addRow(const char *id, const QString &label);
    void apply(int index);

    platform::Hotkeys *hotkeys_;
    QGridLayout *grid_;
    QVector<Row> rows_;
};

}  // namespace ui
