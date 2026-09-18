#pragma once

#include "translation.h"

#include <QNetworkAccessManager>
#include <memory>
#include <vector>

namespace Trans {

enum class HttpProtocol { OpenAI, DeepSeek };
QNetworkRequest providerNetworkRequest(const ProviderConfig &config, const QString &suffix);

class HttpTranslationProvider final : public TranslationProvider {
public:
    explicit HttpTranslationProvider(HttpProtocol protocol);
    ProviderDescriptor descriptor() const override;
    TranslationJob *translate(const TranslationRequest &request,
                              const ProviderConfig &config, QObject *owner) override;
private:
    HttpProtocol m_protocol;
    QNetworkAccessManager m_network;
};

class ProviderRegistry {
public:
    void add(std::unique_ptr<TranslationProvider> provider);
    TranslationProvider *find(const QString &id) const;
    QVariantList descriptors() const;
    static ProviderRegistry builtins();
private:
    std::vector<std::unique_ptr<TranslationProvider>> m_providers;
};

} // namespace Trans
