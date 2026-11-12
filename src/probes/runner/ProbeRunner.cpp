// The manifest runner's mechanics. Every Qt control it acts on is reached
// through an objectName or the controller's actual menu model; every wait is
// an event loop with a deadline; every assertion reads observable state (a
// QObject property, an mpv property, a signal count, a child object) and is
// printed with what it actually saw against what the manifest expected.
//
// The signal vocabulary is resolved dynamically from the live objects, so the
// names a manifest may use are exactly the signals the window, controller and
// session declare. Resolving by name rather than by a compiled signature keeps
// a reworded or renamed signal a manifest failure that names itself instead of
// a runner that silently stops observing.

#include "ProbeRunner.hpp"

#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QHash>
#include <QKeySequence>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <QWindow>
#include <QQuickItem>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "I18n.hpp"
#include "backend/MikaiClient.hpp"
#include "dialogs/SettingsDialog.hpp"
#include "pages/MainWindow.hpp"
#include "workers/Runner.hpp"

#include "PlayerController.hpp"
#include "PlayerWindow.hpp"
#include "VideoSession.hpp"
#include "state/NativePlaybackAssembly.hpp"
#include "VideoSurface.hpp"

namespace mikai::probes {
namespace {

using mikai::app::MainWindow;
using mikai::app::Runner;
using mikai::app::SettingsDialog;
using mikai::backend::MikaiClient;
using mplayer::ProbePropertyKind;
using mplayer::ProbePropertyValue;
using mplayer::VideoSession;
using mplayer::VideoSurface;
using mplayer::quick::PlayerController;
using mplayer::quick::PlayerWindow;
using mplayer::shell::QualityEntry;

constexpr int kContextTimeoutMs = 20000;
constexpr int kTeardownSettleMs = 6000;
constexpr int kDefaultTimeoutMs = 5000;
constexpr double kFrozenEpsilon = 0.05;
constexpr int kFrozenHoldMs = 400;

/// The host a seeded series answers to. Every row points at the manifest's
/// own media, so selecting one exercises the controller's production episode
/// path rather than inventing a stream.
class ProbePlayerHost : public mplayer::shell::PlayerHost {
public:
    PlayerController* controller = nullptr;
    /// The rows and qualities the runner seeded, so an episode request
    /// re-applies known state through the production controller.
    QVector<QVariantMap> rows;
    QList<QualityEntry> qualities;

    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int index) override {
        if (controller == nullptr || index < 0 || index >= rows.size()) return;
        const QString label = rows.at(index).value(QStringLiteral("label")).toString();
        const QString embed = rows.at(index).value(QStringLiteral("embed")).toString();
        controller->applyEpisode(label, embed, index, qualities, {});
    }
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

struct Surface {
    QObject* root = nullptr;   ///< the shown window steps are found under
    VideoSurface* video = nullptr;  ///< optional render view for the bare surface mode
    VideoSession* session = nullptr;  ///< engine state and commands, for every player mode
    PlayerController* controller = nullptr;
    bool settingsDialog = false;
    /// The settings dialog borrows these, so the Surface owns them and is reset
    /// only after the dialog is destroyed; every other surface leaves them null.
    std::unique_ptr<MikaiClient> client;
    std::unique_ptr<Runner> runner;
    /// The controller borrows this host; member order destroys the controller first.
    std::unique_ptr<ProbePlayerHost> extraHost;
    std::unique_ptr<PlayerController> player;
};

void report(bool passed, const QString& line) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", line.toUtf8().constData());
    std::fflush(stdout);
}

QString valueText(const QVariant& value) {
    if (!value.isValid()) {
        return QStringLiteral("<absent>");
    }
    switch (value.metaType().id()) {
        case QMetaType::Bool:
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
            return QString::number(value.toDouble(), 'g', 12);
        default:
            break;
    }
    if (value.metaType().id() == QMetaType::QStringList) {
        return value.toStringList().join(QStringLiteral(", "));
    }
    return value.toString();
}

/// True only for the shapes a numeric comparison may read: a number, or a bool
/// read as 0/1. A string that merely contains digits is NOT a number here, so
/// "greater_than" cannot be answered by a display label.
bool asNumber(const QVariant& value, double* out) {
    switch (value.metaType().id()) {
        case QMetaType::Bool:
            *out = value.toBool() ? 1.0 : 0.0;
            return true;
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
            *out = value.toDouble();
            return true;
        default:
            return false;
    }
}

struct Verdict {
    bool ok = false;
    QString reason;
};

Verdict compareValues(const QString& comparison, const QVariant& actual, const QVariant& expected,
                      double tolerance) {
    const QString op = comparison.isEmpty() ? QStringLiteral("equals") : comparison;
    if (op == QLatin1String("equals") || op == QLatin1String("not_equals")) {
        bool equal = false;
        double left = 0.0;
        double right = 0.0;
        if (asNumber(actual, &left) && asNumber(expected, &right)) {
            equal = (left == right);
        } else {
            equal = (actual.toString() == expected.toString());
        }
        if (op == QLatin1String("not_equals")) {
            equal = !equal;
        }
        return {equal, {}};
    }
    if (op == QLatin1String("approximately")) {
        double left = 0.0;
        double right = 0.0;
        if (!asNumber(actual, &left) || !asNumber(expected, &right)) {
            return {false, QStringLiteral("approximately needs numeric values")};
        }
        return {std::fabs(left - right) <= tolerance, {}};
    }
    if (op == QLatin1String("greater_than") || op == QLatin1String("less_than")) {
        double left = 0.0;
        double right = 0.0;
        if (!asNumber(actual, &left) || !asNumber(expected, &right)) {
            return {false, QStringLiteral("%1 needs numeric values").arg(op)};
        }
        return {op == QLatin1String("greater_than") ? left > right : left < right, {}};
    }
    if (op == QLatin1String("contains")) {
        const QString needle = expected.toString();
        if (actual.toString().contains(needle)) {
            return {true, {}};
        }
        if (actual.canConvert<QVariantList>()) {
            const QVariantList list = actual.toList();
            for (const QVariant& item : list) {
                if (item.toString() == needle) {
                    return {true, {}};
                }
            }
        }
        return {false, {}};
    }
    return {false, QStringLiteral("unknown comparison '%1'").arg(op)};
}

QObject* findByName(QObject* root, const QString& name) {
    if (root == nullptr || name.isEmpty()) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    return root->findChild<QObject*>(name);
}
QObject* findTarget(const Surface& surface, const QString& name) {
    if (surface.controller != nullptr && surface.controller->objectName() == name) {
        return surface.controller;
    }
    if (surface.controller != nullptr && name.startsWith(QLatin1String("menu:"))) {
        return surface.controller->menuModel(name.mid(5));
    }
    QObject* root = surface.root;
    if (root == nullptr || name.isEmpty()) return nullptr;
    if (root->objectName() == name) return root;
    return root->findChild<QObject*>(name);
}

QWidget* widgetTarget(const Surface& surface, const QString& name, QString* reason) {
    if (name.isEmpty()) {
        *reason = QStringLiteral("the step names no widget");
        return nullptr;
    }
    QObject* object = findTarget(surface, name);
    if (object == nullptr) {
        *reason = QStringLiteral("no widget named '%1'").arg(name);
        return nullptr;
    }
    auto* widget = qobject_cast<QWidget*>(object);
    if (widget == nullptr) {
        *reason = QStringLiteral("'%1' is an object, but not a widget that can be driven").arg(name);
        return nullptr;
    }
    return widget;
}
bool objectVisible(QObject* object) {
    if (object == nullptr) return false;
    if (auto* widget = qobject_cast<QWidget*>(object)) return widget->isVisible();
    if (auto* window = qobject_cast<QWindow*>(object)) return window->isVisible();
    const int visibleIndex = object->metaObject()->indexOfProperty("visible");
    return visibleIndex >= 0 && object->property("visible").toBool();
}

bool visibleMenuMatches(const Surface& surface, const QString& menuId) {
    if (surface.root == nullptr || menuId.isEmpty()) return false;
    const QList<QObject*> objects = surface.root->findChildren<QObject*>();
    for (QObject* object : objects) {
        if (object == nullptr) continue;
        const int menuIdIndex = object->metaObject()->indexOfProperty("menuId");
        if (menuIdIndex < 0 || object->property("menuId").toString() != menuId) continue;
        if (objectVisible(object)) return true;
    }
    return false;
}

QPoint windowPoint(const QQuickItem* item, const QPointF& localPoint) {
    return item->mapToScene(localPoint).toPoint();
}

/// Signal names are resolved on the actual window, controller and playback
/// session that implement the selected surface.
QVector<QObject*> signalCandidates(const Surface& surface) {
    QVector<QObject*> candidates;
    if (surface.root != nullptr) {
        candidates.append(surface.root);
    }
    if (surface.controller != nullptr) {
        candidates.append(surface.controller);
    }
    if (surface.session != nullptr) {
        candidates.append(surface.session);
    }
    return candidates;
}

bool resolveSignal(const QVector<QObject*>& candidates, const QString& name, QObject** emitter,
                   QMetaMethod* method) {
    if (name.isEmpty()) {
        return false;
    }
    for (QObject* candidate : candidates) {
        if (candidate == nullptr) {
            continue;
        }
        const QMetaObject* meta = candidate->metaObject();
        for (int index = 0; index < meta->methodCount(); ++index) {
            const QMetaMethod candidateMethod = meta->method(index);
            if (candidateMethod.methodType() != QMetaMethod::Signal) {
                continue;
            }
            if (QString::fromLatin1(candidateMethod.name()) == name ||
                QString::fromLatin1(candidateMethod.methodSignature()) == name) {
                *emitter = candidate;
                *method = candidateMethod;
                return true;
            }
        }
    }
    return false;
}

QString stepKindName(ProbeStep::Kind kind) {
    switch (kind) {
        case ProbeStep::Kind::Click: return QStringLiteral("click");
        case ProbeStep::Kind::DoubleClick: return QStringLiteral("double_click");
        case ProbeStep::Kind::MouseMove: return QStringLiteral("mouse_move");
        case ProbeStep::Kind::Drag: return QStringLiteral("drag");
        case ProbeStep::Kind::KeyPress: return QStringLiteral("key_press");
        case ProbeStep::Kind::SetValue: return QStringLiteral("set_value");
        case ProbeStep::Kind::TypeText: return QStringLiteral("type_text");
        case ProbeStep::Kind::Focus: return QStringLiteral("focus");
        case ProbeStep::Kind::WaitForSignal: return QStringLiteral("wait_for_signal");
        case ProbeStep::Kind::WaitFor: return QStringLiteral("wait_for");
        case ProbeStep::Kind::SleepMs: return QStringLiteral("sleep_ms");
    }
    return QStringLiteral("step");
}

Qt::Key aliasKey(const QString& text) {
    static const QHash<QString, Qt::Key> aliases = {
        {QStringLiteral("space"), Qt::Key_Space},
        {QStringLiteral("spacebar"), Qt::Key_Space},
        {QStringLiteral("escape"), Qt::Key_Escape},
        {QStringLiteral("esc"), Qt::Key_Escape},
        {QStringLiteral("enter"), Qt::Key_Return},
        {QStringLiteral("return"), Qt::Key_Return},
        {QStringLiteral("tab"), Qt::Key_Tab},
        {QStringLiteral("backspace"), Qt::Key_Backspace},
        {QStringLiteral("delete"), Qt::Key_Delete},
        {QStringLiteral("del"), Qt::Key_Delete},
        {QStringLiteral("insert"), Qt::Key_Insert},
        {QStringLiteral("left"), Qt::Key_Left},
        {QStringLiteral("right"), Qt::Key_Right},
        {QStringLiteral("up"), Qt::Key_Up},
        {QStringLiteral("down"), Qt::Key_Down},
        {QStringLiteral("home"), Qt::Key_Home},
        {QStringLiteral("end"), Qt::Key_End},
        {QStringLiteral("pageup"), Qt::Key_PageUp},
        {QStringLiteral("pagedown"), Qt::Key_PageDown},
    };
    const auto it = aliases.constFind(text.toLower());
    return it == aliases.constEnd() ? Qt::Key_unknown : it.value();
}

/// The manifest spells a key the way a shortcut is written ("Space", "Q",
/// "Ctrl+S", "Escape"). QKeySequence decodes most of them; the spelled-out key
/// names it does not are covered by the alias table, and both paths contribute
/// their modifiers.
Qt::Key keyFromText(const QString& text, Qt::KeyboardModifiers* modifiers) {
    const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    if (!sequence.isEmpty()) {
        const QKeyCombination combination = sequence[0];
        const Qt::Key key = combination.key();
        if (key != Qt::Key_unknown && static_cast<int>(key) != 0) {
            *modifiers |= combination.keyboardModifiers();
            return key;
        }
    }
    Qt::KeyboardModifiers parsed = Qt::NoModifier;
    Qt::Key key = Qt::Key_unknown;
    const QStringList parts = text.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString token = part.trimmed();
        Qt::KeyboardModifiers modifier = Qt::NoModifier;
        if (token.compare(QLatin1String("Ctrl"), Qt::CaseInsensitive) == 0 ||
            token.compare(QLatin1String("Control"), Qt::CaseInsensitive) == 0) {
            modifier = Qt::ControlModifier;
        } else if (token.compare(QLatin1String("Shift"), Qt::CaseInsensitive) == 0) {
            modifier = Qt::ShiftModifier;
        } else if (token.compare(QLatin1String("Alt"), Qt::CaseInsensitive) == 0) {
            modifier = Qt::AltModifier;
        } else if (token.compare(QLatin1String("Meta"), Qt::CaseInsensitive) == 0 ||
                   token.compare(QLatin1String("Win"), Qt::CaseInsensitive) == 0) {
            modifier = Qt::MetaModifier;
        }
        if (modifier != Qt::NoModifier) {
            parsed |= modifier;
            continue;
        }
        key = aliasKey(token);
    }
    if (key == Qt::Key_unknown) {
        return Qt::Key_unknown;
    }
    *modifiers |= parsed;
    return key;
}

