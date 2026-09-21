#pragma once

#include <QElapsedTimer>
#include <QWidget>

#include "core/telemetry/traffic_history.h"

class QAreaSeries;
class QCheckBox;
class QGraphsTheme;
class QLabel;
class QLineSeries;
class QPushButton;
class QQuickView;
class QValueAxis;

namespace ui {
class ComboBox;

/// Qt Graphs area view of bounded, observed traffic samples. Pausing freezes
/// the display; incoming samples continue to be retained for resuming.
class TrafficGraph : public QWidget {
    Q_OBJECT
public:
    explicit TrafficGraph(QWidget *parent = nullptr);
    ~TrafficGraph() override;
    void append(quint64 up, quint64 down);
    void clear();

private slots:
    void showSampleAt(double fraction);
    void clearHover();
    void renderFrame();

private:
    bool eventFilter(QObject *object, QEvent *event) override;
    void refresh(bool resetScale = false);
    void updateAnimationState();
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void applyTheme();
    void updateDetails();
    qint64 windowMs() const;

    QElapsedTimer clock_;
    core::TrafficHistory history_;
    core::TrafficHistory pausedHistory_;
    core::TrafficSnapshot displayed_;
    std::optional<core::TrafficPoint> leftEdgeSample_;
    qint64 pausedAtMs_ = 0;
    double hoverFraction_ = -1;
    core::TrafficScale renderScale_;
    qint64 scaleShrinkSinceMs_ = -1;
    qint64 renderedAtMs_ = 0;
    ComboBox *windowBox_;
    QPushButton *pauseButton_;
    QCheckBox *downloadToggle_;
    QCheckBox *uploadToggle_;
    QLabel *statistics_;
    QLabel *inspection_;
    QLabel *errorLabel_;
    QQuickView *quick_;
    QWidget *viewContainer_;
    QValueAxis *timeAxis_;
    QValueAxis *rateAxis_;
    QGraphsTheme *graphTheme_;
    QLineSeries *downloadLine_;
    QLineSeries *uploadLine_;
    QAreaSeries *downloadArea_;
    QAreaSeries *uploadArea_;
};
} // namespace ui
