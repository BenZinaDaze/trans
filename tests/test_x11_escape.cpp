#include "controller.h"
#include "desktop.h"
#include "platform/platform_services.h"
#include "provider_tools.h"

#include <KGlobalAccel>
#include <KWindowInfo>
#include <KX11Extras>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QLineEdit>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>

using namespace Trans;

// A separate process provides a real PRIMARY selection and owns keyboard focus.
class SelectionSource final : public QLineEdit {
protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
            QTextStream(stdout) << "source-escape\n" << Qt::flush;
        QLineEdit::keyPressEvent(event);
    }
};

class PendingJob final : public TranslationJob {
public:
    using TranslationJob::TranslationJob;
    bool cancelled = false;
    void cancel() override { cancelled = true; }
    void finish(const QString &text = QStringLiteral("测试译文")) { emit succeeded({text, "EN"}); }
};

class TestProvider final : public TranslationProvider {
public:
    PendingJob *job = nullptr;
    ProviderDescriptor descriptor() const override { return {"openai", "Test", "http://localhost", "", false, false}; }
    TranslationJob *translate(const TranslationRequest &, const ProviderConfig &, QObject *owner) override
    {
        job = new PendingJob(owner);
        return job;
    }
};

class X11EscapeTest final : public QObject {
    Q_OBJECT
private:
    bool xdotool(const QStringList &arguments)
    {
        QProcess process;
        process.start(QStringLiteral("xdotool"), arguments);
        if (!process.waitForStarted(2000))
            return false;
        // Keep the Qt event loop running while X11/DBus deliver events.
        if (!QTest::qWaitFor([&] { return process.state() == QProcess::NotRunning; }, 4000)) {
            process.kill();
            process.waitForFinished(1000);
            return false;
        }
        return process.exitCode() == 0;
    }

private slots:
    void regionScreenshot()
    {
        const auto oldFocus = KX11Extras::activeWindow();
        const auto oldCursor = QCursor::pos();
        const auto restore = qScopeGuard([&] {
            QCursor::setPos(oldCursor);
            if (KX11Extras::hasWId(oldFocus)) KX11Extras::forceActiveWindow(oldFocus);
        });
        auto findOverlay = []() -> QWidget * {
            for (auto *widget : QApplication::topLevelWidgets())
                if (widget->objectName() == "screenshotRegionOverlay" && widget->isVisible()
                    && widget->geometry().contains(QCursor::pos())) return widget;
            return nullptr;
        };
        std::unique_ptr<ScreenshotService> screenshots(createScreenshotService());
        auto *job = screenshots->captureRegion(this);
        QSignalSpy success(job, &ScreenshotJob::succeeded);
        QSignalSpy failure(job, &ScreenshotJob::failed);
        QTRY_VERIFY(findOverlay());
        auto *overlay = findOverlay();
        const auto origin = overlay->mapToGlobal(QPoint(50, 60));
        const auto end = overlay->mapToGlobal(QPoint(230, 160));
        QVERIFY(xdotool({"mousemove", QString::number(origin.x()), QString::number(origin.y()), "mousedown", "1"}));
        QVERIFY(xdotool({"mousemove", QString::number(end.x()), QString::number(end.y()), "mouseup", "1"}));
        QTRY_COMPARE(success.size(), 1);
        QVERIFY(failure.isEmpty());
        const auto image = qvariant_cast<QImage>(success.first().first());
        const qreal dpr = QGuiApplication::screenAt(origin)->devicePixelRatio();
        QVERIFY(qAbs(image.width() - 180 * dpr) <= 1);
        QVERIFY(qAbs(image.height() - 100 * dpr) <= 1);
        QTRY_VERIFY(!findOverlay());
        // Escape must work immediately, without clicking to focus the overlay.
        job = screenshots->captureRegion(this);
        QSignalSpy cancelled(job, &ScreenshotJob::failed);
        QTRY_VERIFY(findOverlay());
        QVERIFY(xdotool({"key", "Escape"}));
        QTRY_COMPARE(cancelled.size(), 1);
        QCOMPARE(qvariant_cast<PlatformError>(cancelled.first().first()).code, PlatformErrorCode::Cancelled);
        QTRY_VERIFY(!findOverlay());
    }
    void automaticPopupSize_data()
    {
        QTest::addColumn<QString>("position");
        QTest::newRow("center") << QStringLiteral("screen");
        QTest::newRow("cursor-at-screen-edge") << QStringLiteral("cursor");
    }

