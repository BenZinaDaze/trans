#pragma once

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Trans::Native {
using Json = nlohmann::json;

struct Result {
    std::string text;
    std::string detectedLanguage;
    std::string error;
};

Json defaultSettings();
// Does not create or modify files. On a fatal read/parse error returns null;
// on a validation error returns the snapshot so the settings UI can repair it.
// A valid INI import must be privately persisted by the caller before use.
Json loadSettings(const std::filesystem::path &configDirectory, std::string &error);
std::string validateSettings(const Json &settings);
std::string defaultSystemPrompt();
std::vector<std::pair<std::string, std::string>> languages();

// Synchronous, thread-safe operations. Run away from the UI thread. No secrets
// or server-provided error bodies are included in errors. Cancellation is final.
Result translate(const std::string &text, const Json &settings, std::stop_token stop);
std::vector<std::string> fetchModels(const std::string &providerId, const Json &settings, std::stop_token stop,
                                     std::string &error);
Result recognize(const std::vector<std::uint8_t> &png, const Json &settings, std::stop_token stop);
} // namespace Trans::Native
