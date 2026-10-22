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

    /// Display-ready card pixmap: the page paints it once per poster URL
    /// through rounded() and hands the same one to every card sharing it.
    void applyPixmap(const QPixmap& pixmap);
    static QPixmap rounded(const QPixmap& pixmap, const QSize& target);
    static QString elideTwoLines(const QString& text, const QFont& font, int maxWidth);
    void applyStatus(const QString& status);
    void applyEpisodes(int aired, int planned);
    void applyDetail(const backend::AnimeDetail& detail);

Q_SIGNALS:
    void openRequested(const QString& ref, const QString& poster);
    void menuRequested(const QString& ref, const QPoint& globalPos);

protected:
    /// Restates the card's own captions in place after a LanguageChange: the
    /// poster already painted and the data last applied are kept as they are.
    void changeEvent(QEvent* event) override;

private:
    QLabel* makeLabel(const QString& objectName, const QString& text);
    /// Re-words the poster fallback, the status and the count from the last
    /// data applied, without touching a landed pixmap.
    void retranslateUi();

    QString ref_;
    QString imageKey_;
    QString posterUrl_;
    /// The raw status last applied: the translated label is derived from it, so
    /// a language switch re-derives instead of keeping the old language.
    QString statusRaw_;
    int planned_ = 0;
    /// The aired fraction last applied, kept beside `planned_` for the same
    /// reason: the count line is rebuilt from both on a switch.
    int aired_ = 0;
    int localized_ = 0;
    bool hasDetailStats_ = false;
    /// Whether `status_` currently carries the live-status object name; the
    /// label starts out with the non-live name, matching statusLive_'s zero.
    bool statusLive_ = false;

    QLabel* poster_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* episodes_ = nullptr;
};

}  // namespace mikai::app
