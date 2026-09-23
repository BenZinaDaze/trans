#include "app.h"
#include "business.h"
#include "platform.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <png.h>
#include <thread>

using namespace Trans::Native;
using namespace std::chrono_literals;
namespace {
std::string str(const slint::SharedString &s) { return std::string(s.data(), s.size()); }
std::vector<std::uint8_t> pngEncode(const ScreenImage &image, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x + width > image.width || y + height > image.height)
        throw std::runtime_error("无效截图区域");
    std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
    for (int row = 0; row < height; ++row)
        std::memcpy(pixels.data() + std::size_t(row) * width * 4,
                    image.rgba.data() + (std::size_t(y + row) * image.width + x) * 4, std::size_t(width) * 4);
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = width;
    png.height = height;
    png.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&png, nullptr, &size, 0, pixels.data(), 0, nullptr))
        throw std::runtime_error(png.message);
    std::vector<std::uint8_t> data(size);
    if (!png_image_write_to_memory(&png, data.data(), &size, 0, pixels.data(), 0, nullptr))
        throw std::runtime_error(png.message);
    data.resize(size);
    return data;
}
class Application {
    std::unique_ptr<Platform> platform = createPlatform();
    slint::ComponentHandle<TranslationWindow> popup = TranslationWindow::create();
    slint::ComponentHandle<SettingsWindow> settings = SettingsWindow::create();
    slint::ComponentHandle<Tray> tray = Tray::create();
    std::vector<slint::ComponentHandle<CaptureWindow>> overlays;
    Json config, draft, repairDraft;
    std::string configPath, source, startupError, provider = "openai";
    SourcePtr origin;
    std::jthread worker, toolWorker;
    std::uint64_t generation = 0, toolGeneration = 0;
    bool quitting = false, ready = false, smoke = false;
    bool popupBeforeCapture = false, settingsBeforeCapture = false;
    slint::Timer captureTimer, smokeTimer, nativeTimer;
    std::shared_ptr<Capture> capture;
    std::uintptr_t handle(slint::Window &window) {
#ifdef _WIN32
        return reinterpret_cast<std::uintptr_t>(window.win32_hwnd());
#else
        return window.x11_window_id();
#endif
    }
    template <class F> void post(F f) {
        slint::invoke_from_event_loop([this, f = std::move(f)]() mutable {
            if (!quitting)
                f();
        });
    }
    void cancel() {
        ++generation;
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
        captureTimer.stop();
        closeOverlays();
        popup->set_busy(false);
    }
    void closeOverlays() {
        for (auto &o : overlays)
            o->hide();
        overlays.clear();
        capture.reset();
    }
    void status(const std::string &s) { popup->set_status_text(slint::SharedString(s)); }
    void showPopup() {
        popup->set_pinned(config.value("stayOnTop", true));
        popup->show();
        auto area = platform->availableGeometry(config.value("popupPosition", "screen") == "cursor");
        auto size = popup->window().size();
        if (area.width > 0 && area.height > 0) {
            size.width = std::min(size.width, unsigned(area.width));
            size.height = std::min(size.height, unsigned(area.height));
            popup->window().set_size(size);
            int x = area.x + (area.width - int(size.width)) / 2, y = area.y + (area.height - int(size.height)) / 2;
            if (config.value("popupPosition", "screen") == "cursor") {
                const auto cursor = platform->cursorGeometry();
                if (cursor.width > 0) {
                    x = cursor.x + 16;
                    y = cursor.y + 16;
                }
            }
            x = std::clamp(x, area.x, area.x + area.width - int(size.width));
            y = std::clamp(y, area.y, area.y + area.height - int(size.height));
            popup->window().set_position(slint::PhysicalPosition({x, y}));
        }
        configureNativeWindows();
    }
    void closePopup() {
        cancel();
        popup->hide();
        if (config.value("restoreFocus", true) && !settings->window().is_visible())
            platform->restoreSource(origin);
    }
    void openSettings(int page) {
        draft = repairDraft.is_null() ? config : repairDraft;
        const auto selected = draft.contains("providerId") && draft["providerId"].is_string()
                                  ? draft["providerId"].get<std::string>()
                                  : "";
        provider = selected == "deepseek" ? "deepseek" : "openai";
        settings->set_provider_index(selected == "deepseek" ? 1 : selected == "openai" ? 0 : -1);
        settings->set_page(page);
        settings->set_feedback(slint::SharedString(startupError));
        settings->set_tool_result(slint::SharedString(""));
        populateFields();
        settings->set_pinned(config.value("stayOnTop", true));
        settings->show();
        configureNativeWindows();
    }
    void configureNativeWindows() {
        nativeTimer.start(slint::TimerMode::SingleShot, 50ms, [this] {
            const bool pinned = config.value("stayOnTop", true);
            const auto popupHandle = handle(popup->window()), settingsHandle = handle(settings->window());
            if (popupHandle)
                platform->configureWindow(popupHandle, true, pinned);
            if (settingsHandle)
                platform->configureWindow(settingsHandle, false, pinned, popupHandle);
            if (settings->window().is_visible())
                platform->activateWindow(settingsHandle);
            else if (popup->window().is_visible())
                platform->activateWindow(popupHandle);
        });
    }
    void closeSettings() {
        ++toolGeneration;
        if (toolWorker.joinable()) {
            toolWorker.request_stop();
            toolWorker.join();
        }
        settings->set_busy(false);
        settings->hide();
        draft = Json{};
        settings->set_tool_result(slint::SharedString(""));
    }
    void populateFields() {
        std::vector<SettingField> fields;
        auto add = [&](const char *key, const char *label, bool secret = false, bool multiline = false) {
            const Json &obj = settings->get_page() == 0 ? draft["providerConfigs"][provider] : draft;
            auto val = obj.value(key, Json{});
            std::string text = val.is_string() ? val.get<std::string>() : val.dump();
            std::vector<slint::SharedString> choices;
            const std::string name = key;
            if (name == "sourceLanguage")
                choices.emplace_back("auto");
            if (name == "sourceLanguage" || name == "targetLanguage")
                for (const auto &[id, label] : languages())
                    choices.emplace_back(id);
            if (name == "popupPosition")
                choices = {"screen", "cursor"};
            if (name == "apiMode")
                choices = provider == "deepseek" ? std::vector<slint::SharedString>{"chat"}
                                                 : std::vector<slint::SharedString>{"responses", "chat"};
            if (name == "reasoning")
                choices = provider == "deepseek"
                              ? std::vector<slint::SharedString>{"default", "none", "low", "high", "max"}
                              : std::vector<slint::SharedString>{"default", "none", "minimal", "low",
                                                                 "medium",  "high", "xhigh"};
            auto found = std::find(choices.begin(), choices.end(), slint::SharedString(text));
            const int index = found == choices.end() ? -1 : static_cast<int>(found - choices.begin());
            fields.push_back({key, label, slint::SharedString(text), secret, multiline, val.is_boolean(),
                              std::make_shared<slint::VectorModel<slint::SharedString>>(choices), index});
        };
        switch (settings->get_page()) {
        case 0:
            add("endpoint", "基础地址");
            add("apiKey", "API 密钥", true);
            add("model", "模型");
            add("apiMode", "API 模式：responses / chat");
            add("temperatureEnabled", "启用温度：true / false");
            add("temperature", "温度（0–2）");
            add("maxOutputTokens", "最大输出 Token（0 为默认）");
            add("reasoning", "推理级别：default / none / minimal / low / medium / high");
            add("headersJson", "自定义请求头 JSON", false, true);
            add("optionsJson", "额外参数 JSON", false, true);
            break;
        case 1:
            add("sourceLanguage", "源语言：auto / zh-CN / en / ja / ko / de / fr / es");
            add("targetLanguage", "目标语言：zh-CN / en / ja / ko / de / fr / es");
            add("systemPrompt", "翻译提示词（保留 {{targetLanguage}}）", false, true);
            add("timeoutSeconds", "请求超时（秒）");
            add("maxInputChars", "最大输入字符数");
            add("maxResponseKiB", "最大响应（KiB）");
            break;
        case 2:
            add("shortcut", "选区快捷键（例 Ctrl+Alt+T，清空禁用）");
            add("screenshotShortcut", "截图快捷键");
            add("stayOnTop", "窗口置顶：true / false");
            add("restoreFocus", "关闭后恢复焦点：true / false");
            add("popupPosition", "弹窗位置：screen / cursor");
            add("fontSize", "译文字号（10–32）");
            break;
        case 3:
            add("ocrApiKey", "百度 OCR API Key", true);
            add("ocrSecretKey", "百度 OCR Secret Key", true);
            break;
        }
        settings->set_fields(std::make_shared<slint::VectorModel<SettingField>>(fields));
    }
    void editField(const std::string &key, const std::string &value) {
        Json &obj = settings->get_page() == 0 ? draft["providerConfigs"][provider] : draft;
        if (!obj.contains(key))
            return;
        try {
            const auto defaults = defaultSettings();
            const auto &schema = settings->get_page() == 0 ? defaults.at("providerConfigs").at(provider) : defaults;
            if (schema.at(key).is_boolean()) {
                if (value != "true" && value != "false")
                    throw std::runtime_error("请输入 true 或 false");
                obj[key] = value == "true";
            } else if (schema.at(key).is_number()) {
                auto parsed = Json::parse(value);
                if (!parsed.is_number())
                    throw std::runtime_error("请输入数字");
                obj[key] = parsed;
            } else
                obj[key] = value;
            settings->set_feedback(slint::SharedString(""));
        } catch (const std::exception &e) {
            obj[key] = value;
            settings->set_feedback(slint::SharedString(e.what()));
        }
    }
    void saveSettings() {
        auto error = validateSettings(draft);
        if (!error.empty()) {
            settings->set_feedback(slint::SharedString(error));
            return;
        }
        auto old = config;
        const bool shortcutsChanged =
            draft["shortcut"] != old["shortcut"] || draft["screenshotShortcut"] != old["screenshotShortcut"];
        Error result;
        if (shortcutsChanged) {
            result = platform->setShortcuts(draft.value("shortcut", ""), draft.value("screenshotShortcut", ""), true);
            if (result) {
                settings->set_feedback(slint::SharedString(result.message));
                return;
            }
        }
        result = platform->writePrivateFile(configPath, draft.dump(2));
        if (result) {
            Error rollback;
            if (shortcutsChanged)
                rollback = platform->setShortcuts(old.value("shortcut", ""), old.value("screenshotShortcut", ""), true);
            settings->set_feedback(
                slint::SharedString(result.message + (rollback ? "；恢复快捷键失败：" + rollback.message : "")));
            return;
        }
        config = draft;
        repairDraft = nullptr;
        startupError.clear();
        applyConfig();
        settings->set_feedback(slint::SharedString("所有设置已保存。"));
        settings->set_pinned(config.value("stayOnTop", true));
        if (!source.empty() &&
            (old["providerId"] != config["providerId"] || old["sourceLanguage"] != config["sourceLanguage"] ||
             old["targetLanguage"] != config["targetLanguage"]))
            translateSource();
    }
    void applyConfig() {
        popup->set_text_size(config.value("fontSize", 17));
        popup->set_pinned(config.value("stayOnTop", true));
        popup->set_provider_name(config.value("providerId", "openai") == "deepseek" ? "DeepSeek" : "OpenAI");
        popup->set_language_text(slint::SharedString(config.value("sourceLanguage", "auto") + " → " +
                                                     config.value("targetLanguage", "zh-CN")));
    }
    void finishResult(const Result &result) {
        popup->set_busy(false);
        if (!result.error.empty()) {
            status(result.error);
            return;
        }
        popup->set_translated_text(slint::SharedString(result.text));
        status("");
        if (!result.detectedLanguage.empty())
            popup->set_language_text(
                slint::SharedString(result.detectedLanguage + " → " + config.value("targetLanguage", "zh-CN")));
    }
    void translateSource() {
        cancel();
        showPopup();
        popup->set_source_text(slint::SharedString(source));
        popup->set_translated_text(slint::SharedString(""));
        popup->set_busy(true);
        status("正在翻译…");
        auto id = generation;
        auto snapshot = config;
        auto input = source;
        worker = std::jthread([this, id, snapshot, input](std::stop_token stop) {
            auto result = translate(input, snapshot, stop);
            post([this, id, result = std::move(result)] {
                if (id == generation)
                    finishResult(result);
            });
        });
    }
    void selection() {
        cancel();
        origin = platform->captureSource();
        auto context = origin;
        auto id = generation;
        popup->set_source_is_ocr(false);
        status("正在读取选区…");
        worker = std::jthread([this, context, id](std::stop_token stop) {
            auto result = platform->readSelection(context, stop);
            post([this, id, result = std::move(result)] {
                if (id != generation)
                    return;
                if (result.error) {
                    showPopup();
                    status(result.error.message);
                    return;
                }
                source = result.text;
                translateSource();
            });
        });
    }
    void recognizeImage(std::vector<std::uint8_t> bytes) {
        closeOverlays();
        popup->set_busy(true);
        status("正在识别截图…");
        showPopup();
        auto id = generation;
        auto snapshot = config;
        if (worker.joinable())
            worker.join();
        worker = std::jthread([this, id, snapshot, bytes = std::move(bytes)](std::stop_token stop) {
            auto result = recognize(bytes, snapshot, stop);
            post([this, id, result = std::move(result)] {
                if (id != generation)
                    return;
                if (!result.error.empty()) {
                    popup->set_busy(false);
                    status(result.error);
                    return;
                }
                source = result.text;
                popup->set_source_is_ocr(true);
                translateSource();
            });
        });
    }
    void presentCapture(Capture result) {
        if (result.error) {
            showPopup();
            status(result.error.message);
            return;
        }
        if (result.screens.empty()) {
            showPopup();
            status("未找到可截图屏幕。");
            return;
        }
        capture = std::make_shared<Capture>(std::move(result));
        if (capture->regionSelected) {
            auto &s = capture->screens.front();
            recognizeImage(pngEncode(s, 0, 0, s.width, s.height));
            return;
        }
        auto frame = capture;
        for (std::size_t i = 0; i < frame->screens.size(); ++i) {
            auto o = CaptureWindow::create();
            const auto &s = frame->screens[i];
            slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(s.width, s.height);
            std::memcpy(buffer.begin(), s.rgba.data(), s.rgba.size());
            o->set_screen_image(slint::Image(buffer));
            o->on_cancelled([this] {
                cancel();
                if (popupBeforeCapture)
                    showPopup();
                if (settingsBeforeCapture)
                    settings->show();
            });
            o->on_selected([this, frame, i](float x, float y, float x2, float y2) {
                const auto &image = frame->screens[i];
                auto size = overlays[i]->window().size();
                auto scale = overlays[i]->window().scale_factor();
                int left = std::clamp(int(std::min(x, x2) * scale * image.width / size.width), 0, image.width);
                int top = std::clamp(int(std::min(y, y2) * scale * image.height / size.height), 0, image.height);
                int right = std::clamp(int(std::max(x, x2) * scale * image.width / size.width), 0, image.width);
                int bottom = std::clamp(int(std::max(y, y2) * scale * image.height / size.height), 0, image.height);
                try {
                    recognizeImage(pngEncode(image, left, top, right - left, bottom - top));
                } catch (const std::exception &e) {
                    closeOverlays();
                    showPopup();
                    status(e.what());
                }
            });
            o->show();
            o->window().set_position(slint::PhysicalPosition({s.geometry.x, s.geometry.y}));
            o->window().set_size(slint::PhysicalSize({unsigned(s.geometry.width), unsigned(s.geometry.height)}));
            o->window().set_fullscreen(true);
            overlays.push_back(o);
        }
    }
    void screenshot() {
        if (config.value("ocrApiKey", "").empty() || config.value("ocrSecretKey", "").empty()) {
            openSettings(3);
            settings->set_feedback("请先填写百度 OCR API Key 和 Secret Key。");
            return;
        }
        cancel();
        origin = platform->captureSource();
        popupBeforeCapture = popup->window().is_visible();
        settingsBeforeCapture = settings->window().is_visible();
        popup->hide();
        settings->hide();
        auto id = generation;
        captureTimer.start(slint::TimerMode::SingleShot, 150ms, [this, id] {
            worker = std::jthread([this, id](std::stop_token stop) {
                auto result = platform->captureScreens(stop);
                post([this, id, result = std::move(result)]() mutable {
                    if (id == generation)
                        presentCapture(std::move(result));
                });
            });
        });
    }
    void runTool(bool models) {
        ++toolGeneration;
        if (toolWorker.joinable()) {
            toolWorker.request_stop();
            toolWorker.join();
        }
        auto id = toolGeneration;
        auto snapshot = draft;
        snapshot["providerId"] = provider;
        auto p = provider;
        settings->set_busy(true);
        settings->set_tool_result(slint::SharedString(models ? "正在获取模型…" : "正在测试翻译…"));
        toolWorker = std::jthread([this, id, snapshot, p, models](std::stop_token stop) {
            if (models) {
                std::string error;
                auto list = fetchModels(p, snapshot, stop, error);
                post([this, id, list = std::move(list), error] {
                    if (id != toolGeneration)
                        return;
                    settings->set_busy(false);
                    settings->set_tool_result(slint::SharedString(error));
                    std::vector<slint::SharedString> values;
                    for (auto &s : list)
                        values.emplace_back(s);
                    settings->set_models(std::make_shared<slint::VectorModel<slint::SharedString>>(values));
                });
            } else {
                auto result = translate("Hello, world!", snapshot, stop);
                post([this, id, result = std::move(result)] {
                    if (id == toolGeneration) {
                        settings->set_busy(false);
                        settings->set_tool_result(
                            slint::SharedString(result.error.empty() ? result.text : result.error));
                    }
                });
            }
        });
    }
    void quit() {
        quitting = true;
        cancel();
        closeSettings();
        tray->set_tray_visible(false);
        popup->hide();
        slint::quit_event_loop();
    }

