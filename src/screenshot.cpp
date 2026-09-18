#include "screenshot.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QImageReader>
#include <QUrl>
#include <QUuid>

namespace Trans {
namespace {
const QString desktopPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString screenshotInterface = QStringLiteral("org.freedesktop.portal.Screenshot");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");
}
PortalScreenshotJob::PortalScreenshotJob(QObject *owner, const QDBusConnection &bus, const QString &service)
    : ScreenshotJob(owner), m_bus(bus), m_service(service)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        finish({ErrorCode::Timeout, QStringLiteral("截图请求超时，请重试。")});
    });
    // Allow the compositor to process the application's hidden windows first.
    QTimer::singleShot(150, this, &PortalScreenshotJob::start);
}

PortalScreenshotJob::~PortalScreenshotJob() { closeRequest(); }

QVariantMap PortalScreenshotJob::captureOptions(uint version, uint targets, const QString &token)
{
    QVariantMap options{{"handle_token", token}, {"interactive", true}, {"modal", false}};
    if (version >= 3 && (targets & 4)) options.insert("target", uint(4));
    return options;
}

void PortalScreenshotJob::start()
{
    if (m_done) return;
    m_timer.start(180000);
    auto message = QDBusMessage::createMethodCall(m_service, desktopPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    message << screenshotInterface;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QVariantMap> reply = *watcher;
        watcher->deleteLater();
        if (m_done) return;
        if (reply.isError()) {
            finish({ErrorCode::Configuration, QStringLiteral("桌面截图服务不可用，请安装或检查 xdg-desktop-portal 和 xdg-desktop-portal-kde。")});
            return;
        }
        const auto properties = reply.value();
        const uint version = properties.value("version").toUInt();
        const uint targets = properties.value("AvailableTargets").toUInt();
        if (version < 3 || !(targets & 4)) {
            finish({ErrorCode::Configuration, QStringLiteral("当前桌面 Portal 不支持区域截图。请使用 Plasma X11，或升级到支持区域截图的桌面后端。")});
            return;
        }
        request(version, targets);
    });
}

void PortalScreenshotJob::request(uint version, uint targets)
{
    const QString token = "trans_" + QUuid::createUuid().toString(QUuid::Id128);
    QString sender = m_bus.baseService().mid(1);
    sender.replace('.', '_');
    m_path = "/org/freedesktop/portal/desktop/request/" + sender + '/' + token;
    // Subscribe before calling Screenshot: a backend may emit Response before the method reply.
    if (!m_bus.connect(m_service, m_path, requestInterface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)))) {
        finish({ErrorCode::Network, QStringLiteral("无法监听桌面截图结果。")}); return;
    }
    auto message = QDBusMessage::createMethodCall(m_service, desktopPath, screenshotInterface, QStringLiteral("Screenshot"));
    message << QString() << captureOptions(version, targets, token);
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_done) return;
        if (reply.isError()) {
            finish({ErrorCode::Network, QStringLiteral("无法启动系统截图，请检查桌面截图服务。")}); return;
        }
        const auto actual = reply.value().path();
        if (actual != m_path) {
            m_bus.disconnect(m_service, m_path, requestInterface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)));
            m_path = actual;
            if (!m_bus.connect(m_service, m_path, requestInterface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap))))
                finish({ErrorCode::Network, QStringLiteral("无法监听桌面截图结果。")});
        }
    });
}

QImage PortalScreenshotJob::readImage(const QString &uri, QString *error)
{
    error->clear();
    const QUrl url(uri);
    if (!url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost")) {
        *error = QStringLiteral("截图服务返回了无效的本地图片地址。"); return {};
    }
    const QFileInfo file(url.toLocalFile());
    if (!file.isFile() || file.size() > 64 * 1024 * 1024) {
        *error = QStringLiteral("无法读取截图，或截图文件过大，请缩小截图范围。"); return {};
    }
    QImageReader reader(file.absoluteFilePath());
    const QSize size = reader.size();
    if (!size.isValid() || qMin(size.width(), size.height()) < 15 || qMax(size.width(), size.height()) > 8192) {
        *error = QStringLiteral("截图短边须至少 15 像素，长边最多 8192 像素，请重新框选。"); return {};
    }
    const QImage image = reader.read();
    if (image.isNull()) *error = QStringLiteral("无法解码系统返回的截图，请重试。");
    return image;
}

void PortalScreenshotJob::response(uint code, const QVariantMap &results)
{
    if (m_done) return;
    // The portal owns the returned file; never delete an arbitrary URI supplied by it.
    if (code != 0) {
        finish({code == 1 ? ErrorCode::Cancelled : ErrorCode::Network,
                code == 1 ? QStringLiteral("截图已取消。") : QStringLiteral("系统截图失败，请重新截图。")}); return;
    }
    QString error;
    const auto image = readImage(results.value("uri").toString(), &error);
    if (!error.isEmpty()) { finish({ErrorCode::InvalidResponse, error}); return; }
    m_done = true;
    m_timer.stop();
    m_bus.disconnect(m_service, m_path, requestInterface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)));
    m_path.clear();
    emit succeeded(image);
    deleteLater();
}

void PortalScreenshotJob::closeRequest()
{
    if (m_path.isEmpty()) return;
    m_bus.disconnect(m_service, m_path, requestInterface, QStringLiteral("Response"), this, SLOT(response(uint,QVariantMap)));
    m_bus.asyncCall(QDBusMessage::createMethodCall(m_service, m_path, requestInterface, QStringLiteral("Close")));
    m_path.clear();
}
void PortalScreenshotJob::finish(const TranslationError &error)
{
    if (m_done) return;
    m_done = true;
    m_timer.stop();
    closeRequest();
    emit failed(error);
    deleteLater();
}
void PortalScreenshotJob::cancel() { finish({ErrorCode::Cancelled, QStringLiteral("截图已取消。")}); }
} // namespace Trans
