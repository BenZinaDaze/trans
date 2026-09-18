#pragma once

#include "providers.h"
#include <QtQml/qqmlregistration.h>

namespace Trans {

QString validateShortcut(const QString &shortcut);

class AppSettings final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(QString providerId READ providerId NOTIFY settingsChanged)
    Q_PROPERTY(QString targetLanguage READ targetLanguage NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList providers READ providers CONSTANT)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY settingsChanged)
    Q_PROPERTY(bool stayOnTop READ stayOnTop NOTIFY settingsChanged)
public:
    AppSettings(const ProviderRegistry &registry, const QString &path, QObject *parent = nullptr);
    QString providerId() const { return m_values.value("providerId").toString(); }
    QString targetLanguage() const { return m_values.value("targetLanguage").toString(); }
    QVariantList providers() const { return m_registry.descriptors(); }
    QVariantList languages() const;
    QString lastError() const { return m_error; }
    QString configPath() const { return m_path; }
    int fontSize() const { return m_values.value("fontSize").toInt(); }
    bool stayOnTop() const { return m_values.value("stayOnTop").toBool(); }
    bool restoreFocus() const { return m_values.value("restoreFocus").toBool(); }
    QString popupPosition() const { return m_values.value("popupPosition").toString(); }
    QString shortcut() const { return m_values.value("shortcut").toString(); }
    int maxInputChars() const { return m_values.value("maxInputChars").toInt(); }
    ProviderConfig config(const QString &id) const;
    TranslationRequest request(const QString &text) const;
    static TranslationRequest requestFromSnapshot(const QString &text, const QVariantMap &values);
    bool isConfigured() const;
    QString validate(const QVariantMap &values) const;
    Q_INVOKABLE QVariantMap snapshot() const { return m_values; }
    Q_INVOKABLE QVariantMap defaults() const;
    Q_INVOKABLE bool save(const QVariantMap &values);
signals:
    void settingsChanged();
    void errorChanged();
private:
    bool persist(const QVariantMap &values);
    bool setError(const QString &error);
    const ProviderRegistry &m_registry;
    QString m_path;
    QVariantMap m_values;
    QString m_error;
};

} // namespace Trans
