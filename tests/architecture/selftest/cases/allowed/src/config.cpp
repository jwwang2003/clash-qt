#include <QJSEngine>
int arch_selftest_config() { QJSEngine e; return e.isInterrupted() ? 1 : 0; }
