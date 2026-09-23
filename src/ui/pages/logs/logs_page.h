#pragma once

#include <QWidget>
#include <QList>

#include "core/backend/types.h"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace core::backend {
class BackendBridge;
}

namespace ui {

class LogsPage : public QWidget {
    Q_OBJECT

public:
    explicit LogsPage(core::backend::BackendBridge *bridge, QWidget *parent = nullptr);

public slots:
    /// stdout of a core we launched ourselves, interleaved with mihomo's own stream.
    void appendCoreLine(const QString &line);

private slots:
    void onLogReceived(const core::backend::LogEntry &entry);

private:
    void appendEntry(const core::backend::LogEntry &entry);
    void renderEntries();
    bool matchesFilter(const core::backend::LogEntry &entry) const;
    QString entryHtml(const core::backend::LogEntry &entry) const;

    core::backend::BackendBridge *bridge_;
    QPlainTextEdit *view_;
    QComboBox *levelBox_;
    QPushButton *pauseButton_;
    QLineEdit *filterEdit_;
    QList<core::backend::LogEntry> entries_;
};

}  // namespace ui
