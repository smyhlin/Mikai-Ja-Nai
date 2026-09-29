#pragma once

#include <QDialog>
#include <QImage>
#include <QString>
#include <QStringList>

class QLabel;
class QListWidget;
class QSlider;
class QComboBox;
class QPushButton;

namespace mplayer {

class VideoSurface;

namespace shell {

/// A side-by-side look at two shader chains on one frozen frame.
///
/// It owns no mpv core and no network: the frame arrives as an image from the
/// host, and each side is rendered by applying a chain to that image through a
/// private surface. That is what makes the two sides comparable - they are the
/// same pixels, so a difference between them is the chain and nothing else.
///
/// The window is non-blocking on purpose. The player keeps playing underneath
/// and the comparison is a still frame the viewer steps through, because a
/// live preview would differ between two grabs for reasons that have nothing to
/// do with the shader.
class CompareWindow : public QDialog {
    Q_OBJECT

public:
    /// How the two sides are presented.
    enum class Layout {
        SideBySide,  ///< A and B next to each other
        Split,       ///< one image with a draggable divide between A and B
        Difference   ///< the absolute pixel difference, amplified
    };

    /// `surface` is borrowed, not owned: the window reads frames from it and
    /// never outlives the player that created it.
    CompareWindow(mplayer::VideoSurface* surface, const QString& currentCandidateId, QWidget* parent = nullptr);
    ~CompareWindow() override;

    /// The candidate the viewer is meant to adopt. Empty when nothing applies.
    QString appliedCandidateId() const { return appliedId_; }

signals:
    /// The viewer picked a winner. The host writes it to the live core.
    void applyRequested(const QString& candidateId);

private slots:
    void onSelectionChanged();
    void onLayoutChanged(int index);
    void onApplyA();
    void onApplyB();
    void onScrubMoved(int value);
    void onSplitMoved(int value);

private:
    void buildUi();
    void populateList();
    void captureFrame();
    void render();
    /// Applies `id` to the live core and returns the settled frame that comes
    /// back. Empty when there is no surface or no frame to work from.
    QImage renderWithChain(const QString& id);
    void renderSideBySide();
    QString labelFor(const QString& id) const;
    void renderSplit();
    void renderDifference();
    void updateInfo();
    /// The picked ids in press order: first is A, second is B. A selection is
    /// a set, so the order the viewer chose has to be read explicitly.
    QStringList selection() const;
    QString sideA() const;
    QString sideB() const;
    QString chainFor(const QString& id) const;

    mplayer::VideoSurface* surface_ = nullptr;
    QListWidget* list_ = nullptr;
    QComboBox* layoutBox_ = nullptr;
    QLabel* canvas_ = nullptr;
    QLabel* info_ = nullptr;
    QSlider* scrub_ = nullptr;
    QSlider* split_ = nullptr;
    QPushButton* applyA_ = nullptr;
    QPushButton* applyB_ = nullptr;

    QImage frame_;
    QStringList candidates_;
    QString appliedId_;
    /// Frozen position, in seconds, so both sides render the same frame.
    double position_ = 0.0;
    /// 0..100 across the image, for the split layout only.
    int splitAt_ = 500;
};

}  // namespace shell
}  // namespace mplayer
