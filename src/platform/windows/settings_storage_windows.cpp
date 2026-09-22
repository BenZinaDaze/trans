#include "platform/settings_storage.h"
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QUuid>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <limits>
#include <vector>

namespace Trans {
namespace {
const wchar_t *wide(const QString &value) { return reinterpret_cast<const wchar_t *>(value.utf16()); }
bool failure(QString *error, DWORD code)
{
    *error = QStringLiteral("无法安全保存配置（Windows 错误 %1）。").arg(code);
    return false;
}
class PrivateSecurity {
public:
    ~PrivateSecurity() { if (descriptor) LocalFree(descriptor); }
    bool initialize(QString *error)
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return failure(error, GetLastError());
        const auto close = qScopeGuard([&] { CloseHandle(token); });
        DWORD length = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &length);
        std::vector<BYTE> storage(length);
        if (!GetTokenInformation(token, TokenUser, storage.data(), length, &length)) return failure(error, GetLastError());
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(storage.data())->User.Sid, &sid))
            return failure(error, GetLastError());
        const auto freeSid = qScopeGuard([&] { LocalFree(sid); });
        // Protected, inheritable ACL: only this user and LocalSystem can read credentials.
        const QString sddl = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%1)").arg(QString::fromWCharArray(sid));
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(wide(sddl), SDDL_REVISION_1, &descriptor, nullptr))
            return failure(error, GetLastError());
        attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        return true;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
};
}

bool prepareSettingsDirectory(const QString &directory, QString *error)
{
    if (!QDir().mkpath(directory)) return failure(error, ERROR_PATH_NOT_FOUND);
    const DWORD attributes = GetFileAttributesW(wide(directory));
    if (attributes == INVALID_FILE_ATTRIBUTES) return failure(error, GetLastError());
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return failure(error, ERROR_ACCESS_DENIED);
    PrivateSecurity security;
    if (!security.initialize(error)) return false;
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(security.descriptor, &present, &acl, &defaulted) || !present)
        return failure(error, ERROR_INVALID_SECURITY_DESCR);
    const DWORD result = SetNamedSecurityInfoW(const_cast<wchar_t *>(wide(directory)), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    return result == ERROR_SUCCESS || failure(error, result);
}

bool writePrivateSettings(const QString &path, const QByteArray &contents, QString *error)
{
    PrivateSecurity security;
    if (!security.initialize(error)) return false;
    const QString temporary = QFileInfo(path).absolutePath() + QStringLiteral("/.settings-%1.tmp")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    HANDLE file = CreateFileW(wide(temporary), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return failure(error, GetLastError());
    const auto cleanup = qScopeGuard([&] {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(wide(temporary));
    });
    qsizetype offset = 0;
    while (offset < contents.size()) {
        const DWORD count = DWORD(std::min<qsizetype>(contents.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, contents.constData() + offset, count, &written, nullptr)) return failure(error, GetLastError());
        if (!written) return failure(error, ERROR_WRITE_FAULT);
        offset += written;
    }
    if (!FlushFileBuffers(file)) return failure(error, GetLastError());
    if (!CloseHandle(file)) { file = INVALID_HANDLE_VALUE; return failure(error, GetLastError()); }
    file = INVALID_HANDLE_VALUE;
    // Same-directory rename replaces the old file with the new protected ACL.
    if (!MoveFileExW(wide(temporary), wide(path), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return failure(error, GetLastError());
    return true;
}
}
