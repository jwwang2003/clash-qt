#pragma once

#include <QWidget>

#include "app/app_context.h"

class QLabel;
class QListWidget;
class QLineEdit;
class QPushButton;
class QProgressDialog;

namespace core { class BackupStore; }

namespace ui {

class BackupPage : public QWidget {
    Q_OBJECT
public:
    explicit BackupPage(const app::Context &context, QWidget *parent = nullptr);

private:
    void refresh();
    void restore();

    app::Context context_;
    core::BackupStore *store_;
    QListWidget *list_;
    QLabel *status_;
    QLineEdit *url_;
    QLineEdit *username_;
    QLineEdit *password_;
    QPushButton *restore_;
    QProgressDialog *progress_ = nullptr;
    bool waitingForCore_ = false;
    bool waitingForFiles_ = false;
};

}  // namespace ui
