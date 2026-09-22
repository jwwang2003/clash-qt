#pragma once

#include <QDateTime>
#include <QString>
#include <QWidget>

#include "core/config/config_composer.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace core {
class ProfileStore;
}

namespace ui {

/// The effective configuration of one chain, as the store composes it: the YAML
/// itself, where each field came from, what the composer objected to, and the
/// composer's log.
///
/// A preview is a pure read. It asks
/// ProfileStore::requestEffectiveConfigPreview(), which composes asynchronously
/// from an immutable snapshot and -- by contract revision config-r1 -- writes
/// no runtime or preview file, seeds nothing and launches nothing. This widget
/// accordingly never touches the runtime-generation API, and its suite asserts
/// that previewing leaves no runtime file behind.
///
/// A failed preview never replaces the last successful one. The pane goes on
/// showing the last YAML that composed, labelled as the older one, because an
/// emptied pane after a typo reads as "your configuration is now empty" -- and
/// the contract makes the same distinction on the store side: a composition
/// failure must not overwrite the last usable result.
class EffectiveConfigView : public QWidget {
    Q_OBJECT

public:
    explicit EffectiveConfigView(core::ProfileStore *store, QWidget *parent = nullptr);

    /// A preview has been asked for and not yet answered.
    bool isPending() const { return pending_; }
    /// The last composition that succeeded. Empty until one does.
    QString lastSuccessfulYaml() const { return lastSuccessfulYaml_; }
    /// The YAML on screen is older than the last answer, because that answer
    /// failed.
    bool isStale() const { return stale_; }
    /// The chain the next answer is expected for: empty is the current profile.
    QString requestedUid() const { return requestedUid_; }

public slots:
    /// Preview `profileUid`; empty means whichever profile the store has
    /// selected.
    void requestPreview(const QString &profileUid = QString());

private:
    void render(const core::config::ComposeResult &result);
    void updateTabLabels();

    core::ProfileStore *store_;
    QString requestedUid_;
    QString lastSuccessfulYaml_;
    QDateTime lastSuccessAt_;
    bool pending_ = false;
    bool stale_ = false;

    QLabel *summaryLabel_;
    QLabel *errorLabel_;
    QPushButton *refreshButton_;
    QTabWidget *tabs_;
    QPlainTextEdit *yamlView_;
    QTreeWidget *provenanceList_;
    QTreeWidget *diagnosticList_;
    QPlainTextEdit *logView_;
};

}  // namespace ui
