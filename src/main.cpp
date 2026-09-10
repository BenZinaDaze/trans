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
    QCoreApplication::setApplicationName(QStringLiteral("tran"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TRAN_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Tran"));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.tran.Tran"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/tran.svg")));
    QApplication::setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("KDE X11 选区翻译工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"translate", "Translate the current X11 selection."});
    parser.addOption({"settings", "Open settings (also in an existing instance)."});
    parser.addOption({"smoke-test", "Load both windows without desktop registration or network requests."});
    parser.addOption({"screenshot", "Save the settings window during --smoke-test.", "path"});
    parser.process(app);

    const bool smoke = parser.isSet("smoke-test");
    const QString service = QStringLiteral("io.github.tran.Tran");
    auto bus = QDBusConnection::sessionBus();
    if (!smoke) {
        if (!bus.isConnected()) {
            qCritical("Cannot connect to the session D-Bus. Run Tran inside a KDE desktop session.");
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
    auto registry = Tran::ProviderRegistry::builtins();
    Tran::AppSettings settings(registry, QDir(configDirectory).filePath(QStringLiteral("settings.ini")));
    Tran::TranslationController controller(registry, settings);
    Tran::DesktopBridge desktop(controller, settings);
    Tran::ProviderTools providerTools(registry);
    QQmlApplicationEngine engine;
    bool qmlWarnings = false;
    QObject::connect(&engine, &QQmlEngine::warnings, &app, [&qmlWarnings] { qmlWarnings = true; });
    engine.setInitialProperties({{"controller", QVariant::fromValue(&controller)},
                                 {"appSettings", QVariant::fromValue(&settings)},
                                 {"desktop", QVariant::fromValue(&desktop)},
                                 {"providerTools", QVariant::fromValue(&providerTools)}});
    engine.loadFromModule(QStringLiteral("Tran"), QStringLiteral("Main"));
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
        if (!bus.registerObject(QStringLiteral("/Tran"), &desktop, QDBusConnection::ExportScriptableInvokables)) {
            qCritical("Cannot register the Tran D-Bus interface.");
            return 1;
        }
        // Publish the service only after the object and both windows can handle commands.
        if (!bus.registerService(service)) {
            QDBusInterface existing(service, QStringLiteral("/Tran"), service, bus);
            const auto reply = existing.call(parser.isSet("translate") ? "TranslateSelection" : "ShowSettings");
            if (reply.type() == QDBusMessage::ErrorMessage) {
                qCritical("Cannot contact the running Tran instance.");
                return 1;
            }
            return 0;
        }
        desktop.initialize();
        if (parser.isSet("translate"))
            QTimer::singleShot(0, &desktop, &Tran::DesktopBridge::TranslateSelection);
        else if (parser.isSet("settings") || !settings.isConfigured() || !desktop.shortcutError().isEmpty())
            QTimer::singleShot(0, &desktop, &Tran::DesktopBridge::ShowSettings);
    }
    return app.exec();
}
