#include "desktopprogress.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QLocale>
#include <QTime>

namespace {

const QString kLauncherPath = QStringLiteral("/com/canonical/unity/launcherentry/shred_gui");
const QString kLauncherInterface = QStringLiteral("com.canonical.Unity.LauncherEntry");
const QString kJobServer = QStringLiteral("org.kde.JobViewServer");
const QString kJobServerPath = QStringLiteral("/JobViewServer");
const QString kJobServerInterface = QStringLiteral("org.kde.JobViewServerV2");
const QString kJobInterface = QStringLiteral("org.kde.JobViewV3");
// The progress shared between running instances: Update(double progress, int64 totalBytes, bool running).
const QString kPeerPath = QStringLiteral("/shred_gui/progress");
const QString kPeerInterface = QStringLiteral("io.github.fapped.ShredGui.Progress");
const QString kPeerSignal = QStringLiteral("Update");
const QString kSelf; // this instance in m_combined

// KJob capabilities and error codes, as understood by the job view server.
constexpr int kCapabilityKillable = 0x0001;
constexpr uint kErrorNone = 0;
constexpr uint kErrorKilled = 1;
constexpr uint kErrorUserDefined = 100;

constexpr int kUpdateIntervalMs = 250;
constexpr int kHeartbeatIntervalMs = 1000; // well below CombinedProgress::StaleMs

} // namespace

DesktopProgress::DesktopProgress(QObject *parent)
    : QObject(parent)
{
    m_appUri = QStringLiteral("application://") + QGuiApplication::desktopFileName() + QStringLiteral(".desktop");
    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(kUpdateIntervalMs);
    connect(&m_updateTimer, &QTimer::timeout, this, &DesktopProgress::flush);
    m_heartbeatTimer.setInterval(kHeartbeatIntervalMs);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &DesktopProgress::heartbeat);
    m_clock.start();
    // From the start, also while idle: a job started later counts the ones already running.
    QDBusConnection::sessionBus().connect(QString(), kPeerPath, kPeerInterface, kPeerSignal, this,
                                          SLOT(onPeerUpdate(QDBusMessage)));
}

DesktopProgress::~DesktopProgress()
{
    if (m_active) {
        endSharing();
        // Blocking: the application may be quitting, an asynchronous call would be lost.
        terminateJob(kErrorKilled, QString(), true);
    }
}

void DesktopProgress::start(const QString &title, const QString &target, const QString &amount)
{
    if (m_active)
        finish(true);
    m_active = true;
    m_finishPending = false;
    m_requestPending = false;
    m_jobService.clear();
    m_jobPath.clear();
    m_progress = 0.0;
    m_processedBytes = 0;
    m_totalBytes = 0;
    m_progressChanged = true;
    m_elapsed.start();
    m_pending = {
        {QStringLiteral("title"), title},
        {QStringLiteral("descriptionLabel1"), tr("Target")},
        {QStringLiteral("descriptionValue1"), target},
        {QStringLiteral("descriptionLabel2"), tr("Amount")},
        {QStringLiteral("descriptionValue2"), amount},
        {QStringLiteral("percent"), 0u},
    };
    // The task manager entry waits for the reply to requestView(): with a job view Plasma shows the jobs
    // on the task manager itself, without one this instance does.
    m_ownLauncherEntry = false;
    broadcast(true);
    m_heartbeatTimer.start();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    QDBusMessage request = QDBusMessage::createMethodCall(kJobServer, kJobServerPath, kJobServerInterface,
                                                          QStringLiteral("requestView"));
    request << QGuiApplication::desktopFileName() << kCapabilityKillable << m_pending;
    m_requestPending = true;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        m_requestPending = false;
        const QDBusPendingReply<QDBusObjectPath> reply = *call;
        if (reply.isError() || !m_active) {
            // No job view server (not Plasma) - the task manager progress is shown by this instance.
            if (m_finishPending) {
                m_active = false;
            } else if (m_active) {
                m_ownLauncherEntry = true;
                publishLauncherEntry(true);
            }
            return;
        }
        // Plasma forwards the progress of the jobs to the task manager itself (the average of the
        // application's jobs, with their count) - a second value from here would fight with it.
        // The job views live on the connection that owns the server name (the reply's sender is not
        // available here), so the well-known name is used for them too.
        m_jobService = kJobServer;
        m_jobPath = reply.value().path();
        QDBusConnection::sessionBus().connect(m_jobService, m_jobPath, kJobInterface, QStringLiteral("cancelRequested"),
                                              this, SLOT(onCancelRequested()));
        if (m_finishPending) {
            sendUpdate({{QStringLiteral("infoMessage"), m_finishInfo}});
            terminateJob(m_finishCode, m_finishMessage, false);
            m_active = false;
            return;
        }
        flush();
    });
    // The initial properties went with the request.
    m_pending.clear();
}

