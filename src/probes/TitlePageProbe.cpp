// Live title-page probe: the one reported bug no amount of source reading has
// root-caused is "Ashdi player episodes are not displayed". Everything up to
// the player reads correctly in source, and the playlist path reads correctly,
// so the only honest way to settle it is to build the real page against the
// real API and look at what actually arrives.
//
// This answers one question with two numbers per provider tab:
//   1. how many episode pills the tab's grid actually holds, and
//   2. after clicking one, whether a PlayerDialog exists, whether its series
//      toggle is visible, and how many rows the series drawer lists.
// A tab that renders zero pills points at fillEpisodes' grouping; a tab that
// renders twelve with an empty drawer points at tabPlaylist/setPlaylist.

#include <QApplication>
#include <QCoreApplication>
#include <QTest>
#include <QMouseEvent>
#include <QComboBox>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include <cstdio>

#include "backend/MikaiClient.hpp"
#include "pages/TitlePage.hpp"
#include "player/PlayerDialog.hpp"
#include "player/PlayerShell.hpp"
#include "state/QualityCache.hpp"
#include "workers/Runner.hpp"

using mikai::app::QualityCache;
using mikai::app::Runner;
using mikai::app::TitlePage;
using mikai::backend::MikaiClient;

namespace {

QPushButton* playerSeriesToggle(QWidget* root) {
    return root->findChild<QPushButton*>(QStringLiteral("playerSeriesToggle"));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString ref = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("7428");

    MikaiClient client(qEnvironmentVariable("MIKAI_API_KEY"));
    Runner runner;
    QualityCache cache;
    auto* page = new TitlePage(runner, client, cache, ref, QString(), nullptr);
    page->resize(1400, 900);
    page->show();

    // Stage 1: what did the tabs actually receive?
    QTimer::singleShot(9000, [page, ref] {
        auto* tabs = page->findChild<QTabWidget*>();
        if (tabs == nullptr || tabs->count() == 0) {
            std::fprintf(stderr, "TITLE-PROBE: %s produced no release tabs\n", qPrintable(ref));
            return;
        }
        std::fprintf(stderr, "TITLE-PROBE: %s -> %d provider tabs\n", qPrintable(ref), tabs->count());
        for (int i = 0; i < tabs->count(); ++i) {
            QWidget* tabPage = tabs->widget(i);
            const int pills = tabPage->findChildren<QPushButton*>(QStringLiteral("episodeButton")).size();
            std::fprintf(stderr, "TITLE-PROBE:   tab %d %-12s pills=%d\n", i,
                         qPrintable(tabs->tabText(i)), pills);
        }

        // Stage 2: click the first pill of the FIRST tab and see what the player
        // does with it. Whichever provider that is, the drawer answer is the
        // thing that has never been observed.
        QWidget* first = tabs->widget(0);
        auto* pill = first->findChild<QPushButton*>(QStringLiteral("episodeButton"));
        if (pill == nullptr) {
            std::fprintf(stderr, "TITLE-PROBE: tab 0 has no pill to click\n");
            return;
        }
        std::fprintf(stderr, "TITLE-PROBE: clicking a pill on %s\n",
                     qPrintable(tabs->tabText(0)));
        pill->click();
    });

    // Stage 3: the player (if any) and its series drawer.
    QTimer::singleShot(20000, [&] {
        QWidget* dialog = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (QString::fromLatin1(w->metaObject()->className())
                    .contains(QStringLiteral("PlayerDialog"))) {
                dialog = w;
                break;
            }
        }
        if (dialog == nullptr) {
            std::fprintf(stderr, "TITLE-PROBE: NO PlayerDialog opened after clicking a pill\n");
            std::fprintf(stderr, "TITLE-PROBE: RESULT INCONCLUSIVE\n");
            return;
        }
        QPushButton* toggle = playerSeriesToggle(dialog);
        std::fprintf(stderr, "TITLE-PROBE: PlayerDialog exists; series toggle %s\n",
                     toggle == nullptr ? "MISSING"
                                       : (toggle->isVisible() ? "visible" : "PRESENT BUT HIDDEN"));
        if (toggle != nullptr) {
            // isHidden() is true only when THIS widget was hidden explicitly;
            // isVisible() is false if any ANCESTOR is hidden. The difference is
            // the whole question: an empty playlist calls setVisible(false) on
            // the button itself, while a hidden strip leaves the button itself
            // visible and merely buries it.
            std::fprintf(stderr, "TITLE-PROBE:   toggle->isHidden()=%d  visibleToParent=%d\n",
                         toggle->isHidden() ? 1 : 0,
                         toggle->isVisibleTo(toggle->parentWidget()) ? 1 : 0);
            for (QWidget* w = toggle->parentWidget(); w != nullptr; w = w->parentWidget()) {
                std::fprintf(stderr, "TITLE-PROBE:   ancestor %-24s visible=%d hidden=%d\n",
                             qPrintable(w->objectName().isEmpty() ? w->metaObject()->className()
                                                                  : w->objectName()),
                             w->isVisible() ? 1 : 0, w->isHidden() ? 1 : 0);
            }
        }
        // The strip is hidden by the chrome auto-hide, so reach the episode
        // list the way a user would: wake the chrome, then open the drawer and
        // leave it alone. If the drawer is still up 3.5s later - well past the
        // 2.5s idle - the auto-hide is not stealing it, which is the contract.
        for (QWidget* w : dialog->findChildren<QWidget*>()) {
            if (QString::fromLatin1(w->metaObject()->className())
                    .contains(QStringLiteral("VideoSurface"))) {
                for (const QPointF& at : {QPointF(40, 40), QPointF(60, 60)}) {
                    QMouseEvent move(QEvent::MouseMove, at, w->mapToGlobal(at), Qt::NoButton,
                                     Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(w, &move);
                }
            }
        }
        QApplication::processEvents();
        std::fprintf(stderr, "TITLE-PROBE: after waking, toggle visible=%d strip visible=%d\n",
                     toggle != nullptr && toggle->isVisible() ? 1 : 0,
                     toggle != nullptr && toggle->parentWidget() != nullptr &&
                             toggle->parentWidget()->isVisible()
                         ? 1
                         : 0);
        if (toggle != nullptr && toggle->isVisible()) {
            toggle->click();
            QApplication::processEvents();
            auto* drawer = dialog->findChild<QWidget*>(QStringLiteral("playerSeriesDrawer"));
            const int rows =
                drawer == nullptr
                    ? 0
                    : drawer->findChildren<QPushButton*>(QStringLiteral("playerSeriesItem")).size() +
                          drawer->findChildren<QPushButton*>(
                              QStringLiteral("playerSeriesItemCurrent")).size();
            std::fprintf(stderr, "TITLE-PROBE: drawer %s, rows=%d\n",
                         drawer == nullptr ? "MISSING" : "present", rows);
            // Now stop touching anything and let the idle timer run past its
            // 2.5s hold. A user reading the episode list is not idle.
            QTest::qWait(3500);
            QApplication::processEvents();
            std::fprintf(stderr,
                         "TITLE-PROBE: 3.5s after opening, drawer visible=%d rows=%d\n",
                         drawer->isVisible() ? 1 : 0,
                         drawer->findChildren<QPushButton*>(QStringLiteral("playerSeriesItem")).size() +
                             drawer->findChildren<QPushButton*>(
                                 QStringLiteral("playerSeriesItemCurrent")).size());
        }
        std::fprintf(stderr, "TITLE-PROBE: RESULT OK\n");
    });

    // Stage 4: the reported crash. Opening and closing the cover repeatedly used
    // to fault: the viewer is WA_DeleteOnClose, so closing it destroyed the
    // dialog while the page kept the pointer, and the NEXT open called close()
    // on freed memory. Four rounds, because one round passes by luck.
    QTimer::singleShot(26000, [page] {
        auto* cover = page->findChild<QWidget*>(QStringLiteral("titleCover"));
        if (cover == nullptr) {
            std::fprintf(stderr, "COVER-PROBE: no titleCover label found\n");
            return;
        }
        if (!cover->isEnabled()) {
            std::fprintf(stderr, "COVER-PROBE: cover is disabled, skipping\n");
            return;
        }
        int opened = 0;
        for (int round = 0; round < 4; ++round) {
            const QPointF at(4, 4);
            QMouseEvent press(QEvent::MouseButtonPress, at, cover->mapToGlobal(at),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(cover, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, cover->mapToGlobal(at),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(cover, &release);
            QApplication::processEvents();

            QWidget* viewer = nullptr;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (QString::fromLatin1(w->metaObject()->className())
                        .contains(QStringLiteral("CoverViewer"))) {
                    viewer = w;
                    break;
                }
            }
            if (viewer == nullptr) {
                std::fprintf(stderr, "COVER-PROBE: round %d did not open a viewer\n", round);
                continue;
            }
            ++opened;
            std::fprintf(stderr, "COVER-PROBE: round %d opened the viewer\n", round);
            viewer->close();
            QApplication::processEvents();
        }
        std::fprintf(stderr, "COVER-PROBE: %d/4 rounds opened; reaching here is the fix\n",
                     opened);
        std::fprintf(stderr, "COVER-PROBE: RESULT OK\n");
    });

    return app.exec();
}
