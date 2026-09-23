#include "controller.h"
#include "desktop.h"
#include "platform/platform_services.h"
#include "platform/selection_reader.h"
#include "platform/shortcut_service.h"
#ifdef Q_OS_LINUX
#include "platform/linux/instance_channel_linux.h"
#endif
#include "providers.h"
#include "settings.h"
#include "provider_tools.h"
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QScreen>

#include <QClipboard>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtTest/qtestwheel.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace Trans;

class MockServer final : public QTcpServer {
public:
    struct Request { QByteArray path; QMap<QByteArray, QByteArray> headers; QJsonObject body; };
    QList<Request> requests;
    int status = 200;
    QByteArray body = R"({"choices":[{"message":{"content":"你好"},"finish_reason":"stop"}]})";
    bool respond = true;

    MockServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer = QByteArray(), handled = false]() mutable {
                    buffer += socket->readAll();
                    const auto split = buffer.indexOf("\r\n\r\n");
                    if (handled || split < 0)
                        return;
                    Request request;
                    const auto lines = buffer.left(split).split('\n');
                    request.path = lines.first().split(' ').value(1);
                    for (const auto &line : lines.mid(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0)
                            request.headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (buffer.size() < split + 4 + length)
                        return;
                    handled = true;
                    request.body = QJsonDocument::fromJson(buffer.mid(split + 4, length)).object();
                    requests.append(request);
                    if (respond) {
                        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: "
                                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    }
                });
            }
        });
    }

    QString endpoint() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
};

// Deliberately completes after cancellation to exercise the controller's generation guard.
class ManualJob final : public TranslationJob {
public:
    using TranslationJob::TranslationJob;
    bool cancelled = false;
    void cancel() override { cancelled = true; }
    void succeed(const QString &text) { emit succeeded({text, "EN"}); }
    void fail() { emit failed({ErrorCode::Network, "late failure"}); }
};

class ManualProvider final : public TranslationProvider {
public:
    QList<ManualJob *> jobs;
    QList<TranslationRequest> requests;
    ProviderDescriptor info{"openai", "Test", "http://localhost", "", false, false};
    ProviderDescriptor descriptor() const override { return info; }
    TranslationJob *translate(const TranslationRequest &request, const ProviderConfig &, QObject *owner) override
    {
        auto *job = new ManualJob(owner);
        jobs.append(job);
        requests.append(request);
        return job;
    }
};

class TestShortcutJob final : public ShortcutJob {
public:
    using ShortcutJob::ShortcutJob;
    void cancel() override { fail({PlatformErrorCode::Cancelled, "Cancelled"}); }
    void complete(bool accepted) {
        if (accepted) succeed();
        else fail({PlatformErrorCode::Conflict, "Occupied"});
    }
};

class TestShortcut final : public ShortcutService {
public:
    QMap<ShortcutAction, QString> values{{ShortcutAction::Selection, "Meta+Shift+T"},
                                        {ShortcutAction::Screenshot, "Meta+Shift+O"}};
    bool reject = false;
    ShortcutJob *update(ShortcutAction action, const QString &requested, QObject *owner) override
    {
        auto *job = new TestShortcutJob(owner);
        QTimer::singleShot(0, job, [this, job, action, requested] {
            if (!reject) values[action] = requested;
            job->complete(!reject);
        });
        return job;
    }
    QString sequence(ShortcutAction action) const override { return values.value(action); }
    CapabilityState availability() const override { return CapabilityState::Available; }
    QString unavailableReason() const override { return {}; }
};

class PendingScreenshot final : public ScreenshotJob {
public:
    using ScreenshotJob::ScreenshotJob;
    void cancel() override {
        emit failed({PlatformErrorCode::Cancelled, "Cancelled"});
        deleteLater();
    }
};

class TestScreenshots final : public ScreenshotService {
public:
    ScreenshotJob *captureRegion(QObject *owner) override { return new PendingScreenshot(owner); }
    CapabilityState availability() const override { return CapabilityState::Available; }
    QString unavailableReason() const override { return {}; }
};

static std::unique_ptr<PlatformServices> testPlatform(TestShortcut **shortcut)
{
    auto keys = std::make_unique<TestShortcut>();
    *shortcut = keys.get();
    return std::make_unique<PlatformServices>(
        std::unique_ptr<SelectionReader>(createSelectionReader()), std::move(keys),
        std::make_unique<TestScreenshots>(),
        std::unique_ptr<WindowIntegration>(createWindowIntegration()));
}

class DelayedSelection final : public SelectionJob {
public:
    using SelectionJob::SelectionJob;
    bool cancelled = false;
    void cancel() override { cancelled = true; }
    void deliver(const QString &text) { emit succeeded({text, {}}); }
};

class TransTest : public QObject {
    Q_OBJECT
private slots:
    void staleSelectionCannotStartTranslation()
    {
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<ManualProvider>();
        auto *manual = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        QSignalSpy finished(&controller, &TranslationController::selectionFinished);
        auto *cancelled = new DelayedSelection(&controller);
        controller.translateSelection(cancelled);
        QVERIFY(controller.busy());
        controller.cancel();
        QVERIFY(cancelled->cancelled);
        const auto notifications = finished.size();
        cancelled->deliver("stale after close");
        QVERIFY(manual->requests.isEmpty());
        QCOMPARE(finished.size(), notifications);

        auto *old = new DelayedSelection(&controller);
        auto *current = new DelayedSelection(&controller);
        controller.translateSelection(old);
        controller.translateSelection(current);
        QVERIFY(old->cancelled);
        old->deliver("replaced selection");
        QVERIFY(manual->requests.isEmpty());
        current->deliver("current selection");
        QCOMPARE(manual->requests.size(), 1);
        QCOMPARE(manual->requests.first().text, QStringLiteral("current selection"));
        manual->jobs.first()->succeed("current result");
        old->deliver("late old selection");
        QCOMPARE(controller.translatedText(), QStringLiteral("current result"));
        QCOMPARE(manual->requests.size(), 1);
    }

    void trayReopensWithoutTranslating()
    {
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<ManualProvider>();
        auto *manual = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        TestShortcut *shortcut;
        auto platform = testPlatform(&shortcut);
        DesktopBridge desktop(controller, settings, *platform);
        QWindow popup;
        QWindow settingsWindow;
        desktop.setWindows(&popup, &settingsWindow);
        desktop.initialize();
        auto *tray = desktop.findChild<QSystemTrayIcon *>();
        QVERIFY(tray && tray->contextMenu());
        auto *openAction = tray->contextMenu()->defaultAction();
        QVERIFY(openAction);

        // First open must not read PRIMARY or make a request, even without a result.
        tray->activated(QSystemTrayIcon::Trigger);
        QVERIFY(popup.isVisible());
        QCOMPARE(controller.status(), QStringLiteral("idle"));
        QVERIFY(manual->requests.isEmpty());
        desktop.closeTranslation();

        controller.translateText("previous selection");
        manual->jobs.last()->succeed(QStringLiteral("上次选区的译文"));
        desktop.ShowTranslation();
        desktop.closeTranslation();
        QVERIFY(!popup.isVisible());
        QSignalSpy changes(&controller, &TranslationController::stateChanged);
        auto draft = settings.snapshot();
        draft["targetLanguage"] = "ja";
        QVERIFY(settings.save(draft));

        // Repeated tray clicks and the open menu action preserve the completed result,
        // including its original language after preferences have changed.
        for (int i = 0; i < 3; ++i) {
            if (i == 1)
                openAction->trigger();
            else
                tray->activated(QSystemTrayIcon::Trigger);
            QVERIFY(popup.isVisible());
            QCOMPARE(manual->requests.size(), 1);
            QCOMPARE(controller.status(), QStringLiteral("success"));
            QCOMPARE(controller.sourceText(), QStringLiteral("previous selection"));
            QCOMPARE(controller.translatedText(), QStringLiteral("上次选区的译文"));
            QCOMPARE(controller.providerId(), QStringLiteral("openai"));
            QCOMPARE(controller.targetLanguage(), QStringLiteral("zh-CN"));
            QCOMPARE(controller.detectedLanguage(), QStringLiteral("EN"));
            QVERIFY(changes.isEmpty());
            desktop.closeTranslation();
        }
        tray->activated(QSystemTrayIcon::Context);
        QVERIFY(!popup.isVisible());
        QCOMPARE(manual->requests.size(), 1);

        // Explicit retry still sends a new request with the current settings.
        controller.retry();
        QCOMPARE(manual->requests.size(), 2);
        QCOMPARE(manual->requests.last().targetLanguage, QStringLiteral("ja"));
        QVERIFY(controller.busy());
        tray->activated(QSystemTrayIcon::Trigger);
        QCOMPARE(manual->requests.size(), 2);
        QVERIFY(controller.busy());
        desktop.closeTranslation();
        QVERIFY(manual->jobs.last()->cancelled);
        QCOMPARE(controller.status(), QStringLiteral("cancelled"));
        manual->jobs.last()->succeed(QStringLiteral("迟到的结果"));
        tray->activated(QSystemTrayIcon::Trigger);
        QVERIFY(popup.isVisible());
        QCOMPARE(manual->requests.size(), 2);
        QCOMPARE(controller.status(), QStringLiteral("cancelled"));
        QVERIFY(controller.translatedText().isEmpty());

        controller.retry();
        manual->jobs.last()->fail();
        const auto errorMessage = controller.message();
        desktop.closeTranslation();
        openAction->trigger();
        QVERIFY(popup.isVisible());
        QCOMPARE(manual->requests.size(), 3);
        QCOMPARE(controller.status(), QStringLiteral("error"));
        QCOMPARE(controller.message(), errorMessage);
    }

