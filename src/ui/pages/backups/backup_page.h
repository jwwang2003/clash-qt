#pragma once

#include <QWidget>

#include "app/app_context.h"

class QLabel;
class QListWidget;
class QLineEdit;
class QPushButton;
class QProgressDialog;

namespace app::backup { class BackupCoordinator; }
namespace core { class BackupStore; }

namespace ui {

class BackupPage : public QWidget {
    Q_OBJECT
public:
    /// The page is a view now. `backups` owns the store, the maintenance gate,
    /// the pending-write gate, the core stop that precedes a restore and the
    /// reload that follows it; the page keeps the widgets - the list, the
    /// buttons, the status label and the progress dialog - and binds them to the
    /// store it is handed. `backups` is not owned and must outlive the page.
    explicit BackupPage(const app::Context &context, app::backup::BackupCoordinator &backups,
                        QWidget *parent = nullptr);

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
};

}  // namespace ui
