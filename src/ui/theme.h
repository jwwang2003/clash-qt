#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>

class QVBoxLayout;
class QWidget;

namespace ui {
namespace theme {

/// Page metrics. Every page uses these so their control rows line up across
/// the stack.
constexpr int kPageMargin = 12;
constexpr int kPageSpacing = 8;
constexpr int kNavIconSize = 18;

/// The whole palette. Everything the UI paints resolves to one of these, so a
/// retune happens in one place. Neutrals follow the system light/dark scheme
/// and the accent follows the user's system accent colour.
struct Tokens {
    QColor surface;
    QColor surfaceAlt;
    QColor border;
    QColor hover;
    QColor text;
    QColor textDim;
    QColor textFaint;
    QColor accent;
    QColor accentText;
    QColor accentSoft;
    QColor accentFaint;
    QColor success;
    QColor warning;
    QColor danger;
    QColor dangerSoft;
    QColor dangerBorder;
};

const Tokens &tokens();

QColor blend(const QColor &base, const QColor &over, qreal ratio);

enum class Glyph { Profiles, Proxies, Connections, Logs, Rules, Settings };

/// Sidebar glyph, carrying both the resting and the selected tint.
QIcon navIcon(Glyph glyph);

/// Standard page scaffold: control row on top, content below.
QVBoxLayout *pageLayout(QWidget *page);

/// Applies the stylesheet to qApp and keeps it in step with the system
/// light/dark switch. Idempotent.
void install();

class Notifier : public QObject {
    Q_OBJECT

signals:
    void changed();
};

/// Emits changed() once the palette has flipped, for painters holding cached
/// colours.
Notifier *notifier();

}  // namespace theme
}  // namespace ui
