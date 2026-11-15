#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Erasing a whole device - the methods that destroy the data on flash storage, where overwriting files does not
// reach the old cells: the device's own erase commands (NVMe sanitize/format, ATA Secure Erase), destroying the keys
// of an encrypted volume (LUKS), and discarding all blocks (TRIM). They need administrator rights; the commands
// run as one POSIX shell script (`script()`) that checks the device again before it erases anything.
namespace DeviceErase {

enum class Method {
    NvmeSanitizeCrypto, // nvme sanitize -a 4: the controller destroys the media encryption key
    NvmeSanitizeBlock,  // nvme sanitize -a 2: the controller erases all blocks (also the spare ones)
    NvmeFormatCrypto,   // nvme format --ses=2: cryptographic erase as part of the format
    NvmeFormatUserData, // nvme format --ses=1: user data erase as part of the format
    AtaEnhanced,        // hdparm --security-erase-enhanced (all cells, also the reallocated ones)
    AtaNormal,          // hdparm --security-erase
    LuksErase,          // cryptsetup luksErase: all key slots are wiped, the data stays encrypted for good
    BlkdiscardSecure,   // blkdiscard --secure: secure discard (rarely supported)
    Blkdiscard,         // blkdiscard: TRIM of the whole device
};

struct Device
{
    QString name;      // "nvme0n1", "sda", "sda3"
    QString path;      // "/dev/nvme0n1"
    QString type;      // "disk", "part", "crypt", "lvm", ...
    QString transport; // "nvme", "sata", "usb", ... (of the disk, also for its partitions)
    QString model;
    QString serial;
    QString fileSystem; // "crypto_LUKS", "ext4", ...
    qint64 size = 0;
    bool rotational = false;
    bool removable = false;
    bool discard = false; // DISC-MAX > 0
    // Why the device cannot be erased now: mounted file systems, active swap, open devices on it (dm-crypt,
    // LVM, RAID) - of the device and of everything on it. Empty if it is not in use.
    QStringList inUse;
    QList<Device> children;

    bool isFlash() const { return !rotational || removable; }
};

// The lsblk command whose JSON output parseLsblk() reads.
QStringList lsblkArguments();
// The disks of "lsblk -J -b -o ..." (lsblkArguments()) with their partitions as children; loop, zram, rom and
// other virtual devices are left out. An error is set for output that is not lsblk's JSON.
QList<Device> parseLsblk(const QByteArray &json, QString *error = nullptr);

// The methods that apply to the device, the recommended first.
QList<Method> methods(const Device &device);
QString title(Method method);
// What the method does and what it does not guarantee (plain text, several sentences).
QString description(Method method);
// The program the method needs ("nvme", "hdparm", "cryptsetup", "blkdiscard").
QString program(Method method);
// The package that provides program(method) on Debian/Ubuntu.
QString package(Method method);

// The full paths of the programs (found also in /usr/sbin and /sbin, outside the PATH of users).
struct Tools
{
    QString lsblk;
    QString nvme;
    QString hdparm;
    QString cryptsetup;
    QString blkdiscard;

    static Tools find();
    QString path(Method method) const;
};

// The shell script that erases `device` with `method`. It first checks, as root, that the device is still the one
// that was selected (its size and serial number) and that nothing on it is mounted or open, and the method's own
// preconditions (e.g. that the drive supports the command, an ATA drive that is not frozen). Lines starting with
// "ERROR: " explain why it stopped; the exit code is 0 only after a successful erase.
QString script(Method method, const Device &device, const Tools &tools);
// The command that runs the script: "[pkexec] /bin/sh -c <script>" (pkexec left out when `elevate` is empty or
// this process is root).
QStringList command(const QString &script, const QString &elevate = QStringLiteral("pkexec"));

QString formatSize(qint64 bytes);

} // namespace DeviceErase
