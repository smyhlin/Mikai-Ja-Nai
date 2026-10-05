// Manifest validation. Every rejection names the first thing wrong with the
// document: a manifest is written by hand (or by an agent) from the header
// comments, so "invalid manifest" with no detail is the least useful output
// this file could produce.
//
// Strictness is deliberate. An unknown key is a refusal, not a shrug: a typo in
// `asertions` that silently drops every check reads as a green run that
// asserted nothing, which is the one failure mode a probe system cannot have.

#include "ProbeManifest.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMap>

#include <cmath>

namespace mikai::probes {
namespace {

// Writes the one reason a refusal has and returns false, so a rejection is a
// single `return fail(reason, ...)` at the point it is detected.
bool fail(QString* reason, const QString& text) {
    if (reason != nullptr) {
        *reason = text;
    }
    return false;
}

/// lowercased with `_`, `-` and spaces removed, so `double_click`, `DoubleClick`
/// and `double click` all name the same step kind.
QString folded(const QString& text) {
    QString key = text.trimmed().toLower();
    key.remove(QLatin1Char('_'));
    key.remove(QLatin1Char('-'));
    key.remove(QLatin1Char(' '));
    return key;
}

QString jsonTypeName(const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Null:
            return QStringLiteral("null");
        case QJsonValue::Bool:
            return QStringLiteral("a boolean");
        case QJsonValue::Double:
            return QStringLiteral("a number");
        case QJsonValue::String:
            return QStringLiteral("a string");
        case QJsonValue::Array:
            return QStringLiteral("an array");
        case QJsonValue::Object:
            return QStringLiteral("an object");
        case QJsonValue::Undefined:
            break;
    }
    return QStringLiteral("nothing");
}

/// Reads an optional string member. Returns false (with `reason` filled) only
/// when the member exists and is not a string.
bool readString(const QJsonObject& owner, const QString& key, const QString& where,
                QString* out, QString* reason) {
    if (!owner.contains(key)) {
        return true;
    }
    const QJsonValue value = owner.value(key);
    if (!value.isString()) {
        return fail(reason,
                    QStringLiteral("%1: '%2' must be a string, not %3")
                        .arg(where, key, jsonTypeName(value)));
    }
    *out = value.toString();
    return true;
}

/// The accepted predicate names, mirroring the runner's dispatch table. Names
/// the table so `wait_for` fails here rather than after the surface is built.
bool knownPredicate(const QString& name) {
    static const QStringList names{
        QStringLiteral("playing"),        QStringLiteral("mpvPlaying"),
        QStringLiteral("paused"),         QStringLiteral("mpvPaused"),
        QStringLiteral("hasVideo"),       QStringLiteral("contextReady"),
        QStringLiteral("durationKnown"),  QStringLiteral("positionKnown"),
        QStringLiteral("positionAdvanced"), QStringLiteral("positionIncreasing"),
        QStringLiteral("positionFrozen"), QStringLiteral("positionStopped"),
        QStringLiteral("mpvPause"),       QStringLiteral("mpvPauseCleared"),
        QStringLiteral("widgetExists"),   QStringLiteral("widgetVisible"),
        QStringLiteral("widgetHidden"),
    };
    const QString key = folded(name);
    for (const QString& candidate : names) {
        if (folded(candidate) == key) {
            return true;
        }
    }
    return false;
}

/// The `widget*` predicates read the target object; without one they would
/// silently watch the root and answer about nothing.
bool predicateNeedsTarget(const QString& name) {
    const QString key = folded(name);
    return key == QLatin1String("widgetexists") || key == QLatin1String("widgetvisible")
           || key == QLatin1String("widgethidden");
}

std::optional<ProbeStep::Kind> stepKind(const QString& literal) {
    const QString key = folded(literal);
    if (key == QLatin1String("click")) {
        return ProbeStep::Kind::Click;
    }
    if (key == QLatin1String("doubleclick")) {
        return ProbeStep::Kind::DoubleClick;
    }
    if (key == QLatin1String("mousemove")) {
        return ProbeStep::Kind::MouseMove;
    }
    if (key == QLatin1String("drag")) {
        return ProbeStep::Kind::Drag;
    }
    if (key == QLatin1String("keypress")) {
        return ProbeStep::Kind::KeyPress;
    }
    if (key == QLatin1String("setvalue")) {
        return ProbeStep::Kind::SetValue;
    }
    if (key == QLatin1String("typetext")) {
        return ProbeStep::Kind::TypeText;
    }
    if (key == QLatin1String("focus")) {
        return ProbeStep::Kind::Focus;
    }
    if (key == QLatin1String("waitforsignal")) {
        return ProbeStep::Kind::WaitForSignal;
    }
    if (key == QLatin1String("waitfor")) {
        return ProbeStep::Kind::WaitFor;
    }
    if (key == QLatin1String("sleepms")) {
        return ProbeStep::Kind::SleepMs;
    }
    return std::nullopt;
}

QString stepKindName(ProbeStep::Kind kind) {
    switch (kind) {
        case ProbeStep::Kind::Click:
            return QStringLiteral("click");
        case ProbeStep::Kind::DoubleClick:
            return QStringLiteral("double_click");
        case ProbeStep::Kind::MouseMove:
            return QStringLiteral("mouse_move");
        case ProbeStep::Kind::Drag:
            return QStringLiteral("drag");
        case ProbeStep::Kind::KeyPress:
            return QStringLiteral("key_press");
        case ProbeStep::Kind::SetValue:
            return QStringLiteral("set_value");
        case ProbeStep::Kind::TypeText:
            return QStringLiteral("type_text");
        case ProbeStep::Kind::Focus:
            return QStringLiteral("focus");
        case ProbeStep::Kind::WaitForSignal:
            return QStringLiteral("wait_for_signal");
        case ProbeStep::Kind::WaitFor:
            return QStringLiteral("wait_for");
        case ProbeStep::Kind::SleepMs:
            return QStringLiteral("sleep_ms");
    }
    return QStringLiteral("click");
}

/// The steps that act on a named widget; the runner looks the object up by
/// objectName and refuses the run without one.
bool stepNeedsTarget(ProbeStep::Kind kind) {
    switch (kind) {
        case ProbeStep::Kind::Click:
        case ProbeStep::Kind::DoubleClick:
        case ProbeStep::Kind::MouseMove:
        case ProbeStep::Kind::Drag:
        case ProbeStep::Kind::SetValue:
        case ProbeStep::Kind::TypeText:
        case ProbeStep::Kind::Focus:
            return true;
        case ProbeStep::Kind::KeyPress:
        case ProbeStep::Kind::WaitForSignal:
        case ProbeStep::Kind::WaitFor:
        case ProbeStep::Kind::SleepMs:
            return false;
    }
    return false;
}

/// The steps that act on a named object rather than the focus or the clock.
bool stepNeedsName(ProbeStep::Kind kind) {
    switch (kind) {
        case ProbeStep::Kind::SetValue:      ///< property name
        case ProbeStep::Kind::WaitForSignal:  ///< signal name
        case ProbeStep::Kind::WaitFor:        ///< predicate name
            return true;
        case ProbeStep::Kind::Click:
        case ProbeStep::Kind::DoubleClick:
        case ProbeStep::Kind::KeyPress:
        case ProbeStep::Kind::MouseMove:
        case ProbeStep::Kind::Drag:
        case ProbeStep::Kind::TypeText:
        case ProbeStep::Kind::Focus:
        case ProbeStep::Kind::SleepMs:
            return false;
    }
    return false;
}

QString stepNameRole(ProbeStep::Kind kind) {
    switch (kind) {
        case ProbeStep::Kind::SetValue:
            return QStringLiteral("property name");
        case ProbeStep::Kind::WaitForSignal:
            return QStringLiteral("signal name");
        case ProbeStep::Kind::WaitFor:
            return QStringLiteral("predicate name");
        case ProbeStep::Kind::Click:
        case ProbeStep::Kind::DoubleClick:
        case ProbeStep::Kind::KeyPress:
        case ProbeStep::Kind::MouseMove:
        case ProbeStep::Kind::Drag:
        case ProbeStep::Kind::TypeText:
        case ProbeStep::Kind::Focus:
        case ProbeStep::Kind::SleepMs:
            break;
    }
    return QStringLiteral("name");
}

std::optional<ProbeAssertion::Kind> assertionKind(const QString& literal) {
    const QString key = folded(literal);
    if (key == QLatin1String("signalemitted")) {
        return ProbeAssertion::Kind::SignalEmitted;
    }
    if (key == QLatin1String("qtproperty")) {
        return ProbeAssertion::Kind::QtProperty;
    }
    if (key == QLatin1String("mpvproperty")) {
        return ProbeAssertion::Kind::MpvProperty;
    }
    if (key == QLatin1String("widgetproperty")) {
        return ProbeAssertion::Kind::WidgetProperty;
    }
    if (key == QLatin1String("widgetexists")) {
        return ProbeAssertion::Kind::WidgetExists;
    }
    if (key == QLatin1String("signalnotemitted")) {
        return ProbeAssertion::Kind::SignalNotEmitted;
    }
    return std::nullopt;
}

QString assertionKindName(ProbeAssertion::Kind kind) {
    switch (kind) {
        case ProbeAssertion::Kind::SignalEmitted:
            return QStringLiteral("signal_emitted");
        case ProbeAssertion::Kind::QtProperty:
            return QStringLiteral("qt_property");
        case ProbeAssertion::Kind::MpvProperty:
            return QStringLiteral("mpv_property");
        case ProbeAssertion::Kind::WidgetProperty:
            return QStringLiteral("widget_property");
        case ProbeAssertion::Kind::WidgetExists:
            return QStringLiteral("widget_exists");
        case ProbeAssertion::Kind::SignalNotEmitted:
            return QStringLiteral("signal_not_emitted");
    }
    return QStringLiteral("qt_property");
}

/// The widget assertions name a widget; the mpv and signal assertions name a
/// property or a signal instead.
bool assertionNeedsTarget(ProbeAssertion::Kind kind) {
    switch (kind) {
        case ProbeAssertion::Kind::QtProperty:
        case ProbeAssertion::Kind::WidgetProperty:
        case ProbeAssertion::Kind::WidgetExists:
            return true;
        case ProbeAssertion::Kind::SignalEmitted:
        case ProbeAssertion::Kind::MpvProperty:
        case ProbeAssertion::Kind::SignalNotEmitted:
            return false;
    }
    return false;
}

QString assertionNameRole(ProbeAssertion::Kind kind) {
    switch (kind) {
        case ProbeAssertion::Kind::SignalEmitted:
        case ProbeAssertion::Kind::SignalNotEmitted:
            return QStringLiteral("signal name");
        case ProbeAssertion::Kind::MpvProperty:
            return QStringLiteral("mpv property name");
        case ProbeAssertion::Kind::QtProperty:
        case ProbeAssertion::Kind::WidgetProperty:
            return QStringLiteral("Qt property name");
        case ProbeAssertion::Kind::WidgetExists:
            break;
    }
    return QStringLiteral("property");
}

bool knownComparison(const QString& comparison) {
    static const QStringList names{
        QStringLiteral("equals"),     QStringLiteral("not_equals"), QStringLiteral("greater_than"),
        QStringLiteral("less_than"), QStringLiteral("approximately"), QStringLiteral("contains"),
    };
    const QString key = folded(comparison);
    for (const QString& candidate : names) {
        if (folded(candidate) == key) {
            return true;
        }
    }
    return false;
}

/// The runner compares by exact literal, so a manifest may spell a comparison
/// loosely but must not store it loosely.
QString canonicalComparison(const QString& comparison) {
    static const QMap<QString, QString> canonical{
        {QStringLiteral("equals"), QStringLiteral("equals")},
        {QStringLiteral("notequals"), QStringLiteral("not_equals")},
        {QStringLiteral("greaterthan"), QStringLiteral("greater_than")},
        {QStringLiteral("lessthan"), QStringLiteral("less_than")},
        {QStringLiteral("approximately"), QStringLiteral("approximately")},
        {QStringLiteral("contains"), QStringLiteral("contains")},
    };
    return canonical.value(folded(comparison));
}

std::optional<ProbeMpvKind> mpvKind(const QString& literal) {
    const QString key = folded(literal);
    if (key == QLatin1String("number")) {
        return ProbeMpvKind::Number;
    }
    if (key == QLatin1String("flag")) {
        return ProbeMpvKind::Flag;
    }
    if (key == QLatin1String("text")) {
        return ProbeMpvKind::Text;
    }
    return std::nullopt;
}

bool readModifiers(const QJsonObject& owner, const QString& where, Qt::KeyboardModifiers* out,
                   QString* reason) {
    *out = Qt::NoModifier;
    if (!owner.contains(QStringLiteral("modifiers"))) {
        return true;
    }
    const QJsonValue value = owner.value(QStringLiteral("modifiers"));
    if (!value.isArray()) {
        return fail(reason,
                    QStringLiteral("%1: 'modifiers' must be an array of strings, not %2")
                        .arg(where, jsonTypeName(value)));
    }
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    const QJsonArray array = value.toArray();
    for (const QJsonValue& entry : array) {
        if (!entry.isString()) {
            return fail(reason,
                        QStringLiteral("%1: 'modifiers' must hold only strings, found %2")
                            .arg(where, jsonTypeName(entry)));
        }
        const QString key = folded(entry.toString());
        if (key == QLatin1String("ctrl") || key == QLatin1String("control")) {
            modifiers |= Qt::ControlModifier;
        } else if (key == QLatin1String("alt")) {
            modifiers |= Qt::AltModifier;
        } else if (key == QLatin1String("shift")) {
            modifiers |= Qt::ShiftModifier;
        } else {
            return fail(reason, QStringLiteral("%1: unknown modifier '%2', expected ctrl, alt or "
                                               "shift")
                                   .arg(where, entry.toString()));
        }
    }
    *out = modifiers;
    return true;
}

bool readTimeoutMs(const QJsonObject& owner, const QString& where, int* out, QString* reason) {
    *out = 5000;
    if (!owner.contains(QStringLiteral("timeout_ms"))) {
        return true;
    }
    const QJsonValue value = owner.value(QStringLiteral("timeout_ms"));
    if (!value.isDouble()) {
        return fail(reason,
                    QStringLiteral("%1: 'timeout_ms' must be a number, not %2")
                        .arg(where, jsonTypeName(value)));
    }
    const int timeout = value.toInt();
    if (timeout <= 0) {
        return fail(reason, QStringLiteral("%1: 'timeout_ms' must be greater than 0, got %2")
                               .arg(where)
                               .arg(timeout));
    }
    *out = timeout;
    return true;
}

/// Reads an optional fraction of a widget's width. Absent leaves `*out`
/// untouched, so each step keeps the default its kind calls for.
bool readFraction(const QJsonObject& owner, const QString& key, const QString& where, double* out,
                  QString* reason) {
    if (!owner.contains(key)) {
        return true;
    }
    const QJsonValue value = owner.value(key);
    if (!value.isDouble()) {
        return fail(reason, QStringLiteral("%1: '%2' must be a number, not %3")
                                .arg(where, key, jsonTypeName(value)));
    }
    const double fraction = value.toDouble();
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        return fail(reason,
                    QStringLiteral("%1: '%2' must be a finite number between 0 and 1, got %3")
                        .arg(where, key)
                        .arg(fraction));
    }
    *out = fraction;
    return true;
}

