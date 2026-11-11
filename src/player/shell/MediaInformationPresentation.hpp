#pragma once

#include "../core/MediaInformation.hpp"
#include "../core/MediaTracks.hpp"

#include <QString>
#include <QTextCursor>
#include <QScrollBar>
#include <QVector>

#include <algorithm>

namespace mplayer::shell {

struct MediaInformationNode {
    QString id;
    QString label;
    QString value;
    QVector<MediaInformationNode> children;
    friend bool operator==(const MediaInformationNode&, const MediaInformationNode&);
};

using MediaInformationDocument = QVector<MediaInformationNode>;

MediaInformationDocument buildMediaInformationDocument(
    const MediaInformation& information, const MediaTrackSnapshot& tracks,
    bool timedOut, const QString& trackError, const QString& playbackError);
QString mediaInformationPlainText(const MediaInformationDocument& document);

/// X-H8: the one shared minimal-diff text update for the media-information
/// views (FactValue is a QTextEdit, the dialog's plain view a
/// QPlainTextEdit — hence the template; both expose the same cursor,
/// document and scroll-bar API). Replaces only the differing span so
/// selection and scroll stay attached to their content, and — unlike a
/// bare insertText("") — a pure deletion actually deletes: an empty
/// replacement calls removeSelectedText(), because
/// QTextCursor::insertText("") is an immediate no-op. Surrogate pairs
/// are never split at either diff boundary, and the undo stack is
/// disabled on the read-only edit so 1 Hz live refreshes cannot grow
/// it without bound.
template <typename Edit>
void updateTextEditPreservingSelection(Edit* edit, const QString& value) {
    if (edit == nullptr) return;
    edit->setUndoRedoEnabled(false);
    QString normalized = value;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    normalized.replace(QChar(0x2028), QLatin1Char('\n'));
    normalized.replace(QChar(0x2029), QLatin1Char('\n'));
    const QString previous = edit->toPlainText();
    if (normalized == previous) return;
    const QTextCursor selection = edit->textCursor();
    const int anchor = selection.anchor();
    const int position = selection.position();
    const int scroll = edit->verticalScrollBar()->value();

    qsizetype prefix = 0;
    const qsizetype commonSize = std::min(previous.size(), normalized.size());
    while (prefix < commonSize && previous[prefix] == normalized[prefix]) ++prefix;
    // Never split a surrogate pair at the prefix boundary.
    if (prefix > 0 && prefix < previous.size() && previous[prefix - 1].isHighSurrogate() &&
        previous[prefix].isLowSurrogate()) {
        --prefix;
    }
    qsizetype suffix = 0;
    while (suffix < commonSize - prefix &&
           previous[previous.size() - suffix - 1] == normalized[normalized.size() - suffix - 1]) {
        ++suffix;
    }
    // Never split a surrogate pair at the suffix boundary either: extend
    // the selected span past the low surrogate instead.
    const qsizetype oldEndUnclamped = previous.size() - suffix;
    if (oldEndUnclamped > 0 && oldEndUnclamped < previous.size() &&
        previous[oldEndUnclamped - 1].isHighSurrogate() &&
        previous[oldEndUnclamped].isLowSurrogate()) {
        --suffix;
    }
    const int oldEnd = static_cast<int>(previous.size() - suffix);
    const QString replacement = normalized.mid(prefix, normalized.size() - prefix - suffix);
    QTextCursor editCursor(edit->document());
    editCursor.setPosition(static_cast<int>(prefix));
    editCursor.setPosition(oldEnd, QTextCursor::KeepAnchor);
    if (replacement.isEmpty()) {
        editCursor.removeSelectedText();
    } else {
        editCursor.insertText(replacement);
    }
    const int newEnd = editCursor.position();
    const auto updatedPosition = [prefix, oldEnd, newEnd](int cursorPosition) {
        if (cursorPosition <= prefix) return cursorPosition;
        if (cursorPosition >= oldEnd) return cursorPosition + newEnd - oldEnd;
        return std::min(cursorPosition, newEnd);
    };

    const int maxPosition = qMax(0, edit->document()->characterCount() - 1);
    QTextCursor restored(edit->document());
    restored.setPosition(qBound(0, updatedPosition(anchor), maxPosition));
    restored.setPosition(qBound(0, updatedPosition(position), maxPosition),
                         QTextCursor::KeepAnchor);
    edit->setTextCursor(restored);
    edit->verticalScrollBar()->setValue(scroll);
}

}
