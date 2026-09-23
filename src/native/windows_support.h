#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <climits>
#include <cstdint>
#include <functional>
#include <limits>
#include <sddl.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

namespace Trans::Native::Windows {
class Handle {
  public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_(other.release()) {}
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) {
        if (*this)
            CloseHandle(value_);
        value_ = value;
    }

  private:
    HANDLE value_;
};
struct LocalAllocation {
    HLOCAL value = nullptr;
    ~LocalAllocation() {
        if (value)
            LocalFree(value);
    }
    LocalAllocation() = default;
    LocalAllocation(const LocalAllocation &) = delete;
    LocalAllocation &operator=(const LocalAllocation &) = delete;
};
inline bool toWide(std::string_view text, std::wstring &output) {
    output.clear();
    if (text.empty())
        return true;
    if (text.size() > INT_MAX || text.find('\0') != text.npos)
        return false;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!count)
        return false;
    output.resize(count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), output.data(), count) ==
           count;
}
inline bool toUtf8(std::wstring_view text, std::string &output) {
    output.clear();
    if (text.empty())
        return true;
    if (text.size() > INT_MAX || text.find(L'\0') != text.npos)
        return false;
    int count =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count)
        return false;
    output.resize(count);
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), output.data(), count,
                               nullptr, nullptr) == count;
}
inline bool tokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<BYTE> &buffer) {
    DWORD count = 0;
    if (GetTokenInformation(token, kind, nullptr, 0, &count) || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !count ||
        count > 1024 * 1024)
        return false;
    buffer.resize(count);
    return GetTokenInformation(token, kind, buffer.data(), count, &count) != FALSE;
}
struct Identity {
    std::vector<BYTE> user, logon;
    DWORD session = 0;
    LUID authentication{};
};
inline bool readIdentity(HANDLE process, Identity &identity) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    std::vector<BYTE> user, groups;
    if (!tokenInformation(token.get(), TokenUser, user) || !tokenInformation(token.get(), TokenGroups, groups))
        return false;
    PSID sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
    if (!IsValidSid(sid))
        return false;
    auto *bytes = static_cast<BYTE *>(sid);
    identity.user.assign(bytes, bytes + GetLengthSid(sid));
    identity.logon.clear();
    const auto *group = reinterpret_cast<TOKEN_GROUPS *>(groups.data());
    for (DWORD i = 0; i < group->GroupCount; ++i) {
        if ((group->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID)
            continue;
        sid = group->Groups[i].Sid;
        if (!IsValidSid(sid))
            return false;
        bytes = static_cast<BYTE *>(sid);
        identity.logon.assign(bytes, bytes + GetLengthSid(sid));
        break;
    }
    if (identity.logon.empty()) {
        SetLastError(ERROR_NO_SUCH_LOGON_SESSION);
        return false;
    }
    TOKEN_STATISTICS statistics{};
    DWORD count = 0;
    if (!GetTokenInformation(token.get(), TokenSessionId, &identity.session, sizeof(identity.session), &count) ||
        !GetTokenInformation(token.get(), TokenStatistics, &statistics, sizeof(statistics), &count))
        return false;
    identity.authentication = statistics.AuthenticationId;
    return true;
}
inline bool sameIdentity(const Identity &a, const Identity &b) {
    return a.user == b.user && a.logon == b.logon && a.session == b.session &&
           a.authentication.HighPart == b.authentication.HighPart &&
           a.authentication.LowPart == b.authentication.LowPart;
}
inline bool integrityLevel(HANDLE process, DWORD &level) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    std::vector<BYTE> buffer;
    if (!tokenInformation(token.get(), TokenIntegrityLevel, buffer))
        return false;
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer.data())->Label.Sid;
    if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid))
        return false;
    level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    return true;
}
inline std::wstring sidString(const std::vector<BYTE> &sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(const_cast<BYTE *>(sid.data()), &text))
        return {};
    LocalAllocation allocation;
    allocation.value = text;
    return text;
}
class Security {
  public:
    bool initialize(const Identity &identity, bool file = false) {
        const auto user = sidString(identity.user);
        const auto logon = sidString(identity.logon);
        if (user.empty() || logon.empty())
            return false;
        const auto sddl = file ? L"O:" + user + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + user + L")"
                               : L"O:" + user + L"D:P(A;;GA;;;" + logon + L")";
        PSECURITY_DESCRIPTOR value = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &value, nullptr))
            return false;
        descriptor_.value = value;
        attributes_ = {sizeof(SECURITY_ATTRIBUTES), value, FALSE};
        return true;
    }
    SECURITY_ATTRIBUTES *attributes() { return &attributes_; }
    PSECURITY_DESCRIPTOR descriptor() const { return descriptor_.value; }

  private:
    LocalAllocation descriptor_;
    SECURITY_ATTRIBUTES attributes_{};
};
inline bool verifyObjectSecurity(HANDLE object, const Identity &identity) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD error = GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  &owner, nullptr, &dacl, nullptr, &descriptor);
    LocalAllocation allocation;
    allocation.value = descriptor;
    if (error != ERROR_SUCCESS) {
        SetLastError(error);
        return false;
    }
    void *rawAce = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!owner || !EqualSid(owner, const_cast<BYTE *>(identity.user.data())) || !dacl || !IsValidAcl(dacl) ||
        dacl->AceCount != 1 || !GetAce(dacl, 0, &rawAce) ||
        !GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
        !EqualSid(const_cast<DWORD *>(&ace->SidStart), const_cast<BYTE *>(identity.logon.data()))) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    return true;
}
inline bool verifyPeer(HANDLE pipe, bool server, const Identity &identity, ULONG *peerId = nullptr) {
    ULONG id = 0;
    if (!(server ? GetNamedPipeClientProcessId(pipe, &id) : GetNamedPipeServerProcessId(pipe, &id)) || !id)
        return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id));
    Identity peer;
    if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT || !readIdentity(process.get(), peer))
        return false;
    if (!sameIdentity(identity, peer)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    if (peerId)
        *peerId = id;
    return true;
}
inline bool randomBytes(std::span<std::uint8_t> bytes) {
    return BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}