void DesktopProgress::setTitle(const QString &title)
{
    if (!m_active)
        return;
    m_pending.insert(QStringLiteral("title"), title);
    scheduleUpdate();
}

void DesktopProgress::setInfo(const QString &info)
{
    if (!m_active)
        return;
    m_pending.insert(QStringLiteral("infoMessage"), info);
    scheduleUpdate();
}

void DesktopProgress::setProgress(int permille)
{
    if (!m_active)
        return;
    const double progress = qBound(0, permille, 1000) / 1000.0;
    if (progress == m_progress)
        return;
    m_progress = progress;
    m_progressChanged = true;
    m_pending.insert(QStringLiteral("percent"), uint(permille / 10));
    scheduleUpdate();
}

void DesktopProgress::setSpeed(qint64 bytesPerSecond, qint64 processedBytes)
{
    if (!m_active)
        return;
    m_pending.insert(QStringLiteral("speed"), qulonglong(qMax<qint64>(0, bytesPerSecond)));
    if (processedBytes >= 0) {
        m_processedBytes = processedBytes;
        m_pending.insert(QStringLiteral("processedBytes"), qulonglong(processedBytes));
    }
    scheduleUpdate();
}

void DesktopProgress::setTotalBytes(qint64 totalBytes)
{
    if (!m_active || totalBytes <= 0)
        return;
    m_totalBytes = totalBytes;
    m_progressChanged = true;
    m_pending.insert(QStringLiteral("totalBytes"), qulonglong(totalBytes));
    scheduleUpdate();
}

void DesktopProgress::finish(bool cancelled, const QString &errorMessage)
{
    if (!m_active)
        return;
    m_updateTimer.stop();
    m_pending.clear();
    m_progressChanged = false;
    endSharing();
    const uint code = cancelled ? kErrorKilled : errorMessage.isEmpty() ? kErrorNone : kErrorUserDefined;

    // The finished notification keeps the last info line - replace the progress (the current file)
    // with a summary: "24,0 GiB written in 0:00:57".
    const QString duration = QTime(0, 0).addMSecs(int(m_elapsed.elapsed())).toString(QStringLiteral("H:mm:ss"));
    if (cancelled)
        m_finishInfo = tr("Cancelled after %1").arg(duration);
    else if (!errorMessage.isEmpty())
        m_finishInfo = errorMessage;
    else if (m_processedBytes > 0)
        m_finishInfo = tr("%1 written in %2").arg(QLocale().formattedDataSize(m_processedBytes), duration);
    else
        m_finishInfo = tr("Done in %1").arg(duration);

    if (m_requestPending) {
        // The job view is still being created; it is terminated as soon as it exists.
        m_finishPending = true;
        m_finishCode = code;
        m_finishMessage = errorMessage;
        return;
    }
    sendUpdate({{QStringLiteral("infoMessage"), m_finishInfo}});
    terminateJob(code, errorMessage, false);
    m_active = false;
}

void DesktopProgress::onCancelRequested()
{
    emit cancelRequested();
}

void DesktopProgress::scheduleUpdate()
{
    if (!m_updateTimer.isActive())
        m_updateTimer.start();
}

