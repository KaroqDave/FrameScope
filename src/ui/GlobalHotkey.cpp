#include "ui/GlobalHotkey.h"

#include <QCoreApplication>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <thread>
#endif

namespace framescope::ui {
namespace {

constexpr int kHotkeyId = 0x4653; // "FS"; ids are per thread, so no clash with other programs

// Windows virtual-key codes (the same values everywhere, so this builds and
// can be checked on any platform).
std::optional<unsigned> virtualKey(Qt::Key key)
{
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return 0x70 + static_cast<unsigned>(key - Qt::Key_F1);
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return 'A' + static_cast<unsigned>(key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return '0' + static_cast<unsigned>(key - Qt::Key_0);
    switch (key) {
    case Qt::Key_Pause:      return 0x13;
    case Qt::Key_ScrollLock: return 0x91;
    case Qt::Key_PageUp:     return 0x21;
    case Qt::Key_PageDown:   return 0x22;
    case Qt::Key_End:        return 0x23;
    case Qt::Key_Home:       return 0x24;
    case Qt::Key_Insert:     return 0x2D;
    case Qt::Key_Delete:     return 0x2E;
    default:                 return std::nullopt;
    }
}

// Keys nobody types with: fine as a hotkey on their own.
bool fineWithoutModifier(Qt::Key key)
{
    return (key >= Qt::Key_F1 && key <= Qt::Key_F24) || key == Qt::Key_Pause || key == Qt::Key_ScrollLock;
}

} // namespace

std::optional<NativeHotkey> toNativeHotkey(const QKeySequence& sequence)
{
    if (sequence.isEmpty())
        return std::nullopt;
    const QKeyCombination combination = sequence[0];
    const auto mods = combination.keyboardModifiers();
    const auto vk = virtualKey(combination.key());
    if (!vk || mods.testFlag(Qt::MetaModifier))
        return std::nullopt;

    NativeHotkey native;
    native.virtualKey = *vk;
    if (mods.testFlag(Qt::AltModifier))
        native.modifiers |= 0x1;
    if (mods.testFlag(Qt::ControlModifier))
        native.modifiers |= 0x2;
    if (mods.testFlag(Qt::ShiftModifier))
        native.modifiers |= 0x4;
    // Shift alone still types ("A", "!"), so it doesn't count.
    if ((native.modifiers & 0x3) == 0 && !fineWithoutModifier(combination.key()))
        return std::nullopt;
    return native;
}

GlobalHotkey::GlobalHotkey(QObject* parent)
    : QObject(parent)
{
    // WM_HOTKEY goes to the thread's message queue, not to a window.
    QCoreApplication::instance()->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey()
{
    unregister();
    if (auto* app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}

bool GlobalHotkey::isSupported()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void GlobalHotkey::unregister()
{
#ifdef _WIN32
    if (m_registered)
        UnregisterHotKey(nullptr, kHotkeyId);
#endif
    m_registered = false;
    m_sequence = {};
}

bool GlobalHotkey::set(const QKeySequence& sequence)
{
    m_lastError.clear();
    if (sequence.isEmpty()) {
        unregister();
        return true;
    }
    const QKeySequence first{sequence[0]};
    const auto native = toNativeHotkey(first);
    if (!native) {
        m_lastError = tr("%1 can't be used as a hotkey. Use a function key, or a letter or digit together with "
                         "Ctrl or Alt.").arg(first.toString(QKeySequence::NativeText));
        return false;
    }
#ifdef _WIN32
    const QKeySequence previous = m_sequence;
    unregister();
    constexpr UINT kNoRepeat = 0x4000; // MOD_NOREPEAT: holding the key down doesn't toggle again and again
    if (!RegisterHotKey(nullptr, kHotkeyId, native->modifiers | kNoRepeat, native->virtualKey)) {
        const auto code = GetLastError();
        m_lastError = code == ERROR_HOTKEY_ALREADY_REGISTERED
                          ? tr("%1 is already taken by another program.").arg(first.toString(QKeySequence::NativeText))
                          : tr("Windows refused the hotkey %1 (error %2).")
                                .arg(first.toString(QKeySequence::NativeText))
                                .arg(code);
        // Back to the one that worked before.
        if (const auto old = toNativeHotkey(previous);
            old && RegisterHotKey(nullptr, kHotkeyId, old->modifiers | kNoRepeat, old->virtualKey)) {
            m_registered = true;
            m_sequence = previous;
        }
        return false;
    }
    m_registered = true;
    m_sequence = first;
    return true;
#else
    m_lastError = tr("Global hotkeys are only available on Windows.");
    return false;
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray& eventType, void* message, qintptr*)
{
#ifdef _WIN32
    if (m_registered && eventType == "windows_generic_MSG") {
        const auto* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_HOTKEY && msg->wParam == static_cast<WPARAM>(kHotkeyId)) {
            emit pressed();
            return true;
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
#endif
    return false;
}

void playHotkeyTone(HotkeyTone tone)
{
#ifdef _WIN32
    // Beep() waits for the tone to end; keep that off the UI thread.
    std::thread([tone] {
        switch (tone) {
        case HotkeyTone::Started:
            Beep(660, 90);
            Beep(990, 120);
            break;
        case HotkeyTone::Stopped:
            Beep(990, 90);
            Beep(660, 120);
            break;
        case HotkeyTone::Failed:
            Beep(220, 350);
            break;
        }
    }).detach();
#else
    Q_UNUSED(tone)
#endif
}

} // namespace framescope::ui