bool parseStep(const QJsonValue& entry, int index, ProbeStep* out, QString* reason) {
    const QString where = QStringLiteral("steps[%1]").arg(index);
    if (!entry.isObject()) {
        return fail(reason,
                    QStringLiteral("%1: each step must be an object, found %2")
                        .arg(where, jsonTypeName(entry)));
    }
    const QJsonObject object = entry.toObject();
    static const QStringList known{
        QStringLiteral("kind"),     QStringLiteral("target"),      QStringLiteral("name"),
        QStringLiteral("value"),    QStringLiteral("key"),         QStringLiteral("modifiers"),
        QStringLiteral("position"), QStringLiteral("position_end"), QStringLiteral("timeout_ms"),
        QStringLiteral("note"),
    };
    for (const QString& key : object.keys()) {
        if (!known.contains(key)) {
            return fail(reason,
                        QStringLiteral("%1: unknown key '%2' (accepted: %3)")
                            .arg(where, key, known.join(QLatin1String(", "))));
        }
    }
    if (!object.contains(QStringLiteral("kind"))) {
        return fail(reason, QStringLiteral("%1: 'kind' is required").arg(where));
    }
    const QJsonValue kindValue = object.value(QStringLiteral("kind"));
    if (!kindValue.isString()) {
        return fail(reason,
                    QStringLiteral("%1: 'kind' must be a string, not %2")
                        .arg(where, jsonTypeName(kindValue)));
    }
    const std::optional<ProbeStep::Kind> kind = stepKind(kindValue.toString());
    if (!kind.has_value()) {
        return fail(reason,
                    QStringLiteral("%1: unknown step kind '%2' (accepted: click, double_click, "
                                   "mouse_move, drag, key_press, set_value, type_text, focus, "
                                   "wait_for_signal, wait_for, sleep_ms)")
                        .arg(where, kindValue.toString()));
    }

    ProbeStep step;
    step.kind = *kind;
    if (!readString(object, QStringLiteral("target"), where, &step.target, reason)
        || !readString(object, QStringLiteral("name"), where, &step.name, reason)
        || !readString(object, QStringLiteral("key"), where, &step.key, reason)
        || !readString(object, QStringLiteral("note"), where, &step.note, reason)
        || !readModifiers(object, where, &step.modifiers, reason)
        || !readFraction(object, QStringLiteral("position"), where, &step.position, reason)
        || !readFraction(object, QStringLiteral("position_end"), where, &step.positionEnd, reason)
        || !readTimeoutMs(object, where, &step.timeoutMs, reason)) {
        return false;
    }
    if (object.contains(QStringLiteral("value"))) {
        // Kept as the JSON value's own type: a number must stay a number, or a
        // numeric comparison answers against a string.
        step.value = object.value(QStringLiteral("value")).toVariant();
    }

    if (stepNeedsTarget(step.kind) && step.target.trimmed().isEmpty()) {
        return fail(reason,
                    QStringLiteral("%1: %2 needs a 'target' objectName, got none")
                        .arg(where, stepKindName(step.kind)));
    }
    if (stepNeedsName(step.kind) && step.name.trimmed().isEmpty()) {
        return fail(reason, QStringLiteral("%1: %2 needs a 'name' carrying its %3")
                               .arg(where, stepKindName(step.kind), stepNameRole(step.kind)));
    }
    if (step.kind == ProbeStep::Kind::KeyPress && step.key.trimmed().isEmpty()) {
        return fail(reason, QStringLiteral("%1: key_press needs a 'key' (e.g. \"Space\", \"Q\"), "
                                          "got none")
                               .arg(where));
    }
    if (step.kind == ProbeStep::Kind::TypeText && !object.contains(QStringLiteral("value"))) {
        return fail(reason, QStringLiteral("%1: type_text needs a 'value' to type, got none")
                               .arg(where));
    }
    if (step.kind == ProbeStep::Kind::SetValue && !object.contains(QStringLiteral("value"))) {
        return fail(reason,
                    QStringLiteral("%1: set_value needs a 'value' to write, got none").arg(where));
    }
    // A drag with no start would press the widget's centre and call that the
    // press point the manifest asked for, so the start is required outright.
    if (step.kind == ProbeStep::Kind::Drag && !object.contains(QStringLiteral("position"))) {
        return fail(reason,
                    QStringLiteral("%1: drag needs a 'position' (0..1) to press at, got none")
                        .arg(where));
    }
    if (step.kind == ProbeStep::Kind::WaitFor) {
        if (!knownPredicate(step.name)) {
            return fail(reason,
                        QStringLiteral("%1: unknown wait_for predicate '%2' (accepted: playing, "
                                       "paused, has_video, context_ready, duration_known, "
                                       "position_known, position_advanced, position_increasing, "
                                       "position_frozen, mpv_pause, mpv_pause_cleared, "
                                       "widget_exists, widget_visible, widget_hidden)")
                            .arg(where, step.name));
        }
        if (predicateNeedsTarget(step.name) && step.target.trimmed().isEmpty()) {
            return fail(reason,
                        QStringLiteral("%1: wait_for '%2' reads a widget, so it needs a 'target' "
                                       "objectName")
                            .arg(where, step.name));
        }
    }
    *out = step;
    return true;
}

