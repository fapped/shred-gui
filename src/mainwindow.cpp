#include "mainwindow.h"
#include "desktopprogress.h"
#include "localpath.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

namespace {

constexpr int kMaxListedPaths = 500;
constexpr int kPathRole = Qt::UserRole;

QString typeDescription(ShredJob::TargetType type)
{
    switch (type) {
    case ShredJob::TargetType::Missing:
        return MainWindow::tr("does not exist");
    case ShredJob::TargetType::File:
        return MainWindow::tr("file");
    case ShredJob::TargetType::Directory:
        return MainWindow::tr("directory (recursive)");
    case ShredJob::TargetType::Symlink:
        return MainWindow::tr("symbolic link - not allowed");
    case ShredJob::TargetType::Special:
        return MainWindow::tr("special file (device / FIFO / socket)");
    }
    return {};
}

QString settingsFileFilter()
{
    return MainWindow::tr("Shred GUI settings (*.json);;All files (*)");
}

QString optionsBoxTitle()
{
    return MainWindow::tr("shred options");
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Shred GUI"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    setAcceptDrops(true);
    m_desktopProgress = new DesktopProgress(this);
    connect(m_desktopProgress, &DesktopProgress::cancelRequested, this, &MainWindow::cancelShred);
    buildUi();
    buildMenus();
    loadSettings();
    updatePathInfo();
    updatePreview();
    updateControls();
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);

    // --- Targets ---
    m_targetBox = new QGroupBox(tr("Files and directories to shred"), central);
    auto *targetLayout = new QVBoxLayout(m_targetBox);
    m_targetList = new QListWidget(m_targetBox);
    m_targetList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_targetList->setAcceptDrops(false); // drops are handled by the window (file:// URL to path conversion)
    m_targetList->setMinimumHeight(90);
    m_targetList->setContextMenuPolicy(Qt::ActionsContextMenu);
    targetLayout->addWidget(m_targetList, 1);