void DesktopProgress::flush()
{
    if (!m_active)
        return;
    if (m_progressChanged) {
        broadcast(true);
        publishLauncherEntry(true);
        m_progressChanged = false;
    }
    if (m_jobPath.isEmpty() || m_pending.isEmpty())
        return;
    // Milliseconds since the start, like KJob::elapsedTime(). The keys of the map are sent in alphabetical
    // order, so Plasma has it before "processedBytes" when it computes the average speed.
    m_pending.insert(QStringLiteral("elapsedTime"), qlonglong(m_elapsed.elapsed()));
    sendUpdate(m_pending);
    m_pending.clear();
}

void DesktopProgress::sendUpdate(const QVariantMap &properties)
{
    if (m_jobPath.isEmpty())
        return;
    QDBusMessage update = QDBusMessage::createMethodCall(m_jobService, m_jobPath, kJobInterface, QStringLiteral("update"));
    update << properties;
    QDBusConnection::sessionBus().asyncCall(update);
}

void DesktopProgress::sendLauncherEntry(double progress, bool visible)
{
    QDBusMessage signal = QDBusMessage::createSignal(kLauncherPath, kLauncherInterface, QStringLiteral("Update"));
    signal << m_appUri
           << QVariantMap{{QStringLiteral("progress"), progress}, {QStringLiteral("progress-visible"), visible}};
    QDBusConnection::sessionBus().send(signal);
}

void DesktopProgress::terminateJob(uint errorCode, const QString &errorMessage, bool blocking)
{
    if (m_jobPath.isEmpty())
        return;
    QDBusMessage terminate = QDBusMessage::createMethodCall(m_jobService, m_jobPath, kJobInterface,
                                                            QStringLiteral("terminate"));
    terminate << errorCode << errorMessage << QVariantMap();
    if (blocking)
        QDBusConnection::sessionBus().call(terminate, QDBus::Block, 500);
    else
        QDBusConnection::sessionBus().asyncCall(terminate);
    QDBusConnection::sessionBus().disconnect(m_jobService, m_jobPath, kJobInterface, QStringLiteral("cancelRequested"),
                                             this, SLOT(onCancelRequested()));
    m_jobPath.clear();
}

void DesktopProgress::broadcast(bool running)
{
    QDBusMessage signal = QDBusMessage::createSignal(kPeerPath, kPeerInterface, kPeerSignal);
    signal << m_progress << qlonglong(m_totalBytes) << running;
    QDBusConnection::sessionBus().send(signal);
}

void DesktopProgress::publishLauncherEntry(bool includeSelf)
{
    if (!m_ownLauncherEntry)
        return;
    const qint64 now = m_clock.elapsed();
    m_combined.expire(now);
    if (includeSelf)
        m_combined.set(kSelf, m_progress, m_totalBytes, now);
    else
        m_combined.remove(kSelf);
    sendLauncherEntry(m_combined.value(), !m_combined.isEmpty());
}

void DesktopProgress::heartbeat()
{
    // Unlike a change of the progress, sent while the job view is being created or ending too.
    if (!m_active || m_finishPending) {
        m_heartbeatTimer.stop();
        return;
    }
    broadcast(true);
    publishLauncherEntry(true);
}

void DesktopProgress::endSharing()
{
    m_heartbeatTimer.stop();
    broadcast(false);
    // The task manager entry stays with the progress of the other instances, if any still run.
    publishLauncherEntry(false);
}

void DesktopProgress::onPeerUpdate(const QDBusMessage &message)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (message.service() == bus.baseService()) // this instance's own signal
        return;
    const QList<QVariant> args = message.arguments();
    if (args.size() < 3)
        return;
    if (args.at(2).toBool())
        m_combined.set(message.service(), args.at(0).toDouble(), args.at(1).toLongLong(), m_clock.elapsed());
    else
        m_combined.remove(message.service());
    // The sender updates the task manager entry itself (with this instance counted); nothing to send here.
}
