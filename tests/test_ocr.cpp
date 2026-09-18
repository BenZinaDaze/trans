#include "controller.h"
#include "desktop.h"
#include "ocr.h"
#include "screenshot.h"
#include "region_capture.h"
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <functional>

using namespace Trans;

class OcrServer : public QTcpServer {
public:
    struct Request { QByteArray path; QByteArray body; };
    QList<Request> requests;
    int tokens = 0;
    int images = 0;
    bool respond = true;
    QByteArray result = R"({"words_result":[{"words":"Hello"},{"words":"world"}]})";
    QByteArray authResult = R"({"access_token":"token","expires_in":3600})";
    std::function<QByteArray(int)> imageResponse;
    OcrServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer = QByteArray(), handled = false]() mutable {
                buffer += socket->readAll();
                const int split = buffer.indexOf("\r\n\r\n");
                if (handled || split < 0) return;
                int length = 0;
                for (const auto &line : buffer.left(split).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (buffer.size() < split + 4 + length) return;
                handled = true;
                Request request{buffer.split(' ').value(1), buffer.mid(split + 4, length)};
                requests.append(request);
                QByteArray response;
                if (request.path.startsWith("/oauth/")) { ++tokens; response = authResult; }
                else { ++images; response = imageResponse ? imageResponse(images) : result; }
                if (respond) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(response.size())
                                  + "\r\nConnection: close\r\n\r\n" + response);
                    socket->disconnectFromHost();
                }
            });
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); }
};

class Capture : public ScreenshotJob {
public:
    using ScreenshotJob::ScreenshotJob;
    bool cancelled = false;
    void cancel() override { cancelled = true; }
};
class TextJob : public TranslationJob {
public:
    using TranslationJob::TranslationJob;
    bool cancelled = false;
    void cancel() override { cancelled = true; }
};
class TextProvider : public TranslationProvider {
public:
    QStringList texts;
    QList<TextJob *> jobs;
    ProviderDescriptor descriptor() const override { return {"openai", "Test", "http://localhost", "", false, false}; }
    TranslationJob *translate(const TranslationRequest &request, const ProviderConfig &, QObject *owner) override
    {
        texts.append(request.text);
        auto *job = new TextJob(owner);
        jobs.append(job);
        return job;
    }
};
class Shortcut : public ShortcutService {
public:
    QString value;
    QString reject;
    bool apply(const QString &sequence) override { if (!reject.isEmpty() && sequence == reject) return false; value = sequence; return true; }
    QString sequence() const override { return value; }
};

// Real D-Bus messages on a private bus exercise response subscription and cancellation.
class Portal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Screenshot")
    Q_PROPERTY(uint version MEMBER version)
    Q_PROPERTY(uint AvailableTargets MEMBER targets)
public:
    uint version = 3;
    uint targets = 4;
    QVariantMap options;
    QString path;
    QDBusConnection bus;
    explicit Portal(const QDBusConnection &connection) : bus(connection) {}
    void respond(uint code, const QVariantMap &results)
    {
        auto signal = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
        signal << code << results;
        bus.send(signal);
    }
public slots:
    QDBusObjectPath Screenshot(const QString &, const QVariantMap &values)
    {
        options = values;
        QString sender = message().service().mid(1);
        sender.replace('.', '_');
        path = "/org/freedesktop/portal/desktop/request/" + sender + '/' + values.value("handle_token").toString();
        return QDBusObjectPath(path);
    }
};
class PortalRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")
public:
    bool closed = false;
public slots:
    void Close() { closed = true; }
};

