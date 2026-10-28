#include "shredjob.h"
#include "localpath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <vector>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QString errnoString(int err)
{
    return QString::fromLocal8Bit(std::strerror(err));
}

// Recursively collects the content of a directory without following symlinks
// and without crossing into other file systems.
bool scanDirectory(const QByteArray &dir, dev_t rootDevice, ShredJob::Plan &plan)
{
    DIR *d = ::opendir(dir.constData());
    if (!d) {
        const int err = errno;
        plan.error = QObject::tr("Cannot read directory \"%1\": %2")
                         .arg(LocalPath::display(LocalPath::decode(dir)), errnoString(err));
        return false;
    }

    QList<QByteArray> subdirs;
    errno = 0;
    while (dirent *entry = ::readdir(d)) {
        const char *name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
            continue;
        const QByteArray path = dir + '/' + name;
        struct stat st;
        if (::lstat(path.constData(), &st) != 0) {
            const int err = errno;
            plan.error = QObject::tr("Cannot read \"%1\": %2")
                             .arg(LocalPath::display(LocalPath::decode(path)), errnoString(err));
            ::closedir(d);
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != rootDevice)
                plan.skipped << LocalPath::decode(path);
            else
                subdirs << path;
        } else if (S_ISREG(st.st_mode)) {
            plan.files << LocalPath::decode(path);
        } else {
            // Symlinks are deliberately NOT passed to shred - it would destroy the file they point to.
            plan.others << LocalPath::decode(path);
        }
    }
    const int readErr = errno;
    ::closedir(d);
    if (readErr != 0) {
        plan.error = QObject::tr("Error reading directory \"%1\": %2")
                         .arg(LocalPath::display(LocalPath::decode(dir)), errnoString(readErr));
        return false;
    }

    std::sort(subdirs.begin(), subdirs.end());
    for (const QByteArray &sub : std::as_const(subdirs)) {
        if (!scanDirectory(sub, rootDevice, plan))
            return false;
    }
    plan.dirs << LocalPath::decode(dir);
    return true;
}

bool isEmptyDirectory(const QByteArray &path)
{
    DIR *d = ::opendir(path.constData());
    if (!d)
        return false;
    bool empty = true;
    while (dirent *entry = ::readdir(d)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    ::closedir(d);
    return empty;
}

// Obfuscates the name like shred --remove=wipe: successive renames to strings of zeros of decreasing length.
QByteArray wipeName(const QByteArray &path, bool sync)
{
    const int slash = path.lastIndexOf('/');
    const QByteArray parent = slash > 0 ? path.left(slash) : QByteArray("/");
    QByteArray current = path;
    const int fd = sync ? ::open(parent.constData(), O_RDONLY | O_DIRECTORY) : -1;
    for (qsizetype len = path.size() - slash - 1; len > 0; --len) {
        const QByteArray candidate = parent + '/' + QByteArray(len, '0');
        if (candidate == current)
            continue;
        struct stat st;
        if (::lstat(candidate.constData(), &st) == 0)
            continue;
        if (::rename(current.constData(), candidate.constData()) == 0) {
            current = candidate;
            if (fd >= 0)
                ::fsync(fd);
        }
    }
    if (fd >= 0)
        ::close(fd);
    return current;
}

} // namespace

ShredJob::TargetType ShredJob::targetType(const QString &path)
{
    if (path.isEmpty())
        return TargetType::Missing;
    struct stat st;
    if (::lstat(LocalPath::encode(path).constData(), &st) != 0)
        return TargetType::Missing;
    if (S_ISLNK(st.st_mode))
        return TargetType::Symlink;
    if (S_ISDIR(st.st_mode))
        return TargetType::Directory;
    if (S_ISREG(st.st_mode))
        return TargetType::File;
    return TargetType::Special;
}