    auto *pasteAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Paste paths"), m_targetList);
    pasteAction->setShortcut(QKeySequence::Paste);
    pasteAction->setShortcutContext(Qt::WidgetShortcut);
    auto *removeAction = new QAction(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove from list"), m_targetList);
    removeAction->setShortcut(QKeySequence::Delete);
    removeAction->setShortcutContext(Qt::WidgetShortcut);
    m_targetList->addAction(pasteAction);
    m_targetList->addAction(removeAction);

    auto *pathRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit(m_targetBox);
    m_pathEdit->setPlaceholderText(tr("Type or paste a path and press Enter"));
    m_pathEdit->setClearButtonEnabled(true);
    m_pathEdit->setAcceptDrops(false);
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), m_targetBox);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(addButton);
    targetLayout->addLayout(pathRow);

    auto *targetButtons = new QHBoxLayout;
    auto *addFilesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), tr("Add files..."), m_targetBox);
    auto *addDirButton = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Add directory..."), m_targetBox);
    auto *pasteButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Paste"), m_targetBox);
    pasteButton->setToolTip(tr("Add paths from the clipboard (one per line, or files copied in a file manager)"));
    m_removeTargetsButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), m_targetBox);
    m_clearTargetsButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear-list")), tr("Clear"), m_targetBox);
    targetButtons->addWidget(addFilesButton);
    targetButtons->addWidget(addDirButton);
    targetButtons->addWidget(pasteButton);
    targetButtons->addStretch(1);
    targetButtons->addWidget(m_removeTargetsButton);
    targetButtons->addWidget(m_clearTargetsButton);
    targetLayout->addLayout(targetButtons);

    m_pathInfo = new QLabel(m_targetBox);
    m_pathInfo->setWordWrap(true);
    targetLayout->addWidget(m_pathInfo);
    mainLayout->addWidget(m_targetBox, 1);

    // --- Options ---
    m_optionsBox = new QGroupBox(optionsBoxTitle(), central);
    auto *form = new QFormLayout(m_optionsBox);

    auto *programRow = new QHBoxLayout;
    m_programCombo = new QComboBox(m_optionsBox);
    m_programCombo->setEditable(true);
    m_programCombo->setInsertPolicy(QComboBox::NoInsert);
    for (const QString &name : {QStringLiteral("shred"), QStringLiteral("gnushred")}) {
        if (!QStandardPaths::findExecutable(name).isEmpty())
            m_programCombo->addItem(name);
    }
    m_programCombo->setToolTip(tr("The shred program. On Ubuntu 25.10+ \"shred\" is the uutils (Rust) version "
                                  "and \"gnushred\" is GNU coreutils."));
    auto *programBrowse = new QPushButton(tr("..."), m_optionsBox);
    programBrowse->setToolTip(tr("Select the shred executable"));
    programRow->addWidget(m_programCombo, 1);
    programRow->addWidget(programBrowse);
    form->addRow(tr("Program:"), programRow);

    m_iterationsSpin = new QSpinBox(m_optionsBox);
    m_iterationsSpin->setRange(0, ShredOptions::MaxIterations);
    m_iterationsSpin->setToolTip(tr("-n, --iterations=N: overwrite N times instead of the default (3)"));
    form->addRow(tr("Passes (-n):"), m_iterationsSpin);

    m_zeroCheck = new QCheckBox(tr("Add a final overwrite with zeros to hide shredding (-z)"), m_optionsBox);
    m_forceCheck = new QCheckBox(tr("Change permissions to allow writing if necessary (-f)"), m_optionsBox);
    m_exactCheck = new QCheckBox(tr("Do not round file sizes up to the next full block (-x)"), m_optionsBox);
    m_verboseCheck = new QCheckBox(tr("Show progress (-v)"), m_optionsBox);
    form->addRow(QString(), m_zeroCheck);
    form->addRow(QString(), m_forceCheck);
    form->addRow(QString(), m_exactCheck);
    form->addRow(QString(), m_verboseCheck);

    auto *sizeRow = new QHBoxLayout;
    m_sizeCheck = new QCheckBox(tr("Shred only"), m_optionsBox);
    m_sizeEdit = new QLineEdit(m_optionsBox);
    m_sizeEdit->setPlaceholderText(tr("e.g. 4096, 512K, 10M, 1GiB"));
    m_sizeEdit->setToolTip(tr("-s, --size=N: shred this many bytes (suffixes like K, M, G are accepted)"));
    sizeRow->addWidget(m_sizeCheck);
    sizeRow->addWidget(m_sizeEdit, 1);
    form->addRow(tr("Size (-s):"), sizeRow);

    auto *randomRow = new QHBoxLayout;
    m_randomCheck = new QCheckBox(tr("Use file"), m_optionsBox);
    m_randomEdit = new QLineEdit(m_optionsBox);
    m_randomEdit->setPlaceholderText(QStringLiteral("/dev/urandom"));
    m_randomEdit->setToolTip(tr("--random-source=FILE: get random bytes from FILE"));
    m_randomBrowse = new QPushButton(tr("..."), m_optionsBox);
    randomRow->addWidget(m_randomCheck);
    randomRow->addWidget(m_randomEdit, 1);
    randomRow->addWidget(m_randomBrowse);
    form->addRow(tr("Random source:"), randomRow);

    m_removeCombo = new QComboBox(m_optionsBox);
    m_removeCombo->addItem(tr("Do not remove (overwrite only)"), int(ShredOptions::RemoveMode::None));
    m_removeCombo->addItem(tr("-u: remove after overwriting (default: wipesync)"), int(ShredOptions::RemoveMode::Default));
    m_removeCombo->addItem(tr("--remove=unlink: plain unlink"), int(ShredOptions::RemoveMode::Unlink));
    m_removeCombo->addItem(tr("--remove=wipe: obfuscate the name first"), int(ShredOptions::RemoveMode::Wipe));
    m_removeCombo->addItem(tr("--remove=wipesync: obfuscate the name and sync each change"),
                           int(ShredOptions::RemoveMode::WipeSync));
    m_removeCombo->setToolTip(tr("Removing files is required for directories. Subdirectory names are obfuscated "
                                 "the same way as file names (for the wipe/wipesync/-u modes)."));
    form->addRow(tr("Removal:"), m_removeCombo);

    auto *settingsRow = new QHBoxLayout;
    auto *defaultsButton = new QPushButton(tr("Restore defaults"), m_optionsBox);
    auto *importButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), tr("Import..."), m_optionsBox);
    importButton->setToolTip(tr("Load options from a JSON settings file"));
    auto *exportButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), tr("Export..."), m_optionsBox);
    exportButton->setToolTip(tr("Save the current options to a JSON settings file"));
    settingsRow->addWidget(defaultsButton);
    settingsRow->addStretch(1);
    settingsRow->addWidget(importButton);
    settingsRow->addWidget(exportButton);
    form->addRow(QString(), settingsRow);
    mainLayout->addWidget(m_optionsBox);

    // --- Command preview ---
    auto *previewRow = new QHBoxLayout;
    m_previewEdit = new QLineEdit(central);
    m_previewEdit->setReadOnly(true);
    m_previewEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    previewRow->addWidget(new QLabel(tr("Command:"), central));
    previewRow->addWidget(m_previewEdit, 1);
    mainLayout->addLayout(previewRow);

    // --- Run ---
    auto *runRow = new QHBoxLayout;
    m_progress = new QProgressBar(central);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setFormat(QStringLiteral("%p%"));
    m_startButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), tr("Shred"), central);
    m_startButton->setStyleSheet(QStringLiteral("QPushButton:enabled { color: white; background-color: #c62828; "
                                                "font-weight: bold; padding: 4px 16px; }"));
    m_cancelButton = new QPushButton(QIcon::fromTheme(QStringLiteral("process-stop")), tr("Cancel"), central);
    runRow->addWidget(m_progress, 1);
    runRow->addWidget(m_startButton);
    runRow->addWidget(m_cancelButton);
    mainLayout->addLayout(runRow);

    m_currentFile = new QLabel(central);
    m_currentFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(m_currentFile);

    // --- Log ---
    auto *logBox = new QGroupBox(tr("Log"), central);
    auto *logLayout = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(20000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMinimumHeight(120);
    auto *clearLog = new QPushButton(tr("Clear log"), logBox);
    logLayout->addWidget(m_log, 1);
    logLayout->addWidget(clearLog, 0, Qt::AlignRight);
    mainLayout->addWidget(logBox, 1);

    setCentralWidget(central);
    resize(840, 960);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &MainWindow::addTypedPath);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addTypedPath);
    connect(addFilesButton, &QPushButton::clicked, this, &MainWindow::browseFiles);
    connect(addDirButton, &QPushButton::clicked, this, &MainWindow::browseDirectory);
    connect(pasteButton, &QPushButton::clicked, this, &MainWindow::pasteTargets);
    connect(pasteAction, &QAction::triggered, this, &MainWindow::pasteTargets);
    connect(m_removeTargetsButton, &QPushButton::clicked, this, &MainWindow::removeSelectedTargets);
    connect(removeAction, &QAction::triggered, this, &MainWindow::removeSelectedTargets);
    connect(m_clearTargetsButton, &QPushButton::clicked, this, [this] {
        m_targetList->clear();
        updatePathInfo();
        updatePreview();
        updateControls();
    });
    connect(m_targetList, &QListWidget::itemSelectionChanged, this, &MainWindow::updateControls);

    connect(programBrowse, &QPushButton::clicked, this, &MainWindow::browseProgram);
    connect(m_randomBrowse, &QPushButton::clicked, this, &MainWindow::browseRandomSource);
    connect(defaultsButton, &QPushButton::clicked, this, [this] {
        ShredOptions defaults;
        defaults.program = m_programCombo->count() > 0 ? m_programCombo->itemText(0) : defaults.program;
        applyOptions(defaults);
    });
    connect(importButton, &QPushButton::clicked, this, &MainWindow::importSettings);
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportSettings);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startShred);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::cancelShred);
    connect(clearLog, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);

    const auto refresh = [this] {
        updatePreview();
        updateControls();
    };
    connect(m_programCombo, &QComboBox::currentTextChanged, this, refresh);
    connect(m_iterationsSpin, &QSpinBox::valueChanged, this, refresh);
    for (QCheckBox *box : {m_zeroCheck, m_forceCheck, m_exactCheck, m_verboseCheck, m_sizeCheck, m_randomCheck})
        connect(box, &QCheckBox::toggled, this, refresh);
    connect(m_sizeEdit, &QLineEdit::textChanged, this, refresh);
    connect(m_randomEdit, &QLineEdit::textChanged, this, refresh);
    connect(m_removeCombo, &QComboBox::currentIndexChanged, this, refresh);
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    m_presetMenu = fileMenu->addMenu(QIcon::fromTheme(QStringLiteral("bookmarks")), tr("Load &preset"));
    connect(m_presetMenu, &QMenu::aboutToShow, this, &MainWindow::populatePresetMenu);
    m_importAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                         tr("&Import settings..."), this, &MainWindow::importSettings);
    m_importAction->setShortcut(QKeySequence::Open);
    m_exportAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                         tr("&Export settings..."), this, &MainWindow::exportSettings);
    m_exportAction->setShortcut(QKeySequence::Save);
    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), tr("&Quit"), this,
                                        &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, &MainWindow::showAbout);
}