/// Re-evaluated by wait_for. Every predicate reads observable state only; the
/// ones that need memory (moving/frozen) keep it in the closure.
std::function<bool()> makePredicate(const ProbeStep& step, const Surface& surface) {
    QString key = step.name.trimmed().toLower();
    key.remove(QLatin1Char('_'));
    const QString target = step.target;

    if (key == QLatin1String("menuopen")) {
        return [&surface, target] {
            return surface.controller != nullptr && surface.controller->menuStackVisible() &&
                   visibleMenuMatches(surface, target);
        };
    }
    if (key == QLatin1String("playing") || key == QLatin1String("mpvplaying")) {
        return [&surface] { return surface.session != nullptr && surface.session->isPlaying(); };
    }
    if (key == QLatin1String("paused") || key == QLatin1String("mpvpaused")) {
        return [&surface] { return surface.session != nullptr && !surface.session->isPlaying(); };
    }
    if (key == QLatin1String("hasvideo")) {
        return [&surface] { return surface.session != nullptr && surface.session->hasVideo(); };
    }
    if (key == QLatin1String("contextready")) {
        return [&surface] { return surface.session != nullptr && surface.session->isContextReady(); };
    }
    if (key == QLatin1String("durationknown") || key == QLatin1String("positionknown")) {
        return [&surface] { return surface.session != nullptr && surface.session->duration() > 0.0; };
    }
    if (key == QLatin1String("positionadvanced")) {
        return [&surface] { return surface.session != nullptr && surface.session->position() > 0.0; };
    }
    if (key == QLatin1String("positionincreasing")) {
        auto baseline = std::make_shared<double>(-1.0);
        return [&surface, baseline] {
            if (surface.session == nullptr) {
                return false;
            }
            const double position = surface.session->position();
            if (*baseline < 0.0) {
                *baseline = position;
                return false;
            }
            return position > *baseline + kFrozenEpsilon;
        };
    }
    if (key == QLatin1String("positionfrozen") || key == QLatin1String("positionstopped")) {
        struct FrozenProbe {
            double last = 0.0;
            bool started = false;
            QElapsedTimer stable;
        };
        auto probe = std::make_shared<FrozenProbe>();
        return [&surface, probe] {
            if (surface.session == nullptr) {
                return false;
            }
            const double position = surface.session->position();
            if (!probe->started) {
                probe->started = true;
                probe->last = position;
                probe->stable.start();
                return false;
            }
            if (std::fabs(position - probe->last) > kFrozenEpsilon) {
                probe->last = position;
                probe->stable.restart();
                return false;
            }
            return probe->stable.elapsed() >= kFrozenHoldMs;
        };
    }
    if (key == QLatin1String("mpvpause") || key == QLatin1String("mpvpausecleared")) {
        const bool wantPaused = (key == QLatin1String("mpvpause"));
        return [&surface, wantPaused] {
            if (surface.session == nullptr) {
                return false;
            }
            const auto value = surface.session->probeMpvProperty(QStringLiteral("pause"),
                                                                 ProbePropertyKind::Flag);
            return value.has_value() && value->flag == wantPaused;
        };
    }
    if (key == QLatin1String("widgetexists")) {
        return [&surface, target] { return findTarget(surface, target) != nullptr; };
    }
    if (key == QLatin1String("widgetvisible") || key == QLatin1String("objectvisible")) {
        return [&surface, target] { return objectVisible(findTarget(surface, target)); };
    }
    if (key == QLatin1String("widgethidden") || key == QLatin1String("objecthidden")) {
        return [&surface, target] { return !objectVisible(findTarget(surface, target)); };
    }
    return {};
}