bool ShredJob::isProtectedPath(const QString &path)
{
    QString canonical = LocalPath::canonical(path);
    if (canonical.isEmpty())
        canonical = QDir::cleanPath(path);
    static const QStringList systemDirs{
        QStringLiteral("/"),     QStringLiteral("/bin"),   QStringLiteral("/boot"),
        QStringLiteral("/dev"),  QStringLiteral("/etc"),   QStringLiteral("/home"),
        QStringLiteral("/lib"),  QStringLiteral("/lib32"), QStringLiteral("/lib64"),
        QStringLiteral("/media"), QStringLiteral("/mnt"),  QStringLiteral("/opt"),
        QStringLiteral("/proc"), QStringLiteral("/root"),  QStringLiteral("/run"),
        QStringLiteral("/sbin"), QStringLiteral("/srv"),   QStringLiteral("/sys"),
        QStringLiteral("/tmp"),  QStringLiteral("/usr"),   QStringLiteral("/var"),
    };
    if (systemDirs.contains(canonical))
        return true;
    const QString home = LocalPath::canonical(QDir::homePath());
    return !home.isEmpty() && canonical == home;
}

ShredJob::Plan ShredJob::scan(const QString &target)
{
    Plan plan;
    plan.target = target.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(target).absoluteFilePath());
    plan.type = targetType(plan.target);

    switch (plan.type) {
    case TargetType::Missing:
        plan.error = QObject::tr("Path \"%1\" does not exist.").arg(LocalPath::display(target));
        break;
    case TargetType::Symlink:
        plan.error = QObject::tr("\"%1\" is a symbolic link. Select the target file or directory directly.")
                         .arg(LocalPath::display(plan.target));
        break;
    case TargetType::File:
    case TargetType::Special:
        plan.files << plan.target;
        break;
    case TargetType::Directory: {
        if (isProtectedPath(plan.target)) {
            plan.error = QObject::tr("Directory \"%1\" is protected (root, home or system directory).")
                             .arg(LocalPath::display(plan.target));
            break;
        }
        const QByteArray root = LocalPath::encode(plan.target);
        struct stat st;
        if (::lstat(root.constData(), &st) != 0) {
            const int err = errno;
            plan.error = QObject::tr("Cannot read \"%1\": %2").arg(LocalPath::display(plan.target), errnoString(err));
            break;
        }
        scanDirectory(root, st.st_dev, plan);
        break;
    }
    }
    return plan;
}

QList<ShredJob::Plan> ShredJob::scanAll(const QStringList &targets, QStringList *notes)
{
    QList<Plan> plans;
    QStringList canonicalPaths;
    for (const QString &target : targets) {
        Plan plan = scan(target);
        const QString canonical = LocalPath::canonical(plan.target);
        if (!canonical.isEmpty() && canonicalPaths.contains(canonical)) {
            if (notes)
                *notes << tr("Duplicate target skipped: %1").arg(LocalPath::display(plan.target));
            continue;
        }
        canonicalPaths << canonical;
        plans << std::move(plan);
    }

    // A target inside another directory target would be processed twice - keep only the outer one.
    QList<Plan> result;
    for (qsizetype i = 0; i < plans.size(); ++i) {
        const QString inner = canonicalPaths.at(i);
        QString outer;
        for (qsizetype j = 0; j < plans.size() && outer.isEmpty(); ++j) {
            if (i != j && plans.at(j).isDirectory() && plans.at(j).error.isEmpty() && !inner.isEmpty()
                && inner.startsWith(canonicalPaths.at(j) + QLatin1Char('/'))) {
                outer = plans.at(j).target;
            }
        }
        if (outer.isEmpty())
            result << plans.at(i);
        else if (notes)
            *notes << tr("%1 is inside %2 and will be processed as part of it.")
                          .arg(LocalPath::display(plans.at(i).target), LocalPath::display(outer));
    }
    return result;
}

ShredJob::ShredJob(Plan plan, ShredOptions options, QObject *parent)
    : ShredJob(QList<Plan>{std::move(plan)}, std::move(options), parent)
{
}

ShredJob::ShredJob(QList<Plan> plans, ShredOptions options, QObject *parent)
    : QObject(parent)
    , m_plans(std::move(plans))
    , m_options(std::move(options))
{
    // Each file is one step, plus one step per target for removing symlinks and directories.
    for (const Plan &plan : std::as_const(m_plans))
        m_totalSteps += plan.files.size() + 1;
}

ShredJob::~ShredJob()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void ShredJob::start()
{
    if (m_running)
        return;
    m_running = true;
    m_planIndex = 0;
    m_fileIndex = 0;
    m_doneSteps = 0;
    m_planAnnounced = false;
    m_summary = {};
    m_cancelRequested = false;
    emit progress(0);
    startNext();
}