    void automaticPopupSize()
    {
        QFETCH(QString, position);
        const auto oldFocus = KX11Extras::activeWindow();
        const auto oldCursor = QCursor::pos();
        const auto restoreDesktop = qScopeGuard([&] {
            QCursor::setPos(oldCursor);
            if (KX11Extras::hasWId(oldFocus))
                KX11Extras::forceActiveWindow(oldFocus);
        });
        auto *screen = QGuiApplication::screenAt(oldCursor);
        QVERIFY(screen);
        const auto available = screen->availableGeometry();
        QCursor::setPos(available.bottomRight() - QPoint(2, 2));
        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<TestProvider>();
        auto *manual = provider.get();
        registry.add(std::move(provider));
        AppSettings settings(registry, directory.filePath("settings.ini"));
        auto values = settings.snapshot();
        values["popupPosition"] = position;
        QVERIFY(settings.save(values));
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
        desktop.setWindows(popup, window);
        controller.translateText("hello");
        manual->job->finish(QStringLiteral("你好"));
        desktop.ShowTranslation();
        QTRY_VERIFY(popup->isExposed());
        QTRY_COMPARE(popup->size(), popup->property("preferredSize").toSize());
        QTRY_VERIFY(available.contains(popup->frameGeometry()));
        const auto small = popup->size();
        const auto initialFrame = popup->frameGeometry();
        QCursor::setPos(available.topLeft() + QPoint(10, 10));
        controller.translateText(QStringLiteral("A long source paragraph.\n").repeated(80));
        manual->job->finish(QStringLiteral("译文随内容展开，超长文本在窗口内部滚动。\n").repeated(150));
        // These short lines require more height, not necessarily more width:
        // native font metrics may keep every line within the minimum width.
        QTRY_VERIFY(popup->height() > small.height());
        QTRY_COMPARE(popup->size(), popup->property("preferredSize").toSize());
        QTRY_VERIFY(available.contains(popup->frameGeometry()));
        // Results resize in place on the original screen instead of chasing the pointer.
        if (position == "screen")
            QTRY_VERIFY_WITH_TIMEOUT((popup->frameGeometry().center() - initialFrame.center()).manhattanLength() <= 4, 2000);
        else
            QTRY_VERIFY_WITH_TIMEOUT((popup->frameGeometry().bottomRight() - initialFrame.bottomRight()).manhattanLength() <= 4, 2000);
        controller.translateText("hello");
        manual->job->finish(QStringLiteral("你好"));
        QTRY_COMPARE(popup->size(), small);
        QTRY_VERIFY(available.contains(popup->frameGeometry()));
    }

    void settingsAboveTranslation_data()
    {
        QTest::addColumn<bool>("stayOnTop");
        QTest::newRow("pinned") << true;
        QTest::newRow("normal") << false;
    }

