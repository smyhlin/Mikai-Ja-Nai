#include "ClipDialog.hpp"

#include "ClipExport.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "../core/MediaTracks.hpp"
#include "../core/VideoSurface.hpp"

#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QSlider>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace mplayer::shell {
namespace {

class ClipTranslation {
    Q_DECLARE_TR_FUNCTIONS(mplayer::shell::ClipDialog)
};

QString clipDialogStyleSheet() {
    const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;
    const double cardRadius = radius + 4.0;
    const QString acc = accent().name(QColor::HexRgb);
    const QString ink = inkOn(accent()).name(QColor::HexRgb);
    const QString accRgb = QStringLiteral("%1, %2, %3").arg(accent().red()).arg(accent().green()).arg(accent().blue());
    const QString hoverHex = accent().lighter(115).name(QColor::HexRgb);
    return QStringLiteral(
        "QDialog#clipDialog {"
        "  background-color: #0d0e13;"
        "  color: #f4f4f5;"
        "}"
        "QFrame#clipCard {"
        "  background-color: #14161e;"
        "  border: 1px solid rgba(255, 255, 255, 0.09);"
        "  border-radius: %1px;"
        "}"
        "QLabel {"
        "  color: #a1a1aa;"
        "  background: transparent;"
        "  font-size: 13px;"
        "}"
        "QLabel#clipSectionTitle {"
        "  color: #f4f4f5;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "}"
        "QLabel#clipHeaderTag {"
        "  color: #71717a;"
        "  font-size: 11px;"
        "  font-weight: 700;"
        "  letter-spacing: 0.5px;"
        "}"
        "QLabel#clipDurationBadge {"
        "  background-color: rgba(%4, 0.18);"
        "  color: %2;"
        "  border: 1px solid rgba(%4, 0.35);"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QLabel#clipDurationBadge[valid=\"false\"] {"
        "  background-color: rgba(239, 68, 68, 0.18);"
        "  color: #ef4444;"
        "  border-color: rgba(239, 68, 68, 0.35);"
        "}"
        "QLabel#clipTimeReadout {"
        "  background-color: rgba(0, 0, 0, 0.40);"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.12);"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QLabel#clipStatus {"
        "  color: #a1a1aa;"
        "  font-size: 12px;"
        "}"
        "QComboBox#clipQuality {"
        "  background-color: #1b1e28;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.15);"
        "  border-radius: %5px;"
        "  padding: 3px 10px;"
        "  min-height: 26px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        "QComboBox#clipQuality:hover {"
        "  border-color: %2;"
        "}"
        "QComboBox#clipQuality QAbstractItemView {"
        "  background-color: #14161e;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.18);"
        "  selection-background-color: %2;"
        "  selection-color: %3;"
        "}"
        "QDoubleSpinBox {"
        "  background-color: #1b1e28;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.15);"
        "  border-radius: %5px;"
        "  padding: 3px 8px;"
        "  min-height: 26px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        "QDoubleSpinBox:focus {"
        "  border-color: %2;"
        "  background-color: #222634;"
        "}"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {"
        "  width: 16px;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QPushButton {"
        "  background-color: #1b1e28;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.15);"
        "  border-radius: %5px;"
        "  padding: 4px 12px;"
        "  min-height: 26px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        "QPushButton:hover {"
        "  background-color: #252a38;"
        "  border-color: rgba(255, 255, 255, 0.28);"
        "  color: #ffffff;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #141620;"
        "}"
        "QPushButton:checked {"
        "  background-color: rgba(%4, 0.22);"
        "  border-color: %2;"
        "  color: %2;"
        "}"
        "QPushButton#clipPresetButton {"
        "  background-color: rgba(255, 255, 255, 0.06);"
        "  color: #d4d4d8;"
        "  border: 1px solid rgba(255, 255, 255, 0.12);"
        "  border-radius: 4px;"
        "  padding: 2px 9px;"
        "  min-height: 22px;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "}"
        "QPushButton#clipPresetButton:hover {"
        "  background-color: rgba(%4, 0.22);"
        "  border-color: %2;"
        "  color: %2;"
        "}"
        "QPushButton#clipMiniAction {"
        "  background-color: rgba(255, 255, 255, 0.07);"
        "  color: #e4e4e7;"
        "  border: 1px solid rgba(255, 255, 255, 0.14);"
        "  border-radius: 4px;"
        "  padding: 3px 8px;"
        "  min-height: 24px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QPushButton#clipMiniAction:hover {"
        "  background-color: #262b3a;"
        "  border-color: %2;"
        "}"
        "QPushButton#clipExport {"
        "  background-color: %2;"
        "  color: %3;"
        "  border: 1px solid %2;"
        "  border-radius: %5px;"
        "  font-weight: 600;"
        "  min-height: 32px;"
        "  padding: 5px 22px;"
        "  font-size: 13px;"
        "}"
        "QPushButton#clipExport:hover {"
        "  background-color: %6;"
        "  border-color: %6;"
        "}"
        "QPushButton#clipExport:disabled {"
        "  background-color: rgba(255, 255, 255, 0.08);"
        "  color: rgba(255, 255, 255, 0.35);"
        "  border-color: transparent;"
        "}"
        "QProgressBar#clipProgress {"
        "  background-color: rgba(255, 255, 255, 0.10);"
        "  border-radius: 4px;"
        "  border: none;"
        "  min-height: 8px;"
        "  max-height: 8px;"
        "}"
        "QProgressBar#clipProgress::chunk {"
        "  background-color: %2;"
        "  border-radius: 4px;"
        "}"
        "QSlider#clipPreviewScrub::groove:horizontal {"
        "  height: 6px;"
        "  background: rgba(255, 255, 255, 0.15);"
        "  border-radius: 3px;"
        "}"
        "QSlider#clipPreviewScrub::sub-page:horizontal {"
        "  background: %2;"
        "  border-radius: 3px;"
        "}"
        "QSlider#clipPreviewScrub::handle:horizontal {"
        "  width: 14px;"
        "  height: 14px;"
        "  margin: -4px 0;"
        "  border-radius: 7px;"
        "  background: #ffffff;"
        "}"
        "QSlider#clipPreviewScrub::handle:horizontal:hover {"
        "  background: %2;"
        "}")
        .arg(QString::number(cardRadius, 'f', 1), acc, ink, accRgb,
             QString::number(radius, 'f', 1), hoverHex);
}

class ClipRange final : public QWidget {
public:
    explicit ClipRange(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("clipRange"));
        setAccessibleName(ClipTranslation::tr("Clip range"));
        setAccessibleDescription(ClipTranslation::tr("Use left and right arrows to adjust the selected clip range."));
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(44);
        setMinimumWidth(280);
        setMouseTracking(true);
    }

