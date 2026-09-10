#pragma once

#include "settings.h"
#include <QPointer>

namespace Tran {

class TranslationController final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(QString sourceText READ sourceText NOTIFY stateChanged)
    Q_PROPERTY(QString translatedText READ translatedText NOTIFY stateChanged)
    Q_PROPERTY(QString detectedLanguage READ detectedLanguage NOTIFY stateChanged)
    Q_PROPERTY(QString providerId READ providerId NOTIFY stateChanged)
    Q_PROPERTY(QString sourceLanguage READ sourceLanguage NOTIFY stateChanged)
    Q_PROPERTY(QString targetLanguage READ targetLanguage NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
public:
    TranslationController(ProviderRegistry &registry, AppSettings &settings, QObject *parent = nullptr);
    ~TranslationController() override;
    QString sourceText() const { return m_source; }
    QString translatedText() const { return m_translation; }
    QString detectedLanguage() const { return m_detected; }
    QString providerId() const { return m_providerId; }
    QString sourceLanguage() const { return m_sourceLanguage; }
    QString targetLanguage() const { return m_targetLanguage; }
    QString status() const { return m_status; }
    QString message() const { return m_message; }
    bool busy() const { return m_status == "loading"; }
    void translateText(const QString &text);
    void selectionError(const QString &message);
    Q_INVOKABLE void retry();
    Q_INVOKABLE void cancel();
signals:
    void stateChanged();
private:
    void invalidateRequest();
    ProviderRegistry &m_registry;
    AppSettings &m_settings;
    QPointer<TranslationJob> m_job;
    quint64 m_generation = 0;
    QString m_source;
    QString m_translation;
    QString m_detected;
    QString m_providerId;
    QString m_sourceLanguage;
    QString m_targetLanguage;
    QString m_status = QStringLiteral("idle");
    QString m_message;
};

} // namespace Tran