void MainWindow::populatePresetMenu()
{
    m_presetMenu->clear();
    const QMap<QString, QString> presets = ShredOptions::availablePresets();
    if (presets.isEmpty()) {
        m_presetMenu->addAction(tr("No presets found"))->setEnabled(false);
        return;
    }
    for (auto it = presets.cbegin(); it != presets.cend(); ++it) {
        const QString file = it.value();
        QAction *action = m_presetMenu->addAction(it.key(), this, [this, file] {
            importSettingsFile(file, Persistence::SessionOnly);
        });
        action->setToolTip(file);
    }
    m_presetMenu->addSeparator();
    m_presetMenu->addAction(tr("Presets are used for this session only"))->setEnabled(false);
}

void MainWindow::loadSettings()
{
    QSettings settings;
    ShredOptions options = ShredOptions::load(settings);
    if (!settings.contains(QStringLiteral("options/program")) && m_programCombo->count() > 0)
        options.program = m_programCombo->itemText(0);
    applyOptions(options);
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
}

void MainWindow::saveSettings() const
{
    QSettings settings;
    if (m_rememberOptions)
        currentOptions().save(settings);
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
}

void MainWindow::setOptionsSource(const QString &description)
{
    m_optionsBox->setTitle(description.isEmpty() ? optionsBoxTitle()
                                                 : tr("%1 - %2").arg(optionsBoxTitle(), description));
}

