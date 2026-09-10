#include "provider_tools.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>

namespace Tran {

ProviderTools::ProviderTools(ProviderRegistry &registry, QObject *parent) : QObject(parent), m_registry(registry)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { fail(QStringLiteral("获取模型超时，请检查服务地址或延长超时。")); });
}

ProviderTools::~ProviderTools() { cancel(); }

void ProviderTools::cancel()
{
    m_timer.stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    if (m_job) {
        disconnect(m_job, nullptr, this, nullptr);
        m_job->cancel();
        m_job.clear();
    }
    m_busy = false;
}

void ProviderTools::clear()
{
    cancel();
    m_message.clear();
    m_models.clear();
    emit changed();
}

void ProviderTools::fail(const QString &message)
{
    cancel();
    m_message = message;
    emit changed();
}

void ProviderTools::fetchModels(const QString &id, const QVariantMap &snapshot)
{
    clear();
    auto *provider = m_registry.find(id);
    if (!provider)
        return fail(QStringLiteral("未知的提供商。"));
    const auto config = ProviderConfig::fromVariant(id, snapshot.value("providerConfigs").toMap().value(id).toMap());
    auto descriptor = provider->descriptor();
    descriptor.requiresModel = false;
    const auto error = validateConfig(descriptor, config);
    if (!error.isEmpty())
        return fail(error);
    const auto request = AppSettings::requestFromSnapshot({}, snapshot);
    m_maxBytes = request.maxResponseBytes;
    m_body.clear();
    m_reply = m_network.get(providerNetworkRequest(config, "/models"));
    m_reply->setReadBufferSize(m_maxBytes + 1);
    connect(m_reply, &QIODevice::readyRead, this, [this] {
        m_body += m_reply->readAll();
        if (m_body.size() > m_maxBytes)
            fail(QStringLiteral("模型列表超过设置中的响应大小上限。"));
    });
    connect(m_reply, &QNetworkReply::finished, this, &ProviderTools::receiveModels);
    m_busy = true;
    m_message = QStringLiteral("正在获取模型…");
    m_timer.start(request.timeoutMs);
    emit changed();
}

void ProviderTools::receiveModels()
{
    m_body += m_reply->readAll();
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403)
        return fail(QStringLiteral("鉴权失败，请检查此提供商的 API 密钥。"));
    if (status >= 300 || m_reply->error() != QNetworkReply::NoError)
        return fail(QStringLiteral("无法获取模型列表（HTTP %1）。仍可直接填写模型名称。").arg(status));
    const auto document = QJsonDocument::fromJson(m_body);
    if (m_body.size() > m_maxBytes || !document.isObject() || !document.object().value("data").isArray())
        return fail(QStringLiteral("服务返回了无效的模型列表。"));
    for (const auto &item : document.object().value("data").toArray()) {
        const auto id = item.toObject().value("id").toString();
        if (!id.isEmpty())
            m_models.append(id);
    }
    m_models.removeDuplicates();
    m_models.sort();
    cancel();
    m_message = m_models.isEmpty() ? QStringLiteral("该密钥没有返回可用模型，可以直接填写模型名称。")
                                 : QStringLiteral("已获取 %1 个模型，可从列表选择。").arg(m_models.size());
    emit changed();
}

void ProviderTools::testTranslation(const QString &id, const QVariantMap &snapshot)
{
    clear();
    auto *provider = m_registry.find(id);
    if (!provider)
        return fail(QStringLiteral("未知的提供商。"));
    const auto config = ProviderConfig::fromVariant(id, snapshot.value("providerConfigs").toMap().value(id).toMap());
    m_job = provider->translate(AppSettings::requestFromSnapshot(QStringLiteral("Hello, world!"), snapshot), config, this);
    connect(m_job, &TranslationJob::succeeded, this, [this](const TranslationResult &result) {
        m_job.clear();
        m_busy = false;
        m_message = QStringLiteral("测试成功：") + result.text;
        emit changed();
    });
    connect(m_job, &TranslationJob::failed, this, [this](const TranslationError &error) {
        m_job.clear();
        fail(error.message);
    });
    m_busy = true;
    m_message = QStringLiteral("正在使用当前页面的配置翻译 Hello, world!…");
    emit changed();
}

} // namespace Tran
