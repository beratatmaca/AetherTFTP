#pragma once

#include <QTranslator>
#include <QHash>
#include <QString>

namespace tftp::gui {

class MapTranslator : public QTranslator {
public:
    explicit MapTranslator(QObject *parent = nullptr) : QTranslator(parent) {}

    void addTranslation(const QString &source, const QString &translation) {
        m_translations.insert(source, translation);
    }

    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        QString key = QString::fromUtf8(sourceText);
        if (m_translations.contains(key)) {
            return m_translations.value(key);
        }
        return QString();
    }

    static MapTranslator *create(const QString &lang, QObject *parent = nullptr) {
        auto *t = new MapTranslator(parent);
        if (lang == QStringLiteral("de")) {
            t->addTranslation("Client", "Client");
            t->addTranslation("Server", "Server");
            t->addTranslation("&Theme", "&Design");
            t->addTranslation("&Language", "&Sprache");
            t->addTranslation("&Help", "&Hilfe");
            t->addTranslation("&About AetherTFTP…", "&Über AetherTFTP…");
            t->addTranslation("About AetherTFTP", "Über AetherTFTP");
            t->addTranslation("&View", "&Ansicht");
            t->addTranslation("&System", "&System");
            t->addTranslation("&Light", "&Hell");
            t->addTranslation("&Dark", "&Dunkel");
            t->addTranslation("&Nord", "&Nord");
            t->addTranslation("Save Profile", "Profil speichern");
            t->addTranslation("Delete Profile", "Profil löschen");
            t->addTranslation("Import Profile", "Profil importieren");
            t->addTranslation("Export Profile", "Profil exportieren");
            t->addTranslation("Select a saved client configuration profile.", "Wählen Sie ein gespeichertes Client-Konfigurationsprofil aus.");
            t->addTranslation("Switch the panel below between client and server configuration.", "Schalten Sie das folgende Feld zwischen Client- und Serverkonfiguration um.");
        } else if (lang == QStringLiteral("tr")) {
            t->addTranslation("Client", "İstemci");
            t->addTranslation("Server", "Sunucu");
            t->addTranslation("&Theme", "&Tema");
            t->addTranslation("&Language", "&Dil");
            t->addTranslation("&Help", "&Yardım");
            t->addTranslation("&About AetherTFTP…", "&AetherTFTP Hakkında…");
            t->addTranslation("About AetherTFTP", "AetherTFTP Hakkında");
            t->addTranslation("&View", "&Görünüm");
            t->addTranslation("&System", "&Sistem");
            t->addTranslation("&Light", "&Açık");
            t->addTranslation("&Dark", "&Koyu");
            t->addTranslation("&Nord", "&Kuzey");
            t->addTranslation("Save Profile", "Profili Kaydet");
            t->addTranslation("Delete Profile", "Profili Sil");
            t->addTranslation("Import Profile", "Profili İçe Aktar");
            t->addTranslation("Export Profile", "Profili Dışa Aktar");
            t->addTranslation("Select a saved client configuration profile.", "Kaydedilmiş bir istemci yapılandırma profilini seçin.");
            t->addTranslation("Switch the panel below between client and server configuration.", "Aşağıdaki paneli istemci ve sunucu yapılandırması arasında değiştirin.");
        } else if (lang == QStringLiteral("es")) {
            t->addTranslation("Client", "Cliente");
            t->addTranslation("Server", "Servidor");
            t->addTranslation("&Theme", "&Tema");
            t->addTranslation("&Language", "&Idioma");
            t->addTranslation("&Help", "&Ayuda");
            t->addTranslation("&About AetherTFTP…", "&Acerca de AetherTFTP…");
            t->addTranslation("About AetherTFTP", "Acerca de AetherTFTP");
            t->addTranslation("&View", "&Ver");
            t->addTranslation("&System", "&Sistema");
            t->addTranslation("&Light", "&Claro");
            t->addTranslation("&Dark", "&Oscuro");
            t->addTranslation("&Nord", "&Nórdico");
            t->addTranslation("Save Profile", "Guardar Perfil");
            t->addTranslation("Delete Profile", "Eliminar Perfil");
            t->addTranslation("Import Profile", "Importar Perfil");
            t->addTranslation("Export Profile", "Exportar Perfil");
            t->addTranslation("Select a saved client configuration profile.", "Seleccione un perfil de configuración de cliente guardado.");
            t->addTranslation("Switch the panel below between client and server configuration.", "Cambie el panel inferior entre la configuración del cliente y del servidor.");
        }
        return t;
    }

private:
    QHash<QString, QString> m_translations;
};

} // namespace tftp::gui
