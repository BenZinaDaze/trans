#pragma once

#include "platform/instance_channel.h"

namespace Trans {

// A namespace is mixed into the Windows user/logon identity, not used as a raw
// pipe path. An empty namespace selects the application's production endpoint.
std::unique_ptr<InstanceChannel> createWindowsInstanceChannel(const QString &instanceNamespace,
                                                            QObject *parent = nullptr);

// Private transport-test seam; empty on Windows token/SID lookup failure.
QString windowsInstanceEndpointName(const QString &instanceNamespace);

} // namespace Trans
