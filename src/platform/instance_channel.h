#pragma once

#include "platform_error.h"

#include <QObject>
#include <functional>
#include <memory>

namespace Trans {

enum class AppCommand {
    ShowTranslation,
    TranslateSelection,
    TranslateScreenshot,
    ShowSettings
};

enum class InstanceRole { Primary, Forwarded, Error };

struct InstanceStartResult {
    InstanceRole role = InstanceRole::Error;
    PlatformError error;
};

class InstanceChannel : public QObject {
public:
    using CommandHandler = std::function<void(AppCommand)>;
    using QObject::QObject;
    ~InstanceChannel() override = default;

    // A primary reserves the endpoint without running the command. A secondary
    // returns Forwarded only after the primary has dispatched its command.
    virtual InstanceStartResult start(AppCommand command) = 0;

    // Install only after the application can handle all commands. Calls received
    // during startup wait for this handler; failed startup never acknowledges them.
    virtual void setReady(CommandHandler handler) = 0;
};

std::unique_ptr<InstanceChannel> createInstanceChannel(QObject *parent = nullptr);

} // namespace Trans
