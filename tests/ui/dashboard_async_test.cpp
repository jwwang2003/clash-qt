#include <QtTest>
#include <QMenu>
#include <QThread>
#include <QTimer>
#include <atomic>
#include "platform/browser/browser_launcher.h"
#include "ui/shell/dashboard_button.h"

namespace {
std::atomic_int discoveries{0};
std::atomic_int completed{0};
std::atomic_bool ranOnGuiThread{false};

// Injected through the constructor instead of replacing the production symbols
// at link time, so this target links the real browser_launcher.cpp. The fixture
// is file-scope because discovery keeps running after a button is destroyed.
class FixtureBrowsers final : public platform::BrowserOperations {
public:
    QVector<platform::Browser> available() override {
        ++discoveries;
        ranOnGuiThread = QThread::currentThread() == qApp->thread();
        QThread::msleep(200); // A deliberately slow OS enumeration, with no OS access.
        ++completed;
        return {{"fixture.browser", "Fixture Browser", true}};
    }
    bool open(const QUrl &, const QString &) override { return true; }
};
FixtureBrowsers browsers;
}

class DashboardAsyncTest : public QObject {
    Q_OBJECT
private slots:
    void menuDiscoveryDoesNotBlockGui() {
        core::MihomoClient client;
        ui::DashboardButton button(&client, &browsers);
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, this, [&] { if (completed.load() == 0) ++ticks; });
        heartbeat.start();
        // Exercise the menu-opening callback without platform popup/font
        // initialization contaminating the worker responsiveness measurement.
        QVERIFY(QMetaObject::invokeMethod(button.menu(), "aboutToShow", Qt::DirectConnection));
        QTRY_VERIFY(discoveries.load() >= 1);
        QVERIFY(!button.menu()->actions().isEmpty());
        QCOMPARE(button.menu()->actions().first()->text(), QString("System Default Browser"));
        QTRY_VERIFY(completed.load() >= 1);
        QTRY_VERIFY(button.menu()->actions().last()->text().contains("Fixture Browser"));
        QVERIFY2(ticks > 0, "Browser discovery stalled the GUI event loop");
        QVERIFY(!ranOnGuiThread.load());
        QCOMPARE(discoveries.load(), 1); // Opening during prefetch must not duplicate work.
        button.menu()->hide();
    }

    void destructionDuringDiscoveryIsSafe() {
        core::MihomoClient client;
        const int before = completed;
        auto *button = new ui::DashboardButton(&client, &browsers);
        QTRY_VERIFY(discoveries.load() > before);
        QElapsedTimer elapsed;
        elapsed.start();
        delete button;
        QVERIFY2(elapsed.elapsed() < 100, "Destroying the button waited for discovery");
        QTRY_VERIFY(completed.load() > before);
    }
};
QTEST_MAIN(DashboardAsyncTest)
#include "dashboard_async_test.moc"
