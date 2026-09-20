#pragma once

#include <QFrame>

class QLabel;
class QLayout;
class QVBoxLayout;

namespace ui {

/// One titled card on the Settings page. Sections stack down a single column,
/// so the title, the hint and the two banners all sit at the same geometry.
class SettingsSection : public QFrame {
    Q_OBJECT

public:
    SettingsSection(const QString &title, const QString &hint, QWidget *parent = nullptr);

    void addWidget(QWidget *widget);
    void addLayout(QLayout *layout);

    /// Why a control is inert: a platform that cannot do this, a port not yet
    /// known. Empty hides the banner.
    void setNotice(const QString &text);
    /// What just failed. Empty hides the banner.
    void setError(const QString &text);

private:
    QVBoxLayout *body_;
    QLabel *notice_;
    QLabel *error_;
};

}  // namespace ui
