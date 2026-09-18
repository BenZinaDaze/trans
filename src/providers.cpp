#include "providers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

namespace Trans {

QNetworkRequest providerNetworkRequest(const ProviderConfig &config, const QString &suffix)
{
    QUrl url(config.endpoint);
    QString path = url.path();
    while (path.endsWith('/'))
        path.chop(1);
    url.setPath(path + suffix);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const auto headers = QJsonDocument::fromJson(config.headersJson.toUtf8()).object();
    for (auto it = headers.begin(); it != headers.end(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    if (!config.apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + config.apiKey.toUtf8());
    return request;
}

namespace {

QJsonObject requestBody(const TranslationRequest &request, const ProviderConfig &config)
{
    auto body = QJsonDocument::fromJson(config.optionsJson.toUtf8()).object();
    body.insert("model", config.model);
    body.insert("stream", false);
    const bool responses = config.apiMode == "responses";
    if (responses) {
        body.insert("store", false);
        body.insert("instructions", renderSystemPrompt(request));
        body.insert("input", request.text);
    } else {
        body.insert("messages", QJsonArray{
            QJsonObject{{"role", "system"}, {"content", renderSystemPrompt(request)}},
            QJsonObject{{"role", "user"}, {"content", request.text}}});
    }
    if (config.temperatureEnabled)
        body.insert("temperature", config.temperature);
    if (config.maxOutputTokens > 0)
        body.insert(responses ? "max_output_tokens" : config.id == "deepseek" ? "max_tokens" : "max_completion_tokens", config.maxOutputTokens);
    if (config.reasoning != "default") {
        if (config.id == "deepseek") {
            body.insert("thinking", QJsonObject{{"type", config.reasoning == "none" ? "disabled" : "enabled"}});
            if (config.reasoning != "none")
                body.insert("reasoning_effort", config.reasoning);
        } else if (responses) {
            body.insert("reasoning", QJsonObject{{"effort", config.reasoning}});
        } else {
            body.insert("reasoning_effort", config.reasoning);
        }
    }
    return body;
}

class HttpJob final : public TranslationJob {
public:
    HttpJob(QNetworkAccessManager *network, const TranslationRequest &request, const ProviderConfig &config,
            const QString &validationError, QObject *owner)
        : TranslationJob(owner), m_responses(config.apiMode == "responses"), m_maxBytes(request.maxResponseBytes)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            fail(ErrorCode::Timeout, QStringLiteral("翻译请求超时，请重试或在设置中延长超时。"));
        });
        QTimer::singleShot(0, this, [this, network, request, config, validationError] {
            if (m_finished)
                return;
            if (!validationError.isEmpty()) {
                fail(ErrorCode::Configuration, validationError);
                return;
            }
            const auto httpRequest = providerNetworkRequest(config, m_responses ? "/responses" : "/chat/completions");
            m_reply = network->post(httpRequest, QJsonDocument(requestBody(request, config)).toJson(QJsonDocument::Compact));
            m_reply->setReadBufferSize(m_maxBytes + 1);
            connect(m_reply, &QIODevice::readyRead, this, [this] { readAvailable(); });
            connect(m_reply, &QNetworkReply::finished, this, [this] { receive(); });
            m_timer.start(request.timeoutMs);
        });
    }

    ~HttpJob() override { disposeReply(); }
    void cancel() override { fail(ErrorCode::Cancelled, QStringLiteral("翻译已取消。")); }

private:
    void disposeReply()
    {
        if (m_reply) {
            disconnect(m_reply, nullptr, this, nullptr);
            if (!m_reply->isFinished())
                m_reply->abort();
            m_reply->deleteLater();
            m_reply.clear();
        }
    }

    void fail(ErrorCode code, const QString &message)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_timer.stop();
        disposeReply();
        emit failed({code, message});
        deleteLater();
    }

    bool readAvailable()
    {
        if (m_finished || !m_reply)
            return false;
        m_body += m_reply->readAll();
        if (m_body.size() > m_maxBytes) {
            fail(ErrorCode::InvalidResponse, QStringLiteral("服务返回的内容超过设置中的响应大小上限。"));
            return false;
        }
        return true;
    }

