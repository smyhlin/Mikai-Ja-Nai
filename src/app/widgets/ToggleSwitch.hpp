#pragma once

#include <QAbstractButton>
#include <QWindow>

namespace mikai::app {

/// The pill switch the customization pages use for on/off preferences: a
/// painted track and knob rather than a check box, so the state reads at a
/// glance. It is a QAbstractButton, so Space toggles it and assistive
/// technology sees a checkable button with a checked state.
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT

public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    bool hitButton(const QPoint& position) const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

}  // namespace mikai::app
