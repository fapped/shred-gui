#include "deviceerasedialog.h"
#include "shredoptions.h" // commandLine

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTime>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int kDiskRole = Qt::UserRole;
constexpr int kPartRole = Qt::UserRole + 1;

QString kindOf(const DeviceErase::Device &device)
{
    QString kind = device.removable ? QObject::tr("removable flash") : device.rotational ? QObject::tr("HDD")
                                                                                          : QObject::tr("SSD");
    if (!device.transport.isEmpty())
        kind += QStringLiteral(", ") + device.transport.toUpper();
    return kind;
}

// The devices with the same path in a new listing (nullptr if it is gone).
const DeviceErase::Device *findDevice(const QList<DeviceErase::Device> &disks, const QString &path)
{
    for (const DeviceErase::Device &disk : disks) {
        if (disk.path == path)
            return &disk;
        for (const DeviceErase::Device &part : disk.children) {
            if (part.path == path)
                return &part;
        }
    }
    return nullptr;
}

} // namespace

DeviceEraseDialog::DeviceEraseDialog(QWidget *parent)
    : QDialog(parent)
    , m_tools(DeviceErase::Tools::find())
{
    setWindowTitle(tr("Erase a whole device"));
    resize(860, 640);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("<b>Everything on the selected device is destroyed - all partitions and files.</b> "
                                "On flash storage (SSD, USB stick) overwriting files does not reach the old cells; "
                                "these methods let the device itself erase them, or make encrypted data unreadable "
                                "for good. To overwrite a hard disk, select the device (e.g. /dev/sdb) as a target in "
                                "the main window instead. Devices in use (mounted, swap, open encrypted volumes) cannot "
                                "be erased."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_tree = new QTreeWidget(splitter);
    m_tree->setHeaderLabels({tr("Device"), tr("Size"), tr("Model"), tr("Type"), tr("State")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);

    auto *bottom = new QWidget(splitter);
    auto *bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    auto *methodRow = new QHBoxLayout;
    methodRow->addWidget(new QLabel(tr("Method:"), bottom));
    m_methodCombo = new QComboBox(bottom);
    methodRow->addWidget(m_methodCombo, 1);
    bottomLayout->addLayout(methodRow);
    m_descriptionLabel = new QLabel(bottom);
    m_descriptionLabel->setWordWrap(true);
    m_descriptionLabel->setTextFormat(Qt::PlainText);
    bottomLayout->addWidget(m_descriptionLabel);
    m_stateLabel = new QLabel(bottom);
    m_stateLabel->setWordWrap(true);
    bottomLayout->addWidget(m_stateLabel);
    m_log = new QPlainTextEdit(bottom);
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    bottomLayout->addWidget(m_log, 1);
    splitter->addWidget(m_tree);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto *buttons = new QHBoxLayout;
    m_refreshButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("Refresh"), this);
    m_eraseButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete-shred")), tr("Erase..."), this);
    m_closeButton = new QPushButton(tr("Close"), this);
    buttons->addWidget(m_refreshButton);
    buttons->addStretch(1);
    buttons->addWidget(m_eraseButton);
    buttons->addWidget(m_closeButton);
    layout->addLayout(buttons);

    connect(m_refreshButton, &QPushButton::clicked, this, &DeviceEraseDialog::refresh);
    connect(m_eraseButton, &QPushButton::clicked, this, &DeviceEraseDialog::erase);
    connect(m_closeButton, &QPushButton::clicked, this, &DeviceEraseDialog::reject);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &DeviceEraseDialog::updateMethods);
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, &DeviceEraseDialog::updateDescription);

    refresh();
}

DeviceEraseDialog::~DeviceEraseDialog()
{
    // Never reached while an erase runs (closing is refused); a process that is still there is left alone - it runs
    // as root and must not be interrupted.
    if (m_process)
        m_process->setParent(nullptr);
}

