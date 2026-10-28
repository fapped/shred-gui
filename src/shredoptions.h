#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

class QSettings;
struct ShredPreset;

// All command line options of shred (GNU coreutils / uutils).
struct ShredOptions
{
    enum class RemoveMode {
        None,     // do not remove
        Default,  // -u (equivalent to --remove=wipesync in GNU)
        Unlink,   // --remove=unlink
        Wipe,     // --remove=wipe
        WipeSync, // --remove=wipesync
    };

    static constexpr int MaxIterations = 1000;
    static constexpr int JsonFormatVersion = 1;

    QString program = QStringLiteral("shred");
    int iterations = 3;           // -n
    bool force = false;           // -f
    bool verbose = true;          // -v
    bool exact = false;           // -x
    bool zero = false;            // -z
    bool useSize = false;         // -s
    QString size;
    bool useRandomSource = false; // --random-source
    QString randomSource;
    RemoveMode removeMode = RemoveMode::None;

    bool removesFiles() const { return removeMode != RemoveMode::None; }
    // Whether directory names should be obfuscated (renamed to zeros) before removal, like shred does.
    bool wipesNames() const;
    bool syncsNames() const;

    QStringList arguments() const;
    QStringList arguments(const QString &file) const;
    // The options that decide how the data is destroyed, e.g. "-n 3 -z -u" (for preset labels).
    QString summaryFlags() const;
    // "<name> (<summary flags>)", e.g. "Shred and delete permanently (-n 3 -z -u)".
    QString label(const QString &name) const;

    // Returns an empty string if the options are valid, otherwise an error description.
    QString validate() const;

    // Persistence between application runs.
    void save(QSettings &settings) const;
    static ShredOptions load(QSettings &settings);

    // Importable/exportable JSON configuration. Keys missing from the JSON keep the values from `base`
    // (the defaults unless specified). `name` receives the optional human readable name of the file.
    QJsonObject toJson() const;
    static ShredOptions fromJson(const QJsonObject &json, QString *error = nullptr);
    static ShredOptions fromJson(const QJsonObject &json, QString *error, const ShredOptions &base);
    bool saveToFile(const QString &path, QString *error = nullptr) const;
    static ShredOptions loadFromFile(const QString &path, QString *error = nullptr);
    static ShredOptions loadFromFile(const QString &path, QString *error, const ShredOptions &base,
                                     QString *name = nullptr);

    // Presets are JSON settings files shipped with the application (or provided by the user)
    // in <data dir>/shred-gui/presets/<name>.json. Earlier directories take precedence.
    static QStringList presetSearchPaths();
    // Returns the file of the given preset or an empty string. A name containing '/' is treated as a path.
    static QString locatePreset(const QString &name, const QStringList &searchPaths = presetSearchPaths());
    // Preset names (file names without .json) mapped to their files.
    static QMap<QString, QString> availablePresets(const QStringList &searchPaths = presetSearchPaths());
    // Available presets sorted by file name (id). Dolphin sorts the actions of a submenu by their ids
    // (not by Actions=), and the service menus use action ids in the same order as the preset ids.
    static QList<ShredPreset> presets(const QStringList &searchPaths = presetSearchPaths());

    static bool isValidSize(const QString &size);
    static QString removeModeName(RemoveMode mode);
    static bool removeModeFromName(const QString &name, RemoveMode *mode);

    bool operator==(const ShredOptions &other) const;
    bool operator!=(const ShredOptions &other) const { return !(*this == other); }
};

struct ShredPreset
{
    QString id;    // the file name without .json, used by --preset
    QString file;
    QString name;  // the "name" from the file, else the id
    QString label; // the name with the summary flags (the Dolphin label adds "…")
    QString error; // non-empty if the file cannot be loaded
    ShredOptions options; // the preset applied to the default options
};

QString shellQuote(const QString &arg);
QString commandLine(const QString &program, const QStringList &args);
