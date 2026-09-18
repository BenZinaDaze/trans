#include "controller.h"

namespace Trans {

TranslationController::TranslationController(ProviderRegistry &registry, AppSettings &settings, QObject *parent, BaiduOcrProvider *ocr)
    : QObject(parent), m_registry(registry), m_settings(settings), m_ocrProvider(ocr ? ocr : &m_ocr)
{
}

TranslationController::~TranslationController()
{
    invalidateRequest();
}

void TranslationController::invalidateRequest()
{
    ++m_generation;
    if (m_captureJob) {
        auto previous = m_captureJob;
        m_captureJob.clear();
        previous->cancel();
    }
    if (m_ocrJob) {
        auto previous = m_ocrJob;
        m_ocrJob.clear();
        previous->cancel();
    }
    if (m_job) {
        const auto previous = m_job;
        m_job.clear();
        previous->cancel();
    }
}

void TranslationController::translateText(const QString &text)
{
    startTranslation(text, false);
}

void TranslationController::startTranslation(const QString &text, bool sourceIsOcr)
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
    m_sourceIsOcr = sourceIsOcr && !input.isEmpty();
    m_translation.clear();
    m_detected.clear();
    m_message.clear();
    m_status = QStringLiteral("error");
    if (input.isEmpty()) {
        m_message = QStringLiteral("请先在其他应用中选中一个词或一句话，再按翻译快捷键。");
    } else if (input.size() > m_settings.maxInputChars()) {
        m_message = QStringLiteral("输入文本过长，当前上限为 %1 个字符，可在设置中修改。").arg(m_settings.maxInputChars());
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
    m_sourceIsOcr = false;
    m_translation.clear();
    m_detected.clear();
    m_status = QStringLiteral("error");
    m_message = message;
    emit stateChanged();
}

void TranslationController::translateScreenshot(ScreenshotJob *job)
{
    const auto previousStatus = m_status == "capturing" ? m_beforeCaptureStatus
        : (busy() ? QStringLiteral("cancelled") : m_status);
    invalidateRequest();
    m_beforeCaptureStatus = previousStatus;
    m_captureJob = job;
    m_status = QStringLiteral("capturing");
    const auto generation = m_generation;
    const auto values = m_settings.snapshot();
    const OcrConfig config{values.value("ocrApiKey").toString(), values.value("ocrSecretKey").toString(),
                           values.value("timeoutSeconds").toInt() * 1000};
    connect(job, &ScreenshotJob::succeeded, this, [this, generation, config](const QImage &image) {
        if (generation != m_generation) return;
        m_captureJob.clear();
        m_source.clear();
        m_sourceIsOcr = false;
        m_translation.clear();
        m_detected.clear();
        m_providerId.clear();
        m_sourceLanguage.clear();
        m_targetLanguage.clear();
        m_message.clear();
        m_status = QStringLiteral("recognizing");
        m_ocrJob = m_ocrProvider->recognize(image, config, this);
        connect(m_ocrJob, &OcrJob::succeeded, this, [this, generation](const QString &text) {
            if (generation != m_generation) return;
            m_ocrJob.clear();
            startTranslation(text, true);
        });
        connect(m_ocrJob, &OcrJob::failed, this, [this, generation](const TranslationError &error) {
            if (generation != m_generation) return;
            m_ocrJob.clear();
            selectionError(error.message);
        });
        emit stateChanged();
        emit captureFinished(false);
    });
    connect(job, &ScreenshotJob::failed, this, [this, generation](const TranslationError &error) {
        if (generation != m_generation) return;
        m_captureJob.clear();
        if (error.code == ErrorCode::Cancelled) {
            m_status = m_beforeCaptureStatus;
            emit stateChanged();
        } else {
            selectionError(error.message);
        }
        emit captureFinished(error.code == ErrorCode::Cancelled);
    });
    emit stateChanged();
}

void TranslationController::retry()
{
    if (!busy() && !m_source.isEmpty()) startTranslation(m_source, m_sourceIsOcr);
}

void TranslationController::cancel()
{
    if (!busy())
        return;
    const bool capturing = m_status == "capturing";
    invalidateRequest();
    if (capturing) {
        m_status = m_beforeCaptureStatus;
        emit stateChanged();
        emit captureFinished(true);
        return;
    }
    m_status = QStringLiteral("cancelled");
    m_message = QStringLiteral("请求已取消。");
    emit stateChanged();
}

} // namespace Trans
