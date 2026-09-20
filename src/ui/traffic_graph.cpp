#include "ui/traffic_graph.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QAreaSeries>
#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QWheelEvent>
#include <QCheckBox>
#include <QGraphsTheme>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineSeries>
#include <QPushButton>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <QSurfaceFormat>
#include <QShowEvent>
#include <QHideEvent>
#include <QValueAxis>
#include <QVBoxLayout>

#include "ui/combo_box.h"
#include "ui/theme.h"

namespace ui {
namespace {
QString rateText(double bytesPerSecond) {
    static const QStringList units{"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s", "PiB/s", "EiB/s"};
    double value = std::max(0.0, bytesPerSecond);
    int unit = 0;
    while (value >= 1024 && unit < units.size() - 1) { value /= 1024; ++unit; }
    return QString::number(value, 'f', value < 10 ? 1 : 0) + ' ' + units.at(unit);
}

double areaCoordinate(double rate, double divisor) {
    // Qt Graphs 6.11 AreaRenderer starts a new subpath at consecutive exact
    // zeros, then closes the final subpath back to the first sample. When that
    // sample is nonzero this paints a diagonal band above later zero traffic.
    // A positive sentinel keeps a single path and still rounds to the exact
    // baseline in pixel coordinates. Only render geometry uses this value;
    // observed samples, statistics, and hover readouts retain their true zeros.
    return rate == 0 ? std::numeric_limits<double>::min() : rate / divisor;
}

QColor areaFill(QColor color) {
    color.setAlphaF(0.24);
    return color;
}
} // namespace

TrafficGraph::TrafficGraph(QWidget *parent) : QWidget(parent) {
    clock_.start();
    setObjectName("trafficGraph");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto *controls = new QHBoxLayout;
    controls->setSpacing(10);
    downloadToggle_ = new QCheckBox(tr("↓ Download"), this);
    downloadToggle_->setObjectName("trafficDownloadVisible");
    downloadToggle_->setChecked(true);
    uploadToggle_ = new QCheckBox(tr("↑ Upload"), this);
    uploadToggle_->setObjectName("trafficUploadVisible");
    uploadToggle_->setChecked(true);
    windowBox_ = new ComboBox(this);
    windowBox_->setObjectName("trafficTimeWindow");
    windowBox_->setAccessibleName(tr("Traffic time window"));
    windowBox_->addItem(tr("1 minute"), core::TrafficHistory::OneMinute);
    windowBox_->addItem(tr("5 minutes"), core::TrafficHistory::FiveMinutes);
    windowBox_->addItem(tr("15 minutes"), core::TrafficHistory::FifteenMinutes);
    pauseButton_ = new QPushButton(tr("Pause"), this);
    pauseButton_->setObjectName("trafficPause");
    pauseButton_->setCheckable(true);
    pauseButton_->setToolTip(tr("Freeze the graph while continuing to collect traffic samples"));
    controls->addWidget(downloadToggle_);
    controls->addSpacing(8);
    controls->addWidget(uploadToggle_);
    controls->addStretch();
    controls->addWidget(windowBox_);
    controls->addWidget(pauseButton_);
    layout->addLayout(controls);

    timeAxis_ = new QValueAxis(this);
    timeAxis_->setObjectName("trafficTimeAxis");
    timeAxis_->setRange(-60, 0);
    timeAxis_->setTickInterval(15);
    timeAxis_->setSubGridVisible(false);
    timeAxis_->setLabelFormat("%.0f s");
    timeAxis_->setLabelDecimals(0);
    rateAxis_ = new QValueAxis(this);
    rateAxis_->setObjectName("trafficRateAxis");
    rateAxis_->setRange(0, 1);
    rateAxis_->setSubGridVisible(false);
    rateAxis_->setTitleVisible(true);
    graphTheme_ = new QGraphsTheme(this);
    graphTheme_->setTheme(QGraphsTheme::Theme::UserDefined);
    downloadArea_ = new QAreaSeries(this);
    downloadArea_->setObjectName("trafficDownloadArea");
    downloadArea_->setName(tr("Download"));
    downloadArea_->setBorderWidth(1.8);
    downloadLine_ = new QLineSeries(downloadArea_);
    downloadLine_->setObjectName("trafficDownloadSamples");
    downloadArea_->setUpperSeries(downloadLine_);
    uploadArea_ = new QAreaSeries(this);
    uploadArea_->setObjectName("trafficUploadArea");
    uploadArea_->setName(tr("Upload"));
    uploadArea_->setBorderWidth(1.8);
    uploadLine_ = new QLineSeries(uploadArea_);
    uploadLine_->setObjectName("trafficUploadSamples");
    uploadArea_->setUpperSeries(uploadLine_);
    for (QObject *object : QList<QObject *>{timeAxis_, rateAxis_, graphTheme_, downloadArea_, uploadArea_})
        QQmlEngine::setObjectOwnership(object, QQmlEngine::CppOwnership);

    quick_ = new QQuickView;
    quick_->setObjectName("trafficGraphsView");
    // A native Quick window retains threaded, display-synchronized rendering.
    // Do not replace this with QQuickWidget: it disables that render loop.
    QSurfaceFormat surfaceFormat = quick_->format();
    surfaceFormat.setSamples(4);
    surfaceFormat.setSwapInterval(1);
    quick_->setFormat(surfaceFormat);
    quick_->setResizeMode(QQuickView::SizeRootObjectToView);
    viewContainer_ = QWidget::createWindowContainer(quick_, this);
    viewContainer_->setObjectName("trafficGraphsContainer");
    quick_->installEventFilter(this);
    viewContainer_->setMinimumHeight(220);
    viewContainer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The plot is hover-only; keep keyboard navigation on the widget controls.
    viewContainer_->setFocusPolicy(Qt::NoFocus);
    viewContainer_->setAccessibleName(tr("Traffic area graph"));
    viewContainer_->setAccessibleDescription(tr("Download and upload throughput over the selected time window; averages and peaks are shown below."));
    layout->addWidget(viewContainer_, 1);
    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName("errorBanner");
    errorLabel_->setWordWrap(true);
    errorLabel_->setTextFormat(Qt::PlainText);
    errorLabel_->hide();
    layout->addWidget(errorLabel_);
    statistics_ = new QLabel(this);
    statistics_->setObjectName("trafficStatistics");
    statistics_->setWordWrap(true);
    statistics_->setTextFormat(Qt::PlainText);
    statistics_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statistics_->setToolTip(tr("Averages use observed samples in the selected window. Missing samples are not counted as zero; the graph connects adjacent observations."));
    inspection_ = new QLabel(this);
    inspection_->setObjectName("trafficInspection");
    inspection_->setWordWrap(true);
    inspection_->setTextFormat(Qt::PlainText);
    inspection_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(statistics_);
    layout->addWidget(inspection_);

    applyTheme();
    quick_->setInitialProperties({
        {"timeAxis", QVariant::fromValue(timeAxis_)},
        {"rateAxis", QVariant::fromValue(rateAxis_)},
        {"graphTheme", QVariant::fromValue(graphTheme_)},
        {"downloadSeries", QVariant::fromValue(downloadArea_)},
        {"uploadSeries", QVariant::fromValue(uploadArea_)},
        {"hintColor", theme::tokens().textDim},
        {"hoverColor", theme::tokens().textDim}
    });
    connect(quick_, &QQuickView::statusChanged, this, [this](QQuickView::Status status) {
        if (status == QQuickView::Error) {
            QStringList reasons;
            for (const auto &error : quick_->errors()) reasons.append(error.toString());
            errorLabel_->setText(tr("Traffic graph could not load: %1").arg(reasons.join('\n')));
            errorLabel_->show();
        } else if (status == QQuickView::Ready) {
            errorLabel_->hide();
            connect(quick_->rootObject(), SIGNAL(sampleHovered(double)), this, SLOT(showSampleAt(double)));
            connect(quick_->rootObject(), SIGNAL(hoverEnded()), this, SLOT(clearHover()));
            connect(quick_->rootObject(), SIGNAL(frameRequested()), this, SLOT(renderFrame()));
            applyTheme();
            refresh();
        }
    });
    connect(quick_, &QQuickView::sceneGraphError, this, [this](QQuickWindow::SceneGraphError, const QString &message) {
        errorLabel_->setText(tr("Traffic graph rendering failed: %1").arg(message));
        errorLabel_->show();
    }, Qt::QueuedConnection);
    quick_->loadFromModule("ClashQt", "TrafficGraph");
    connect(theme::notifier(), &theme::Notifier::changed, this, &TrafficGraph::applyTheme);
    connect(windowBox_, &QComboBox::currentIndexChanged, this, [this] { clearHover(); refresh(true); });
    connect(downloadToggle_, &QCheckBox::toggled, this, [this](bool visible) { downloadArea_->setVisible(visible); refresh(true); });
    connect(uploadToggle_, &QCheckBox::toggled, this, [this](bool visible) { uploadArea_->setVisible(visible); refresh(true); });
    connect(pauseButton_, &QPushButton::toggled, this, [this](bool paused) {
        if (paused) { pausedAtMs_ = clock_.elapsed(); pausedHistory_ = history_; }
        pauseButton_->setText(paused ? tr("Resume") : tr("Pause"));
        clearHover();
        refresh(true);
    });
    auto *timer = new QTimer(this);
    // Statistics/expiry are independent of frame pacing; do not rebuild labels
    // or axes on every animation frame.
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible() && !pauseButton_->isChecked()) refresh();
    });
    timer->start();
    refresh();
}

