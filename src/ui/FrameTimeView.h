#pragma once

#include <QPoint>
#include <QWidget>

namespace framescope::core {
class CaptureSession;
}

namespace framescope::ui {

// Frame-time graph with spike markers.
//  - Follows the newest frame while live (double-click to re-enable).
//  - Mouse wheel zooms the time span, drag pans.
//  - Long sessions are drawn as per-pixel min/max, so it stays fast.
class FrameTimeView : public QWidget {
    Q_OBJECT

public:
    explicit FrameTimeView(QWidget* parent = nullptr);

    void setSession(const core::CaptureSession* session);
    void setFollow(bool follow);
    void setSpanSeconds(double seconds);
    void focusTime(double seconds); // centre the view on a time, stop following
    void showAll();

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] double viewEnd() const;

    const core::CaptureSession* m_session = nullptr;
    bool m_follow = true;
    double m_spanSeconds = 30.0;
    double m_viewEnd = 0.0;

    QPoint m_dragStart;
    double m_dragViewEnd = 0.0;
};

} // namespace framescope::ui
