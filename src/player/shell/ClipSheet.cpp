#include "ClipSheet.hpp"

#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"

#include <QButtonGroup>
#include <QIcon>
#include <QEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QResizeEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyleHints>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mplayer::shell {
namespace {

class ClipSheetTranslation {
    Q_DECLARE_TR_FUNCTIONS(mplayer::shell::ClipSheet)
};

/// The reference layout's own scale: a 64 px strip, a 56 px action row, and an
/// 8-based gutter. Every number below is one of those three steps.
constexpr int kStripHeight = 64;
constexpr int kRowHeight = 56;
constexpr int kGutter = 10;
constexpr int kEdgeGutter = 12;
constexpr int kHandleWidth = 11;
constexpr int kHandleHeight = 34;
constexpr double kHandleGrab = 13.0;
constexpr double kMinimumSpan = 0.05;
constexpr int kHeaderHeight = 22;
constexpr int kStatusHeight = 18;

QColor surfaceBed() { return QColor(0x10, 0x11, 0x17); }
QColor textMuted() { return QColor(0xa1, 0xa1, 0xaa); }

/// Locale decimal separator, because the reference sheet prints "30,0 s" in
/// Ukrainian and a hard-coded dot reads as a different number there.
QString decimal(double value, int places) {
    return QLocale().toString(value, 'f', places);
}

QString formatSpan(double seconds) {
    return ClipSheetTranslation::tr("%1 s").arg(decimal(seconds, 1));
}

/// The exporter's cap is a round number: "60 s", never "60,0 s".
QString formatCap(double seconds) {
    return ClipSheetTranslation::tr("%1 s").arg(decimal(seconds, 0));
}

QString formatClock(double seconds) {
    const double clamped = std::max(0.0, seconds);
    const int total = int(clamped);
    const int tenths = int(std::lround((clamped - total) * 10.0)) % 10;
    const int hours = total / 3600;
    const int minutes = (total / 60) % 60;
    const int secs = total % 60;
    const QString head = hours > 0 ? QStringLiteral("%1:%2:%3")
                                         .arg(hours)
                                         .arg(minutes, 2, 10, QLatin1Char('0'))
                                         .arg(secs, 2, 10, QLatin1Char('0'))
                                   : QStringLiteral("%1:%2").arg(minutes).arg(secs, 2, 10, QLatin1Char('0'));
    // The separator is the locale's: "5:11,1" in Ukrainian, "5:11.1" in English.
    const QString dot = QLocale().decimalPoint();
    return QStringLiteral("%1%2%3").arg(head, dot).arg(tenths);
}

QIcon glyphIcon(void (*painter)(QPainter&, qreal, qreal, qreal, const QColor&), int size,
                const QColor& colour) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    {
        QPainter p(&pixmap);
        enableChromeAntialiasing(p);
        painter(p, 0.0, 0.0, size, colour);
    }
    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    pixmap.setDevicePixelRatio(dpr);
    return QIcon(pixmap);
}