TrafficGraph::~TrafficGraph() {
    // Destroy the QML view before the externally owned axes, theme, and series.
    // The container owns the QQuickView and shuts down its render thread.
    quick_->removeEventFilter(this);
    delete viewContainer_;
    quick_ = nullptr;
}

bool TrafficGraph::eventFilter(QObject *object, QEvent *event) {
    if (object == quick_ && event->type() == QEvent::Wheel) {
        // Native child windows do not propagate wheel input through QWidget
        // parents. The hover-only plot should scroll the surrounding Home page.
        for (auto *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            auto *scroll = qobject_cast<QAbstractScrollArea *>(ancestor);
            if (!scroll) continue;
            const auto *wheel = static_cast<QWheelEvent *>(event);
            QWheelEvent forwarded(scroll->viewport()->mapFromGlobal(wheel->globalPosition()),
                                  wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(),
                                  wheel->buttons(), wheel->modifiers(), wheel->phase(),
                                  wheel->inverted(), wheel->source(), wheel->pointingDevice());
            QCoreApplication::sendEvent(scroll->viewport(), &forwarded);
            event->setAccepted(forwarded.isAccepted());
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}

void TrafficGraph::append(quint64 up, quint64 down) {
    history_.append(clock_.elapsed(), static_cast<double>(up), static_cast<double>(down));
    if (isVisible() && !pauseButton_->isChecked()) refresh();
}

void TrafficGraph::clear() {
    history_.clear();
    pausedHistory_.clear();
    leftEdgeSample_.reset();
    pausedAtMs_ = clock_.elapsed();
    clearHover();
    refresh(true);
}

qint64 TrafficGraph::windowMs() const { return windowBox_->currentData().toLongLong(); }

void TrafficGraph::refresh(bool resetScale) {
    const bool paused = pauseButton_->isChecked();
    const qint64 reference = paused ? pausedAtMs_ : clock_.elapsed();
    const qint64 cutoff = reference - windowMs();
    if (leftEdgeSample_ && leftEdgeSample_->timestampMs >= cutoff) leftEdgeSample_.reset();
    for (const auto &point : displayed_.samples) {
        if (point.timestampMs >= cutoff) break;
        leftEdgeSample_ = point;
    }
    displayed_ = (paused ? pausedHistory_ : history_).snapshot(windowMs(), reference);
    const double peak = std::max(downloadToggle_->isChecked() ? displayed_.stats.peakDownloadBps : 0,
                                 uploadToggle_->isChecked() ? displayed_.stats.peakUploadBps : 0);
    const auto candidate = core::TrafficHistory::scaleForPeak(peak);
    const double capacity = renderScale_.maximum * renderScale_.divisor;
    const double desiredCapacity = candidate.maximum * candidate.divisor;
    // Expand immediately for real peaks, but require sustained headroom before
    // shrinking. Otherwise a departing peak makes the entire plot jump around.
    if (resetScale || desiredCapacity > capacity || displayed_.samples.isEmpty()) {
        renderScale_ = candidate;
        scaleShrinkSinceMs_ = -1;
    } else if (desiredCapacity < capacity * 0.6) {
        if (scaleShrinkSinceMs_ < 0) scaleShrinkSinceMs_ = reference;
        if (reference - scaleShrinkSinceMs_ >= 10000) {
            renderScale_ = candidate;
            scaleShrinkSinceMs_ = -1;
        }
    } else {
        scaleShrinkSinceMs_ = -1;
    }
    const auto &scale = renderScale_;
    timeAxis_->setRange(-windowMs() / 1000.0, 0);
    timeAxis_->setTickInterval(windowMs() == core::TrafficHistory::OneMinute ? 15 :
                               windowMs() == core::TrafficHistory::FiveMinutes ? 60 : 180);
    rateAxis_->setRange(0, scale.maximum);
    rateAxis_->setTickInterval(scale.maximum / 5);
    rateAxis_->setLabelDecimals(scale.maximum < 10 ? 1 : 0);
    rateAxis_->setTitleText(scale.unit);
    if (auto *root = quick_->rootObject()) {
        QString message;
        if (!downloadToggle_->isChecked() && !uploadToggle_->isChecked()) message = tr("Enable a series to view traffic");
        else if (displayed_.samples.isEmpty()) message = paused ? tr("No samples in this paused window") : tr("Waiting for traffic samples…");
        else if (displayed_.samples.size() == 1) message = tr("Collecting traffic history…");
        root->setProperty("emptyMessage", message);
    }
    updateDetails();
    updateAnimationState();
    // Live geometry changes are coalesced into the next animation frame.
    if (resetScale || pauseButton_->isChecked() || !isVisible()
        || displayed_.samples.isEmpty() || downloadLine_->count() == 0)
        renderFrame();
}

void TrafficGraph::renderFrame() {
    renderedAtMs_ = pauseButton_->isChecked() ? pausedAtMs_ : clock_.elapsed();
    QList<QPointF> downloads, uploads;
    downloads.reserve(displayed_.samples.size());
    uploads.reserve(displayed_.samples.size());
    const double left = -windowMs() / 1000.0;
    auto previous = leftEdgeSample_;
    for (const auto &sample : displayed_.samples) {
        const double seconds = (sample.timestampMs - renderedAtMs_) / 1000.0;
        if (seconds < left) { previous = sample; continue; }
        // Preserve the segment crossing the left boundary. Dropping its first
        // point outright makes the filled polygon snap to zero once per sample.
        if (downloads.isEmpty() && previous && previous->timestampMs < sample.timestampMs) {
            const double previousSeconds = (previous->timestampMs - renderedAtMs_) / 1000.0;
            if (previousSeconds < left && sample.timestampMs - previous->timestampMs <= 2000) {
                const double fraction = (left - previousSeconds) / (seconds - previousSeconds);
                downloads.append(QPointF(left, areaCoordinate(std::lerp(previous->downloadBps, sample.downloadBps, fraction), renderScale_.divisor)));
                uploads.append(QPointF(left, areaCoordinate(std::lerp(previous->uploadBps, sample.uploadBps, fraction), renderScale_.divisor)));
            }
        }
        downloads.append(QPointF(seconds, areaCoordinate(sample.downloadBps, renderScale_.divisor)));
        uploads.append(QPointF(seconds, areaCoordinate(sample.uploadBps, renderScale_.divisor)));
    }
    // Both areas are committed in the same GUI/scene-graph animation turn.
    // Never clear then append: that exposes empty/intermediate polygons.
    downloadLine_->replace(downloads);
    uploadLine_->replace(uploads);
    if (hoverFraction_ >= 0) showSampleAt(hoverFraction_);
}

void TrafficGraph::updateAnimationState() {
    if (auto *root = quick_->rootObject())
        root->setProperty("animate", isVisible() && !pauseButton_->isChecked()
            && !displayed_.samples.isEmpty()
            && (downloadToggle_->isChecked() || uploadToggle_->isChecked()));
}

void TrafficGraph::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    refresh();
}

void TrafficGraph::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    updateAnimationState();
}

