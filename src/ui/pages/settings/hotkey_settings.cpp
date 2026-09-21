#include "ui/pages/settings/hotkey_settings.h"

#include <QGridLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QSettings>

#include "platform/system/hotkeys.h"
#include "ui/theme/theme.h"

namespace ui {
namespace {

constexpr int kEditWidth = 220;

QSettings settings() { return QSettings("clash-qt", "clash-qt"); }

QString settingsKey(const QString &id) { return "hotkeys/" + id; }

}  // namespace

HotkeySettings::HotkeySettings(platform::Hotkeys *hotkeys, QWidget *parent)
    : QWidget(parent), hotkeys_(hotkeys) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(theme::kPageSpacing * 2);
    grid_->setVerticalSpacing(6);
    grid_->setColumnStretch(2, 1);

    addRow(hotkey::kToggleWindow, tr("Show / hide the window"));
    addRow(hotkey::kToggleProxy, tr("Toggle the system proxy"));
    addRow(hotkey::kCycleMode, tr("Switch routing mode"));
}

void HotkeySettings::addRow(const char *id, const QString &label) {
    const int index = rows_.size();
    const int row = index * 2;

    auto *name = new QLabel(label, this);
    name->setObjectName("fieldLabel");

    auto *edit = new QKeySequenceEdit(this);
    // A global hotkey is a single chord; the OS has nowhere to put the rest.
    edit->setMaximumSequenceLength(1);
    edit->setClearButtonEnabled(true);
    edit->setEnabled(platform::Hotkeys::isSupported());
    edit->setFixedWidth(kEditWidth);

    auto *error = new QLabel(this);
    error->setObjectName("fieldError");
    error->setWordWrap(true);
    error->hide();

    grid_->addWidget(name, row, 0);
    grid_->addWidget(edit, row, 1);
    grid_->addWidget(error, row + 1, 1, 1, 2);
    rows_.append({QString::fromLatin1(id), edit, error});

    edit->setKeySequence(
        QKeySequence(settings().value(settingsKey(rows_.at(index).id)).toString()));
    // keySequenceChanged rather than editingFinished: the clear button emits
    // only the former.
    connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this, index] { apply(index); });

    if (platform::Hotkeys::isSupported() && !edit->keySequence().isEmpty()) apply(index);
}

void HotkeySettings::apply(int index) {
    const Row &row = rows_.at(index);
    const QKeySequence sequence = row.edit->keySequence();
    settings().setValue(settingsKey(row.id), sequence.toString());

    if (sequence.isEmpty()) {
        hotkeys_->unbind(row.id);
        row.error->hide();
        return;
    }

    if (hotkeys_->bind(row.id, sequence)) {
        row.error->hide();
        return;
    }

    const QString reason = hotkeys_->lastError();
    row.error->setText(reason.isEmpty()
                           ? tr("The system refused %1 — another app is probably using it.")
                                 .arg(sequence.toString(QKeySequence::NativeText))
                           : reason);
    row.error->show();
}

}  // namespace ui