bool runStep(const ProbeStep& step, const Surface& surface, QString* reason) {
    switch (step.kind) {
        case ProbeStep::Kind::Click: {
            QObject* object = findTarget(surface, step.target);
            if (auto* widget = qobject_cast<QWidget*>(object)) {
                QCoreApplication::processEvents();
                QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, widget->rect().center());
                return true;
            }
            auto* item = qobject_cast<QQuickItem*>(object);
            auto* window = item != nullptr ? item->window() : nullptr;
            if (item == nullptr || window == nullptr) {
                *reason = QStringLiteral("no clickable widget or Quick item named '%1'")
                              .arg(step.target);
                return false;
            }
            QCoreApplication::processEvents();
            const QPoint point = windowPoint(item, QPointF(item->width() / 2.0, item->height() / 2.0));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
            return true;
        }
        case ProbeStep::Kind::DoubleClick: {
            QObject* object = findTarget(surface, step.target);
            if (auto* widget = qobject_cast<QWidget*>(object)) {
                QCoreApplication::processEvents();
                QTest::mouseDClick(widget, Qt::LeftButton, Qt::NoModifier, widget->rect().center());
                return true;
            }
            auto* item = qobject_cast<QQuickItem*>(object);
            auto* window = item != nullptr ? item->window() : nullptr;
            if (item == nullptr || window == nullptr) {
                *reason = QStringLiteral("no double-clickable widget or Quick item named '%1'")
                              .arg(step.target);
                return false;
            }
            QCoreApplication::processEvents();
            const QPoint point = windowPoint(item, QPointF(item->width() / 2.0, item->height() / 2.0));
            QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, point);
            return true;
        }
        case ProbeStep::Kind::MouseMove: {
            QObject* object = findTarget(surface, step.target);
            QCoreApplication::processEvents();
            const double fraction = step.position >= 0.0 ? step.position : 0.5;
            if (auto* widget = qobject_cast<QWidget*>(object)) {
                const QPoint point(qRound(widget->width() * fraction), widget->height() / 2);
                QTest::mouseMove(widget, point);
                return true;
            }
            auto* item = qobject_cast<QQuickItem*>(object);
            auto* window = item != nullptr ? item->window() : nullptr;
            if (item == nullptr || window == nullptr) {
                *reason = QStringLiteral("no hoverable widget or Quick item named '%1'")
                              .arg(step.target);
                return false;
            }
            const QPoint point =
                windowPoint(item, QPointF(item->width() * fraction, item->height() / 2.0));
            QTest::mouseMove(window, point);
            return true;
        }
        case ProbeStep::Kind::Drag: {
            QObject* object = findTarget(surface, step.target);
            QCoreApplication::processEvents();
            if (auto* widget = qobject_cast<QWidget*>(object)) {
                // Press, move and release through the control's own handlers.
                const QPoint start(qRound(widget->width() * step.position),
                                   widget->height() / 2);
                const QPoint end(qRound(widget->width() * step.positionEnd),
                                 widget->height() / 2);
                QTest::mousePress(widget, Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(widget, end);
                QTest::mouseRelease(widget, Qt::LeftButton, Qt::NoModifier, end);
                return true;
            }
            auto* item = qobject_cast<QQuickItem*>(object);
            auto* window = item != nullptr ? item->window() : nullptr;
            if (item == nullptr || window == nullptr) {
                *reason = QStringLiteral("no draggable widget or Quick item named '%1'")
                              .arg(step.target);
                return false;
            }
            const QPoint start = windowPoint(
                item, QPointF(item->width() * step.position, item->height() / 2.0));
            const QPoint end = windowPoint(
                item, QPointF(item->width() * step.positionEnd, item->height() / 2.0));
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, end);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
            return true;
        }
        case ProbeStep::Kind::KeyPress: {
            QWidget* target = nullptr;
            if (auto* widgetRoot = qobject_cast<QWidget*>(surface.root)) {
                target = widgetRoot->focusWidget();
                if (target == nullptr) target = widgetRoot;
            }
            if (target == nullptr && surface.root == nullptr) {
                *reason = QStringLiteral("there is no window to deliver a key to");
                return false;
            }
            Qt::KeyboardModifiers modifiers = step.modifiers;
            const Qt::Key key = keyFromText(step.key, &modifiers);
            if (key == Qt::Key_unknown) {
                *reason = QStringLiteral("no key named '%1'").arg(step.key);
                return false;
            }
            if (target != nullptr) {
                QTest::keyClick(target, key, modifiers);
            } else if (auto* window = qobject_cast<QWindow*>(surface.root)) {
                QTest::keyClick(window, key, modifiers);
            } else {
                *reason = QStringLiteral("the surface root is not a key event target");
                return false;
            }
            return true;
        }
        case ProbeStep::Kind::SetValue: {
            QObject* object = findTarget(surface, step.target);
            if (object == nullptr) {
                *reason = QStringLiteral("no widget named '%1'").arg(step.target);
                return false;
            }
            const QByteArray name = step.name.toUtf8();
            if (object->metaObject()->indexOfProperty(name.constData()) < 0) {
                *reason = QStringLiteral("'%1' has no Q_PROPERTY named '%2'")
                              .arg(step.target, step.name);
                return false;
            }
            // setProperty runs the declared setter, so the write is the same one
            // a user action performs and emits exactly what it emits.
            if (!object->setProperty(name.constData(), step.value)) {
                *reason = QStringLiteral("the setter for '%1.%2' refused the value")
                              .arg(step.target, step.name);
                return false;
            }
            return true;
        }
        case ProbeStep::Kind::TypeText: {
            QWidget* widget = widgetTarget(surface, step.target, reason);
            if (widget == nullptr) {
                return false;
            }
            widget->setFocus(Qt::OtherFocusReason);
            QCoreApplication::processEvents();
            QTest::keyClicks(widget, step.value.toString());
            return true;
        }
        case ProbeStep::Kind::Focus: {
            QObject* object = findTarget(surface, step.target);
            if (auto* widget = qobject_cast<QWidget*>(object)) {
                widget->setFocus(Qt::OtherFocusReason);
            } else if (auto* item = qobject_cast<QQuickItem*>(object)) {
                item->forceActiveFocus(Qt::OtherFocusReason);
            } else if (auto* window = qobject_cast<QWindow*>(object)) {
                window->requestActivate();
            } else {
                *reason = QStringLiteral("no focusable widget, Quick item or window named '%1'")
                              .arg(step.target);
                return false;
            }
            QCoreApplication::processEvents();
            return true;
        }
        case ProbeStep::Kind::WaitForSignal: {
            QObject* emitter = nullptr;
            QMetaMethod method;
            if (!resolveSignal(signalCandidates(surface), step.name, &emitter, &method)) {
                *reason = QStringLiteral("no signal named '%1' on the window, controller or session")
                              .arg(step.name);
                return false;
            }
            QSignalSpy spy(emitter, method);
            if (!spy.isValid()) {
                *reason = QStringLiteral("the signal '%1' could not be observed").arg(step.name);
                return false;
            }
            if (!spy.isEmpty()) {
                return true;
            }
            const int timeout = step.timeoutMs > 0 ? step.timeoutMs : kDefaultTimeoutMs;
            if (!spy.wait(timeout)) {
                *reason = QStringLiteral("signal '%1' did not arrive within %2ms")
                              .arg(step.name)
                              .arg(timeout);
                return false;
            }
            return true;
        }
        case ProbeStep::Kind::WaitFor: {
            const std::function<bool()> predicate = makePredicate(step, surface);
            if (!predicate) {
                *reason = QStringLiteral("no predicate named '%1'").arg(step.name);
                return false;
            }
            const int timeout = step.timeoutMs > 0 ? step.timeoutMs : kDefaultTimeoutMs;
            if (!QTest::qWaitFor(predicate, timeout)) {
                *reason = QStringLiteral("predicate '%1' did not hold within %2ms")
                              .arg(step.name)
                              .arg(timeout);
                return false;
            }
            return true;
        }
        case ProbeStep::Kind::SleepMs:
            QTest::qWait(step.timeoutMs > 0 ? step.timeoutMs : 0);
            return true;
    }
    *reason = QStringLiteral("unknown step kind");
    return false;
}