class OcrTest : public QObject {
    Q_OBJECT
    QImage sample() const { QImage image(100, 40, QImage::Format_RGB32); image.fill(Qt::white); return image; }
private slots:
    void initTestCase() { qRegisterMetaType<TranslationError>(); }
    void regionCoordinatesAndPixels()
    {
        QCOMPARE(RegionOverlay::pixelRect(QRectF(10, 20, 30, 40), QSize(100, 100), QSize(200, 200)), QRect(20, 40, 60, 80));
        QCOMPARE(RegionOverlay::pixelRect(QRectF(QPointF(80, 70), QPointF(10, 20)), QSize(100, 100), QSize(150, 150)), QRect(15, 30, 105, 75));
        QCOMPARE(RegionOverlay::pixelRect(QRectF(-10, -20, 50, 50), QSize(100, 100), QSize(100, 100)), QRect(0, 0, 40, 30));
        QVERIFY(RegionOverlay::pixelRect(QRectF(110, 0, 20, 20), QSize(100, 100), QSize(100, 100)).isEmpty());
        QImage image(200, 200, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) image.setPixel(x, y, qRgb(x, y, 0));
        // Simulate a negative-origin display with a 2x device pixel ratio.
        image.setDevicePixelRatio(2);
        RegionOverlay overlay(image, QRect(-100, 0, 100, 100));
        QImage cropped;
        int selections = 0;
        int cancellations = 0;
        overlay.selected = [&](const QImage &result) { cropped = result; ++selections; };
        overlay.cancelled = [&] { ++cancellations; };
        overlay.show();
        QTest::mousePress(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(60, 70));
        QTest::mouseMove(&overlay, QPoint(10, 20));
        QTest::mouseRelease(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(10, 20));
        QCOMPARE(selections, 1);
        QCOMPARE(cropped.size(), QSize(100, 100));
        QCOMPARE(cropped.pixel(0, 0), qRgb(20, 40, 0));
        QCOMPARE(cropped.pixel(99, 99), qRgb(119, 139, 0));
        QCOMPARE(cropped.devicePixelRatio(), qreal(1));
        QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier, QPoint(30, 30));
        QCOMPARE(selections, 1); // A click cannot send the whole screenshot to OCR.
        QTest::keyClick(&overlay, Qt::Key_Escape);
        QCOMPARE(cancellations, 1);
        QTest::mouseClick(&overlay, Qt::RightButton, Qt::NoModifier, QPoint(30, 30));
        QCOMPARE(cancellations, 2);
    }
    void formAndImageLimits()
    {
        QString error;
        const auto body = BaiduOcrProvider::imageForm(sample(), &error);
        QVERIFY(error.isEmpty());
        const QUrlQuery form(QString::fromLatin1(body));
        QCOMPARE(form.queryItemValue("language_type"), QStringLiteral("auto_detect"));
        const auto png = QByteArray::fromBase64(form.queryItemValue("image", QUrl::FullyDecoded).toLatin1());
        QCOMPARE(QImage::fromData(png).size(), sample().size());
        QVERIFY(body.contains("%"));
        for (const QSize size : {QSize(14, 40), QSize(8193, 15)}) {
            QVERIFY(BaiduOcrProvider::imageForm(QImage(size, QImage::Format_RGB32), &error).isEmpty());
            QVERIFY(!error.isEmpty());
        }
        // Incompressible image hits the encoded byte limit independently of dimensions.
        QImage noise(1800, 1800, QImage::Format_RGB32);
        quint32 state = 1234567;
        for (int y = 0; y < noise.height(); ++y) {
            auto *row = reinterpret_cast<quint32 *>(noise.scanLine(y));
            for (int x = 0; x < noise.width(); ++x) {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                row[x] = 0xff000000 | (state & 0xffffff);
            }
        }
        QVERIFY(BaiduOcrProvider::imageForm(noise, &error).isEmpty());
        QVERIFY(error.contains("10 MB"));
    }
    void tokenCacheAndEncoding()
    {
        OcrServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        BaiduOcrProvider provider(server.url());
        const OcrConfig config{"key+&", "secret=/", 2000};
        for (int i = 0; i < 2; ++i) {
            auto *job = provider.recognize(sample(), config, this);
            QSignalSpy success(job, &OcrJob::succeeded);
            QTRY_COMPARE(success.size(), 1);
            QCOMPARE(success.first().first().toString(), QStringLiteral("Hello\nworld"));
        }
        QCOMPARE(server.tokens, 1);
        QCOMPARE(server.images, 2);
        QCOMPARE(QUrlQuery(QString::fromLatin1(server.requests.first().body)).queryItemValue("client_id", QUrl::FullyDecoded), config.apiKey);
        QVERIFY(server.requests.at(1).path.startsWith("/rest/2.0/ocr/v1/accurate_basic?access_token=token"));
        auto *job = provider.recognize(sample(), {"different", "secret", 2000}, this);
        QSignalSpy success(job, &OcrJob::succeeded);
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(server.tokens, 2);
    }
    void refreshOnce()
    {
        OcrServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.result = R"({"error_code":111})";
        BaiduOcrProvider provider(server.url());
        auto *job = provider.recognize(sample(), {"key", "secret", 2000}, this);
        QSignalSpy failed(job, &OcrJob::failed);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(server.tokens, 2);
        QCOMPARE(server.images, 2);
        QCOMPARE(qvariant_cast<TranslationError>(failed.first().first()).code, ErrorCode::Authentication);
        server.imageResponse = [](int count) { return count == 3 ? QByteArray(R"({"error_code":110})") : QByteArray(R"({"words_result":[{"words":"ok"}]})"); };
        job = provider.recognize(sample(), {"key", "secret", 2000}, this);
        QSignalSpy success(job, &OcrJob::succeeded);
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(server.tokens, 3);
        QCOMPARE(server.images, 4);
    }
    void errors_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<ErrorCode>("code");
        QTest::newRow("empty") << QByteArray(R"({"words_result":[]})") << ErrorCode::InvalidResponse;
        QTest::newRow("malformed") << QByteArray("not json") << ErrorCode::InvalidResponse;
        QTest::newRow("missing") << QByteArray("{}") << ErrorCode::InvalidResponse;
        QTest::newRow("quota") << QByteArray(R"({"error_code":17})") << ErrorCode::RateLimit;
        QTest::newRow("permission") << QByteArray(R"({"error_code":6})") << ErrorCode::Authentication;
    }
    void errors()
    {
        QFETCH(QByteArray, body); QFETCH(ErrorCode, code);
        OcrServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.result = body;
        BaiduOcrProvider provider(server.url());
        auto *job = provider.recognize(sample(), {"key", "secret", 2000}, this);
        QSignalSpy failure(job, &OcrJob::failed);
        QTRY_COMPARE(failure.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(failure.first().first()).code, code);
    }
    void timeoutCancelAndAuthentication()
    {
        OcrServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        BaiduOcrProvider provider(server.url());
        server.respond = false;
        auto *job = provider.recognize(sample(), {"key", "secret", 50}, this);
        QSignalSpy timeout(job, &OcrJob::failed);
        QTRY_COMPARE(timeout.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(timeout.first().first()).code, ErrorCode::Timeout);
        job = provider.recognize(sample(), {"key", "secret", 1000}, this);
        QSignalSpy cancelled(job, &OcrJob::failed);
        job->cancel();
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(cancelled.first().first()).code, ErrorCode::Cancelled);
        server.respond = true;
        server.authResult = R"({"error":"invalid_client","error_description":"do not echo secrets"})";
        job = provider.recognize(sample(), {"key", "secret", 1000}, this);
        QSignalSpy failure(job, &OcrJob::failed);
        QTRY_COMPARE(failure.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(failure.first().first()).code, ErrorCode::Authentication);
        QVERIFY(!qvariant_cast<TranslationError>(failure.first().first()).message.contains("secrets"));
    }
    void pipelineCancelsStaleWorkAndRetrySkipsOcr()
    {
        OcrServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        BaiduOcrProvider ocr(server.url());
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<TextProvider>();
        auto *text = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        auto draft = settings.snapshot();
        draft["ocrApiKey"] = "key"; draft["ocrSecretKey"] = "secret";
        QVERIFY(settings.save(draft));
        TranslationController controller(registry, settings, nullptr, &ocr);
        auto *capture = new Capture(&controller);
        controller.translateScreenshot(capture);
        QCOMPARE(controller.status(), QStringLiteral("capturing"));
        emit capture->succeeded(sample());
        QCOMPARE(controller.status(), QStringLiteral("recognizing"));
        QVERIFY(!controller.sourceIsOcr());
        QTRY_COMPARE(text->texts.size(), 1);
        QVERIFY(controller.sourceIsOcr());
        QCOMPARE(text->texts.first(), QStringLiteral("Hello\nworld"));
        emit text->jobs.last()->succeeded({"你好", "en"});
        controller.retry();
        QVERIFY(controller.sourceIsOcr());
        QCOMPARE(text->texts.size(), 2);
        QCOMPARE(server.images, 1);
        emit text->jobs.last()->succeeded({"你好", "en"});
        auto *cancelled = new Capture(&controller);
        controller.translateScreenshot(cancelled);
        emit cancelled->failed({ErrorCode::Cancelled, "cancel"});
        QCOMPARE(controller.status(), QStringLiteral("success"));
        QCOMPARE(controller.translatedText(), QStringLiteral("你好"));
        QVERIFY(controller.sourceIsOcr());
        auto *stale = new Capture(&controller);
        controller.translateScreenshot(stale);
        controller.translateText("new selection");
        QVERIFY(stale->cancelled);
        emit stale->succeeded(sample());
        QCOMPARE(controller.sourceText(), QStringLiteral("new selection"));
        QVERIFY(!controller.sourceIsOcr());
        QCOMPARE(server.images, 1);
        server.respond = false;
        auto *pending = new Capture(&controller);
        controller.translateScreenshot(pending);
        emit pending->succeeded(sample());
        QTRY_COMPARE(server.images, 2);
        controller.cancel();
        QCOMPARE(controller.status(), QStringLiteral("cancelled"));
        QCOMPARE(text->texts.size(), 3);
    }
    void dualShortcutRollbackAndConfig()
    {
        QTemporaryDir directory;
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        Shortcut selection, screenshot;
        selection.value = "Meta+Shift+T"; screenshot.value = "Meta+Shift+O";
        DesktopBridge desktop(controller, settings, nullptr, &selection, &screenshot);
        auto draft = settings.snapshot();
        draft["shortcut"] = "Meta+Shift+O"; draft["screenshotShortcut"] = "Meta+Shift+T";
        draft["ocrApiKey"] = "key"; draft["ocrSecretKey"] = "secret";
        QVERIFY(desktop.saveSettings(draft));
        QCOMPARE(selection.value, QStringLiteral("Meta+Shift+O"));
        QCOMPARE(screenshot.value, QStringLiteral("Meta+Shift+T"));
        draft["shortcut"] = "Ctrl+Alt+A"; draft["screenshotShortcut"] = "Ctrl+Alt+B";
        screenshot.reject = "Ctrl+Alt+B";
        QVERIFY(!desktop.saveSettings(draft));
        QCOMPARE(selection.value, QStringLiteral("Meta+Shift+O"));
        QCOMPARE(screenshot.value, QStringLiteral("Meta+Shift+T"));
        AppSettings reloaded(registry, settings.configPath());
        QCOMPARE(reloaded.snapshot().value("ocrSecretKey").toString(), QStringLiteral("secret"));
        QCOMPARE(reloaded.snapshot().value("screenshotShortcut").toString(), screenshot.value);
        draft["screenshotShortcut"] = draft["shortcut"];
        QVERIFY(!desktop.saveSettings(draft));
        QFile blocker(directory.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        AppSettings broken(registry, directory.filePath("blocker/settings.ini"));
        TranslationController brokenController(registry, broken);
        DesktopBridge brokenDesktop(brokenController, broken, nullptr, &selection, &screenshot);
        draft["screenshotShortcut"] = "Ctrl+Alt+C";
        QVERIFY(!brokenDesktop.saveSettings(draft));
        QCOMPARE(selection.value, QStringLiteral("Meta+Shift+O"));
        QCOMPARE(screenshot.value, QStringLiteral("Meta+Shift+T"));
    }
    void portalResponses()
    {
        QTemporaryDir directory;
        QProcess daemon;
        daemon.start("dbus-daemon", {"--session", "--nofork", "--nopidfile", "--print-address=1", "--address=unix:tmpdir=" + directory.path()});
        QVERIFY(daemon.waitForStarted());
        QVERIFY(daemon.waitForReadyRead());
        const QString address = QString::fromUtf8(daemon.readLine()).trimmed();
        auto backend = QDBusConnection::connectToBus(address, "ocr-portal-backend");
        auto client = QDBusConnection::connectToBus(address, "ocr-portal-client");
        QVERIFY(backend.isConnected()); QVERIFY(client.isConnected());
        Portal portal(backend);
        QVERIFY(backend.registerService("org.freedesktop.portal.Desktop"));
        QVERIFY(backend.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
        const QString imagePath = directory.filePath("sample.png");
        QVERIFY(sample().save(imagePath));
        auto *job = new PortalScreenshotJob(this, client);
        QSignalSpy success(job, &ScreenshotJob::succeeded);
        QTRY_VERIFY(!portal.path.isEmpty());
        QCOMPARE(portal.options.value("target").toUInt(), uint(4));
        portal.respond(0, {{"uri", QUrl::fromLocalFile(imagePath).toString()}});
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(qvariant_cast<QImage>(success.first().first()).size(), sample().size());
        QVERIFY(QFile::exists(imagePath));
        portal.path.clear(); portal.version = 2; portal.targets = 0;
        job = new PortalScreenshotJob(this, client);
        QSignalSpy unsupported(job, &ScreenshotJob::failed);
        QTRY_COMPARE(unsupported.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(unsupported.first().first()).code, ErrorCode::Configuration);
        QVERIFY(portal.path.isEmpty()); // Never offer or upload a whole screen as a fallback.
        portal.version = 3; portal.targets = 4;
        job = new PortalScreenshotJob(this, client);
        QSignalSpy cancel(job, &ScreenshotJob::failed);
        QTRY_VERIFY(!portal.path.isEmpty());
        QCOMPARE(portal.options.value("target").toUInt(), uint(4));
        QVERIFY(portal.options.value("interactive").toBool());
        portal.respond(1, {});
        QTRY_COMPARE(cancel.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(cancel.first().first()).code, ErrorCode::Cancelled);
        portal.path.clear();
        job = new PortalScreenshotJob(this, client);
        QSignalSpy invalid(job, &ScreenshotJob::failed);
        QTRY_VERIFY(!portal.path.isEmpty());
        portal.respond(0, {{"uri", "https://example.com/image.png"}});
        QTRY_COMPARE(invalid.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(invalid.first().first()).code, ErrorCode::InvalidResponse);
        portal.path.clear();
        job = new PortalScreenshotJob(this, client);
        QSignalSpy stopped(job, &ScreenshotJob::failed);
        QTRY_VERIFY(!portal.path.isEmpty());
        PortalRequest request;
        QVERIFY(backend.registerObject(portal.path, &request, QDBusConnection::ExportAllSlots));
        job->cancel();
        QTRY_VERIFY(request.closed);
        QCOMPARE(stopped.size(), 1);
        portal.respond(0, {{"uri", QUrl::fromLocalFile(imagePath).toString()}});
        job = new PortalScreenshotJob(this, client, "org.example.MissingPortal");
        QSignalSpy missing(job, &ScreenshotJob::failed);
        QTRY_COMPARE(missing.size(), 1);
        QCOMPARE(qvariant_cast<TranslationError>(missing.first().first()).code, ErrorCode::Configuration);
        QDBusConnection::disconnectFromBus("ocr-portal-client");
        QDBusConnection::disconnectFromBus("ocr-portal-backend");
        daemon.terminate();
        QVERIFY(daemon.waitForFinished(2000));
    }
};
QTEST_MAIN(OcrTest)
#include "test_ocr.moc"