    void settingsAboveTranslation()
    {
        QFETCH(bool, stayOnTop);
        const auto oldFocus = KX11Extras::activeWindow();
        const auto oldCursor = QCursor::pos();
        const auto restoreDesktop = qScopeGuard([&] {
            QCursor::setPos(oldCursor);
            if (KX11Extras::hasWId(oldFocus))
                KX11Extras::forceActiveWindow(oldFocus);
        });
        QTemporaryDir directory;
        auto registry = ProviderRegistry::builtins();
        AppSettings settings(registry, directory.filePath("settings.ini"));
        auto values = settings.snapshot();
        values["stayOnTop"] = stayOnTop;
        QVERIFY(settings.save(values));
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
        desktop.ShowTranslation();
        // Establish the starting window as if the user had selected it.
        KX11Extras::forceActiveWindow(popup->winId());
        QTRY_COMPARE_WITH_TIMEOUT(KX11Extras::activeWindow(), popup->winId(), 2000);
        // Exercise both actual buttons, including reopening after settings was hidden.
        for (const auto &hint : {"打开设置", "设置翻译语言"}) {
            QQuickItem *button = nullptr;
            for (auto *item : popup->findChildren<QQuickItem *>()) {
                if (item->property("hint").toString() == QString::fromUtf8(hint))
                    button = item;
            }
            QVERIFY(button);
            const auto position = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
            QVERIFY(xdotool({"mousemove", "--window", QString::number(popup->winId()), QString::number(position.x()),
                QString::number(position.y()), "click", "1"}));
            QTRY_VERIFY_WITH_TIMEOUT(window->isVisible(), 2000);
            QTRY_COMPARE_WITH_TIMEOUT(KX11Extras::activeWindow(), window->winId(), 2000);
            const auto settingsAbove = [&] {
                const auto order = KX11Extras::stackingOrder();
                return order.contains(window->winId()) && order.contains(popup->winId())
                    && order.indexOf(window->winId()) > order.indexOf(popup->winId());
            };
            QTRY_VERIFY_WITH_TIMEOUT(settingsAbove(), 2000);
            // Even raising the translation must preserve settings' higher stacking order.
            popup->raise();
            QTest::qWait(100);
            QVERIFY(settingsAbove());
            if (QString::fromUtf8(hint) == QStringLiteral("设置翻译语言")) {
                auto *page = window->findChild<QQuickItem *>("translationPage");
                QVERIFY(page && page->isVisible());
                auto *flickable = page->property("contentItem").value<QObject *>();
                QVERIFY(flickable);
                const auto before = flickable->property("contentY").toReal();
                const auto wheelPosition = page->mapToScene(QPointF(page->width() - 6, page->height() / 2)).toPoint();
                // The very first wheel over the settings scrollbar must work without another focus click.
                QVERIFY(xdotool({"mousemove", "--window", QString::number(window->winId()), QString::number(wheelPosition.x()),
                    QString::number(wheelPosition.y()), "click", "5"}));
                QTRY_VERIFY_WITH_TIMEOUT(flickable->property("contentY").toReal() > before + 1, 1000);
            }
            desktop.closeSettings();
            QTRY_VERIFY(!window->isVisible());
            QTRY_COMPARE_WITH_TIMEOUT(KX11Extras::activeWindow(), popup->winId(), 2000);
        }
        desktop.closeTranslation();
        desktop.ShowSettings();
        QTRY_VERIFY(window->isExposed());
        QVERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        auto *prompt = window->findChild<QQuickItem *>("promptField");
        QVERIFY(prompt);
        prompt->setProperty("text", QStringLiteral("Unsaved settings draft"));
        // Starting with settings open must obey the same ordering as opening it from translation.
        desktop.ShowTranslation();
        QTRY_VERIFY(popup->isExposed());
        QTRY_VERIFY(KWindowInfo(popup->winId(), NET::WMState).state().testFlag(NET::SkipTaskbar));
        QTRY_VERIFY(KX11Extras::stackingOrder().indexOf(window->winId())
            > KX11Extras::stackingOrder().indexOf(popup->winId()));
        desktop.closeTranslation();
        QTRY_VERIFY(window->isVisible() && window->isExposed());
        QVERIFY(!window->flags().testFlag(Qt::WindowStaysOnTopHint));
        QCOMPARE(prompt->property("text").toString(), QStringLiteral("Unsaved settings draft"));
    }

