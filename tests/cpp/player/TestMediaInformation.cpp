#include "TestMediaInformation.hpp"

#include "MediaInformationOverview.hpp"
#include "MediaInformationPresentation.hpp"
#include "MediaInformationTree.hpp"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QEvent>
#include <QHeaderView>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTreeWidgetItemIterator>

#include <cmath>
#include <limits>

using namespace mplayer;
using namespace mplayer::shell;

namespace {

const MediaInformationNode* findNode(const MediaInformationDocument& document, const QString& id) {
    for (const MediaInformationNode& node : document) {
        if (node.id == id) return &node;
        if (const auto* child = findNode(node.children, id)) return child;
    }
    return nullptr;
}

QTextEdit* findEditor(MediaInformationOverview& overview, const QString& value) {
    for (QTextEdit* editor : overview.findChildren<QTextEdit*>())
        if (editor->toPlainText() == value) return editor;
    return nullptr;
}

QTreeWidgetItem* findItem(MediaInformationTree& tree, const QString& id) {
    for (QTreeWidgetItemIterator iterator(&tree); *iterator; ++iterator)
        if ((*iterator)->data(0, Qt::UserRole).toString() == id) return *iterator;
    return nullptr;
}

MediaInformationDocument manyFacts() {
    MediaInformationNode file{QStringLiteral("file"), QStringLiteral("File"), {}, {}};
    for (int index = 0; index < 40; ++index) {
        const QString id = QStringLiteral("file.fact.%1").arg(index);
        file.children.append({id, QStringLiteral("Field %1").arg(index),
                              QStringLiteral("literal-value-%1-").arg(index) + QString(64, QLatin1Char('x')), {}});
    }
    return {file};
}

class DefaultLocale final {
public:
    explicit DefaultLocale(const QLocale& locale) : previous_() { QLocale::setDefault(locale); }
    ~DefaultLocale() { QLocale::setDefault(previous_); }
private:
    QLocale previous_;
};

QString clockDigits(const QLocale& locale, quint64 value, int width) {
    QLocale clock(locale);
    clock.setNumberOptions(clock.numberOptions() | QLocale::OmitGroupSeparator);
    QString text = clock.toString(value);
    while (text.size() < width) text.prepend(clock.toString(0));
    return text;
}

} // namespace

void TestMediaInformation::overviewMeasurementDoesNotRelayoutTheLiveDocument() {
    MediaInformationOverview overview;
    const QString path = QStringLiteral("/media/") + QString(1024, QLatin1Char('a')) + QStringLiteral(".mkv");
    overview.setDocument({{QStringLiteral("file"), QStringLiteral("File"), {}, {
        {QStringLiteral("file.path"), QStringLiteral("Path"), path, {}}}}});
    overview.resize(420, 320);
    overview.show();
    QTextEdit* editor = findEditor(overview, path);
    QVERIFY(editor != nullptr);
    QTRY_VERIFY(editor->viewport()->width() > 0);
    const qreal liveWidth = editor->document()->textWidth();
    QSignalSpy documentResizes(editor->document()->documentLayout(),
                              &QAbstractTextDocumentLayout::documentSizeChanged);
    const int narrow = editor->heightForWidth(80);
    const int wide = editor->heightForWidth(320);
    QVERIFY(narrow > wide);
    QCOMPARE(editor->heightForWidth(80), narrow);
    QCOMPARE(editor->document()->textWidth(), liveWidth);
    QCOMPARE(documentResizes.count(), 0);
}

void TestMediaInformation::overviewWrapsAndCopiesLiteralUnbrokenMetadata() {
    MediaInformationOverview overview;
    const QString path = QStringLiteral("/media/<b>&") + QString(900, QChar(0x754c)) + QStringLiteral(".mkv");
    overview.setDocument({{QStringLiteral("file"), QStringLiteral("File"), {}, {
        {QStringLiteral("file.path"), QStringLiteral("Path"), path, {}}}}});
    overview.resize(420, 320);
    overview.show();
    QTextEdit* editor = findEditor(overview, path);
    QVERIFY(editor != nullptr);
    QTRY_VERIFY(editor->document()->size().height() > 2 * editor->fontMetrics().height());
    QTRY_VERIFY(editor->viewport()->height() + 1 >= std::ceil(editor->document()->size().height()));
    QVERIFY(editor->width() <= overview.viewport()->width());
    QVERIFY(!overview.horizontalScrollBar()->isVisible());
    QVERIFY(!editor->horizontalScrollBar()->isVisible());
    editor->selectAll();
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), path);
    QTest::keyClick(editor, Qt::Key_X);
    QCOMPARE(editor->toPlainText(), path);
    QCOMPARE(editor->textCursor().selectedText(), path);
}

