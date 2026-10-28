#pragma once

#include "shredoptions.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <atomic>

// Runs shred on files, or recursively on the whole content of directories,
// and then removes the emptied directories (deepest first, the target last).
// Targets are processed one after another, one shred process per file.
class ShredJob : public QObject
{
    Q_OBJECT

public:
    enum class LogLevel { Info, Output, Warning, Error, Success };
    Q_ENUM(LogLevel)

    enum class TargetType { Missing, File, Directory, Symlink, Special };

    // Result of scanning the target - what exactly will be shredded/removed.
    struct Plan
    {
        QString target;
        TargetType type = TargetType::Missing;
        QStringList files;   // regular files passed to shred
        QList<qint64> fileSizes; // the sizes of `files` (0 for special files)
        QStringList others;  // symlinks, FIFOs, sockets, devices - unlink only
        QStringList dirs;    // directories to remove, deepest first, the target last
        QStringList skipped; // mount points of other file systems - skipped
        QStringList hardLinked; // files of `files` with other hard links (names elsewhere)
        QStringList storageWarnings; // where overwriting in place does not reach the old data (see StorageCheck)
        QString error;       // non-empty if the target cannot be processed

        bool isDirectory() const { return type == TargetType::Directory; }
    };

    struct Summary
    {
        int filesShredded = 0;
        int filesFailed = 0;
        int othersRemoved = 0;
        int othersFailed = 0;
        int dirsRemoved = 0;
        int dirsFailed = 0;
        bool cancelled = false;
        bool aborted = false; // e.g. shred could not be started

        bool ok() const
        {
            return !cancelled && !aborted && filesFailed == 0 && othersFailed == 0 && dirsFailed == 0;
        }
    };

    // Lets another thread follow and cancel a scan.
    struct ScanControl
    {
        std::atomic<bool> cancelled{false};
        std::atomic<qint64> entries{0}; // directory entries read so far
    };

    static TargetType targetType(const QString &path);
    static Plan scan(const QString &target, ScanControl *control = nullptr);
    // Scans several targets, skipping duplicates and targets nested inside another directory target
    // (described in `notes`). Plans with a non-empty error are kept so that the caller can report them.
    // Returns an empty list if the scan was cancelled through `control`.
    static QList<Plan> scanAll(const QStringList &targets, QStringList *notes = nullptr,
                               ScanControl *control = nullptr);
    // Paths that must never be processed recursively (/, the home directory, system directories).
    static bool isProtectedPath(const QString &path);

    ShredJob(Plan plan, ShredOptions options, QObject *parent = nullptr);
    ShredJob(QList<Plan> plans, ShredOptions options, QObject *parent = nullptr);
    ~ShredJob() override;

    void start();
    void cancel();
    bool isRunning() const { return m_running; }
    const Summary &summary() const { return m_summary; }
    const QList<Plan> &plans() const { return m_plans; }
    // The amount shred writes: the size of all files times the number of passes (an estimate with -s).
    qint64 totalBytes() const { return m_totalBytes; }

    // The number of bytes the process has written so far (the "wchar" line of /proc/<pid>/io), -1 if unknown.
    static qint64 writtenBytes(qint64 pid);
    static constexpr int WrittenBytesIntervalMs = 500;

signals:
    void log(ShredJob::LogLevel level, const QString &text);
    void progress(int permille);
    // The amount written of totalBytes() and the current speed (over the last few seconds).
    void processedBytesChanged(qint64 bytes, qint64 bytesPerSecond);
    void currentFileChanged(const QString &path);
    void finished();

private:
    void startNext();
    void startProcess(const QString &file);
    void setRawArguments(QProcess *process, const QStringList &args) const;
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void removeLeftovers(const Plan &plan);
    void finish();
    void emitProgress(double currentFraction);
    void pollWrittenBytes();
    qint64 updateRate(qint64 processed);
    qint64 currentFileBytes() const; // what shred writes for the current file

    QList<Plan> m_plans;
    ShredOptions m_options;
    Summary m_summary;
    QPointer<QProcess> m_process;
    QByteArray m_pending;
    QString m_currentFile;
    qsizetype m_planIndex = 0;
    qsizetype m_fileIndex = 0;
    qsizetype m_doneSteps = 0;
    qsizetype m_totalSteps = 0;
    qint64 m_totalBytes = 0;
    qint64 m_doneBytes = 0; // written for the finished files
    double m_fileFraction = 0.0; // of the current file, never decreasing
    // shred reports only the start of each pass, so the amount written by the running process is read
    // from /proc every WrittenBytesIntervalMs.
    QTimer m_pollTimer;
    QElapsedTimer m_clock;
    QList<QPair<qint64, qint64>> m_rateSamples; // (ms since start, bytes processed)
    qint64 m_rate = 0;
    bool m_planAnnounced = false;
    bool m_running = false;
    bool m_cancelRequested = false;
};
