#include "platform/selection_reader.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>

namespace Trans {
namespace {

class LinuxSelectionJob final : public SelectionJob {
public:
    LinuxSelectionJob(SourceContextPtr source, QObject *owner) : SelectionJob(owner)
    {
        QTimer::singleShot(0, this, [this, source = std::move(source)] {
            if (finished()) return;
            auto *clipboard = QGuiApplication::clipboard();
            if (QGuiApplication::platformName() != "xcb" || !clipboard->supportsSelection()) {
                fail({PlatformErrorCode::Unsupported,
                      QStringLiteral("当前会话不支持读取其他应用的选区。请使用截图翻译，或切换到 X11 会话。")});
                return;
            }
            const QPointer<LinuxSelectionJob> guard(this);
            const auto text = clipboard->text(QClipboard::Selection);
            // Retrieving an X11 selection can run a nested event loop. A newer
            // request or close may have cancelled us while its owner replied.
            if (!guard || finished()) return;
            if (text.trimmed().isEmpty()) {
                fail({PlatformErrorCode::NoSelection,
                      QStringLiteral("请先在其他应用中选中一个词或一句话，再按翻译快捷键。")});
                return;
            }
            succeed({text, source});
        });
    }
    void cancel() override { fail({PlatformErrorCode::Cancelled, QStringLiteral("选区读取已取消。")}); }
};

class LinuxSelectionReader final : public SelectionReader {
public:
    using SelectionReader::SelectionReader;
    SelectionJob *read(SourceContextPtr source, QObject *owner) override
    {
        return new LinuxSelectionJob(std::move(source), owner);
    }
    CapabilityState availability() const override
    {
        return QGuiApplication::platformName() == "xcb" && QGuiApplication::clipboard()->supportsSelection()
            ? CapabilityState::Available : CapabilityState::Unsupported;
    }
    QString unavailableReason() const override
    {
        return availability() == CapabilityState::Available ? QString()
            : QStringLiteral("当前会话不支持读取其他应用的选区。请使用截图翻译，或切换到 X11 会话。");
    }
};

} // namespace

SelectionReader *createSelectionReader(QObject *owner) { return new LinuxSelectionReader(owner); }

} // namespace Trans