void TestMediaInformation::overviewKeepsSelectionAndScrollWhenSectionsChange() {
    MediaInformationOverview overview;
    MediaInformationDocument document = manyFacts();
    document.prepend({QStringLiteral("status"), QStringLiteral("Status"), {}, {
        {QStringLiteral("status.loading"), QStringLiteral("State"), QStringLiteral("Loading"), {}}}});
    overview.setDocument(document);
    overview.resize(460, 320);
    overview.show();
    const QString selected = document[1].children[22].value;
    QPointer<QTextEdit> editor = findEditor(overview, selected);
    QVERIFY(editor != nullptr);
    QTRY_VERIFY(overview.verticalScrollBar()->maximum() > overview.viewport()->height());
    overview.ensureWidgetVisible(editor, 0, 0);
    editor->selectAll();
    editor->setFocus();
    QCoreApplication::processEvents();
    const int top = editor->mapTo(overview.viewport(), QPoint()).y();
    document.removeFirst();
    overview.setDocument(document);
    QVERIFY(editor != nullptr);
    QCOMPARE(findEditor(overview, selected), editor.data());
    QCOMPARE(editor->textCursor().selectedText(), selected);
    QTRY_VERIFY(std::abs(editor->mapTo(overview.viewport(), QPoint()).y() - top) <= 1);
}

void TestMediaInformation::overviewKeepsSelectedTextWhenAnotherPartOfTheValueChanges() {
    MediaInformationOverview overview;
    const QString kept = QStringLiteral("日本語 <literal>");
    MediaInformationDocument document{{QStringLiteral("audio"), QStringLiteral("Audio"), {}, {
        {QStringLiteral("audio.1"), QStringLiteral("Track"), kept + QStringLiteral(" · old"), {
            {QStringLiteral("audio.1.channels"), QStringLiteral("Channels"), QStringLiteral("2"), {}}}}}}};
    overview.setDocument(document);
    QTextEdit* editor = findEditor(overview, document[0].children[0].value);
    QVERIFY(editor != nullptr);
    QTextCursor selection(editor->document());
    selection.setPosition(kept.size());
    selection.setPosition(0, QTextCursor::KeepAnchor);
    editor->setTextCursor(selection);
    document[0].children[0].value = kept + QStringLiteral(" · a changed selection flag");
    overview.setDocument(document);
    QCOMPARE(editor->textCursor().selectedText(), kept);
    QCOMPARE(editor->textCursor().position(), 0);
    QCOMPARE(editor->textCursor().anchor(), kept.size());
}

void TestMediaInformation::treeKeepsValuesVisibleWhenThePropertyHeaderIsDragged() {
    MediaInformationTree tree;
    const QString path = QString(600, QLatin1Char('p'));
    tree.setDocument({{QStringLiteral("file"), QStringLiteral("File"), {}, {
        {QStringLiteral("file.path"), QStringLiteral("Path"), path, {}}}}});
    tree.resize(420, 320);
    tree.show();
    QTRY_VERIFY(tree.viewport()->width() > 300);
    // resizeSection is the same header API used by an interactive handle drag.
    tree.header()->resizeSection(0, 2000);
    QTRY_VERIFY(tree.columnWidth(0) <= tree.viewport()->width() / 2);
    QVERIFY(tree.columnWidth(1) >= tree.viewport()->width() / 2 - 1);
    QTreeWidgetItem* pathItem = findItem(tree, QStringLiteral("file.path"));
    QVERIFY(pathItem != nullptr);
    QTRY_VERIFY(tree.visualItemRect(pathItem).height() > 2 * tree.fontMetrics().height());
    QCOMPARE(pathItem->text(1), path);
    QVERIFY(!tree.horizontalScrollBar()->isVisible());
}

