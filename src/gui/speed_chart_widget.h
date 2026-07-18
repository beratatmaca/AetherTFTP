#pragma once

#include <QWidget>
#include <QVector>

namespace tftp::gui {

/**
 * @brief A beautiful, dynamic widget displaying a real-time transfer speed chart.
 */
class SpeedChartWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
    Q_PROPERTY(QColor bgColor READ bgColor WRITE setBgColor)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor)
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)
public:
    explicit SpeedChartWidget(QWidget *parent = nullptr);

    QColor lineColor() const { return m_lineColor; }
    void setLineColor(const QColor &c) {
        m_lineColor = c;
        update();
    }

    QColor bgColor() const { return m_bgColor; }
    void setBgColor(const QColor &c) {
        m_bgColor = c;
        update();
    }

    QColor gridColor() const { return m_gridColor; }
    void setGridColor(const QColor &c) {
        m_gridColor = c;
        update();
    }

    QColor textColor() const { return m_textColor; }
    void setTextColor(const QColor &c) {
        m_textColor = c;
        update();
    }

    /** @brief Append a new speed measurement (in bytes per second) and update. */
    void addSpeedSample(double bytesPerSec);

    /** @brief Clear the history. */
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<double> m_samples;
    int m_maxSamples = 60;  // Keep last 60 seconds

    QColor m_lineColor = QColor(6, 182, 212);    // Default Cyan
    QColor m_bgColor = QColor(30, 41, 59);       // Default Slate
    QColor m_gridColor = QColor(51, 65, 85);     // Default light slate
    QColor m_textColor = QColor(248, 250, 252);  // Default white/slate
};

}  // namespace tftp::gui
