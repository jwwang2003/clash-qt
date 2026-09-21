#include "ui/pages/backups/backup_page.h"

#include <QFileDialog>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include "core/backups/backup_store.h"
#include "core/config/enhance/config_enhancer.h"
#include "core/mihomo/process/core_process.h"
#include "core/preferences/preferences.h"
#include "core/profiles/profile_store.h"
#include "ui/widgets/settings_section.h"
#include "ui/theme/theme.h"

namespace ui {

BackupPage::BackupPage(const app::Context &context, QWidget *parent)
    : QWidget(parent), context_(context), store_(new core::BackupStore(context.profiles->dataDir(), this)) {
    store_->requirePreparation(true);
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, theme::kPageSpacing, 0);
    column->setSpacing(theme::kPageSpacing);
    auto *local = new SettingsSection(tr("Local backups"),
        tr("Save profiles, enhancement scripts, runtime overrides and app preferences. "
           "Archives contain subscription credentials and are not encrypted. Only restore backups you trust."), this);
    auto *create = new QPushButton(tr("Create backup"), local);
    auto *exportButton = new QPushButton(tr("Export current…"), local);
    auto *importButton = new QPushButton(tr("Import backup…"), local);
    auto *row = new QHBoxLayout;
    row->addWidget(create);
    row->addWidget(exportButton);
    row->addWidget(importButton);
    row->addStretch();
    list_ = new QListWidget(local);
    list_->setMinimumHeight(150);
    list_->setMaximumHeight(250);
    restore_ = new QPushButton(tr("Restore selected backup…"), local);
    local->addLayout(row);
    local->addWidget(list_);
    local->addWidget(restore_);
    column->addWidget(local);