void TrafficGraph::updateDetails() {
    const auto &stats = displayed_.stats;
    if (stats.sampleCount == 0) {
        statistics_->setText(tr("↓ Download  Avg — · Peak —\n↑ Upload  Avg — · Peak —"));
    } else {
        statistics_->setText(tr("↓ Download  Avg %1 · Peak %2\n↑ Upload  Avg %3 · Peak %4")
            .arg(rateText(stats.averageDownloadBps), rateText(stats.peakDownloadBps),
                 rateText(stats.averageUploadBps), rateText(stats.peakUploadBps)));
    }
    if (hoverFraction_ < 0) {
        inspection_->setText(tr("%1 · %2 samples · Hover to inspect")
            .arg(pauseButton_->isChecked() ? tr("Paused — still collecting") : tr("Live"))
            .arg(stats.sampleCount));
    }
}

void TrafficGraph::showSampleAt(double fraction) {
    hoverFraction_ = std::clamp(fraction, 0.0, 1.0);
    const qint64 timestamp = renderedAtMs_ - windowMs() + qRound64(windowMs() * hoverFraction_);
    const auto sample = displayed_.nearest(timestamp);
    if (!sample || timestamp < displayed_.samples.first().timestampMs ||
        qAbs(sample->timestampMs - timestamp) > 2000) {
        inspection_->setText(tr("No observed sample at this time"));
        return;
    }
    const qint64 age = qMax<qint64>(0, (renderedAtMs_ - sample->timestampMs) / 1000);
    inspection_->setText(tr("%1 s ago · ↓ %2 · ↑ %3%4")
        .arg(age).arg(rateText(sample->downloadBps), rateText(sample->uploadBps),
                     pauseButton_->isChecked() ? tr(" · paused") : QString()));
}

