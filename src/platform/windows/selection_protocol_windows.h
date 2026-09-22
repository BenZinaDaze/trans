#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringDecoder>

namespace Trans::SelectionProtocol {

inline constexpr int Version = 1;
inline constexpr int TimeoutMs = 3500;
inline constexpr qsizetype MaxRequestBytes = 2048;
inline constexpr qsizetype MaxResponseBytes = 512 * 1024;
inline constexpr qsizetype MaxStderrBytes = 4096;
inline constexpr qsizetype MaxTextChars = 64 * 1024;
inline constexpr int MaxRanges = 128;

struct Request {
    QString id;
    quint64 window = 0;
    quint32 processId = 0;
    quint32 threadId = 0;
    quint32 parentProcessId = 0;
};

enum class Status { Success, NoSelection, Unsupported, PermissionDenied, Cancelled, Failed };

inline QString statusName(Status status)
{
    switch (status) {
    case Status::Success: return QStringLiteral("success");
    case Status::NoSelection: return QStringLiteral("no_selection");
    case Status::Unsupported: return QStringLiteral("unsupported");
    case Status::PermissionDenied: return QStringLiteral("permission_denied");
    case Status::Cancelled: return QStringLiteral("cancelled");
    case Status::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

inline bool parseObject(const QByteArray &bytes, qsizetype limit, QJsonObject &object)
{
    if (bytes.isEmpty() || bytes.size() > limit) return false;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder.decode(bytes);
    if (decoder.hasError() || !decoded.isValidUtf16()) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    object = document.object();
    return object.value(QStringLiteral("version")).isDouble()
        && object.value(QStringLiteral("version")).toDouble() == Version;
}

inline QByteArray encodeRequest(const Request &request)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("version"), Version},
        {QStringLiteral("id"), request.id},
        {QStringLiteral("window"), QString::number(request.window, 16)},
        {QStringLiteral("process"), static_cast<qint64>(request.processId)},
        {QStringLiteral("thread"), static_cast<qint64>(request.threadId)},
        {QStringLiteral("parent"), static_cast<qint64>(request.parentProcessId)}
    }).toJson(QJsonDocument::Compact);
}

inline bool parseRequest(const QByteArray &bytes, Request &request)
{
    QJsonObject object;
    if (!parseObject(bytes, MaxRequestBytes, object) || object.size() != 6) return false;
    const auto id = object.value(QStringLiteral("id"));
    const auto window = object.value(QStringLiteral("window"));
    if (!id.isString() || id.toString().size() != 32 || !window.isString()) return false;
    request.id = id.toString();
    for (const QChar c : request.id) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) return false;
    }
    const QString windowText = window.toString();
    bool ok = false;
    request.window = windowText.toULongLong(&ok, 16);
    if (!ok || !request.window || QString::number(request.window, 16) != windowText) return false;
    const auto readId = [&object](const QString &key, quint32 &value) {
        const auto item = object.value(key);
        if (!item.isDouble()) return false;
        const double number = item.toDouble();
        if (number < 1 || number > 4294967295.0) return false;
        value = static_cast<quint32>(number);
        return static_cast<double>(value) == number;
    };
    return readId(QStringLiteral("process"), request.processId)
        && readId(QStringLiteral("thread"), request.threadId)
        && readId(QStringLiteral("parent"), request.parentProcessId);
}

inline bool validText(const QString &text)
{
    return !text.trimmed().isEmpty() && text.size() <= MaxTextChars
        && !text.contains(QChar::Null) && text.isValidUtf16();
}

inline QByteArray encodeResponse(const QString &id, Status status, const QString &text = {})
{
    QJsonObject object{{QStringLiteral("version"), Version},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("status"), statusName(status)}};
    if (status == Status::Success) object.insert(QStringLiteral("text"), text);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

inline bool parseResponse(const QByteArray &bytes, const QString &id, Status &status, QString &text)
{
    QJsonObject object;
    if (!parseObject(bytes, MaxResponseBytes, object)
        || !object.value(QStringLiteral("id")).isString()
        || object.value(QStringLiteral("id")).toString() != id
        || !object.value(QStringLiteral("status")).isString()) return false;
    const QString name = object.value(QStringLiteral("status")).toString();
    for (const auto candidate : {Status::Success, Status::NoSelection, Status::Unsupported,
                                 Status::PermissionDenied, Status::Cancelled, Status::Failed}) {
        if (name != statusName(candidate)) continue;
        status = candidate;
        if (status != Status::Success) return object.size() == 3;
        const auto value = object.value(QStringLiteral("text"));
        if (object.size() != 4 || !value.isString()) return false;
        text = value.toString();
        return validText(text);
    }
    return false;
}

} // namespace Trans::SelectionProtocol