    void shortcutRestoresWithoutSelfConflict()
    {
        // Use our own KDE component and unused keys; never edit Trans's real binding.
        const auto originalName = QCoreApplication::applicationName();
        const auto component = QStringLiteral("trans-shortcut-regression-%1").arg(QCoreApplication::applicationPid());
        QCoreApplication::setApplicationName(component);
        const auto restoreName = qScopeGuard([&] { QCoreApplication::setApplicationName(originalName); });
        auto *accelerator = KGlobalAccel::self();
        const QKeySequence owned(QStringLiteral("Ctrl+Alt+Shift+F11"));
        const QKeySequence occupied(QStringLiteral("Ctrl+Alt+Shift+F12"));
        QVERIFY(KGlobalAccel::isGlobalShortcutAvailable(owned));
        QVERIFY(KGlobalAccel::isGlobalShortcutAvailable(occupied));
        const auto cleanup = qScopeGuard([&] {
            QAction action;
            action.setObjectName(QStringLiteral("translate-selection"));
            action.setText(QStringLiteral("Trans temporary shortcut regression test"));
            accelerator->setDefaultShortcut(&action, {});
            accelerator->removeAllShortcuts(&action);
        });
        {
            // This is the binding the daemon remembers after a previous process exits.
            QAction saved;
            saved.setObjectName(QStringLiteral("translate-selection"));
            saved.setText(QStringLiteral("Trans temporary shortcut regression test"));
            QVERIFY(accelerator->setShortcut(&saved, {owned}, KGlobalAccel::NoAutoloading));
            QCOMPARE(accelerator->shortcut(&saved), QList<QKeySequence>{owned});
        }
        const auto update = [this](ShortcutService &service, const QString &sequence, bool accepted = true) {
            auto *job = service.update(ShortcutAction::Selection, sequence, this);
            QSignalSpy succeeded(job, &ShortcutJob::succeeded);
            QSignalSpy failed(job, &ShortcutJob::failed);
            if (!QTest::qWaitFor([&] { return !succeeded.isEmpty() || !failed.isEmpty(); }, 5000))
                return false;
            if (accepted)
                return succeeded.size() == 1 && failed.isEmpty();
            return succeeded.isEmpty() && failed.size() == 1
                && qvariant_cast<PlatformError>(failed.first().first()).code == PlatformErrorCode::Conflict;
        };
        {
            std::unique_ptr<ShortcutService> service(createShortcutService());
            QVERIFY(update(*service, owned.toString(QKeySequence::PortableText)));
            // The availability API reports an active shortcut as occupied even for its owner.
            QVERIFY(!KGlobalAccel::isGlobalShortcutAvailable(owned, component));
            QCOMPARE(service->sequence(ShortcutAction::Selection), owned.toString(QKeySequence::PortableText));
            QVERIFY(update(*service, service->sequence(ShortcutAction::Selection)));

            QAction other;
            other.setProperty("componentName", component + QStringLiteral("-other"));
            other.setObjectName(QStringLiteral("other-action"));
            other.setText(QStringLiteral("Trans temporary conflict regression test"));
            const auto removeOther = qScopeGuard([&] { accelerator->removeAllShortcuts(&other); });
            QVERIFY(accelerator->setShortcut(&other, {occupied}, KGlobalAccel::NoAutoloading));
            QCOMPARE(accelerator->shortcut(&other), QList<QKeySequence>{occupied});
            QVERIFY(update(*service, occupied.toString(QKeySequence::PortableText), false));
            QCOMPARE(service->sequence(ShortcutAction::Selection), owned.toString(QKeySequence::PortableText));
            QCOMPARE(accelerator->shortcut(&other), QList<QKeySequence>{occupied});
            QVERIFY(update(*service, owned.toString(QKeySequence::PortableText)));
        }
        // Recreate the service as on a normal restart; KDE must restore our saved key.
        std::unique_ptr<ShortcutService> restarted(createShortcutService());
        QVERIFY(update(*restarted, owned.toString(QKeySequence::PortableText)));
        QCOMPARE(restarted->sequence(ShortcutAction::Selection), owned.toString(QKeySequence::PortableText));
        QVERIFY(update(*restarted, QString()));
        QVERIFY(restarted->sequence(ShortcutAction::Selection).isEmpty());
        QVERIFY(update(*restarted, QString()));
        QVERIFY(restarted->sequence(ShortcutAction::Selection).isEmpty());
        QVERIFY(KGlobalAccel::isGlobalShortcutAvailable(owned));
    }

    void escapeAfterGlobalShortcut_data()
    {
        QTest::addColumn<bool>("settingsVisible");
        QTest::addColumn<bool>("completed");
        QTest::newRow("translation-loading") << false << false;
        QTest::newRow("translation-text-focus") << false << true;
        QTest::newRow("both-windows-loading") << true << false;
        QTest::newRow("both-windows-text-focus") << true << true;
    }

