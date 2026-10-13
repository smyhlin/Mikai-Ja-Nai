#pragma once

#include <QObject>
#include <QPointer>

class QWidget;

namespace mikai::app {

/// The one app-wide window stacking policy.
///
/// A window that opens - a player dialog, the cover viewer, the settings dialog,
/// the compare window - must arrive above the app's other windows rather than
/// hide behind whichever one happened to be raised last, and it must be usable
/// immediately rather than showing the new content behind a window that still
/// holds focus. That is the ordinary expectation for a newly opened window, but
/// Qt does not supply it: each call site would have to remember `raise()` and
/// `activateWindow()`, and the ones that forgot left a fresh player behind the
/// window the viewer was already looking at.
///
/// This is installed once on the QApplication in `main()` and implements the
/// policy for every top-level window in the process, so no call site carries it
/// and a new window inherits it. It hooks QEvent::Show, which is delivered
/// before the window is exposed, so the raise and the activation request land
/// together with the exposure instead of racing it.
///
/// Deliberate non-activating windows keep their behaviour. Applying the
/// activation to those would steal focus from whatever the viewer is doing:
/// the engine build overlay must not interrupt typing while a long TensorRT
/// compile is announced beside the player, and a parked picture-in-picture
/// float must not pull focus back to itself. `isExempt()` names all of them,
/// and the exemption is one-way - a window the policy leaves alone can still
/// raise and activate itself explicitly when it needs to.
///
/// Nothing here makes the app permanently topmost above unrelated
/// applications: the request is a transient raise and activation of one window
/// over the app's own windows, which the window manager may refuse (a focused
/// foreground window owned by another process, a fullscreen game), exactly as
/// any ordinary activation request is. Making every Mikai window topmost would
/// put the app over the viewer's other work for the whole session, which is
/// worse than the missing raise this fixes.
class WindowRaisePolicy : public QObject {
    Q_OBJECT

public:
    /// Installs the policy on the application. Idempotent: the app installs it
    /// once in `main()` and a suite that installs it again gets exactly one live
    /// filter rather than two processing the same Show twice.
    static void install();

    /// True when the policy deliberately leaves a window alone, so a window can
    /// ask before showing rather than being surprised afterwards.
    static bool isExempt(const QWidget* window);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit WindowRaisePolicy(QObject* parent = nullptr);
};

}  // namespace mikai::app