    void setRange(double duration, double start, double end) {
        duration_ = std::max(0.0, duration);
        start_ = std::clamp(start, 0.0, duration_);
        end_ = std::clamp(end, start_, duration_);
        update();
    }

    void setPlayhead(double position) {
        if (qFuzzyCompare(playhead_, position)) return;
        playhead_ = std::max(0.0, position);
        update();
    }

    double start() const { return start_; }
    double end() const { return end_; }
    void setChanged(std::function<void(double, double)> changed) { changed_ = std::move(changed); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        enableChromeAntialiasing(painter);
        const qreal h = height();
        const qreal trackHeight = 12.0;
        const qreal trackY = (h - trackHeight) / 2.0;
        const qreal left = 16.0;
        const qreal right = width() - 16.0;
        const qreal trackW = std::max<qreal>(1.0, right - left);

        // Rail background with subtle rounded border
        painter.setPen(QPen(QColor(255, 255, 255, 18), 1.0));
        painter.setBrush(QColor(18, 20, 26));
        painter.drawRoundedRect(QRectF(left, trackY, trackW, trackHeight), 6, 6);

        if (duration_ <= 0.0) return;

        // Selected interval
        const qreal a = coordinate(start_);
        const qreal b = coordinate(end_);
        const qreal spanW = std::max<qreal>(0.0, b - a);

        QLinearGradient rangeGrad(a, trackY, a, trackY + trackHeight);
        QColor accColor = accent();
        rangeGrad.setColorAt(0.0, accColor.lighter(118));
        rangeGrad.setColorAt(1.0, accColor.darker(108));
        painter.setPen(Qt::NoPen);
        painter.setBrush(rangeGrad);
        painter.drawRoundedRect(QRectF(a, trackY, spanW, trackHeight), 4, 4);

        // Current playhead needle
        if (playhead_ >= 0.0 && playhead_ <= duration_) {
            const qreal px = coordinate(playhead_);
            painter.setPen(QPen(QColor(255, 255, 255, 240), 2.0));
            painter.drawLine(QPointF(px, trackY - 5), QPointF(px, trackY + trackHeight + 5));
            painter.setBrush(QColor(255, 255, 255));
            painter.setPen(QPen(QColor(18, 19, 26), 1.0));
            painter.drawEllipse(QPointF(px, trackY - 5), 3.5, 3.5);
        }

        // Handles
        const auto drawHandle = [&](qreal x, bool isEnd, bool isActive) {
            const qreal hw = 12.0;
            const qreal hh = 26.0;
            const qreal hy = (h - hh) / 2.0;
            const qreal hx = x - (isEnd ? 2.0 : hw - 2.0);
            QRectF handleRect(hx, hy, hw, hh);

            painter.setPen(QPen(isActive ? accent() : QColor(255, 255, 255, 70), isActive ? 1.5 : 1.0));
            painter.setBrush(QColor(240, 242, 248));
            painter.drawRoundedRect(handleRect, 4, 4);

            // Vertical tactile grip marks
            painter.setPen(QPen(QColor(60, 64, 78), 1.0));
            const qreal mx = handleRect.center().x();
            painter.drawLine(QPointF(mx - 1.5, hy + 7), QPointF(mx - 1.5, hy + hh - 7));
            painter.drawLine(QPointF(mx + 1.5, hy + 7), QPointF(mx + 1.5, hy + hh - 7));

            if (isActive) {
                painter.setPen(QPen(accent(), 2.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(handleRect.adjusted(-2, -2, 2, 2), 6, 6);
            }
        };

        drawHandle(a, false, !activeEnd_);
        drawHandle(b, true, activeEnd_);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || duration_ <= 0.0) return;
        setFocus(Qt::MouseFocusReason);
        const double seconds = valueAt(event->position().x());
        const qreal sx = coordinate(start_);
        const qreal ex = coordinate(end_);
        const qreal mx = event->position().x();

        if (std::abs(mx - sx) <= 12.0) {
            dragMode_ = DragMode::Start;
            activeEnd_ = false;
        } else if (std::abs(mx - ex) <= 12.0) {
            dragMode_ = DragMode::End;
            activeEnd_ = true;
        } else if (mx > sx + 12.0 && mx < ex - 12.0) {
            dragMode_ = DragMode::Span;
            dragOffset_ = seconds - start_;
        } else {
            activeEnd_ = std::abs(seconds - end_) < std::abs(seconds - start_);
            dragMode_ = activeEnd_ ? DragMode::End : DragMode::Start;
            updateValue(seconds);
        }
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) {
            const double seconds = valueAt(event->position().x());
            if (dragMode_ == DragMode::Start) {
                updateStart(seconds);
            } else if (dragMode_ == DragMode::End) {
                updateEnd(seconds);
            } else if (dragMode_ == DragMode::Span) {
                slideSpan(seconds - dragOffset_);
            }
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        dragMode_ = DragMode::None;
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
            QWidget::keyPressEvent(event);
            return;
        }
        double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 1.0 : 0.1;
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Down) step = -step;
        else if (event->key() != Qt::Key_Right && event->key() != Qt::Key_Up) {
            QWidget::keyPressEvent(event);
            return;
        }
        updateValue((activeEnd_ ? end_ : start_) + step);
        event->accept();
    }

