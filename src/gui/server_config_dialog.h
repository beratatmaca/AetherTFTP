#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;
class QSpinBox;

namespace tftp::gui {

/**
 * @brief Modal dialog for configuring server-mode settings.
 *
 * Collects the listen port, served root directory, and a cap on concurrent
 * GUI-initiated transfers. Values are read back via the accessors after the
 * dialog is accepted.
 */
class ServerConfigDialog : public QDialog {
    Q_OBJECT
public:
    /**
     * @brief Construct the dialog pre-filled with current settings.
     * @param port Initial listen port.
     * @param rootDir Initial served directory.
     * @param maxConcurrent Initial concurrent-transfer cap.
     * @param parent Optional parent widget.
     */
    ServerConfigDialog(quint16 port, const QString &rootDir, int maxConcurrent, QWidget *parent = nullptr);

    /** @return The chosen listen port. */
    quint16 port() const;
    /** @return The chosen served root directory. */
    QString rootDir() const;
    /** @return The chosen concurrent-transfer cap. */
    int maxConcurrent() const;

private slots:
    /** @brief Open a directory picker for the root directory field. */
    void browseDir();

private:
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_dirEdit = nullptr;
    QSpinBox *m_maxSpin = nullptr;
};

}  // namespace tftp::gui