QString sheetStyleSheet() {
    const QColor acc = accent();
    const QString accHex = acc.name(QColor::HexRgb);
    const QString accHover = acc.lighter(115).name(QColor::HexRgb);
    const QString inkHex = inkOn(acc).name(QColor::HexRgb);
    const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;
    const QString radiusPx = QString::number(radius + 4.0, 'f', 1);
    const QString pill = QString::number(radius + 4.0, 'f', 1);
    const QString accRgb = QStringLiteral("%1, %2, %3").arg(acc.red()).arg(acc.green()).arg(acc.blue());
    return QStringLiteral(
               "QLabel#clipSheetWindow { color: #71717a; font-size: 11px; font-weight: 600;"
               "  letter-spacing: 0.3px; }"
               "QLabel#clipSheetTitle { color: #f4f4f5; font-size: 15px; font-weight: 600; }"
               "QLabel#clipSheetSub { color: #a1a1aa; font-size: 12px; }"
               "QLabel#clipSheetStatus { color: #a1a1aa; font-size: 11px; }"
               "QPushButton#clipPreset {"
               "  background: rgba(255, 255, 255, 0.05); color: #d4d4d8;"
               "  border: 1px solid rgba(255, 255, 255, 0.10); border-radius: %1px;"
               "  padding: 0px 9px; font-size: 11px; font-weight: 600; min-height: 20px;"
               "  max-height: 20px; }"
               "QPushButton#clipPreset:hover { border-color: %2; color: #ffffff; }"
               "QPushButton#clipBoundary {"
               "  background: transparent; border: 1px solid transparent; color: #d4d4d8;"
               "  border-radius: %1px; padding: 0px 6px; min-height: 20px; max-height: 20px;"
               "  font-size: 11px; font-weight: 600; }"
               "QPushButton#clipBoundary:hover { background: rgba(255, 255, 255, 0.07);"
               "  border-color: rgba(255, 255, 255, 0.14); }"
               "QPushButton#clipChip {"
               "  background: rgba(255, 255, 255, 0.06); color: #d4d4d8;"
               "  border: 1px solid rgba(255, 255, 255, 0.10); border-radius: %1px;"
               "  padding: 0px 14px; min-height: 32px; max-height: 32px;"
               "  font-size: 12px; font-weight: 600; }"
               "QPushButton#clipChip:hover { background: rgba(255, 255, 255, 0.10); color: #ffffff; }"
               "QPushButton#clipChip:checked { background: #e9e9ee; color: #16171d;"
               "  border-color: #e9e9ee; }"
               "QPushButton#clipChip:disabled { color: rgba(255, 255, 255, 0.30); }"
               "QPushButton#clipDownload {"
               "  background: %2; color: %3; border: 1px solid %2;"
               "  border-radius: %4px; padding: 0px 18px; min-height: 40px; max-height: 40px;"
               "  font-size: 13px; font-weight: 600; }"
               "QPushButton#clipDownload:hover { background: %5; border-color: %5; }"
               "QPushButton#clipDownload:disabled { background: rgba(255, 255, 255, 0.07);"
               "  border-color: transparent; color: rgba(255, 255, 255, 0.32); }"
               "QPushButton#clipSheetClose { background: transparent; border: none; }"
               "QPushButton#clipSheetClose:hover { background: rgba(255, 255, 255, 0.08);"
               "  border-radius: %4px; }"
               "QPushButton#clipPlay { background: rgba(255, 255, 255, 0.08);"
               "  border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 22px; }"
               "QPushButton#clipPlay:hover { background: rgba(%6, 0.20); border-color: %2; }")
        .arg(radiusPx, accHex, inkHex, pill, accHover, accRgb);
}

}  // namespace

