#include "platform.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <aclapi.h>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <objbase.h>
#include <sddl.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace {
class Handle {
  public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_;
};
struct LocalAllocation {
    HLOCAL value = nullptr;
    ~LocalAllocation() {
        if (value)
            LocalFree(value);
    }
};
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string utf8(const std::filesystem::path &path) {
    const auto &wide = path.native();
    int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    require(size > 0, "Cannot encode temporary file path");
    std::string result(size, '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), int(wide.size()), result.data(), size,
                                nullptr, nullptr) == size,
            "Cannot encode temporary file path");
    return result;
}
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::wstring temporary(32768, L'\0');
        DWORD count = GetTempPathW(DWORD(temporary.size()), temporary.data());
        require(count && count < temporary.size(), "Cannot locate Windows temporary directory");
        temporary.resize(count);
        // Resolve the system's temporary-directory alias once; the production
        // private writer correctly refuses reparse points in its target path.
        Handle directory(CreateFileW(temporary.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        require(bool(directory), "Cannot open Windows temporary directory");
        std::wstring resolved(32768, L'\0');
        count = GetFinalPathNameByHandleW(directory.get(), resolved.data(), DWORD(resolved.size()),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        require(count && count < resolved.size(), "Cannot resolve Windows temporary directory");
        resolved.resize(count);
        // Use the ordinary DOS spelling returned by KnownFolder paths in the
        // application, while retaining the resolved (not junction) location.
        if (resolved.starts_with(L"\\\\?\\UNC\\"))
            resolved = L"\\\\" + resolved.substr(8);
        else if (resolved.starts_with(L"\\\\?\\"))
            resolved.erase(0, 4);
        GUID id{};
        require(SUCCEEDED(CoCreateGuid(&id)), "Cannot generate isolated test directory name");
        wchar_t name[40]{};
        require(StringFromGUID2(id, name, int(std::size(name))) > 0, "Cannot encode test directory name");
        path_ = std::filesystem::path(resolved) / (std::wstring(L"trans-storage-test-") + name);
        require(CreateDirectoryW(path_.c_str(), nullptr) != FALSE, "Cannot create isolated test directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};
std::vector<BYTE> currentUser() {
    HANDLE raw = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "Cannot inspect current user token");
    Handle token(raw);
    DWORD length = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    require(length != 0, "Cannot measure current user token");
    std::vector<BYTE> information(length);
    require(GetTokenInformation(token.get(), TokenUser, information.data(), length, &length) != FALSE,
            "Cannot read current user token");
    PSID sid = reinterpret_cast<TOKEN_USER *>(information.data())->User.Sid;
    require(IsValidSid(sid) != FALSE, "Current user SID is invalid");
    const auto *bytes = static_cast<const BYTE *>(sid);
    return {bytes, bytes + GetLengthSid(sid)};
}
void verifyPrivate(const std::filesystem::path &path, const std::vector<BYTE> &user) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    auto writablePath = path.native();
    DWORD result = GetNamedSecurityInfoW(writablePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, &acl, nullptr, &descriptor);
    LocalAllocation allocation{descriptor};
    require(result == ERROR_SUCCESS, "Cannot inspect configuration access policy");
    require(acl && IsValidAcl(acl), "Configuration has no valid restrictive ACL");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE,
            "Cannot inspect ACL inheritance policy");
    require((control & SE_DACL_PROTECTED) != 0, "Configuration ACL permits inherited access");
    bool userAllowed = false, systemAllowed = false;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void *entry = nullptr;
        require(GetAce(acl, index, &entry) != FALSE, "Cannot inspect configuration ACL entry");
        const auto *header = static_cast<ACE_HEADER *>(entry);
        require(header->AceType == ACCESS_ALLOWED_ACE_TYPE, "Configuration ACL contains an unexpected entry type");
        auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
        PSID sid = &ace->SidStart;
        const bool current = EqualSid(sid, const_cast<BYTE *>(user.data())) != FALSE;
        const bool system = IsWellKnownSid(sid, WinLocalSystemSid) != FALSE;
        require(current || system, "Configuration is accessible to an identity other than the current user or System");
        if (!(header->AceFlags & INHERIT_ONLY_ACE)) {
            require((ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS,
                    "Configuration owner or System cannot maintain the file");
            userAllowed = userAllowed || current;
            systemAllowed = systemAllowed || system;
        }
    }
    require(userAllowed && systemAllowed, "Configuration lacks effective user/System access");
}
void makeReadableByEveryone(const std::filesystem::path &path, const std::vector<BYTE> &user) {
    LPWSTR sid = nullptr;
    require(ConvertSidToStringSidW(const_cast<BYTE *>(user.data()), &sid) != FALSE, "Cannot encode current user SID");
    LocalAllocation sidAllocation{sid};
    const std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(sid) + L")(A;;FR;;;WD)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) !=
                FALSE,
            "Cannot create pre-existing permissive test ACL");
    LocalAllocation descriptorAllocation{descriptor};
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    require(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present,
            "Cannot obtain permissive test ACL");
    auto writablePath = path.native();
    require(SetNamedSecurityInfoW(writablePath.data(), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                  acl, nullptr) == ERROR_SUCCESS,
            "Cannot install pre-existing permissive test ACL");
}
std::string readAll(HANDLE file) {
    LARGE_INTEGER beginning{};
    require(SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) != FALSE, "Cannot rewind test file");
    std::string text;
    char buffer[256];
    for (;;) {
        DWORD count = 0;
        require(ReadFile(file, buffer, sizeof(buffer), &count, nullptr) != FALSE,
                "Cannot read persisted configuration");
        if (!count)
            return text;
        require(text.size() + count <= 4096, "Unexpected configuration file size");
        text.append(buffer, count);
    }
}
std::string readPath(const std::filesystem::path &path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(bool(file), "Cannot reopen persisted configuration");
    return readAll(file.get());
}
void verifyNoTemporaryCredentials(const std::filesystem::path &directory, const std::filesystem::path &expected) {
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        require(entry.path() == expected, "A temporary file containing credentials remained after the write");
}
void privateReplacementAndFailedWritePreservesOldFile() {
    TemporaryDirectory temporary;
    auto platform = Trans::Native::createPlatform();
    const auto user = currentUser();
    const auto directory = temporary.path() / L"config-\u914d\u7f6e";
    const auto path = directory / L"settings.json";
    const auto target = utf8(path);
    const std::string first = "{\"secret\":\"first\"}";
    const std::string second = "{\"secret\":\"second\"}";
    auto error = platform->writePrivateFile(target, first);
    require(!error, "First private write failed: " + error.message);
    require(readPath(path) == first, "First private write did not persist its contents");
    verifyPrivate(directory, user);
    verifyPrivate(path, user);
    // A replacement must repair an old permissive ACL, not inherit or retain
    // it from the old inode/file. An already-open reader retains its old snapshot.
    makeReadableByEveryone(path, user);
    makeReadableByEveryone(directory, user);
    {
        Handle snapshot(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        require(bool(snapshot), "Cannot hold a reader across atomic replacement");
        error = platform->writePrivateFile(target, second);
        require(!error, "Private replacement failed: " + error.message);
        require(readAll(snapshot.get()) == first, "Replacement modified data visible to an existing reader");
        require(readPath(path) == second, "New readers do not observe the complete replacement");
    }
    verifyPrivate(directory, user);
    verifyPrivate(path, user);
    verifyNoTemporaryCredentials(directory, path);
    {
        // Deny DELETE sharing so the final atomic rename fails after its private
        // temporary file was written. The old file and its ACL must survive.
        Handle locked(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
        require(bool(locked), "Cannot lock the old file against replacement");
        error = platform->writePrivateFile(target, "{\"secret\":\"lost\"}");
        require(bool(error), "A blocked replacement incorrectly reported success");
        require(readAll(locked.get()) == second, "Failed replacement damaged the previously committed contents");
        verifyPrivate(path, user);
        verifyNoTemporaryCredentials(directory, path);
    }
    require(readPath(path) == second, "Old configuration was not preserved after the failed write");
}
} // namespace

int main() {
    try {
        privateReplacementAndFailedWritePreservesOldFile();
        std::cout << "PASS: private ACLs, atomic reader snapshots, and failed-write preservation\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