private:
    enum class DragMode { None, Start, End, Span };

    qreal coordinate(double value) const {
        constexpr qreal inset = 16.0;
        return inset + (width() - 2.0 * inset) * (duration_ > 0.0 ? value / duration_ : 0.0);
    }

    double valueAt(qreal x) const {
        constexpr qreal inset = 16.0;
        return duration_ * std::clamp((x - inset) / std::max<qreal>(1.0, width() - 2.0 * inset), 0.0, 1.0);
    }

    void updateStart(double val) {
        val = std::clamp(val, std::max(0.0, end_ - kMaximumClipDuration), end_ - 0.001);
        start_ = val;
        update();
        if (changed_) changed_(start_, end_);
    }

    void updateEnd(double val) {
        val = std::clamp(val, start_ + 0.001, std::min(duration_, start_ + kMaximumClipDuration));
        end_ = val;
        update();
        if (changed_) changed_(start_, end_);
    }

    void slideSpan(double newStart) {
        const double span = end_ - start_;
        newStart = std::clamp(newStart, 0.0, std::max(0.0, duration_ - span));
        start_ = newStart;
        end_ = newStart + span;
        update();
        if (changed_) changed_(start_, end_);
    }

    void updateValue(double value) {
        value = std::clamp(value, 0.0, duration_);
        if (activeEnd_) end_ = std::clamp(value, start_, std::min(duration_, start_ + kMaximumClipDuration));
        else start_ = std::clamp(value, std::max(0.0, end_ - kMaximumClipDuration), end_);
        update();
        if (changed_) changed_(start_, end_);
    }

    double duration_ = 0.0;
    double start_ = 0.0;
    double end_ = 0.0;
    double playhead_ = 0.0;
    DragMode dragMode_ = DragMode::None;
    double dragOffset_ = 0.0;
    bool activeEnd_ = false;
    std::function<void(double, double)> changed_;
};

QString formatSeconds(double value) {
    const qint64 ms = static_cast<qint64>(std::llround(std::max(0.0, value) * 1000.0));
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000, 2, 10, QLatin1Char('0'))
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QDoubleSpinBox* makeTimeSpin(QWidget* parent, const QString& name) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setDecimals(3);
    spin->setSingleStep(0.1);
    spin->setKeyboardTracking(false);
    spin->setSuffix(ClipTranslation::tr(" s"));
    spin->setMinimumWidth(110);
    spin->setAccessibleName(name == QLatin1String("clipStart") ? ClipTranslation::tr("Clip start time")
                                                               : ClipTranslation::tr("Clip end time"));
    return spin;
}

} // namespace