ShredOptions MainWindow::currentOptions() const
{
    ShredOptions o;
    o.program = m_programCombo->currentText().trimmed();
    o.iterations = m_iterationsSpin->value();
    o.zero = m_zeroCheck->isChecked();
    o.force = m_forceCheck->isChecked();
    o.exact = m_exactCheck->isChecked();
    o.verbose = m_verboseCheck->isChecked();
    o.useSize = m_sizeCheck->isChecked();
    o.size = m_sizeEdit->text().trimmed();
    o.useRandomSource = m_randomCheck->isChecked();
    o.randomSource = m_randomEdit->text().trimmed();
    o.removeMode = static_cast<ShredOptions::RemoveMode>(m_removeCombo->currentData().toInt());
    return o;
}

void MainWindow::applyOptions(const ShredOptions &o)
{
    if (m_programCombo->findText(o.program) < 0 && !o.program.isEmpty())
        m_programCombo->addItem(o.program);
    m_programCombo->setCurrentText(o.program);
    m_iterationsSpin->setValue(o.iterations);
    m_zeroCheck->setChecked(o.zero);
    m_forceCheck->setChecked(o.force);
    m_exactCheck->setChecked(o.exact);
    m_verboseCheck->setChecked(o.verbose);
    m_sizeCheck->setChecked(o.useSize);
    m_sizeEdit->setText(o.size);
    m_randomCheck->setChecked(o.useRandomSource);
    m_randomEdit->setText(o.randomSource);
    m_removeCombo->setCurrentIndex(qMax(0, m_removeCombo->findData(int(o.removeMode))));
    updatePreview();
    updateControls();
}