void TrafficGraph::clearHover() {
    hoverFraction_ = -1;
    updateDetails();
}

void TrafficGraph::applyTheme() {
    const auto &t = theme::tokens();
    QColor uploadColor = t.success;
    if (qAbs(uploadColor.red() - t.accent.red()) + qAbs(uploadColor.green() - t.accent.green()) +
        qAbs(uploadColor.blue() - t.accent.blue()) < 100) uploadColor = t.warning;
    graphTheme_->setColorScheme(t.surface.lightness() < 128 ? QGraphsTheme::ColorScheme::Dark : QGraphsTheme::ColorScheme::Light);
    graphTheme_->setBackgroundVisible(true);
    graphTheme_->setBackgroundColor(t.surface);
    graphTheme_->setPlotAreaBackgroundVisible(true);
    graphTheme_->setPlotAreaBackgroundColor(t.surface);
    graphTheme_->setGridVisible(true);
    QGraphsLine grid;
    grid.setMainColor(t.border);
    grid.setSubColor(t.surfaceAlt);
    grid.setMainWidth(1);
    grid.setSubWidth(0);
    graphTheme_->setGrid(grid);
    QGraphsLine axis;
    axis.setMainColor(t.border);
    axis.setSubColor(t.border);
    axis.setMainWidth(1);
    axis.setLabelTextColor(t.textDim);
    graphTheme_->setAxisX(axis);
    graphTheme_->setAxisY(axis);
    QFont axisFont = font();
    axisFont.setPointSizeF(qMax(10.0, font().pointSizeF() - 1));
    graphTheme_->setAxisXLabelFont(axisFont);
    graphTheme_->setAxisYLabelFont(axisFont);
    graphTheme_->setLabelTextColor(t.textDim);
    rateAxis_->setTitleColor(t.textDim);
    rateAxis_->setTitleFont(axisFont);
    downloadArea_->setColor(areaFill(t.accent));
    downloadArea_->setBorderColor(t.accent);
    uploadArea_->setColor(areaFill(uploadColor));
    uploadArea_->setBorderColor(uploadColor);
    downloadToggle_->setStyleSheet(QString("QCheckBox { color: %1; }").arg(t.accent.name()));
    uploadToggle_->setStyleSheet(QString("QCheckBox { color: %1; }").arg(uploadColor.name()));
    quick_->setColor(t.surface);
    if (auto *root = quick_->rootObject()) {
        root->setProperty("hintColor", t.textDim);
        root->setProperty("hoverColor", t.textDim);
    }
}
} // namespace ui