void TestMediaInformation::treeKeepsExpansionSelectionAndScrollAcrossRefresh() {
    MediaInformationTree tree;
    MediaInformationDocument document = manyFacts();
    document.append({QStringLiteral("audio"), QStringLiteral("Audio"), {}, {
        {QStringLiteral("audio.1"), QStringLiteral("Track"), QStringLiteral("unchanged"), {
            {QStringLiteral("audio.1.codec"), QStringLiteral("Codec"), QStringLiteral("AAC"), {}}}}}});
    tree.setDocument(document);
    tree.resize(460, 320);
    tree.show();
    auto* current = findItem(tree, QStringLiteral("file.fact.22"));
    QVERIFY(current != nullptr);
    tree.setCurrentItem(current, 1);
    tree.scrollToItem(current, QAbstractItemView::PositionAtTop);
    QCoreApplication::processEvents();
    const int top = tree.visualItemRect(current).top();
    const int scroll = tree.verticalScrollBar()->value();
    QVERIFY(scroll > 0);
    findItem(tree, QStringLiteral("audio"))->setExpanded(false);
    document[1].children[0].value = QStringLiteral("updated track selection");
    tree.setDocument(document);
    QCOMPARE(tree.currentItem()->data(0, Qt::UserRole).toString(), QStringLiteral("file.fact.22"));
    QCOMPARE(tree.currentColumn(), 1);
    QVERIFY(tree.currentItem()->isSelected());
    QVERIFY(!findItem(tree, QStringLiteral("audio"))->isExpanded());
    QTRY_COMPARE(tree.visualItemRect(tree.currentItem()).top(), top);
    QCOMPARE(tree.verticalScrollBar()->value(), scroll);
}

void TestMediaInformation::treeDoesNotRecreateAClearedSelection() {
    MediaInformationTree tree;
    MediaInformationDocument document = manyFacts();
    tree.setDocument(document);
    tree.setCurrentItem(findItem(tree, QStringLiteral("file.fact.2")), 1);
    tree.clearSelection();
    QVERIFY(tree.selectedItems().isEmpty());
    document[0].children[3].value += QLatin1Char('!');
    tree.setDocument(document);
    QVERIFY(tree.currentItem() != nullptr);
    QCOMPARE(tree.currentItem()->data(0, Qt::UserRole).toString(), QStringLiteral("file.fact.2"));
    QVERIFY(tree.selectedItems().isEmpty());
}

