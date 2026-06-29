#include "gui/server_config_dialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

namespace tftp::gui {

ServerConfigDialog::ServerConfigDialog(quint16 port, const QString &rootDir, int maxConcurrent, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Server Configuration"));
    auto *layout = new QFormLayout(this);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(port == 0 ? 69 : port);
    m_portSpin->setToolTip(tr("UDP port the server listens on (1–65535)."));
    layout->addRow(tr("&Port:"), m_portSpin);

    auto *dirLayout = new QHBoxLayout();
    m_dirEdit = new QLineEdit(rootDir, this);
    m_dirEdit->setToolTip(tr("Directory whose files are served and into which uploads are written."));
    dirLayout->addWidget(m_dirEdit);
    auto *browseButton = new QPushButton(tr("&Browse…"), this);
    connect(browseButton, &QPushButton::clicked, this, &ServerConfigDialog::browseDir);
    dirLayout->addWidget(browseButton);
    layout->addRow(tr("Root &Directory:"), dirLayout);

    m_maxSpin = new QSpinBox(this);
    m_maxSpin->setRange(1, 100);
    m_maxSpin->setValue(maxConcurrent);
    m_maxSpin->setToolTip(tr("Maximum number of GUI-initiated transfers running at once."));
    layout->addRow(tr("&Max Concurrent Transfers:"), m_maxSpin);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setObjectName(QStringLiteral("statusStopped"));
    m_hintLabel->setWordWrap(true);
    layout->addRow(m_hintLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okButton = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);

    connect(m_dirEdit, &QLineEdit::textChanged, this, &ServerConfigDialog::validate);
    validate();
}

quint16 ServerConfigDialog::port() const {
    return quint16(m_portSpin->value());
}

QString ServerConfigDialog::rootDir() const {
    return m_dirEdit->text();
}

int ServerConfigDialog::maxConcurrent() const {
    return m_maxSpin->value();
}

void ServerConfigDialog::browseDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Root Directory"), m_dirEdit->text());
    if (!dir.isEmpty())
        m_dirEdit->setText(dir);
}

void ServerConfigDialog::validate() {
    const QString dir = m_dirEdit->text().trimmed();
    const bool ok = !dir.isEmpty() && QDir(dir).exists();
    if (m_okButton)
        m_okButton->setEnabled(ok);
    m_hintLabel->setText(ok ? QString() : tr("Root directory does not exist."));
}

}  // namespace tftp::gui