QString MainWindow::normalizePath(const QString &text)
{
    QString path = text.trimmed();
    if (path.size() >= 2
        && ((path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"')))
            || (path.startsWith(QLatin1Char('\'')) && path.endsWith(QLatin1Char('\''))))) {
        path = path.mid(1, path.size() - 2);
    }
    if (path.startsWith(QStringLiteral("file://")))
        path = LocalPath::fromFileUrl(path); // keeps bytes that are not valid UTF-8 (see LocalPath)
    if (path == QStringLiteral("~"))
        path = QDir::homePath();
    else if (path.startsWith(QStringLiteral("~/")))
        path = QDir::homePath() + path.mid(1);
    if (path.isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void MainWindow::addTargets(const QStringList &paths)
{
    QStringList existing = targets();
    int added = 0;
    for (const QString &raw : paths) {
        const QString path = normalizePath(raw);
        if (path.isEmpty() || existing.contains(path))
            continue;
        auto *item = new QListWidgetItem(LocalPath::display(path), m_targetList);
        item->setData(kPathRole, path);
        refreshTargetItem(item);
        existing << path;
        ++added;
    }
    if (added > 0)
        m_targetList->scrollToBottom();
    updatePathInfo();
    updatePreview();
    updateControls();
}

QStringList MainWindow::targets() const
{
    QStringList paths;
    for (int i = 0; i < m_targetList->count(); ++i)
        paths << m_targetList->item(i)->data(kPathRole).toString();
    return paths;
}

void MainWindow::refreshTargetItem(QListWidgetItem *item)
{
    const QString path = item->data(kPathRole).toString();
    const ShredJob::TargetType type = ShredJob::targetType(path);
    QString icon;
    QString tooltip = typeDescription(type);
    switch (type) {
    case ShredJob::TargetType::File:
        icon = QStringLiteral("text-x-generic");
        tooltip += tr(", %1").arg(QLocale().formattedDataSize(LocalPath::info(path).size));
        break;
    case ShredJob::TargetType::Directory:
        if (ShredJob::isProtectedPath(path)) {
            icon = QStringLiteral("dialog-error");
            tooltip += tr(" - protected, cannot be shredded");
        } else {
            icon = QStringLiteral("folder");
        }
        break;
    case ShredJob::TargetType::Special:
        icon = QStringLiteral("drive-harddisk");
        break;
    case ShredJob::TargetType::Symlink:
        icon = QStringLiteral("dialog-error");
        tooltip += tr(" (points to: %1)").arg(LocalPath::display(LocalPath::info(path).symLinkTarget));
        break;
    case ShredJob::TargetType::Missing:
        icon = QStringLiteral("dialog-error");
        break;
    }
    item->setIcon(QIcon::fromTheme(icon));
    item->setToolTip(tooltip);
}

void MainWindow::refreshTargets()
{
    for (int i = 0; i < m_targetList->count(); ++i)
        refreshTargetItem(m_targetList->item(i));
    updatePathInfo();
    updatePreview();
    updateControls();
}

void MainWindow::updatePathInfo()
{
    const QStringList paths = targets();
    if (paths.isEmpty()) {
        m_pathInfo->setText(tr("No targets. Add files or directories, paste paths or drop them here."));
        return;
    }
    if (paths.size() == 1) {
        m_pathInfo->setText(tr("Type: %1").arg(m_targetList->item(0)->toolTip()));
        return;
    }
    int files = 0;
    int dirs = 0;
    int invalid = 0;
    for (const QString &path : paths) {
        switch (ShredJob::targetType(path)) {
        case ShredJob::TargetType::File:
        case ShredJob::TargetType::Special:
            ++files;
            break;
        case ShredJob::TargetType::Directory:
            ShredJob::isProtectedPath(path) ? ++invalid : ++dirs;
            break;
        case ShredJob::TargetType::Missing:
        case ShredJob::TargetType::Symlink:
            ++invalid;
            break;
        }
    }
    QString info = tr("%1 targets: %2 file(s), %3 director(y/ies)").arg(paths.size()).arg(files).arg(dirs);
    if (invalid > 0)
        info += tr(", %1 invalid (they will be skipped)").arg(invalid);
    m_pathInfo->setText(info);
}

void MainWindow::updatePreview()
{
    const QStringList paths = targets();
    const ShredOptions options = currentOptions();
    QString target;
    if (paths.isEmpty())
        target = tr("<file>");
    else if (paths.size() > 1)
        target = tr("<each file of the %1 targets>").arg(paths.size());
    else if (ShredJob::targetType(paths.first()) == ShredJob::TargetType::Directory)
        target = tr("<each file in %1>").arg(LocalPath::display(paths.first()));
    else
        target = shellQuote(paths.first());
    m_previewEdit->setText(commandLine(options.program, options.arguments()) + QStringLiteral(" -- ") + target);
    m_previewEdit->setCursorPosition(0);
}

void MainWindow::updateControls()
{
    const bool running = m_job && m_job->isRunning();
    m_targetBox->setEnabled(!running);
    m_optionsBox->setEnabled(!running);
    if (m_importAction)
        m_importAction->setEnabled(!running);
    if (m_presetMenu)
        m_presetMenu->setEnabled(!running);
    m_sizeEdit->setEnabled(m_sizeCheck->isChecked());
    m_randomEdit->setEnabled(m_randomCheck->isChecked());
    m_randomBrowse->setEnabled(m_randomCheck->isChecked());
    m_removeTargetsButton->setEnabled(!m_targetList->selectedItems().isEmpty());
    m_clearTargetsButton->setEnabled(m_targetList->count() > 0);
    m_startButton->setEnabled(!running && m_targetList->count() > 0);
    m_cancelButton->setEnabled(running);

    const QString error = currentOptions().validate();
    statusBar()->showMessage(error);
}

void MainWindow::addTypedPath()
{
    if (m_pathEdit->text().trimmed().isEmpty())
        return;
    addTargets({m_pathEdit->text()});
    m_pathEdit->clear();
}

void MainWindow::pasteTargets()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return;
    QStringList paths;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            // Encoded URLs keep the exact bytes of names that are not valid UTF-8 (see LocalPath).
            if (url.isLocalFile())
                paths << url.toString(QUrl::FullyEncoded);
        }
    } else {
        paths = mime->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }
    addTargets(paths);
}

void MainWindow::removeSelectedTargets()
{
    qDeleteAll(m_targetList->selectedItems());
    updatePathInfo();
    updatePreview();
    updateControls();
}

void MainWindow::browseFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Select files to shred"), QDir::homePath(),
                                                            QString(), nullptr, QFileDialog::DontResolveSymlinks);
    addTargets(files);
}

void MainWindow::browseDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select a directory to shred"), QDir::homePath(),
                                                          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        addTargets({dir});
}

void MainWindow::browseProgram()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Select the shred program"), QStringLiteral("/usr/bin"));
    if (!file.isEmpty())
        m_programCombo->setCurrentText(file);
}

