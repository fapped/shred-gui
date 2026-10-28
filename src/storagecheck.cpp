#include "storagecheck.h"
#include "localpath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace StorageCheck {

namespace {

// Devices stacked deeper than this (or loops of loop devices) are not followed.
constexpr int kMaxDepth = 8;

const QStringList kCopyOnWrite{QStringLiteral("btrfs"), QStringLiteral("zfs"), QStringLiteral("bcachefs")};
const QStringList kLogStructured{QStringLiteral("f2fs"), QStringLiteral("nilfs2"), QStringLiteral("jffs2"),
                                 QStringLiteral("ubifs")};
const QStringList kNetwork{QStringLiteral("nfs"),        QStringLiteral("nfs4"),       QStringLiteral("cifs"),
                           QStringLiteral("smb3"),       QStringLiteral("smbfs"),      QStringLiteral("9p"),
                           QStringLiteral("ceph"),       QStringLiteral("glusterfs"),  QStringLiteral("afs"),
                           QStringLiteral("davfs"),      QStringLiteral("fuse.sshfs"), QStringLiteral("fuse.rclone"),
                           QStringLiteral("fuse.s3fs"),  QStringLiteral("fuse.gvfsd-fuse")};
const QStringList kOverlay{QStringLiteral("overlay"), QStringLiteral("aufs"), QStringLiteral("unionfs")};

// "/media/my\040disk" -> "/media/my disk" (mountinfo escapes space, tab, newline and backslash as octal).
QByteArray unescape(const QByteArray &field)
{
    QByteArray result;
    result.reserve(field.size());
    for (qsizetype i = 0; i < field.size(); ++i) {
        if (field.at(i) == '\\' && i + 3 < field.size()) {
            bool ok = false;
            const int value = field.mid(i + 1, 3).toInt(&ok, 8);
            if (ok && value < 256) {
                result += char(value);
                i += 3;
                continue;
            }
        }
        result += field.at(i);
    }
    return result;
}

bool isUnder(const QString &path, const QString &mountPoint)
{
    return mountPoint == QLatin1String("/") || path == mountPoint || path.startsWith(mountPoint + QLatin1Char('/'));
}

QByteArray readSysfs(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll().trimmed();
}

QString sysfsDirOfDevice(quint32 major, quint32 minor)
{
    const QString dir = QStringLiteral("/sys/dev/block/%1:%2").arg(major).arg(minor);
    return QFileInfo::exists(dir) ? dir : QString();
}

bool flashDevice(const QString &sysfsBlockDir, int depth);

Info inspectPath(const QString &path, int depth)
{
    Info info;
    struct stat st;
    if (depth > kMaxDepth || ::lstat(LocalPath::encode(path).constData(), &st) != 0)
        return info;
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (file.open(QIODevice::ReadOnly)) {
        QString canonical = LocalPath::canonical(path);
        if (canonical.isEmpty())
            canonical = path;
        info = parseMountInfo(file.readAll(), ::major(st.st_dev), ::minor(st.st_dev), canonical);
    }
    // Disk file systems have a block device; btrfs has an anonymous device number, but its source is a device.
    QString sysfsDir = sysfsDirOfDevice(::major(st.st_dev), ::minor(st.st_dev));
    if (sysfsDir.isEmpty() && info.source.startsWith(QLatin1String("/dev/"))) {
        const QString device = QFileInfo(info.source).canonicalFilePath();
        if (!device.isEmpty())
            sysfsDir = QStringLiteral("/sys/class/block/") + QFileInfo(device).fileName();
    }
    if (!sysfsDir.isEmpty())
        info.flash = flashDevice(sysfsDir, depth);
    return info;
}

bool flashDevice(const QString &sysfsBlockDir, int depth)
{
    const QString dir = QFileInfo(sysfsBlockDir).canonicalFilePath();
    if (dir.isEmpty() || depth > kMaxDepth)
        return false;

    // A device mapper or RAID device: what matters is where the data really is. Its own "rotational" flag
    // does not tell (e.g. dm-crypt reports 0 even on a hard disk with some kernels).
    const QStringList slaves = QDir(dir + QStringLiteral("/slaves")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!slaves.isEmpty()) {
        for (const QString &slave : slaves) {
            if (flashDevice(dir + QStringLiteral("/slaves/") + slave, depth + 1))
                return true;
        }
        return false;
    }
    const QByteArray backingFile = readSysfs(dir + QStringLiteral("/loop/backing_file"));
    if (!backingFile.isEmpty())
        return inspectPath(LocalPath::decode(backingFile), depth + 1).flash;

    // A partition has no queue of its own and is not "removable" - the whole disk (its parent) is.
    const QString parent = QFileInfo(dir).path();
    auto attribute = [&](const QString &name) {
        const QByteArray own = readSysfs(dir + name);
        return own.isEmpty() ? readSysfs(parent + name) : own;
    };
    // USB sticks and memory card readers often claim to be rotational, but they are removable.
    return attribute(QStringLiteral("/queue/rotational")) == "0" || attribute(QStringLiteral("/removable")) == "1";
}

} // namespace

