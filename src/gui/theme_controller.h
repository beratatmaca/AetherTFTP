#pragma once

#include <QObject>
#include <QString>

class QApplication;

namespace tftp::gui {

/**
 * @brief Applies the application stylesheet and follows the OS colour scheme.
 *
 * Three modes are supported: @c System (track the OS light/dark setting),
 * @c Light, and @c Dark. In @c System mode the effective scheme is resolved
 * from @c QStyleHints::colorScheme() on Qt 6.5+, or from the application
 * palette's lightness on older Qt, and is re-applied live when the OS scheme
 * changes (Qt 6.5+).
 */
class ThemeController : public QObject {
    Q_OBJECT
public:
    /** @brief Theme selection mode. */
    enum class Mode : quint8 { System, Light, Dark, Nord };

    /**
     * @brief Construct and apply the initial theme.
     * @param app The application whose stylesheet is driven.
     * @param parent Optional QObject parent.
     */
    explicit ThemeController(QApplication *app, QObject *parent = nullptr);

    /** @return The current selection mode. */
    Mode mode() const { return m_mode; }

    /** @brief Set the selection mode and re-apply the stylesheet. */
    void setMode(Mode mode);

    /** @brief Parse a persisted mode string ("light"/"dark"/anything else). */
    static Mode modeFromString(const QString &text);
    /** @brief Serialise a mode to a stable string for persistence. */
    static QString modeToString(Mode mode);

private:
    void apply();
    bool effectiveDark() const;

    QApplication *m_app;
    Mode m_mode = Mode::System;
};

}  // namespace tftp::gui