void MainWindow::browseRandomSource()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Select a random source"), QStringLiteral("/dev"));
    if (!file.isEmpty())
        m_randomEdit->setText(file);
}

QString MainWindow::settingsDialogDir() const
{
    const QString dir = QSettings().value(QStringLiteral("paths/settingsDir")).toString();
    return !dir.isEmpty() && QFileInfo(dir).isDir() ? dir : QDir::homePath();
}

void MainWindow::rememberSettingsDialogDir(const QString &file) const
{
    QSettings().setValue(QStringLiteral("paths/settingsDir"), QFileInfo(file).absolutePath());
}

bool MainWindow::importSettingsFile(const QString &path, Persistence persistence)
{
    QString error;
    QString name;
    // Keys missing from the file keep the current values (e.g. presets do not choose the program).
    const ShredOptions options = ShredOptions::loadFromFile(path, &error, currentOptions(), &name);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Import failed"), error);
        return false;
    }
    if (name.isEmpty())
        name = QFileInfo(path).completeBaseName();
    applyOptions(options);

    if (persistence == Persistence::Remember) {
        m_rememberOptions = true;
        setOptionsSource(QString());
        saveSettings();
        appendLog(ShredJob::LogLevel::Info, tr("Imported settings from %1").arg(path));
    } else {
        // Session only: the remembered options stay untouched until settings are imported explicitly.
        m_rememberOptions = false;
        setOptionsSource(tr("%1 (session only, not remembered)").arg(name));
        appendLog(ShredJob::LogLevel::Info, tr("Loaded \"%1\" from %2 (session only)").arg(name, path));
    }
    // The validation message (e.g. a program missing on this machine) takes precedence in the status bar.
    if (options.validate().isEmpty())
        statusBar()->showMessage(tr("Loaded settings \"%1\"").arg(name), 8000);
    return true;
}

bool MainWindow::loadPreset(const QString &name)
{
    const QString file = ShredOptions::locatePreset(name);
    if (file.isEmpty()) {
        QMessageBox::warning(this, tr("Preset not found"),
                             tr("Preset \"%1\" was not found. Searched in:\n%2")
                                 .arg(name, ShredOptions::presetSearchPaths().join(QLatin1Char('\n'))));
        return false;
    }
    return importSettingsFile(file, Persistence::SessionOnly);
}

void MainWindow::importSettings()
{
    if (m_job && m_job->isRunning())
        return;
    const QString file = QFileDialog::getOpenFileName(this, tr("Import settings"), settingsDialogDir(),
                                                      settingsFileFilter());
    if (file.isEmpty())
        return;
    rememberSettingsDialogDir(file);
    importSettingsFile(file, Persistence::Remember);
}

void MainWindow::exportSettings()
{
    const QString start = QDir(settingsDialogDir()).filePath(QStringLiteral("shred-gui-settings.json"));
    QString file = QFileDialog::getSaveFileName(this, tr("Export settings"), start, settingsFileFilter());
    if (file.isEmpty())
        return;
    if (QFileInfo(file).suffix().isEmpty())
        file += QStringLiteral(".json");
    rememberSettingsDialogDir(file);

    QString error;
    if (!currentOptions().saveToFile(file, &error)) {
        QMessageBox::warning(this, tr("Export failed"), error);
        return;
    }
    appendLog(ShredJob::LogLevel::Info, tr("Exported settings to %1").arg(file));
    statusBar()->showMessage(tr("Exported settings to %1").arg(file), 8000);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Shred GUI"),
                       tr("<b>Shred GUI</b> %1<br><br>"
                          "A graphical front-end for the <code>shred</code> command.<br><br>"
                          "<b>Created with AI:</b> the code, tests and documentation were written by "
                          "Claude (Anthropic) in Claude Code, following the author's requirements, and "
                          "checked with automated tests and by hand. Review the code before relying on it - "
                          "shredding cannot be undone.<br><br>"
                          "Licensed under the MIT License.")
                           .arg(QApplication::applicationVersion()));
}

