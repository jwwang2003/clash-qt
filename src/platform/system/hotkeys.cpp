#include "platform/system/hotkeys.h"

#include <QGuiApplication>
#include <QHash>

#if defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#elif defined(Q_OS_WIN)
#include <QAbstractNativeEventFilter>
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <QAbstractNativeEventFilter>
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
// Xlib's macros collide with Qt's enum names.
#undef KeyPress
#undef KeyRelease
#undef Status
#undef None
#endif

namespace platform {
namespace {

QString g_lastError;

struct Binding {
    Hotkeys *owner;
    QString id;
};

QHash<quint32, Binding> g_bindings;
quint32 g_nextNativeId = 1;

void deliver(quint32 nativeId) {
    const auto binding = g_bindings.constFind(nativeId);
    if (binding == g_bindings.constEnd()) {
        return;
    }
    Hotkeys *owner = binding->owner;
    const QString id = binding->id;
    // The native callbacks are not guaranteed to run inside Qt's event loop.
    QMetaObject::invokeMethod(
        owner, [owner, id] { emit owner->triggered(id); }, Qt::QueuedConnection);
}

#if defined(Q_OS_MACOS)

QHash<quint32, EventHotKeyRef> g_hotKeyRefs;

// kVK_ codes are keyboard positions, so neither letters nor digits are contiguous.
constexpr UInt32 kLetterCodes[] = {
    kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
    kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
    kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
    kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z,
};
constexpr UInt32 kDigitCodes[] = {
    kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
    kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
};
constexpr UInt32 kFunctionCodes[] = {
    kVK_F1,  kVK_F2,  kVK_F3,  kVK_F4,  kVK_F5,  kVK_F6,  kVK_F7,  kVK_F8,  kVK_F9,  kVK_F10,
    kVK_F11, kVK_F12, kVK_F13, kVK_F14, kVK_F15, kVK_F16, kVK_F17, kVK_F18, kVK_F19, kVK_F20,
};

struct NamedKey {
    int key;
    UInt32 code;
};
constexpr NamedKey kNamedKeys[] = {
    {Qt::Key_Space, kVK_Space},
    {Qt::Key_Return, kVK_Return},
    {Qt::Key_Enter, kVK_ANSI_KeypadEnter},
    {Qt::Key_Tab, kVK_Tab},
    {Qt::Key_Escape, kVK_Escape},
    {Qt::Key_Backspace, kVK_Delete},
    {Qt::Key_Delete, kVK_ForwardDelete},
    {Qt::Key_Home, kVK_Home},
    {Qt::Key_End, kVK_End},
    {Qt::Key_PageUp, kVK_PageUp},
    {Qt::Key_PageDown, kVK_PageDown},
    {Qt::Key_Left, kVK_LeftArrow},
    {Qt::Key_Right, kVK_RightArrow},
    {Qt::Key_Up, kVK_UpArrow},
    {Qt::Key_Down, kVK_DownArrow},
};

bool keyCode(int key, UInt32 &code) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        code = kLetterCodes[key - Qt::Key_A];
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        code = kDigitCodes[key - Qt::Key_0];
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F20) {
        code = kFunctionCodes[key - Qt::Key_F1];
        return true;
    }
    for (const NamedKey &named : kNamedKeys) {
        if (named.key == key) {
            code = named.code;
            return true;
        }
    }
    return false;
}

UInt32 modifierMask(Qt::KeyboardModifiers modifiers) {
    // Qt reports the command key as ControlModifier on macOS, which is also how
    // QKeySequence renders and parses it.
    UInt32 mask = 0;
    if (modifiers & Qt::ControlModifier) {
        mask |= cmdKey;
    }
    if (modifiers & Qt::MetaModifier) {
        mask |= controlKey;
    }
    if (modifiers & Qt::AltModifier) {
        mask |= optionKey;
    }
    if (modifiers & Qt::ShiftModifier) {
        mask |= shiftKey;
    }
    return mask;
}

OSStatus hotKeyPressed(EventHandlerCallRef, EventRef event, void *) {
    EventHotKeyID hotKey;
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                          sizeof(hotKey), nullptr, &hotKey) == noErr) {
        deliver(hotKey.id);
    }
    return noErr;
}

bool handlerInstalled() {
    static const bool installed = [] {
        const EventTypeSpec pressed{kEventClassKeyboard, kEventHotKeyPressed};
        return InstallApplicationEventHandler(&hotKeyPressed, 1, &pressed, nullptr, nullptr) ==
               noErr;
    }();
    return installed;
}

