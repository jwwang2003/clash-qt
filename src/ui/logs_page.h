#pragma once

#include <QWidget>

#include "core/types.h"

class QComboBox;
class QPlainTextEdit;
class QPushButton;

namespace core {
class MihomoClient;
}

namespace ui {

class LogsPage : public QWidget {
    Q_OBJECT

public:
    explicit LogsPage(core::MihomoClient *client, QWidget *parent = nullptr);

public slots:
    /// stdout of a core we launched ourselves, interleaved with mihomo's own stream.
    void appendCoreLine(const QString &line);

private slots:
    void onLogReceived(const core::LogEntry &entry);

private:
    core::MihomoClient *client_;
    QPlainTextEdit *view_;
    QComboBox *levelBox_;
    QPushButton *pauseButton_;
};

}  // namespace ui
