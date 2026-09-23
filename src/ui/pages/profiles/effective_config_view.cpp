#include "ui/pages/profiles/effective_config_view.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "core/profiles/profile_store.h"
#include "ui/theme/theme.h"

namespace ui {
namespace {

enum Tab { YamlTab, ProvenanceTab, DiagnosticsTab, LogTab };

enum ProvenanceColumn { ProvenancePath, ProvenanceSource, ProvenanceColumnCount };
enum DiagnosticColumn {
    DiagnosticSeverity,
    DiagnosticSource,
    DiagnosticPath,
    DiagnosticMessage,
    DiagnosticColumnCount
};

QFont monospace(const QFont &fallback) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(qMax(font.pointSizeF(), fallback.pointSizeF()));
    return font;
}

}  // namespace

EffectiveConfigView::EffectiveConfigView(core::ProfileStore *store, QWidget *parent)
    : QWidget(parent), store_(store) {
    summaryLabel_ = new QLabel(tr("No preview yet."), this);
    summaryLabel_->setObjectName("sectionHint");
    summaryLabel_->setWordWrap(true);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName("errorBanner");
    errorLabel_->setTextFormat(Qt::PlainText);
    errorLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();

    refreshButton_ = new QPushButton(tr("Refresh Preview"), this);
    refreshButton_->setObjectName("previewRefresh");

    yamlView_ = new QPlainTextEdit(this);
    yamlView_->setObjectName("previewYaml");
    yamlView_->setReadOnly(true);
    yamlView_->setFont(monospace(font()));
    yamlView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    yamlView_->setPlaceholderText(tr("The composed configuration appears here."));

    provenanceList_ = new QTreeWidget(this);
    provenanceList_->setObjectName("previewProvenance");
    provenanceList_->setColumnCount(ProvenanceColumnCount);
    provenanceList_->setHeaderLabels({tr("Path"), tr("Source")});
    provenanceList_->setRootIsDecorated(false);
    provenanceList_->setUniformRowHeights(true);
    provenanceList_->header()->setSectionResizeMode(ProvenancePath, QHeaderView::Stretch);

    diagnosticList_ = new QTreeWidget(this);
    diagnosticList_->setObjectName("previewDiagnostics");
    diagnosticList_->setColumnCount(DiagnosticColumnCount);
    diagnosticList_->setHeaderLabels({tr("Severity"), tr("Source"), tr("Path"), tr("Message")});
    diagnosticList_->setRootIsDecorated(false);
    diagnosticList_->setUniformRowHeights(true);
    diagnosticList_->header()->setSectionResizeMode(DiagnosticMessage, QHeaderView::Stretch);
    diagnosticList_->setColumnWidth(DiagnosticSeverity, 80);

    logView_ = new QPlainTextEdit(this);
    logView_->setObjectName("previewLog");
    logView_->setReadOnly(true);
    logView_->setFont(monospace(font()));
    logView_->setPlaceholderText(tr("The composer reported nothing."));

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("previewTabs");
    tabs_->addTab(yamlView_, tr("Effective YAML"));
    tabs_->addTab(provenanceList_, tr("Provenance"));
    tabs_->addTab(diagnosticList_, tr("Diagnostics"));
    tabs_->addTab(logView_, tr("Composer Log"));

    auto *header = new QHBoxLayout;
    header->setSpacing(theme::kPageSpacing);
    header->addWidget(summaryLabel_, 1);
    header->addWidget(refreshButton_);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kPageSpacing);
    layout->addLayout(header);
    layout->addWidget(errorLabel_);
    layout->addWidget(tabs_, 1);

    connect(refreshButton_, &QPushButton::clicked, this,
            [this] { requestPreview(requestedUid_); });
    connect(store_, &core::ProfileStore::effectiveConfigPreviewReady, this,
            &EffectiveConfigView::render);

    updateTabLabels();
}

void EffectiveConfigView::requestPreview(const QString &profileUid) {
    requestedUid_ = profileUid;
    pending_ = true;
    summaryLabel_->setText(tr("Composing…"));
    store_->requestEffectiveConfigPreview(profileUid);
}

void EffectiveConfigView::render(const core::config::ComposeResult &result) {
    pending_ = false;

    // Diagnostics, provenance and the log always describe the attempt that just
    // answered, even when it failed -- they are the explanation of the failure.
    provenanceList_->clear();
    for (const core::config::Provenance &entry : result.provenance) {
        auto *item = new QTreeWidgetItem(provenanceList_);
        item->setText(ProvenancePath, entry.path);
        item->setText(ProvenanceSource, entry.source);
    }

    diagnosticList_->clear();
    for (const core::config::Diagnostic &entry : result.diagnostics) {
        auto *item = new QTreeWidgetItem(diagnosticList_);
        item->setText(DiagnosticSeverity, entry.severity);
        item->setText(DiagnosticSource, entry.source);
        item->setText(DiagnosticPath, entry.path);
        item->setText(DiagnosticMessage, entry.message);
        item->setToolTip(DiagnosticMessage, entry.message);
    }

    logView_->setPlainText(result.logs.join(QLatin1Char('\n')));

    if (result.ok) {
        lastSuccessfulYaml_ = result.yaml;
        lastSuccessAt_ = QDateTime::currentDateTime();
        stale_ = false;
        yamlView_->setPlainText(result.yaml);
        errorLabel_->hide();
        summaryLabel_->setText(
            result.diagnostics.isEmpty()
                ? tr("Composed at %1.").arg(lastSuccessAt_.toString("HH:mm:ss"))
                : tr("Composed at %1 with %2 diagnostic(s).")
                      .arg(lastSuccessAt_.toString("HH:mm:ss"))
                      .arg(result.diagnostics.size()));
        updateTabLabels();
        return;
    }

    // Failure: keep the last good YAML on screen and say plainly that it is the
    // older one, so a failed preview is never read as a successful empty result.
    stale_ = !lastSuccessfulYaml_.isEmpty();
    yamlView_->setPlainText(lastSuccessfulYaml_);
    errorLabel_->setText(
        stale_ ? tr("This preview did not compose. Nothing was written; the YAML below is the "
                    "last composition that succeeded, at %1.")
                     .arg(lastSuccessAt_.toString("HH:mm:ss"))
               : tr("This preview did not compose. Nothing was written, and no composition has "
                    "succeeded yet."));
    errorLabel_->show();
    summaryLabel_->setText(
        tr("Preview failed with %1 diagnostic(s).").arg(result.diagnostics.size()));
    updateTabLabels();
}

void EffectiveConfigView::updateTabLabels() {
    tabs_->setTabText(YamlTab, stale_ ? tr("Effective YAML (older)") : tr("Effective YAML"));
    tabs_->setTabText(ProvenanceTab,
                      tr("Provenance (%1)").arg(provenanceList_->topLevelItemCount()));
    tabs_->setTabText(DiagnosticsTab,
                      tr("Diagnostics (%1)").arg(diagnosticList_->topLevelItemCount()));
    tabs_->setTabText(LogTab, tr("Composer Log (%1)")
                                  .arg(logView_->toPlainText().isEmpty()
                                           ? 0
                                           : logView_->toPlainText().count(QLatin1Char('\n')) + 1));
}

}  // namespace ui
