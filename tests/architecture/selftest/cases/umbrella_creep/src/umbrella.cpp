#include <QWidget>
int arch_selftest_umbrella() { return QWidget::find(0) ? 1 : 0; }
