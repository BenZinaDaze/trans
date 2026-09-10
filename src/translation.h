#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QJsonObject>
#include <QStringList>

namespace Tran {

struct TranslationRequest {
    QString text;
    QString sourceLanguage = QStringLiteral("auto");
    QString targetLanguage = QStringLiteral("zh-CN");
    int timeoutMs = 30000;
    int maxResponseBytes = 2 * 1024 * 1024;
    QString systemPrompt;
};

struct TranslationResult {
    QString text;
    QString detectedSourceLanguage;
};

enum class ErrorCode { Configuration, Authentication, RateLimit, Timeout, Network, InvalidResponse, Cancelled };

struct TranslationError {
    ErrorCode code = ErrorCode::Network;
    QString message;
};

struct ProviderConfig {
    QString id;
    QString endpoint;
    QString model;
    QString apiKey;
    QString apiMode = QStringLiteral("responses");
    bool temperatureEnabled = false;
    double temperature = 0.3;
    int maxOutputTokens = 0;
    QString reasoning = QStringLiteral("none");
    QString headersJson = QStringLiteral("{}");
    QString optionsJson = QStringLiteral("{}");
    QVariantMap toVariant() const;
    static ProviderConfig fromVariant(const QString &id, const QVariantMap &values);
};

struct ProviderDescriptor {
    QString id;
    QString name;
    QString defaultEndpoint;
    QString endpointHint;
    bool requiresModel = false;
    bool requiresKey = false;
    QString defaultModel;
    QStringList models;
    QStringList reasoningOptions;
    QVariantMap toVariant() const;
};

// Every job must complete asynchronously, including validation failures.
// The owner may cancel or destroy it; implementations delete themselves after completion.
class TranslationJob : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void cancel() = 0;
signals:
    void succeeded(const Tran::TranslationResult &result);
    void failed(const Tran::TranslationError &error);
};

class TranslationProvider {
public:
    virtual ~TranslationProvider() = default;
    virtual ProviderDescriptor descriptor() const = 0;
    virtual TranslationJob *translate(const TranslationRequest &request,
                                      const ProviderConfig &config, QObject *owner) = 0;
};

QString defaultSystemPrompt();
QString renderSystemPrompt(const TranslationRequest &request);
QString validateConfig(const ProviderDescriptor &descriptor, const ProviderConfig &config, bool requireReady = true);

} // namespace Tran

Q_DECLARE_METATYPE(Tran::TranslationResult)
Q_DECLARE_METATYPE(Tran::TranslationError)
