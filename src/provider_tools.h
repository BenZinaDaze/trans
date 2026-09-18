#pragma once

#include "settings.h"
#include <QPointer>
#include <QTimer>

class QNetworkReply;

namespace Trans {

class ProviderTools final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QStringList models READ models NOTIFY changed)
public:
    explicit ProviderTools(ProviderRegistry &registry, QObject *parent = nullptr);
    ~ProviderTools() override;
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    QStringList models() const { return m_models; }
    Q_INVOKABLE void fetchModels(const QString &id, const QVariantMap &snapshot);
    Q_INVOKABLE void testTranslation(const QString &id, const QVariantMap &snapshot);
    Q_INVOKABLE void clear();
signals:
    void changed();
private:
    void cancel();
    void fail(const QString &message);
    void receiveModels();
    ProviderRegistry &m_registry;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QPointer<TranslationJob> m_job;
    QTimer m_timer;
    QByteArray m_body;
    int m_maxBytes = 0;
    bool m_busy = false;
    QString m_message;
    QStringList m_models;
};

} // namespace Trans