bool buildSurface(const ProbeManifest& manifest, Surface* surface, QString* reason) {
    const QString title = QStringLiteral("Mikai probe: %1")
                              .arg(manifest.name.isEmpty() ? QStringLiteral("(unnamed)")
                                                           : manifest.name);
    if (manifest.surface == ProbeSurface::MainWindow) {
        if (!manifest.media.isEmpty() || !manifest.episodes.isEmpty()) {
            *reason = QStringLiteral("the main_window surface opens no stream, and manifest "
                                     "'%1' names media or episodes")
                          .arg(manifest.name);
            return false;
        }
        // Exactly the way main() builds it: a client, a window, show. The
        // window is the discovery root, so home-page widgets are reached by
        // objectName.
        surface->client = std::make_unique<MikaiClient>(QStringLiteral("probe-key"));
        auto* window = new MainWindow(*surface->client);
        surface->root = window;
        window->setWindowTitle(title);
        window->resize(manifest.width, manifest.height);
        window->show();
        return true;
    }
    if (manifest.surface == ProbeSurface::SettingsDialog) {
        if (!manifest.media.isEmpty()) {
            *reason = QStringLiteral("the settings_dialog surface opens no stream, and manifest "
                                     "'%1' names media '%2'")
                          .arg(manifest.name, manifest.media);
            return false;
        }
        // The real dialog over the same fixture the HLS probe's recorder builds:
        // a MikaiClient and a Runner it borrows. The dialog itself is the
        // discovery root, so every step and assertion is found under it.
        surface->client = std::make_unique<MikaiClient>(QStringLiteral("probe-key"));
        surface->runner = std::make_unique<Runner>();
        auto* dialog = new SettingsDialog(*surface->client, *surface->runner, nullptr);
        surface->root = dialog;
        surface->settingsDialog = true;
        dialog->setWindowTitle(title);
        dialog->resize(manifest.width, manifest.height);
        dialog->show();
        return true;
    }
    if (manifest.surface == ProbeSurface::PlayerWindow) {
        if (manifest.media.isEmpty()) {
            *reason = QStringLiteral(
                          "the player_dialog manifest surface needs media, and manifest '%1' names none")
                          .arg(manifest.name);
            return false;
        }
        // Keep the manifest's stream and optional episode rows, but drive them
        // through the production Quick controller and its app-owned assembly.
        const QList<QualityEntry> qualities{{manifest.height, manifest.media}};
        surface->runner = std::make_unique<Runner>();
        surface->extraHost = std::make_unique<ProbePlayerHost>();
        surface->player = std::make_unique<PlayerController>(
            manifest.name.isEmpty() ? QStringLiteral("probe") : manifest.name, manifest.media,
            qualities, manifest.height,
            manifest.episodes.isEmpty() ? nullptr : surface->extraHost.get());
        surface->controller = surface->player.get();
        surface->controller->setObjectName(QStringLiteral("playerController"));
        surface->session = surface->controller->session();
        PlayerWindow* window = surface->controller->window();
        if (window == nullptr || surface->session == nullptr) {
            *reason = QStringLiteral("the production player exposes no window or VideoSession");
            return false;
        }
        surface->root = window;
        window->setTitle(title);
        window->resize(QSize(manifest.width, manifest.height));
        if (!manifest.episodes.isEmpty()) {
            QVector<QVariantMap> playlist;
            for (const QString& label : manifest.episodes) {
                QVariantMap row;
                row.insert(QStringLiteral("label"), label);
                row.insert(QStringLiteral("embed"), manifest.media);
                playlist.append(row);
            }
            surface->extraHost->rows = playlist;
            surface->extraHost->qualities = qualities;
            surface->extraHost->controller = surface->controller;
            surface->controller->setPlaylist(playlist, 0);
        }
        mikai::app::bindNativePlaybackAssembly(*surface->runner, *surface->controller);
        surface->controller->showWindow();
        if (!surface->session->waitForContext(kContextTimeoutMs)) {
            *reason = QStringLiteral(
                          "the production player's VideoSession never created a render context");
            return false;
        }
        surface->controller->startPlayer();
        return true;
    }

    auto* video = new VideoSurface();
    surface->root = video;
    surface->video = video;
    surface->session = video->session();
    // The standalone surface has no other identity to be found under, so it
    // carries the name the standalone surface probe expects.
    video->setObjectName(QStringLiteral("videoSurface"));
    video->setWindowTitle(title);
    video->resize(manifest.width, manifest.height);
    video->show();
    if (!video->waitForContext(kContextTimeoutMs)) {
        *reason = QStringLiteral("the VideoSurface never created a render context");
        return false;
    }
    if (!manifest.media.isEmpty() && !surface->session->open(manifest.media)) {
        *reason = QStringLiteral("the mpv engine refused to open '%1'").arg(manifest.media);
        return false;
    }
    return true;
}

