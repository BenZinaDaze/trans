#include "controller.h"

namespace Tran {

TranslationController::TranslationController(ProviderRegistry &registry, AppSettings &settings, QObject *parent)
    : QObject(parent), m_registry(registry), m_settings(settings)
{
}

TranslationController::~TranslationController()
{
    invalidateRequest();
}

void TranslationController::invalidateRequest()
{
    ++m_generation;
    if (m_job) {
        const auto previous = m_job;
        m_job.clear();
        previous->cancel();
    }
}

void TranslationController::translateText(const QString &text)
{
    // Copy first: retry() passes m_source back into this function.
    const QString input = text.trimmed();
    invalidateRequest();
    // Keep the result card's labels tied to this request if settings change while it is open.
    const auto request = m_settings.request(input);
    m_providerId = m_settings.providerId();
    m_sourceLanguage = request.sourceLanguage;
    m_targetLanguage = request.targetLanguage;
    m_source = input;
    m_translation.clear();
    m_detected.clear();
    m_message.clear();
    m_status = QStringLiteral("error");
    if (input.isEmpty()) {
        m_message = QStringLiteral("请先在其他应用中选中一个词或一句话，再按翻译快捷键。");
    } else if (input.size() > m_settings.maxInputChars()) {
        m_message = QStringLiteral("选中的文本过长，当前上限为 %1 个字符，可在设置中修改。").arg(m_settings.maxInputChars());
    } else if (auto *provider = m_registry.find(m_settings.providerId())) {
        const auto config = m_settings.config(m_settings.providerId());
        m_message = validateConfig(provider->descriptor(), config);
        if (m_message.isEmpty()) {
            m_status = QStringLiteral("loading");
            m_job = provider->translate(request, config, this);
            const auto generation = m_generation;
            connect(m_job, &TranslationJob::succeeded, this, [this, generation](const TranslationResult &result) {
                if (generation != m_generation)
                    return;
                m_job.clear();
                m_translation = result.text;
                m_detected = result.detectedSourceLanguage;
                m_status = QStringLiteral("success");
                emit stateChanged();
            });
            connect(m_job, &TranslationJob::failed, this, [this, generation](const TranslationError &error) {
                if (generation != m_generation)
                    return;
                m_job.clear();
                m_status = error.code == ErrorCode::Cancelled ? QStringLiteral("cancelled") : QStringLiteral("error");
                m_message = error.message;
                emit stateChanged();
            });
        }
    } else {
        m_message = QStringLiteral("请选择可用的翻译服务。");
    }
    emit stateChanged();
}

void TranslationController::selectionError(const QString &message)
{
    invalidateRequest();
    m_providerId.clear();
    m_sourceLanguage.clear();
    m_targetLanguage.clear();
    m_source.clear();
    m_translation.clear();
    m_detected.clear();
    m_status = QStringLiteral("error");
    m_message = message;
    emit stateChanged();
}

void TranslationController::retry() { translateText(m_source); }

void TranslationController::cancel()
{
    if (!busy())
        return;
    invalidateRequest();
    m_status = QStringLiteral("cancelled");
    m_message = QStringLiteral("翻译已取消。");
    emit stateChanged();
}

} // namespace Tran
