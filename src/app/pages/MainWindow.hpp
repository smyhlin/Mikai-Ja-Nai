#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QStackedWidget;

namespace mikai::backend {
class MikaiClient;
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
    QString titleRef_;
    QString titlePoster_;
    QList<QPair<QString, QString>> titleHistory_;
};

}  // namespace mikai::app
