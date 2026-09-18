#pragma once

#include "translation.h"
#include <QDBusConnection>
#include <QImage>
#include <QTimer>

namespace Trans {

class ScreenshotJob : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void cancel() = 0;
signals:
    void succeeded(const QImage &image);
    void failed(const Trans::TranslationError &error);
};

class PortalScreenshotJob final : public ScreenshotJob {
    Q_OBJECT
public:
    explicit PortalScreenshotJob(QObject *owner = nullptr,
                                 const QDBusConnection &bus = QDBusConnection::sessionBus(),
                                 const QString &service = QStringLiteral("org.freedesktop.portal.Desktop"));
    ~PortalScreenshotJob() override;
    void cancel() override;
    static QVariantMap captureOptions(uint version, uint targets, const QString &token);
    static QImage readImage(const QString &uri, QString *error);
private slots:
    void response(uint code, const QVariantMap &results);
private:
    void start();
    void request(uint version, uint targets);
    void finish(const TranslationError &error);
    void closeRequest();
    QDBusConnection m_bus;
    QString m_service;
    QString m_path;
    QTimer m_timer;
    bool m_done = false;
};

} // namespace Trans