  public:
    ~Application() {
        quitting = true;
        cancel();
        closeSettings();
        platform.reset();
    }
    Application(bool smokeMode) : smoke(smokeMode) {
        settings->set_app_version("v" TRANS_VERSION);
        std::string error;
        auto directory = platform->configDirectory();
        configPath = directory + "/settings.json";
        config = smoke ? defaultSettings() : loadSettings(std::filesystem::u8path(directory), error);
        if (config.is_null())
            throw std::runtime_error(error);
        if (!config.contains("schemaVersion") || config["schemaVersion"] != 1 || !config.contains("providerConfigs") ||
            !config["providerConfigs"].is_object())
            throw std::runtime_error(error.empty() ? "配置文件结构损坏，未修改原文件。" : error);
        for (const auto id : {"openai", "deepseek"})
            if (!config["providerConfigs"].contains(id) || !config["providerConfigs"][id].is_object())
                throw std::runtime_error(error.empty() ? "服务配置结构损坏，未修改原文件。" : error);
        if (!smoke && error.empty() && !std::filesystem::exists(std::filesystem::u8path(configPath)) &&
            std::filesystem::exists(std::filesystem::u8path(directory) / "settings.ini")) {
            auto result = platform->writePrivateFile(configPath, config.dump(2));
            if (result)
                error = result.message;
        }
        if (!error.empty()) {
            startupError = error;
            repairDraft = config;
            config = defaultSettings();
        }
        draft = config;
        applyConfig();
        status(error);
        popup->on_settings([this](int page) { openSettings(page); });
        popup->on_retry([this] { translateSource(); });
        popup->on_cancel([this] {
            cancel();
            status("已取消。");
        });
        popup->on_copy([this] { status("已复制。"); });
        popup->on_close_popup([this] { closePopup(); });
        popup->window().on_close_requested([this] {
            closePopup();
            return slint::CloseRequestResponse::KeepWindowShown;
        });
        settings->on_close_settings([this] { closeSettings(); });
        settings->window().on_close_requested([this] {
            closeSettings();
            return slint::CloseRequestResponse::KeepWindowShown;
        });
        settings->on_page_changed([this](int) { populateFields(); });
        settings->on_provider_changed([this](int index) {
            provider = index == 1 ? "deepseek" : "openai";
            draft["providerId"] = provider;
            populateFields();
        });
        settings->on_field_changed([this](slint::SharedString k, slint::SharedString v) { editField(str(k), str(v)); });
        settings->on_shortcut_recorded(
            [this](slint::SharedString field, slint::SharedString key, bool control, bool alt, bool shift, bool meta) {
                std::vector<std::string> parts;
                if (control)
                    parts.emplace_back("Control");
                if (alt)
                    parts.emplace_back("Alt");
                if (shift)
                    parts.emplace_back("Shift");
                if (meta)
                    parts.emplace_back("Meta");
                parts.push_back(str(key));
                auto keys = slint::Keys::from_parts(parts);
                if (!keys) {
                    settings->set_feedback("无法识别此快捷键，请直接输入。");
                    return;
                }
                std::string portable;
                for (const auto &part : keys->to_parts()) {
                    auto name = str(part);
                    if (name == "Control")
                        name = "Ctrl";
                    else if (name == "Escape")
                        name = "Esc";
                    else if (name == "PageUp")
                        name = "PgUp";
                    else if (name == "PageDown")
                        name = "PgDown";
                    else if (name == "Insert")
                        name = "Ins";
                    else if (name == "Delete")
                        name = "Del";
                    if (!portable.empty())
                        portable += "+";
                    portable += name;
                }
                editField(str(field), portable);
                populateFields();
            });
        settings->on_save([this] { saveSettings(); });
        settings->on_discard([this] { openSettings(settings->get_page()); });
        settings->on_fetch_models([this] { runTool(true); });
        settings->on_test_provider([this] { runTool(false); });
        settings->on_select_model([this](slint::SharedString model) {
            draft["providerConfigs"][provider]["model"] = str(model);
            populateFields();
        });
        settings->on_cancel_tool([this] {
            ++toolGeneration;
            if (toolWorker.joinable()) {
                toolWorker.request_stop();
                toolWorker.join();
            }
            settings->set_busy(false);
            settings->set_tool_result(slint::SharedString("已取消。"));
        });
        tray->on_show_translation([this] { showPopup(); });
        tray->on_translate_selection([this] { selection(); });
        tray->on_screenshot([this] { screenshot(); });
        tray->on_settings([this] { openSettings(0); });
        tray->on_quit([this] { quit(); });
    }
    int run(Command command, bool explicitCommand) {
        if (!smoke) {
            auto instance = platform->startInstance(command, [this](Command c) { post([this, c] { dispatch(c); }); });
            if (instance.role == InstanceResult::Forwarded)
                return 0;
            if (instance.role == InstanceResult::Failed)
                throw std::runtime_error(instance.error.message);
            tray->set_tray_visible(true);
            auto error = platform->setShortcuts(config.value("shortcut", ""), config.value("screenshotShortcut", ""));
            if (error) {
                startupError = error.message;
                status(error.message);
            }
            ready = true;
            platform->setReady();
            if (explicitCommand)
                dispatch(command);
            else if (!startupError.empty() ||
                     config["providerConfigs"][config["providerId"].get<std::string>()].value("apiKey", "").empty())
                openSettings(0);
        } else {
            showPopup();
            openSettings(0);
            smokeTimer.start(slint::TimerMode::SingleShot, 1s, [this] {
                std::cout << "TRANS_SMOKE_PASS\n";
                quit();
            });
        }
        slint::run_event_loop(slint::EventLoopMode::RunUntilQuit);
        return 0;
    }
    void dispatch(Command c) {
        if (!settings->get_recording_key().empty())
            return;
        switch (c) {
        case Command::ShowTranslation:
            showPopup();
            break;
        case Command::TranslateSelection:
            selection();
            break;
        case Command::TranslateScreenshot:
            screenshot();
            break;
        case Command::ShowSettings:
            openSettings(0);
            break;
        }
    }
};
} // namespace
int main(int argc, char **argv) {
    bool smoke = false;
    Command command = Command::ShowTranslation;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--translate")
            command = Command::TranslateSelection;
        else if (arg == "--ocr")
            command = Command::TranslateScreenshot;
        else if (arg == "--settings")
            command = Command::ShowSettings;
        else if (arg == "--smoke-test")
            smoke = true;
        else if (arg == "--version") {
            std::cout << "Trans " << TRANS_VERSION << '\n';
            return 0;
        } else if (arg == "--help") {
            std::cout << "trans [--settings|--translate|--ocr|--smoke-test|--version]\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return 2;
        }
    }
    try {
        Application app(smoke);
        return app.run(command, argc > 1);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        if (!smoke) {
            try {
                auto failure = FailureWindow::create();
                failure->set_message(slint::SharedString(e.what()));
                failure->on_exit([] { slint::quit_event_loop(); });
                failure->run();
            } catch (...) { /* No graphical backend is available; stderr retains the original error. */
            }
        }
        return 1;
    }
}