    void redesignedUi_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("compact");
        QTest::newRow("light") << false << false;
        QTest::newRow("dark") << true << false;
        QTest::newRow("light-compact") << false << true;
        QTest::newRow("dark-compact") << true << true;
    }

    void redesignedUi()
    {
        QFETCH(bool, dark);
        QFETCH(bool, compact);
        const auto originalVersion = QCoreApplication::applicationVersion();
        const auto restoreVersion = qScopeGuard([&] { QCoreApplication::setApplicationVersion(originalVersion); });
        QCoreApplication::setApplicationVersion(QStringLiteral(TRANS_VERSION));
        const auto originalPalette = QApplication::palette();
        const auto restorePalette = qScopeGuard([&] { QApplication::setPalette(originalPalette); });
        auto palette = originalPalette;
        palette.setColor(QPalette::Window, dark ? QColor("#202124") : QColor("#f5f6f8"));
        QApplication::setPalette(palette);
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<ManualProvider>();
        auto *manual = provider.get();
        provider->info = HttpTranslationProvider(HttpProtocol::OpenAI).descriptor();
        registry.add(std::move(provider));
        registry.add(std::make_unique<HttpTranslationProvider>(HttpProtocol::DeepSeek));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        auto values = settings.snapshot();
        auto configs = values["providerConfigs"].toMap();
        auto config = configs["openai"].toMap();
        config["apiKey"] = "ui-fixture-key";
        configs["openai"] = config;
        values["providerConfigs"] = configs;
        QVERIFY(settings.save(values));
        TranslationController controller(registry, settings);
        auto platform = createPlatformServices();
        DesktopBridge desktop(controller, settings, *platform);
        ProviderTools tools(registry);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"appSettings", QVariant::fromValue(&settings)},
            {"controller", QVariant::fromValue(&controller)}, {"desktop", QVariant::fromValue(&desktop)},
            {"providerTools", QVariant::fromValue(&tools)}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(TRANS_SOURCE_DIR "/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *popup = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *window = popup->findChild<QQuickWindow *>("settingsWindow");
        QVERIFY(window);
        desktop.setWindows(popup, window);
        popup->resize(compact ? QSize(480, 360) : QSize(600, 480));
        popup->show();
        QTRY_VERIFY(popup->isExposed());
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            TITLEBARINFO titleBar{};
            titleBar.cbSize = sizeof(titleBar);
            QVERIFY(GetTitleBarInfo(reinterpret_cast<HWND>(popup->winId()), &titleBar));
            // The last title-bar child is the close button.
            QVERIFY(!(titleBar.rgstate[5] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE)));
        }
#endif
        const QString previewDirectory = qEnvironmentVariable("TRANS_UI_SCREENSHOT_DIR");
        const auto render = [](QQuickWindow *target) {
            QSignalSpy frame(target, &QQuickWindow::frameSwapped);
            target->update();
            return frame.wait(1000);
        };
        const auto capture = [&](QQuickWindow *target, const QString &name) {
            // Wait for layout and scene synchronization even when screenshots are disabled.
            if (target == popup) {
                QCoreApplication::processEvents();
                // Exercise constrained layouts as well as the separately tested automatic sizes.
                target->resize(compact ? QSize(480, 360) : QSize(600, 480));
            }
            if (!render(target)) return false;
            if (previewDirectory.isEmpty()) return true;
            QDir().mkpath(previewDirectory);
            return target->grabWindow().save(QDir(previewDirectory).filePath(
                QString::fromLatin1(QTest::currentDataTag()) + "-" + name + ".png"));
        };
        auto *copy = popup->findChild<QQuickItem *>("copyTranslationButton");
        auto *retry = popup->findChild<QQuickItem *>("retryTranslationButton");
        auto *cancel = popup->findChild<QQuickItem *>("cancelTranslationButton");
        QVERIFY(copy && retry && cancel);
        QVERIFY(!popup->findChild<QQuickItem *>("screenshotTranslationButton"));
        auto *ocrSource = popup->findChild<QQuickItem *>("ocrSourceLabel");
        QVERIFY(ocrSource && !ocrSource->isVisible());
        QVERIFY(!copy->isEnabled());
        QVERIFY(capture(popup, "idle"));
        controller.translateText("The limits of my language mean the limits of my world.");
        QVERIFY(cancel->isVisible());
        QVERIFY(!retry->isVisible());
        QVERIFY(!copy->isEnabled());
        QVERIFY(capture(popup, "loading"));
        manual->jobs.last()->succeed(QStringLiteral("我的语言的界限，意味着我的世界的界限。"));
        QTRY_VERIFY(copy->isEnabled());
        QVERIFY(retry->isVisible());
        QVERIFY(!cancel->isVisible());
        QVERIFY(capture(popup, "translation"));
        const auto copyPosition = copy->mapToScene(QPointF(0, 0));
        QVERIFY(copyPosition.x() >= 0 && copyPosition.y() >= 0);
        QVERIFY(copyPosition.x() + copy->width() <= popup->width());
        QVERIFY(copyPosition.y() + copy->height() <= popup->height());
        QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                          copy->mapToScene(QPointF(copy->width() / 2, copy->height() / 2)).toPoint());
        QCOMPARE(QGuiApplication::clipboard()->text(), controller.translatedText());
        QCOMPARE(copy->property("text").toString(), QStringLiteral("已复制"));
        QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                          retry->mapToScene(QPointF(retry->width() / 2, retry->height() / 2)).toPoint());
        QVERIFY(controller.busy());
        QVERIFY(!copy->isEnabled());
        QVERIFY(render(popup));
        QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier,
                          cancel->mapToScene(QPointF(cancel->width() / 2, cancel->height() / 2)).toPoint());
        QCOMPARE(controller.status(), QStringLiteral("cancelled"));
        QVERIFY(capture(popup, "cancelled"));
        controller.retry();
        manual->jobs.last()->fail();
        QVERIFY(retry->isVisible());
        QVERIFY(!copy->isEnabled());
        QVERIFY(capture(popup, "error"));
        controller.translateText(QStringLiteral("A longer passage with multiple paragraphs.\n\n").repeated(30));
        manual->jobs.last()->succeed(QStringLiteral("文字让我们跨越语言的边界，理解不同的观点。\n\n").repeated(30));
        auto *translation = popup->findChild<QQuickItem *>("translationText");
        QVERIFY(translation && translation->isVisible());
        QVERIFY(translation->property("readOnly").toBool());
        QVERIFY(copy->isEnabled());
        QVERIFY(capture(popup, "long-text"));

        desktop.ShowSettings();
        window->resize(compact ? QSize(500, 420) : QSize(940, 740));
        QTRY_VERIFY(window->isExposed());
        QVERIFY(render(window));
        auto *tabs = window->findChild<QObject *>("settingsTabs");
        QVERIFY(tabs);
        for (int page = 0; page < 4; ++page) {
            QQuickItem *nav = nullptr;
            // Repeater delegates belong to the visual tree, not the QObject ownership tree.
            for (auto *item : qobject_cast<QQuickItem *>(tabs)->childItems()) {
                if (item->objectName() == QStringLiteral("settingsNav%1").arg(page))
                    nav = item;
            }
            QVERIFY(nav);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                nav->mapToScene(QPointF(nav->width() / 2, nav->height() / 2)).toPoint());
            QCOMPARE(tabs->property("currentIndex").toInt(), page);
            QVERIFY(capture(window, QStringLiteral("settings-%1").arg(page)));
            if (page == 0) {
                auto *combo = window->findChild<QQuickItem *>("providerCombo");
                QVERIFY(combo);
                combo->forceActiveFocus();
                QTest::keyClick(window, Qt::Key_Space);
                QVERIFY(capture(window, "provider-menu"));
                QTest::keyClick(window, Qt::Key_Down);
                QTest::keyClick(window, Qt::Key_Return);
                auto *providerPage = window->findChild<QQuickItem *>("providerPage");
                QCOMPARE(providerPage->property("editingProvider").toString(), QStringLiteral("deepseek"));
                QCOMPARE(settings.providerId(), QStringLiteral("openai")); // Remains a draft until saved.
                QVERIFY(capture(window, "deepseek"));
            }
            auto *settingsPage = window->findChild<QQuickItem *>(
                QStringList{"providerPage", "translationPage", "desktopPage", "ocrPage"}.at(page));
            QVERIFY(settingsPage);
            auto *flickable = settingsPage->property("contentItem").value<QObject *>();
            QVERIFY(flickable);
            const qreal bottom = qMax(0.0, flickable->property("contentHeight").toReal() - flickable->property("height").toReal());
            QVERIFY(flickable->setProperty("contentY", bottom));
            QVERIFY(capture(window, QStringLiteral("settings-%1-bottom").arg(page)));
            if (page == 2) {
                auto *lastField = window->findChild<QQuickItem *>("restoreFocusField");
                QVERIFY(lastField);
                const auto position = lastField->mapToItem(settingsPage, QPointF());
                QVERIFY(position.y() >= 0 && position.y() + lastField->height() <= settingsPage->height() + 1);
            }
        }
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("windows"))
            SendMessageW(reinterpret_cast<HWND>(popup->winId()), WM_SYSCOMMAND, SC_CLOSE, 0);
        else