    auto *remote = new SettingsSection(tr("WebDAV"),
        tr("Use the full URL of a backup file in an existing WebDAV folder. Upload replaces that file; "
           "download saves a local backup for review before restoring. Use HTTPS to protect credentials."), this);
    url_ = new QLineEdit(remote);
    url_->setPlaceholderText("https://example.com/dav/clash-qt.cqtbackup");
    username_ = new QLineEdit(remote);
    password_ = new QLineEdit(remote);
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("Used for this session only"));
    QSettings preferences = core::preferences::open();
    url_->setText(preferences.value("backup/webdavUrl").toString());
    username_->setText(preferences.value("backup/webdavUser").toString());
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(tr("Backup URL"), url_);
    form->addRow(tr("Username"), username_);
    form->addRow(tr("Password"), password_);
    remote->addLayout(form);
    auto *upload = new QPushButton(tr("Upload current backup"), remote);
    auto *download = new QPushButton(tr("Download backup"), remote);
    auto *remoteRow = new QHBoxLayout;
    remoteRow->addWidget(upload);
    remoteRow->addWidget(download);
    remoteRow->addStretch();
    remote->addLayout(remoteRow);
    column->addWidget(remote);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    column->addWidget(status_);
    column->addStretch();
    auto *body = new QWidget(this);
    body->setLayout(column);
    auto *scroll = new QScrollArea(this);
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    theme::pageLayout(this)->addWidget(scroll);

    connect(create, &QPushButton::clicked, store_, &core::BackupStore::createLocalAsync);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getSaveFileName(this, tr("Export backup"), "clash-qt.cqtbackup",
                                                      tr("clash-qt backup (*.cqtbackup)"));
        if (!file.isEmpty()) store_->exportArchiveAsync(file);
    });
    connect(importButton, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, tr("Import backup"), {},
                                                      tr("clash-qt backup (*.cqtbackup);;All files (*)"));
        if (!file.isEmpty()) store_->importArchiveAsync(file);
    });
    connect(restore_, &QPushButton::clicked, this, &BackupPage::restore);
    connect(list_, &QListWidget::itemSelectionChanged, this, [this] {
        restore_->setEnabled(!store_->isBusy() && list_->currentItem() != nullptr);
    });
    const auto saveAddress = [this] {
        QSettings preferences = core::preferences::open();
        preferences.setValue("backup/webdavUrl", url_->text().trimmed());
        preferences.setValue("backup/webdavUser", username_->text());
    };
    connect(upload, &QPushButton::clicked, this, [this, saveAddress] {
        if (QMessageBox::question(this, tr("Upload backup"),
            tr("Upload profiles, credentials and scripts to the specified WebDAV file? "
               "An existing file at that URL will be replaced.")) != QMessageBox::Yes) return;
        saveAddress();
        store_->uploadWebDav(url_->text().trimmed(), username_->text(), password_->text());
    });
    connect(download, &QPushButton::clicked, this, [this, saveAddress] {
        saveAddress();
        store_->downloadWebDav(url_->text().trimmed(), username_->text(), password_->text());
    });
    connect(store_, &core::BackupStore::busyChanged, this,
            [this, upload, download, create, exportButton, importButton](bool busy) {
        upload->setEnabled(!busy);
        download->setEnabled(!busy);
        url_->setEnabled(!busy);
        username_->setEnabled(!busy);
        password_->setEnabled(!busy);
        create->setEnabled(!busy);
        exportButton->setEnabled(!busy);
        importButton->setEnabled(!busy);
        restore_->setEnabled(!busy && list_->currentItem() != nullptr);
    });
    connect(store_, &core::BackupStore::localBusyChanged, this, [this](bool busy, bool restoring) {
        const bool maintenance = busy || QCoreApplication::instance()->property("shuttingDown").toBool();
        context_.profiles->setMaintenanceMode(maintenance);
        context_.enhancer->setMaintenanceMode(maintenance);
        if (busy && restoring) {
            progress_ = new QProgressDialog(tr("Preparing and validating backup…"), {}, 0, 0, this);
            progress_->setWindowTitle(tr("Restore Backup"));
            progress_->setWindowModality(Qt::ApplicationModal);
            progress_->setMinimumDuration(0);
            progress_->setAutoClose(false);
            progress_->setCancelButton(nullptr);
            progress_->show();
        } else if (!busy) {
            waitingForCore_ = false;
            waitingForFiles_ = false;
            if (progress_) { progress_->close(); progress_->deleteLater(); progress_ = nullptr; }
        }
    });
    const auto continueWhenIdle = [this] {
        if (!waitingForFiles_ || context_.profiles->isFileBusy() || context_.enhancer->isFileBusy()) return;
        waitingForFiles_ = false;
        store_->continuePreparation();
    };
    connect(store_, &core::BackupStore::operationPreparing, this, [this, continueWhenIdle] {
        waitingForFiles_ = true;
        status_->setText(tr("Waiting for pending profile changes to finish…"));
        continueWhenIdle();
    });
    connect(context_.profiles, &core::ProfileStore::fileBusyChanged, this, continueWhenIdle);
    connect(context_.enhancer, &core::ConfigEnhancer::fileBusyChanged, this, continueWhenIdle);
    connect(store_, &core::BackupStore::backupsChanged, this, &BackupPage::refresh);
    connect(store_, &core::BackupStore::errorOccurred, status_, &QLabel::setText);
    connect(store_, &core::BackupStore::statusChanged, status_, &QLabel::setText);
    connect(store_, &core::BackupStore::restorePrepared, this, [this] {
        waitingForCore_ = true;
        status_->setText(tr("Stopping the core before restoring…"));
        if (progress_) progress_->setLabelText(status_->text());
        context_.coreProcess->stop();
        if (waitingForCore_ && context_.coreProcess->state() == core::CoreState::Stopped) {
            waitingForCore_ = false;
            store_->continueRestore(true);
        }
    });
    connect(context_.coreProcess, &core::CoreProcess::stopped, this, [this] {
        if (!waitingForCore_) return;
        waitingForCore_ = false;
        if (progress_) progress_->setLabelText(tr("Restoring files and preferences…"));
        store_->continueRestore(true);
    });
    connect(store_, &core::BackupStore::restored, this, [this] {
        context_.enhancer->load();
        context_.profiles->load();
    });
    refresh();
}

void BackupPage::refresh() {
    const QString selected = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString();
    list_->clear();
    for (const auto &path : store_->localBackups()) {
        auto *item = new QListWidgetItem(QFileInfo(path).fileName(), list_);
        item->setData(Qt::UserRole, path);
        if (path == selected) list_->setCurrentItem(item);
    }
    restore_->setEnabled(!store_->isBusy() && list_->currentItem() != nullptr);
    if (list_->count() == 0) status_->setText(tr("No local backups yet."));
}

void BackupPage::restore() {
    if (!list_->currentItem()) return;
    const QString path = list_->currentItem()->data(Qt::UserRole).toString();
    if (QMessageBox::warning(this, tr("Restore backup"),
        tr("Replace profiles, scripts, overrides and app preferences with this backup? "
           "The managed core will stop. A backup of the current state will be saved first. "
           "Restart the app afterward to apply all preferences."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    store_->restoreLocalAsync(path);
}

}  // namespace ui
