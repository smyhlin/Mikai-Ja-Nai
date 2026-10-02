#pragma once

#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QWidget>

class QStackedWidget;

namespace mikai::backend {
class MikaiClient;
}

namespace mplayer::shell {
class PlayerDialog;
}

namespace mikai::app {

class QualityCache;
class Runner;
class SearchPage;
class TitlePage;

/// The application shell: a `QStackedWidget` holding one `SearchPage` and at
/// most one `TitlePage`.
///
/// Opening a title disposes the previous one, which saves the quality cache and
/// closes every player it opened. A related title pushes onto a small history
/// so "back" walks the chain instead of dropping to the search page.
class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(backend::MikaiClient& client, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void openSettings();
    void chooseMediaFile();
    void chooseMediaUrl();
    void openMedia(const QString& source);
    void closeMedia();
    /// The standalone player's version of the title page's runtime request: it
    /// provisions the mpv runtime when the core could not be created, then
    /// starts the player again.
    void requestMediaRuntime();
    void openTitle(const QString& ref, const QString& poster, bool preserveHistory = false);
    void openRelated(const QString& ref, const QString& poster);
    void goSearch();
    void disposeTitle();

    backend::MikaiClient& client_;
    Runner* runner_ = nullptr;
    QualityCache* qualityCache_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    SearchPage* search_ = nullptr;
    TitlePage* title_ = nullptr;
    QPointer<mplayer::shell::PlayerDialog> mediaPlayer_;
    /// One runtime request at a time for the standalone player. `runtimeSeq_`
    /// is bumped whenever that dialog goes away, so a completion belonging to a
    /// superseded dialog is dropped instead of starting the new one.
    int runtimeSeq_ = 0;
    bool runtimePending_ = false;
    bool runtimeRetrying_ = false;
    QString titleRef_;
    QString titlePoster_;
    QList<QPair<QString, QString>> titleHistory_;
};

}  // namespace mikai::app
