#pragma once

#include <QPixmap>
#include <QVector>
#include <QWidget>

#include <optional>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QTimer;
QT_END_NAMESPACE

namespace mplayer::shell {

class IconButton;

/// The clip sheet: a filmstrip of the available window with a draggable
/// selection, and the one row that acts on it - play, what is selected, the
/// quality chips, the download action with its size, and close.
///
/// The widget owns no player and no export: it is a view plus the drag maths,
/// so the same sheet can be rendered by a probe without a decoder. The window it
/// shows is a *slice* of the source (`setWindow`), because a two-hour episode
/// cannot be scrubbed to the frame on one rail and the dialog only ever acts
/// inside that slice.
class ClipSheet final : public QWidget {
    Q_OBJECT

public:
    struct Range {
        double start = 0.0;
        double end = 0.0;
        double length() const { return end - start; }
    };

    explicit ClipSheet(QWidget* parent = nullptr);
    ~ClipSheet() override;

    /// `windowStart`/`windowEnd` bound everything else: the strip maps this
    /// slice to its width and a selection outside it is clamped into it.
    void setWindow(double windowStart, double windowEnd);
    void setPlayhead(double seconds);
    Range selection() const;
    /// Emits nothing; the caller decides what a programmatic change means.
    void setSelection(double start, double end);
    /// Hard cap on the dragged length, in seconds. The dialog owns the policy
    /// (the exporter's limit); the sheet only refuses to draw a longer one.
    void setMaximumSpan(double seconds);

    /// Slot count is fixed by the strip width; `index` < 0 or a null pixmap
    /// leaves that slot as a dark placeholder rather than a fake frame.
    int tileCount() const;
    void setTile(int index, const QPixmap& tile);
    void setTiles(const QVector<QPixmap>& tiles);

    /// Heights offered as chips, in the order shown, and the active one.
    void setQualityOptions(const QVector<int>& heights, int current);
    int quality() const;
    void setQualityEnabled(bool enabled);

    /// The primary action's label carries the estimate, because the size is the
    /// number a person decides on; `enabled` is false while exporting or when
    /// there is nothing valid to save.
    void setDownloadLabel(const QString& text, bool enabled);
    void setExporting(bool exporting);
    /// 0..1 while exporting; negative hides the meter.
    void setProgress(double fraction);
    /// The line under the sheet: what the export is doing, or why it cannot.
    void setStatus(const QString& text);

    void setPlaying(bool playing);
    void setPlayEnabled(bool enabled);
    void setEnabledForSource(bool enabled);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void selectionChanged(double start, double end);
    void playToggled();
    void qualityChanged(int height);
    void downloadRequested();
    /// A click on the strip away from the selection: move the preview there.
    void seekRequested(double seconds);
    /// The strip was resized and now wants this many frames. Only emitted when
    /// the count changes, so a resize storm does not flood the decoder.
    void tilesNeeded(int count);
    /// A quick preset was chosen; the sheet sends the length, the dialog clamps.
    void durationPresetRequested(int seconds);
    /// The in/out boundary should follow the playhead.
    void boundaryFromPlayheadRequested(bool end);
    /// Retranslate hook for the theme change notification.
    void restyleRequested();

protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    class Filmstrip;
    class Private;
    Private* d_ = nullptr;
};

}  // namespace mplayer::shell