bool parseAssertion(const QJsonValue& entry, int index, ProbeAssertion* out, QString* reason) {
    const QString where = QStringLiteral("assertions[%1]").arg(index);
    if (!entry.isObject()) {
        return fail(reason,
                    QStringLiteral("%1: each assertion must be an object, found %2")
                        .arg(where, jsonTypeName(entry)));
    }
    const QJsonObject object = entry.toObject();
    static const QStringList known{
        QStringLiteral("kind"),   QStringLiteral("target"),    QStringLiteral("property"),
        QStringLiteral("mpv_kind"), QStringLiteral("expected"), QStringLiteral("comparison"),
        QStringLiteral("tolerance"), QStringLiteral("note"),
    };
    for (const QString& key : object.keys()) {
        if (!known.contains(key)) {
            return fail(reason,
                        QStringLiteral("%1: unknown key '%2' (accepted: %3)")
                            .arg(where, key, known.join(QLatin1String(", "))));
        }
    }
    if (!object.contains(QStringLiteral("kind"))) {
        return fail(reason, QStringLiteral("%1: 'kind' is required").arg(where));
    }
    const QJsonValue kindValue = object.value(QStringLiteral("kind"));
    if (!kindValue.isString()) {
        return fail(reason,
                    QStringLiteral("%1: 'kind' must be a string, not %2")
                        .arg(where, jsonTypeName(kindValue)));
    }
    const std::optional<ProbeAssertion::Kind> kind = assertionKind(kindValue.toString());
    if (!kind.has_value()) {
        return fail(reason,
                    QStringLiteral("%1: unknown assertion kind '%2' (accepted: signal_emitted, "
                                   "qt_property, mpv_property, widget_property, widget_exists, "
                                   "signal_not_emitted)")
                        .arg(where, kindValue.toString()));
    }

    ProbeAssertion assertion;
    assertion.kind = *kind;
    if (!readString(object, QStringLiteral("target"), where, &assertion.target, reason)
        || !readString(object, QStringLiteral("property"), where, &assertion.property, reason)
        || !readString(object, QStringLiteral("note"), where, &assertion.note, reason)) {
        return false;
    }

    if (assertionNeedsTarget(assertion.kind) && assertion.target.trimmed().isEmpty()) {
        return fail(reason,
                    QStringLiteral("%1: %2 needs a 'target' objectName, got none")
                        .arg(where, assertionKindName(assertion.kind)));
    }
    if (assertion.kind != ProbeAssertion::Kind::WidgetExists
        && assertion.property.trimmed().isEmpty()) {
        return fail(reason,
                    QStringLiteral("%1: %2 needs a 'property' carrying its %3")
                        .arg(where, assertionKindName(assertion.kind),
                             assertionNameRole(assertion.kind)));
    }

    if (assertion.kind == ProbeAssertion::Kind::MpvProperty
        && object.contains(QStringLiteral("mpv_kind"))) {
        const QJsonValue mpvValue = object.value(QStringLiteral("mpv_kind"));
        if (!mpvValue.isString()) {
            return fail(reason,
                        QStringLiteral("%1: 'mpv_kind' must be a string, not %2")
                            .arg(where, jsonTypeName(mpvValue)));
        }
        const std::optional<ProbeMpvKind> kindValue2 = mpvKind(mpvValue.toString());
        if (!kindValue2.has_value()) {
            return fail(reason,
                        QStringLiteral("%1: unknown mpv_kind '%2' (accepted: number, flag, text)")
                            .arg(where, mpvValue.toString()));
        }
        assertion.mpvKind = *kindValue2;
    }

    if (object.contains(QStringLiteral("comparison"))) {
        const QJsonValue comparisonValue = object.value(QStringLiteral("comparison"));
        if (!comparisonValue.isString()) {
            return fail(reason,
                        QStringLiteral("%1: 'comparison' must be a string, not %2")
                            .arg(where, jsonTypeName(comparisonValue)));
        }
        if (!knownComparison(comparisonValue.toString())) {
            return fail(reason,
                        QStringLiteral("%1: unknown comparison '%2' (accepted: equals, "
                                       "not_equals, greater_than, less_than, approximately, "
                                       "contains)")
                            .arg(where, comparisonValue.toString()));
        }
        assertion.comparison = canonicalComparison(comparisonValue.toString());
    }

    // `expected` is only read by the three kinds that compare a property value.
    // The signal assertions decide on an emission count and widget_exists on
    // presence, so an `expected` there would be a value nothing looks at.
    const bool needsExpected = assertion.kind == ProbeAssertion::Kind::QtProperty
                               || assertion.kind == ProbeAssertion::Kind::WidgetProperty
                               || assertion.kind == ProbeAssertion::Kind::MpvProperty;
    if (needsExpected && !object.contains(QStringLiteral("expected"))) {
        return fail(reason,
                    QStringLiteral("%1: %2 needs an 'expected' value")
                        .arg(where, assertionKindName(assertion.kind)));
    }
    if (object.contains(QStringLiteral("expected"))) {
        assertion.expected = object.value(QStringLiteral("expected")).toVariant();
    }

    if (object.contains(QStringLiteral("tolerance"))) {
        const QJsonValue toleranceValue = object.value(QStringLiteral("tolerance"));
        if (!toleranceValue.isDouble()) {
            return fail(reason,
                        QStringLiteral("%1: 'tolerance' must be a number, not %2")
                            .arg(where, jsonTypeName(toleranceValue)));
        }
        assertion.tolerance = toleranceValue.toDouble();
    }
    if (folded(assertion.comparison) == QLatin1String("approximately")
        && assertion.tolerance <= 0.0) {
        return fail(reason,
                    QStringLiteral("%1: comparison 'approximately' needs a 'tolerance' greater "
                                   "than 0, got %2")
                        .arg(where)
                        .arg(assertion.tolerance));
    }

    *out = assertion;
    return true;
}

}  // namespace