    void escapeAfterGlobalShortcut()
    {
        QFETCH(bool, settingsVisible);
        QFETCH(bool, completed);
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        const auto oldFocus = KX11Extras::activeWindow();
        const auto originalSelection = qApp->clipboard()->text(QClipboard::Selection);
        const auto restoreDesktop = qScopeGuard([&] {
            qApp->clipboard()->setText(originalSelection, QClipboard::Selection);
            if (KX11Extras::hasWId(oldFocus))
                KX11Extras::forceActiveWindow(oldFocus);
        });

        QTemporaryDir directory;
        ProviderRegistry registry;
        auto provider = std::make_unique<TestProvider>();
        auto *testProvider = provider.get();
        registry.add(std::move(provider));
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
        auto *settingsWindow = popup->findChild<QQuickWindow *>("settingsWindow");
        QVERIFY(settingsWindow);
        desktop.setWindows(popup, settingsWindow);
        if (settingsVisible)
            desktop.ShowSettings();

        QProcess source;
        source.start(QCoreApplication::applicationFilePath(), {"--selection-source"});
        const auto stopSource = qScopeGuard([&] {
            source.terminate();
            if (!source.waitForFinished(2000)) {
                source.kill();
                source.waitForFinished(1000);
            }
        });
        QVERIFY(source.waitForStarted(2000));
        QTRY_VERIFY_WITH_TIMEOUT(source.canReadLine(), 4000);
        bool validId = false;
        const auto sourceId = source.readLine().trimmed().toULongLong(&validId);
        QVERIFY(validId);
        QVERIFY(xdotool({"windowactivate", "--sync", QString::number(sourceId)}));
        QTRY_COMPARE_WITH_TIMEOUT(KX11Extras::activeWindow(), sourceId, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(qApp->clipboard()->text(QClipboard::Selection), QStringLiteral("Hello from an external selection"), 2000);

        QAction action;
        action.setObjectName(QStringLiteral("escape-regression-%1").arg(QCoreApplication::applicationPid()));
        action.setText(QStringLiteral("Trans temporary Escape regression test"));
        auto *accelerator = KGlobalAccel::self();
        const QKeySequence key(QStringLiteral("Ctrl+Alt+Shift+F12"));
        QVERIFY(KGlobalAccel::isGlobalShortcutAvailable(key));
        const auto removeShortcut = qScopeGuard([&] { accelerator->removeAllShortcuts(&action); });
        QVERIFY(accelerator->setShortcut(&action, {key}, KGlobalAccel::NoAutoloading));
        connect(&action, &QAction::triggered, &desktop, &DesktopBridge::TranslateSelection);
        // Registration crosses DBus; give the daemon time to install the native key grab.
        QTest::qWait(250);
        QVERIFY(xdotool({"key", "--clearmodifiers", "ctrl+alt+shift+F12"}));
        QTRY_VERIFY_WITH_TIMEOUT(popup->isVisible(), 4000);
        QCOMPARE(controller.sourceText(), QStringLiteral("Hello from an external selection"));
        // Never call requestActivate() or send a key directly to popup: test the real focus handoff.
        QTRY_COMPARE_WITH_TIMEOUT(KX11Extras::activeWindow(), popup->winId(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(popup->isActive(), 2000);
        if (settingsVisible) {
            QTRY_VERIFY_WITH_TIMEOUT(KX11Extras::stackingOrder().indexOf(settingsWindow->winId())
                > KX11Extras::stackingOrder().indexOf(popup->winId()), 2000);
        }
        if (completed) {
            testProvider->job->finish();
            auto *text = popup->findChild<QQuickItem *>("translationText");
            QVERIFY(text);
            text->forceActiveFocus();
        }
        QVERIFY(xdotool({"key", "Escape"}));
        QTRY_VERIFY_WITH_TIMEOUT(!popup->isVisible(), 2000);
        if (!completed)
            QVERIFY(testProvider->job->cancelled);
        if (settingsVisible) {
            QVERIFY(settingsWindow->isVisible());
            QTRY_COMPARE_WITH_TIMEOUT(KX11Extras::activeWindow(), settingsWindow->winId(), 2000);
            QVERIFY(xdotool({"key", "Escape"}));
            QTRY_VERIFY_WITH_TIMEOUT(!settingsWindow->isVisible(), 2000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(KX11Extras::activeWindow(), sourceId, 2000);
        QVERIFY(xdotool({"key", "Escape"}));
        QTRY_VERIFY_WITH_TIMEOUT(source.canReadLine(), 2000);
        QCOMPARE(source.readLine().trimmed(), QByteArray("source-escape"));
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("trans-x11-regression"));
    QApplication::setQuitOnLastWindowClosed(false);
    if (app.arguments().contains("--selection-source")) {
        SelectionSource source;
        source.setWindowTitle(QStringLiteral("Trans Escape test · selection source"));
        source.resize(440, 80);
        source.setText(QStringLiteral("Hello from an external selection"));
        source.show();
        source.selectAll();
        QTimer::singleShot(100, &source, [&] {
            qApp->clipboard()->setText(source.text(), QClipboard::Selection);
            QTextStream(stdout) << source.winId() << '\n' << Qt::flush;
        });
        return app.exec();
    }
    X11EscapeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_x11_escape.moc"
