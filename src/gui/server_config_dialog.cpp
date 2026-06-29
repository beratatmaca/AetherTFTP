#include "gui/server_config_dialog.h"
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QFileDialog>
#include <QHBoxLayout>

namespace tftp::gui {

ServerConfigDialog::ServerConfigDialog(quint16 port, const QString &rootDir, int maxConcurrent, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Server Configuration"));
    auto *layout = new QFormLayout(this);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(0, 65535);
    m_portSpin->setValue(port);
    layout->addRow(tr("Port:"), m_portSpin);

    auto *dirLayout = new QHBoxLayout();
    m_dirEdit = new QLineEdit(rootDir, this);
    dirLayout->addWidget(m_dirEdit);
    auto *browseButton = new QPushButton(tr("Browse..."), this);
    connect(browseButton, &QPushButton::clicked, this, &ServerConfigDialog::browseDir);
    dirLayout->addWidget(browseButton);
    layout->addRow(tr("Root Directory:"), dirLayout);

    m_maxSpin = new QSpinBox(this);
    m_maxSpin->setRange(1, 100);
    m_maxSpin->setValue(maxConcurrent);
    layout->addRow(tr("Max Concurrent Transfers:"), m_maxSpin);

    auto *buttonsLayout = new QHBoxLayout();
    auto *okButton = new QPushButton(tr("OK"), this);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonsLayout->addWidget(okButton);
    buttonsLayout->addWidget(cancelButton);
    layout->addRow(buttonsLayout);
}

quint16 ServerConfigDialog::port() const {
    return m_portSpin->value();
}

QString ServerConfigDialog::rootDir() const {
    return m_dirEdit->text();
}

int ServerConfigDialog::maxConcurrent() const {
    return m_maxSpin->value();
}

void ServerConfigDialog::browseDir() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Root Directory"), m_dirEdit->text());
    if (!dir.isEmpty()) {
        m_dirEdit->setText(dir);
    }
}

}  // namespace tftp::gui