std::optional<ProbeManifest> parseProbeManifest(const QJsonObject& document, QString* reason) {
    if (reason != nullptr) {
        reason->clear();
    }

    // A manifest with no name cannot be named in a failure line, so it is not a
    // manifest yet.
    if (!document.contains(QStringLiteral("name"))) {
        fail(reason, QStringLiteral("manifest: 'name' is required"));
        return std::nullopt;
    }
    const QJsonValue nameValue = document.value(QStringLiteral("name"));
    if (!nameValue.isString() || nameValue.toString().trimmed().isEmpty()) {
        fail(reason, QStringLiteral("manifest: 'name' must be a non-empty string, found %1")
                         .arg(nameValue.isString() ? QStringLiteral("an empty string")
                                                  : jsonTypeName(nameValue)));
        return std::nullopt;
    }

    static const QStringList known{
        QStringLiteral("name"),        QStringLiteral("description"),
        QStringLiteral("category"),    QStringLiteral("surface"),
        QStringLiteral("media"),       QStringLiteral("width"),
        QStringLiteral("height"),      QStringLiteral("steps"),
        QStringLiteral("assertions"),
    };
    for (const QString& key : document.keys()) {
        if (!known.contains(key)) {
            fail(reason,
                 QStringLiteral("manifest '%1': unknown top-level key '%2' (accepted: %3)")
                     .arg(nameValue.toString(), key, known.join(QLatin1String(", "))));
            return std::nullopt;
        }
    }

    ProbeManifest manifest;
    manifest.name = nameValue.toString().trimmed();
    // Free text by design: category and description label the run, they do not
    // steer it, so they are copied rather than checked against a list that
    // would only ever be extended.
    if (!readString(document, QStringLiteral("description"), QStringLiteral("manifest"),
                    &manifest.description, reason)
        || !readString(document, QStringLiteral("category"), QStringLiteral("manifest"),
                       &manifest.category, reason)) {
        return std::nullopt;
    }

    if (document.contains(QStringLiteral("surface"))) {
        const QJsonValue surfaceValue = document.value(QStringLiteral("surface"));
        if (!surfaceValue.isString()) {
            fail(reason, QStringLiteral("manifest '%1': 'surface' must be a string, not %2")
                             .arg(manifest.name, jsonTypeName(surfaceValue)));
            return std::nullopt;
        }
        const QString surface = folded(surfaceValue.toString());
        if (surface == QLatin1String("videosurface")) {
            manifest.surface = ProbeSurface::VideoSurface;
        } else if (surface == QLatin1String("playerdialog")) {
            manifest.surface = ProbeSurface::PlayerDialog;
        } else {
            fail(reason,
                 QStringLiteral("manifest '%1': unknown surface '%2' (accepted: video_surface, "
                                "player_dialog)")
                     .arg(manifest.name, surfaceValue.toString()));
            return std::nullopt;
        }
    }

    if (document.contains(QStringLiteral("width"))) {
        const QJsonValue widthValue = document.value(QStringLiteral("width"));
        if (!widthValue.isDouble()) {
            fail(reason, QStringLiteral("manifest '%1': 'width' must be a number, not %2")
                             .arg(manifest.name, jsonTypeName(widthValue)));
            return std::nullopt;
        }
        manifest.width = widthValue.toInt();
        if (manifest.width <= 0) {
            fail(reason, QStringLiteral("manifest '%1': 'width' must be greater than 0, got %2")
                             .arg(manifest.name)
                             .arg(widthValue.toInt()));
            return std::nullopt;
        }
    }
    if (document.contains(QStringLiteral("height"))) {
        const QJsonValue heightValue = document.value(QStringLiteral("height"));
        if (!heightValue.isDouble()) {
            fail(reason, QStringLiteral("manifest '%1': 'height' must be a number, not %2")
                             .arg(manifest.name, jsonTypeName(heightValue)));
            return std::nullopt;
        }
        manifest.height = heightValue.toInt();
        if (manifest.height <= 0) {
            fail(reason, QStringLiteral("manifest '%1': 'height' must be greater than 0, got %2")
                             .arg(manifest.name)
                             .arg(heightValue.toInt()));
            return std::nullopt;
        }
    }

    if (document.contains(QStringLiteral("media"))) {
        if (!readString(document, QStringLiteral("media"), QStringLiteral("manifest"),
                        &manifest.media, reason)) {
            return std::nullopt;
        }
        manifest.media = manifest.media.trimmed();
    }
    // PlayerDialog starts the stream it is handed, so media is what it opens;
    // VideoSurface has no start path, so media there would be a claim the run
    // cannot keep. Refusing both directions keeps the field honest.
    if (manifest.surface == ProbeSurface::PlayerDialog && manifest.media.isEmpty()) {
        fail(reason, QStringLiteral("manifest '%1': surface 'player_dialog' needs 'media' to "
                                    "open, got none")
                         .arg(manifest.name));
        return std::nullopt;
    }
    if (manifest.surface == ProbeSurface::VideoSurface && !manifest.media.isEmpty()) {
        fail(reason, QStringLiteral("manifest '%1': surface 'video_surface' opens no media, so "
                                    "'media' ('%2') is refused there")
                         .arg(manifest.name, manifest.media));
        return std::nullopt;
    }

    if (document.contains(QStringLiteral("steps"))) {
        const QJsonValue stepsValue = document.value(QStringLiteral("steps"));
        if (!stepsValue.isArray()) {
            fail(reason, QStringLiteral("manifest '%1': 'steps' must be an array, not %2")
                             .arg(manifest.name, jsonTypeName(stepsValue)));
            return std::nullopt;
        }
        const QJsonArray steps = stepsValue.toArray();
        manifest.steps.reserve(steps.size());
        for (int index = 0; index < steps.size(); ++index) {
            ProbeStep step;
            if (!parseStep(steps.at(index), index, &step, reason)) {
                return std::nullopt;
            }
            manifest.steps.append(std::move(step));
        }
    }

    if (document.contains(QStringLiteral("assertions"))) {
        const QJsonValue assertionsValue = document.value(QStringLiteral("assertions"));
        if (!assertionsValue.isArray()) {
            fail(reason, QStringLiteral("manifest '%1': 'assertions' must be an array, not %2")
                             .arg(manifest.name, jsonTypeName(assertionsValue)));
            return std::nullopt;
        }
        const QJsonArray assertions = assertionsValue.toArray();
        manifest.assertions.reserve(assertions.size());
        for (int index = 0; index < assertions.size(); ++index) {
            ProbeAssertion assertion;
            if (!parseAssertion(assertions.at(index), index, &assertion, reason)) {
                return std::nullopt;
            }
            manifest.assertions.append(std::move(assertion));
        }
    }

    if (manifest.steps.isEmpty() && manifest.assertions.isEmpty()) {
        fail(reason,
             QStringLiteral("manifest '%1': at least one step or one assertion is required, got "
                            "neither")
                 .arg(manifest.name));
        return std::nullopt;
    }

    return manifest;
}

