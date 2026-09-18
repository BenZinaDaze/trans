#include "controller.h"
#include "desktop.h"
#include "settings.h"
#include "provider_tools.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("trans"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TRANS_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Trans"));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.trans.Trans"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/trans.svg")));
    QApplication::setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("KDE X11 选区与截图翻译工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"ocr", "Capture a screenshot, recognize and translate text."});
    parser.addOption({"translate", "Translate the current X11 selection."});
    parser.addOption({"settings", "Open settings (also in an existing instance)."});
    parser.addOption({"smoke-test", "Load both windows without desktop registration or network requests."});
    parser.addOption({"screenshot", "Save the settings window during --smoke-test.", "path"});
    parser.process(app);

    const bool smoke = parser.isSet("smoke-test");
    const QString service = QStringLiteral("io.github.trans.Trans");
    auto bus = QDBusConnection::sessionBus();
    if (!smoke) {
        if (!bus.isConnected()) {
            qCritical("Cannot connect to the session D-Bus. Run Trans inside a KDE desktop session.");
            return 1;
        }
    }

    QTemporaryDir smokeDirectory;
    const QString configDirectory = smoke ? smokeDirectory.path()
        : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDirectory.isEmpty()) {
        qCritical("Cannot determine the configuration directory.");
        return 1;
    }
    auto registry = Trans::ProviderRegistry::builtins();
    Trans::AppSettings settings(registry, QDir(configDirectory).filePath(QStringLiteral("settings.ini")));
    Trans::TranslationController controller(registry, settings);
    Trans::DesktopBridge desktop(controller, settings);
    Trans::ProviderTools providerTools(registry);
    QQmlApplicationEngine engine;
    bool qmlWarnings = false;
    QObject::connect(&engine, &QQmlEngine::warnings, &app, [&qmlWarnings] { qmlWarnings = true; });
    engine.setInitialProperties({{"controller", QVariant::fromValue(&controller)},
                                 {"appSettings", QVariant::fromValue(&settings)},
                                 {"desktop", QVariant::fromValue(&desktop)},
                                 {"providerTools", QVariant::fromValue(&providerTools)}});
    engine.loadFromModule(QStringLiteral("Trans"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *popup = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    auto *settingsWindow = popup ? popup->findChild<QQuickWindow *>(QStringLiteral("settingsWindow")) : nullptr;
    if (!popup || !settingsWindow) {
        qCritical("The translation or settings window could not be loaded.");
        return 1;
    }
    desktop.setWindows(popup, settingsWindow);

    if (smoke) {
        controller.selectionError(QStringLiteral("请先在其他应用中选中文字，再按翻译快捷键。"));
        popup->show();
        desktop.ShowSettings();
        QTimer::singleShot(400, &app, [&] {
            bool valid = popup->isVisible() && settingsWindow->isVisible() && !qmlWarnings;
            if (parser.isSet("screenshot"))
                valid &= settingsWindow->grabWindow().save(parser.value("screenshot"));
            app.exit(valid ? 0 : 1);
        });
    } else {
        if (!bus.registerObject(QStringLiteral("/Trans"), &desktop, QDBusConnection::ExportScriptableInvokables)) {
            qCritical("Cannot register the Trans D-Bus interface.");
            return 1;
        }
        // Publish the service only after the object and both windows can handle commands.
        if (!bus.registerService(service)) {
            QDBusInterface existing(service, QStringLiteral("/Trans"), service, bus);
            const auto reply = existing.call(parser.isSet("ocr") ? "TranslateScreenshot" : parser.isSet("translate") ? "TranslateSelection" : "ShowSettings");
            if (reply.type() == QDBusMessage::ErrorMessage) {
                qCritical("Cannot contact the running Trans instance.");
                return 1;
            }
            return 0;
        }
        desktop.initialize();
        if (parser.isSet("ocr"))
            QTimer::singleShot(0, &desktop, &Trans::DesktopBridge::TranslateScreenshot);
        else if (parser.isSet("translate"))
            QTimer::singleShot(0, &desktop, &Trans::DesktopBridge::TranslateSelection);
        else if (parser.isSet("settings") || !settings.isConfigured() || !desktop.shortcutError().isEmpty())
            QTimer::singleShot(0, &desktop, &Trans::DesktopBridge::ShowSettings);
    }
    return app.exec();
}
