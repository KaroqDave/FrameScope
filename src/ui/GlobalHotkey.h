#pragma once

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>

#include <optional>

namespace framescope::ui {

// A key combination as Windows' RegisterHotKey wants it.
struct NativeHotkey {
    unsigned modifiers = 0; // MOD_ALT 1, MOD_CONTROL 2, MOD_SHIFT 4
    unsigned virtualKey = 0;
};

// Translates the first key of a sequence. Empty if it can't be a global
// hotkey: unknown key, the Windows key, or a key that types something
// (letters, digits, Insert ...) without Ctrl or Alt, which would be taken
// away from every other program.
std::optional<NativeHotkey> toNativeHotkey(const QKeySequence& sequence);

// A system-wide hotkey: pressed() comes while any program, e.g. the game, has
// the keyboard. Windows only; elsewhere set() always fails.
class GlobalHotkey : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject* parent = nullptr);
    ~GlobalHotkey() override;

    [[nodiscard]] static bool isSupported();

    // Registers the hotkey, replacing the current one. An empty sequence
    // switches it off (and succeeds). On failure nothing is registered and
    // lastError() says why.
    bool set(const QKeySequence& sequence);
    [[nodiscard]] const QKeySequence& sequence() const noexcept { return m_sequence; }
    [[nodiscard]] const QString& lastError() const noexcept { return m_lastError; }

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void pressed();

private:
    void unregister();

    QKeySequence m_sequence;
    QString m_lastError;
    bool m_registered = false;
};

// Short tones for feedback while the game covers the screen: rising = capture
// started, falling = stopped, low = it didn't work. Doesn't block.
enum class HotkeyTone { Started, Stopped, Failed };
void playHotkeyTone(HotkeyTone tone);

} // namespace framescope::ui