bool nativeRegister(quint32 nativeId, const QKeySequence &sequence) {
    UInt32 code = 0;
    if (!keyCode(sequence[0].key(), code)) {
        g_lastError = QStringLiteral("%1 uses a key this platform cannot bind")
                          .arg(sequence.toString(QKeySequence::NativeText));
        return false;
    }
    if (!handlerInstalled()) {
        g_lastError = QStringLiteral("Cannot install the hotkey event handler");
        return false;
    }

    EventHotKeyRef ref = nullptr;
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    const OSStatus status = RegisterEventHotKey(
        code, modifierMask(sequence[0].keyboardModifiers()),
        EventHotKeyID{'cqtk', nativeId}, GetApplicationEventTarget(), 0, &ref);
    QT_WARNING_POP
    if (status != noErr) {
        g_lastError = QStringLiteral("%1 is already taken (OSStatus %2)")
                          .arg(sequence.toString(QKeySequence::NativeText))
                          .arg(status);
        return false;
    }
    g_hotKeyRefs.insert(nativeId, ref);
    return true;
}

void nativeUnregister(quint32 nativeId) {
    if (EventHotKeyRef ref = g_hotKeyRefs.take(nativeId)) {
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        UnregisterEventHotKey(ref);
        QT_WARNING_POP
    }
}

#elif defined(Q_OS_WIN)

class HotkeyFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override {
        auto *msg = static_cast<MSG *>(message);
        if (type != "windows_generic_MSG" || msg->message != WM_HOTKEY) {
            return false;
        }
        deliver(static_cast<quint32>(msg->wParam));
        return true;
    }
};

bool keyCode(int key, UINT &code) {
    // Virtual key codes for letters and digits are their ASCII values, as Qt's are.
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)) {
        code = static_cast<UINT>(key);
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        code = VK_F1 + static_cast<UINT>(key - Qt::Key_F1);
        return true;
    }
    switch (key) {
    case Qt::Key_Space: code = VK_SPACE; return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: code = VK_RETURN; return true;
    case Qt::Key_Tab: code = VK_TAB; return true;
    case Qt::Key_Escape: code = VK_ESCAPE; return true;
    case Qt::Key_Left: code = VK_LEFT; return true;
    case Qt::Key_Right: code = VK_RIGHT; return true;
    case Qt::Key_Up: code = VK_UP; return true;
    case Qt::Key_Down: code = VK_DOWN; return true;
    default: return false;
    }
}

UINT modifierMask(Qt::KeyboardModifiers modifiers) {
    UINT mask = MOD_NOREPEAT;
    if (modifiers & Qt::ControlModifier) {
        mask |= MOD_CONTROL;
    }
    if (modifiers & Qt::AltModifier) {
        mask |= MOD_ALT;
    }
    if (modifiers & Qt::ShiftModifier) {
        mask |= MOD_SHIFT;
    }
    if (modifiers & Qt::MetaModifier) {
        mask |= MOD_WIN;
    }
    return mask;
}

bool nativeRegister(quint32 nativeId, const QKeySequence &sequence) {
    UINT code = 0;
    if (!keyCode(sequence[0].key(), code)) {
        g_lastError = QStringLiteral("%1 uses a key this platform cannot bind")
                          .arg(sequence.toString(QKeySequence::NativeText));
        return false;
    }
    static HotkeyFilter filter;
    static const bool installed = [] {
        qApp->installNativeEventFilter(&filter);
        return true;
    }();
    Q_UNUSED(installed)

    if (!RegisterHotKey(nullptr, static_cast<int>(nativeId),
                        modifierMask(sequence[0].keyboardModifiers()), code)) {
        g_lastError = QStringLiteral("%1 is already taken (error %2)")
                          .arg(sequence.toString(QKeySequence::NativeText))
                          .arg(GetLastError());
        return false;
    }
    return true;
}

void nativeUnregister(quint32 nativeId) { UnregisterHotKey(nullptr, static_cast<int>(nativeId)); }

#elif defined(Q_OS_LINUX)

// The lock keys are modifiers to X11, so every combination has to be grabbed
// once per lock state for the hotkey to survive Caps or Num lock.
constexpr unsigned kLockMasks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

struct GrabbedKey {
    xcb_keycode_t code;
    unsigned modifiers;
};
QHash<quint32, GrabbedKey> g_grabbedKeys;

bool g_grabFailed = false;
int grabErrorHandler(Display *, XErrorEvent *) {
    g_grabFailed = true;
    return 0;
}

Display *display() {
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->display() : nullptr;
}

class HotkeyFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override {
        if (type != "xcb_generic_event_t") {
            return false;
        }
        auto *event = static_cast<xcb_generic_event_t *>(message);
        if ((event->response_type & ~0x80) != XCB_KEY_PRESS) {
            return false;
        }
        auto *key = reinterpret_cast<xcb_key_press_event_t *>(event);
        const unsigned modifiers = key->state & ~(LockMask | Mod2Mask);
        for (auto it = g_grabbedKeys.constBegin(); it != g_grabbedKeys.constEnd(); ++it) {
            if (it->code == key->detail && it->modifiers == modifiers) {
                deliver(it.key());
                return true;
            }
        }
        return false;
    }
};

