#include "gui/theme_controller.h"

#include <QApplication>
#include <QFile>
#include <QPalette>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

namespace tftp::gui {

ThemeController::ThemeController(QApplication *app, QObject *parent) : QObject(parent), m_app(app) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Track live OS scheme changes; only relevant while in System mode.
    connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
        if (m_mode == Mode::System)
            apply();
    });
#endif
    apply();
}

void ThemeController::setMode(Mode mode) {
    m_mode = mode;
    apply();
}

ThemeController::Mode ThemeController::modeFromString(const QString &text) {
    if (text == QLatin1String("light"))
        return Mode::Light;
    if (text == QLatin1String("dark"))
        return Mode::Dark;
    if (text == QLatin1String("nord"))
        return Mode::Nord;
    return Mode::System;
}

QString ThemeController::modeToString(Mode mode) {
    switch (mode) {
        case Mode::Light:
            return QStringLiteral("light");
        case Mode::Dark:
            return QStringLiteral("dark");
        case Mode::Nord:
            return QStringLiteral("nord");
        case Mode::System:
            break;
    }
    return QStringLiteral("system");
}

bool ThemeController::effectiveDark() const {
    if (m_mode == Mode::Dark)
        return true;
    if (m_mode == Mode::Light)
        return false;
    if (m_mode == Mode::Nord)
        return true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    // Heuristic fallback for Qt < 6.5: a dark window base implies dark mode.
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
#endif
}

void ThemeController::apply() {
    QString path;
    if (m_mode == Mode::Nord) {
        path = QStringLiteral(":/aether/theme-nord.qss");
    } else {
        path = effectiveDark() ? QStringLiteral(":/aether/theme-dark.qss") : QStringLiteral(":/aether/theme-light.qss");
    }
    QFile qss(path);
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        m_app->setStyleSheet(QString::fromUtf8(qss.readAll()));
}

}  // namespace tftp::gui
