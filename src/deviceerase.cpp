#include "deviceerase.h"
#include "shredoptions.h" // shellQuote

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QObject>
#include <QStandardPaths>

#include <unistd.h>

namespace DeviceErase {

namespace {

// lsblk prints numbers and booleans as JSON types (util-linux 2.33+) or as strings (older versions).
qint64 jsonNumber(const QJsonValue &value)
{
    if (value.isDouble())
        return qint64(value.toDouble());
    return value.toString().trimmed().toLongLong();
}

bool jsonBool(const QJsonValue &value)
{
    if (value.isBool())
        return value.toBool();
    if (value.isDouble())
        return value.toDouble() != 0;
    const QString text = value.toString().trimmed();
    return text == QLatin1String("1") || text == QLatin1String("true");
}

QString jsonString(const QJsonValue &value)
{
    return value.isString() ? value.toString().trimmed() : QString();
}

bool isHolderType(const QString &type)
{
    // Devices made from others: dm-crypt, LVM, multipath, RAID ("raid1", "md"...).
    return type == QLatin1String("crypt") || type == QLatin1String("lvm") || type == QLatin1String("dm")
        || type == QLatin1String("mpath") || type.startsWith(QLatin1String("raid")) || type == QLatin1String("md");
}

Device parseDevice(const QJsonObject &object, const Device *parent)
{
    Device device;
    device.name = jsonString(object.value(QStringLiteral("name")));
    device.path = jsonString(object.value(QStringLiteral("path")));
    if (device.path.isEmpty() && !device.name.isEmpty())
        device.path = QStringLiteral("/dev/") + device.name;
    device.type = jsonString(object.value(QStringLiteral("type")));
    device.transport = jsonString(object.value(QStringLiteral("tran")));
    device.model = jsonString(object.value(QStringLiteral("model")));
    device.serial = jsonString(object.value(QStringLiteral("serial")));
    device.fileSystem = jsonString(object.value(QStringLiteral("fstype")));
    device.size = jsonNumber(object.value(QStringLiteral("size")));
    device.rotational = jsonBool(object.value(QStringLiteral("rota")));
    device.removable = jsonBool(object.value(QStringLiteral("rm")));
    device.discard = jsonNumber(object.value(QStringLiteral("disc-max"))) > 0;
    if (parent) {
        // Partitions have no transport, model or serial number of their own.
        if (device.transport.isEmpty())
            device.transport = parent->transport;
        if (device.model.isEmpty())
            device.model = parent->model;
        device.removable = device.removable || parent->removable;
    }

    // "mountpoint" (all versions) or "mountpoints" (2.37+, an array).
    QStringList mountPoints;
    const QJsonValue single = object.value(QStringLiteral("mountpoint"));
    if (single.isString() && !single.toString().isEmpty())
        mountPoints << single.toString();
    for (const QJsonValue &value : object.value(QStringLiteral("mountpoints")).toArray()) {
        if (value.isString() && !value.toString().isEmpty() && !mountPoints.contains(value.toString()))
            mountPoints << value.toString();
    }
    for (const QString &mountPoint : std::as_const(mountPoints)) {
        device.inUse << (mountPoint == QLatin1String("[SWAP]")
                             ? QObject::tr("%1 is active swap").arg(device.path)
                             : QObject::tr("%1 is mounted at %2").arg(device.path, mountPoint));
    }

    for (const QJsonValue &value : object.value(QStringLiteral("children")).toArray()) {
        const Device child = parseDevice(value.toObject(), &device);
        if (isHolderType(child.type))
            device.inUse << QObject::tr("%1 is open as %2 (%3)").arg(device.path, child.name, child.type);
        for (const QString &reason : child.inUse) {
            if (!device.inUse.contains(reason))
                device.inUse << reason;
        }
        if (child.type == QLatin1String("part"))
            device.children << child;
    }
    return device;
}

QString q(const QString &text)
{
    return shellQuote(text);
}

QString findTool(const QString &name)
{
    QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(name, {QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
                                                     QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    return path;
}

} // namespace

QStringList lsblkArguments()
{
    return {QStringLiteral("-J"), QStringLiteral("-b"), QStringLiteral("-o"),
            QStringLiteral("NAME,PATH,TYPE,TRAN,MODEL,SERIAL,SIZE,ROTA,RM,FSTYPE,MOUNTPOINT,DISC-MAX")};
}

QList<Device> parseLsblk(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !document.object().value(QStringLiteral("blockdevices")).isArray()) {
        if (error)
            *error = QObject::tr("Unexpected output of lsblk: %1").arg(parseError.errorString());
        return {};
    }
    if (error)
        error->clear();
    QList<Device> disks;
    for (const QJsonValue &value : document.object().value(QStringLiteral("blockdevices")).toArray()) {
        const Device device = parseDevice(value.toObject(), nullptr);
        // Only real disks: not loop devices, RAM disks (zram, ram), optical drives.
        if (device.type != QLatin1String("disk") || device.name.startsWith(QLatin1String("zram"))
            || device.name.startsWith(QLatin1String("ram")) || device.name.startsWith(QLatin1String("loop")))
            continue;
        disks << device;
    }
    return disks;
}

QList<Method> methods(const Device &device)
{
    QList<Method> result;
    const bool disk = device.type == QLatin1String("disk");
    if (disk && device.transport == QLatin1String("nvme")) {
        result << Method::NvmeSanitizeCrypto << Method::NvmeFormatCrypto << Method::NvmeSanitizeBlock
               << Method::NvmeFormatUserData;
    }
    // ATA security commands only on drives attached directly; USB bridges often pass them wrongly or not at all.
    if (disk && (device.transport == QLatin1String("sata") || device.transport == QLatin1String("ata")))
        result << Method::AtaEnhanced << Method::AtaNormal;
    if (device.fileSystem == QLatin1String("crypto_LUKS"))
        result << Method::LuksErase;
    if (device.discard)
        result << Method::BlkdiscardSecure << Method::Blkdiscard;
    return result;
}

QString title(Method method)
{
    switch (method) {
    case Method::NvmeSanitizeCrypto: return QObject::tr("NVMe sanitize - crypto erase");
    case Method::NvmeSanitizeBlock: return QObject::tr("NVMe sanitize - block erase");
    case Method::NvmeFormatCrypto: return QObject::tr("NVMe format - crypto erase");
    case Method::NvmeFormatUserData: return QObject::tr("NVMe format - user data erase");
    case Method::AtaEnhanced: return QObject::tr("ATA Secure Erase - enhanced");
    case Method::AtaNormal: return QObject::tr("ATA Secure Erase");
    case Method::LuksErase: return QObject::tr("LUKS - erase all keys (luksErase)");
    case Method::BlkdiscardSecure: return QObject::tr("Secure discard of all blocks (blkdiscard --secure)");
    case Method::Blkdiscard: return QObject::tr("Discard (TRIM) of all blocks (blkdiscard)");
    }
    return {};
}

QString description(Method method)
{
    switch (method) {
    case Method::NvmeSanitizeCrypto:
        return QObject::tr("The controller destroys the key with which the drive encrypts all data internally - "
                           "every cell, also the spare ones, becomes unreadable at once. The best method if the "
                           "drive supports it. The sanitize operation cannot be stopped and continues after a power "
                           "loss until it is complete.");
    case Method::NvmeSanitizeBlock:
        return QObject::tr("The controller erases every block of the flash memory, also the spare and reallocated ones. "
                           "Takes longer than the crypto erase. It cannot be stopped and continues after a power loss "
                           "until it is complete.");
    case Method::NvmeFormatCrypto:
        return QObject::tr("Formats the namespace with a cryptographic erase (Secure Erase Setting 2). On many drives "
                           "it covers all namespaces of the controller.");
    case Method::NvmeFormatUserData:
        return QObject::tr("Formats the namespace with a user data erase (Secure Erase Setting 1): the controller "
                           "erases the data; whether the spare cells are included depends on the drive.");
    case Method::AtaEnhanced:
        return QObject::tr("The drive erases all its cells including the reallocated ones (on SSDs often a "
                           "cryptographic erase). A temporary drive password is set for it and removed by the erase. "
                           "A drive in the \"frozen\" state refuses it - suspend the computer and resume, then try "
                           "again. Do not interrupt it or disconnect the drive.");
    case Method::AtaNormal:
        return QObject::tr("The drive erases the user data area. A temporary drive password is set for it and removed "
                           "by the erase. A drive in the \"frozen\" state refuses it - suspend the computer and resume, "
                           "then try again. Do not interrupt it or disconnect the drive.");
    case Method::LuksErase:
        return QObject::tr("Wipes all key slots of the LUKS header: the encrypted data can no longer be decrypted with "
                           "any passphrase or key file - unless a backup of the header exists somewhere. Takes a "
                           "second; the encrypted data itself stays on the device. The volume must be closed.");
    case Method::BlkdiscardSecure:
        return QObject::tr("Asks the device to discard all blocks and erase their content, including the copies. "
                           "Few devices support it - it then fails without changing anything.");
    case Method::Blkdiscard:
        return QObject::tr("Tells the device that all blocks are free (TRIM). Most SSDs then read them as zeros and "
                           "erase the cells eventually, but when exactly is up to the controller - this is not a "
                           "guaranteed erase. Use one of the other methods if the device offers it.");
    }
    return {};
}

QString program(Method method)
{
    switch (method) {
    case Method::NvmeSanitizeCrypto:
    case Method::NvmeSanitizeBlock:
    case Method::NvmeFormatCrypto:
    case Method::NvmeFormatUserData:
        return QStringLiteral("nvme");
    case Method::AtaEnhanced:
    case Method::AtaNormal:
        return QStringLiteral("hdparm");
    case Method::LuksErase:
        return QStringLiteral("cryptsetup");
    case Method::BlkdiscardSecure:
    case Method::Blkdiscard:
        return QStringLiteral("blkdiscard");
    }
    return {};
}

QString package(Method method)
{
    const QString name = program(method);
    if (name == QLatin1String("nvme"))
        return QStringLiteral("nvme-cli");
    if (name == QLatin1String("blkdiscard"))
        return QStringLiteral("util-linux");
    return name; // hdparm, cryptsetup
}

Tools Tools::find()
{
    Tools tools;
    tools.lsblk = findTool(QStringLiteral("lsblk"));
    tools.nvme = findTool(QStringLiteral("nvme"));
    tools.hdparm = findTool(QStringLiteral("hdparm"));
    tools.cryptsetup = findTool(QStringLiteral("cryptsetup"));
    tools.blkdiscard = findTool(QStringLiteral("blkdiscard"));
    return tools;
}

QString Tools::path(Method method) const
{
    const QString name = program(method);
    if (name == QLatin1String("nvme"))
        return nvme;
    if (name == QLatin1String("hdparm"))
        return hdparm;
    if (name == QLatin1String("cryptsetup"))
        return cryptsetup;
    return blkdiscard;
}

QString script(Method method, const Device &device, const Tools &tools)
{
    // The checks run as root right before the erase: the device may have changed since it was listed (another
    // disk under the same name after unplugging, a volume mounted in the meantime).
    QString s = QStringLiteral(
                    "set -u\n"
                    "fail() { echo \"ERROR: $*\"; exit 3; }\n"
                    "dev=%1\n"
                    "lsblk=%2\n"
                    "size=$(\"$lsblk\" -dnbo SIZE \"$dev\" 2>/dev/null | tr -d ' ')\n"
                    "[ \"$size\" = %3 ] || fail \"$dev has changed (size ${size:-unknown}, expected %3) - "
                    "nothing was erased\"\n")
                    .arg(q(device.path), q(tools.lsblk), q(QString::number(device.size)));
    if (!device.serial.isEmpty()) {
        s += QStringLiteral("serial=$(\"$lsblk\" -dno SERIAL \"$dev\" 2>/dev/null | sed 's/^ *//;s/ *$//')\n"
                            "[ \"$serial\" = %1 ] || fail \"$dev has changed (serial number $serial) - nothing was "
                            "erased\"\n")
                 .arg(q(device.serial));
    }
    s += QStringLiteral(
        "if \"$lsblk\" -nro MOUNTPOINT \"$dev\" 2>/dev/null | grep -q '[^[:space:]]'; then\n"
        "    fail \"something on $dev is mounted or used as swap - nothing was erased\"\n"
        "fi\n"
        "if \"$lsblk\" -nro TYPE \"$dev\" 2>/dev/null | tail -n +2 | grep -qE '^(crypt|lvm|dm|mpath|md|raid)'; then\n"
        "    fail \"a device on $dev is open (dm-crypt, LVM or RAID) - close it first; nothing was erased\"\n"
        "fi\n");

    const QString tool = q(tools.path(method));
    switch (method) {
    case Method::NvmeSanitizeCrypto:
    case Method::NvmeSanitizeBlock: {
        const bool crypto = method == Method::NvmeSanitizeCrypto;
        s += QStringLiteral(
                 "nvme=%1\n"
                 "caps=$(\"$nvme\" id-ctrl -H \"$dev\") || fail \"nvme id-ctrl failed\"\n"
                 "if echo \"$caps\" | grep -q '%2 Sanitize Operation Not Supported'; then\n"
                 "    fail \"the drive does not support the %3 sanitize operation\"\n"
                 "fi\n"
                 "echo \"Starting the sanitize operation...\"\n"
                 "\"$nvme\" sanitize -a %4 \"$dev\" || fail \"the sanitize command failed\"\n"
                 // The controller works in the background; its log reports the progress (SPROG, 65535 = done).
                 "i=0\n"
                 "while [ $i -lt 17280 ]; do\n"
                 "    log=$(\"$nvme\" sanitize-log \"$dev\" 2>&1)\n"
                 "    if ! echo \"$log\" | grep -q 'SPROG'; then\n"
                 "        echo \"The drive works on it in the background; check with: nvme sanitize-log $dev\"\n"
                 "        exit 0\n"
                 "    fi\n"
                 "    if echo \"$log\" | grep -qi 'completed successfully'; then echo \"Sanitize completed.\"; exit 0; fi\n"
                 "    if echo \"$log\" | grep -qi 'failed'; then echo \"$log\"; fail \"the sanitize operation failed\"; fi\n"
                 "    echo \"$log\" | grep 'SPROG' | head -n 1\n"
                 "    sleep 5\n"
                 "    i=$((i + 1))\n"
                 "done\n"
                 "echo \"Still running; check with: nvme sanitize-log $dev\"\n")
                 .arg(tool, crypto ? QStringLiteral("Crypto Erase") : QStringLiteral("Block Erase"),
                      crypto ? QStringLiteral("crypto erase") : QStringLiteral("block erase"),
                      crypto ? QStringLiteral("4") : QStringLiteral("2"));
        break;
    }
    case Method::NvmeFormatCrypto:
    case Method::NvmeFormatUserData: {
        const bool crypto = method == Method::NvmeFormatCrypto;
        s += QStringLiteral(
                 "nvme=%1\n"
                 "caps=$(\"$nvme\" id-ctrl -H \"$dev\") || fail \"nvme id-ctrl failed\"\n"
                 "%2"
                 // nvme-cli 2 asks for a confirmation (with a delay) unless --force is given; 1.x has no such option.
                 "force=\n"
                 "\"$nvme\" version 2>/dev/null | grep -qE 'version [2-9]' && force=--force\n"
                 "echo \"Formatting...\"\n"
                 "\"$nvme\" format \"$dev\" --ses=%3 $force || fail \"the format command failed\"\n"
                 "echo \"Format completed.\"\n")
                 .arg(tool,
                      crypto ? QStringLiteral("if echo \"$caps\" | grep -q 'Crypto Erase Not Supported as part of Secure "
                                              "Erase'; then\n"
                                              "    fail \"the drive does not support a cryptographic erase\"\n"
                                              "fi\n")
                             : QString(),
                      crypto ? QStringLiteral("2") : QStringLiteral("1"));
        break;
    }
    case Method::AtaEnhanced:
    case Method::AtaNormal: {
        const bool enhanced = method == Method::AtaEnhanced;
        s += QStringLiteral(
                 "hdparm=%1\n"
                 "info=$(\"$hdparm\" -I \"$dev\") || fail \"hdparm -I failed\"\n"
                 "sec=$(echo \"$info\" | sed -n '/^Security:/,/^[^[:space:]]/p')\n"
                 "echo \"$sec\" | grep -qE '^[[:space:]]+supported$' || fail \"the drive does not support the ATA "
                 "security commands\"\n"
                 "if echo \"$sec\" | grep -qE '^[[:space:]]+frozen$'; then\n"
                 "    fail \"the drive is frozen - suspend the computer (systemctl suspend), resume and try again\"\n"
                 "fi\n"
                 "if echo \"$sec\" | grep -qE '^[[:space:]]+enabled$'; then\n"
                 "    fail \"a drive password is already set - it has to be removed first\"\n"
                 "fi\n"
                 "%2"
                 "pass=shred-gui\n"
                 "\"$hdparm\" --user-master u --security-set-pass \"$pass\" \"$dev\" || fail \"setting the temporary "
                 "drive password failed\"\n"
                 "echo \"Erasing (the drive reports how long it takes in hdparm -I)...\"\n"
                 "if \"$hdparm\" --user-master u %3 \"$pass\" \"$dev\"; then\n"
                 "    echo \"Secure erase completed.\"\n"
                 "    exit 0\n"
                 "fi\n"
                 "\"$hdparm\" --user-master u --security-disable \"$pass\" \"$dev\"\n"
                 "fail \"the erase failed; the temporary password was removed (if the drive is still locked, its "
                 "password is: $pass)\"\n")
                 .arg(tool,
                      enhanced ? QStringLiteral("echo \"$sec\" | grep -q 'supported: enhanced erase' || fail \"the "
                                                "drive does not support the enhanced erase\"\n")
                               : QString(),
                      enhanced ? QStringLiteral("--security-erase-enhanced") : QStringLiteral("--security-erase"));
        break;
    }
    case Method::LuksErase:
        s += QStringLiteral("cryptsetup=%1\n"
                            "\"$cryptsetup\" isLuks \"$dev\" || fail \"$dev is not a LUKS volume\"\n"
                            "\"$cryptsetup\" luksErase -q \"$dev\" || fail \"luksErase failed\"\n"
                            "echo \"All key slots were erased.\"\n")
                 .arg(tool);
        break;
    case Method::BlkdiscardSecure:
    case Method::Blkdiscard:
        // -f: blkdiscard 2.36+ refuses devices with a partition table or file system otherwise (the checks above
        // decide whether the device is in use).
        s += QStringLiteral("blkdiscard=%1\n"
                            "\"$blkdiscard\" -f %2-v \"$dev\" || fail \"blkdiscard failed\"\n"
                            "echo \"All blocks were discarded.\"\n")
                 .arg(tool, method == Method::BlkdiscardSecure ? QStringLiteral("--secure ") : QString());
        break;
    }
    return s;
}

QStringList command(const QString &script, const QString &elevate)
{
    QStringList result;
    if (!elevate.isEmpty() && ::geteuid() != 0)
        result << elevate;
    result << QStringLiteral("/bin/sh") << QStringLiteral("-c") << script;
    return result;
}

QString formatSize(qint64 bytes)
{
    // Drives are sold in decimal units.
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

} // namespace DeviceErase