#endif
            popup->close();
        QTRY_VERIFY(!popup->isVisible());
        popup->show();
        QTRY_VERIFY(popup->isExposed());
        QVERIFY2(warnings.isEmpty(), "Redesigned UI must load and handle every state without QML warnings.");
    }

    void translationWindowFitsContent()
    {
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<ManualProvider>();
        auto *manual = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        auto platform = createPlatformServices();
        DesktopBridge desktop(controller, settings, *platform);
        ProviderTools tools(registry);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"appSettings", QVariant::fromValue(&settings)},
            {"controller", QVariant::fromValue(&controller)}, {"desktop", QVariant::fromValue(&desktop)},
            {"providerTools", QVariant::fromValue(&tools)}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(TRANS_SOURCE_DIR "/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *popup = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *window = popup->findChild<QQuickWindow *>("settingsWindow");
        desktop.setWindows(popup, window);
        const auto capture = [&](const QString &name) {
            QSignalSpy frame(popup, &QQuickWindow::frameSwapped);
            popup->update();
            if (!frame.wait(1000)) return false;
            const auto path = qEnvironmentVariable("TRANS_UI_SCREENSHOT_DIR");
            if (path.isEmpty()) return true;
            QDir().mkpath(path);
            return popup->grabWindow().save(QDir(path).filePath("auto-" + name + ".png"));
        };
        const auto viewport = [](QQuickItem *item) {
            for (auto *parent = item->parentItem(); parent; parent = parent->parentItem()) {
                if (parent->property("contentY").isValid()) return parent;
            }
            return static_cast<QQuickItem *>(nullptr);
        };
        controller.translateText("hello");
        desktop.ShowTranslation();
        QTRY_VERIFY(popup->isExposed());
        QVERIFY(capture("loading"));
        manual->jobs.last()->succeed(QStringLiteral("你好"));
        QTRY_COMPARE(popup->size(), popup->property("preferredSize").toSize());
        QVERIFY(capture("word"));
        const auto small = popup->size();
        QCOMPARE(small, QSize(480, 360));
        auto *text = popup->findChild<QQuickItem *>("translationText");
        auto *resultViewport = viewport(text);
        QVERIFY(resultViewport);
        QVERIFY(resultViewport->property("contentHeight").toReal() <= resultViewport->height() + 1);

        controller.translateText("A paragraph should expand the window enough to read comfortably.");
        manual->jobs.last()->succeed(QStringLiteral("让不同语言的文字变得容易理解，窗口应随内容自动调整。\n").repeated(8));
        QTRY_COMPARE(popup->size(), popup->property("preferredSize").toSize());
        QVERIFY(capture("paragraph"));
        QVERIFY(popup->width() > small.width());
        QVERIFY(popup->height() > small.height());
        QVERIFY(resultViewport->property("contentHeight").toReal() <= resultViewport->height() + 1);
        const auto paragraphHeight = popup->height();
        auto values = settings.snapshot();
        values["fontSize"] = 32;
        QVERIFY(settings.save(values));
        QTRY_VERIFY(popup->height() > paragraphHeight);
        QTRY_COMPARE(popup->size(), popup->property("preferredSize").toSize());
        QVERIFY(capture("large-font"));

        controller.translateText(QStringLiteral("A long source paragraph with multiple lines.\n").repeated(60));
        manual->jobs.last()->succeed(QStringLiteral("很长的译文应该留在屏幕以内，并且可以完整滚动阅读。\n").repeated(120));
        QTRY_COMPARE(popup->size(), popup->property("preferredSize").toSize());
        QVERIFY(capture("long-text"));
        QVERIFY(popup->screen()->availableGeometry().contains(popup->frameGeometry()));
        QVERIFY(resultViewport->property("contentHeight").toReal() > resultViewport->height());
        const auto position = resultViewport->mapToScene(QPointF(resultViewport->width() / 2, resultViewport->height() / 2));
        QTest::wheelEvent(popup, position, QPoint(0, -120));
        QTRY_VERIFY(resultViewport->property("contentY").toReal() > 0);
        auto *copy = popup->findChild<QQuickItem *>("copyTranslationButton");
        QVERIFY(copy && copy->isEnabled());
        QVERIFY(popup->contentItem()->contains(popup->contentItem()->mapFromItem(copy, QPointF(copy->width(), copy->height()))));
        const auto large = popup->size();
        QTest::qWait(100);
        QCOMPARE(popup->size(), large); // Layout and compact-mode changes must settle, without oscillation.

        values["fontSize"] = 17;
        QVERIFY(settings.save(values));
        controller.translateText("hello");
        manual->jobs.last()->succeed(QStringLiteral("你好"));
        QTRY_COMPARE(popup->size(), small);
        QCOMPARE(resultViewport->property("contentY").toReal(), 0);
        desktop.closeTranslation();
        desktop.ShowTranslation();
        QTRY_COMPARE(popup->size(), small);
        QCOMPARE(manual->requests.size(), 4); // Resizing and reopening never issue another request.
        QVERIFY2(warnings.isEmpty(), "Automatic sizing must not introduce QML binding loops or layout warnings.");
    }

    void preferencesWheelScrolling_data()
    {
        QTest::addColumn<QString>("area");
        for (const auto &area : {"page", "scrollbar", "pixel-scrollbar", "short-prompt", "prompt-bottom",
                                "prompt-top", "prompt-scroll", "after-drag", "after-page-switch"})
            QTest::newRow(area) << QString::fromLatin1(area);
    }

    void preferencesWheelScrolling()
    {
        QFETCH(QString, area);
        QTemporaryDir directory;
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        auto platform = createPlatformServices();
        DesktopBridge desktop(controller, settings, *platform);
        ProviderTools tools(registry);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"appSettings", QVariant::fromValue(&settings)},
            {"controller", QVariant::fromValue(&controller)}, {"desktop", QVariant::fromValue(&desktop)},
            {"providerTools", QVariant::fromValue(&tools)}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(TRANS_SOURCE_DIR "/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *popup = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *window = popup->findChild<QQuickWindow *>("settingsWindow");
        QVERIFY(window);
        desktop.setWindows(popup, window);
        desktop.ShowSettings();
        window->resize(500, 600);
        window->findChild<QObject *>("settingsTabs")->setProperty("currentIndex", 1);
        QTRY_VERIFY(window->isExposed());
        auto *page = window->findChild<QQuickItem *>("translationPage");
        auto *prompt = window->findChild<QQuickItem *>("promptField");
        QVERIFY(page && prompt);
        auto *flickable = page->property("contentItem").value<QObject *>();
        QVERIFY(flickable);
        QTest::qWait(30);
        QQuickItem *inner = nullptr;
        int direction = -120;
        QPointF position = page->mapToScene(QPointF(3, page->height() / 2));
        if (area == "scrollbar" || area == "pixel-scrollbar" || area == "after-drag") {
            position = page->mapToScene(QPointF(page->width() - 2, page->height() / 2));
            if (area == "after-drag") {
                const auto start = page->mapToScene(QPointF(page->width() - 2, 20)).toPoint();
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(window, start + QPoint(0, 30));
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(0, 30));
                QTest::qWait(30);
                QVERIFY(flickable->property("contentY").toReal() > 0);
            }
        } else if (area == "short-prompt" || area.startsWith("prompt-")) {
            prompt->setProperty("text", area == "short-prompt" ? QStringLiteral("Short prompt")
                : QStringLiteral("A long editable prompt.\n").repeated(80));
            QTest::qWait(30);
            const qreal bottom = flickable->property("contentHeight").toReal() - flickable->property("height").toReal();
            flickable->setProperty("contentY", qBound(0.0, prompt->mapToItem(page, QPointF()).y() - 50, bottom));
            inner = prompt->parentItem();
            while (inner && !inner->property("contentY").isValid())
                inner = inner->parentItem();
            QVERIFY(inner && inner != flickable);
            if (area == "prompt-bottom") {
                const qreal innerBottom = inner->property("contentHeight").toReal() - inner->height();
                QVERIFY(innerBottom > 0);
                inner->setProperty("contentY", innerBottom);
            } else {
                inner->setProperty("contentY", 0);
            }
            if (area == "prompt-top")
                direction = 120;
            QTest::qWait(30);
            position = inner->mapToScene(QPointF(inner->width() / 2, inner->height() / 2));
        } else if (area == "after-page-switch") {
            auto *tabs = window->findChild<QObject *>("settingsTabs");
            tabs->setProperty("currentIndex", 0);
            QTest::qWait(30);
            tabs->setProperty("currentIndex", 1);
            QTest::qWait(30);
        }
        const qreal before = flickable->property("contentY").toReal();
        QVERIFY(before < flickable->property("contentHeight").toReal() - flickable->property("height").toReal() - 10);
        // No focus click: wheel input must work wherever the pointer is, including over a nested editor at its boundary.
        QTest::wheelEvent(window, position, area == "pixel-scrollbar" ? QPoint() : QPoint(0, direction),
            area == "pixel-scrollbar" ? QPoint(0, -35) : QPoint());
        if (area == "prompt-scroll") {
            // An editor that can still scroll must retain the wheel, leaving the page in place.
            QTRY_VERIFY_WITH_TIMEOUT(inner->property("contentY").toReal() > 1, 1000);
            QCOMPARE(flickable->property("contentY").toReal(), before);
        } else if (direction > 0) {
            QTRY_VERIFY_WITH_TIMEOUT(flickable->property("contentY").toReal() < before - 1, 1000);
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(flickable->property("contentY").toReal() > before + 1, 1000);
        }
    }

    void escapeClosesOnlyFocusedWindow()
    {
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<ManualProvider>();
        auto *manual = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        TestShortcut *shortcut;
        auto platform = testPlatform(&shortcut);
        DesktopBridge desktop(controller, settings, *platform);
        ProviderTools tools(registry);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"appSettings", QVariant::fromValue(&settings)},
            {"controller", QVariant::fromValue(&controller)}, {"desktop", QVariant::fromValue(&desktop)},
            {"providerTools", QVariant::fromValue(&tools)}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(TRANS_SOURCE_DIR "/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *popup = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *window = popup->findChild<QQuickWindow *>("settingsWindow");
        QVERIFY(window);
        desktop.setWindows(popup, window);

        // Both windows are visible, with keyboard focus inside the translation text.
        controller.translateText("selected text");
        manual->jobs.last()->succeed("译文");
        popup->show();
        desktop.ShowSettings();
        popup->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), popup);
        auto *text = popup->findChild<QQuickItem *>("translationText");
        QVERIFY(text);
        text->forceActiveFocus();
        QTRY_VERIFY(text->hasActiveFocus());
        QTest::keyClick(popup, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!popup->isVisible(), 1000);
        QVERIFY(window->isVisible());

        // Closing settings must not close or cancel the translation behind it.
        controller.translateText("next selection");
        auto *pending = manual->jobs.last();
        popup->show();
        window->requestActivate();
        // isActive() also includes transient relatives; wait for the actual keyboard target.
        QTRY_COMPARE(QGuiApplication::focusWindow(), window);
        auto *tabs = window->findChild<QObject *>("settingsTabs");
        auto *desktopPage = window->findChild<QObject *>("desktopPage");
        auto *shortcutInput = window->findChild<QQuickItem *>("shortcutField");
        QVERIFY(tabs && desktopPage && shortcutInput);
        tabs->setProperty("currentIndex", 2);
        desktopPage->setProperty("recording", true);
        shortcutInput->forceActiveFocus();
        QTRY_VERIFY(shortcutInput->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!desktopPage->property("recording").toBool());
        QVERIFY(window->isVisible());
        QVERIFY(popup->isVisible());
        QVERIFY(!pending->cancelled);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 1000);
        QVERIFY(popup->isVisible());
        QVERIFY(!pending->cancelled);
        popup->requestActivate();
        QTRY_COMPARE(QGuiApplication::focusWindow(), popup);
        QTest::keyClick(popup, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!popup->isVisible(), 1000);
        QVERIFY(pending->cancelled);
        pending->succeed("late response");
        QVERIFY(!popup->isVisible());
    }

    void shortcutConflictsAndSaveFailureRollback()
    {
        QTemporaryDir directory;
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        TestShortcut *shortcut;
        auto platform = testPlatform(&shortcut);
        DesktopBridge desktop(controller, settings, *platform);
        QSignalSpy saved(&desktop, &DesktopBridge::settingsSaveFinished);
        auto draft = settings.snapshot();
        const auto original = settings.shortcut();
        draft["shortcut"] = "Ctrl+Alt+Y";
        shortcut->reject = true;
        desktop.saveSettings(draft);
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(!saved.takeFirst().first().toBool());
        QVERIFY(!QFile::exists(settings.configPath()));
        QCOMPARE(settings.shortcut(), original);
        shortcut->reject = false;
        // A regular file as the parent directory forces a real persistence error.
        QFile blocker(directory.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        AppSettings broken(registry, directory.filePath("blocker/settings.ini"));
        TranslationController brokenController(registry, broken);
        DesktopBridge brokenDesktop(brokenController, broken, *platform);
        QSignalSpy brokenSaved(&brokenDesktop, &DesktopBridge::settingsSaveFinished);
        brokenDesktop.saveSettings(draft);
        QTRY_COMPARE(brokenSaved.size(), 1);
        QVERIFY(!brokenSaved.first().first().toBool());
        QCOMPARE(shortcut->sequence(ShortcutAction::Selection), QStringLiteral("Meta+Shift+T"));
        desktop.saveSettings(draft);
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(saved.takeFirst().first().toBool());
        QCOMPARE(shortcut->sequence(ShortcutAction::Selection), QStringLiteral("Ctrl+Alt+Y"));
        QCOMPARE(settings.shortcut(), QStringLiteral("Ctrl+Alt+Y"));
        draft["shortcut"] = "";
        desktop.saveSettings(draft);
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(saved.first().first().toBool());
        QVERIFY(shortcut->sequence(ShortcutAction::Selection).isEmpty());
        QVERIFY(settings.shortcut().isEmpty());
    }

    void responsesMustBeComplete()
    {
        MockServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = R"({"status":"incomplete","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"partial"}]}]})";
        HttpTranslationProvider provider(HttpProtocol::OpenAI);
        auto *job = provider.translate({"Hello"}, {"openai", server.endpoint(), "model", "key"}, this);
        QSignalSpy failed(job, &TranslationJob::failed);
        QSignalSpy succeeded(job, &TranslationJob::succeeded);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(succeeded.count(), 0);
        QCOMPARE(failed.first().first().value<TranslationError>().code, ErrorCode::InvalidResponse);
    }
    void providerSettingsValidation()
    {
        auto registry = ProviderRegistry::builtins();
        const auto descriptor = registry.find("deepseek")->descriptor();
        ProviderConfig config{"deepseek", descriptor.defaultEndpoint, descriptor.defaultModel, "key", "chat"};
        QVERIFY(validateConfig(descriptor, config).isEmpty());
        config.headersJson = R"({"Authorization":"replacement"})";
        QVERIFY(!validateConfig(descriptor, config).isEmpty());
        config.headersJson = R"({"X-Test":"bad\nvalue"})";
        QVERIFY(!validateConfig(descriptor, config).isEmpty());
        config.headersJson = "{}";
        config.optionsJson = R"({"stream":true})";
        QVERIFY(!validateConfig(descriptor, config).isEmpty());
        config.optionsJson = "[]";
        QVERIFY(!validateConfig(descriptor, config).isEmpty());
        config.optionsJson = "{}";
        config.reasoning = "minimal";
        QVERIFY(!validateConfig(descriptor, config).isEmpty());
        config.reasoning = "none";
        config.temperature = 3;
        QVERIFY(!validateConfig(descriptor, config).isEmpty());
        QVERIFY(validateShortcut("Ctrl+Alt+Y").isEmpty());
        QVERIFY(validateShortcut("").isEmpty());
        QVERIFY(!validateShortcut("T").isEmpty());
        QVERIFY(!validateShortcut("Shift+T").isEmpty());
    }

    void migratesOldProviderSettings()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("settings.ini");
        {
            QSettings old(path, QSettings::IniFormat);
            old.setValue("translation/provider", "ollama");
            old.setValue("providers/ollama/apiKey", "old-key");
            old.setValue("providers/deepl/apiKey", "old-key");
            old.setValue("providers/openai/endpoint", "https://example.com/v1");
            old.setValue("providers/openai/model", "old-model");
            old.setValue("providers/openai/apiKey", "preserved-key");
        }
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, path);
        QCOMPARE(settings.providerId(), QStringLiteral("openai"));
        QCOMPARE(settings.config("openai").apiMode, QStringLiteral("chat"));
        QCOMPARE(settings.config("openai").model, QStringLiteral("old-model"));
        QCOMPARE(settings.config("openai").apiKey, QStringLiteral("preserved-key"));
        QVERIFY(settings.save(settings.snapshot()));
        QSettings migrated(path, QSettings::IniFormat);
        QVERIFY(!migrated.contains("providers/ollama/apiKey"));
        QVERIFY(!migrated.contains("providers/deepl/apiKey"));
        QCOMPARE(migrated.value("providers/openai/apiKey").toString(), QStringLiteral("preserved-key"));
    }

    void providerToolsUseUnsavedConfiguration()
    {
        MockServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = R"({"data":[{"id":"custom-model"},{"id":"another"},{"id":"custom-model"}]})";
        QTemporaryDir directory;
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, directory.filePath("settings.ini"));
        auto draft = settings.snapshot();
        auto configs = draft["providerConfigs"].toMap();
        auto config = configs["deepseek"].toMap();
        config["endpoint"] = server.endpoint() + "/v1/";
        config["apiKey"] = "unsaved-key";
        config["model"] = "custom-model";
        configs["deepseek"] = config;
        draft["providerConfigs"] = configs;
        ProviderTools tools(registry);
        tools.fetchModels("deepseek", draft);
        QTRY_VERIFY(!tools.busy());
        QCOMPARE(tools.models(), QStringList({"another", "custom-model"}));
        QCOMPARE(server.requests.first().path, QByteArray("/v1/models"));
        QCOMPARE(server.requests.first().headers.value("authorization"), QByteArray("Bearer unsaved-key"));
        QVERIFY(settings.config("deepseek").apiKey.isEmpty());
        QVERIFY(!QFile::exists(settings.configPath()));
        server.body = R"({"choices":[{"message":{"content":"你好，世界！"},"finish_reason":"stop"}]})";
        tools.testTranslation("deepseek", draft);
        QTRY_VERIFY(!tools.busy());
        QVERIFY(tools.message().contains("你好，世界！"));
        QCOMPARE(server.requests.last().body.value("model").toString(), QStringLiteral("custom-model"));
        QVERIFY(!server.requests.last().body.contains("temperature"));
        QVERIFY(!server.requests.last().body.contains("max_tokens"));
        QCOMPARE(server.requests.last().body.value("thinking").toObject().value("type").toString(), QStringLiteral("disabled"));
        server.status = 401;
        tools.fetchModels("deepseek", draft);
        QTRY_VERIFY(!tools.busy());
        QVERIFY(tools.message().contains("鉴权失败"));
        QVERIFY(tools.models().isEmpty());
        server.respond = false;
        tools.fetchModels("deepseek", draft);
        tools.clear();
        QVERIFY(!tools.busy());
        QVERIFY(tools.message().isEmpty());
    }

    void settingsPageSavesEveryOption()
    {
        QTemporaryDir directory;
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        TestShortcut *shortcut;
        auto platform = testPlatform(&shortcut);
        DesktopBridge desktop(controller, settings, *platform);
        ProviderTools tools(registry);
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{"appSettings", QVariant::fromValue(&settings)},
            {"controller", QVariant::fromValue(&controller)}, {"desktop", QVariant::fromValue(&desktop)},
            {"providerTools", QVariant::fromValue(&tools)}});
        engine.load(QUrl::fromLocalFile(QStringLiteral(TRANS_SOURCE_DIR "/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *popup = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *window = popup->findChild<QQuickWindow *>("settingsWindow");
        QVERIFY(window);
        desktop.setWindows(popup, window);
        desktop.ShowSettings();
        QTRY_VERIFY(window->isVisible());
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->isVisible());
        desktop.ShowSettings();
        QTRY_VERIFY(window->isVisible());
        auto field = [window](const char *name) { return window->findChild<QObject *>(QString::fromUtf8(name)); };
        const auto set = [&field](const char *name, const char *property, const QVariant &value) {
            auto *object = field(name);
            return object && object->setProperty(property, value);
        };
        auto *page = field("providerPage");
        QVERIFY(page);
        QVERIFY(set("apiKeyField", "text", "openai-test-key"));
        QVERIFY(set("endpointField", "text", "https://openai.example/v1"));
        QVERIFY(set("modelField", "editText", "custom-openai-model"));
        QVERIFY(QMetaObject::invokeMethod(page, "selectProvider", Q_ARG(QVariant, 1)));
        QVERIFY(set("apiKeyField", "text", "deepseek-test-key"));
        QVERIFY(set("endpointField", "text", "https://deepseek.example"));
        QVERIFY(set("modelField", "editText", "custom-deepseek-model"));
        QVERIFY(set("temperatureEnabledField", "checked", true));
        QVERIFY(set("temperatureField", "value", 70));
        QVERIFY(set("outputTokensField", "value", 2048));
        QVERIFY(set("reasoningField", "currentIndex", 2));
        QVERIFY(set("headersField", "text", R"({"X-Project":"ui-test"})"));
        QVERIFY(set("optionsField", "text", R"({"top_p":0.7})"));
        QVERIFY(set("sourceLanguageField", "currentIndex", 2));
        QVERIFY(set("targetLanguageField", "currentIndex", 2));
        QVERIFY(set("promptField", "text", "Translate {{sourceLanguage}} to {{targetLanguage}} precisely."));
        QVERIFY(set("timeoutField", "value", 75));
        QVERIFY(set("maxInputField", "value", 15000));
        QVERIFY(set("maxResponseField", "value", 4096));
        QVERIFY(!field("widthField"));
        QVERIFY(!field("heightField"));
        QVERIFY(!field("rememberSizeField"));
        QVERIFY(set("fontSizeField", "value", 23));
        QVERIFY(set("stayOnTopField", "checked", false));
        QVERIFY(set("restoreFocusField", "checked", false));
        QVERIFY(set("positionField", "currentIndex", 1));
        auto *desktopPage = field("desktopPage");
        QVERIFY(desktopPage);
        QVERIFY(set("settingsTabs", "currentIndex", 2));
        auto *shortcutInput = qobject_cast<QQuickItem *>(field("shortcutField"));
        QVERIFY(shortcutInput);
        QVERIFY(desktopPage->setProperty("recording", true));
        shortcutInput->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(desktopPage->property("shortcut").toString(), QStringLiteral("Ctrl+Alt+Y"));
        QVERIFY(!desktopPage->property("recording").toBool());
        auto *screenshotInput = qobject_cast<QQuickItem *>(field("screenshotShortcutField"));
        QVERIFY(screenshotInput);
        QVERIFY(desktopPage->setProperty("recordingScreenshot", true));
        screenshotInput->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(desktopPage->property("screenshotShortcut").toString(), QStringLiteral("Ctrl+Alt+O"));
        QVERIFY(!desktopPage->property("recordingScreenshot").toBool());
        QVERIFY(set("ocrApiKeyField", "text", "ocr-key"));
        QVERIFY(set("ocrSecretKeyField", "text", "ocr-secret"));
        QSignalSpy saved(&desktop, &DesktopBridge::settingsSaveFinished);
        QVERIFY(QMetaObject::invokeMethod(window, "saveAll"));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY2(saved.takeFirst().first().toBool(), qPrintable(desktop.settingsError()));
        const auto expected = settings.snapshot();
        QCOMPARE(expected.value("ocrApiKey").toString(), QStringLiteral("ocr-key"));
        QCOMPARE(expected.value("ocrSecretKey").toString(), QStringLiteral("ocr-secret"));
        QCOMPARE(expected.value("screenshotShortcut").toString(), QStringLiteral("Ctrl+Alt+O"));
        // Cancelling before Portal starts must restore visibility without discarding drafts.
        QVERIFY(set("ocrSecretKeyField", "text", "unsaved-ocr-secret"));
        desktop.TranslateScreenshot();
        QVERIFY(!window->isVisible());
        controller.cancel();
        QVERIFY(window->isVisible());
        QCOMPARE(field("ocrSecretKeyField")->property("text").toString(), QStringLiteral("unsaved-ocr-secret"));
        QCOMPARE(settings.providerId(), QStringLiteral("deepseek"));
        QCOMPARE(settings.config("openai").apiKey, QStringLiteral("openai-test-key"));
        QCOMPARE(settings.config("openai").model, QStringLiteral("custom-openai-model"));
        QCOMPARE(settings.config("deepseek").apiKey, QStringLiteral("deepseek-test-key"));
        QCOMPARE(settings.config("deepseek").temperature, 0.7);
        QCOMPARE(settings.config("deepseek").reasoning, QStringLiteral("low"));
        QCOMPARE(settings.request("hello").sourceLanguage, QStringLiteral("en"));
        QCOMPARE(settings.targetLanguage(), QStringLiteral("ja"));
        QCOMPARE(settings.request("hello").timeoutMs, 75000);
        QCOMPARE(settings.maxInputChars(), 15000);
        QCOMPARE(settings.fontSize(), 23);
        QCOMPARE(settings.shortcut(), QStringLiteral("Ctrl+Alt+Y"));
        QVERIFY(!popup->flags().testFlag(Qt::WindowStaysOnTopHint));
        const auto font = popup->findChild<QObject *>("translationText")->property("font").value<QFont>();
        QCOMPARE(font.pixelSize(), 23);
        // Hide/reopen must discard unsaved keys and restore both providers' saved fields.
        QVERIFY(set("apiKeyField", "text", "discarded-key"));
        desktop.closeSettings();
        QCOMPARE(field("apiKeyField")->property("text").toString(), QString());
        QCOMPARE(field("ocrSecretKeyField")->property("text").toString(), QString());
        desktop.ShowSettings();
        QCOMPARE(field("apiKeyField")->property("text").toString(), QStringLiteral("deepseek-test-key"));
        QVariant collected;
        QVERIFY(QMetaObject::invokeMethod(window, "collect", Q_RETURN_ARG(QVariant, collected)));
        QCOMPARE(collected.toMap(), expected);
        AppSettings reloaded(registry, settings.configPath());
        QCOMPARE(reloaded.snapshot(), expected);
        desktop.closeSettings();
        desktop.TranslateSelection();
        QTRY_VERIFY(popup->isVisible());
        popup->requestActivate();
        QTRY_VERIFY(popup->isActive());
        QTest::keyClick(popup, Qt::Key_Escape);
        QTRY_VERIFY(!popup->isVisible());
        // A bad advanced parameter must fail in the page and leave saved settings intact.
        QVERIFY(set("optionsField", "text", R"({"stream":true})"));
        QVERIFY(QMetaObject::invokeMethod(window, "saveAll"));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(!saved.first().first().toBool());
        QCOMPARE(settings.snapshot(), expected);
        QVERIFY(!desktop.settingsError().isEmpty());
        QCOMPARE(warnings.count(), 0);
    }
    void protocols_data()
    {
        QTest::addColumn<int>("protocol");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<QByteArray>("path");
        QTest::newRow("openai-responses") << int(HttpProtocol::OpenAI) << QString("responses")
            << QByteArray(R"({"status":"completed","output":[{"type":"reasoning","summary":[]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"你好\n世界"}]}]})") << QByteArray("/proxy/v1/responses");
        QTest::newRow("openai-chat") << int(HttpProtocol::OpenAI) << QString("chat")
            << QByteArray(R"({"choices":[{"message":{"content":"你好\n世界"},"finish_reason":"stop"}]})") << QByteArray("/proxy/v1/chat/completions");
        QTest::newRow("deepseek") << int(HttpProtocol::DeepSeek) << QString("chat")
            << QByteArray(R"({"choices":[{"message":{"content":"你好\n世界","reasoning_content":"not the translation"},"finish_reason":"stop"}]})") << QByteArray("/proxy/v1/chat/completions");
    }

    void protocols()
    {
        QFETCH(int, protocol);
        QFETCH(QString, mode);
        QFETCH(QByteArray, response);
        QFETCH(QByteArray, path);
        MockServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = response;
        HttpTranslationProvider provider(static_cast<HttpProtocol>(protocol));
        ProviderConfig config{provider.descriptor().id, server.endpoint() + "/proxy/v1/", "test-model", "test-key"};
        config.apiMode = mode;
        config.temperatureEnabled = true;
        config.temperature = 0.4;
        config.maxOutputTokens = 1024;
        config.reasoning = "low";
        config.headersJson = R"({"X-Project":"trans"})";
        config.optionsJson = R"({"top_p":0.9})";
        TranslationRequest input{"Hello\n世界", "en", "zh-CN"};
        input.systemPrompt = "Translate {{sourceLanguage}} to {{targetLanguage}}. Keep lines.";
        auto *job = provider.translate(input, config, this);
        QSignalSpy succeeded(job, &TranslationJob::succeeded);
        QSignalSpy failed(job, &TranslationJob::failed);
        QTRY_COMPARE(succeeded.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(succeeded.first().first().value<TranslationResult>().text, QStringLiteral("你好\n世界"));
        QCOMPARE(server.requests.size(), 1);
        const auto request = server.requests.first();
        QCOMPARE(request.path, path);
        QCOMPARE(request.headers.value("content-type"), QByteArray("application/json"));
        QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer test-key"));
        QCOMPARE(request.headers.value("x-project"), QByteArray("trans"));
        QCOMPARE(request.body.value("model").toString(), QStringLiteral("test-model"));
        QCOMPARE(request.body.value("stream").toBool(), false);
        QCOMPARE(request.body.value("temperature").toDouble(), 0.4);
        QCOMPARE(request.body.value("top_p").toDouble(), 0.9);
        if (mode == "responses") {
            QCOMPARE(request.body.value("store").toBool(true), false);
            QCOMPARE(request.body.value("input").toString(), input.text);
            QCOMPARE(request.body.value("instructions").toString(), QStringLiteral("Translate en to zh-CN. Keep lines."));
            QCOMPARE(request.body.value("max_output_tokens").toInt(), 1024);
            QCOMPARE(request.body.value("reasoning").toObject().value("effort").toString(), QStringLiteral("low"));
        } else {
            const auto messages = request.body.value("messages").toArray();
            QCOMPARE(messages.size(), 2);
            QCOMPARE(messages.first().toObject().value("content").toString(), QStringLiteral("Translate en to zh-CN. Keep lines."));
            QCOMPARE(messages.last().toObject().value("content").toString(), input.text);
            QCOMPARE(request.body.value("reasoning_effort").toString(), QStringLiteral("low"));
            QCOMPARE(request.body.value(protocol == int(HttpProtocol::DeepSeek) ? "max_tokens" : "max_completion_tokens").toInt(), 1024);
            if (protocol == int(HttpProtocol::DeepSeek))
                QCOMPARE(request.body.value("thinking").toObject().value("type").toString(), QStringLiteral("enabled"));
        }
    }

    void errors_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("expected");
        QTest::newRow("unauthorized") << 401 << QByteArray("{}") << int(ErrorCode::Authentication);
        QTest::newRow("forbidden") << 403 << QByteArray("{}") << int(ErrorCode::Authentication);
        QTest::newRow("rate-limited") << 429 << QByteArray("{}") << int(ErrorCode::RateLimit);
        QTest::newRow("server") << 500 << QByteArray("{}") << int(ErrorCode::Network);
        QTest::newRow("redirect") << 302 << QByteArray("{}") << int(ErrorCode::Network);
        QTest::newRow("invalid-json") << 200 << QByteArray("<html>error</html>") << int(ErrorCode::InvalidResponse);
        QTest::newRow("missing-choices") << 200 << QByteArray("{}") << int(ErrorCode::InvalidResponse);
        QTest::newRow("empty-choices") << 200 << QByteArray(R"({"choices":[]})") << int(ErrorCode::InvalidResponse);
        QTest::newRow("empty-text") << 200 << QByteArray(R"({"choices":[{"message":{"content":" "}}]})") << int(ErrorCode::InvalidResponse);
        QTest::newRow("truncated") << 200 << QByteArray(R"({"choices":[{"message":{"content":"partial"},"finish_reason":"length"}]})") << int(ErrorCode::InvalidResponse);
        QTest::newRow("response-limit") << 200 << QByteArray(2 * 1024 * 1024 + 1, 'x') << int(ErrorCode::InvalidResponse);
    }

    void errors()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, expected);
        MockServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.status = status;
        server.body = body;
        HttpTranslationProvider provider(HttpProtocol::OpenAI);
        auto *job = provider.translate({"Hello"}, {"openai", server.endpoint(), "model", "test-key", "chat"}, this);
        QSignalSpy failed(job, &TranslationJob::failed);
        QSignalSpy succeeded(job, &TranslationJob::succeeded);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(succeeded.count(), 0);
        QCOMPARE(int(failed.first().first().value<TranslationError>().code), expected);
    }

    void timeoutAndCancel()
    {
        MockServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.respond = false;
        HttpTranslationProvider provider(HttpProtocol::OpenAI);
        const ProviderConfig config{"openai", server.endpoint(), "model", "test-key", "chat"};
        auto *job = provider.translate({"Hello", "auto", "zh-CN", 80}, config, this);
        QSignalSpy failed(job, &TranslationJob::failed);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.first().first().value<TranslationError>().code, ErrorCode::Timeout);

        auto *cancelled = provider.translate({"Hello"}, config, this);
        QSignalSpy cancelledSpy(cancelled, &TranslationJob::failed);
        QTRY_COMPARE(server.requests.size(), 2);
        cancelled->cancel();
        QCOMPARE(cancelledSpy.count(), 1);
        QCOMPARE(cancelledSpy.first().first().value<TranslationError>().code, ErrorCode::Cancelled);
    }

    void configurationErrorsAreAsync()
    {
        HttpTranslationProvider provider(HttpProtocol::DeepSeek);
        auto *job = provider.translate({"Hello"}, {"deepseek", "https://api.deepseek.com", "deepseek-flash", {}, "chat"}, this);
        QSignalSpy failed(job, &TranslationJob::failed);
        QCOMPARE(failed.count(), 0);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.first().first().value<TranslationError>().code, ErrorCode::Configuration);
    }

    void latestRequestWinsAndRetryUsesSettings()
    {
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<ManualProvider>();
        auto *manual = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        controller.translateText("first");
        controller.translateText("second");
        QVERIFY(manual->jobs[0]->cancelled);
        manual->jobs[1]->succeed("第二个");
        manual->jobs[0]->succeed("过时的译文");
        manual->jobs[0]->fail();
        QCOMPARE(controller.sourceText(), QStringLiteral("second"));
        QCOMPARE(controller.translatedText(), QStringLiteral("第二个"));
        QCOMPARE(controller.status(), QStringLiteral("success"));

        auto next = settings.snapshot();
        next["targetLanguage"] = "ja";
        next["sourceLanguage"] = "en";
        next["timeoutSeconds"] = 45;
        next["maxResponseKiB"] = 128;
        QVERIFY(settings.save(next));
        // Existing results retain the language/provider labels of their own request.
        QCOMPARE(controller.providerId(), QStringLiteral("openai"));
        QCOMPARE(controller.sourceLanguage(), QStringLiteral("auto"));
        QCOMPARE(controller.targetLanguage(), QStringLiteral("zh-CN"));
        controller.retry();
        QCOMPARE(controller.sourceLanguage(), QStringLiteral("en"));
        QCOMPARE(controller.targetLanguage(), QStringLiteral("ja"));
        QCOMPARE(manual->requests.last().text, QStringLiteral("second"));
        QCOMPARE(manual->requests.last().targetLanguage, QStringLiteral("ja"));
        QCOMPARE(manual->requests.last().sourceLanguage, QStringLiteral("en"));
        QCOMPARE(manual->requests.last().timeoutMs, 45000);
        QCOMPARE(manual->requests.last().maxResponseBytes, 128 * 1024);
        controller.cancel();
        manual->jobs.last()->succeed("cancelled response");
        QCOMPARE(controller.status(), QStringLiteral("cancelled"));
        QVERIFY(controller.translatedText().isEmpty());
    }

    void emptyAndLongSelectionsDoNotSend()
    {
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<ManualProvider>();
        auto *manual = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        controller.translateText(" \n\t ");
        QCOMPARE(controller.status(), QStringLiteral("error"));
        controller.translateText(QString(20001, 'x'));
        QCOMPARE(manual->jobs.size(), 0);
        controller.translateText("你好\n世界");
        QCOMPARE(manual->requests.first().text, QStringLiteral("你好\n世界"));
        controller.selectionError("unsupported platform");
        QVERIFY(manual->jobs.first()->cancelled);
        manual->jobs.first()->fail();
        QCOMPARE(controller.message(), QStringLiteral("unsupported platform"));
    }

    void localSettingsRoundTripAndPermissions()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("trans/settings.ini");
        auto registry = ProviderRegistry::builtins();
        QVariantMap expected;
        {
            AppSettings settings(registry, path);
            QCOMPARE(settings.providers().size(), 2);
            QVERIFY(!registry.find("ollama"));
            QVERIFY(!registry.find("deepl"));
            QVERIFY(!settings.isConfigured());
            QVERIFY(!QFile::exists(path));
            expected = settings.snapshot();
            auto configs = expected["providerConfigs"].toMap();
            for (const auto &id : {"openai", "deepseek"}) {
                auto fields = configs[id].toMap();
                fields["apiKey"] = QString(id) + "-dummy-local-key";
                fields["temperatureEnabled"] = true;
                fields["temperature"] = 0.7;
                fields["maxOutputTokens"] = 1024;
                fields["headersJson"] = R"({"X-Custom":"value"})";
                fields["optionsJson"] = R"({"top_p":0.8})";
                configs[id] = fields;
            }
            expected["providerConfigs"] = configs;
            expected["providerId"] = "deepseek";
            expected["sourceLanguage"] = "en";
            expected["targetLanguage"] = "ja";
            expected["systemPrompt"] = "Translate {{sourceLanguage}} to {{targetLanguage}}";
            expected["timeoutSeconds"] = 60;
            expected["maxInputChars"] = 9000;
            expected["maxResponseKiB"] = 500;
            expected["shortcut"] = "Ctrl+Alt+Y";
            expected["fontSize"] = 22;
            expected["stayOnTop"] = false;
            expected["restoreFocus"] = false;
            expected["popupPosition"] = "cursor";
            QVERIFY2(settings.save(expected), qPrintable(settings.lastError()));
        }
        AppSettings loaded(registry, path);
        QCOMPARE(loaded.snapshot(), expected);
        QVERIFY(loaded.isConfigured());
        #ifdef Q_OS_UNIX
        const auto permissions = QFile::permissions(path);
        QVERIFY(permissions.testFlag(QFileDevice::ReadOwner));
        QVERIFY(permissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                                 | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)));
        #endif
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("dummy-local-key"));
        auto invalid = expected;
        invalid["providerId"] = "unknown";
        QVERIFY(!loaded.save(invalid));
        invalid = expected;
        invalid["timeoutSeconds"] = 0;
        QVERIFY(!loaded.save(invalid));
        invalid = expected;
        invalid["systemPrompt"] = "missing target placeholder";
        QVERIFY(!loaded.save(invalid));
        QCOMPARE(loaded.snapshot(), expected);
    }

    void popupStaysWithinScreen()
    {
        const QRect leftScreen(-1920, 40, 1920, 1040);
        const auto normal = PopupPresenter::centeredGeometry(leftScreen, {600, 480});
        QVERIFY(leftScreen.contains(normal));
        QCOMPARE(normal.size(), QSize(600, 480));
        QCOMPARE(normal.x(), -1260);
        QCOMPARE(PopupPresenter::centeredGeometry({0, 30, 320, 240}, {600, 480}), QRect(0, 30, 320, 240));
        const auto nearEdge = PopupPresenter::cursorGeometry(leftScreen, {600, 480}, {-1, 1079});
        QVERIFY(leftScreen.contains(nearEdge));
        QCOMPARE(nearEdge.bottomRight(), leftScreen.bottomRight());
        const QMargins frame(5, 30, 5, 5);
        const auto resized = PopupPresenter::boundedGeometry(leftScreen, QRect(-200, 950, 720, 900), frame);
        QVERIFY(leftScreen.contains(resized.marginsAdded(frame)));
        const QRect smallScreen(0, 25, 320, 240);
        const auto bounded = PopupPresenter::boundedGeometry(smallScreen, QRect(300, 200, 720, 900), frame);
        QCOMPARE(bounded.marginsAdded(frame), smallScreen);
    }

    void obsoleteWindowDimensionsAreIgnored()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("settings.ini");
        {
            QSettings old(path, QSettings::IniFormat);
            old.setValue("window/width", -1);
            old.setValue("window/height", 99999);
            old.setValue("window/rememberSize", true);
            old.setValue("window/fontSize", 24);
        }
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, path);
        QVERIFY(settings.lastError().isEmpty());
        QCOMPARE(settings.fontSize(), 24);
        for (const auto &name : {"windowWidth", "windowHeight", "rememberWindowSize"})
            QVERIFY(!settings.snapshot().contains(name));
        QVERIFY(settings.save(settings.snapshot()));
        QSettings saved(path, QSettings::IniFormat);
        QVERIFY(!saved.contains("window/width"));
        QVERIFY(!saved.contains("window/height"));
        QVERIFY(!saved.contains("window/rememberSize"));
    }

