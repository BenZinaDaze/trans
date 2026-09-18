#pragma once

#include "translation.h"
#include <QDateTime>
#include <QImage>
#include <QNetworkAccessManager>
#include <QUrl>

namespace Trans {

struct OcrConfig {
    QString apiKey;
    QString secretKey;
    int timeoutMs = 30000;
};

class OcrJob : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void cancel() = 0;
signals:
    void succeeded(const QString &text);
    void failed(const Trans::TranslationError &error);
};

class BaiduOcrProvider {
public:
    // Endpoint injection is for local protocol tests; production uses Baidu HTTPS only.
    explicit BaiduOcrProvider(const QUrl &base = QUrl(QStringLiteral("https://aip.baidubce.com")));
    OcrJob *recognize(const QImage &image, const OcrConfig &config, QObject *owner);
    static QByteArray imageForm(const QImage &image, QString *error);
private:
    friend class BaiduOcrJob;
    QUrl m_base;
    QNetworkAccessManager m_network;
    QByteArray m_credentials;
    QString m_token;
    QDateTime m_expires;
};

} // namespace Trans