std::optional<ProbeManifest> loadProbeManifest(const QString& path, QString* reason) {
    if (reason != nullptr) {
        reason->clear();
    }
    if (path.trimmed().isEmpty()) {
        fail(reason, QStringLiteral("probe manifest: no path given"));
        return std::nullopt;
    }

    QFile file(path);
    if (!file.exists()) {
        fail(reason, QStringLiteral("probe manifest '%1': no such file").arg(path));
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        fail(reason, QStringLiteral("probe manifest '%1': cannot be opened (%2)")
                         .arg(path, file.errorString()));
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        fail(reason, QStringLiteral("probe manifest '%1': cannot be read (%2)")
                         .arg(path, file.errorString()));
        return std::nullopt;
    }
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        fail(reason, QStringLiteral("probe manifest '%1': invalid JSON at byte %2 (%3)")
                         .arg(path)
                         .arg(parseError.offset)
                         .arg(parseError.errorString()));
        return std::nullopt;
    }
    if (!document.isObject()) {
        // A bare array or scalar parses cleanly and then says nothing about
        // what the probe should do, so it is named here rather than reaching
        // the surface as an empty manifest.
        const QString found = document.isArray() ? QStringLiteral("an array")
                                                 : QStringLiteral("a bare JSON value");
        fail(reason,
             QStringLiteral("probe manifest '%1': the top level must be a JSON object, found %2")
                 .arg(path, found));
        return std::nullopt;
    }

    const std::optional<ProbeManifest> manifest =
        parseProbeManifest(document.object(), reason);
    if (!manifest.has_value()) {
        // Keep the file in the message: a manifest named by path is diagnosable,
        // one named only by its own `name` field is a guess when several exist.
        if (reason != nullptr && !reason->isEmpty()) {
            *reason = QStringLiteral("probe manifest '%1': %2").arg(path, *reason);
        }
        return std::nullopt;
    }
    return manifest;
}

}  // namespace mikai::probes