class ClipDialog::Private final : public QObject {
public:
    explicit Private(ClipDialog* dialog, VideoSurface* source, const QString& caption)
        : QObject(dialog), dialog(dialog), source(source), caption(caption) {
        build();
        if (!source || source->sourceUrl().isEmpty()) {
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The current video source is unavailable."));
            return;
        }
        sourceUrl = source->sourceUrl();
        connect(source, &VideoSurface::mediaInformationInvalidated, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &VideoSurface::sourceEpochChanged, this, [this](uint64_t) {
            invalidated = true;
            exportWorker.cancel();
            preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &VideoSurface::destroyed, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            if (preview) preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video player was closed."));
        });
        connect(source, &VideoSurface::durationChanged, this, [this](double seconds) {
            if (invalidated || exporting || !std::isfinite(seconds) || seconds <= 0.0) return;
            duration = seconds;
            if (!rangeInitialized) {
                initializeRange();
                return;
            }
            setTimes(start->value(), end->value(), 0);
        });
        connect(source, &VideoSurface::videoSizeChanged, this, [this](int width, int height) {
            if (invalidated || exporting || width <= 0 || height <= 0) return;
            size = QSize(width, height);
            exportButton->setEnabled(validRange());
            previewScrub->setEnabled(duration > 0.0);
        });
        connect(preview, &VideoSurface::errorOccurred, this, [this](const QString& reason) {
            if (!invalidated) {
                statusSource = QStringLiteral("Preview unavailable: %1");
                statusArgs = {reason};
                status->setText(ClipTranslation::tr("Preview unavailable: %1").arg(reason));
            }
        });
        connect(preview, &VideoSurface::durationChanged, this, [this](double seconds) {
            if (std::isfinite(seconds) && seconds > 0.0 && preview && start) {
                preview->setPaused(true);
                preview->seek(std::clamp(start->value(), 0.0, seconds));
            }
        });
        connect(source, &VideoSurface::mediaTracksChanged, this, [this] { captureTracks(); });
        connect(preview, &VideoSurface::mediaTracksChanged, this, [this] { syncPreviewAudio(); });
        connect(&exportWorker, &ClipExport::encodingStarted, this, [this](const QString& codec) {
            codecName = codec;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1…"), {codecName});
        });
        connect(&exportWorker, &ClipExport::progressChanged, this, [this](double fraction) {
            progress->setValue(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000.0));
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1… %2%"),
                      {codecName.isEmpty() ? ClipTranslation::tr("video") : codecName,
                       QString::number(progress->value() / 10.0, 'f', 1)});
        });
        connect(&exportWorker, &ClipExport::finished, this,
                [this](bool succeeded, const QString& path, const QString& error) {
            exporting = false;
            progress->setVisible(false);
            cancel->setVisible(false);
            exportButton->setEnabled(!invalidated && validRange());
            start->setEnabled(!invalidated && rangeInitialized);
            end->setEnabled(!invalidated && rangeInitialized);
            range->setEnabled(!invalidated && rangeInitialized);
            quality->setEnabled(!invalidated && rangeInitialized);
            if (!succeeded) {
                if (error.isEmpty()) setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Export failed."));
                else setExternalStatus(error);
                exportButtonState = ExportButtonState::Retry;
                retranslateUi();
                return;
            }
            completedPath = path;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Saved %1"), {QFileInfo(path).fileName()});
            exportButtonState = ExportButtonState::Another;
            retranslateUi();
            reveal->setVisible(true);
            copyPath->setVisible(true);
        });
        captureTracks();
        const QSize sourceSize(source->videoWidth(), source->videoHeight());
        if (sourceSize.isValid()) size = sourceSize;
        if (duration > 0.0) initializeRange();
        preview->setMuted(true);
        preview->open(sourceUrl);
        preview->setPaused(true);
        if (std::isfinite(source->duration()) && source->duration() > 0.0) {
            duration = source->duration();
            initializeRange();
        }
        retranslateUi();
    }

    ~Private() override { exportWorker.cancel(); }

    enum class ExportButtonState { Default, Retry, Another };

    void retranslateUi() {
        dialog->setWindowTitle(caption.trimmed().isEmpty()
                                   ? ClipTranslation::tr("Cut moment")
                                   : ClipTranslation::tr("Cut moment — %1").arg(caption));
        dialog->setStyleSheet(clipDialogStyleSheet());
        range->setAccessibleName(ClipTranslation::tr("Clip range"));
        range->setAccessibleDescription(ClipTranslation::tr("Use left and right arrows to adjust the selected clip range."));
        if (rangeTitle) rangeTitle->setText(ClipTranslation::tr("Clip range"));
        startLabel->setText(ClipTranslation::tr("Start"));
        endLabel->setText(ClipTranslation::tr("End"));
        start->setSuffix(ClipTranslation::tr(" s"));
        start->setAccessibleName(ClipTranslation::tr("Clip start time"));
        end->setSuffix(ClipTranslation::tr(" s"));
        end->setAccessibleName(ClipTranslation::tr("Clip end time"));
        qualityLabel->setText(ClipTranslation::tr("Maximum resolution"));
        quality->setItemText(0, ClipTranslation::tr("1080p"));
        quality->setItemText(1, ClipTranslation::tr("720p"));
        quality->setItemText(2, ClipTranslation::tr("480p"));
        previewScrub->setAccessibleName(ClipTranslation::tr("Scrub within selected moment"));
        updatePositionDisplay();
        if (rangeInitialized)
            durationLabel->setText(ClipTranslation::tr("%1 selected").arg(formatSeconds(end->value() - start->value())));

        playButton->setToolTip(preview && preview->isPlaying() ? ClipTranslation::tr("Pause preview")
                                                               : ClipTranslation::tr("Play preview"));
        muteButton->setToolTip(previewMuted ? ClipTranslation::tr("Preview audio on")
                                            : ClipTranslation::tr("Preview audio off"));

        btnSetStartCurrent->setToolTip(ClipTranslation::tr("Set clip start to current video position ([)"));
        btnSeekStart->setToolTip(ClipTranslation::tr("Seek preview to clip start"));
        btnSetEndCurrent->setToolTip(ClipTranslation::tr("Set clip end to current video position (])"));
        btnSeekEnd->setToolTip(ClipTranslation::tr("Seek preview to clip end"));

        exportButton->setText(exporting ? ClipTranslation::tr("Exporting…")
                                       : exportButtonState == ExportButtonState::Retry ? ClipTranslation::tr("Try again")
                                       : exportButtonState == ExportButtonState::Another ? ClipTranslation::tr("Export another")
                                                                                         : ClipTranslation::tr("Export MP4…"));
        cancel->setText(ClipTranslation::tr("Cancel export"));
        reveal->setText(ClipTranslation::tr("Show in folder"));
        copyPath->setText(ClipTranslation::tr("Copy path"));
        if (!statusSource.isEmpty()) {
            QString text = ClipTranslation::tr(statusSource.toUtf8().constData());
            for (const QString& arg : statusArgs) text = text.arg(arg);
            status->setText(text);
        }
    }

    void setStatus(const char* sourceText, std::initializer_list<QString> args = {}) {
        statusSource = QString::fromUtf8(sourceText);
        statusArgs = QStringList(args);
        QString text = ClipTranslation::tr(sourceText);
        for (const QString& arg : statusArgs) text = text.arg(arg);
        status->setText(text);
    }

    void setExternalStatus(const QString& text) {
        statusSource.clear();
        statusArgs.clear();
        status->setText(text);
    }

    bool handleKeyPress(QKeyEvent* event) {
        if (event->key() == Qt::Key_Space) {
            togglePlay();
            return true;
        }
        if (event->key() == Qt::Key_BracketLeft) {
            setStartToCurrent();
            return true;
        }
        if (event->key() == Qt::Key_BracketRight) {
            setEndToCurrent();
            return true;
        }
        if (event->key() == Qt::Key_M) {
            toggleMute();
            return true;
        }
        if (event->key() == Qt::Key_Left) {
            stepSeconds(event->modifiers().testFlag(Qt::ShiftModifier) ? -0.1 : -1.0);
            return true;
        }
        if (event->key() == Qt::Key_Right) {
            stepSeconds(event->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 1.0);
            return true;
        }
        return false;
    }

    void build() {
        dialog->setObjectName(QStringLiteral("clipDialog"));
        dialog->setWindowTitle(ClipTranslation::tr("Cut moment"));
        dialog->setModal(false);
        dialog->setMinimumWidth(720);
        dialog->setMinimumHeight(600);
        dialog->resize(760, 640);
        dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);

        const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;

        auto* root = new QVBoxLayout(dialog);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(10);

        // ── Card 1: Video Preview & Transport Bar ──────────────────────────────
        previewCard = new QFrame(dialog);
        previewCard->setObjectName(QStringLiteral("clipCard"));
        auto* previewLayout = new QVBoxLayout(previewCard);
        previewLayout->setContentsMargins(10, 10, 10, 10);
        previewLayout->setSpacing(8);

        preview = new VideoSurface(previewCard);
        preview->setObjectName(QStringLiteral("clipPreview"));
        preview->setMinimumHeight(200);
        preview->setMaximumHeight(360);
        preview->setDisplayCornerRadius(qMax(4.0, radius));
        preview->setFocusPolicy(Qt::StrongFocus);
        previewLayout->addWidget(preview, 1);

        auto* previewBar = new QHBoxLayout;
        previewBar->setContentsMargins(2, 0, 2, 0);
        previewBar->setSpacing(6);

        playButton = new IconButton(IconPainter::play, ClipTranslation::tr("Play preview"), 30, 16, previewCard);
        playButton->setObjectName(QStringLiteral("clipPlayToggle"));

        btnStepBack = new QPushButton(QStringLiteral("◀ 1s"), previewCard);
        btnStepBack->setObjectName(QStringLiteral("clipMiniAction"));
        btnStepBack->setToolTip(QStringLiteral("Step back 1 second (Left arrow)"));

        btnStepForward = new QPushButton(QStringLiteral("1s ▶"), previewCard);
        btnStepForward->setObjectName(QStringLiteral("clipMiniAction"));
        btnStepForward->setToolTip(QStringLiteral("Step forward 1 second (Right arrow)"));

        muteButton = new IconButton(IconPainter::mute, ClipTranslation::tr("Preview audio on"), 30, 16, previewCard);
        muteButton->setObjectName(QStringLiteral("clipMuteToggle"));

        previewScrub = new QSlider(Qt::Horizontal, previewCard);
        previewScrub->setObjectName(QStringLiteral("clipPreviewScrub"));
        previewScrub->setRange(0, 1000);
        previewScrub->setAccessibleName(ClipTranslation::tr("Scrub within selected moment"));
        previewScrub->setEnabled(false);

        previewTime = new QLabel(previewCard);
        previewTime->setObjectName(QStringLiteral("clipTimeReadout"));
        previewTime->setTextInteractionFlags(Qt::TextSelectableByMouse);

        previewBar->addWidget(playButton);
        previewBar->addWidget(btnStepBack);
        previewBar->addWidget(btnStepForward);
        previewBar->addWidget(muteButton);
        previewBar->addWidget(previewScrub, 1);
        previewBar->addWidget(previewTime);
        previewLayout->addLayout(previewBar);
        root->addWidget(previewCard, 1);

        // ── Card 2: Range & Quick Presets ──────────────────────────────────────
        rangeCard = new QFrame(dialog);
        rangeCard->setObjectName(QStringLiteral("clipCard"));
        auto* rangeLayout = new QVBoxLayout(rangeCard);
        rangeLayout->setContentsMargins(12, 10, 12, 10);
        rangeLayout->setSpacing(8);

        auto* rangeHeader = new QHBoxLayout;
        rangeHeader->setContentsMargins(0, 0, 0, 0);
        rangeHeader->setSpacing(8);

        rangeTitle = new QLabel(ClipTranslation::tr("Clip range"), rangeCard);
        rangeTitle->setObjectName(QStringLiteral("clipSectionTitle"));
        rangeHeader->addWidget(rangeTitle);

        // Quick duration presets
        auto* presetsRow = new QHBoxLayout;
        presetsRow->setSpacing(4);
        const auto addPreset = [this, presetsRow](int seconds) {
            auto* btn = new QPushButton(QStringLiteral("%1 s").arg(seconds), rangeCard);
            btn->setObjectName(QStringLiteral("clipPresetButton"));
            btn->setToolTip(QStringLiteral("Set clip duration to %1 seconds").arg(seconds));
            connect(btn, &QPushButton::clicked, this, [this, seconds] {
                applyDurationPreset(static_cast<double>(seconds));
            });
            presetsRow->addWidget(btn);
            presetButtons.append(btn);
        };
        addPreset(5);
        addPreset(10);
        addPreset(15);
        addPreset(30);
        addPreset(60);
        rangeHeader->addLayout(presetsRow);

        rangeHeader->addStretch(1);
        durationLabel = new QLabel(rangeCard);
        durationLabel->setObjectName(QStringLiteral("clipDurationBadge"));
        rangeHeader->addWidget(durationLabel);
        rangeLayout->addLayout(rangeHeader);

        // Timeline scrubber
        range = new ClipRange(rangeCard);
        rangeLayout->addWidget(range);

        // Precision In / Out Controls
        auto* times = new QHBoxLayout;
        times->setContentsMargins(0, 2, 0, 0);
        times->setSpacing(8);

        // IN point controls
        startLabel = new QLabel(ClipTranslation::tr("Start"), rangeCard);
        startLabel->setObjectName(QStringLiteral("clipHeaderTag"));
        times->addWidget(startLabel);
        start = makeTimeSpin(rangeCard, QStringLiteral("clipStart"));
        times->addWidget(start);

        btnSetStartCurrent = new QPushButton(QStringLiteral("⏱ ["), rangeCard);
        btnSetStartCurrent->setObjectName(QStringLiteral("clipMiniAction"));
        times->addWidget(btnSetStartCurrent);

        btnSeekStart = new QPushButton(QStringLiteral("⇤"), rangeCard);
        btnSeekStart->setObjectName(QStringLiteral("clipMiniAction"));
        times->addWidget(btnSeekStart);

        times->addSpacing(16);

        // OUT point controls
        endLabel = new QLabel(ClipTranslation::tr("End"), rangeCard);
        endLabel->setObjectName(QStringLiteral("clipHeaderTag"));
        times->addWidget(endLabel);
        end = makeTimeSpin(rangeCard, QStringLiteral("clipEnd"));
        times->addWidget(end);

        btnSetEndCurrent = new QPushButton(QStringLiteral("⏱ ]"), rangeCard);
        btnSetEndCurrent->setObjectName(QStringLiteral("clipMiniAction"));
        times->addWidget(btnSetEndCurrent);

        btnSeekEnd = new QPushButton(QStringLiteral("⇥"), rangeCard);
        btnSeekEnd->setObjectName(QStringLiteral("clipMiniAction"));
        times->addWidget(btnSeekEnd);

        times->addStretch(1);
        rangeLayout->addLayout(times);
        root->addWidget(rangeCard);

        // ── Card 3: Export Options & Actions ───────────────────────────────────
        exportCard = new QFrame(dialog);
        exportCard->setObjectName(QStringLiteral("clipCard"));
        auto* exportLayout = new QVBoxLayout(exportCard);
        exportLayout->setContentsMargins(12, 10, 12, 10);
        exportLayout->setSpacing(8);

        auto* qualityRow = new QHBoxLayout;
        qualityRow->setContentsMargins(0, 0, 0, 0);
        qualityRow->setSpacing(8);
        qualityLabel = new QLabel(exportCard);
        qualityRow->addWidget(qualityLabel);
        quality = new QComboBox(exportCard);
        quality->setObjectName(QStringLiteral("clipQuality"));
        quality->addItem(ClipTranslation::tr("1080p"), 1080);
        quality->addItem(ClipTranslation::tr("720p"), 720);
        quality->addItem(ClipTranslation::tr("480p"), 480);
        qualityRow->addWidget(quality);

        auto* formatBadge = new QLabel(QStringLiteral("MP4 · H.264 / AAC"), exportCard);
        formatBadge->setObjectName(QStringLiteral("clipHeaderTag"));
        qualityRow->addWidget(formatBadge);

        qualityRow->addSpacing(12);
        status = new QLabel(ClipTranslation::tr("Select up to 60 seconds to export as MP4."), exportCard);
        status->setObjectName(QStringLiteral("clipStatus"));
        status->setWordWrap(true);
        qualityRow->addWidget(status, 1);
        exportLayout->addLayout(qualityRow);

        progress = new QProgressBar(exportCard);
        progress->setObjectName(QStringLiteral("clipProgress"));
        progress->setRange(0, 1000);
        progress->setTextVisible(false);
        progress->setVisible(false);
        exportLayout->addWidget(progress);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(8);
        reveal = new QPushButton(ClipTranslation::tr("Show in folder"), exportCard);
        reveal->setVisible(false);
        copyPath = new QPushButton(ClipTranslation::tr("Copy path"), exportCard);
        copyPath->setVisible(false);
        actions->addWidget(reveal);
        actions->addWidget(copyPath);
        actions->addStretch(1);
        cancel = new QPushButton(ClipTranslation::tr("Cancel export"), exportCard);
        cancel->setObjectName(QStringLiteral("clipCancel"));
        cancel->setVisible(false);
        exportButton = new QPushButton(ClipTranslation::tr("Export MP4…"), exportCard);
        exportButton->setObjectName(QStringLiteral("clipExport"));
        exportButton->setDefault(true);
        actions->addWidget(cancel);
        actions->addWidget(exportButton);
        exportLayout->addLayout(actions);
        root->addWidget(exportCard);

        connect(&accentNotifier(), &AccentNotifier::changed, this, [this] {
            dialog->setStyleSheet(clipDialogStyleSheet());
            range->update();
        });

        setStatus("Select up to 60 seconds to export as MP4.");
        range->setChanged([this](double a, double b) { setTimes(a, b, false); });
        connect(start, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            if (syncing) return;
            setTimes(value, end->value(), 1);
        });
        connect(end, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            if (syncing) return;
            setTimes(start->value(), value, 2);
        });

        connect(playButton, &QPushButton::clicked, this, [this] { togglePlay(); });
        connect(muteButton, &QPushButton::clicked, this, [this] { toggleMute(); });
        connect(btnStepBack, &QPushButton::clicked, this, [this] { stepSeconds(-1.0); });
        connect(btnStepForward, &QPushButton::clicked, this, [this] { stepSeconds(1.0); });

        connect(btnSetStartCurrent, &QPushButton::clicked, this, [this] { setStartToCurrent(); });
        connect(btnSetEndCurrent, &QPushButton::clicked, this, [this] { setEndToCurrent(); });
        connect(btnSeekStart, &QPushButton::clicked, this, [this] {
            if (preview && !invalidated) preview->seek(start->value());
        });
        connect(btnSeekEnd, &QPushButton::clicked, this, [this] {
            if (preview && !invalidated) preview->seek(end->value());
        });

        connect(previewScrub, &QSlider::valueChanged, this, [this](int value) {
            if (!scrubbingPreview && preview && duration > 0.0) {
                preview->seek(start->value() + (end->value() - start->value()) * value / 1000.0);
            }
        });
        connect(preview, &VideoSurface::positionChanged, this, [this](double position) {
            if (duration > 0.0) {
                scrubbingPreview = true;
                previewScrub->setValue(static_cast<int>(std::clamp(
                    (position - start->value()) / std::max(0.001, end->value() - start->value()), 0.0, 1.0) * 1000.0));
                scrubbingPreview = false;
            }
            previewPosition = position;
            previewPositionKnown = true;
            range->setPlayhead(position);
            updatePositionDisplay();
            if (duration > 0.0 && position >= end->value()) {
                preview->setPaused(true);
                preview->seek(start->value());
                updatePlayButton(false);
            }
        });
        connect(exportButton, &QPushButton::clicked, this, [this] { chooseOutput(); });
        connect(cancel, &QPushButton::clicked, this, [this] {
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
            cancel->setEnabled(false);
            exportWorker.cancel();
        });
        connect(reveal, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(completedPath).absolutePath()));
        });
        connect(copyPath, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty()) QGuiApplication::clipboard()->setText(completedPath);
        });
        retranslateUi();
    }

    void togglePlay() {
        if (!preview || invalidated) return;
        const bool willPlay = !preview->isPlaying();
        preview->setPaused(!willPlay);
        updatePlayButton(willPlay);
    }

    void updatePlayButton(bool playing) {
        if (playButton == nullptr) return;
        playButton->setIcon(playing ? IconPainter::pause : IconPainter::play);
        playButton->setToolTip(playing ? ClipTranslation::tr("Pause preview") : ClipTranslation::tr("Play preview"));
    }

    void toggleMute() {
        if (!preview || invalidated) return;
        previewMuted = !previewMuted;
        preview->setMuted(previewMuted);
        if (muteButton != nullptr) {
            muteButton->setIcon(previewMuted ? IconPainter::mute : IconPainter::volume);
            muteButton->setToolTip(previewMuted ? ClipTranslation::tr("Preview audio on") : ClipTranslation::tr("Preview audio off"));
        }
    }

    void stepSeconds(double delta) {
        if (!preview || invalidated || duration <= 0.0) return;
        const double target = std::clamp(previewPosition + delta, 0.0, duration);
        preview->seek(target);
    }

    void setStartToCurrent() {
        if (duration <= 0.0 || invalidated || !rangeInitialized) return;
        setTimes(previewPosition, end->value(), 1);
    }

    void setEndToCurrent() {
        if (duration <= 0.0 || invalidated || !rangeInitialized) return;
        setTimes(start->value(), previewPosition, 2);
    }

    void applyDurationPreset(double seconds) {
        if (duration <= 0.0 || invalidated || !rangeInitialized) return;
        const double currentStart = start->value();
        double targetEnd = currentStart + seconds;
        if (targetEnd > duration) {
            targetEnd = duration;
            setTimes(std::max(0.0, targetEnd - seconds), targetEnd, 0);
        } else {
            setTimes(currentStart, targetEnd, 2);
        }
    }

    void updatePositionDisplay() {
        if (!previewTime) return;
        if (previewPositionKnown && rangeInitialized) {
            const double clipLen = std::max(0.0, end->value() - start->value());
            previewTime->setText(QStringLiteral("%1 / %2")
                                     .arg(formatSeconds(previewPosition), formatSeconds(clipLen)));
        } else {
            previewTime->setText(ClipTranslation::tr("Preview starts at the selected in point"));
        }
    }

    void initializeRange() {
        if (rangeInitialized || exporting || invalidated || !std::isfinite(duration) || duration <= 0.0) return;
        const double center = source ? std::clamp(source->position(), 0.0, duration) : 0.0;
        const double a = std::max(0.0, center - 5.0);
        const double b = std::min(duration, a + 10.0);
        const double boundedStart = std::max(0.0, b - kMaximumClipDuration);
        rangeInitialized = true;
        setTimes(std::max(a, boundedStart), b, 0);
        range->setEnabled(true);
        start->setEnabled(true);
        end->setEnabled(true);
        quality->setEnabled(true);
        for (auto* btn : presetButtons) btn->setEnabled(true);
        btnSetStartCurrent->setEnabled(true);
        btnSetEndCurrent->setEnabled(true);
        btnSeekStart->setEnabled(true);
        btnSeekEnd->setEnabled(true);
        btnStepBack->setEnabled(true);
        btnStepForward->setEnabled(true);
        playButton->setEnabled(true);
        muteButton->setEnabled(true);
        previewScrub->setEnabled(true);
        exportButton->setEnabled(size.isValid() && !invalidated);
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Choose a moment up to 60 seconds long."));
    }

    void captureTracks() {
        externalAudio.clear();
        audioTrack.reset();
        if (!source || !source->mediaTracks().ready) return;
        const MediaTrackSelection selection = source->mediaTrackSelection();
        audioTrack = selection.audio;
        if (selection.audio.has_value()) {
            for (const MediaTrack& track : source->mediaTracks().tracks) {
                if (track.type == MediaTrackType::Audio && track.id == *selection.audio && track.external) {
                    externalAudio = track.externalFilename;
                    break;
                }
            }
        }
        if (preview && !externalAudio.isEmpty() && !previewExternalAudioAdded) {
            previewExternalAudioAdded = preview->addExternalTrack(externalAudio, false);
        }
        syncPreviewAudio();
    }

    void syncPreviewAudio() {
        if (!preview || !preview->mediaTracks().ready || !audioTrack.has_value()) return;
        if (!externalAudio.isEmpty() && !previewExternalAudioAdded) {
            previewExternalAudioAdded = preview->addExternalTrack(externalAudio, false);
            if (previewExternalAudioAdded) return;
        }
        const MediaTrackSnapshot& snapshot = preview->mediaTracks();
        for (const MediaTrack& track : snapshot.tracks) {
            if (track.type == MediaTrackType::Audio && track.id == *audioTrack) {
                preview->selectMediaTrack(MediaTrackType::Audio, *audioTrack,
                                          snapshot.coreIdentity, snapshot.sourceEpoch);
                return;
            }
        }
    }

    void setTimes(double a, double b, int editedField) {
        if (duration <= 0.0 || exporting || invalidated || !rangeInitialized) return;
        a = std::clamp(a, 0.0, duration);
        b = std::clamp(b, 0.0, duration);
        if (b <= a) {
            if (a >= duration) a = std::max(0.0, duration - 0.001);
            else b = std::min(duration, a + 0.001);
        }
        if (b - a > kMaximumClipDuration) {
            if (editedField == 1) b = std::min(duration, a + kMaximumClipDuration);
            else if (editedField == 2) a = std::max(0.0, b - kMaximumClipDuration);
            else b = a + kMaximumClipDuration;
        }
        syncing = true;
        start->setRange(0.0, duration);
        end->setRange(0.0, duration);
        start->setValue(a);
        end->setValue(b);
        durationLabel->setText(ClipTranslation::tr("%1 selected").arg(formatSeconds(b - a)));
        range->setRange(duration, a, b);
        syncing = false;
        exportButton->setEnabled(!invalidated && !exporting && size.isValid() && b > a && b - a <= kMaximumClipDuration);
        if (previewScrub) {
            scrubbingPreview = true;
            previewScrub->setValue(0);
            scrubbingPreview = false;
            previewScrub->setEnabled(duration > 0.0);
        }
        if (preview && !invalidated) {
            preview->setPaused(true);
            preview->seek(a);
            updatePlayButton(false);
        }
        updatePositionDisplay();
    }

    bool validRange() const {
        if (!rangeInitialized || exporting || invalidated || !start || !end || !quality) return false;
        const double a = start->value();
        const double b = end->value();
        return !invalidated && std::isfinite(a) && std::isfinite(b) && a >= 0.0 && b > a &&
               b <= duration && b - a <= kMaximumClipDuration && size.isValid();
    }

    void chooseOutput() {
        if (exporting || !validRange() || !source) return;
        QPointer<Private> self(this);
        QPointer<ClipDialog> guardedDialog(dialog);
        QPointer<VideoSurface> guardedSource(source);
        const uint64_t sourceEpoch = source->sourceEpoch();
        const QString selectedSource = sourceUrl;
        const double selectedStart = start->value();
        const double selectedEnd = end->value();
        const QSize selectedSize = size;
        const int selectedHeight = quality->currentData().toInt();
        const std::optional<qint64> selectedAudioTrack = audioTrack;
        const QString selectedExternalAudio = externalAudio;
        QString suggestedName = QFileInfo(QUrl(selectedSource).path()).completeBaseName();
        if (suggestedName.isEmpty()) suggestedName = ClipTranslation::tr("clip");
        suggestedName += QStringLiteral(".mp4");
        QString path = QFileDialog::getSaveFileName(dialog, ClipTranslation::tr("Save clip"), suggestedName,
                                                    ClipTranslation::tr("MP4 video (*.mp4)"),
                                                    nullptr, QFileDialog::DontConfirmOverwrite);
        if (!self || !guardedDialog || !guardedSource) return;
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
            sourceUrl != selectedSource) return;
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) path += QStringLiteral(".mp4");
        bool replaceExisting = false;
        if (QFileInfo::exists(path)) {
            const auto answer = QMessageBox::question(dialog, ClipTranslation::tr("Replace existing file?"),
                                                      ClipTranslation::tr("%1 already exists. Replace it after the new clip is complete?").arg(QFileInfo(path).fileName()),
                                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (!self || !guardedDialog || !guardedSource) return;
            if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
                sourceUrl != selectedSource) return;
            if (answer != QMessageBox::Yes) return;
            replaceExisting = true;
        }
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
            sourceUrl != selectedSource || start->value() != selectedStart ||
            end->value() != selectedEnd || size != selectedSize ||
            quality->currentData().toInt() != selectedHeight) return;
        ClipExportRequest request;
        request.source = selectedSource;
        request.outputPath = path;
        request.startSeconds = selectedStart;
        request.endSeconds = selectedEnd;
        request.sourceSize = selectedSize;
        request.maximumHeight = selectedHeight;
        request.audioTrackId = selectedAudioTrack;
        request.externalAudioPath = selectedExternalAudio;
        request.replaceExisting = replaceExisting;
        request.sourceOptions = {
            {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                           "Chrome/142.0.0.0 Safari/537.36"},
            {"http-header-fields", "Accept-Language: uk-UA"}};
        exporting = true;
        progress->setValue(0);
        progress->setVisible(true);
        cancel->setVisible(true);
        cancel->setEnabled(true);
        exportButton->setText(ClipTranslation::tr("Exporting…"));
        exportButton->setEnabled(false);
        start->setEnabled(false);
        end->setEnabled(false);
        range->setEnabled(false);
        quality->setEnabled(false);
        for (auto* btn : presetButtons) btn->setEnabled(false);
        btnSetStartCurrent->setEnabled(false);
        btnSetEndCurrent->setEnabled(false);
        btnSeekStart->setEnabled(false);
        btnSeekEnd->setEnabled(false);
        btnStepBack->setEnabled(false);
        btnStepForward->setEnabled(false);
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Preparing export…"));
        exportButtonState = ExportButtonState::Default;
        exportWorker.start(request);
    }

    void unavailable(const char* sourceText) {
        setStatus(sourceText);
        range->setEnabled(false);
        start->setEnabled(false);
        end->setEnabled(false);
        quality->setEnabled(false);
        for (auto* btn : presetButtons) btn->setEnabled(false);
        btnSetStartCurrent->setEnabled(false);
        btnSetEndCurrent->setEnabled(false);
        btnSeekStart->setEnabled(false);
        btnSeekEnd->setEnabled(false);
        btnStepBack->setEnabled(false);
        btnStepForward->setEnabled(false);
        playButton->setEnabled(false);
        muteButton->setEnabled(false);
        previewScrub->setEnabled(false);
        exportButton->setEnabled(false);
    }

    ClipDialog* dialog;
    QPointer<VideoSurface> source;
    QString caption;
    VideoSurface* preview = nullptr;
    ClipExport exportWorker;
    ClipRange* range = nullptr;
    QDoubleSpinBox* start = nullptr;
    QDoubleSpinBox* end = nullptr;
    QComboBox* quality = nullptr;
    QLabel* status = nullptr;
    QLabel* startLabel = nullptr;
    QLabel* endLabel = nullptr;
    QLabel* qualityLabel = nullptr;
    QLabel* durationLabel = nullptr;
    QLabel* rangeTitle = nullptr;
    QLabel* previewTime = nullptr;
    QFrame* previewCard = nullptr;
    QFrame* rangeCard = nullptr;
    QFrame* exportCard = nullptr;
    QSlider* previewScrub = nullptr;
    IconButton* playButton = nullptr;
    IconButton* muteButton = nullptr;
    QPushButton* btnStepBack = nullptr;
    QPushButton* btnStepForward = nullptr;
    QPushButton* btnSetStartCurrent = nullptr;
    QPushButton* btnSeekStart = nullptr;
    QPushButton* btnSetEndCurrent = nullptr;
    QPushButton* btnSeekEnd = nullptr;
    QVector<QPushButton*> presetButtons;
    QPushButton* exportButton = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* reveal = nullptr;
    QPushButton* copyPath = nullptr;
    QProgressBar* progress = nullptr;
    QString sourceUrl;
    QString externalAudio;
    QString completedPath;
    std::optional<qint64> audioTrack;
    QSize size;
    double duration = 0.0;
    QString codecName;
    QString statusSource;
    QStringList statusArgs;
    double previewPosition = 0.0;
    ExportButtonState exportButtonState = ExportButtonState::Default;
    bool previewPositionKnown = false;
    bool previewMuted = true;
    bool syncing = false;
    bool exporting = false;
    bool rangeInitialized = false;
    bool previewExternalAudioAdded = false;
    bool invalidated = false;
    bool scrubbingPreview = false;
};

ClipDialog::ClipDialog(VideoSurface* source, const QString& caption, QWidget* parent)
    : QDialog(parent), d_(new Private(this, source, caption)) {}

void ClipDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && d_) d_->retranslateUi();
    QDialog::changeEvent(event);
}

void ClipDialog::keyPressEvent(QKeyEvent* event) {
    if (d_ && d_->handleKeyPress(event)) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

ClipDialog::~ClipDialog() = default;

void ClipDialog::closeEvent(QCloseEvent* event) {
    if (d_) {
        if (d_->preview) d_->preview->setPaused(true);
        if (d_->exporting) {
            d_->exportWorker.cancel();
            d_->setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
        }
    }
    QDialog::closeEvent(event);
}

} // namespace mplayer::shell