inline std::wstring randomName() {
    std::array<std::uint8_t, 16> value{};
    if (!randomBytes(value))
        return {};
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(32);
    for (auto byte : value) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
// Every pending I/O is cancelled and drained before its OVERLAPPED storage dies.
inline DWORD pipeOperation(HANDLE pipe, void *data, DWORD size, DWORD &transferred, bool writing, HANDLE stop,
                           ULONGLONG deadline, const std::function<bool()> &context = {}) {
    transferred = 0;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return GetLastError();
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    BOOL ready = writing ? WriteFile(pipe, data, size, &transferred, &operation)
                         : ReadFile(pipe, data, size, &transferred, &operation);
    if (ready)
        return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING)
        return error;
    for (;;) {
        if (context && !context()) {
            error = ERROR_CANCELLED;
            break;
        }
        auto now = GetTickCount64();
        if (now >= deadline) {
            error = ERROR_TIMEOUT;
            break;
        }
        DWORD timeout = DWORD(std::min<ULONGLONG>(deadline - now, context ? 40 : MAXDWORD - 1));
        HANDLE events[]{event.get(), stop};
        DWORD wait = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeout);
        if (wait == WAIT_OBJECT_0)
            return GetOverlappedResult(pipe, &operation, &transferred, FALSE) ? ERROR_SUCCESS : GetLastError();
        if (stop && wait == WAIT_OBJECT_0 + 1) {
            error = ERROR_CANCELLED;
            break;
        }
        if (wait == WAIT_FAILED) {
            error = GetLastError();
            break;
        }
    }
    CancelIoEx(pipe, &operation);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &operation, &ignored, TRUE);
    return error;
}
inline DWORD transfer(HANDLE pipe, void *data, DWORD size, bool writing, HANDLE stop, ULONGLONG deadline,
                      const std::function<bool()> &context = {}) {
    DWORD offset = 0;
    while (offset < size) {
        if ((stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) || (context && !context()))
            return ERROR_CANCELLED;
        if (GetTickCount64() >= deadline)
            return ERROR_TIMEOUT;
        DWORD count = 0;
        DWORD error = pipeOperation(pipe, static_cast<BYTE *>(data) + offset, size - offset, count, writing, stop,
                                    deadline, context);
        if (error != ERROR_SUCCESS)
            return error;
        if (!count)
            return ERROR_BROKEN_PIPE;
        offset += count;
    }
    return ERROR_SUCCESS;
}
inline void put32(std::uint8_t *out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out[i] = std::uint8_t(value >> (i * 8));
}
inline std::uint32_t get32(const std::uint8_t *in) {
    return std::uint32_t(in[0]) | std::uint32_t(in[1]) << 8 | std::uint32_t(in[2]) << 16 | std::uint32_t(in[3]) << 24;
}
inline bool whitespace(wchar_t c) {
    return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
inline bool validText(std::wstring_view text) {
    bool visible = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = std::uint16_t(text[i]);
        if (!c)
            return false;
        if (!whitespace(c))
            visible = true;
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                return false;
        } else if (c >= 0xdc00 && c <= 0xdfff)
            return false;
    }
    return visible;
}
namespace SelectionProtocol {
inline constexpr DWORD TimeoutMs = 3500;
inline constexpr size_t MaxResponseBytes = 512 * 1024, MaxStderrBytes = 4096, MaxTextChars = 64 * 1024;
inline constexpr int MaxRanges = 128;
inline constexpr std::uint32_t Magic = 0x534e5254, Version = 2;
enum class Status : std::uint32_t { Success, NoSelection, Unsupported, PermissionDenied, Cancelled, Failed };
struct Request {
    std::array<std::uint8_t, 16> id{};
    std::uint64_t window = 0;
    DWORD process = 0, thread = 0, parent = 0;
};
using RequestBytes = std::array<std::uint8_t, 48>;
inline RequestBytes encode(const Request &request) {
    RequestBytes bytes{};
    put32(bytes.data(), Magic);
    put32(bytes.data() + 4, Version);
    std::copy(request.id.begin(), request.id.end(), bytes.begin() + 8);
    put32(bytes.data() + 24, std::uint32_t(request.window));
    put32(bytes.data() + 28, std::uint32_t(request.window >> 32));
    put32(bytes.data() + 32, request.process);
    put32(bytes.data() + 36, request.thread);
    put32(bytes.data() + 40, request.parent);
    return bytes;
}
inline bool decode(const RequestBytes &bytes, Request &request) {
    if (get32(bytes.data()) != Magic || get32(bytes.data() + 4) != Version || get32(bytes.data() + 44))
        return false;
    std::copy_n(bytes.begin() + 8, 16, request.id.begin());
    request.window = get32(bytes.data() + 24) | std::uint64_t(get32(bytes.data() + 28)) << 32;
    request.process = get32(bytes.data() + 32);
    request.thread = get32(bytes.data() + 36);
    request.parent = get32(bytes.data() + 40);
    return request.window && request.window <= UINTPTR_MAX && request.process && request.thread && request.parent;
}
inline std::vector<std::uint8_t> response(const Request &request, Status status, std::wstring_view text) {
    if (status != Status::Success)
        text = {};
    std::vector<std::uint8_t> bytes(32 + text.size() * 2);
    put32(bytes.data(), Magic);
    put32(bytes.data() + 4, Version);
    std::copy(request.id.begin(), request.id.end(), bytes.begin() + 8);
    put32(bytes.data() + 24, std::uint32_t(status));
    put32(bytes.data() + 28, std::uint32_t(text.size()));
    for (size_t i = 0; i < text.size(); ++i) {
        bytes[32 + 2 * i] = std::uint8_t(text[i]);
        bytes[33 + 2 * i] = std::uint8_t(text[i] >> 8);
    }
    return bytes;
}
inline bool parseResponse(std::span<const std::uint8_t> bytes, const Request &request, Status &status,
                          std::wstring &text) {
    if (bytes.size() < 32 || bytes.size() > MaxResponseBytes || get32(bytes.data()) != Magic ||
        get32(bytes.data() + 4) != Version || !std::equal(request.id.begin(), request.id.end(), bytes.begin() + 8) ||
        get32(bytes.data() + 24) > std::uint32_t(Status::Failed))
        return false;
    status = Status(get32(bytes.data() + 24));
    size_t count = get32(bytes.data() + 28);
    if (count > MaxTextChars || bytes.size() != 32 + count * 2 || (status != Status::Success && count))
        return false;
    text.resize(count);
    for (size_t i = 0; i < count; ++i)
        text[i] = wchar_t(bytes[32 + i * 2] | unsigned(bytes[33 + i * 2]) << 8);
    return status != Status::Success || validText(text);
}
} // namespace SelectionProtocol
} // namespace Trans::Native::Windows
