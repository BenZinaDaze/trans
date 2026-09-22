#include "settings.h"

#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QSettings>
#include <QTemporaryFile>
#include "platform/settings_storage.h"

namespace Trans {
namespace {
#ifdef Q_OS_WIN
constexpr auto SelectionShortcut = "Ctrl+Alt+T";
constexpr auto ScreenshotShortcut = "Ctrl+Alt+O";
#else
constexpr auto SelectionShortcut = "Meta+Shift+T";
constexpr auto ScreenshotShortcut = "Meta+Shift+O";
#endif
struct Option { const char *name; const char *key; QVariant value; };
const QList<Option> &options()
{
    static const QList<Option> entries{
        {"providerId", "translation/provider", "openai"},
        {"sourceLanguage", "translation/sourceLanguage", "auto"},
        {"targetLanguage", "translation/targetLanguage", "zh-CN"},
        {"systemPrompt", "translation/systemPrompt", defaultSystemPrompt()},
        {"timeoutSeconds", "translation/timeoutSeconds", 30},
        {"maxInputChars", "translation/maxInputChars", 20000},
        {"maxResponseKiB", "translation/maxResponseKiB", 2048},
        {"shortcut", "desktop/shortcut", SelectionShortcut},
        {"screenshotShortcut", "desktop/screenshotShortcut", ScreenshotShortcut},
        {"ocrApiKey", "ocr/baidu/apiKey", ""},
        {"ocrSecretKey", "ocr/baidu/secretKey", ""},
        {"fontSize", "window/fontSize", 17}, {"stayOnTop", "window/stayOnTop", true},
        {"restoreFocus", "window/restoreFocus", true}, {"popupPosition", "window/position", "screen"}
    };
    return entries;
}
}

QString validateShortcut(const QString &shortcut)
{
    if (shortcut.isEmpty())
        return {};
    const auto sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
    if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown
        || sequence[0].key() == Qt::Key_Control || sequence[0].key() == Qt::Key_Shift
        || sequence[0].key() == Qt::Key_Alt || sequence[0].key() == Qt::Key_Meta
        || !(sequence[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        return QStringLiteral("快捷键应包含 Ctrl、Alt 或 Meta，以及一个普通按键。清空可禁用快捷键。");
    return {};
}

AppSettings::AppSettings(const ProviderRegistry &registry, const QString &path, QObject *parent)
    : QObject(parent), m_registry(registry), m_path(QFileInfo(path).absoluteFilePath()), m_values(defaults())
{
    QSettings stored(m_path, QSettings::IniFormat);
    stored.setFallbacksEnabled(false);
    for (const auto &option : options()) {
        auto value = stored.value(option.key, option.value);
        if (!value.convert(option.value.metaType()))
            value = option.value;
        m_values[option.name] = value;
    }
    if (!m_registry.find(providerId()))
        m_values["providerId"] = QStringLiteral("openai");
    auto configs = m_values.value("providerConfigs").toMap();
    for (auto it = configs.begin(); it != configs.end(); ++it) {
        auto fields = it.value().toMap();
        const QString prefix = "providers/" + it.key() + '/';
        for (auto field = fields.begin(); field != fields.end(); ++field)
            field.value() = stored.value(prefix + field.key(), field.value());
        // Existing OpenAI settings used Chat Completions. Preserve that protocol during migration.
        if (it.key() == "openai" && stored.contains(prefix + "endpoint") && !stored.contains(prefix + "apiMode")) {
            fields["apiMode"] = QStringLiteral("chat");
            fields["reasoning"] = QStringLiteral("default");
        }
        it.value() = ProviderConfig::fromVariant(it.key(), fields).toVariant();
    }
    m_values["providerConfigs"] = configs;
    // A damaged or manually edited file remains repairable through the settings page.
    const auto error = validate(m_values);
    if (!error.isEmpty()) {
        m_error = error;
        // Runtime accessors use safe defaults until the user saves a valid snapshot.
        auto safe = defaults();
        safe["providerConfigs"] = configs;
        safe["providerId"] = m_values.value("providerId");
        m_values = safe;
    }
}

QVariantMap AppSettings::defaults() const
{
    QVariantMap values;
    for (const auto &option : options())
        values.insert(option.name, option.value);
    QVariantMap configs;
    for (const auto &item : providers()) {
        const auto descriptor = item.toMap();
        ProviderConfig config;
        config.id = descriptor.value("id").toString();
        config.endpoint = descriptor.value("defaultEndpoint").toString();
        config.model = descriptor.value("defaultModel").toString();
        config.apiMode = config.id == "deepseek" ? "chat" : "responses";
        configs.insert(config.id, config.toVariant());
    }
    values.insert("providerConfigs", configs);
    return values;
}

QVariantList AppSettings::languages() const
{
    return {QVariantMap{{"id", "zh-CN"}, {"name", "简体中文"}}, QVariantMap{{"id", "en"}, {"name", "English"}},
            QVariantMap{{"id", "ja"}, {"name", "日本語"}}, QVariantMap{{"id", "ko"}, {"name", "한국어"}},
            QVariantMap{{"id", "de"}, {"name", "Deutsch"}}, QVariantMap{{"id", "fr"}, {"name", "Français"}},
            QVariantMap{{"id", "es"}, {"name", "Español"}}};
}

ProviderConfig AppSettings::config(const QString &id) const
{
    return ProviderConfig::fromVariant(id, m_values.value("providerConfigs").toMap().value(id).toMap());
}

TranslationRequest AppSettings::requestFromSnapshot(const QString &text, const QVariantMap &values)
{
    return {text, values.value("sourceLanguage", "auto").toString(), values.value("targetLanguage", "zh-CN").toString(),
            qBound(1, values.value("timeoutSeconds", 30).toInt(), 600) * 1000,
            qBound(16, values.value("maxResponseKiB", 2048).toInt(), 16384) * 1024,
            values.value("systemPrompt", defaultSystemPrompt()).toString()};
}

TranslationRequest AppSettings::request(const QString &text) const { return requestFromSnapshot(text, m_values); }

bool AppSettings::isConfigured() const
{
    const auto *provider = m_registry.find(providerId());
    return provider && validateConfig(provider->descriptor(), config(providerId())).isEmpty();
}

QString AppSettings::validate(const QVariantMap &values) const
{
    if (!m_registry.find(values.value("providerId").toString()))
        return QStringLiteral("请选择 OpenAI 或 DeepSeek。");
    QStringList languageIds;
    for (const auto &language : languages())
        languageIds.append(language.toMap().value("id").toString());
    if (!languageIds.contains(values.value("targetLanguage").toString())
        || (!languageIds.contains(values.value("sourceLanguage").toString()) && values.value("sourceLanguage") != "auto"))
        return QStringLiteral("请选择有效的源语言和目标语言。");
    struct Range { const char *key; int min; int max; };
    for (const auto &range : {Range{"timeoutSeconds", 1, 600}, Range{"maxInputChars", 1, 200000},
                              Range{"maxResponseKiB", 16, 16384}, Range{"fontSize", 10, 32}}) {
        bool ok = false;
        const auto number = values.value(range.key).toInt(&ok);
        if (!ok || number < range.min || number > range.max)
            return QStringLiteral("设置项 %1 必须在 %2 到 %3 之间。").arg(range.key).arg(range.min).arg(range.max);
    }
    const auto prompt = values.value("systemPrompt").toString();
    if (prompt.trimmed().isEmpty() || prompt.size() > 20000 || !prompt.contains("{{targetLanguage}}"))
        return QStringLiteral("翻译提示词不能为空，最多 20,000 个字符，并须包含 {{targetLanguage}}。");
    if (values.value("popupPosition") != "screen" && values.value("popupPosition") != "cursor")
        return QStringLiteral("请选择有效的弹窗位置。");
    const auto shortcutError = validateShortcut(values.value("shortcut").toString());
    if (!shortcutError.isEmpty())
        return shortcutError;
    const auto screenshotShortcut = values.value("screenshotShortcut").toString();
    const auto screenshotError = validateShortcut(screenshotShortcut);
    if (!screenshotError.isEmpty()) return screenshotError;
    if (!screenshotShortcut.isEmpty() && QKeySequence(screenshotShortcut) == QKeySequence(values.value("shortcut").toString()))
        return QStringLiteral("截图翻译与选区翻译不能使用相同的快捷键。");
    const auto configs = values.value("providerConfigs").toMap();
    for (const auto &item : providers()) {
        const auto id = item.toMap().value("id").toString();
        const auto *provider = m_registry.find(id);
        const auto error = validateConfig(provider->descriptor(), ProviderConfig::fromVariant(id, configs.value(id).toMap()), false);
        if (!error.isEmpty())
            return provider->descriptor().name + QStringLiteral("：") + error;
    }
    return {};
}

bool AppSettings::setError(const QString &error)
{
    m_error = error;
    emit errorChanged();
    return error.isEmpty();
}

bool AppSettings::persist(const QVariantMap &values)
{
    const auto directory = QFileInfo(m_path).absolutePath();
    QString storageError;
    if (!prepareSettingsDirectory(directory, &storageError))
        return setError(storageError);
    QTemporaryFile temporary(directory + "/.settings-XXXXXX.ini");
    if (!temporary.open())
        return setError(QStringLiteral("无法创建临时配置文件。"));
    temporary.close();
    {
        QSettings output(temporary.fileName(), QSettings::IniFormat);
        output.setFallbacksEnabled(false);
        for (const auto &option : options())
            output.setValue(option.key, values.value(option.name));
        const auto configs = values.value("providerConfigs").toMap();
        for (const auto &item : providers()) {
            const auto id = item.toMap().value("id").toString();
            const auto fields = ProviderConfig::fromVariant(id, configs.value(id).toMap()).toVariant();
            for (auto it = fields.begin(); it != fields.end(); ++it)
                output.setValue("providers/" + id + '/' + it.key(), it.value());
        }
        output.sync();
        if (output.status() != QSettings::NoError)
            return setError(QStringLiteral("配置文件写入失败。"));
    }
    QFile source(temporary.fileName());
    if (!source.open(QIODevice::ReadOnly))
        return setError(QStringLiteral("无法读取临时配置文件。"));
    if (!writePrivateSettings(m_path, source.readAll(), &storageError))
        return setError(storageError);
    return true;
}

bool AppSettings::save(const QVariantMap &values)
{
    const auto error = validate(values);
    if (!error.isEmpty())
        return setError(error);
    if (!persist(values))
        return false;
    m_values = values;
    setError({});
    emit settingsChanged();
    return true;
}

} // namespace Trans
