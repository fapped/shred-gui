#pragma once

#include "shredjob.h"
#include "shredoptions.h"

#include <QDate>
#include <QMainWindow>
#include <QPointer>

#include <memory>

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
class QPushButton;
class SpeedGraph;
class QSpinBox;
class QThread;
class QTimer;

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
    void populateRecentSettingsMenu();
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
    // `builtIn`: the built-in file dialog, otherwise the preferred one.
    void browseFiles(bool builtIn = false);
    void browseDirectory(bool builtIn = false);
    void browseProgram();
    void browseRandomSource();
    void importSettings();
    void exportSettings();
    void showAbout();
    bool isBusy() const; // scanning or shredding
    void startShred();
    void confirmAndStart(const QList<ShredJob::Plan> &scanned, const QStringList &notes);
    void cancelShred();
    void startDesktopProgress();
    void appendLog(ShredJob::LogLevel level, const QString &text);
    // `withDate`: the line starts with the date too (the start of a run).
    void appendLogLines(ShredJob::LogLevel level, const QString &text, bool withDate);
    void onJobFinished();

    QString settingsDialogDir() const;
    void rememberSettingsDialogDir(const QString &file) const;
    void rememberRecentSettingsFile(const QString &file);

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
    QMenu *m_recentSettingsMenu = nullptr;

    QLineEdit *m_previewEdit = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QCheckBox *m_closeWhenFinishedCheck = nullptr;
    SpeedGraph *m_progress = nullptr; // the progress bar with the speed graph
    QLabel *m_currentFile = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QPointer<ShredJob> m_job;
    // The scan before the confirmation runs in a thread (large directories take a while).
    std::shared_ptr<ShredJob::ScanControl> m_scanControl;
    QPointer<QThread> m_scanThread;
    QTimer *m_scanTimer = nullptr; // shows the number of entries scanned so far
    DesktopProgress *m_desktopProgress = nullptr; // the task manager and the Plasma notification
    QDate m_logDate; // the date of the last log line - a new day gets a line of its own
    // False while the options come from a preset or a --config file: they are not remembered.
    bool m_rememberOptions = true;
};