/// The rail itself. It owns every drag gesture and no policy beyond the window
/// and the maximum span: what a change *means* is the dialog's business.
class ClipSheet::Filmstrip final : public QWidget {
public:
    explicit Filmstrip(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("clipFilmstrip"));
        setAccessibleName(ClipSheetTranslation::tr("Clip range"));
        setAccessibleDescription(ClipSheetTranslation::tr(
            "Drag the handles to set the clip start and end, or drag the selection to move it."));
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(kStripHeight);
        setMaximumHeight(kStripHeight);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }

    void setWindow(double start, double end) {
        windowStart_ = std::max(0.0, start);
        windowEnd_ = std::max(windowStart_ + 0.001, end);
        update();
    }
    void setSelection(double start, double end) {
        start_ = std::clamp(start, windowStart_, windowEnd_);
        end_ = std::clamp(end, start_ + kMinimumSpan, windowEnd_);
        if (end_ - start_ > maximumSpan_) {
            end_ = std::min(windowEnd_, start_ + maximumSpan_);
            start_ = std::max(windowStart_, end_ - maximumSpan_);
        }
        update();
    }
    void setMaximumSpan(double seconds) {
        maximumSpan_ = std::max(kMinimumSpan, seconds);
        setSelection(start_, end_);
    }
    void setPlayhead(double seconds) {
        if (qFuzzyCompare(playhead_ + 1.0, seconds + 1.0)) return;
        playhead_ = seconds;
        update();
    }
    void setTile(int index, const QPixmap& tile) {
        if (index < 0 || tile.isNull()) return;
        // A caller that fills slots before the first layout pass must not lose
        // them: the vector grows to the slot it is addressing.
        if (index >= tiles_.size()) tiles_.resize(index + 1);
        tiles_[index] = tile;
        update();
    }
    void setTiles(const QVector<QPixmap>& tiles) {
        tiles_ = tiles;
        update();
    }
    int tileCount() const {
        const qreal usable = std::max<qreal>(1.0, width());
        const int byWidth = int(std::ceil(usable / std::max(1.0, tileWidth())));
        return std::clamp(byWidth, 1, 40);
    }
    QVector<int> pendingSlots() const {
        QVector<int> missing;
        for (int i = 0; i < tiles_.size(); ++i) {
            if (tiles_.at(i).isNull()) missing.append(i);
        }
        return missing;
    }
    double start() const { return start_; }
    double end() const { return end_; }

    std::function<void(double, double)> changed;
    std::function<void(double)> seek;
    std::function<void(int)> tilesNeeded;

    void resizeEvent(QResizeEvent*) override {
        const int want = tileCount();
        if (tiles_.size() == want) return;
        const QVector<QPixmap> previous = tiles_;
        tiles_ = QVector<QPixmap>(want);
        for (int i = 0; i < int(std::min<qsizetype>(previous.size(), want)); ++i) {
            tiles_[i] = previous.at(i);
        }
        if (tilesNeeded) tilesNeeded(want);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        enableChromeAntialiasing(p);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRectF bed = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = std::min<qreal>(10.0, bed.height() / 2.0);

        QPainterPath clip;
        clip.addRoundedRect(bed, radius, radius);
        p.save();
        p.setClipPath(clip);
        p.fillRect(bed, surfaceBed());

        // Frames, edge to edge. A slot without a picture stays a dark plate:
        // a filmstrip that invents frames is worse than one that is still
        // loading.
        const int slotCount = std::max(1, int(tiles_.size()));
        const qreal slotWidth = bed.width() / slotCount;
        for (int i = 0; i < slotCount; ++i) {
            const QRectF slot(bed.left() + i * slotWidth, bed.top(), slotWidth + 0.5, bed.height());
            const QPixmap& tile = i < tiles_.size() ? tiles_.at(i) : QPixmap();
            if (!tile.isNull()) {
                const QPixmap scaled = tile.scaled(slot.size().toSize(), Qt::KeepAspectRatioByExpanding,
                                                   Qt::SmoothTransformation);
                const QPointF origin(slot.center().x() - scaled.width() / 2.0,
                                     slot.center().y() - scaled.height() / 2.0);
                p.drawPixmap(origin, scaled);
            } else {
                QLinearGradient plate(slot.topLeft(), slot.bottomLeft());
                plate.setColorAt(0.0, QColor(0x18, 0x1a, 0x22));
                plate.setColorAt(1.0, QColor(0x12, 0x13, 0x19));
                p.fillRect(slot, plate);
            }
        }

        const double span = std::max(0.001, windowEnd_ - windowStart_);
        const auto xOf = [&](double value) {
            return bed.left() + bed.width() * std::clamp((value - windowStart_) / span, 0.0, 1.0);
        };
        const qreal a = xOf(start_);
        const qreal b = xOf(end_);

        // Everything outside the selection is dimmed, which is what makes the
        // selected window read as the subject of the sheet.
        p.fillRect(QRectF(bed.left(), bed.top(), a - bed.left(), bed.height()),
                   QColor(0, 0, 0, 128));
        p.fillRect(QRectF(b, bed.top(), bed.right() - b, bed.height()), QColor(0, 0, 0, 128));

        const QColor acc = accent();
        QColor tint = acc;
        tint.setAlpha(46);
        p.fillRect(QRectF(a, bed.top(), std::max<qreal>(0.0, b - a), bed.height()), tint);

        // Playhead first, so the handles stay legible above it.
        if (playhead_ >= windowStart_ && playhead_ <= windowEnd_) {
            const qreal px = xOf(playhead_);
            p.setPen(QPen(QColor(255, 255, 255, 235), 2.0));
            p.drawLine(QPointF(px, bed.top() + 1.0), QPointF(px, bed.bottom() - 1.0));
        }
        p.restore();

        // Selection frame.
        p.setPen(QPen(acc, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(a, bed.top() + 1.0, std::max<qreal>(1.0, b - a), bed.height() - 2.0),
                          radius - 2.0, radius - 2.0);

        // Bracket handles: white plates with two tactile grip lines.
        const auto drawHandle = [&](qreal x, bool isEnd, bool active, bool hot) {
            // Kept inside the rail: a selection at the very end of the window
            // would otherwise draw its plate over the rounded corner.
            const qreal raw = isEnd ? x + 3.0 : x - kHandleWidth - 3.0;
            const qreal hx = std::clamp(raw, bed.left() + 1.0,
                                        bed.right() - kHandleWidth - 1.0);
            const QRectF plate(hx, (height() - kHandleHeight) / 2.0, kHandleWidth, kHandleHeight);
            if (hot || active) {
                QColor halo = acc;
                halo.setAlpha(active ? 150 : 90);
                p.setPen(QPen(halo, 2.0));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(plate.adjusted(-2.5, -2.5, 2.5, 2.5), 6.0, 6.0);
            }
            p.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
            p.setBrush(QColor(0xf1, 0xf2, 0xf6));
            p.drawRoundedRect(plate, 4.0, 4.0);
            p.setPen(QPen(QColor(0x3a, 0x3d, 0x4a), 1.2));
            const qreal mx = plate.center().x();
            p.drawLine(QPointF(mx - 1.6, plate.top() + 9.0), QPointF(mx - 1.6, plate.bottom() - 9.0));
            p.drawLine(QPointF(mx + 1.6, plate.top() + 9.0), QPointF(mx + 1.6, plate.bottom() - 9.0));
        };
        drawHandle(a, false, active_ == Drag::Start, hot_ == Drag::Start);
        drawHandle(b, true, active_ == Drag::End, hot_ == Drag::End);

        // Inner edge, so the strip keeps a defined boundary on any frame.
        p.setPen(QPen(QColor(255, 255, 255, 16), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(bed, radius, radius);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        setFocus(Qt::MouseFocusReason);
        const double seconds = valueAt(event->position().x());
        const qreal a = xOf(start_);
        const qreal b = xOf(end_);
        const qreal mx = event->position().x();
        const double span = end_ - start_;

        if (std::abs(mx - a) <= kHandleGrab) {
            drag_ = Drag::Start;
        } else if (std::abs(mx - b) <= kHandleGrab) {
            drag_ = Drag::End;
        } else if (mx > a && mx < b && span > 0.0) {
            drag_ = Drag::Span;
            grabOffset_ = seconds - start_;
            setCursor(Qt::ClosedHandCursor);
        } else {
            // Outside the selection a click moves the preview; the sheet does
            // not silently retarget the clip the user already framed.
            drag_ = Drag::None;
            if (seek) seek(seconds);
            update();
            event->accept();
            return;
        }
        active_ = drag_;
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (drag_ == Drag::None) {
            const qreal a = xOf(start_);
            const qreal b = xOf(end_);
            const qreal mx = event->position().x();
            Drag hot = Drag::None;
            if (std::abs(mx - a) <= kHandleGrab) hot = Drag::Start;
            else if (std::abs(mx - b) <= kHandleGrab) hot = Drag::End;
            else if (mx > a && mx < b) hot = Drag::Span;
            if (hot != hot_) {
                hot_ = hot;
                setCursor(hot_ == Drag::None ? Qt::PointingHandCursor
                                             : hot_ == Drag::Span ? Qt::OpenHandCursor
                                                                  : Qt::SizeHorCursor);
                update();
            }
            QWidget::mouseMoveEvent(event);
            return;
        }
        const double seconds = valueAt(event->position().x());
        if (drag_ == Drag::Start) {
            const double lower = std::max(windowStart_, end_ - maximumSpan_);
            start_ = std::clamp(seconds, lower, end_ - kMinimumSpan);
        } else if (drag_ == Drag::End) {
            const double upper = std::min(windowEnd_, start_ + maximumSpan_);
            end_ = std::clamp(seconds, start_ + kMinimumSpan, std::max(start_ + kMinimumSpan, upper));
        } else {
            slide(seconds - grabOffset_);
        }
        update();
        if (changed) changed(start_, end_);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        drag_ = Drag::None;
        setCursor(Qt::PointingHandCursor);
        update();
        QWidget::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hot_ = Drag::None;
        update();
        QWidget::leaveEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
            QWidget::keyPressEvent(event);
            return;
        }
        const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 1.0 : 0.1;
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            const double delta = event->key() == Qt::Key_Left ? -step : step;
            if (active_ == Drag::Start || active_ == Drag::None) {
                const double lower = std::max(windowStart_, end_ - maximumSpan_);
                start_ = std::clamp(start_ + delta, lower, end_ - kMinimumSpan);
            } else {
                const double upper = std::min(windowEnd_, start_ + maximumSpan_);
                end_ = std::clamp(end_ + delta, start_ + kMinimumSpan,
                                  std::max(start_ + kMinimumSpan, upper));
            }
            update();
            if (changed) changed(start_, end_);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    enum class Drag { None, Start, End, Span };

    qreal xOf(double value) const {
        const double span = std::max(0.001, windowEnd_ - windowStart_);
        return width() * std::clamp((value - windowStart_) / span, 0.0, 1.0);
    }

    double valueAt(qreal x) const {
        const double span = std::max(0.001, windowEnd_ - windowStart_);
        const double fraction = std::clamp<qreal>(x / std::max<qreal>(1.0, width()), 0.0, 1.0);
        return windowStart_ + span * fraction;
    }

    qreal tileWidth() const { return std::max<qreal>(48.0, height() * 16.0 / 9.0); }

    void slide(double newStart) {
        const double span = end_ - start_;
        newStart = std::clamp(newStart, windowStart_, std::max(windowStart_, windowEnd_ - span));
        start_ = newStart;
        end_ = newStart + span;
    }

    QVector<QPixmap> tiles_;
    double windowStart_ = 0.0;
    double windowEnd_ = 1.0;
    double start_ = 0.0;
    double end_ = 1.0;
    double playhead_ = -1.0;
    double maximumSpan_ = 60.0;
    double grabOffset_ = 0.0;
    Drag drag_ = Drag::None;
    Drag hot_ = Drag::None;
    Drag active_ = Drag::None;
};

class ClipSheet::Private final : public QObject {
public:
    Private(ClipSheet* owner) : QObject(owner), sheet(owner) {}

    void build() {
        sheet->setObjectName(QStringLiteral("clipSheet"));
        sheet->setAttribute(Qt::WA_StyledBackground, false);

        auto* root = new QVBoxLayout(sheet);
        root->setContentsMargins(kEdgeGutter, kEdgeGutter, kEdgeGutter, kGutter);
        root->setSpacing(6);

        auto* header = new QHBoxLayout;
        header->setContentsMargins(2, 0, 2, 0);
        header->setSpacing(6);
        windowLabel = new QLabel(sheet);
        windowLabel->setObjectName(QStringLiteral("clipSheetWindow"));
        header->addWidget(windowLabel);
        header->addStretch(1);

        boundaryIn = new QPushButton(ClipSheetTranslation::tr("In"), sheet);
        boundaryIn->setObjectName(QStringLiteral("clipBoundary"));
        boundaryOut = new QPushButton(ClipSheetTranslation::tr("Out"), sheet);
        boundaryOut->setObjectName(QStringLiteral("clipBoundary"));
        header->addWidget(boundaryIn);
        header->addWidget(boundaryOut);

        const int presets[] = {5, 10, 15, 30, 60};
        for (int seconds : presets) {
            auto* preset = new QPushButton(QStringLiteral("%1s").arg(seconds), sheet);
            preset->setObjectName(QStringLiteral("clipPreset"));
            preset->setCursor(Qt::PointingHandCursor);
            connect(preset, &QPushButton::clicked, this, [this, seconds] {
                emit sheet->durationPresetRequested(seconds);
            });
            header->addWidget(preset);
            presetButtons.append(preset);
        }
        root->addLayout(header);

        strip = new Filmstrip(sheet);
        strip->setMinimumWidth(520);
        strip->changed = [this](double a, double b) {
            selection = {a, b};
            updateReadout();
            emit sheet->selectionChanged(a, b);
        };
        strip->seek = [this](double seconds) { emit sheet->seekRequested(seconds); };
        strip->tilesNeeded = [this](int count) { emit sheet->tilesNeeded(count); };
        root->addWidget(strip);

        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 2, 0, 0);
        row->setSpacing(kGutter);

        play = new IconButton(IconPainter::play, ClipSheetTranslation::tr("Play preview"), 44, 18, sheet);
        play->setObjectName(QStringLiteral("clipPlay"));
        play->setFixedSize(44, 44);
        connect(play, &QPushButton::clicked, sheet, [this] { emit sheet->playToggled(); });
        row->addWidget(play);

        auto* readout = new QVBoxLayout;
        readout->setContentsMargins(0, 0, 0, 0);
        readout->setSpacing(1);
        title = new QLabel(sheet);
        title->setObjectName(QStringLiteral("clipSheetTitle"));
        subtitle = new QLabel(sheet);
        subtitle->setObjectName(QStringLiteral("clipSheetSub"));
        readout->addWidget(title);
        readout->addWidget(subtitle);
        row->addLayout(readout);
        row->addStretch(1);

        qualityGroup = new QButtonGroup(sheet);
        qualityGroup->setExclusive(true);
        qualityRow = new QHBoxLayout;
        qualityRow->setContentsMargins(0, 0, 0, 0);
        qualityRow->setSpacing(6);
        row->addLayout(qualityRow);

        download = new QPushButton(sheet);
        download->setObjectName(QStringLiteral("clipDownload"));
        download->setCursor(Qt::PointingHandCursor);
        download->setIcon(glyphIcon(&IconPainter::download, 16, inkOn(accent())));
        download->setIconSize(QSize(16, 16));
        connect(download, &QPushButton::clicked, sheet, [this] { emit sheet->downloadRequested(); });
        row->addWidget(download);

        close = new QPushButton(sheet);
        close->setObjectName(QStringLiteral("clipSheetClose"));
        close->setCursor(Qt::PointingHandCursor);
        close->setFixedSize(32, 32);
        close->setIcon(glyphIcon(&IconPainter::close, 14, textMuted()));
        close->setIconSize(QSize(14, 14));
        connect(close, &QPushButton::clicked, sheet, [this] { emit sheet->closeRequested(); });
        row->addWidget(close);
        root->addLayout(row);

        status = new QLabel(sheet);
        status->setObjectName(QStringLiteral("clipSheetStatus"));
        status->setWordWrap(false);
        root->addWidget(status);

        connect(boundaryIn, &QPushButton::clicked, sheet,
                [this] { emit sheet->boundaryFromPlayheadRequested(false); });
        connect(boundaryOut, &QPushButton::clicked, sheet,
                [this] { emit sheet->boundaryFromPlayheadRequested(true); });

        connect(&accentNotifier(), &AccentNotifier::changed, sheet, [this] {
            restyle();
            emit sheet->restyleRequested();
        });
        restyle();
        retranslate();
    }

    void restyle() {
        sheet->setStyleSheet(sheetStyleSheet());
        if (download != nullptr) {
            download->setIcon(glyphIcon(&IconPainter::download, 16, inkOn(accent())));
        }
        if (close != nullptr) close->setIcon(glyphIcon(&IconPainter::close, 14, textMuted()));
        strip->update();
        sheet->updateGeometry();
    }

    void retranslate() {
        strip->setAccessibleName(ClipSheetTranslation::tr("Clip range"));
        strip->setAccessibleDescription(ClipSheetTranslation::tr(
            "Drag the handles to set the clip start and end, or drag the selection to move it."));
        boundaryIn->setToolTip(ClipSheetTranslation::tr("Set the clip start to the playhead ([)"));
        boundaryOut->setToolTip(ClipSheetTranslation::tr("Set the clip end to the playhead (])"));
        for (QPushButton* preset : presetButtons) {
            preset->setToolTip(ClipSheetTranslation::tr("Make the clip %1 seconds long")
                                   .arg(preset->text().chopped(1)));
        }
        close->setToolTip(ClipSheetTranslation::tr("Close"));
        play->setToolTip(playing ? ClipSheetTranslation::tr("Pause preview")
                                 : ClipSheetTranslation::tr("Play preview"));
        for (QPushButton* chip : chips) {
            chip->setToolTip(ClipSheetTranslation::tr("Export at %1").arg(chip->text()));
        }
        updateWindowLabel();
        updateReadout();
    }

    void updateWindowLabel() {
        if (windowEnd <= windowStart) {
            windowLabel->clear();
            return;
        }
        windowLabel->setText(ClipSheetTranslation::tr("Available %1 – %2")
                                 .arg(formatClock(windowStart), formatClock(windowEnd)));
    }

    void updateReadout() {
        if (title == nullptr) return;
        const double length = selection.length();
        // "30,0 s of 60": what is selected, against the exporter's own limit.
        title->setText(ClipSheetTranslation::tr("%1 of %2")
                           .arg(formatSpan(length), formatCap(maximumSpan)));
        subtitle->setText(QStringLiteral("%1 – %2")
                              .arg(formatClock(selection.start), formatClock(selection.end)));
    }

    void setQualityOptions(const QVector<int>& heights, int current) {
        for (QPushButton* chip : chips) {
            qualityGroup->removeButton(chip);
            qualityRow->removeWidget(chip);
            chip->deleteLater();
        }
        chips.clear();
        for (int height : heights) {
            auto* chip = new QPushButton(QStringLiteral("%1p").arg(height), sheet);
            chip->setObjectName(QStringLiteral("clipChip"));
            chip->setCheckable(true);
            chip->setCursor(Qt::PointingHandCursor);
            chip->setChecked(height == current);
            qualityGroup->addButton(chip, height);
            qualityRow->addWidget(chip);
            chips.append(chip);
        }
        connect(qualityGroup, &QButtonGroup::idClicked, sheet, [this](int id) {
            emit sheet->qualityChanged(id);
        });
    }

    ClipSheet* sheet = nullptr;
    Filmstrip* strip = nullptr;
    QLabel* windowLabel = nullptr;
    QLabel* title = nullptr;
    QLabel* subtitle = nullptr;
    QLabel* status = nullptr;
    IconButton* play = nullptr;
    QPushButton* download = nullptr;
    QPushButton* close = nullptr;
    QPushButton* boundaryIn = nullptr;
    QPushButton* boundaryOut = nullptr;
    QButtonGroup* qualityGroup = nullptr;
    QHBoxLayout* qualityRow = nullptr;
    QVector<QPushButton*> chips;
    QVector<QPushButton*> presetButtons;
    ClipSheet::Range selection;
    double windowStart = 0.0;
    double windowEnd = 0.0;
    double maximumSpan = 60.0;
    double progress = -1.0;
    bool playing = false;
    bool exporting = false;
};

ClipSheet::ClipSheet(QWidget* parent) : QWidget(parent), d_(new Private(this)) { d_->build(); }

ClipSheet::~ClipSheet() = default;

void ClipSheet::setWindow(double windowStart, double windowEnd) {
    d_->windowStart = std::max(0.0, windowStart);
    d_->windowEnd = std::max(d_->windowStart + 0.001, windowEnd);
    d_->strip->setWindow(d_->windowStart, d_->windowEnd);
    d_->updateWindowLabel();
}

void ClipSheet::setPlayhead(double seconds) { d_->strip->setPlayhead(seconds); }

void ClipSheet::setSelection(double start, double end) {
    d_->selection = {std::clamp(start, d_->windowStart, d_->windowEnd),
                     std::clamp(end, d_->windowStart, d_->windowEnd)};
    d_->strip->setSelection(d_->selection.start, d_->selection.end);
    d_->updateReadout();
}

void ClipSheet::setMaximumSpan(double seconds) {
    d_->maximumSpan = std::max(0.05, seconds);
    d_->strip->setMaximumSpan(d_->maximumSpan);
    d_->updateReadout();
}

ClipSheet::Range ClipSheet::selection() const { return d_->selection; }

int ClipSheet::tileCount() const { return d_->strip->tileCount(); }

void ClipSheet::setTile(int index, const QPixmap& tile) { d_->strip->setTile(index, tile); }

void ClipSheet::setTiles(const QVector<QPixmap>& tiles) { d_->strip->setTiles(tiles); }

void ClipSheet::setQualityOptions(const QVector<int>& heights, int current) {
    d_->setQualityOptions(heights, current);
}

int ClipSheet::quality() const { return d_->qualityGroup->checkedId(); }

void ClipSheet::setQualityEnabled(bool enabled) {
    for (QPushButton* chip : d_->chips) chip->setEnabled(enabled);
}

void ClipSheet::setDownloadLabel(const QString& text, bool enabled) {
    d_->download->setText(text);
    d_->download->setEnabled(enabled);
}

void ClipSheet::setExporting(bool exporting) {
    d_->exporting = exporting;
    d_->strip->setEnabled(!exporting);
    for (QPushButton* preset : d_->presetButtons) preset->setEnabled(!exporting);
    d_->download->setEnabled(!exporting && d_->download->isEnabled());
    update();
}

void ClipSheet::setProgress(double fraction) {
    d_->progress = fraction;
    update();
}

void ClipSheet::setStatus(const QString& text) { d_->status->setText(text); }

void ClipSheet::setPlaying(bool playing) {
    d_->playing = playing;
    d_->play->setIcon(playing ? IconPainter::pause : IconPainter::play);
    d_->play->setToolTip(playing ? ClipSheetTranslation::tr("Pause preview")
                                 : ClipSheetTranslation::tr("Play preview"));
}

void ClipSheet::setPlayEnabled(bool enabled) { d_->play->setEnabled(enabled); }

void ClipSheet::setEnabledForSource(bool enabled) {
    d_->strip->setEnabled(enabled);
    for (QPushButton* preset : d_->presetButtons) preset->setEnabled(enabled);
    d_->boundaryIn->setEnabled(enabled);
    d_->boundaryOut->setEnabled(enabled);
    setQualityEnabled(enabled);
    if (!enabled) d_->download->setEnabled(false);
    setPlayEnabled(enabled);
}

QSize ClipSheet::sizeHint() const {
    return QSize(720, kHeaderHeight + kStripHeight + kRowHeight + kStatusHeight + 34);
}

QSize ClipSheet::minimumSizeHint() const { return QSize(560, sizeHint().height()); }

void ClipSheet::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && d_ != nullptr) d_->retranslate();
    QWidget::changeEvent(event);
}

void ClipSheet::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // The presets are the first thing to go when the sheet is narrow: the
    // selection, the estimate and the action are what a clip needs.
    const bool roomForPresets = width() >= 700;
    if (d_ != nullptr) {
        for (QPushButton* preset : d_->presetButtons) preset->setVisible(roomForPresets);
    }
}

void ClipSheet::paintEvent(QPaintEvent*) {
    QPainter p(this);
    enableChromeAntialiasing(p);
    const double radius = cornerRadius() > 0.0 ? cornerRadius() + 4.0 : 12.0;
    p.setPen(QPen(QColor(255, 255, 255, 22), 1.0));
    p.setBrush(QColor(0x0d, 0x0e, 0x13));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    if (d_ != nullptr && d_->progress >= 0.0) {
        const QRectF track(kEdgeGutter, height() - 5.0, width() - 2 * kEdgeGutter, 3.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 22));
        p.drawRoundedRect(track, 1.5, 1.5);
        QRectF filled = track;
        filled.setWidth(track.width() * std::clamp(d_->progress, 0.0, 1.0));
        p.setBrush(accent());
        p.drawRoundedRect(filled, 1.5, 1.5);
    }
}

}  // namespace mplayer::shell
