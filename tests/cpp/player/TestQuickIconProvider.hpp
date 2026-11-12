// The native Quick icon provider (image://player-icons) rasterises the shell's
// IconPainter glyphs into QImages for the QML chrome. These cases check the
// pixels that reach the window: the same logical ink at every device ratio, the
// requested colour and alpha, distinguishable play/pause shapes, and a typo
// failing instead of quietly drawing another symbol.
#pragma once

#include <QtTest/QTest>

class TestQuickIconProvider : public QObject {
    Q_OBJECT

private slots:
    void glyphPixelsKeepTheirLogicalPosition_data();
    void glyphPixelsKeepTheirLogicalPosition();

    void playAndPausePaintVisiblyDifferentShapes();
    void eachRequestPaintsItsOwnColour();
    void aTranslucentRequestKeepsItsAlphaAndTone();
    void anUnknownSymbolFailsInsteadOfDrawingAnother();
    void anImageThreadRequestPaintsTheSameGlyph();
};
