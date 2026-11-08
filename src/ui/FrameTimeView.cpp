#include "ui/FrameTimeView.h"

#include "core/CaptureSession.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace framescope::ui {
namespace {

constexpr int kLeft = 52;
constexpr int kRight = 12;
constexpr int kTop = 12;
constexpr int kBottom = 26;

const QColor kBackground{0x1b, 0x1d, 0x22};
const QColor kGrid{0x33, 0x37, 0x40};
const QColor kText{0xa0, 0xa6, 0xb2};
const QColor kLine{0x4f, 0xc3, 0xf7};
const QColor kSpikeSystem{0xff, 0x52, 0x52};
const QColor kSpikeGame{0x9c, 0xa3, 0xff};
const QColor kSpikeMixed{0xff, 0xb3, 0x00};
const QColor kSpikePending{0xe0, 0xe0, 0xe0};

// Red = your PC, violet = the game, amber = could be either.
QColor spikeColor(const core::Spike& s)
{
    if (!s.attribution)
        return kSpikePending;
    switch (s.attribution->side) {
    case core::Side::System:  return kSpikeSystem;
    case core::Side::Game:    return kSpikeGame;
    case core::Side::Mixed:   return kSpikeMixed;
    case core::Side::Unknown: break;
    }
    return kSpikePending;
}

double niceTimeStep(double span)
{
    constexpr std::array steps{0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0};
    for (const double s : steps) {
        if (span / s <= 10.0)
            return s;
    }
    return 3600.0;
}

} // namespace

FrameTimeView::FrameTimeView(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(220);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setToolTip(tr("Wheel: zoom · Drag: pan · Double-click: follow live"));
}

void FrameTimeView::setSession(const core::CaptureSession* session)
{
    m_session = session;
    update();
}

void FrameTimeView::setFollow(bool follow)
{
    m_follow = follow;
    update();
}

void FrameTimeView::setSpanSeconds(double seconds)
{
    m_spanSeconds = std::clamp(seconds, 1.0, 6.0 * 3600.0);
    update();
}

void FrameTimeView::focusTime(double seconds)
{
    m_follow = false;
    m_viewEnd = seconds + m_spanSeconds / 2.0;
    update();
}

void FrameTimeView::showAll()
{
    m_follow = false;
    if (m_session && !m_session->frames().empty()) {
        m_viewEnd = m_session->frames().back().timeSeconds;
        m_spanSeconds = std::max(1.0, m_viewEnd);
    }
    update();
}

double FrameTimeView::viewEnd() const
{
    if (m_follow && m_session && !m_session->frames().empty())
        return std::max(m_spanSeconds, m_session->frames().back().timeSeconds);
    return m_viewEnd;
}

