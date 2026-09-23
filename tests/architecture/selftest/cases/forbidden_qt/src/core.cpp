#include <QWidget>
int arch_selftest_core() { return QWidget::find(0) ? 1 : 0; }