void ShredJob::cancel()
{
    if (!m_running)
        return;
    m_cancelRequested = true;
    emit log(LogLevel::Warning, tr("Cancelling..."));
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        QPointer<QProcess> process = m_process;
        QTimer::singleShot(3000, this, [process] {
            if (process && process->state() != QProcess::NotRunning)
                process->kill();
        });
    }
}

void ShredJob::startNext()
{
    while (!m_cancelRequested && m_planIndex < m_plans.size()) {
        const Plan &plan = m_plans.at(m_planIndex);
        if (!m_planAnnounced) {
            m_planAnnounced = true;
            if (m_plans.size() > 1)
                emit log(LogLevel::Info, tr("--- Target %1/%2: %3 ---").arg(m_planIndex + 1).arg(m_plans.size()).arg(LocalPath::display(plan.target)));
        }
        if (m_fileIndex < plan.files.size()) {
            startProcess(plan.files.at(m_fileIndex));
            return;
        }
        removeLeftovers(plan);
        ++m_doneSteps;
        ++m_planIndex;
        m_fileIndex = 0;
        m_planAnnounced = false;
    }
    m_summary.cancelled = m_cancelRequested;
    finish();
}

void ShredJob::startProcess(const QString &file)
{
    m_currentFile = file;
    emit currentFileChanged(file);
    emitProgress(0.0);

    const QStringList args = m_options.arguments(file);
    emit log(LogLevel::Info, QStringLiteral("$ ") + commandLine(m_options.program, args));

    m_pending.clear();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &ShredJob::onReadyRead);
    connect(m_process, &QProcess::finished, this, &ShredJob::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &ShredJob::onProcessError);
    setRawArguments(m_process, args);
    m_process->start(m_options.program, args);
}

void ShredJob::onReadyRead()
{
    if (!m_process)
        return;
    m_pending += m_process->readAll();
    static const QRegularExpression passRe(QStringLiteral("(\\d+)/(\\d+)"));
    qsizetype nl;
    while ((nl = m_pending.indexOf('\n')) >= 0) {
        // shred prints the file name as it is - keep bytes that are not valid UTF-8 readable.
        const QString line = LocalPath::display(LocalPath::decode(m_pending.left(nl))).trimmed();
        m_pending.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        emit log(LogLevel::Output, line);
        // "shred: FILE: pass 2/4 (random)..." - progress is taken from the last segment only,
        // because the path itself may contain digits and slashes.
        const QString tail = line.mid(line.lastIndexOf(QStringLiteral(": ")) + 1);
        const QRegularExpressionMatch m = passRe.match(tail);
        if (m.hasMatch()) {
            const int pass = m.captured(1).toInt();
            const int total = m.captured(2).toInt();
            if (total > 0 && pass > 0 && pass <= total)
                emitProgress(double(pass - 1) / total);
        }
    }
}

void ShredJob::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process)
        return;
    onReadyRead();
    if (!m_pending.trimmed().isEmpty())
        emit log(LogLevel::Output, LocalPath::display(LocalPath::decode(m_pending)).trimmed());
    m_pending.clear();

    if (status == QProcess::NormalExit && exitCode == 0) {
        ++m_summary.filesShredded;
    } else {
        ++m_summary.filesFailed;
        if (m_cancelRequested)
            emit log(LogLevel::Warning, tr("Cancelled: %1 (the file may have been only partially overwritten)")
                                          .arg(LocalPath::display(m_currentFile)));
        else if (status == QProcess::CrashExit)
            emit log(LogLevel::Error, tr("shred crashed: %1").arg(LocalPath::display(m_currentFile)));
        else
            emit log(LogLevel::Error, tr("Failed (exit code %1): %2").arg(exitCode).arg(LocalPath::display(m_currentFile)));
    }

    m_process->deleteLater();
    m_process = nullptr;
    ++m_fileIndex;
    ++m_doneSteps;
    startNext();
}

void ShredJob::onProcessError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || !m_process)
        return;
    emit log(LogLevel::Error, tr("Failed to start \"%1\": %2")
                                  .arg(m_options.program, m_process->errorString()));
    m_process->deleteLater();
    m_process = nullptr;
    m_summary.aborted = true;
    finish();
}