QString teardown(Surface* surface) {
    QString problem;
    // Settle native work while the render context is alive, then use the
    // production controller's acknowledged close path.
    if (surface->session != nullptr &&
        !surface->session->settleNativeForTeardown(kTeardownSettleMs)) {
        problem = QStringLiteral("the surface did not acknowledge its native stop");
    }
    if (surface->controller != nullptr) {
        if (!surface->controller->requestClose() && problem.isEmpty()) {
            problem = QStringLiteral("the production player refused to close");
        }
        surface->player.reset();
        surface->controller = nullptr;
        surface->video = nullptr;
        surface->session = nullptr;
        surface->root = nullptr;
    } else if (surface->settingsDialog) {
        QPointer<QWidget> guard(qobject_cast<QWidget*>(surface->root));
        if (auto* dialog = qobject_cast<QDialog*>(guard.data())) {
            dialog->close();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!guard.isNull()) {
            // The dialog refused to close or its delete was deferred: destroy
            // the owned settings dialog so nothing survives the manifest.
            delete guard.data();
        }
    } else if (surface->video != nullptr) {
        if (surface->session != nullptr) surface->session->close();
        surface->video->hide();
        delete surface->video;
    }
    *surface = Surface{};
    return problem;
}

QVariant mpvVariant(const ProbePropertyValue& value) {
    switch (value.kind) {
        case ProbePropertyValue::Kind::Double:
            return QVariant(value.number);
        case ProbePropertyValue::Kind::Integer:
            return QVariant::fromValue<qlonglong>(value.integer);
        case ProbePropertyValue::Kind::Flag:
            return QVariant(value.flag);
        case ProbePropertyValue::Kind::Text:
            return QVariant(value.text);
    }
    return {};
}