void MainWindow::startShred()
{
    if (m_job && m_job->isRunning())
        return;

    ShredOptions options = currentOptions();
    if (const QString error = options.validate(); !error.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid options"), error);
        return;
    }

    QStringList notes;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QList<ShredJob::Plan> scanned = ShredJob::scanAll(targets(), &notes);
    QApplication::restoreOverrideCursor();

    QList<ShredJob::Plan> plans;
    QStringList errors;
    for (const ShredJob::Plan &plan : scanned) {
        if (plan.error.isEmpty())
            plans << plan;
        else
            errors << plan.error;
    }
    if (plans.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot continue"), errors.join(QLatin1Char('\n')));
        return;
    }
    if (!errors.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, tr("Some targets cannot be processed"),
            tr("%1\n\nContinue with the remaining %2 target(s)?").arg(errors.join(QLatin1Char('\n'))).arg(plans.size()));
        if (answer != QMessageBox::Yes)
            return;
    }

    const bool anyDirectory = std::any_of(plans.cbegin(), plans.cend(),
                                          [](const ShredJob::Plan &plan) { return plan.isDirectory(); });
    if (anyDirectory && !options.removesFiles()) {
        const auto answer = QMessageBox::question(
            this, tr("File removal required"),
            tr("To remove a directory, its files have to be removed after overwriting.\n\n"
               "Enable the -u option (remove after overwriting)?"));
        if (answer != QMessageBox::Yes)
            return;
        m_removeCombo->setCurrentIndex(m_removeCombo->findData(int(ShredOptions::RemoveMode::Default)));
        options = currentOptions();
    }

    qsizetype files = 0;
    qsizetype others = 0;
    qsizetype dirs = 0;
    qsizetype dirTargets = 0;
    qsizetype skipped = 0;
    bool anySpecial = false;
    QStringList listed;
    for (const ShredJob::Plan &plan : std::as_const(plans)) {
        files += plan.files.size();
        others += plan.others.size();
        dirs += plan.dirs.size();
        skipped += plan.skipped.size();
        dirTargets += plan.isDirectory() ? 1 : 0;
        anySpecial = anySpecial || plan.type == ShredJob::TargetType::Special;
        for (const QStringList *list : {&plan.files, &plan.others}) {
            for (const QString &path : *list) {
                if (listed.size() < kMaxListedPaths)
                    listed << LocalPath::display(path);
            }
        }
    }

    QString text;
    if (plans.size() == 1 && plans.first().isDirectory()) {
        text = tr("Directory <b>%1</b> will be recursively shredded and removed.")
                   .arg(LocalPath::display(plans.first().target).toHtmlEscaped());
    } else if (plans.size() == 1) {
        text = options.removesFiles() ? tr("File <b>%1</b> will be overwritten and removed.")
                                      : tr("File <b>%1</b> will be overwritten (not removed).");
        text = text.arg(LocalPath::display(plans.first().target).toHtmlEscaped());
    } else {
        text = tr("<b>%1</b> targets (%2 file(s), %3 director(y/ies)) will be shredded")
                   .arg(plans.size())
                   .arg(plans.size() - dirTargets)
                   .arg(dirTargets);
        text += options.removesFiles() ? tr(" and removed.") : tr(" (files are overwritten, not removed).");
    }
    if (dirTargets > 0) {
        text += tr("<br><br>Files to overwrite: <b>%1</b><br>"
                   "Symlinks and special files (removed only, their targets are not overwritten): <b>%2</b><br>"
                   "Directories to remove: <b>%3</b>")
                    .arg(files)
                    .arg(others)
                    .arg(dirs);
    }
    if (skipped > 0) {
        text += tr("<br><br><b>Warning:</b> skipped %1 mount point(s) of other file systems - "
                   "their content will not be shredded and their parent directories will not be removed.")
                    .arg(skipped);
    }
    if (anySpecial)
        text += tr("<br><br><b>A special file (e.g. a block device) is selected - its entire content will be overwritten!</b>");
    else if (options.removesFiles())
        text += tr("<br><br>If a file has other hard links, their data will be destroyed as well.");
    for (const QString &note : std::as_const(notes))
        text += QStringLiteral("<br>") + note.toHtmlEscaped();
    text += tr("<br><br>Command:<br><code>%1</code><br><br><b>This operation cannot be undone.</b>")
                .arg(commandLine(options.program, options.arguments()).toHtmlEscaped());

    QString details = listed.join(QLatin1Char('\n'));
    if (files + others > listed.size())
        details += tr("\n... and %1 more").arg(files + others - listed.size());

    QMessageBox box(QMessageBox::Warning, tr("Confirm shredding"), text, QMessageBox::NoButton, this);
    box.setTextFormat(Qt::RichText);
    if (!details.isEmpty())
        box.setDetailedText(details);
    QPushButton *confirm = box.addButton(tr("Shred"), QMessageBox::DestructiveRole);
    QPushButton *abort = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(abort);
    box.exec();
    if (box.clickedButton() != confirm)
        return;

    saveSettings();
    delete m_job;
    m_job = new ShredJob(std::move(plans), options, this);
    connect(m_job, &ShredJob::log, this, &MainWindow::appendLog);
    connect(m_job, &ShredJob::progress, m_progress, &QProgressBar::setValue);
    connect(m_job, &ShredJob::currentFileChanged, this, [this](const QString &file) {
        m_currentFile->setText(file.isEmpty() ? QString() : tr("Current file: %1").arg(LocalPath::display(file)));
    });
    connect(m_job, &ShredJob::finished, this, &MainWindow::onJobFinished);

    for (const QString &note : std::as_const(notes))
        appendLog(ShredJob::LogLevel::Warning, note);
    appendLog(ShredJob::LogLevel::Info, tr("=== Start: %1 target(s) ===").arg(m_job->plans().size()));
    startDesktopProgress();
    m_job->start();
    updateControls();
}

