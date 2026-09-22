#pragma once

#include "platform/instance_channel.h"

class QDBusConnection;

namespace Trans {

std::unique_ptr<InstanceChannel> createLinuxInstanceChannel(const QDBusConnection &connection,
                                                          QObject *parent = nullptr);

} // namespace Trans
