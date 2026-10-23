#pragma once

#include "MediaInformationPresentation.hpp"

#include <QTreeWidget>

class QKeyEvent;
class QResizeEvent;

namespace mplayer::shell {

class MediaInformationTree : public QTreeWidget {
public:
    explicit MediaInformationTree(QWidget* parent = nullptr);
    void setDocument(const MediaInformationDocument& document);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
};

}
