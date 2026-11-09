// Renders the clip sheet offscreen so its layout, contrast and selection
// chrome can be inspected without a player, a decoder or a display. It is a
// screenshot probe, not an acceptance test: the numbers it prints are geometry,
// and the PNGs are the evidence.

#include "ClipSheet.hpp"
#include "ShellTheme.hpp"

#include <QApplication>
#include <QDir>

#include <functional>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QWidget>
#include <QCoreApplication>

#include <cstdio>

using namespace mplayer::shell;

namespace {

QPixmap slotFrame(int index, int count) {
    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter p(&canvas);
    const double phase = count > 1 ? double(index) / double(count - 1) : 0.0;
    QLinearGradient sky(0, 0, 0, 180);
    sky.setColorAt(0.0, QColor::fromHsvF(0.55 + 0.12 * phase, 0.55, 0.72));
    sky.setColorAt(1.0, QColor::fromHsvF(0.58 + 0.12 * phase, 0.30, 0.30));
    p.fillRect(canvas.rect(), sky);
    // A horizon and two silhouettes give the tile the texture a real frame has,
    // so the sheet's dimming and selection tint can be judged.
    p.setBrush(QColor::fromHsvF(0.60 + 0.12 * phase, 0.40, 0.16));
    p.setPen(Qt::NoPen);
    p.drawRect(0, 118, 320, 62);
    p.setBrush(QColor(0x0f, 0x10, 0x14));
    p.drawEllipse(QPointF(96, 96), 16, 22);
    p.drawRect(QRectF(80, 112, 32, 42));
    p.drawEllipse(QPointF(232, 92), 15, 20);
    p.drawRect(QRectF(217, 108, 30, 46));
    QFont label = p.font();
    label.setPixelSize(20);
    label.setBold(true);
    p.setFont(label);
    p.setPen(QColor(255, 255, 255, 190));
    p.drawText(canvas.rect().adjusted(0, 0, -10, -8), Qt::AlignRight | Qt::AlignBottom,
               QStringLiteral("%1").arg(index + 1));
    p.end();
    return QPixmap::fromImage(canvas);
}

int grabSheet(ClipSheet* sheet, const std::function<void()>& fill, const QString& name, int width) {
    sheet->resize(width, sheet->sizeHint().height());
    QCoreApplication::processEvents();
    // The strip learns its width in the layout pass, so frames are (re)made for
    // the width this grab will use, never for the previous one.
    fill();
    QCoreApplication::processEvents();
    const QImage image = sheet->grab().toImage();
    QDir().mkpath(QStringLiteral(".probe-out"));
    const QString path = QStringLiteral(".probe-out/%1.png").arg(name);
    image.save(path);
    std::fprintf(stderr, "ok   %s: %dx%d -> %s\n", qPrintable(name), image.width(),
                 image.height(), qPrintable(path));
    return image.isNull() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Standalone, not inside a layout: grab() then reports the size the widget
    // was given, which is the whole point of a layout probe.
    auto* sheet = new ClipSheet(nullptr);
    // Laid out but never displayed: WA_DontShowOnScreen makes Qt run the layout
    // pass, which is what gives the strip its real width before frames are made.
    sheet->setAttribute(Qt::WA_DontShowOnScreen, true);
    sheet->show();
    sheet->resize(1100, sheet->sizeHint().height());
    QCoreApplication::processEvents();

    const double windowStart = 5 * 60 + 11.0 + 0.1;
    const double windowEnd = windowStart + 240.0;
    sheet->setWindow(windowStart, windowEnd);
    sheet->setMaximumSpan(60.0);
    sheet->setSelection(windowStart + 20.0, windowStart + 50.0);
    sheet->setPlayhead(windowStart + 42.7);
    sheet->setQualityOptions({1080, 720, 480}, 1080);
    sheet->setDownloadLabel(QStringLiteral("Download ≈ 5 MB"), true);
    sheet->setStatus(QStringLiteral("MP4 · H.264 / AAC · the clip is cut from the source, not re-encoded twice"));

    // The strip grows its slot vector on the first layout pass, so the frames are
    // made after that pass: a probe that fills slots before it would leave half
    // the rail as empty plates.
    const auto fill = [&] {
        const int count = sheet->tileCount();
        for (int i = 0; i < count; ++i) sheet->setTile(i, slotFrame(i, count));
        return count;
    };
    const int tiles = fill();
    std::fprintf(stderr, "ok   %d filmstrip slots filled at 1100 px\n", tiles);

    int failures = 0;
    failures += grabSheet(sheet, fill, QStringLiteral("clip-sheet-wide"), 1100);

    failures += grabSheet(sheet, fill, QStringLiteral("clip-sheet-narrow"), 620);

    // A selection at the window's edge must stay inside it, and the exporter's
    // span limit must be enforced by the sheet, not only by the dialog.
    sheet->setSelection(windowEnd - 5.0, windowEnd + 40.0);
    const ClipSheet::Range clamped = sheet->selection();
    std::fprintf(stderr, "ok   selection clamped to %.3f-%.3f inside %.3f-%.3f\n", clamped.start,
                 clamped.end, windowStart, windowEnd);
    if (clamped.end > windowEnd + 0.001 || clamped.end - clamped.start > 60.001) {
        std::fprintf(stderr, "FAIL clamping kept the selection inside the window\n");
        ++failures;
    }
    // A mid-window selection with the meter running, which is the state a user
    // sees for the duration of an export.
    sheet->setSelection(windowStart + 40.0, windowStart + 70.0);
    sheet->setProgress(0.42);
    failures += grabSheet(sheet, fill, QStringLiteral("clip-sheet-exporting"), 1100);

    std::fprintf(stderr, "%s\n", failures == 0 ? "DONE clip sheet rendered" : "FAIL clip sheet");
    return failures == 0 ? 0 : 1;
}
