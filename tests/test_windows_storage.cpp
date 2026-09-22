#include "platform/settings_storage.h"
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <windows.h>
#include <aclapi.h>
#include <vector>

using namespace Trans;

class WindowsStorageTest : public QObject {
    Q_OBJECT
    static void verifyPrivate(const QString &path)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL acl = nullptr;
        const auto result = GetNamedSecurityInfoW(reinterpret_cast<LPWSTR>(const_cast<ushort *>(path.utf16())),
            SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor);
        QCOMPARE(result, DWORD(ERROR_SUCCESS));
        const auto free = qScopeGuard([&] { LocalFree(descriptor); });
        QVERIFY(acl);
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &revision));
        QVERIFY(control & SE_DACL_PROTECTED);
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
        const auto close = qScopeGuard([&] { CloseHandle(token); });
        DWORD length = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &length);
        std::vector<BYTE> buffer(length);
        QVERIFY(GetTokenInformation(token, TokenUser, buffer.data(), length, &length));
        const auto user = reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid;
        bool userAllowed = false;
        for (DWORD i = 0; i < acl->AceCount; ++i) {
            void *entry = nullptr;
            QVERIFY(GetAce(acl, i, &entry));
            const auto *header = static_cast<ACE_HEADER *>(entry);
            QCOMPARE(header->AceType, BYTE(ACCESS_ALLOWED_ACE_TYPE));
            auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
            PSID sid = &ace->SidStart;
            const bool current = EqualSid(sid, user);
            QVERIFY(current || IsWellKnownSid(sid, WinLocalSystemSid));
            if (current && !(header->AceFlags & INHERIT_ONLY_ACE)) {
                QVERIFY((ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS);
                userAllowed = true;
            }
        }
        QVERIFY(userAllowed);
    }
private slots:
    void privateReplacementAndFailedWritePreservesOldFile()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto directory = root.filePath("config");
        const auto path = directory + "/settings.ini";
        QString error;
        QVERIFY2(prepareSettingsDirectory(directory, &error), qPrintable(error));
        verifyPrivate(directory);
        QVERIFY2(writePrivateSettings(path, "secret=first", &error), qPrintable(error));
        verifyPrivate(path);
        QVERIFY2(writePrivateSettings(path, "secret=second", &error), qPrintable(error));
        verifyPrivate(path);
        HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        const auto close = qScopeGuard([&] { CloseHandle(locked); });
        QVERIFY(!writePrivateSettings(path, "secret=lost", &error));
        char contents[64]{};
        DWORD size = 0;
        QVERIFY(ReadFile(locked, contents, sizeof(contents), &size, nullptr));
        QCOMPARE(QByteArray(contents, size), QByteArray("secret=second"));
        verifyPrivate(path);
    }
};

QTEST_GUILESS_MAIN(WindowsStorageTest)
#include "test_windows_storage.moc"