bool keySym(int key, KeySym &sym) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        sym = XK_a + (key - Qt::Key_A);
        return true;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        sym = XK_0 + (key - Qt::Key_0);
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F20) {
        sym = XK_F1 + (key - Qt::Key_F1);
        return true;
    }
    switch (key) {
    case Qt::Key_Space: sym = XK_space; return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: sym = XK_Return; return true;
    case Qt::Key_Tab: sym = XK_Tab; return true;
    case Qt::Key_Escape: sym = XK_Escape; return true;
    case Qt::Key_Left: sym = XK_Left; return true;
    case Qt::Key_Right: sym = XK_Right; return true;
    case Qt::Key_Up: sym = XK_Up; return true;
    case Qt::Key_Down: sym = XK_Down; return true;
    default: return false;
    }
}

unsigned modifierMask(Qt::KeyboardModifiers modifiers) {
    unsigned mask = 0;
    if (modifiers & Qt::ControlModifier) {
        mask |= ControlMask;
    }
    if (modifiers & Qt::AltModifier) {
        mask |= Mod1Mask;
    }
    if (modifiers & Qt::ShiftModifier) {
        mask |= ShiftMask;
    }
    if (modifiers & Qt::MetaModifier) {
        mask |= Mod4Mask;
    }
    return mask;
}

bool nativeRegister(quint32 nativeId, const QKeySequence &sequence) {
    Display *dpy = display();
    KeySym sym = 0;
    if (!dpy || !keySym(sequence[0].key(), sym)) {
        g_lastError = QStringLiteral("%1 uses a key this platform cannot bind")
                          .arg(sequence.toString(QKeySequence::NativeText));
        return false;
    }
    const xcb_keycode_t code = XKeysymToKeycode(dpy, sym);
    const unsigned modifiers = modifierMask(sequence[0].keyboardModifiers());

    static HotkeyFilter filter;
    static const bool installed = [] {
        qApp->installNativeEventFilter(&filter);
        return true;
    }();
    Q_UNUSED(installed)

    // XGrabKey is asynchronous, so a refusal only shows up as a BadAccess error.
    g_grabFailed = false;
    XErrorHandler previous = XSetErrorHandler(&grabErrorHandler);
    for (unsigned lock : kLockMasks) {
        XGrabKey(dpy, code, modifiers | lock, DefaultRootWindow(dpy), True, GrabModeAsync,
                 GrabModeAsync);
    }
    XSync(dpy, False);
    XSetErrorHandler(previous);
    if (g_grabFailed) {
        for (unsigned lock : kLockMasks) {
            XUngrabKey(dpy, code, modifiers | lock, DefaultRootWindow(dpy));
        }
        g_lastError = QStringLiteral("%1 is already taken")
                          .arg(sequence.toString(QKeySequence::NativeText));
        return false;
    }
    g_grabbedKeys.insert(nativeId, GrabbedKey{code, modifiers});
    return true;
}

void nativeUnregister(quint32 nativeId) {
    Display *dpy = display();
    const GrabbedKey key = g_grabbedKeys.take(nativeId);
    if (!dpy || key.code == 0) {
        return;
    }
    for (unsigned lock : kLockMasks) {
        XUngrabKey(dpy, key.code, key.modifiers | lock, DefaultRootWindow(dpy));
    }
}

#else

bool nativeRegister(quint32, const QKeySequence &) { return false; }
void nativeUnregister(quint32) {}

#endif

}  // namespace

Hotkeys::Hotkeys(QObject *parent) : QObject(parent) {}

Hotkeys::~Hotkeys() { unbindAll(); }

bool Hotkeys::isSupported() {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_LINUX)
    // XGrabKey has no Wayland counterpart; grabbing there needs the global
    // shortcuts portal, which is a separate mechanism entirely.
    return QGuiApplication::platformName() == QLatin1String("xcb");
#else
    return false;
#endif
}

bool Hotkeys::bind(const QString &id, const QKeySequence &sequence) {
    unbind(id);
    g_lastError.clear();
    if (sequence.isEmpty()) {
        return true;
    }
    if (!isSupported()) {
        g_lastError = QStringLiteral("Global hotkeys are not available on this platform");
        return false;
    }
    if (sequence.count() != 1) {
        g_lastError = QStringLiteral("A hotkey must be a single key combination");
        return false;
    }

    const quint32 nativeId = g_nextNativeId++;
    if (!nativeRegister(nativeId, sequence)) {
        return false;
    }
    g_bindings.insert(nativeId, Binding{this, id});
    return true;
}

void Hotkeys::unbind(const QString &id) {
    for (auto it = g_bindings.begin(); it != g_bindings.end(); ++it) {
        if (it->owner == this && it->id == id) {
            nativeUnregister(it.key());
            g_bindings.erase(it);
            return;
        }
    }
}

void Hotkeys::unbindAll() {
    for (auto it = g_bindings.begin(); it != g_bindings.end();) {
        if (it->owner == this) {
            nativeUnregister(it.key());
            it = g_bindings.erase(it);
        } else {
            ++it;
        }
    }
}

QString Hotkeys::lastError() const { return g_lastError; }

}  // namespace platform
