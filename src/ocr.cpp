#include "ocr.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>

namespace Trans {
namespace {
QByteArray field(const QString &name, const QString &value)
{
    return QUrl::toPercentEncoding(name) + '=' + QUrl::toPercentEncoding(value);
}
}

class BaiduOcrJob final : public OcrJob {
public:
    BaiduOcrJob(BaiduOcrProvider &provider, QImage image, OcrConfig config, QObject *owner)
        : OcrJob(owner), m_provider(provider), m_config(std::move(config))
    {
        m_credentials = QCryptographicHash::hash(m_config.apiKey.toUtf8() + '\0' + m_config.secretKey.toUtf8(), QCryptographicHash::Sha256);
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { fail(ErrorCode::Timeout, QStringLiteral("OCR 请求超时，请重新截图。")); });
        QTimer::singleShot(0, this, [this, image = std::move(image)] {
            if (m_done) return;
            if (m_config.apiKey.trimmed().isEmpty() || m_config.secretKey.trimmed().isEmpty()) {
                fail(ErrorCode::Configuration, QStringLiteral("请在 OCR 设置中填写百度 API Key 和 Secret Key。"));
                return;
            }
            QString error;
            m_form = BaiduOcrProvider::imageForm(image, &error);
            if (!error.isEmpty()) { fail(ErrorCode::Configuration, error); return; }
            m_timer.start(m_config.timeoutMs);
            if (m_provider.m_credentials == m_credentials && !m_provider.m_token.isEmpty()
                && QDateTime::currentDateTimeUtc() < m_provider.m_expires)
                recognize(m_provider.m_token);
            else
                token();
        });
    }
    ~BaiduOcrJob() override { stop(); }
    void cancel() override { fail(ErrorCode::Cancelled, QStringLiteral("OCR 已取消。")); }
private:
    void stop()
    {
        m_timer.stop();
        if (m_reply) {
            auto reply = m_reply;
            m_reply.clear();
            disconnect(reply, nullptr, this, nullptr);
            reply->abort();
            reply->deleteLater();
        }
    }
    void fail(ErrorCode code, const QString &message)
    {
        if (m_done) return;
        m_done = true;
        stop();
        emit failed({code, message});
        deleteLater();
    }
    void token()
    {
        send(QStringLiteral("/oauth/2.0/token"), {}, "grant_type=client_credentials&" + field("client_id", m_config.apiKey)
             + '&' + field("client_secret", m_config.secretKey), true);
    }
    void recognize(const QString &accessToken)
    {
        send(QStringLiteral("/rest/2.0/ocr/v1/accurate_basic"), accessToken, m_form, false);
    }
    void send(const QString &path, const QString &accessToken, const QByteArray &body, bool auth)
    {
        if (m_done) return;
        QUrl url = m_provider.m_base.resolved(QUrl(path));
        if (!auth) {
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("access_token"), accessToken);
            url.setQuery(query);
        }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        m_reply = m_provider.m_network.post(request, body);
        m_bytes.clear();
        connect(m_reply, &QNetworkReply::readyRead, this, [this] {
            m_bytes += m_reply->readAll();
            if (m_bytes.size() > 2 * 1024 * 1024)
                fail(ErrorCode::InvalidResponse, QStringLiteral("OCR 响应过大。"));
        });
        connect(m_reply, &QNetworkReply::finished, this, [this, auth] {
            if (m_done || !m_reply) return;
            auto reply = m_reply;
            m_reply.clear();
            m_bytes += reply->readAll();
            const auto networkError = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();
            if (m_bytes.size() > 2 * 1024 * 1024) {
                fail(ErrorCode::InvalidResponse, QStringLiteral("OCR 响应过大。")); return;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(m_bytes, &parseError);
            const auto object = document.object();
            if (auth && (object.contains("error") || status == 401 || status == 403)) {
                fail(ErrorCode::Authentication, QStringLiteral("百度 OCR 鉴权失败，请检查 API Key、Secret Key 和服务权限。")); return;
            }
            const int code = object.value("error_code").toInt();
            if (!auth && (code == 110 || code == 111) && !m_refreshed) {
                m_refreshed = true;
                m_provider.m_token.clear();
                token(); return;
            }
            if (code != 0) {
                const bool quota = code == 17 || code == 18 || code == 19;
                const bool credentials = code == 110 || code == 111 || code == 6 || code == 100;
                fail(quota ? ErrorCode::RateLimit : (credentials ? ErrorCode::Authentication : ErrorCode::InvalidResponse),
                     QStringLiteral("百度 OCR 错误 %1：%2").arg(code).arg(quota ? QStringLiteral("额度不足或请求过于频繁，请检查账户后重试。")
                         : credentials ? QStringLiteral("鉴权失败或无接口权限，请检查 OCR 配置。") : QStringLiteral("识别失败，请检查图片和服务状态后重新截图。")));
                return;
            }
            if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
                fail(ErrorCode::Network, QStringLiteral("无法连接百度 OCR 服务（HTTP %1），请检查网络。 ").arg(status)); return;
            }
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                fail(ErrorCode::InvalidResponse, QStringLiteral("百度 OCR 返回了无效响应。")); return;
            }
            if (auth) {
                const QString accessToken = object.value("access_token").toString();
                const int expires = object.value("expires_in").toInt();
                if (accessToken.isEmpty() || expires <= 0) {
                    fail(ErrorCode::InvalidResponse, QStringLiteral("百度 OCR 未返回有效的访问令牌。")); return;
                }
                m_provider.m_credentials = m_credentials;
                m_provider.m_token = accessToken;
                m_provider.m_expires = QDateTime::currentDateTimeUtc().addSecs(qMax(0, expires - 60));
                recognize(accessToken);
                return;
            }
            if (!object.value("words_result").isArray()) {
                fail(ErrorCode::InvalidResponse, QStringLiteral("百度 OCR 响应缺少识别结果。")); return;
            }
            QStringList lines;
            for (const auto &entry : object.value("words_result").toArray()) {
                if (!entry.isObject() || !entry.toObject().value("words").isString()) {
                    fail(ErrorCode::InvalidResponse, QStringLiteral("百度 OCR 识别结果格式无效。")); return;
                }
                const auto line = entry.toObject().value("words").toString().trimmed();
                if (!line.isEmpty()) lines.append(line);
            }
            if (lines.isEmpty()) {
                fail(ErrorCode::InvalidResponse, QStringLiteral("截图中未识别到文字，请重新框选清晰的文字区域。")); return;
            }
            m_done = true;
            m_timer.stop();
            emit succeeded(lines.join('\n'));
            deleteLater();
        });
    }
    BaiduOcrProvider &m_provider;
    OcrConfig m_config;
    QByteArray m_credentials;
    QByteArray m_form;
    QByteArray m_bytes;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timer;
    bool m_done = false;
    bool m_refreshed = false;
};

BaiduOcrProvider::BaiduOcrProvider(const QUrl &base) : m_base(base) {}

QByteArray BaiduOcrProvider::imageForm(const QImage &image, QString *error)
{
    error->clear();
    // accurate_basic: encoded image <= 10 MB, shortest side >= 15, longest <= 8192.
    if (image.isNull() || qMin(image.width(), image.height()) < 15 || qMax(image.width(), image.height()) > 8192) {
        *error = QStringLiteral("截图短边须至少 15 像素，长边最多 8192 像素，请重新框选。 ");
        return {};
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        *error = QStringLiteral("无法编码截图，请重新截图。"); return {};
    }
    const auto encoded = QUrl::toPercentEncoding(QString::fromLatin1(png.toBase64()));
    if (encoded.size() > 10 * 1000 * 1000) {
        *error = QStringLiteral("截图编码后超过 10 MB，请缩小截图范围。"); return {};
    }
    return "image=" + encoded + "&language_type=auto_detect";
}

OcrJob *BaiduOcrProvider::recognize(const QImage &image, const OcrConfig &config, QObject *owner)
{
    return new BaiduOcrJob(*this, image, config, owner);
}
} // namespace Trans