    void receive()
    {
        if (!readAvailable())
            return;
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 || status == 403) {
            fail(ErrorCode::Authentication, QStringLiteral("服务鉴权失败，请在设置中检查 API 密钥和访问权限。"));
            return;
        }
        if (status == 429) {
            fail(ErrorCode::RateLimit, QStringLiteral("请求过于频繁或服务额度已用完，请稍后重试。"));
            return;
        }
        if (status >= 300) {
            fail(ErrorCode::Network, QStringLiteral("服务返回 HTTP %1，请检查服务地址、模型和请求参数。").arg(status));
            return;
        }
        if (m_reply->error() != QNetworkReply::NoError) {
            fail(ErrorCode::Network, QStringLiteral("无法连接翻译服务，请检查网络和服务地址。"));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(m_body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(ErrorCode::InvalidResponse, QStringLiteral("翻译服务返回了无效的 JSON。"));
            return;
        }
        const auto root = document.object();
        TranslationResult result;
        bool incomplete = false;
        if (m_responses) {
            incomplete = root.value("status").toString() != "completed";
            QStringList texts;
            for (const auto &item : root.value("output").toArray()) {
                const auto message = item.toObject();
                if (message.value("type") != "message" || message.value("role") != "assistant")
                    continue;
                for (const auto &part : message.value("content").toArray()) {
                    const auto content = part.toObject();
                    if (content.value("type") == "output_text")
                        texts.append(content.value("text").toString());
                }
            }
            result.text = texts.join('\n');
        } else {
            const auto choice = root.value("choices").toArray().first().toObject();
            result.text = choice.value("message").toObject().value("content").toString();
            const auto reason = choice.value("finish_reason").toString();
            incomplete = !reason.isEmpty() && reason != "stop";
        }
        if (result.text.trimmed().isEmpty() || incomplete) {
            fail(ErrorCode::InvalidResponse, QStringLiteral("服务没有返回完整译文，请检查模型、推理设置和输出长度后重试。"));
            return;
        }
        m_finished = true;
        m_timer.stop();
        disposeReply();
        emit succeeded(result);
        deleteLater();
    }

    bool m_responses;
    int m_maxBytes;
    QTimer m_timer;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_body;
    bool m_finished = false;
};

} // namespace

HttpTranslationProvider::HttpTranslationProvider(HttpProtocol protocol) : m_protocol(protocol) {}

ProviderDescriptor HttpTranslationProvider::descriptor() const
{
    if (m_protocol == HttpProtocol::OpenAI)
        return {"openai", "OpenAI", "https://api.openai.com/v1", "基础地址，保留 /v1；也可填写自己的 OpenAI 代理地址。", true, true,
                "gpt-5.6-luna", {"gpt-5.6-luna"}, {"default", "none", "minimal", "low", "medium", "high", "xhigh"}};
    return {"deepseek", "DeepSeek", "https://api.deepseek.com", "基础地址；程序自动追加 /chat/completions 或 /models。", true, true,
            "deepseek-flash", {"deepseek-flash", "deepseek-v4-pro"}, {"default", "none", "low", "high", "max"}};
}

TranslationJob *HttpTranslationProvider::translate(const TranslationRequest &request,
                                                   const ProviderConfig &config, QObject *owner)
{
    return new HttpJob(&m_network, request, config, validateConfig(descriptor(), config), owner);
}

void ProviderRegistry::add(std::unique_ptr<TranslationProvider> provider)
{
    Q_ASSERT(provider && !find(provider->descriptor().id));
    m_providers.push_back(std::move(provider));
}

TranslationProvider *ProviderRegistry::find(const QString &id) const
{
    for (const auto &provider : m_providers) {
        if (provider->descriptor().id == id)
            return provider.get();
    }
    return nullptr;
}

QVariantList ProviderRegistry::descriptors() const
{
    QVariantList result;
    for (const auto &provider : m_providers)
        result.append(provider->descriptor().toVariant());
    return result;
}

ProviderRegistry ProviderRegistry::builtins()
{
    ProviderRegistry registry;
    for (auto protocol : {HttpProtocol::OpenAI, HttpProtocol::DeepSeek})
        registry.add(std::make_unique<HttpTranslationProvider>(protocol));
    return registry;
}

} // namespace Trans
