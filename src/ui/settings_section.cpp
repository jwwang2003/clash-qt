#include "ui/settings_section.h"

#include <QLabel>
#include <QVBoxLayout>

#include "ui/theme.h"

namespace ui {
namespace {

constexpr int kCardPad = 14;

QLabel *banner(const char *objectName, QWidget *parent) {
    auto *label = new QLabel(parent);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->hide();
    return label;
}

}  // namespace

SettingsSection::SettingsSection(const QString &title, const QString &hint, QWidget *parent)
    : QFrame(parent) {
    setObjectName("settingsCard");

    auto *titleLabel = new QLabel(title, this);
    titleLabel->setObjectName("sectionTitle");

    auto *hintLabel = new QLabel(hint, this);
    hintLabel->setObjectName("sectionHint");
    hintLabel->setWordWrap(true);
    hintLabel->setVisible(!hint.isEmpty());

    notice_ = banner("noticeBanner", this);
    error_ = banner("errorBanner", this);

    body_ = new QVBoxLayout(this);
    body_->setContentsMargins(kCardPad, kCardPad, kCardPad, kCardPad);
    body_->setSpacing(theme::kPageSpacing);
    body_->addWidget(titleLabel);
    body_->addWidget(hintLabel);
    body_->addWidget(notice_);
    body_->addWidget(error_);
}

void SettingsSection::addWidget(QWidget *widget) { body_->addWidget(widget); }

void SettingsSection::addLayout(QLayout *layout) { body_->addLayout(layout); }

void SettingsSection::setNotice(const QString &text) {
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
}

void SettingsSection::setError(const QString &text) {
    error_->setText(text);
    error_->setVisible(!text.isEmpty());
}

}  // namespace ui
