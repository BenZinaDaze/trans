#include "linux_internal.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <locale.h>
#include <string_view>

namespace Trans::Native::Linux {
namespace {
std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}
std::string lower(std::string_view value) {
    std::string result(value);
    for (auto &c : result)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return result;
}
int unicodeKey(std::string_view key) {
    if (key.empty())
        return -1;
    const auto lead = static_cast<unsigned char>(key[0]);
    const unsigned count = lead < 0x80                      ? 1
                           : (lead >= 0xc2 && lead <= 0xdf) ? 2
                           : (lead >= 0xe0 && lead <= 0xef) ? 3
                           : (lead >= 0xf0 && lead <= 0xf4) ? 4
                                                            : 0;
    if (!count || key.size() != count)
        return -1;
    unsigned scalar = count == 1 ? lead : lead & (0x7f >> count);
    for (unsigned i = 1; i < count; ++i) {
        const auto byte = static_cast<unsigned char>(key[i]);
        if ((byte & 0xc0) != 0x80)
            return -1;
        scalar = (scalar << 6) | (byte & 63);
    }
    if (scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f) || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff) || (count == 2 && scalar < 0x80) || (count == 3 && scalar < 0x800) ||
        (count == 4 && scalar < 0x10000))
        return -1;
    struct CharacterLocale {
        locale_t value = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
        ~CharacterLocale() {
            if (value)
                freelocale(value);
        }
    };
    static const CharacterLocale locale;
    if (locale.value)
        scalar = towupper_l(scalar, locale.value);
    else if (scalar >= 'a' && scalar <= 'z')
        scalar -= 'a' - 'A';
    return int(scalar);
}
} // namespace
int parseShortcut(const std::string &text) {
    auto sequence = trim(text);
    if (sequence.empty())
        return 0;
    // These are KGlobalAccel's stable integer wire encodings, inspected in the installed
    // protocol headers. No GUI toolkit header or library participates in this backend.
    constexpr int shift = 0x02000000, ctrl = 0x04000000, alt = 0x08000000, meta = 0x10000000;
    int modifiers = 0;
    while (sequence.size() > 1) {
        const auto separator = sequence.find('+');
        if (separator == std::string_view::npos || separator == 0)
            break;
        const auto token = lower(trim(sequence.substr(0, separator)));
        const int modifier = token == "shift"  ? shift
                             : token == "ctrl" ? ctrl
                             : token == "alt"  ? alt
                             : token == "meta" ? meta
                                               : 0;
        if (!modifier || (modifiers & modifier))
            return -1;
        modifiers |= modifier;
        sequence = trim(sequence.substr(separator + 1));
    }
    if (!(modifiers & (ctrl | alt | meta)))
        return -1;
    const auto name = lower(sequence);
    struct Named {
        std::string_view name;
        int key;
    };
    static constexpr Named names[]{{"space", 0x20},
                                   {"esc", 0x01000000},
                                   {"escape", 0x01000000},
                                   {"tab", 0x01000001},
                                   {"backtab", 0x01000002},
                                   {"backspace", 0x01000003},
                                   {"return", 0x01000004},
                                   {"enter", 0x01000005},
                                   {"ins", 0x01000006},
                                   {"insert", 0x01000006},
                                   {"del", 0x01000007},
                                   {"delete", 0x01000007},
                                   {"pause", 0x01000008},
                                   {"print", 0x01000009},
                                   {"home", 0x01000010},
                                   {"end", 0x01000011},
                                   {"left", 0x01000012},
                                   {"up", 0x01000013},
                                   {"right", 0x01000014},
                                   {"down", 0x01000015},
                                   {"pgup", 0x01000016},
                                   {"pgdown", 0x01000017},
                                   {"capslock", 0x01000024},
                                   {"numlock", 0x01000025},
                                   {"scrolllock", 0x01000026},
                                   {"menu", 0x01000055}};
    for (const auto &named : names)
        if (name == named.name)
            return modifiers | named.key;
    if (name.size() >= 2 && name[0] == 'f') {
        int number = 0;
        const auto parsed = std::from_chars(name.data() + 1, name.data() + name.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && number >= 1 && number <= 35)
            return modifiers | (0x01000030 + number - 1);
    }
    const auto key = unicodeKey(sequence);
    return key < 0 ? -1 : modifiers | key;
}
} // namespace Trans::Native::Linux
