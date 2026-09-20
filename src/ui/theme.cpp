#include "ui/theme.h"

#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>
#include <QStyleHints>
#include <QVBoxLayout>

#include <utility>

namespace ui {
namespace theme {
namespace {

constexpr auto kStyleSheet = R"QSS(
/* Window chrome keeps its native background; only spacing and hairlines are
   set here so the app still reads as a macOS app. */
QToolBar {
    background: transparent;
    border: none;
    border-bottom: 1px solid {{border}};
    padding: 6px 10px;
    spacing: 6px;
}
QToolBar QToolButton {
    background: transparent;
    color: {{text}};
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 4px 10px;
}
QToolBar QToolButton:hover { background: {{hover}}; border-color: {{border}}; }
QToolBar QToolButton:pressed { background: {{accentSoft}}; border-color: {{accent}}; }
QToolBar QToolButton:disabled { color: {{textFaint}}; }
/* Reserve the 24px arrow segment plus the same 10px gap as the left edge. */
QToolBar QToolButton[popupMode="1"] { padding-right: 34px; }
QToolBar QToolButton::menu-button {
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 24px;
    background: transparent;
    border: none;
    border-left: 1px solid {{border}};
    border-top-right-radius: 5px;
    border-bottom-right-radius: 5px;
}
QToolBar QToolButton::menu-arrow { image: url({{comboArrow}}); width: 12px; height: 12px; }
QToolBar QToolButton:on { background: {{accentSoft}}; border-color: {{accent}}; }
QToolBar::separator { width: 1px; background: {{border}}; margin: 3px 6px; }
QLabel#toolbarLabel { color: {{textDim}}; padding-left: 4px; }

QStatusBar {
    background: transparent;
    border-top: 1px solid {{border}};
    min-height: 26px;
}
QStatusBar::item { border: none; }
QStatusBar QLabel {
    color: {{textDim}};
    padding: 0px 10px;
    border-left: 1px solid {{border}};
}

QListWidget#navList {
    background: {{surfaceAlt}};
    border: none;
    border-right: 1px solid {{border}};
    border-radius: 0px;
    padding: 8px;
    outline: none;
}
QListWidget#navList::item {
    color: {{textDim}};
    border-radius: 6px;
    padding: 7px 10px;
    margin: 1px 0px;
}
QListWidget#navList::item:hover { background: {{hover}}; color: {{text}}; }
QListWidget#navList::item:selected {
    background: {{accentSoft}};
    color: {{accent}};
    font-weight: 600;
}

QAbstractItemView {
    background: {{surface}};
    alternate-background-color: {{surfaceAlt}};
    color: {{text}};
    border: 1px solid {{border}};
    border-radius: 8px;
    selection-background-color: {{accentSoft}};
    selection-color: {{text}};
    outline: none;
}
QListWidget::item { padding: 5px 8px; border-radius: 5px; }
QListWidget::item:selected { background: {{accentSoft}}; color: {{text}}; }
QTableView { gridline-color: {{border}}; }
QTableView::item { padding: 3px 6px; }
QTreeView::item { padding: 4px 6px; }
QTableView::item:selected { background: {{accentSoft}}; color: {{text}}; }
QHeaderView { background: transparent; border: none; }
QHeaderView::section {
    background: {{surfaceAlt}};
    color: {{textDim}};
    padding: 6px 8px;
    border: none;
    border-bottom: 1px solid {{border}};
    border-right: 1px solid {{border}};
}
QHeaderView::section:last { border-right: none; }
QHeaderView::section:hover { color: {{text}}; }

QPushButton {
    background: {{surface}};
    color: {{text}};
    border: 1px solid {{border}};
    border-radius: 6px;
    padding: 5px 14px;
    min-height: 18px;
}
QPushButton:hover { background: {{hover}}; }
QPushButton:pressed { background: {{accentSoft}}; border-color: {{accent}}; }
QPushButton:checked { background: {{accentSoft}}; border-color: {{accent}}; color: {{accent}}; }
QPushButton:disabled { background: {{surfaceAlt}}; color: {{textFaint}}; }

QLineEdit {
    background: {{surface}};
    color: {{text}};
    border: 1px solid {{border}};
    border-radius: 6px;
    padding: 5px 8px;
    selection-background-color: {{accent}};
    selection-color: {{accentText}};
}
QLineEdit:focus { border-color: {{accent}}; }
QLineEdit:disabled { background: {{surfaceAlt}}; color: {{textFaint}}; }

QComboBox {
    background: {{surface}};
    color: {{text}};
    border: 1px solid {{border}};
    border-radius: 6px;
    padding: 5px 8px 5px 10px;
    min-height: 18px;
}
QComboBox:hover { background: {{hover}}; }
QComboBox:focus, QComboBox:on, QComboBox[menuOpen="true"] { border-color: {{accent}}; }
QComboBox:disabled { background: {{surfaceAlt}}; color: {{textFaint}}; }
QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 24px;
    border: none;
    border-top-right-radius: 5px;
    border-bottom-right-radius: 5px;
    background: transparent;
}
QComboBox::down-arrow { image: url({{comboArrow}}); width: 12px; height: 12px; }

