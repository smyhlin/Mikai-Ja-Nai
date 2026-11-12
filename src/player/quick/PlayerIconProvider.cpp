#include "PlayerIconProvider.hpp"

#include <player/shell/ShellTheme.hpp>

#include <QtCore/QDebug>
#include <QtCore/QHash>
#include <QtCore/QSize>
#include <QtCore/QUrlQuery>
#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtQml/QQmlEngine>

#include <cmath>

namespace mplayer {
namespace quick {

namespace {

using Glyph = void (*)(QPainter&, qreal, qreal, qreal, const QColor&);

struct GlyphEntry {
    const char* name;
    Glyph paint;
};

// One entry per real IconPainter glyph; stroke()/fill() are painter setup, not
// a symbol.
constexpr GlyphEntry kGlyphs[] = {
    {"minimize", &shell::IconPainter::minimize},
    {"restore", &shell::IconPainter::restore},
    {"close", &shell::IconPainter::close},
    {"chevron-left", &shell::IconPainter::chevronLeft},
    {"chevron-right", &shell::IconPainter::chevronRight},
    {"play", &shell::IconPainter::play},
    {"pause", &shell::IconPainter::pause},
    {"volume", &shell::IconPainter::volume},
    {"volume-low", &shell::IconPainter::volumeLow},
    {"mute", &shell::IconPainter::mute},
    {"expand", &shell::IconPainter::expand},
    {"contract", &shell::IconPainter::contract},
    {"pip", &shell::IconPainter::pip},
    {"gear", &shell::IconPainter::gear},
    {"download", &shell::IconPainter::download},
    {"more", &shell::IconPainter::more},
    {"reset", &shell::IconPainter::reset},
    {"spark", &shell::IconPainter::spark},
    {"keyboard", &shell::IconPainter::keyboard},
    {"microphone", &shell::IconPainter::microphone},
    {"captions", &shell::IconPainter::captions},
    {"clapperboard", &shell::IconPainter::clapperboard},
    {"hdr", &shell::IconPainter::hdr},
    {"dolby-vision", &shell::IconPainter::dolbyVision},
    {"dolby-atmos", &shell::IconPainter::dolbyAtmos},
    {"codec-h264", &shell::IconPainter::codecH264},
    {"codec-h265", &shell::IconPainter::codecH265},
    {"codec-av1", &shell::IconPainter::codecAv1},
    {"codec-vp9", &shell::IconPainter::codecVp9},
};

const GlyphEntry* findGlyph(const QString& name) {
    if (name.isEmpty()) return nullptr;
    for (const GlyphEntry& entry : kGlyphs) {
        if (name == QLatin1String(entry.name)) return &entry;
    }
    return nullptr;
}

/// Fixed tokens only: the mutable accent is GUI-thread state, so an
/// accent-coloured icon passes its resolved hex value.
const QColor* themeToken(const QString& token) {
    static const QHash<QString, QColor> tokens = {
        {QStringLiteral("w95"), shell::w95()},
        {QStringLiteral("w90"), shell::w90()},
        {QStringLiteral("w70"), shell::w70()},
        {QStringLiteral("w55"), shell::w55()},
        {QStringLiteral("w45"), shell::w45()},
        {QStringLiteral("w35"), shell::w35()},
        {QStringLiteral("w25"), shell::w25()},
        {QStringLiteral("w15"), shell::w15()},
        {QStringLiteral("w10"), shell::w10()},
        {QStringLiteral("red"), shell::red()},
        {QStringLiteral("green"), shell::green()},
    };
    const auto it = tokens.constFind(token);
    return it == tokens.constEnd() ? nullptr : &it.value();
}

struct ParsedRequest {
    const GlyphEntry* glyph = nullptr;
    QColor color;
    qreal logicalSize = 16.0;
    qreal devicePixelRatio = 1.0;
    QString error;
};

ParsedRequest parseRequest(const QString& request, const QSize& requestedSize) {
    constexpr qreal kMinSize = 1.0;
    constexpr qreal kMaxSize = 512.0;

    ParsedRequest parsed;
    parsed.color = shell::w90();
    parsed.logicalSize = requestedSize.isValid() && requestedSize.width() > 0
                             ? qreal(requestedSize.width())
                             : 16.0;

    const qsizetype mark = request.indexOf(QLatin1Char('?'));
    const QString name = (mark < 0 ? request : request.left(mark)).trimmed();
    if (name.isEmpty()) {
        parsed.error = QStringLiteral("the request names no icon");
        return parsed;
    }
    parsed.glyph = findGlyph(name);
    if (parsed.glyph == nullptr) {
        parsed.error = QStringLiteral("unknown icon \"%1\"").arg(name);
        return parsed;
    }
    if (mark < 0) return parsed;

    const QUrlQuery query(request.mid(mark + 1));
    for (const auto& [key, value] : query.queryItems(QUrl::FullyDecoded)) {
        if (key == QLatin1String("size")) {
            bool ok = false;
            const qreal size = value.toDouble(&ok);
            if (!ok || !std::isfinite(size) || size < kMinSize || size > kMaxSize) {
                parsed.error = QStringLiteral("icon \"%1\": size \"%2\" is not a number in "
                                              "%3..%4")
                                   .arg(name, value)
                                   .arg(kMinSize)
                                   .arg(kMaxSize);
                return parsed;
            }
            parsed.logicalSize = size;
        } else if (key == QLatin1String("dpr")) {
            bool ok = false;
            const qreal ratio = value.toDouble(&ok);
            if (!ok || !std::isfinite(ratio) || ratio <= 0.0) {
                parsed.error = QStringLiteral("icon \"%1\": dpr \"%2\" is not a positive "
                                              "number")
                                   .arg(name, value);
                return parsed;
            }
            parsed.devicePixelRatio = ratio;
        } else if (key == QLatin1String("color")) {
            const QColor* token = themeToken(value);
            const QColor color = token != nullptr ? *token : QColor::fromString(value);
            if (!color.isValid()) {
                parsed.error = QStringLiteral("icon \"%1\": colour \"%2\" is neither a "
                                              "supported theme token nor a colour")
                                   .arg(name, value);
                return parsed;
            }
            parsed.color = color;
        } else {
            parsed.error = QStringLiteral("icon \"%1\": unsupported parameter \"%2\"")
                               .arg(name, key);
            return parsed;
        }
    }
    return parsed;
}

}  // namespace

PlayerIconProvider::PlayerIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

const char* PlayerIconProvider::providerId() { return "player-icons"; }

QImage PlayerIconProvider::requestImage(const QString& id, QSize* size,
                                        const QSize& requestedSize) {
    if (size != nullptr) *size = QSize();

    const ParsedRequest request = parseRequest(id, requestedSize);
    if (!request.error.isEmpty()) {
        qWarning("PlayerIconProvider: %s", qUtf8Printable(request.error));
        return QImage();
    }

    // Allocation guard only; any positive DPR is honoured below.
    constexpr qreal kMaxDeviceSide = 4096.0;
    const qreal deviceSide = request.logicalSize * request.devicePixelRatio;
    if (!std::isfinite(deviceSide) || deviceSide < 1.0 || deviceSide > kMaxDeviceSide) {
        qWarning("PlayerIconProvider: icon \"%s\" would rasterise at %s px",
                 request.glyph->name, qUtf8Printable(QString::number(deviceSide)));
        return QImage();
    }
    const int physical = qRound(deviceSide);

    QImage image(physical, physical, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        qWarning("PlayerIconProvider: icon \"%s\" could not allocate a %d px image",
                 request.glyph->name, physical);
        return QImage();
    }
    image.fill(Qt::transparent);

    QPainter painter(&image);
    if (!painter.isActive()) {
        qWarning("PlayerIconProvider: the icon painter could not start");
        return QImage();
    }
    shell::enableChromeAntialiasing(painter);
    painter.scale(request.devicePixelRatio, request.devicePixelRatio);
    request.glyph->paint(painter, 0.0, 0.0, request.logicalSize, request.color);
    painter.end();

    // Tagged after painting: the DPR is metadata for the Quick image loader,
    // while the glyph above was rasterised straight into the physical pixels.
    image.setDevicePixelRatio(request.devicePixelRatio);
    if (size != nullptr) *size = image.size();
    return image;
}

void installPlayerIcons(QQmlEngine* engine) {
    if (engine == nullptr) return;
    engine->addImageProvider(QString::fromLatin1(PlayerIconProvider::providerId()),
                             new PlayerIconProvider);
}

}  // namespace quick
}  // namespace mplayer
