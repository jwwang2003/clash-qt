#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QQueue>
#include <atomic>
#include <memory>

namespace core {

enum class ChainKind { Merge, Script };

/// One step of the enhancement chain: a YAML fragment merged into the config,
/// or a JS script that rewrites it.
struct ChainItem {
    QString uid;
    QString name;
    ChainKind kind = ChainKind::Merge;
    QString filePath;
    bool enabled = true;
};

struct EnhanceResult {
    QString yaml;        // the enhanced config; the input unchanged on failure
    QStringList logs;    // console output from scripts, "level: message"
    QString error;       // empty on success
};

/// One chain step with its source already read off disk.
///
/// `readError` is non-empty when the file could not be read; the step is still
/// present, because the failure has to be reported against the step it belongs
/// to and the rest of the chain has to keep running -- exactly what the
/// read-at-the-last-moment version did.
struct ChainStep {
    ChainItem item;
    QByteArray contents;
    QString readError;
};

/// An immutable chain: every enabled step's contents, taken at one instant.
using ChainSnapshot = QVector<ChainStep>;

/// Applies a chain of merges and scripts to a profile's YAML.
///
/// The chain is global rather than per-profile: it applies to whichever profile
/// is active. Contract with the ui module. Extend, do not reshape.
class ConfigEnhancer : public QObject {
    Q_OBJECT

public:
    explicit ConfigEnhancer(QObject *parent = nullptr);
    ~ConfigEnhancer() override;

    QString chainDir() const;

    void load();
    void setMaintenanceMode(bool enabled);
    void beginShutdown();
    QVector<ChainItem> chain() const;

    void addMerge(const QString &name);
    void addScript(const QString &name);
    void importItem(const QString &path, ChainKind kind);
    bool saveItemContent(const QString &uid, const QString &contents);
    void importItemAsync(const QString &path, ChainKind kind);
    void saveItemContentAsync(const QString &uid, const QString &contents);
    bool isFileBusy() const;
    void removeItem(const QString &uid);
    void setEnabled(const QString &uid, bool enabled);
    void moveItem(const QString &uid, int toIndex);
    void renameItem(const QString &uid, const QString &name);

    /// Runs the enabled chain over `baseYaml`. Never throws: a failing step is
    /// reported in the result and leaves the config as it was.
    EnhanceResult apply(const QString &baseYaml, const QString &profileName = {}) const;
    static EnhanceResult applyChain(const QString &baseYaml, const QString &profileName,
                                   const QVector<ChainItem> &chain,
                                   const std::shared_ptr<std::atomic_bool> &cancelled = {});

    /// Reads every enabled step's file, once, right now.
    ///
    /// Call this on the thread that owns the chain and hand the result to a
    /// worker: the enhancement then runs on values, not on paths. The version
    /// that takes ChainItems opens step N's file immediately before running step
    /// N, so a merge fragment saved while a slow script two steps earlier was
    /// still running was picked up mid-run and the generated config did not
    /// correspond to any single state of the chain. Disabled steps are carried
    /// through unread -- they are skipped either way, and reading them would
    /// make the snapshot cost depend on steps that do nothing.
    static ChainSnapshot snapshotChain(const QVector<ChainItem> &chain);
    ChainSnapshot snapshot() const;

    /// The same enhancement over an already-taken snapshot. Semantics are
    /// identical to the ChainItem overload, down to which step a failure is
    /// attributed to and the fact that a failing step leaves the chain running.
    static EnhanceResult applyChain(const QString &baseYaml, const QString &profileName,
                                    const ChainSnapshot &snapshot,
                                    const std::shared_ptr<std::atomic_bool> &cancelled = {});

signals:
    void reloaded();
    void chainChanged(const QVector<ChainItem> &chain);
    void errorOccurred(const QString &message);
    void fileBusyChanged(bool busy);
    void itemContentSaved(const QString &uid, bool success);

private:
    struct ItemWrite { ChainItem item; QByteArray contents; QString sourcePath; bool create = false; };
    void enqueueWrite(ItemWrite request);
    void startNextWrite();
    void cancelFileOperations();
    int indexOf(const QString &uid) const;
    bool acceptsChanges();
    bool save();
    void addItem(const QString &name, ChainKind kind, const QByteArray &contents);

    QVector<ChainItem> chain_;
    bool maintenance_ = false;
    bool shuttingDown_ = false;
    bool fileRunning_ = false;
    quint64 fileGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> fileCancellation_;
    QQueue<ItemWrite> fileQueue_;
};

}  // namespace core
