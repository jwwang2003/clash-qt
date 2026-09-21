#ifndef CLASHQT_APP_LIFECYCLE_SHUTDOWN_PORTS_H
#define CLASHQT_APP_LIFECYCLE_SHUTDOWN_PORTS_H

// The seams ShutdownCoordinator is written against. Each is the narrowest
// surface the quit gate actually consumes, so a test can hold and release every
// condition explicitly instead of waiting for one.
//
// Nothing here knows about QApplication, widgets, ProfileStore, ConfigEnhancer
// or SystemProxyService: the composition root adapts those. That is what keeps
// shutdown correctness independent of which objects happen to exist, which is
// the defect PRE-ARCH section 4c records (main.cpp:182 discovering services by
// walking the widget tree).

#include <functional>

#include <QString>

namespace app::lifecycle {

// ---------------------------------------------------------------- busy gates

// One blocking condition of the quit gate.
//
// ORDER IS PART OF THE CONTRACT. Gates are evaluated in registration order and
// the first busy one names the block, which is how the ordering in PRE-ARCH
// section 4b group E item 8 - backup busy, profile runtime busy, profile file
// busy, enhancer file busy - stays observable rather than merely implied by a
// conjunction that short-circuits invisibly.
class BusyGate {
  public:
    virtual ~BusyGate() = default;

    // Stable identifier, reported by ShutdownCoordinator::blockingReason().
    virtual QString name() const = 0;
    virtual bool isBusy() const = 0;
};

// A gate built from a predicate, so the composition root needs no class per
// busy source. The predicate is called on the owning thread only.
class FunctionGate final : public BusyGate {
  public:
    FunctionGate(QString name, std::function<bool()> isBusy);

    QString name() const override { return name_; }
    bool isBusy() const override;

  private:
    QString name_;
    std::function<bool()> isBusy_;
};

// ------------------------------------------------------------- system proxy

// The OS proxy settings restore that must complete BEFORE the managed core is
// stopped (main.cpp:225: coreProcess->stop() is called from inside
// shutdownFinished, never in parallel). Ordering is load-bearing: stopping the
// core first can leave the machine pointed at a dead proxy.
class SystemProxyShutdown {
  public:
    virtual ~SystemProxyShutdown() = default;

    // Asynchronous. The answer arrives as
    // ShutdownCoordinator::onProxyShutdownFinished(), which the composition
    // root connects to SystemProxyService::shutdownFinished.
    virtual void requestShutdown() = 0;
};

// The proxy shutdown as a callable, so the composition root wires
// platform::SystemProxyService without this package linking clash_platform and
// without main() having to declare a class of its own.
class FunctionProxyShutdown final : public SystemProxyShutdown {
  public:
    explicit FunctionProxyShutdown(std::function<void()> request);
    void requestShutdown() override;

  private:
    std::function<void()> request_;
};

// ---------------------------------------------------------------- task drain

// The global task pool the snapshot deletions run on (main.cpp:130 starts them,
// main.cpp:191 refuses to quit while any is still running). Last gate before
// approval, and deliberately checked AFTER the final prune, which is what
// queues the last batch of work.
class TaskDrain {
  public:
    virtual ~TaskDrain() = default;
    virtual int activeTaskCount() const = 0;
};

// QThreadPool::globalInstance()->activeThreadCount(), which is what main.cpp
// checks today. Declared here rather than inlined into the coordinator so a
// test can substitute a counter it controls.
class GlobalThreadPoolDrain final : public TaskDrain {
  public:
    int activeTaskCount() const override;
};

// ------------------------------------------------------------ shutdown state

// The explicit query that replaces the QCoreApplication "shuttingDown" dynamic
// property (main.cpp:236) which ui/backup_page.cpp:137 reads today. PRE-ARCH
// section 4b group E item 5 requires that cross-layer contract to be replaced
// with a query, not silently dropped.
class ShutdownState {
  public:
    virtual ~ShutdownState() = default;
    virtual bool isShuttingDown() const noexcept = 0;
};

}  // namespace app::lifecycle

#endif  // CLASHQT_APP_LIFECYCLE_SHUTDOWN_PORTS_H
