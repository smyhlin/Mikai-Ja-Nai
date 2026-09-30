#include "pages/MainWindow.hpp"

#include "dialogs/SettingsDialog.hpp"
#include "pages/SearchPage.hpp"
#include "pages/TitlePage.hpp"
#include "state/QualityCache.hpp"
#include "workers/Runner.hpp"

#include <backend/MikaiClient.hpp>

#include <PlayerDialog.hpp>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QUrl>
#include <QCloseEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace mikai::app {

MainWindow::MainWindow(backend::MikaiClient& client, QWidget* parent)
    : QWidget(parent), client_(client) {
    setWindowTitle(tr("Mikai Wrapper"));
    setMinimumSize(860, 620);
    resize(1080, 760);

    runner_ = new Runner(this);
    qualityCache_ = new QualityCache(this);
    qualityCache_->load();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    root->addWidget(stack_);

    search_ = new SearchPage(*runner_, client_, stack_);
    connect(search_, &SearchPage::openRequested, this,
            [this](const QString& ref, const QString& poster) { openTitle(ref, poster); });
    connect(search_, &SearchPage::settingsRequested, this, &MainWindow::openSettings);
    connect(search_, &SearchPage::mediaFileRequested, this, &MainWindow::chooseMediaFile);
    connect(search_, &SearchPage::mediaUrlRequested, this, &MainWindow::chooseMediaUrl);
    stack_->addWidget(search_);
}

MainWindow::~MainWindow() {
    closeMedia();
    // The close event covers the ordinary path, but a session that ends any
    // other way (a quit() that never delivers one) would otherwise leave the
    // entries unwritten. flushNow() is a no-op once the cache is clean, so
    // running it twice costs nothing.
    qualityCache_->flushNow();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    closeMedia();
    disposeTitle();
    // The title page flushes when it shuts down, but a session that only browsed
    // the home page never reaches that path. Persist here so those sessions
    // keep their extracted streams, and use the forced variant so the debounce
    // interval cannot swallow the last write.
    qualityCache_->flushNow();
    QWidget::closeEvent(event);
}

void MainWindow::chooseMediaFile() {
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Відкрити відеофайл"), {},
        tr("Відео (*.mkv *.mp4 *.webm *.avi *.mov *.m4v *.ts *.m2ts *.mpeg *.mpg);;Усі файли (*)"));
    if (!file.isEmpty()) {
        openMedia(file);
    }
}

void MainWindow::chooseMediaUrl() {
    bool accepted = false;
    const QString source = QInputDialog::getText(
        this, tr("Відкрити відео за посиланням"), tr("Посилання на відео або потік:"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (accepted && !source.isEmpty()) {
        openMedia(source);
    }
}

void MainWindow::openMedia(const QString& source) {
    const QUrl url = QUrl::fromUserInput(source, {}, QUrl::AssumeLocalFile);
    const QString media = url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded);
    QString caption = url.fileName();
    if (caption.isEmpty()) {
        caption = url.host().isEmpty() ? tr("Відео") : url.host();
    }
    closeMedia();
    mediaPlayer_ = new mplayer::shell::PlayerDialog(caption, media, this);
    mediaPlayer_->show();
    mediaPlayer_->startPlayer();
}

void MainWindow::closeMedia() {
    if (mediaPlayer_) {
        mediaPlayer_->close();
        mediaPlayer_.clear();
    }
}

void MainWindow::openSettings() {
    SettingsDialog dialog(client_, *runner_, this);
    if (dialog.exec() == QDialog::Accepted) {
        search_->refreshFeedSettings();
    }
}

void MainWindow::openTitle(const QString& ref, const QString& poster, bool preserveHistory) {
    if (ref.isEmpty()) {
        return;
    }
    if (!preserveHistory) {
        titleHistory_.clear();
    }
    disposeTitle();
    titleRef_ = ref;
    titlePoster_ = poster;
    // The card's detail lookup usually already landed for this ref, so hand it
    // over and skip the redundant getAnime inside the title page.
    const backend::AnimeDetail* hint = search_->cachedDetail(ref);
    title_ = new TitlePage(*runner_, client_, *qualityCache_, ref, poster,
                           hint != nullptr ? *hint : backend::AnimeDetail(), stack_);
    connect(title_, &TitlePage::backRequested, this, &MainWindow::goSearch);
    connect(title_, &TitlePage::settingsRequested, this, &MainWindow::openSettings);
    connect(title_, &TitlePage::relatedOpenRequested, this,
            [this](const QString& target, const QString& targetPoster) {
                openRelated(target, targetPoster);
            });
    connect(title_, &TitlePage::recentTitleLoaded, search_, &SearchPage::recordRecentTitle);
    stack_->addWidget(title_);
    stack_->setCurrentWidget(title_);
}

void MainWindow::openRelated(const QString& ref, const QString& poster) {
    if (!titleRef_.isEmpty()) {
        titleHistory_.append({titleRef_, titlePoster_});
    }
    openTitle(ref, poster, true);
}

void MainWindow::goSearch() {
    if (!titleHistory_.isEmpty()) {
        const auto previous = titleHistory_.takeLast();
        openTitle(previous.first, previous.second, true);
        return;
    }
    disposeTitle();
    titleRef_.clear();
    titlePoster_.clear();
    stack_->setCurrentWidget(search_);
}

void MainWindow::disposeTitle() {
    if (title_ == nullptr) {
        return;
    }
    // Shutdown saves the quality cache and closes every player before the
    // widget goes, so switching titles never strands a window.
    title_->shutdown();
    stack_->removeWidget(title_);
    title_->deleteLater();
    title_ = nullptr;
}

}  // namespace mikai::app
