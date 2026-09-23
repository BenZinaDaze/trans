#include "business.h"

#include <filesystem>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace Trans::Native;

// A process boundary lets the Python HTTP peer exercise real libcurl and
// configuration I/O on both platforms, without replacing business dependencies.
int main() {
    try {
        std::string line;
        if (!std::getline(std::cin, line))
            return 2;
        const auto input = Json::parse(line);
        const auto operation = input.at("operation").get<std::string>();
        if (operation == "defaults") {
            std::cout << defaultSettings().dump() << '\n';
            return 0;
        }
        if (operation == "load") {
            std::string error;
            const auto path = std::filesystem::u8path(input.at("directory").get<std::string>());
            auto settings = loadSettings(path, error);
            std::cout << Json{{"settings", std::move(settings)}, {"error", std::move(error)}}.dump() << '\n';
            return 0;
        }
        const auto settings = input.at("settings");
        if (operation == "validate") {
            std::cout << Json{{"error", validateSettings(settings)}}.dump() << '\n';
            return 0;
        }
        std::stop_source source;
        if (input.value("cancelled", false))
            source.request_stop();
        std::jthread cancellation;
        if (input.value("waitForCancel", false)) {
            cancellation = std::jthread([&] {
                std::string command;
                if (std::getline(std::cin, command) && command == "cancel")
                    source.request_stop();
            });
        }
        Json output;
        if (operation == "translate") {
            const auto result = translate(input.at("text").get<std::string>(), settings, source.get_token());
            output = {{"text", result.text}, {"detectedLanguage", result.detectedLanguage}, {"error", result.error}};
        } else if (operation == "models") {
            std::string error;
            auto models = fetchModels(input.at("providerId").get<std::string>(), settings, source.get_token(), error);
            output = {{"models", std::move(models)}, {"error", std::move(error)}};
        } else if (operation == "recognize") {
            const auto result =
                recognize(input.at("png").get<std::vector<std::uint8_t>>(), settings, source.get_token());
            output = {{"text", result.text}, {"error", result.error}};
        } else
            return 2;
        std::cout << output.dump() << '\n';
        return 0;
    } catch (...) {
        std::cerr << "Native business driver failed before producing a result.\n";
        return 2;
    }
}