Info parseMountInfo(const QByteArray &mountInfo, quint32 major, quint32 minor, const QString &path)
{
    const QByteArray device = QByteArray::number(major) + ':' + QByteArray::number(minor);
    Info byDevice;
    Info byPrefix;
    for (const QByteArray &line : mountInfo.split('\n')) {
        // "36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue"
        const QList<QByteArray> fields = line.split(' ');
        const qsizetype separator = fields.indexOf("-", 6);
        if (fields.size() < 5 || separator < 0 || separator + 2 >= fields.size())
            continue;
        Info info;
        info.mountPoint = LocalPath::decode(unescape(fields.at(4)));
        info.fileSystem = QString::fromUtf8(unescape(fields.at(separator + 1)));
        info.source = LocalPath::decode(unescape(fields.at(separator + 2)));
        if (!isUnder(path, info.mountPoint))
            continue;
        // Later lines are mounted over the earlier ones; the longest mount point is the innermost one.
        if (fields.at(2) == device && info.mountPoint.size() >= byDevice.mountPoint.size())
            byDevice = info;
        if (info.mountPoint.size() >= byPrefix.mountPoint.size())
            byPrefix = info;
    }
    return byDevice.fileSystem.isEmpty() ? byPrefix : byDevice;
}

bool isFlashDevice(const QString &sysfsBlockDir)
{
    return flashDevice(sysfsBlockDir, 0);
}

Info inspect(const QString &path)
{
    return inspectPath(path, 0);
}

Info inspectDevice(quint32 major, quint32 minor)
{
    Info info;
    const QString dir = sysfsDirOfDevice(major, minor);
    if (dir.isEmpty())
        return info;
    info.source = QStringLiteral("/dev/") + QFileInfo(QFileInfo(dir).canonicalFilePath()).fileName();
    info.flash = isFlashDevice(dir);
    return info;
}

QStringList warnings(const Info &info)
{
    const QString name = info.mountPoint.isEmpty()
                             ? info.source
                             : QStringLiteral("%1 (%2)").arg(LocalPath::display(info.mountPoint), info.fileSystem);
    const QString &fs = info.fileSystem;
    QStringList result;
    if (kCopyOnWrite.contains(fs)) {
        result << QObject::tr("%1: copy-on-write file system - the new data is written to other blocks, "
                              "the old content stays on the disk (and in snapshots, if there are any).")
                      .arg(name);
    } else if (kLogStructured.contains(fs)) {
        result << QObject::tr("%1: log-structured file system - the new data is written to other blocks, "
                              "the old content stays on the disk until the space is reused.")
                      .arg(name);
    } else if (kNetwork.contains(fs)) {
        result << QObject::tr("%1: network file system - whether the data is overwritten on the server "
                              "depends on its storage (copy-on-write, snapshots, backups).")
                      .arg(name);
    } else if (kOverlay.contains(fs)) {
        result << QObject::tr("%1: overlay file system - a file of a lower layer is copied up before it is "
                              "changed, the original stays untouched.")
                      .arg(name);
    } else if (fs == QLatin1String("fuse") || fs.startsWith(QLatin1String("fuse."))) {
        result << QObject::tr("%1: FUSE file system - whether the data is overwritten in place depends on "
                              "its implementation and on the storage below it.")
                      .arg(name);
    }
    if (info.flash) {
        result << QObject::tr("%1: flash storage (SSD, USB stick or memory card) - its controller writes the new "
                              "data to other cells, the old content can stay readable on the chips. Encryption "
                              "or the secure erase of the whole device protect reliably.")
                      .arg(name);
    }
    return result;
}

} // namespace StorageCheck