void DeviceEraseDialog::closeEvent(QCloseEvent *event)
{
    if (m_process) {
        QMessageBox::information(this, windowTitle(), tr("The erase is running and must not be interrupted. Wait "
                                                         "until it is finished."));
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void DeviceEraseDialog::reject()
{
    if (m_process) {
        QMessageBox::information(this, windowTitle(), tr("The erase is running and must not be interrupted. Wait "
                                                         "until it is finished."));
        return;
    }
    QDialog::reject();
}

void DeviceEraseDialog::refresh()
{
    const QString previous = currentDevice() ? currentDevice()->path : QString();
    m_tree->clear();
    m_disks.clear();
    if (m_tools.lsblk.isEmpty()) {
        m_stateLabel->setText(tr("lsblk (util-linux) was not found - the devices cannot be listed."));
        updateMethods();
        return;
    }
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(m_tools.lsblk, DeviceErase::lsblkArguments());
    QString error;
    if (!process.waitForFinished(10000) || process.exitCode() != 0) {
        error = tr("lsblk failed: %1").arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        process.kill();
    } else {
        m_disks = DeviceErase::parseLsblk(process.readAllStandardOutput(), &error);
    }
    if (!error.isEmpty())
        appendLog(error, QStringLiteral("#c0392b"));

    QTreeWidgetItem *select = nullptr;
    const auto fill = [&](QTreeWidgetItem *item, const DeviceErase::Device &device) {
        item->setText(0, device.path + (device.fileSystem.isEmpty() ? QString()
                                                                    : QStringLiteral(" (%1)").arg(device.fileSystem)));
        item->setText(1, DeviceErase::formatSize(device.size));
        item->setText(2, device.serial.isEmpty() ? device.model : tr("%1, S/N %2").arg(device.model, device.serial));
        item->setText(3, kindOf(device));
        item->setText(4, device.inUse.isEmpty() ? tr("not in use") : tr("in use"));
        item->setToolTip(4, device.inUse.join(QLatin1Char('\n')));
        if (!device.inUse.isEmpty())
            item->setForeground(4, QColor(QStringLiteral("#c0392b")));
        if (device.path == previous)
            select = item;
    };
    for (qsizetype i = 0; i < m_disks.size(); ++i) {
        auto *diskItem = new QTreeWidgetItem(m_tree);
        diskItem->setData(0, kDiskRole, int(i));
        diskItem->setData(0, kPartRole, -1);
        fill(diskItem, m_disks.at(i));
        for (qsizetype j = 0; j < m_disks.at(i).children.size(); ++j) {
            auto *partItem = new QTreeWidgetItem(diskItem);
            partItem->setData(0, kDiskRole, int(i));
            partItem->setData(0, kPartRole, int(j));
            fill(partItem, m_disks.at(i).children.at(j));
        }
    }
    m_tree->expandAll();
    if (select)
        m_tree->setCurrentItem(select);
    updateMethods();
}

const DeviceErase::Device *DeviceEraseDialog::currentDisk() const
{
    const QTreeWidgetItem *item = m_tree ? m_tree->currentItem() : nullptr;
    const int disk = item ? item->data(0, kDiskRole).toInt() : -1;
    return disk >= 0 && disk < m_disks.size() ? &m_disks.at(disk) : nullptr;
}

QString DeviceEraseDialog::siblingWarning(const DeviceErase::Device &device) const
{
    // A free partition of a disk whose other partitions are in use - often the disk of the running system, or of
    // another operating system (its partitions are not mounted here, but they are not free).
    const DeviceErase::Device *disk = currentDisk();
    if (!disk || disk->path == device.path || disk->inUse.isEmpty())
        return {};
    return tr("Other partitions of %1 are in use - make sure %2 is the partition you mean (it may belong to another "
              "operating system).")
        .arg(disk->path, device.path);
}

const DeviceErase::Device *DeviceEraseDialog::currentDevice() const
{
    const QTreeWidgetItem *item = m_tree ? m_tree->currentItem() : nullptr;
    if (!item)
        return nullptr;
    const int disk = item->data(0, kDiskRole).toInt();
    const int part = item->data(0, kPartRole).toInt();
    if (disk < 0 || disk >= m_disks.size())
        return nullptr;
    if (part < 0)
        return &m_disks.at(disk);
    return part < m_disks.at(disk).children.size() ? &m_disks.at(disk).children.at(part) : nullptr;
}

void DeviceEraseDialog::updateMethods()
{
    m_methodCombo->blockSignals(true);
    m_methodCombo->clear();
    if (const DeviceErase::Device *device = currentDevice()) {
        for (const DeviceErase::Method method : DeviceErase::methods(*device)) {
            QString text = DeviceErase::title(method);
            if (m_tools.path(method).isEmpty())
                text += tr(" - %1 is not installed").arg(DeviceErase::package(method));
            m_methodCombo->addItem(text, int(method));
        }
    }
    m_methodCombo->blockSignals(false);
    updateDescription();
}

void DeviceEraseDialog::updateDescription()
{
    const DeviceErase::Device *device = currentDevice();
    const bool hasMethod = m_methodCombo->currentIndex() >= 0;
    const auto method = DeviceErase::Method(m_methodCombo->currentData().toInt());
    QString state;
    bool ok = false;
    if (!device) {
        state = tr("Select a disk or a partition.");
    } else if (!device->inUse.isEmpty()) {
        state = tr("In use - unmount it, switch off swap and close the encrypted volumes on it first:\n%1")
                    .arg(device->inUse.join(QLatin1Char('\n')));
    } else if (!hasMethod) {
        state = device->rotational && !device->removable && device->transport != QLatin1String("usb")
                    ? tr("No method for this device here. A hard disk is overwritten reliably - select it as a target "
                         "in the main window.")
                    : tr("This device offers none of the methods (no NVMe/ATA erase command over this connection - "
                         "e.g. USB - no LUKS volume, no discard support). Overwriting the whole device (select it as "
                         "a target in the main window) is the remaining option; on flash storage it is not reliable.");
    } else if (m_tools.path(method).isEmpty()) {
        state = tr("Install %1 (e.g. sudo apt install %1) to use this method.").arg(DeviceErase::package(method));
    } else {
        ok = true;
        state = siblingWarning(*device);
    }
    m_descriptionLabel->setText(hasMethod ? DeviceErase::description(method) : QString());
    m_stateLabel->setText(state);
    m_eraseButton->setEnabled(ok && !m_process);
}

bool DeviceEraseDialog::confirm(const DeviceErase::Device &device, DeviceErase::Method method)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Confirm the erase"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QLabel(&dialog);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setText(tr("<p><b>All data on %1 will be destroyed. This cannot be undone.</b></p>"
                     "<p>Device: <b>%1</b><br>Model: %2<br>Serial number: %3<br>Size: %4<br>Content: %5<br>"
                     "Method: %6</p>%7"
                     "<p>To confirm, type the device name <b>%8</b>:</p>")
                      .arg(device.path.toHtmlEscaped(), device.model.toHtmlEscaped(),
                           device.serial.isEmpty() ? tr("unknown") : device.serial.toHtmlEscaped(),
                           DeviceErase::formatSize(device.size),
                           device.fileSystem.isEmpty() ? tr("no file system recognized") : device.fileSystem.toHtmlEscaped(),
                           DeviceErase::title(method).toHtmlEscaped(),
                           siblingWarning(device).isEmpty()
                               ? QString()
                               : QStringLiteral("<p><b>%1</b></p>").arg(siblingWarning(device).toHtmlEscaped()),
                           device.name.toHtmlEscaped()));
    layout->addWidget(text);
    auto *nameEdit = new QLineEdit(&dialog);
    layout->addWidget(nameEdit);
    auto *understood = new QCheckBox(tr("I have a backup of everything I need from this device"), &dialog);
    layout->addWidget(understood);
    auto *buttons = new QDialogButtonBox(&dialog);
    QPushButton *eraseButton = buttons->addButton(tr("Erase %1").arg(device.name), QDialogButtonBox::DestructiveRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    eraseButton->setEnabled(false);
    eraseButton->setAutoDefault(false);
    layout->addWidget(buttons);
    const auto update = [&] { eraseButton->setEnabled(nameEdit->text() == device.name && understood->isChecked()); };
    connect(nameEdit, &QLineEdit::textChanged, &dialog, update);
    connect(understood, &QCheckBox::toggled, &dialog, update);
    connect(eraseButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted;
}

void DeviceEraseDialog::erase()
{
    const DeviceErase::Device *selected = currentDevice();
    if (!selected || m_methodCombo->currentIndex() < 0)
        return;
    const DeviceErase::Device device = *selected;
    const auto method = DeviceErase::Method(m_methodCombo->currentData().toInt());
    if (!confirm(device, method))
        return;

    // The list may be old: the device must still be there, the same one, and not in use.
    refresh();
    const DeviceErase::Device *now = findDevice(m_disks, device.path);
    if (!now || now->size != device.size || now->serial != device.serial || !now->inUse.isEmpty()) {
        appendLog(tr("%1 has changed or is in use now - nothing was erased.").arg(device.path), QStringLiteral("#c0392b"));
        return;
    }

    const QString script = DeviceErase::script(method, *now, m_tools);
    const QStringList command = DeviceErase::command(script);
    appendLog(tr("=== %1: %2 ===").arg(device.path, DeviceErase::title(method)));
    appendLog(tr("Running as administrator: %1 <script>; the script checks the device again first:")
                  .arg(commandLine(command.first(), command.mid(1, command.size() - 2))));
    for (const QString &line : script.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        appendLog(QStringLiteral("    ") + line, QStringLiteral("#888888"));

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setStandardInputFile(QProcess::nullDevice());
    connect(m_process, &QProcess::readyRead, this, [this] {
        while (m_process && m_process->canReadLine()) {
            const QString line = QString::fromLocal8Bit(m_process->readLine()).trimmed();
            if (!line.isEmpty())
                appendLog(line, line.startsWith(QLatin1String("ERROR: ")) ? QStringLiteral("#c0392b") : QString());
        }
    });
    connect(m_process, &QProcess::finished, this, [this, elevated = command.size() > 3](int exitCode,
                                                                                         QProcess::ExitStatus status) {
        const QString rest = QString::fromLocal8Bit(m_process->readAll()).trimmed();
        if (!rest.isEmpty())
            appendLog(rest);
        m_process->deleteLater();
        m_process = nullptr;
        if (status == QProcess::NormalExit && exitCode == 0)
            appendLog(tr("Done."), QStringLiteral("#27ae60"));
        else if (elevated && status == QProcess::NormalExit && exitCode == 126)
            appendLog(tr("Cancelled - the authentication was dismissed. Nothing was erased."), QStringLiteral("#c0392b"));
        else if (elevated && status == QProcess::NormalExit && exitCode == 127)
            appendLog(tr("Not authorized. Nothing was erased."), QStringLiteral("#c0392b"));
        else
            appendLog(tr("Failed (exit code %1).").arg(exitCode), QStringLiteral("#c0392b"));
        setRunning(false);
        refresh();
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_process)
            return;
        appendLog(tr("Could not start: %1").arg(m_process->errorString()), QStringLiteral("#c0392b"));
        m_process->deleteLater();
        m_process = nullptr;
        setRunning(false);
    });
    setRunning(true);
    m_process->start(command.first(), command.mid(1));
}

void DeviceEraseDialog::appendLog(const QString &text, const QString &color)
{
    const QString time = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    const QString escaped = text.toHtmlEscaped();
    m_log->appendHtml(QStringLiteral("<span style=\"color:#888888\">%1</span> ").arg(time)
                      + (color.isEmpty() ? escaped : QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color, escaped)));
}

void DeviceEraseDialog::setRunning(bool running)
{
    m_tree->setEnabled(!running);
    m_methodCombo->setEnabled(!running);
    m_refreshButton->setEnabled(!running);
    m_closeButton->setEnabled(!running);
    if (running)
        m_eraseButton->setEnabled(false);
    else
        updateDescription();
}
