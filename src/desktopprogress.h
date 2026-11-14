#pragma once

#include "combinedprogress.h"

#include <QDBusMessage>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

// Shows the progress of shredding outside the window, over D-Bus:
// - as a job in the notification area (org.kde.JobViewServerV2, Plasma), with a cancel button;
//   Plasma also shows the jobs on the task manager entry (the average of all jobs of the application),
// - without a job view server (other desktops): on the task manager entry
//   (com.canonical.Unity.LauncherEntry), combined with the other running instances - the task manager
//   keeps one progress per application, so the instances share theirs over D-Bus and all of them show
//   the same, combined value there.
// Without these services (other desktops, no session bus) everything is a no-op.
class DesktopProgress : public QObject
{
    Q_OBJECT

public:
    explicit DesktopProgress(QObject *parent = nullptr);
    ~DesktopProgress() override; // an unfinished job is reported as cancelled

    // `target` and `amount` are shown as the "Target" and "Amount" lines of the job.
    void start(const QString &title, const QString &target, const QString &amount);
    void setTitle(const QString &title);
    void setInfo(const QString &info);
    void setProgress(int permille);
    // Numeric speed and amount written - Plasma draws the speed graph of the job from them.
    void setSpeed(qint64 bytesPerSecond, qint64 processedBytes);
    // The size of the whole job; Plasma's speed graph needs it (its x axis is the share of it).
    void setTotalBytes(qint64 totalBytes);
    // Hides the progress and ends the job (a non-empty error message marks it as failed).
    void finish(bool cancelled, const QString &errorMessage = QString());
    bool isActive() const { return m_active; }

signals:
    void cancelRequested();

private slots:
    void onCancelRequested();
    void onPeerUpdate(const QDBusMessage &message);

private:
    void scheduleUpdate();
    void flush();
    void sendLauncherEntry(double progress, bool visible);
    // Tells the other instances about this one's progress (`running` false: this one has ended).
    void broadcast(bool running);
    // Shows the combined progress of this instance (if `includeSelf`) and the other running ones
    // on the task manager entry, hidden when none is running.
    void publishLauncherEntry(bool includeSelf);
    void heartbeat();
    void endSharing(); // this instance stops contributing to the combined progress
    void terminateJob(uint errorCode, const QString &errorMessage, bool blocking);
    void sendUpdate(const QVariantMap &properties);

    QString m_appUri;       // application://shred-gui.desktop
    QString m_jobService;   // the owner of the job view, known after requestView() returns
    QString m_jobPath;
    QVariantMap m_pending;  // job properties not sent yet (updates are coalesced)
    QTimer m_updateTimer;
    QElapsedTimer m_elapsed; // Plasma's "Average Speed" is processedBytes / elapsedTime
    double m_progress = 0.0;
    qint64 m_processedBytes = 0; // the last amount sent, for the summary line at the end
    qint64 m_totalBytes = 0; // the weight of this job in the combined progress
    CombinedProgress m_combined; // this instance ("") and the other running ones (by D-Bus name)
    QElapsedTimer m_clock; // monotonic time for m_combined
    // Repeats this instance's progress while it runs: keeps it from expiring in the other instances
    // when nothing changes for a while, and drops the instances that stopped reporting.
    QTimer m_heartbeatTimer;
    bool m_progressChanged = false;
    bool m_active = false;
    bool m_requestPending = false; // requestView() has not returned yet
    bool m_ownLauncherEntry = false; // no job view server - this instance sends the task manager progress
    bool m_finishPending = false; // finish() was called before the job view was created
    uint m_finishCode = 0;
    QString m_finishMessage;
    QString m_finishInfo; // the summary line shown in the finished notification
};
