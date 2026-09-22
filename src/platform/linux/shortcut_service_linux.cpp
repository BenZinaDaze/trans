#include "platform/shortcut_service.h"
#include "settings.h"

#include <KGlobalAccel>
#include <QAction>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>
#include <array>

namespace Trans {
namespace {

class LinuxShortcutService;

class LinuxShortcutJob final : public ShortcutJob {
public:
    LinuxShortcutJob(LinuxShortcutService *service, ShortcutAction action, QString sequence, QObject *owner);
    void cancel() override { fail({PlatformErrorCode::Cancelled, QStringLiteral("快捷键更新已取消。")}); }
};

class LinuxShortcutService final : public ShortcutService {
public:
    explicit LinuxShortcutService(QObject *owner) : ShortcutService(owner),
        m_watcher(QStringLiteral("org.kde.kglobalaccel"), QDBusConnection::sessionBus(),
                  QDBusServiceWatcher::WatchForOwnerChange, this)
    {
        m_actions[0].setObjectName(QStringLiteral("translate-selection"));
        m_actions[0].setText(QStringLiteral("选区翻译"));
        m_actions[1].setObjectName(QStringLiteral("translate-screenshot"));
        m_actions[1].setText(QStringLiteral("截图翻译"));
        connect(&m_actions[0], &QAction::triggered, this, [this] { emit triggered(ShortcutAction::Selection); });
        connect(&m_actions[1], &QAction::triggered, this, [this] { emit triggered(ShortcutAction::Screenshot); });
        connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &owner) {
            m_available = !owner.isEmpty();
            if (!m_available) {
                m_sequences = {};
                m_registered = {};
                emit changed();
            }
            emit capabilityChanged();
        });
        if (supported()) refreshAvailability();
    }
    ShortcutJob *update(ShortcutAction action, const QString &sequence, QObject *owner) override
    {
        return new LinuxShortcutJob(this, action, sequence, owner);
    }
    QString sequence(ShortcutAction action) const override { return m_sequences[index(action)]; }
    CapabilityState availability() const override
    {
        if (!supported()) return CapabilityState::Unsupported;
        return m_available ? CapabilityState::Available : CapabilityState::Unavailable;
    }
    QString unavailableReason() const override
    {
        if (!supported()) return QStringLiteral("当前窗口系统不支持此全局快捷键后端。");
        return m_available ? QString() : QStringLiteral("KDE 全局快捷键服务不可用，请检查桌面服务后重试。");
    }
    PlatformError apply(ShortcutAction action, const QString &requested, bool &success)
    {
        success = false;
        const auto validation = validateShortcut(requested);
        if (!validation.isEmpty()) return {PlatformErrorCode::Failed, validation};
        if (!supported()) return {PlatformErrorCode::Unsupported, unavailableReason()};
        if (!QDBusConnection::sessionBus().isConnected())
            return {PlatformErrorCode::Unavailable, unavailableReason()};
        const auto i = index(action);
        ensureRegistered(i);
        refreshAvailability();
        if (!m_available) return {PlatformErrorCode::Unavailable, unavailableReason()};
        auto *accelerator = KGlobalAccel::self();
        const auto key = QKeySequence::fromString(requested, QKeySequence::PortableText);
        const auto previous = accelerator->shortcut(&m_actions[i]);
        // KDE counts the action's own active binding as occupied.
        if (!key.isEmpty() && !previous.contains(key)
            && !KGlobalAccel::isGlobalShortcutAvailable(key, QCoreApplication::applicationName()))
            return {PlatformErrorCode::Conflict, QStringLiteral("此快捷键已被其他应用占用，请录制另一个组合。")};
        const QList<QKeySequence> desired = key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key};
        const bool applied = accelerator->setShortcut(&m_actions[i], desired, KGlobalAccel::NoAutoloading);
        const auto actual = accelerator->shortcut(&m_actions[i]);
        refresh(i, accelerator);
        refreshAvailability();
        emit changed();
        if (!applied || actual != desired) {
            if (!m_available) return {PlatformErrorCode::Unavailable, unavailableReason()};
            return {PlatformErrorCode::Failed, QStringLiteral("KDE 未能应用快捷键，请检查全局快捷键服务后重试。")};
        }
        success = true;
        return {};
    }
private:
    static size_t index(ShortcutAction action) { return action == ShortcutAction::Selection ? 0 : 1; }
    static bool supported()
    {
        const auto platform = QGuiApplication::platformName();
        return platform == "xcb" || platform == "wayland" || platform == "wayland-egl";
    }
    void refresh(size_t i, KGlobalAccel *accelerator)
    {
        const auto keys = accelerator->shortcut(&m_actions[i]);
        m_sequences[i] = keys.isEmpty() ? QString() : keys.first().toString(QKeySequence::PortableText);
    }
    void ensureRegistered(size_t i)
    {
        if (m_registered[i]) return;
        auto *accelerator = KGlobalAccel::self();
        if (!m_observing) {
            m_observing = true;
            connect(accelerator, &KGlobalAccel::globalShortcutChanged, this,
                    [this, accelerator](QAction *action, const QKeySequence &) {
                for (size_t index = 0; index < m_actions.size(); ++index) {
                    if (action == &m_actions[index]) {
                        refresh(index, accelerator);
                        emit changed();
                    }
                }
            });
        }
        const QList<QKeySequence> defaults{QKeySequence(i == 0 ? QStringLiteral("Meta+Shift+T") : QStringLiteral("Meta+Shift+O"))};
        accelerator->setDefaultShortcut(&m_actions[i], defaults);
        m_registered[i] = accelerator->setShortcut(&m_actions[i], defaults, KGlobalAccel::Autoloading);
        refresh(i, accelerator);
        emit changed();
    }
    void refreshAvailability()
    {
        auto *interface = QDBusConnection::sessionBus().interface();
        const bool available = interface && interface->isServiceRegistered(QStringLiteral("org.kde.kglobalaccel")).value();
        if (m_available == available) return;
        m_available = available;
        emit capabilityChanged();
    }
    std::array<QAction, 2> m_actions;
    std::array<QString, 2> m_sequences;
    std::array<bool, 2> m_registered{};
    QDBusServiceWatcher m_watcher;
    bool m_available = false;
    bool m_observing = false;
};

LinuxShortcutJob::LinuxShortcutJob(LinuxShortcutService *service, ShortcutAction action, QString sequence, QObject *owner)
    : ShortcutJob(owner)
{
    QTimer::singleShot(0, this, [this, service = QPointer<LinuxShortcutService>(service), action, sequence = std::move(sequence)] {
        if (finished()) return;
        if (!service) {
            fail({PlatformErrorCode::Unavailable, QStringLiteral("快捷键服务已关闭。")});
            return;
        }
        bool applied = false;
        const auto error = service->apply(action, sequence, applied);
        if (applied) succeed();
        else fail(error);
    });
}

} // namespace

ShortcutService *createShortcutService(QObject *owner) { return new LinuxShortcutService(owner); }

} // namespace Trans
