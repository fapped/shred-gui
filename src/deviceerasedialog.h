#pragma once

#include "deviceerase.h"

#include <QDialog>
#include <QPointer>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Tools -> Erase a whole device: lists the disks and partitions (lsblk), offers the erase methods that apply to the
// selected one and runs the chosen one with administrator rights after a confirmation in which the device name has
// to be typed. Devices in use (mounted, swap, open dm-crypt/LVM/RAID) cannot be erased.
class DeviceEraseDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DeviceEraseDialog(QWidget *parent = nullptr);
    ~DeviceEraseDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void refresh();
    void updateMethods();
    void updateDescription();
    void erase();
    // The device of the current item, or nullptr.
    const DeviceErase::Device *currentDevice() const;
    // The disk of the current item (the item itself or its parent).
    const DeviceErase::Device *currentDisk() const;
    // A warning for a partition whose disk has other partitions in use; empty otherwise.
    QString siblingWarning(const DeviceErase::Device &device) const;
    bool confirm(const DeviceErase::Device &device, DeviceErase::Method method);
    void appendLog(const QString &text, const QString &color = QString());
    void setRunning(bool running);

    DeviceErase::Tools m_tools;
    QList<DeviceErase::Device> m_disks;
    QTreeWidget *m_tree = nullptr;
    QComboBox *m_methodCombo = nullptr;
    QLabel *m_descriptionLabel = nullptr;
    QLabel *m_stateLabel = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_eraseButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QPointer<QProcess> m_process;
};
