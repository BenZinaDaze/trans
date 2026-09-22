#pragma once

#include <QMetaType>
#include <QString>

namespace Trans {

enum class PlatformErrorCode { Unsupported, Unavailable, PermissionDenied, NoSelection, Conflict, Timeout, Cancelled, Failed };

struct PlatformError {
    PlatformErrorCode code = PlatformErrorCode::Failed;
    QString message;
};

enum class CapabilityState { Available, Unavailable, Unsupported, PermissionDenied };

} // namespace Trans

Q_DECLARE_METATYPE(Trans::PlatformError)
Q_DECLARE_METATYPE(Trans::CapabilityState)
