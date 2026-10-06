// The engine-preparation window: what the player shows while a TensorRT engine
// is built.
//
// It is driven through the player's own public API, not by poking the widget, so
// what is judged is the surface a host actually reaches. The window is a
// top-level tool window rather than a child of the player, because the build
// starts from a modal settings dialog; that is why its visibility is read from
// the window itself and not from a parent's child list.

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

#include "PlayerDialog.hpp"
#include "PlayerPopups.hpp"
#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"

using namespace mplayer::shell;

namespace {

class StubHost : public PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestOpenOriginal() override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

QString labelTexts(const QWidget* root) {
    QStringList texts;
    for (const QLabel* label : root->findChildren<QLabel*>()) {
        texts << label->text();
    }
    return texts.join(QStringLiteral(" | "));
}

QProgressBar* bar(const QWidget* root) {
    return root->findChild<QProgressBar*>();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiEnginePopupProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("EnginePopupProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    static StubHost host;
    const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral("av://lavfi:testsrc2=size=640x360:rate=25");
    const QList<QualityEntry> qualities = {{360, url}};

    auto* dialog = new PlayerDialog(QStringLiteral("probe 360p"), url, qualities, 360, &host);
    dialog->startPlayer();
    dialog->show();

    mplayer::probes::Verdict verdict;

    // Nothing is shown before a build is announced: a window that appears on its
    // own would be a claim about work nobody asked for.
    verdict.require(!dialog->enginePreparationVisible(),
                    QStringLiteral("no preparation window before a build is announced"));

    bool cancelSeen = false;
    QObject::connect(dialog, &PlayerDialog::enginePreparationCancelRequested, dialog,
                     [&cancelSeen] { cancelSeen = true; });

    dialog->beginEnginePreparation(QStringLiteral("Preparing TensorRT engine"));
    QApplication::processEvents();
    auto* overlay = dialog->findChild<ProcessingOverlay*>();
    verdict.require(overlay != nullptr, QStringLiteral("the player owns a preparation window"));
    verdict.require(dialog->enginePreparationVisible(),
                    QStringLiteral("the window is visible once a build is announced"));
    if (overlay != nullptr) {
        verdict.require(labelTexts(overlay).contains(QStringLiteral("Preparing TensorRT engine")),
                        QStringLiteral("the window carries the build's own title"));
        QProgressBar* progress = bar(overlay);
        verdict.require(progress != nullptr && progress->minimum() == 0 &&
                            progress->maximum() == 0,
                        QStringLiteral("the bar is indeterminate before a stage count exists"));
    }

    // One real stage event: the plan's own stage count, so the caption is
    // "stage 2 of 3" and the bar is stepped, never a synthesised percentage.
    dialog->updateEnginePreparation(2, 3, QStringLiteral("Building engine"),
                                    QStringLiteral("sm120, 480p -> 1080p"));
    QApplication::processEvents();
    if (overlay != nullptr) {
        const QString text = labelTexts(overlay);
        verdict.require(text.contains(QStringLiteral("Stage 2 of 3")),
                        QStringLiteral("the caption names the stage against the plan's count"));
        verdict.require(text.contains(QStringLiteral("Building engine")),
                        QStringLiteral("the caption carries the builder's own phase"));
        QProgressBar* progress = bar(overlay);
        verdict.require(progress != nullptr && progress->maximum() == 3 && progress->value() == 2,
                        QStringLiteral("the bar steps over the stage count"));
    }

    // Cancel reaches the host, which owns the build.
    if (overlay != nullptr) {
        for (QPushButton* button : overlay->findChildren<QPushButton*>()) {
            if (button->isEnabled()) button->click();
        }
        QApplication::processEvents();
    }
    verdict.require(cancelSeen, QStringLiteral("Cancel is routed to the host that owns the build"));

    // A failure stays up: a refusal that disappears is a refusal nobody read.
    dialog->endEnginePreparation(false, QStringLiteral("native.build.toolFailed"));
    QApplication::processEvents();
    verdict.require(dialog->enginePreparationVisible(),
                    QStringLiteral("a failed build keeps its window up"));
    if (overlay != nullptr) {
        verdict.require(labelTexts(overlay).contains(QStringLiteral("native.build.toolFailed")),
                        QStringLiteral("the window names the failure it stopped on"));
    }

    // A success leaves on its own, after the settle delay. Pumped rather than
    // entered as a nested loop: this probe owns no scene to render and a second
    // event loop only adds a way to die without a verdict.
    dialog->beginEnginePreparation(QStringLiteral("Preparing TensorRT engine"));
    verdict.require(dialog->enginePreparationVisible(),
                    QStringLiteral("a second build reopens the window"));
    dialog->endEnginePreparation(true, QStringLiteral("done"));
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 2500) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    verdict.require(!dialog->enginePreparationVisible(),
                    QStringLiteral("a finished build dismisses its own window"));

    dialog->close();
    return verdict.report("engine_popup_probe");
}
