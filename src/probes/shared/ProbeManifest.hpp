// The declarative probe manifest: one JSON document describing a user-visible
// behaviour, the interaction that should produce it, and what must be true
// afterwards. The runner reads these; nothing here knows about Qt or mpv, so a
// manifest can be written without reading a line of C++.
//
// A manifest names widgets by `objectName` and the core by mpv property name.
// It never carries coordinates, pixel expectations or sleeps: what it can read
// is what a user can reach.

#pragma once

#include <optional>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <Qt>

#include <QVector>

namespace mikai::probes {

/// Which surface the runner builds before the steps run. The manifest never
/// constructs a widget itself, so the surface is part of the scenario.
enum class ProbeSurface {
    VideoSurface,  ///< the render surface alone, no chrome
    PlayerDialog,  ///< the real player: shell, dock, popovers, pip pool
};

/// One thing the probe does to the application.
struct ProbeStep {
    enum class Kind {
        Click,        ///< press+release the left button on a widget
        DoubleClick,
        KeyPress,     ///< a key, optionally with Ctrl/Alt/Shift
        SetValue,     ///< write a widget property (the real setter, so it emits)
        TypeText,     ///< type into a focusable widget
        Focus,
        WaitForSignal,///< block until a named signal arrives
        WaitFor,      ///< block until a predicate over observable state holds
        SleepMs,      ///< bounded settle only; never the primary synchroniser
    };
    Kind kind = Kind::Click;
    /// `objectName` of the target. Empty for key_press and the waits.
    QString target;
    /// Property name for set_value, signal name for wait_for_signal, and the
    /// predicate name for wait_for.
    QString name;
    QVariant value;
    /// Key text for key_press ("Space", "Q", "Escape").
    QString key;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int timeoutMs = 5000;
    /// human-readable note, printed when the step fails
    QString note;
};

/// The shape an mpv property is read as. Named in the manifest rather than
/// guessed: reading a flag as a number would answer 0 and read as a pass.
enum class ProbeMpvKind { Number, Flag, Text };

/// One thing that must be true. Behaviour only: an assertion that could not
/// fail on any build is not worth running.
struct ProbeAssertion {
    enum class Kind {
        SignalEmitted,
        QtProperty,
        MpvProperty,
        WidgetProperty,   ///< widget property the shell owns (checked, value)
        WidgetExists,
        SignalNotEmitted,
    };
    Kind kind = Kind::QtProperty;
    QString target;   ///< objectName for the widget assertions
    QString property; ///< Qt property, or mpv property name
    ProbeMpvKind mpvKind = ProbeMpvKind::Number;
    QVariant expected;
    /// `equals` (default), `not_equals`, `greater_than`, `less_than`,
    /// `approximately` (with `tolerance`), `contains`.
    QString comparison = QStringLiteral("equals");
    double tolerance = 0.0;
    QString note;
};

struct ProbeManifest {
    QString name;
    QString description;
    QString category; ///< ui-interaction | player-behavior | state-sync | regression
    ProbeSurface surface = ProbeSurface::VideoSurface;
    /// Media the surface opens. Required for PlayerDialog; empty is refused.
    QString media;
    /// Widget size for the surface under test.
    int width = 640;
    int height = 360;
    QVector<ProbeStep> steps;
    QVector<ProbeAssertion> assertions;
};

/// Reads and validates one manifest document. `reason` names the first problem
/// so a malformed manifest says what is wrong with it rather than failing later
/// with no explanation.
std::optional<ProbeManifest> parseProbeManifest(const QJsonObject& document, QString* reason);

/// Reads one file. Returns nullopt and names the file in `reason` on any failure.
std::optional<ProbeManifest> loadProbeManifest(const QString& path, QString* reason);

}  // namespace mikai::probes
