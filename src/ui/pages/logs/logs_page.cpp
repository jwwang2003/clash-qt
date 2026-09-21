#include "ui/pages/logs/logs_page.h"

#include <QComboBox>
#include "ui/widgets/combo_box.h"
#include <QDateTime>
#include <QFontDatabase>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QTimer>
#include <QLineEdit>
#include <QSaveFile>
#include <QMessageBox>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/mihomo/mihomo_client.h"
#include "ui/theme/theme.h"

namespace ui {
namespace {

QString levelColor(const QString &level) {
    const theme::Tokens &t = theme::tokens();
    if (level == "error") return t.danger.name();
    if (level == "warning") return t.warning.name();
    if (level == "debug") return t.textFaint.name();
    return t.accent.name();
}

}  // namespace

LogsPage::LogsPage(core::MihomoClient *client, QWidget *parent)
    : QWidget(parent), client_(client) {
    levelBox_ = new ComboBox(this);
    levelBox_->addItems({"debug", "info", "warning", "error"});
    levelBox_->setCurrentText("info");
    connect(levelBox_, &QComboBox::currentTextChanged, this,
            [this](const QString &level) {
                client_->openLogStream(level);
                renderEntries();
            });

    pauseButton_ = new QPushButton(tr("Pause"), this);
    pauseButton_->setCheckable(true);
    connect(pauseButton_, &QPushButton::toggled, this, [this](bool paused) {
        pauseButton_->setText(paused ? tr("Resume") : tr("Pause"));
    });

    auto *clearButton = new QPushButton(tr("Clear"), this);
    auto *saveButton = new QPushButton(tr("Save…"), this);
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(tr("Filter logs…"));
    filterEdit_->setClearButtonEnabled(true);
    auto *filterTimer = new QTimer(this);
    filterTimer->setSingleShot(true);
    filterTimer->setInterval(120);
    connect(filterEdit_, &QLineEdit::textChanged, filterTimer, qOverload<>(&QTimer::start));
    connect(filterTimer, &QTimer::timeout, this, &LogsPage::renderEntries);

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setMaximumBlockCount(5000);
    QFont logFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    logFont.setPointSizeF(qMax(logFont.pointSizeF(), font().pointSizeF()));
    view_->setFont(logFont);
    connect(clearButton, &QPushButton::clicked, this, [this] { entries_.clear(); view_->clear(); });
    connect(saveButton, &QPushButton::clicked, this, [this, saveButton] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Visible Logs"), "clash-qt.log",
                                                        tr("Log files (*.log);;Text files (*.txt)"));
        if (path.isEmpty()) return;
        const QByteArray contents = view_->toPlainText().toUtf8();
        saveButton->setEnabled(false);
        saveButton->setText(tr("Saving…"));
        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, saveButton] {
            const QString error = watcher->result();
            watcher->deleteLater();
            saveButton->setEnabled(true);
            saveButton->setText(tr("Save…"));
            if (!error.isEmpty()) {
                auto *message = new QMessageBox(QMessageBox::Warning, tr("Save Logs"), error,
                                                QMessageBox::Ok, this);
                message->setAttribute(Qt::WA_DeleteOnClose);
                message->open();
            }
        });
        watcher->setFuture(QtConcurrent::run([path, contents] {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
                return file.errorString();
            return QString();
        }));
    });

    auto *controls = new QHBoxLayout;
    controls->setSpacing(theme::kPageSpacing);
    controls->addWidget(levelBox_);
    controls->addWidget(filterEdit_, 1);
    controls->addWidget(pauseButton_);
    controls->addWidget(clearButton);
    controls->addWidget(saveButton);

    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addWidget(view_, 1);

    connect(client_, &core::MihomoClient::logReceived, this, &LogsPage::onLogReceived);
    connect(theme::notifier(), &theme::Notifier::changed, this, &LogsPage::renderEntries);
    connect(client_, &core::MihomoClient::endpointChanged, this, [this] {
        entries_.clear();
        view_->clear();
    });
}

void LogsPage::appendCoreLine(const QString &line) {
    appendEntry(core::LogEntry{"core", line, QDateTime::currentDateTime()});
}

void LogsPage::onLogReceived(const core::LogEntry &entry) { appendEntry(entry); }

QString LogsPage::entryHtml(const core::LogEntry &entry) const {
    return QString("<span style=\"color:%1\">[%2]</span> %3 %4")
        .arg(entry.level == "core" ? theme::tokens().textDim.name() : levelColor(entry.level),
             entry.level.toUpper().toHtmlEscaped(), entry.time.toString("HH:mm:ss"),
             entry.payload.toHtmlEscaped());
}

void LogsPage::appendEntry(const core::LogEntry &entry) {
    if (pauseButton_->isChecked()) return;
    core::LogEntry bounded = entry;
    constexpr int maxPayloadCharacters = 8192;
    if (bounded.payload.size() > maxPayloadCharacters)
        bounded.payload = bounded.payload.left(maxPayloadCharacters) + tr("… [truncated]");
    entries_.append(bounded);
    // Trim in batches to avoid rebuilding a full filtered document on every sample.
    if (entries_.size() > 5500) {
        entries_.erase(entries_.begin(), entries_.end() - 5000);
        renderEntries();
        return;
    }
    if (matchesFilter(bounded)) view_->appendHtml(entryHtml(bounded));
}

bool LogsPage::matchesFilter(const core::LogEntry &entry) const {
    const QStringList levels{"debug", "info", "warning", "error"};
    if (entry.level != "core" && levels.indexOf(entry.level) < levels.indexOf(levelBox_->currentText()))
        return false;
    const QString filter = filterEdit_->text();
    return entry.payload.contains(filter, Qt::CaseInsensitive) ||
           entry.level.contains(filter, Qt::CaseInsensitive);
}

void LogsPage::renderEntries() {
    const int scroll = view_->verticalScrollBar()->value();
    const bool atEnd = scroll == view_->verticalScrollBar()->maximum();
    view_->clear();
    QStringList lines;
    for (const auto &entry : entries_) {
        if (matchesFilter(entry)) lines << entryHtml(entry);
    }
    if (!lines.isEmpty()) view_->appendHtml("<p>" + lines.join("</p><p>") + "</p>");
    view_->verticalScrollBar()->setValue(atEnd ? view_->verticalScrollBar()->maximum() : scroll);
}

}  // namespace ui
