#include "platform/settings_storage.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace Trans {
namespace {
constexpr auto PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
}
bool prepareSettingsDirectory(const QString &directory, QString *error)
{
    if (QDir().mkpath(directory) && QFile::setPermissions(directory, PrivateFile | QFileDevice::ExeOwner))
        return true;
    *error = QStringLiteral("无法创建配置目录或设置访问权限。");
    return false;
}
bool writePrivateSettings(const QString &path, const QByteArray &contents, QString *error)
{
    QSaveFile destination(path);
    if (!destination.open(QIODevice::WriteOnly) || !destination.setPermissions(PrivateFile)) {
        *error = QStringLiteral("无法保存配置，请检查目录权限。");
        return false;
    }
    if (destination.write(contents) != contents.size() || !destination.commit()) {
        *error = QStringLiteral("配置保存失败，请检查磁盘空间。");
        return false;
    }
    return true;
}
}