ProbePropertyKind toMpvKind(ProbeMpvKind kind) {
    switch (kind) {
        case ProbeMpvKind::Flag:
            return ProbePropertyKind::Flag;
        case ProbeMpvKind::Text:
            return ProbePropertyKind::Text;
        case ProbeMpvKind::Number:
            break;
    }
    return ProbePropertyKind::Number;
}

struct AssertionReport {
    bool passed = false;
    QString label;
    QString actual;
    QString expected;
    QString reason;
};

void finishComparison(const ProbeAssertion& assertion, const QVariant& actual,
                      AssertionReport* out) {
    const QString op = assertion.comparison.isEmpty() ? QStringLiteral("equals")
                                                      : assertion.comparison;
    out->actual = valueText(actual);
    out->expected = QStringLiteral("%1 %2").arg(op, valueText(assertion.expected));
    if (op == QLatin1String("approximately")) {
        out->expected += QStringLiteral(" ±%1").arg(assertion.tolerance);
    }
    const Verdict verdict =
        compareValues(assertion.comparison, actual, assertion.expected, assertion.tolerance);
    out->passed = verdict.ok;
    out->reason = verdict.reason;
}

void evaluateAssertion(const ProbeAssertion& assertion, const Surface& surface, int emissions,
                       const QString& armReason, AssertionReport* out) {
    switch (assertion.kind) {
        case ProbeAssertion::Kind::SignalEmitted: {
            out->label = QStringLiteral("signal %1 emitted").arg(assertion.property);
            if (!armReason.isEmpty()) {
                out->reason = armReason;
                return;
            }
            if (assertion.expected.isValid()) {
                finishComparison(assertion, emissions, out);
            } else {
                out->actual = QStringLiteral("%1 emission(s)").arg(emissions);
                out->expected = QStringLiteral(">= 1");
                out->passed = emissions > 0;
            }
            return;
        }
        case ProbeAssertion::Kind::SignalNotEmitted: {
            out->label = QStringLiteral("signal %1 not emitted").arg(assertion.property);
            if (!armReason.isEmpty()) {
                out->reason = armReason;
                return;
            }
            out->actual = QStringLiteral("%1 emission(s)").arg(emissions);
            out->expected = QStringLiteral("0");
            out->passed = emissions == 0;
            return;
        }
        case ProbeAssertion::Kind::WidgetExists: {
            out->label = QStringLiteral("widget %1 exists").arg(assertion.target);
            if (assertion.target.isEmpty()) {
                out->reason = QStringLiteral("the assertion names no widget");
                return;
            }
            const bool exists = findTarget(surface, assertion.target) != nullptr;
            out->actual = exists ? QStringLiteral("present") : QStringLiteral("absent");
            out->expected = QStringLiteral("present");
            out->passed = exists;
            if (!exists) {
                out->reason = QStringLiteral("no widget named '%1'").arg(assertion.target);
            }
            return;
        }
        case ProbeAssertion::Kind::QtProperty:
        case ProbeAssertion::Kind::WidgetProperty: {
            out->label = QStringLiteral("%1 %2.%3")
                             .arg(assertion.kind == ProbeAssertion::Kind::QtProperty
                                      ? QStringLiteral("QtProperty")
                                      : QStringLiteral("WidgetProperty"),
                                  assertion.target, assertion.property);
            if (assertion.target.isEmpty()) {
                out->reason = QStringLiteral("the assertion names no widget");
                return;
            }
            QObject* object = findTarget(surface, assertion.target);
            if (object == nullptr) {
                out->reason = QStringLiteral("no widget named '%1'").arg(assertion.target);
                return;
            }
            const QByteArray name = assertion.property.toUtf8();
            if (auto* model = qobject_cast<mplayer::quick::PlayerMenuModel*>(object);
                model != nullptr) {
                if (assertion.property == QLatin1String("rowCount")) {
                    finishComparison(assertion, model->rowCount(), out);
                    return;
                }
            }
            if (object->metaObject()->indexOfProperty(name.constData()) < 0) {
                out->actual = QStringLiteral("<no such property>");
                out->reason = QStringLiteral("'%1' has no Q_PROPERTY named '%2'")
                                  .arg(assertion.target, assertion.property);
                return;
            }
            finishComparison(assertion, object->property(name.constData()), out);
            return;
        }
        case ProbeAssertion::Kind::MpvProperty: {
            out->label = QStringLiteral("MpvProperty %1").arg(assertion.property);
            if (surface.session == nullptr) {
                out->reason = QStringLiteral("there is no VideoSession to read mpv from");
                return;
            }
            const std::optional<ProbePropertyValue> value =
                surface.session->probeMpvProperty(assertion.property, toMpvKind(assertion.mpvKind));
            if (!value.has_value()) {
                out->actual = QStringLiteral("<unreadable>");
                out->reason = QStringLiteral("mpv property '%1' could not be read")
                                  .arg(assertion.property);
                return;
            }
            finishComparison(assertion, mpvVariant(*value), out);
            return;
        }
    }
}

