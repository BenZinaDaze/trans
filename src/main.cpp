#include "controller.h"
#include "desktop.h"
#include "settings.h"
#include "provider_tools.h"
#include "platform/instance_channel.h"
#include "platform/platform_services.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
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
    parser.setApplicationDescription(QStringLiteral("Desktop selection and screenshot translation tool"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"ocr", "Capture a screenshot, recognize and translate text."});
    parser.addOption({"translate", "Translate the current text selection."});
    parser.addOption({"settings", "Open settings (also in an existing instance)."});
    parser.addOption({"smoke-test", "Load both windows without desktop registration or network requests."});
    parser.addOption({"screenshot", "Save the settings window during --smoke-test.", "path"});
    parser.process(app);

    const bool smoke = parser.isSet("smoke-test");
    const auto command = parser.isSet("ocr") ? Trans::AppCommand::TranslateScreenshot
        : parser.isSet("translate") ? Trans::AppCommand::TranslateSelection : Trans::AppCommand::ShowSettings;
    std::unique_ptr<Trans::InstanceChannel> instance;
    if (!smoke) {
        instance = Trans::createInstanceChannel();
        const auto result = instance->start(command);
        if (result.role == Trans::InstanceRole::Forwarded)
            return 0;
        if (result.role == Trans::InstanceRole::Error) {
            qCritical().noquote() << result.error.message;
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
    auto platform = Trans::createPlatformServices();
    Trans::TranslationController controller(registry, settings);
    Trans::DesktopBridge desktop(controller, settings, *platform);
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
        desktop.initialize();
        instance->setReady([&desktop](Trans::AppCommand requested) { desktop.dispatchCommand(requested); });
        if (parser.isSet("ocr") || parser.isSet("translate") || parser.isSet("settings")
            || !settings.isConfigured() || !desktop.shortcutError().isEmpty())
            QTimer::singleShot(0, &desktop, [&desktop, command] { desktop.dispatchCommand(command); });
    }
    return app.exec();
}
