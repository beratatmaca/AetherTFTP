#include "gui/speed_chart_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QDateTime>
#include <cmath>

namespace tftp::gui {

SpeedChartWidget::SpeedChartWidget(QWidget *parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(150);
}

void SpeedChartWidget::addSpeedSample(double bytesPerSec) {
    m_samples.append(bytesPerSec);
    if (m_samples.size() > m_maxSamples) {
        m_samples.removeFirst();
    }
    update();
}

void SpeedChartWidget::clear() {
    m_samples.clear();
    update();
}

static QString formatSpeed(double bytesPerSec) {
    if (bytesPerSec < 1024.0) {
        return QString::number(bytesPerSec, 'f', 0) + QStringLiteral(" B/s");
    } else if (bytesPerSec < 1024.0 * 1024.0) {
        return QString::number(bytesPerSec / 1024.0, 'f', 1) + QStringLiteral(" KB/s");
    } else {
        return QString::number(bytesPerSec / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB/s");
    }
}

void SpeedChartWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    const int padding = 20;

    // Draw background
    QColor bg = m_bgColor;
    bg.setAlpha(235);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bg);
    painter.drawRoundedRect(rect(), 6, 6);

    // Draw border
    painter.setPen(QPen(m_gridColor, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);

    // If no samples, draw empty state
    if (m_samples.isEmpty()) {
        painter.setPen(m_textColor);
        painter.drawText(rect(), Qt::AlignCenter, tr("No active transfers to chart"));
        return;
    }

    // Find max value in history to scale Y axis
    double maxVal = 1024.0;  // minimum scale of 1 KB/s
    for (double s : m_samples) {
        if (s > maxVal) {
            maxVal = s;
        }
    }
    // Round maxVal to next clean interval
    maxVal = std::ceil(maxVal * 1.15);  // Add 15% headroom

    const int plotWidth = w - padding * 2;
    const int plotHeight = h - padding * 2;

    // Draw horizontal grid lines
    painter.setPen(QPen(m_gridColor, 1, Qt::DashLine));
    for (int i = 1; i <= 3; ++i) {
        int y = padding + (plotHeight * i) / 4;
        painter.drawLine(padding, y, w - padding, y);
    }

    // Draw scale text
    painter.setPen(m_textColor);
    painter.drawText(padding + 5, padding + 12, formatSpeed(maxVal));
    painter.drawText(padding + 5, padding + plotHeight / 2 + 5, formatSpeed(maxVal / 2.0));

    // Map samples to coordinates
    QVector<QPointF> points;
    const int sampleCount = m_samples.size();

    // We want to stretch points to fit the available space
    const double stepX = double(plotWidth) / double(m_maxSamples - 1);

    for (int i = 0; i < sampleCount; ++i) {
        // Place samples starting from the right
        int indexInPlot = m_maxSamples - sampleCount + i;
        double x = padding + indexInPlot * stepX;
        double y = (h - padding) - (m_samples[i] / maxVal) * plotHeight;
        points.append(QPointF(x, y));
    }

    // Create path for line and area fill
    QPainterPath linePath;
    if (!points.isEmpty()) {
        linePath.moveTo(points.first());
        for (int i = 1; i < points.size(); ++i) {
            linePath.lineTo(points[i]);
        }
    }

    // Fill area below line
    QLinearGradient areaGradient(0, padding, 0, h - padding);
    QColor startColor = m_lineColor;
    startColor.setAlpha(80);
    QColor endColor = m_lineColor;
    endColor.setAlpha(0);
    areaGradient.setColorAt(0.0, startColor);
    areaGradient.setColorAt(1.0, endColor);

    QPainterPath areaPath = linePath;
    if (!points.isEmpty()) {
        areaPath.lineTo(points.last().x(), h - padding);
        areaPath.lineTo(points.first().x(), h - padding);
        areaPath.closeSubpath();
        painter.fillPath(areaPath, areaGradient);
    }

    // Draw the glow/main line
    QPen linePen(m_lineColor, 2.5);
    painter.setPen(linePen);
    painter.drawPath(linePath);

    // Draw point at current speed
    if (!points.isEmpty()) {
        painter.setBrush(m_lineColor);
        painter.setPen(QPen(m_bgColor, 1.5));
        painter.drawEllipse(points.last(), 4.5, 4.5);
    }
}

}  // namespace tftp::gui