#ifdef Q_OS_LINUX
    void selectionPreservesClipboard()
    {
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText("clipboard sentinel", QClipboard::Clipboard);
        std::unique_ptr<SelectionReader> reader(createSelectionReader());
        if (QGuiApplication::platformName() == "xcb")
            clipboard->setText("selected text", QClipboard::Selection);
        auto *job = reader->read({}, this);
        QSignalSpy success(job, &SelectionJob::succeeded);
        QSignalSpy failure(job, &SelectionJob::failed);
        QVERIFY(success.isEmpty() && failure.isEmpty());
        if (QGuiApplication::platformName() == "xcb") {
            QTRY_COMPARE(success.size(), 1);
            QCOMPARE(qvariant_cast<SelectionResult>(success.first().first()).text, QStringLiteral("selected text"));
        } else {
            QTRY_COMPARE(failure.size(), 1);
            QCOMPARE(qvariant_cast<PlatformError>(failure.first().first()).code, PlatformErrorCode::Unsupported);
        }
        QCOMPARE(clipboard->text(QClipboard::Clipboard), QStringLiteral("clipboard sentinel"));
    }

    void isolatedDbusCommands()
    {
        QTemporaryDir directory;
        QProcess daemon;
        daemon.start(QStringLiteral("dbus-daemon"), {"--session", "--nofork", "--nopidfile", "--print-address=1",
                     "--address=unix:tmpdir=" + directory.path()});
        const auto stopDaemon = qScopeGuard([&] {
            daemon.terminate();
            if (!daemon.waitForFinished(2000)) {
                daemon.kill();
                daemon.waitForFinished(2000);
            }
        });
        QVERIFY(daemon.waitForStarted());
        QVERIFY(daemon.waitForReadyRead());
        const auto address = QString::fromUtf8(daemon.readLine()).trimmed();
        QVERIFY(!address.isEmpty());
        auto primary = QDBusConnection::connectToBus(address, "trans-test-primary");
        auto secondary = QDBusConnection::connectToBus(address, "trans-test-secondary");
        const auto disconnectBus = qScopeGuard([] {
            QDBusConnection::disconnectFromBus("trans-test-secondary");
            QDBusConnection::disconnectFromBus("trans-test-primary");
        });
        QVERIFY(primary.isConnected());
        QVERIFY(secondary.isConnected());

        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, directory.filePath("settings.ini"));
        TranslationController controller(registry, settings);
        auto platform = createPlatformServices();
        DesktopBridge desktop(controller, settings, *platform);
        QWindow popup;
        QWindow settingsWindow;
        desktop.setWindows(&popup, &settingsWindow);
        const auto service = QStringLiteral("io.github.trans.Trans");
        auto channel = createLinuxInstanceChannel(primary);
        QCOMPARE(channel->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        QVERIFY(!settingsWindow.isVisible());

        auto message = QDBusMessage::createMethodCall(service, "/Trans", service, "ShowSettings");
        QDBusPendingCallWatcher showCall(secondary.asyncCall(message));
        QTest::qWait(30);
        QVERIFY(!showCall.isFinished());
        QVERIFY(!settingsWindow.isVisible());
        channel->setReady([&desktop](AppCommand command) { desktop.dispatchCommand(command); });
        QTRY_VERIFY(showCall.isFinished());
        QVERIFY(!QDBusPendingReply<>(showCall).isError());
        QVERIFY(settingsWindow.isVisible());
        desktop.closeSettings();
        QVERIFY(!settingsWindow.isVisible());

        message = QDBusMessage::createMethodCall(service, "/Trans", service, "ShowTranslation");
        QDBusPendingCallWatcher openCall(secondary.asyncCall(message));
        QTRY_VERIFY(openCall.isFinished());
        QVERIFY(!QDBusPendingReply<>(openCall).isError());
        QVERIFY(popup.isVisible());
        QCOMPARE(controller.status(), QStringLiteral("idle"));
        desktop.closeTranslation();

        message = QDBusMessage::createMethodCall(service, "/Trans", service, "TranslateSelection");
        QDBusPendingCallWatcher translateCall(secondary.asyncCall(message));
        QTRY_VERIFY(translateCall.isFinished());
        QVERIFY(!QDBusPendingReply<>(translateCall).isError());
        QTRY_VERIFY(popup.isVisible());
        QTRY_COMPARE(controller.status(), QStringLiteral("error"));
        desktop.closeTranslation();
        QVERIFY(!popup.isVisible());

        message = QDBusMessage::createMethodCall(service, "/Trans", service, "TranslateScreenshot");
        QDBusPendingCallWatcher captureCall(secondary.asyncCall(message));
        QTRY_VERIFY(captureCall.isFinished());
        QVERIFY(!QDBusPendingReply<>(captureCall).isError());
        QVERIFY(popup.isVisible());
        QCOMPARE(controller.status(), QStringLiteral("error"));
        desktop.closeTranslation();

        message = QDBusMessage::createMethodCall(service, "/Trans", service, "Execute");
        QDBusPendingCallWatcher unknownCall(secondary.asyncCall(message));
        QTRY_VERIFY(unknownCall.isFinished());
        QCOMPARE(QDBusPendingReply<>(unknownCall).error().type(), QDBusError::UnknownMethod);

        auto forwarder = createLinuxInstanceChannel(secondary);
        QCOMPARE(forwarder->start(AppCommand::ShowSettings).role, InstanceRole::Forwarded);
        QVERIFY(settingsWindow.isVisible());
        desktop.closeSettings();

        channel.reset();
        channel = createLinuxInstanceChannel(primary);
        QCOMPARE(channel->start(AppCommand::ShowSettings).role, InstanceRole::Primary);
        message = QDBusMessage::createMethodCall(service, "/Trans", service, "ShowSettings");
        QDBusPendingCallWatcher failedStartup(secondary.asyncCall(message));
        QTest::qWait(30);
        QVERIFY(!failedStartup.isFinished());
        channel.reset();
        QTRY_VERIFY(failedStartup.isFinished());
        QVERIFY(QDBusPendingReply<>(failedStartup).isError());
        QVERIFY(!settingsWindow.isVisible());
    }
    #endif
};

QTEST_MAIN(TransTest)
#include "test_trans.moc"