void FrameTimeView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kBackground);

    const QRectF plot = QRectF(rect()).adjusted(kLeft, kTop, -kRight, -kBottom);
    if (plot.width() < 10 || plot.height() < 10)
        return;

    const auto* frames = m_session ? &m_session->frames() : nullptr;
    if (!frames || frames->empty()) {
        p.setPen(kText);
        p.drawText(plot, Qt::AlignCenter, tr("No frames yet. Start a capture or open a PresentMon CSV."));
        return;
    }

    const double tEnd = viewEnd();
    const double tStart = tEnd - m_spanSeconds;

    const auto byTime = [](const core::FrameRecord& f, double t) { return f.timeSeconds < t; };
    const auto first = std::lower_bound(frames->begin(), frames->end(), tStart, byTime);
    const auto last = std::lower_bound(first, frames->end(), tEnd, byTime);

    // ---- Y range: always show at least the 30 fps line, cap extreme outliers.
    double visibleMax = 0.0;
    for (auto it = first; it != last; ++it)
        visibleMax = std::max(visibleMax, it->frameTimeMs);
    const double yMax = std::clamp(visibleMax * 1.1, 35.0, 250.0);

    const auto xOf = [&](double t) { return plot.left() + (t - tStart) / m_spanSeconds * plot.width(); };
    const auto yOf = [&](double ms) { return plot.bottom() - std::min(ms, yMax) / yMax * plot.height(); };

    // ---- Grid: frame-time reference lines with their fps.
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);

    constexpr std::array refMs{4.17, 6.94, 8.33, 16.67, 33.33, 50.0, 100.0, 200.0};
    double lastLabelY = std::numeric_limits<double>::max();
    for (const double ms : refMs) {
        if (ms > yMax)
            break;
        const double y = yOf(ms);
        if (lastLabelY - y < 14.0)
            continue; // too close to the previous line to label cleanly
        lastLabelY = y;
        p.setPen(QPen(kGrid, 1, Qt::DashLine));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(kText);
        p.drawText(QRectF(0, y - 8, kLeft - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1 ms").arg(ms, 0, 'f', ms < 10 ? 1 : 0));
    }

    const double step = niceTimeStep(m_spanSeconds);
    for (double t = std::ceil(tStart / step) * step; t <= tEnd; t += step) {
        const double x = xOf(t);
        p.setPen(QPen(kGrid, 1, Qt::DotLine));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(kText);
        p.drawText(QRectF(x - 40, plot.bottom() + 4, 80, 16), Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("%1 s").arg(t, 0, 'f', step < 1.0 ? 1 : 0));
    }

    // ---- Frame times: per-pixel min/max so long sessions stay fast.
    p.setClipRect(plot);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int columns = static_cast<int>(plot.width());
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> colMin(columns, nan);
    std::vector<double> colMax(columns, nan);
    for (auto it = first; it != last; ++it) {
        const int c = std::clamp(static_cast<int>(xOf(it->timeSeconds) - plot.left()), 0, columns - 1);
        if (std::isnan(colMax[c])) {
            colMin[c] = colMax[c] = it->frameTimeMs;
        } else {
            colMin[c] = std::min(colMin[c], it->frameTimeMs);
            colMax[c] = std::max(colMax[c], it->frameTimeMs);
        }
    }

    QPainterPath path;
    bool started = false;
    for (int c = 0; c < columns; ++c) {
        if (std::isnan(colMax[c]))
            continue;
        const double x = plot.left() + c;
        if (!started) {
            path.moveTo(x, yOf(colMin[c]));
            started = true;
        } else {
            path.lineTo(x, yOf(colMin[c]));
        }
        if (colMax[c] != colMin[c])
            path.lineTo(x, yOf(colMax[c]));
    }
    p.setPen(QPen(kLine, 1.2));
    p.drawPath(path);

    // ---- Spikes.
    const auto& spikes = m_session->spikes();
    const auto spikeByTime = [](const core::Spike& s, double t) { return s.timeSeconds < t; };
    auto sIt = std::lower_bound(spikes.begin(), spikes.end(), tStart, spikeByTime);
    p.setPen(Qt::NoPen);
    for (; sIt != spikes.end() && sIt->timeSeconds <= tEnd; ++sIt) {
        p.setBrush(spikeColor(*sIt));
        p.drawEllipse(QPointF(xOf(sIt->timeSeconds), yOf(sIt->frameTimeMs)), 3.5, 3.5);
    }

    p.setClipping(false);
    p.setPen(kGrid);
    p.setBrush(Qt::NoBrush);
    p.drawRect(plot);

    if (m_follow) {
        p.setPen(kText);
        p.drawText(plot.adjusted(0, 4, -6, 0), Qt::AlignRight | Qt::AlignTop, tr("LIVE"));
    }
}

void FrameTimeView::wheelEvent(QWheelEvent* event)
{
    const double factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
    if (!m_follow) {
        // Zoom around the view centre.
        const double centre = m_viewEnd - m_spanSeconds / 2.0;
        setSpanSeconds(m_spanSeconds * factor);
        m_viewEnd = centre + m_spanSeconds / 2.0;
    } else {
        setSpanSeconds(m_spanSeconds * factor);
    }
    event->accept();
}

void FrameTimeView::mousePressEvent(QMouseEvent* event)
{
    m_dragStart = event->position().toPoint();
    m_dragViewEnd = viewEnd();
}

void FrameTimeView::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton))
        return;
    const double plotWidth = std::max(1, width() - kLeft - kRight);
    const double dx = event->position().x() - m_dragStart.x();
    m_follow = false;
    m_viewEnd = m_dragViewEnd - dx / plotWidth * m_spanSeconds;
    update();
}

void FrameTimeView::mouseDoubleClickEvent(QMouseEvent*)
{
    setFollow(true);
}

} // namespace framescope::ui