void TestMediaInformation::treeCopiesLiteralMetadataWithoutMakingItEditable() {
    MediaInformationTree tree;
    const QString value = QStringLiteral("<b>literal & metadata</b>\n/one/unbroken/path");
    tree.setDocument({{QStringLiteral("file"), QStringLiteral("File"), {}, {
        {QStringLiteral("file.path"), QStringLiteral("Path"), value, {}}}}});
    tree.show();
    auto* item = findItem(tree, QStringLiteral("file.path"));
    QVERIFY(item != nullptr);
    tree.setCurrentItem(item, 1);
    QTest::keyClick(&tree, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Path: ") + value);
    QTest::keyClick(&tree, Qt::Key_F2);
    QTest::keyClick(&tree, Qt::Key_X);
    QCOMPARE(item->text(1), value);
    QVERIFY(tree.findChildren<QLineEdit*>().isEmpty());
}

void TestMediaInformation::plainTextPreservesLiteralMetadataAndContinuationIndentation() {
    const MediaInformationDocument document{{QStringLiteral("file"), QStringLiteral("File"), {}, {
        {QStringLiteral("file.path"), QStringLiteral("Path"),
         QStringLiteral("<literal> & metadata\n/one/unbroken/path"), {}}}}};
    QCOMPARE(mediaInformationPlainText(document),
             QStringLiteral("File\n  Path: <literal> & metadata\n    /one/unbroken/path\n"));
}

void TestMediaInformation::localizedDurationCarriesWithoutIntegerOverflow_data() {
    QTest::addColumn<QString>("localeName");
    QTest::addColumn<double>("seconds");
    QTest::addColumn<quint64>("hours");
    QTest::addColumn<quint64>("minutes");
    QTest::addColumn<quint64>("wholeSeconds");
    QTest::addColumn<quint64>("milliseconds");
    for (const QString& locale : {QStringLiteral("C"), QStringLiteral("de_DE"), QStringLiteral("ar_EG")}) {
        QTest::newRow(qPrintable(locale + QStringLiteral("-minute-carry")))
            << locale << 59.9996 << quint64(0) << quint64(1) << quint64(0) << quint64(0);
        QTest::newRow(qPrintable(locale + QStringLiteral("-hour-carry")))
            << locale << 3599.9996 << quint64(1) << quint64(0) << quint64(0) << quint64(0);
        QTest::newRow(qPrintable(locale + QStringLiteral("-fraction")))
            << locale << 1.025 << quint64(0) << quint64(0) << quint64(1) << quint64(25);
        QTest::newRow(qPrintable(locale + QStringLiteral("-large-clock")))
            << locale << 3600000001.0 << quint64(1000000) << quint64(0) << quint64(1) << quint64(0);
    }
}

void TestMediaInformation::localizedDurationCarriesWithoutIntegerOverflow() {
    QFETCH(QString, localeName);
    QFETCH(double, seconds);
    QFETCH(quint64, hours);
    QFETCH(quint64, minutes);
    QFETCH(quint64, wholeSeconds);
    QFETCH(quint64, milliseconds);
    const QLocale locale(localeName);
    const DefaultLocale restore(locale);
    MediaInformation information;
    information.duration = seconds;
    information.loading = false;
    const auto document = buildMediaInformationDocument(information, {}, false, {}, {});
    const auto* duration = findNode(document, QStringLiteral("file.duration"));
    QVERIFY(duration != nullptr);
    QString expected = clockDigits(locale, hours, 2) + QLatin1Char(':') +
                       clockDigits(locale, minutes, 2) + QLatin1Char(':') +
                       clockDigits(locale, wholeSeconds, 2);
    if (milliseconds != 0) expected += locale.decimalPoint() + clockDigits(locale, milliseconds, 3);
    QCOMPARE(duration->value, expected);

    information.duration = std::numeric_limits<double>::max();
    const auto huge = buildMediaInformationDocument(information, {}, false, {}, {});
    const QString hugeValue = findNode(huge, QStringLiteral("file.duration"))->value;
    QVERIFY(hugeValue.startsWith(locale.toString(*information.duration, 'g', QLocale::FloatingPointShortest)));
    QVERIFY(hugeValue.size() < 100);
}

void TestMediaInformation::unavailableNumbersAndUnknownFlagsAreNotReportedAsZeroOrNo() {
    MediaInformation information;
    information.loading = false;
    information.duration = std::numeric_limits<double>::infinity();
    information.size = -1;
    information.framesPerSecond = std::numeric_limits<double>::quiet_NaN();
    information.videoBitrate = -1;
    MediaTrack track;
    track.id = 42;
    track.type = MediaTrackType::Audio;
    MediaTrackSnapshot tracks;
    tracks.ready = true;
    tracks.tracks.append(track);
    const auto unavailable = buildMediaInformationDocument(information, tracks, false, {}, {});
    const QString missing = findNode(unavailable, QStringLiteral("video.codec"))->value;
    for (const QString& id : {QStringLiteral("file.duration"), QStringLiteral("file.size"),
                              QStringLiteral("video.frame-rate"), QStringLiteral("video.video-bitrate"),
                              QStringLiteral("audio.42.selected"), QStringLiteral("audio.42.default"),
                              QStringLiteral("audio.42.forced"), QStringLiteral("audio.42.external")})
        QCOMPARE(findNode(unavailable, id)->value, missing);
    tracks.tracks[0].selectionKnown = true;
    tracks.tracks[0].defaultKnown = true;
    tracks.tracks[0].forcedKnown = true;
    tracks.tracks[0].externalKnown = true;
    const auto knownFalse = buildMediaInformationDocument(information, tracks, false, {}, {});
    tracks.tracks[0].selected = true;
    tracks.tracks[0].isDefault = true;
    tracks.tracks[0].forced = true;
    tracks.tracks[0].external = true;
    const auto knownTrue = buildMediaInformationDocument(information, tracks, false, {}, {});
    for (const QString& suffix : {QStringLiteral("selected"), QStringLiteral("default"),
                                  QStringLiteral("forced"), QStringLiteral("external")}) {
        const QString id = QStringLiteral("audio.42.") + suffix;
        QVERIFY(findNode(knownFalse, id)->value != missing);
        QVERIFY(findNode(knownTrue, id)->value != missing);
        QVERIFY(findNode(knownTrue, id)->value != findNode(knownFalse, id)->value);
    }
}

void TestMediaInformation::maximumIntegerMetadataRetainsItsExactValue() {
    const QLocale locale(QStringLiteral("de_DE"));
    const DefaultLocale restore(locale);
    const auto maximum = std::numeric_limits<int64_t>::max();
    MediaInformation information;
    information.loading = false;
    information.size = maximum;
    information.width = maximum;
    information.height = 1;
    MediaTrack track;
    track.id = maximum;
    MediaTrackSnapshot tracks;
    tracks.ready = true;
    tracks.tracks.append(track);
    const auto document = buildMediaInformationDocument(information, tracks, false, {}, {});
    const QString exact = locale.toString(static_cast<qlonglong>(maximum));
    QVERIFY(findNode(document, QStringLiteral("file.size"))->value.contains(exact));
    QVERIFY(findNode(document, QStringLiteral("video.dimensions"))->value.startsWith(exact));
    const QString id = QStringLiteral("audio.") + QString::number(maximum) + QStringLiteral(".id");
    const auto* idNode = findNode(document, id);
    QVERIFY(idNode != nullptr);
    QCOMPARE(idNode->value, exact);
}