/// The Ukrainian catalog the runner installs with, found beside its own
/// executable. The runner links mikai_app, which does not embed the application
/// catalog, so a Ukrainian manifest has to be told where the built file is -
/// the same file the geometry probe reads from MIKAI_PROBE_TRANSLATION.
QString builtUkrainianCatalog() {
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    if (applicationDirectory.isEmpty()) {
        return QString();
    }
    const QString path = QDir(applicationDirectory).absoluteFilePath(QStringLiteral("mikai_uk.qm"));
    return QFileInfo::exists(path) ? path : QString();
}

/// The language the runner itself started in, from the same variable
/// initializeUi read. Restoring this is what keeps one manifest's `language`
/// from being the next manifest's language.
QString startupLanguage() {
    return qEnvironmentVariable("MIKAI_PROBE_LANGUAGE", QStringLiteral("en"));
}

/// Installs the manifest's language. Returns false with a reason naming the
/// language when it cannot be installed: a manifest that asked for Ukrainian and
/// got English would assert against strings it never meant to read, so the run
/// is refused rather than silently answered in the wrong language.
bool installRunLanguage(const ProbeManifest& manifest, QString* reason) {
    const QString language = manifest.language;
    const QString applicationCatalog =
        language == QLatin1String("uk") ? builtUkrainianCatalog() : QString();
    if (!mikai::app::applyInterfaceLanguage(language, applicationCatalog)) {
        *reason = language == QLatin1String("uk")
                      ? QStringLiteral(
                            "language 'uk' is unavailable: no built Ukrainian catalog at %1 "
                            "(the runner looks for mikai_uk.qm beside its own executable)")
                            .arg(applicationCatalog.isEmpty()
                                     ? QStringLiteral("<not found>")
                                     : applicationCatalog)
                      : QStringLiteral("language '%1' is unavailable").arg(language);
        return false;
    }
    std::fprintf(stderr, "mikai probe run: manifest '%s' language=%s installed=%s catalog=%s\n",
                 qPrintable(manifest.name), qPrintable(language),
                 qPrintable(mikai::app::activeInterfaceLanguage()),
                 applicationCatalog.isEmpty() ? "<none>" : qPrintable(applicationCatalog));
    return true;
}