void ShredJob::removeLeftovers(const Plan &plan)
{
    if (!plan.isDirectory())
        return;
    if (!m_options.removesFiles()) {
        emit log(LogLevel::Warning, tr("File removal is disabled - directory \"%1\" stays on disk.")
                                        .arg(LocalPath::display(plan.target)));
        return;
    }

    for (const QString &path : std::as_const(plan.others)) {
        if (::unlink(LocalPath::encode(path).constData()) == 0) {
            ++m_summary.othersRemoved;
            emit log(LogLevel::Info, tr("Removed (not overwritten - symlink or special file): %1").arg(LocalPath::display(path)));
        } else {
            const int err = errno;
            ++m_summary.othersFailed;
            emit log(LogLevel::Error, tr("Cannot remove \"%1\": %2").arg(LocalPath::display(path), errnoString(err)));
        }
    }

    for (const QString &path : std::as_const(plan.skipped))
        emit log(LogLevel::Warning, tr("Skipped mount point of another file system: %1").arg(LocalPath::display(path)));

    for (const QString &dir : std::as_const(plan.dirs)) {
        QByteArray path = LocalPath::encode(dir);
        if (!isEmptyDirectory(path)) {
            ++m_summary.dirsFailed;
            emit log(LogLevel::Error, tr("Directory is not empty or cannot be read - left in place: %1").arg(LocalPath::display(dir)));
            continue;
        }
        if (m_options.wipesNames())
            path = wipeName(path, m_options.syncsNames());
        if (::rmdir(path.constData()) == 0) {
            ++m_summary.dirsRemoved;
            emit log(LogLevel::Info, tr("Removed directory: %1").arg(LocalPath::display(dir)));
        } else {
            const int err = errno;
            ++m_summary.dirsFailed;
            emit log(LogLevel::Error, tr("Cannot remove directory \"%1\" (%2): %3")
                                          .arg(LocalPath::display(dir), LocalPath::display(LocalPath::decode(path)), errnoString(err)));
        }
    }
}

void ShredJob::finish()
{
    m_running = false;
    m_currentFile.clear();
    emit currentFileChanged(QString());

    const Summary &s = m_summary;
    QString text = tr("Files shredded: %1, failed: %2").arg(s.filesShredded).arg(s.filesFailed);
    const bool anyDirectory = std::any_of(m_plans.cbegin(), m_plans.cend(),
                                          [](const Plan &plan) { return plan.isDirectory(); });
    if (anyDirectory) {
        text += tr("; other entries removed: %1 (failed: %2); directories removed: %3 (failed: %4)")
                    .arg(s.othersRemoved).arg(s.othersFailed).arg(s.dirsRemoved).arg(s.dirsFailed);
    }
    if (s.cancelled)
        emit log(LogLevel::Warning, tr("Cancelled by user. ") + text);
    else if (s.aborted)
        emit log(LogLevel::Error, tr("Operation aborted. ") + text);
    else if (s.ok())
        emit log(LogLevel::Success, tr("Finished successfully. ") + text);
    else
        emit log(LogLevel::Error, tr("Finished with errors. ") + text);

    if (s.ok())
        emit progress(1000);
    emit finished();
}

void ShredJob::setRawArguments(QProcess *process, const QStringList &args) const
{
    // QProcess passes arguments as UTF-8, which would change file names that are not valid UTF-8
    // (see LocalPath). For such arguments the child process execs shred itself with the exact bytes;
    // if that fails, QProcess continues with its own exec, which then fails the same way.
    if (std::all_of(args.cbegin(), args.cend(), [](const QString &arg) { return LocalPath::isPlain(arg); }))
        return;
    const QString program = m_options.program.contains(QLatin1Char('/'))
        ? m_options.program
        : QStandardPaths::findExecutable(m_options.program);
    if (program.isEmpty())
        return;
    auto bytes = std::make_shared<std::vector<QByteArray>>();
    bytes->push_back(LocalPath::encode(program));
    for (const QString &arg : args)
        bytes->push_back(LocalPath::encode(arg));
    auto argv = std::make_shared<std::vector<char *>>();
    for (QByteArray &arg : *bytes)
        argv->push_back(arg.data()); // detached here, in the parent
    argv->push_back(nullptr);
    // Runs in the child after fork(); only async-signal-safe calls.
    process->setChildProcessModifier([bytes, argv] { ::execv(argv->front(), argv->data()); });
}

void ShredJob::emitProgress(double currentFraction)
{
    if (m_totalSteps > 0)
        emit progress(int(1000.0 * (m_doneSteps + currentFraction) / m_totalSteps));
}