QPlainTextEdit {
    background: {{surface}};
    color: {{text}};
    border: 1px solid {{border}};
    border-radius: 8px;
    padding: 6px 8px;
    selection-background-color: {{accent}};
    selection-color: {{accentText}};
}

QSplitter::handle { background: transparent; }
QSplitter::handle:horizontal { width: 8px; }
QSplitter::handle:vertical { height: 8px; }

QLabel#pageSummary { color: {{textDim}}; }
QLabel#errorBanner {
    background: {{dangerSoft}};
    color: {{danger}};
    border: 1px solid {{dangerBorder}};
    border-radius: 6px;
    padding: 6px 10px;
}
QLabel#noticeBanner {
    background: {{surfaceAlt}};
    color: {{textDim}};
    border: 1px solid {{border}};
    border-radius: 6px;
    padding: 6px 10px;
}

QCheckBox { color: {{text}}; spacing: 8px; }
QCheckBox:disabled { color: {{textFaint}}; }

QFrame#settingsCard {
    background: {{surface}};
    border: 1px solid {{border}};
    border-radius: 8px;
}
QLabel#sectionTitle { color: {{text}}; font-weight: 600; }
QLabel#sectionHint { color: {{textDim}}; }
QLabel#fieldLabel { color: {{textDim}}; }
QLabel#fieldError { color: {{danger}}; }
QScrollArea#settingsScroll { background: transparent; border: none; }
)QSS";

bool darkScheme() {
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Unknown) {
        return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    }
    return scheme == Qt::ColorScheme::Dark;
}

Tokens makeTokens() {
    const bool dark = darkScheme();
    const QPalette palette = QGuiApplication::palette();

    Tokens t;
    // Surfaces sit where macOS puts its own base colour, so views do not meet
    // the native window chrome with a different grey.
    t.surface = dark ? QColor("#1f1f22") : QColor("#ffffff");
    t.text = dark ? QColor("#ededf0") : QColor("#1b1b1d");
    t.accent = palette.color(QPalette::Highlight);
    if (dark && t.accent.lightnessF() < 0.6) t.accent = QColor("#66b3ff");
    t.accentText = palette.color(QPalette::HighlightedText);
    if (dark && t.accent.lightnessF() >= 0.6) t.accentText = QColor("#102238");
    t.success = dark ? QColor("#32d158") : QColor("#1c8c4a");
    t.warning = dark ? QColor("#ff9f0a") : QColor("#b06f00");
    t.danger = dark ? QColor("#ff453a") : QColor("#c62d24");

    t.surfaceAlt = blend(t.surface, t.text, dark ? 0.07 : 0.035);
    t.border = blend(t.surface, t.text, dark ? 0.20 : 0.16);
    t.hover = blend(t.surface, t.text, dark ? 0.10 : 0.055);
    t.textDim = blend(t.surface, t.text, 0.62);
    t.textFaint = blend(t.surface, t.text, 0.38);
    t.accentSoft = blend(t.surface, t.accent, dark ? 0.30 : 0.14);
    t.accentFaint = blend(t.surface, t.accent, dark ? 0.13 : 0.10);
    t.dangerSoft = blend(t.surface, t.danger, dark ? 0.20 : 0.10);
    t.dangerBorder = blend(t.surface, t.danger, dark ? 0.45 : 0.35);
    return t;
}

Tokens &cache() {
    static Tokens t = makeTokens();
    return t;
}

QString expand(const Tokens &t) {
    QString qss = QString::fromLatin1(kStyleSheet);
    qss.replace("{{comboArrow}}", darkScheme() ? ":/ui/chevron-down-dark.png"
                                             : ":/ui/chevron-down-light.png");
    const std::pair<const char *, QColor> map[] = {
        {"{{surfaceAlt}}", t.surfaceAlt},     {"{{surface}}", t.surface},
        {"{{border}}", t.border},             {"{{hover}}", t.hover},
        {"{{textDim}}", t.textDim},           {"{{textFaint}}", t.textFaint},
        {"{{text}}", t.text},                 {"{{accentText}}", t.accentText},
        {"{{accentSoft}}", t.accentSoft},     {"{{accentFaint}}", t.accentFaint},
        {"{{accent}}", t.accent},
        {"{{dangerSoft}}", t.dangerSoft},     {"{{dangerBorder}}", t.dangerBorder},
        {"{{danger}}", t.danger},
    };
    for (const auto &[name, color] : map) {
        qss.replace(QLatin1String(name), color.name(QColor::HexRgb));
    }
    return qss;
}

