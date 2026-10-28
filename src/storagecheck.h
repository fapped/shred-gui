#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Checks whether overwriting a file in place really overwrites its old data on the storage device.
// It does not on copy-on-write and log-structured file systems, on flash storage (wear leveling,
// spare cells), and it cannot be verified on network, overlay and FUSE file systems.
namespace StorageCheck {

struct Info
{
    QString fileSystem; // "ext4", "btrfs", "fuse.sshfs", ... (empty if unknown)
    QString source;     // the mounted device or share, e.g. "/dev/mapper/root"
    QString mountPoint; // empty for a device given directly
    bool flash = false; // SSD, USB stick or memory card (or a device stacked on one)
};

// The storage of an existing file or directory (symlinks are not followed).
Info inspect(const QString &path);
// The storage of a block device itself (e.g. /dev/sdb selected as the target).
Info inspectDevice(quint32 major, quint32 minor);
// Warnings (one sentence each, plain text) for the storage; empty if overwriting in place works.
QStringList warnings(const Info &info);

// Internals, exposed for the tests.

// Finds the mount of the device major:minor in the content of /proc/self/mountinfo, or - for devices
// without an entry of their own, e.g. btrfs subvolumes - the mount with the longest prefix of `path`.
Info parseMountInfo(const QByteArray &mountInfo, quint32 major, quint32 minor, const QString &path);
// Whether the block device in sysfs (/sys/class/block/<name>, a partition or a whole disk) is flash storage.
// Devices stacked on others (device mapper: LUKS, LVM; RAID) are decided by the devices below them,
// loop devices by the storage of their backing file.
bool isFlashDevice(const QString &sysfsBlockDir);

} // namespace StorageCheck
