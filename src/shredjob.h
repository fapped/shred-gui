#pragma once

#include "shredoptions.h"

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>

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

    static TargetType targetType(const QString &path);
    static Plan scan(const QString &target);
    // Scans several targets, skipping duplicates and targets nested inside another directory target
    // (described in `notes`). Plans with a non-empty error are kept so that the caller can report them.
    static QList<Plan> scanAll(const QStringList &targets, QStringList *notes = nullptr);
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

signals:
    void log(ShredJob::LogLevel level, const QString &text);
    void progress(int permille);
    void processedBytesChanged(qint64 bytes); // of totalBytes(), estimated from the passes
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
    bool m_planAnnounced = false;
    bool m_running = false;
    bool m_cancelRequested = false;
};