void MainWindow::startDesktopProgress()
{
    const QList<ShredJob::Plan> &plans = m_job->plans();
    QString target = plans.isEmpty() ? QString() : LocalPath::display(plans.first().target);
    if (plans.size() > 1)
        target = tr("%1 (+%2 more)").arg(target).arg(plans.size() - 1);
    qsizetype files = 0;
    for (const ShredJob::Plan &plan : plans)
        files += plan.files.size();
    const ShredOptions options = currentOptions();
    const int passes = options.iterations + (options.zero ? 1 : 0);
    const QString amount = tr("%n file(s), %1 to write (%2 pass(es))", nullptr, int(files))
                               .arg(QLocale().formattedDataSize(m_job->totalBytes()))
                               .arg(passes);
    m_desktopProgress->start(tr("Shredding"), target, amount);
    m_desktopProgress->setTotalBytes(m_job->totalBytes());
    m_jobTimer.start();

    connect(m_job, &ShredJob::progress, m_desktopProgress, &DesktopProgress::setProgress);
    // shred reports whole passes only, so the speed is the average since the start.
    connect(m_job, &ShredJob::processedBytesChanged, m_desktopProgress, [this](qint64 bytes) {
        const qint64 ms = qMax<qint64>(1, m_jobTimer.elapsed());
        m_desktopProgress->setSpeed(bytes * 1000 / ms, bytes);
    });
    auto fileNumber = std::make_shared<qsizetype>(0);
    connect(m_job, &ShredJob::currentFileChanged, m_desktopProgress, [this, files, fileNumber](const QString &file) {
        if (file.isEmpty())
            return;
        m_desktopProgress->setInfo(tr("File %1 of %2: %3")
                                       .arg(++*fileNumber)
                                       .arg(files)
                                       .arg(LocalPath::display(QFileInfo(file).fileName())));
    });
}

void MainWindow::cancelShred()
{
    if (m_job)
        m_job->cancel();
}

void MainWindow::appendLog(ShredJob::LogLevel level, const QString &text)
{
    QString color;
    switch (level) {
    case ShredJob::LogLevel::Error:
        color = QStringLiteral("#e53935");
        break;
    case ShredJob::LogLevel::Warning:
        color = QStringLiteral("#fb8c00");
        break;
    case ShredJob::LogLevel::Success:
        color = QStringLiteral("#43a047");
        break;
    case ShredJob::LogLevel::Info:
    case ShredJob::LogLevel::Output:
        break;
    }
    const QString escaped = text.toHtmlEscaped();
    m_log->appendHtml(color.isEmpty() ? escaped
                                      : QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color, escaped));
}

void MainWindow::onJobFinished()
{
    const ShredJob::Summary summary = m_job->summary();
    // Targets that no longer exist were shredded and removed - drop them from the list.
    for (int i = m_targetList->count() - 1; i >= 0; --i) {
        const QString path = m_targetList->item(i)->data(kPathRole).toString();
        if (ShredJob::targetType(path) == ShredJob::TargetType::Missing)
            delete m_targetList->takeItem(i);
    }
    refreshTargets();
    if (summary.ok())
        m_desktopProgress->finish(false);
    else if (summary.cancelled)
        m_desktopProgress->finish(true);
    else
        m_desktopProgress->finish(false, tr("Not everything could be shredded or removed."));
    if (summary.ok()) {
        statusBar()->showMessage(tr("Finished successfully."), 10000);
    } else if (!summary.cancelled) {
        QMessageBox::warning(this, tr("Finished with errors"),
                             tr("Not everything could be shredded or removed. See the log for details."));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_job && m_job->isRunning()) {
        const auto answer = QMessageBox::question(
            this, tr("Shredding in progress"),
            tr("An operation is in progress. Cancel it and quit?\n"
               "The current file may be only partially overwritten."));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        delete m_job; // the destructor kills the shred process
        m_desktopProgress->finish(true);
    }
    saveSettings();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!(m_job && m_job->isRunning()) && event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths << url.toString(QUrl::FullyEncoded);
    }
    if (paths.isEmpty())
        return;
    addTargets(paths);
    event->acceptProposedAction();
}