/// Puts the runner's startup language back, after teardown and while no surface
/// of that run is left holding translated strings. A restore that fails is
/// reported but not fatal: the next manifest installs its own language anyway.
void restoreRunLanguage() {
    const QString startup = startupLanguage();
    if (startup == mikai::app::activeInterfaceLanguage()) {
        return;
    }
    const QString applicationCatalog =
        startup == QLatin1String("uk") ? builtUkrainianCatalog() : QString();
    if (!mikai::app::applyInterfaceLanguage(startup, applicationCatalog)) {
        std::fprintf(stderr,
                     "mikai probe run: FAIL could not restore the runner startup language '%s'\n",
                     qPrintable(startup));
    }
}

}  // namespace

ProbeRunner::ProbeRunner() = default;
ProbeRunner::~ProbeRunner() = default;

ProbeRunResult ProbeRunner::run(const ProbeManifest& manifest) {
    ProbeRunResult result;

    // The manifest's language is installed before the surface exists: a widget
    // reads tr() when it is constructed, so installing after buildSurface would
    // leave every label in the runner's startup language and pass anyway.
    // An empty language means the runner's own default (MIKAI_PROBE_LANGUAGE),
    // which is already installed: nothing to change, nothing to restore. A
    // language that cannot be installed is refused by name, because falling
    // back to English would let a Ukrainian manifest assert against English
    // strings and read as a pass.
    if (!manifest.language.isEmpty()) {
        QString languageReason;
        if (!installRunLanguage(manifest, &languageReason)) {
            result.detail = languageReason;
            report(false, QStringLiteral("%1 :: language — %2").arg(manifest.name, languageReason));
            return result;
        }
    }

    Surface surface;
    QString reason;
    if (!buildSurface(manifest, &surface, &reason)) {
        result.detail = reason;
        report(false, QStringLiteral("%1 :: surface — %2").arg(manifest.name, reason));
        const QString teardownProblem = teardown(&surface);
        if (!teardownProblem.isEmpty()) {
            result.detail = QStringLiteral("%1; teardown: %2").arg(result.detail, teardownProblem);
        }
        restoreRunLanguage();
        return result;
    }

    // Signal assertions are armed BEFORE the steps run: a signal a step causes
    // is exactly what they exist to count, and it would be gone by the time the
    // steps finished. The spies live until every assertion has been decided.
    const int assertionCount = static_cast<int>(manifest.assertions.size());
    QVector<int> emissions(assertionCount, 0);
    QVector<QString> armReasons(assertionCount);
    struct Armed {
        int index;
        QSignalSpy* spy;
    };
    std::vector<Armed> armed;
    const QVector<QObject*> candidates = signalCandidates(surface);
    for (int index = 0; index < assertionCount; ++index) {
        const ProbeAssertion& assertion = manifest.assertions.at(index);
        if (assertion.kind != ProbeAssertion::Kind::SignalEmitted &&
            assertion.kind != ProbeAssertion::Kind::SignalNotEmitted) {
            continue;
        }
        QObject* emitter = nullptr;
        QMetaMethod method;
        if (!resolveSignal(candidates, assertion.property, &emitter, &method)) {
            armReasons[index] =
                QStringLiteral("no signal named '%1' on the window, controller or session")
                    .arg(assertion.property);
            continue;
        }
        auto* spy = new QSignalSpy(emitter, method);
        if (!spy->isValid()) {
            delete spy;
            armReasons[index] =
                QStringLiteral("the signal '%1' could not be observed").arg(assertion.property);
            continue;
        }
        armed.push_back({index, spy});
    }

    const int stepCount = static_cast<int>(manifest.steps.size());
    for (int index = 0; index < stepCount; ++index) {
        const ProbeStep& step = manifest.steps.at(index);
        QString stepReason;
        if (runStep(step, surface, &stepReason)) {
            continue;
        }
        QString detail = QStringLiteral("step %1 (%2): %3")
                             .arg(index + 1)
                             .arg(stepKindName(step.kind), stepReason);
        if (!step.note.isEmpty()) {
            detail += QStringLiteral(" — %1").arg(step.note);
        }
        result.detail = detail;
        report(false, QStringLiteral("%1 :: %2").arg(manifest.name, detail));
        break;
    }

    for (const Armed& entry : armed) {
        emissions[entry.index] = static_cast<int>(entry.spy->count());
        delete entry.spy;
    }
    armed.clear();

    for (int index = 0; index < assertionCount; ++index) {
        const ProbeAssertion& assertion = manifest.assertions.at(index);
        AssertionReport assertionReport;
        evaluateAssertion(assertion, surface, emissions.at(index), armReasons.at(index),
                          &assertionReport);
        ++result.assertions;
        if (!assertionReport.passed) {
            ++result.failed;
        }
        QString line = QStringLiteral("%1 :: %2").arg(manifest.name, assertionReport.label);
        if (!assertionReport.actual.isEmpty() || !assertionReport.expected.isEmpty()) {
            line += QStringLiteral(" (actual %1, expected %2)")
                        .arg(assertionReport.actual.isEmpty() ? QStringLiteral("<none>")
                                                              : assertionReport.actual,
                             assertionReport.expected);
        }
        if (!assertionReport.reason.isEmpty()) {
            line += QStringLiteral(" — %1").arg(assertionReport.reason);
        }
        if (!assertion.note.isEmpty()) {
            line += QStringLiteral(" — %1").arg(assertion.note);
        }
        report(assertionReport.passed, line);
    }

    const QString teardownProblem = teardown(&surface);
    if (!teardownProblem.isEmpty()) {
        result.detail = result.detail.isEmpty()
                            ? QStringLiteral("teardown: %1").arg(teardownProblem)
                            : QStringLiteral("%1; teardown: %2").arg(result.detail, teardownProblem);
    }

    result.ok = result.detail.isEmpty() && result.failed == 0;
    // After teardown, so the next manifest builds its surface in the runner's
    // own language rather than inheriting this manifest's.
    restoreRunLanguage();
    return result;
}

}  // namespace mikai::probes
