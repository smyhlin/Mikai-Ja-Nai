#pragma once

#include <backend/Models.hpp>

#include <QPoint>
#include <QPushButton>
#include <QString>

class QLabel;
class QPixmap;

namespace mikai::app {

/// One entry of a home carousel: poster, title, airing status and episode count.
///
/// List responses carry the planned episode count only, so the aired fraction
/// stays a placeholder until a detail response lands on `applyDetail`.
class TitleCard : public QPushButton {
    Q_OBJECT

public:
    explicit TitleCard(const backend::AnimeSummary& summary, QWidget* parent = nullptr);
    ~TitleCard() override;

    const QString& ref() const { return ref_; }
    /// The URL the poster is cached under; several cards can share one.
    QString imageKey() const { return imageKey_; }
    const QString& posterUrl() const { return posterUrl_; }

    void applyPixmap(const QPixmap& pixmap);
    void applyStatus(const QString& status);
    void applyEpisodes(int aired, int planned);
    void applyDetail(const backend::AnimeDetail& detail);

Q_SIGNALS:
    void openRequested(const QString& ref, const QString& poster);
    void menuRequested(const QString& ref, const QPoint& globalPos);

private:
    QLabel* makeLabel(const QString& objectName, const QString& text);

    QString ref_;
    QString imageKey_;
    QString posterUrl_;
    int planned_ = 0;

    QLabel* poster_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* episodes_ = nullptr;
};

}  // namespace mikai::app
