#include "ui/logs_page.h"

#include <QComboBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/mihomo_client.h"
#include "ui/theme.h"

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
    levelBox_ = new QComboBox(this);
    levelBox_->addItems({"debug", "info", "warning", "error"});
    levelBox_->setCurrentText("info");
    connect(levelBox_, &QComboBox::currentTextChanged, this,
            [this](const QString &level) { client_->openLogStream(level); });

    pauseButton_ = new QPushButton(tr("Pause"), this);
    pauseButton_->setCheckable(true);

    auto *clearButton = new QPushButton(tr("Clear"), this);

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setMaximumBlockCount(5000);
    view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(clearButton, &QPushButton::clicked, view_, &QPlainTextEdit::clear);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(theme::kPageSpacing);
    controls->addWidget(levelBox_);
    controls->addStretch(1);
    controls->addWidget(pauseButton_);
    controls->addWidget(clearButton);

    auto *layout = theme::pageLayout(this);
    layout->addLayout(controls);
    layout->addWidget(view_, 1);

    connect(client_, &core::MihomoClient::logReceived, this, &LogsPage::onLogReceived);
}

void LogsPage::appendCoreLine(const QString &line) {
    if (pauseButton_->isChecked()) return;

    view_->appendHtml(QString("<span style=\"color:%1\">[CORE]</span> %2 %3")
                          .arg(theme::tokens().textDim.name(),
                               QDateTime::currentDateTime().toString("HH:mm:ss"),
                               line.toHtmlEscaped()));
}

void LogsPage::onLogReceived(const core::LogEntry &entry) {
    if (pauseButton_->isChecked()) return;

    view_->appendHtml(QString("<span style=\"color:%1\">[%2]</span> %3 %4")
                          .arg(levelColor(entry.level), entry.level.toUpper(),
                               entry.time.toString("HH:mm:ss"), entry.payload.toHtmlEscaped()));
}

}  // namespace ui
