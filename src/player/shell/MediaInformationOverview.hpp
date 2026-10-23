#pragma once

#include "MediaInformationPresentation.hpp"

#include <QScrollArea>

#include <memory>

namespace mplayer::shell {

class MediaInformationOverview final : public QScrollArea {
public:
    explicit MediaInformationOverview(QWidget* parent = nullptr);
    ~MediaInformationOverview() override;

    void setDocument(const MediaInformationDocument& document);

private:
    struct Content;
    std::unique_ptr<Content> content_;
};

}
