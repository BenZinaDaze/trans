#pragma once

#include <QByteArray>
#include <QString>

namespace Trans {
// Native storage policy: protect credentials before writing any contents.
bool prepareSettingsDirectory(const QString &directory, QString *error);
bool writePrivateSettings(const QString &path, const QByteArray &contents, QString *error);
}
