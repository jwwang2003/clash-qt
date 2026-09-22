// The preset editor and the effective-config preview, driven through the
// controls a user actually operates and asserted against what the store
// persists -- never against the widget's own copy of the document.
//
// Two properties the suite exists for, both from P4_CONFIG_CONTRACT config-r1:
//
//  * A preview is a pure read. Composing must not write a runtime file, seed
//    anything or launch the engine, so every preview case ends by asserting
//    that no runtime artefact appeared and that the store never emitted a
//    generated configuration.
//  * A failed composition must not be mistaken for a successful empty one. The
//    pane keeps the last YAML that composed and says it is the older one; the
//    diagnostics belong to the attempt that failed.
//
// The editor writes through ProfileStore::setPresetDocument() and re-reads
// ProfileStore::presetDocument(), so the assertions here are on the store and,
// for the persistence case, on a second store loaded from the same directory.
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

#include <memory>

#include "core/profiles/profile_store.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"
#include "ui/pages/profiles/effective_config_view.h"
#include "ui/pages/profiles/preset_editor.h"
#include "ui/pages/profiles/profiles_page.h"
#include "ui/theme/theme.h"

namespace {

constexpr auto kSampleYaml = "mixed-port: 7890\nproxies: []\nrules:\n  - MATCH,DIRECT\n";

QJsonArray globalChain(const core::ProfileStore &store) {
    return store.presetDocument().value("global").toArray();
}

QJsonArray profileChain(const core::ProfileStore &store, const QString &uid) {
    return store.presetDocument().value("profiles").toObject().value(uid).toArray();
}

QJsonObject presetAt(const QJsonArray &chain, int index) {
    return chain.at(index).toObject();
}

QJsonObject operationAt(const QJsonObject &preset, int index) {
    return preset.value("operations").toArray().at(index).toObject();
}

// Answers the next modal question with Yes. Polls rather than firing once,
// because the dialog is only the active modal widget after exec() has entered
// its own event loop.
void acceptNextQuestion(QObject *parent) {
    auto *timer = new QTimer(parent);
    auto attempts = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, parent, [timer, attempts] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            box->button(QMessageBox::Yes)->click();
            timer->stop();
            return;
        }
        if (++*attempts > 300) timer->stop();
    });
    timer->start(10);
}

// Fills the operation form the way the user does, then adds the operation.
void addOperation(ui::PresetEditor *editor, const QString &kind, const QString &path,
                  const QString &json) {
    auto *kindBox = editor->findChild<QComboBox *>("operationKind");
    kindBox->setCurrentIndex(kindBox->findData(kind));
    editor->findChild<QLineEdit *>("operationPath")->setText(path);
    editor->findChild<QPlainTextEdit *>("operationValue")->setPlainText(json);
    editor->findChild<QPushButton *>("addOperation")->click();
}

void addPreset(ui::PresetEditor *editor, const QString &name) {
    editor->findChild<QLineEdit *>("presetName")->setText(name);
    editor->findChild<QPushButton *>("addPreset")->click();
}

}  // namespace

class PresetEditorTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { ui::theme::install(); }

    // One scoped environment per case, as the profile-store suite has: the
    // store resolves CLASH_QT_DATA_DIR on every call, so the directory is the
    // isolation boundary for the persisted preset document as well.
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("preset"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() {
        QVERIFY2(environment_->realPreferencesUnchanged(),
                 "the real user preference store changed during this case");
        environment_.reset();
    }

    void shortWindowsKeepProfilesVisibleAndPresetActionsReachable_data() {
        QTest::addColumn<int>("height");
        QTest::newRow("short") << 360;
        QTest::newRow("medium") << 520;
    }

    void shortWindowsKeepProfilesVisibleAndPresetActionsReachable() {
        QFETCH(int, height);
        core::ProfileStore store;
        importSample(store);
        ui::ProfilesPage page(&store);
        page.resize(1400, height);
        page.show();
        QTRY_VERIFY(page.isVisible());
        QCoreApplication::processEvents();
        QCOMPARE(page.height(), height);

        auto *profiles = page.findChild<QListView *>();
        QVERIFY(profiles);
        const QRect card = profiles->visualRect(profiles->model()->index(0, 0));
        QVERIFY2(profiles->viewport()->rect().contains(card),
                 "the preset form squeezed the profile card out of view");

        auto *editor = page.findChild<ui::PresetEditor *>();
        auto *scroll = page.findChild<QScrollArea *>("presetScrollArea");
        QVERIFY(scroll);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        addPreset(editor, "Compact window");
        auto *action = editor->findChild<QPushButton *>("addOperation");
        scroll->ensureWidgetVisible(action);
        QCoreApplication::processEvents();
        const QRect actionRect(action->mapTo(scroll->viewport(), QPoint()), action->size());
        QVERIFY2(scroll->viewport()->rect().contains(actionRect),
                 "the bottom preset actions must be reachable by scrolling");
        addOperation(editor, "merge", "/dns/enable", "true");
        QCOMPARE(presetAt(globalChain(store), 0).value("operations").toArray().size(), 1);

        page.findChild<QTabWidget *>("profileDetails")->setCurrentIndex(1);
        QCoreApplication::processEvents();
        QVERIFY(profiles->viewport()->rect().contains(profiles->visualRect(
            profiles->model()->index(0, 0))));
        QVERIFY(page.findChild<QPushButton *>("previewRefresh")->isVisible());
    }

    void addingAPresetAndAnOperationReachesThePersistedDocument() {
        core::ProfileStore store;
        importSample(store);
        ui::ProfilesPage page(&store);
        page.show();
        auto *editor = page.findChild<ui::PresetEditor *>();
        QVERIFY(editor);

        addPreset(editor, "DNS off");
        QCOMPARE(int(globalChain(store).size()), 1);
        QCOMPARE(presetAt(globalChain(store), 0).value("name").toString(), QString("DNS off"));
        QVERIFY(presetAt(globalChain(store), 0).value("enabled").toBool());
        QVERIFY(!presetAt(globalChain(store), 0).value("id").toString().isEmpty());

        addOperation(editor, "replace", "/dns/enable", "false");
        const QJsonObject operation = operationAt(presetAt(globalChain(store), 0), 0);
        QCOMPARE(operation.value("op").toString(), QString("replace"));
        QCOMPARE(operation.value("path").toString(), QString("/dns/enable"));
        // Explicit false is meaningful in the contract, so it must survive the
        // round trip as a boolean and not as "absent".
        QVERIFY(operation.value("value").isBool());
        QCOMPARE(operation.value("value").toBool(true), false);

        // A second store reading the same directory sees it: the document is
        // persisted, not held in the widget.
        core::ProfileStore reopened;
        reopened.load();
        QCOMPARE(int(globalChain(reopened).size()), 1);
        QCOMPARE(operationAt(presetAt(globalChain(reopened), 0), 0).value("path").toString(),
                 QString("/dns/enable"));
    }

    void orderAndEnablementAreEditedInPlaceAndPersisted() {
        core::ProfileStore store;
        importSample(store);
        ui::ProfilesPage page(&store);
        page.show();
        auto *editor = page.findChild<ui::PresetEditor *>();
        addPreset(editor, "First");
        addPreset(editor, "Second");
        QCOMPARE(int(globalChain(store).size()), 2);
        QCOMPARE(presetAt(globalChain(store), 0).value("name").toString(), QString("First"));

        // "Second" is selected after being added, so Move Up swaps the pair.
        editor->findChild<QPushButton *>("presetUp")->click();
        QCOMPARE(presetAt(globalChain(store), 0).value("name").toString(), QString("Second"));
        QCOMPARE(presetAt(globalChain(store), 1).value("name").toString(), QString("First"));

        auto *list = editor->findChild<QTreeWidget *>("presetList");
        QCOMPARE(list->topLevelItemCount(), 2);
        QCOMPARE(list->topLevelItem(0)->text(0), QString("Second"));
        // Unchecking commits on the next turn of the loop: the commit rebuilds
        // the tree, and the item whose signal is running would be deleted.
        list->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
        QTRY_VERIFY(!presetAt(globalChain(store), 0).value("enabled").toBool(true));
        QVERIFY(presetAt(globalChain(store), 1).value("enabled").toBool(true));
    }

    void removingAPresetTakesItsOperationsWithIt() {
        core::ProfileStore store;
        importSample(store);
        ui::ProfilesPage page(&store);
        page.show();
        auto *editor = page.findChild<ui::PresetEditor *>();
        addPreset(editor, "Doomed");
        addOperation(editor, "merge", "/dns/enable", "true");
        QCOMPARE(int(presetAt(globalChain(store), 0).value("operations").toArray().size()), 1);

        acceptNextQuestion(&page);
        editor->findChild<QPushButton *>("removePreset")->click();
        QTRY_COMPARE(int(globalChain(store).size()), 0);
    }

    void theScopeSelectorWritesUnderTheSelectedProfileUid() {
        core::ProfileStore store;
        importSample(store);
        const QString uid = store.profiles().first().uid;
        ui::ProfilesPage page(&store);
        page.show();
        auto *editor = page.findChild<ui::PresetEditor *>();
        QCOMPARE(editor->scopeUid(), QString());

        auto *scope = editor->findChild<QComboBox *>("presetScope");
        QVERIFY(scope->itemText(1).contains(store.profiles().first().name));
        scope->setCurrentIndex(1);
        QCOMPARE(editor->scopeUid(), uid);

        addPreset(editor, "Only here");
        QCOMPARE(int(profileChain(store, uid).size()), 1);
        QVERIFY(globalChain(store).isEmpty());

        // Switching back shows the other chain, and does not move the preset.
        scope->setCurrentIndex(0);
        QCOMPARE(editor->findChild<QTreeWidget *>("presetList")->topLevelItemCount(), 0);
        QCOMPARE(int(profileChain(store, uid).size()), 1);
    }

    void invalidOperationInputIsRefusedBeforeItReachesTheStore() {
        core::ProfileStore store;
        importSample(store);
        ui::ProfilesPage page(&store);
        page.show();
        auto *editor = page.findChild<ui::PresetEditor *>();
        addPreset(editor, "Draft");
        auto *error = editor->findChild<QLabel *>("fieldError");
        QVERIFY(error);

        addOperation(editor, "merge", "dns/enable", "true");
        QVERIFY(error->isVisible());
        QVERIFY(error->text().contains("/"));
        QVERIFY(presetAt(globalChain(store), 0).value("operations").toArray().isEmpty());

        addOperation(editor, "merge", "/dns/enable", "not json");
        QVERIFY(error->isVisible());
        QVERIFY(presetAt(globalChain(store), 0).value("operations").toArray().isEmpty());

        // append takes a rule array; a scalar is refused with its own message.
        addOperation(editor, "append", "/rules", "\"MATCH,DIRECT\"");
        QVERIFY(error->isVisible());
        QVERIFY(presetAt(globalChain(store), 0).value("operations").toArray().isEmpty());

        // Refused HERE, not merely refused eventually: the store validates the
        // same rules, so without this the case would pass on the store's
        // rejection alone and prove nothing about the editor. A store that was
        // never asked has nothing to say.
        QVERIFY2(store.lastPresetDiagnostics().isEmpty(),
                 "invalid operation input was sent to the store instead of being refused here");

        addOperation(editor, "append", "/rules", "[\"MATCH,DIRECT\"]");
        QVERIFY(!error->isVisible());
        QCOMPARE(int(presetAt(globalChain(store), 0).value("operations").toArray().size()), 1);
    }

    void aPreviewIsComposedAsynchronouslyAndWritesNothing() {
        core::ProfileStore store;
        importSample(store);
        QSignalSpy generated(&store, &core::ProfileStore::runtimeConfigReady);
        ui::ProfilesPage page(&store);
        auto *preview = page.findChild<ui::EffectiveConfigView *>();
        QVERIFY(preview);

        page.show();
        // Asynchronous by contract: nothing has answered on the same turn.
        QVERIFY(preview->isPending());
        auto *yaml = preview->findChild<QPlainTextEdit *>("previewYaml");
        QTRY_VERIFY(!preview->isPending());
        QVERIFY(!yaml->toPlainText().isEmpty());
        QVERIFY(!preview->isStale());
        QCOMPARE(preview->lastSuccessfulYaml(), yaml->toPlainText());
        // Controller-owned fields are reasserted with provenance, so a
        // successful composition always explains at least one field.
        QVERIFY(preview->findChild<QTreeWidget *>("previewProvenance")->topLevelItemCount() > 0);

        QVERIFY(!QFile::exists(store.dataDir() + "/runtime.yaml"));
        QVERIFY(!store.isRuntimeBusy());
        QCOMPARE(generated.size(), 0);
    }

    void aFailedPreviewKeepsTheLastGoodYamlAndSaysSo() {
        core::ProfileStore store;
        importSample(store);
        ui::ProfilesPage page(&store);
        page.show();
        // The preview is the second tab; a user reads it by opening that tab,
        // and a hidden widget reports nothing visible.
        page.findChild<QTabWidget *>("profileDetails")->setCurrentIndex(1);
        auto *preview = page.findChild<ui::EffectiveConfigView *>();
        auto *yaml = preview->findChild<QPlainTextEdit *>("previewYaml");
        QTRY_VERIFY(!preview->isPending());
        const QString composed = preview->lastSuccessfulYaml();
        QVERIFY(!composed.isEmpty());

        QSignalSpy generated(&store, &core::ProfileStore::runtimeConfigReady);
        QVERIFY(QFile::remove(store.profiles().first().filePath));
        preview->findChild<QPushButton *>("previewRefresh")->click();
        QTRY_VERIFY(!preview->isPending());

        auto *banner = preview->findChild<QLabel *>("errorBanner");
        QVERIFY(banner->isVisible());
        QVERIFY(preview->isStale());
        // The YAML pane still shows what composed, not an empty document, and
        // the tab says so.
        QCOMPARE(yaml->toPlainText(), composed);
        QCOMPARE(preview->lastSuccessfulYaml(), composed);
        QVERIFY(preview->findChild<QTabWidget *>("previewTabs")->tabText(0).contains("older"));
        // The diagnostics describe the attempt that failed.
        QVERIFY(preview->findChild<QTreeWidget *>("previewDiagnostics")->topLevelItemCount() > 0);

        QVERIFY(!QFile::exists(store.dataDir() + "/runtime.yaml"));
        QCOMPARE(generated.size(), 0);
    }

    void aPresetEditRequestsAFreshPreviewForTheScopeItWasMadeIn() {
        core::ProfileStore store;
        importSample(store);
        const QString uid = store.profiles().first().uid;
        ui::ProfilesPage page(&store);
        page.show();
        auto *editor = page.findChild<ui::PresetEditor *>();
        auto *preview = page.findChild<ui::EffectiveConfigView *>();
        QTRY_VERIFY(!preview->isPending());
        QCOMPARE(preview->requestedUid(), QString());

        editor->findChild<QComboBox *>("presetScope")->setCurrentIndex(1);
        QCOMPARE(preview->requestedUid(), uid);
        QTRY_VERIFY(!preview->isPending());

        addPreset(editor, "Watched");
        QCOMPARE(preview->requestedUid(), uid);
        QVERIFY(preview->isPending());
        QTRY_VERIFY(!preview->isPending());
        QVERIFY(!QFile::exists(store.dataDir() + "/runtime.yaml"));
    }

    void theEditorFollowsTheProfileSelectedInTheList() {
        core::ProfileStore store;
        importSample(store);
        const QString second = importFixture(store, "other.yaml");
        QVERIFY(!second.isEmpty());
        const QString first = store.profiles().first().uid;
        ui::ProfilesPage page(&store);
        page.show();
        auto *editor = page.findChild<ui::PresetEditor *>();
        auto *list = page.findChild<QListView *>();
        QVERIFY(list);

        editor->findChild<QComboBox *>("presetScope")->setCurrentIndex(1);
        list->setCurrentIndex(list->model()->index(0, 0));
        QCOMPARE(editor->scopeUid(), first);

        addPreset(editor, "For the first");
        QCOMPARE(int(profileChain(store, first).size()), 1);
        QVERIFY(profileChain(store, second).isEmpty());

        // The other profile's chain is a different list, not a filtered view of
        // the same one.
        list->setCurrentIndex(list->model()->index(1, 0));
        QCOMPARE(editor->scopeUid(), second);
        QCOMPARE(editor->findChild<QTreeWidget *>("presetList")->topLevelItemCount(), 0);
    }

    // The legacy controls this page shipped with are still wired to the store.
    void theLegacyProfileControlsSurviveTheNewSections() {
        core::ProfileStore store;
        importSample(store);
        ui::ProfilesPage page(&store);
        page.show();

        auto buttonNamed = [&page](const QString &text) -> QPushButton * {
            for (QPushButton *button : page.findChildren<QPushButton *>())
                if (button->text() == text) return button;
            return nullptr;
        };
        QVERIFY(buttonNamed("Import from URL"));
        QVERIFY(buttonNamed("Import from File"));
        QVERIFY(buttonNamed("New Local…"));
        QVERIFY(buttonNamed("Update All"));

        QSignalSpy errors(&store, &core::ProfileStore::errorOccurred);
        auto *url = page.findChild<QLineEdit *>("subscriptionUrl");
        QVERIFY(url);
        url->setText("file:///etc/passwd");
        buttonNamed("Import from URL")->click();
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(int(store.profiles().size()), 1);
    }

private:
    void importSample(core::ProfileStore &store) {
        QVERIFY(!importFixture(store, "sample.yaml").isEmpty());
        QCOMPARE(int(store.profiles().size()), 1);
    }

    // Import through the store's own synchronous path: no network, no engine.
    QString importFixture(core::ProfileStore &store, const QString &file) {
        const QString path = environment_->filePath(file);
        testsupport::writeFile(path, kSampleYaml);
        const qsizetype before = store.profiles().size();
        store.importFromFile(path);
        if (store.profiles().size() != before + 1) return {};
        return store.profiles().last().uid;
    }

    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(PresetEditorTest)
#include "preset_editor_test.moc"
