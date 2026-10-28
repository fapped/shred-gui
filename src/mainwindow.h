#pragma once

#include "shredjob.h"
#include "shredoptions.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointer>

class DesktopProgress;
class QAction;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Whether options loaded from a file become the remembered options for the next runs.
    enum class Persistence { Remember, SessionOnly };

    explicit MainWindow(QWidget *parent = nullptr);

    void addTargets(const QStringList &paths);
    // Loads options from a JSON settings file; shows an error dialog and returns false on failure.
    bool importSettingsFile(const QString &path, Persistence persistence = Persistence::Remember);
    // Loads a preset by name (see ShredOptions::locatePreset) for this session only.
    bool loadPreset(const QString &name);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildUi();
    void buildMenus();
    void populatePresetMenu();
    void loadSettings();
    void saveSettings() const;
    void setOptionsSource(const QString &description);

    ShredOptions currentOptions() const;
    void applyOptions(const ShredOptions &options);
    QStringList targets() const;
    void refreshTargetItem(QListWidgetItem *item);
    void refreshTargets();
    void updatePathInfo();
    void updatePreview();
    void updateControls();

    void addTypedPath();
    void pasteTargets();
    void removeSelectedTargets();
    void browseFiles();
    void browseDirectory();
    void browseProgram();
    void browseRandomSource();
    void importSettings();
    void exportSettings();
    void showAbout();
    void startShred();
    void cancelShred();
    void startDesktopProgress();
    void appendLog(ShredJob::LogLevel level, const QString &text);
    void onJobFinished();

    QString settingsDialogDir() const;
    void rememberSettingsDialogDir(const QString &file) const;

    static QString normalizePath(const QString &text);

    QListWidget *m_targetList = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_pathInfo = nullptr;
    QPushButton *m_removeTargetsButton = nullptr;
    QPushButton *m_clearTargetsButton = nullptr;

    QComboBox *m_programCombo = nullptr;
    QSpinBox *m_iterationsSpin = nullptr;
    QCheckBox *m_zeroCheck = nullptr;
    QCheckBox *m_forceCheck = nullptr;
    QCheckBox *m_exactCheck = nullptr;
    QCheckBox *m_verboseCheck = nullptr;
    QCheckBox *m_sizeCheck = nullptr;
    QLineEdit *m_sizeEdit = nullptr;
    QCheckBox *m_randomCheck = nullptr;
    QLineEdit *m_randomEdit = nullptr;
    QPushButton *m_randomBrowse = nullptr;
    QComboBox *m_removeCombo = nullptr;
    QGroupBox *m_optionsBox = nullptr;
    QGroupBox *m_targetBox = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_exportAction = nullptr;
    QMenu *m_presetMenu = nullptr;

    QLineEdit *m_previewEdit = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QCheckBox *m_closeWhenFinishedCheck = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_currentFile = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QPointer<ShredJob> m_job;
    DesktopProgress *m_desktopProgress = nullptr; // the task manager and the Plasma notification
    QElapsedTimer m_jobTimer;
    // False while the options come from a preset or a --config file: they are not remembered.
    bool m_rememberOptions = true;
};
