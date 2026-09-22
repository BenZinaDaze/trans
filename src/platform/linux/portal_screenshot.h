#pragma once

#include "screenshot.h"
#include <QDBusConnection>
#include <QTimer>
#include <QVariantMap>

namespace Trans {

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
    void finish(const PlatformError &error);
    void closeRequest();
    QDBusConnection m_bus;
    QString m_service;
    QString m_path;
    QTimer m_timer;
    bool m_done = false;
};

} // namespace Trans