/// Strokes are laid out on the kNavIconSize grid.
void drawGlyph(QPainter *painter, Glyph glyph) {
    switch (glyph) {
        case Glyph::Home:
            painter->drawPolyline(QPolygonF({{2.5, 8}, {9, 2.5}, {15.5, 8}}));
            painter->drawPolyline(QPolygonF({{4.5, 7}, {4.5, 15.5}, {13.5, 15.5}, {13.5, 7}}));
            painter->drawRect(QRectF(7.5, 10.5, 3, 5));
            break;
        case Glyph::Providers:
            painter->drawRoundedRect(QRectF(2.5, 3, 13, 5), 1, 1);
            painter->drawRoundedRect(QRectF(2.5, 10, 13, 5), 1, 1);
            painter->drawPoint(QPointF(5, 5.5));
            painter->drawPoint(QPointF(5, 12.5));
            break;
        case Glyph::Backups:
            painter->drawRect(QRectF(3.5, 6, 11, 9.5));
            painter->drawRect(QRectF(2.5, 2.5, 13, 3.5));
            painter->drawLine(QPointF(7, 9), QPointF(11, 9));
            break;
        case Glyph::Profiles:
            painter->drawRoundedRect(QRectF(3.5, 2.5, 11, 13), 2, 2);
            painter->drawLine(QPointF(6, 6.5), QPointF(13, 6.5));
            painter->drawLine(QPointF(6, 9.5), QPointF(13, 9.5));
            painter->drawLine(QPointF(6, 12.5), QPointF(10.5, 12.5));
            break;
        case Glyph::Proxies:
            painter->drawEllipse(QRectF(2.5, 2.5, 13, 13));
            painter->drawEllipse(QRectF(6.25, 2.5, 5.5, 13));
            painter->drawLine(QPointF(2.5, 9), QPointF(15.5, 9));
            break;
        case Glyph::Connections:
            painter->drawLine(QPointF(3, 6.5), QPointF(14.5, 6.5));
            painter->drawPolyline(QPolygonF({{12, 4.5}, {14.5, 6.5}, {12, 8.5}}));
            painter->drawLine(QPointF(15, 12.5), QPointF(3.5, 12.5));
            painter->drawPolyline(QPolygonF({{6, 10.5}, {3.5, 12.5}, {6, 14.5}}));
            break;
        case Glyph::Logs:
            painter->drawRoundedRect(QRectF(2.5, 3.5, 13, 11), 2, 2);
            painter->drawPolyline(QPolygonF({{5.5, 7}, {8, 9}, {5.5, 11}}));
            painter->drawLine(QPointF(9.5, 11), QPointF(12.5, 11));
            break;
        case Glyph::Rules:
            for (double y : {5.0, 9.0, 13.0}) {
                painter->drawEllipse(QPointF(4, y), 1.1, 1.1);
                painter->drawLine(QPointF(7.5, y), QPointF(15, y));
            }
            break;
        case Glyph::Settings:
            painter->drawLine(QPointF(2.5, 6.5), QPointF(15.5, 6.5));
            painter->drawLine(QPointF(2.5, 11.5), QPointF(15.5, 11.5));
            painter->setBrush(painter->pen().color());
            painter->drawEllipse(QPointF(11.5, 6.5), 2.0, 2.0);
            painter->drawEllipse(QPointF(6.5, 11.5), 2.0, 2.0);
            painter->setBrush(Qt::NoBrush);
            break;
    }
}

QPixmap glyphPixmap(Glyph glyph, const QColor &color) {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pixmap(QSize(kNavIconSize, kNavIconSize) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    drawGlyph(&painter, glyph);
    return pixmap;
}

}  // namespace

const Tokens &tokens() { return cache(); }

QColor blend(const QColor &base, const QColor &over, qreal ratio) {
    return QColor::fromRgbF(base.redF() * (1 - ratio) + over.redF() * ratio,
                            base.greenF() * (1 - ratio) + over.greenF() * ratio,
                            base.blueF() * (1 - ratio) + over.blueF() * ratio);
}

QIcon navIcon(Glyph glyph) {
    const Tokens &t = tokens();
    QIcon icon(glyphPixmap(glyph, t.textDim));
    icon.addPixmap(glyphPixmap(glyph, t.accent), QIcon::Selected);
    icon.addPixmap(glyphPixmap(glyph, t.accent), QIcon::Active);
    return icon;
}

QVBoxLayout *pageLayout(QWidget *page) {
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kPageMargin, kPageMargin, kPageMargin, kPageMargin);
    layout->setSpacing(kPageSpacing);
    return layout;
}

Notifier *notifier() {
    static Notifier instance;
    return &instance;
}

void install() {
    static bool installed = false;
    if (installed) return;
    installed = true;

    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, notifier(),
                     [](Qt::ColorScheme) {
                         cache() = makeTokens();
                         qApp->setStyleSheet(expand(cache()));
                         emit notifier()->changed();
                     });
    qApp->setStyleSheet(expand(cache()));
}

}  // namespace theme
}  // namespace ui
