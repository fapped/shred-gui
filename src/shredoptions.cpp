#include "shredoptions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {

const QString kJsonFormat = QStringLiteral("shred-gui-settings");

struct RemoveModeName
{
    ShredOptions::RemoveMode mode;
    const char *name;
};

constexpr RemoveModeName kRemoveModeNames[] = {
    {ShredOptions::RemoveMode::None, "none"},
    {ShredOptions::RemoveMode::Default, "default"},
    {ShredOptions::RemoveMode::Unlink, "unlink"},
    {ShredOptions::RemoveMode::Wipe, "wipe"},
    {ShredOptions::RemoveMode::WipeSync, "wipesync"},
};

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

// Reads an optional key of the expected type; a present key of a different type is an error.
bool readBool(const QJsonObject &json, const char *key, bool *out, QString *error)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (value.isUndefined())
        return true;
    if (!value.isBool()) {
        setError(error, QObject::tr("\"%1\" must be true or false.").arg(QLatin1String(key)));
        return false;
    }
    *out = value.toBool();
    return true;
}

bool readString(const QJsonObject &json, const char *key, QString *out, QString *error)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (value.isUndefined())
        return true;
    if (!value.isString()) {
        setError(error, QObject::tr("\"%1\" must be a string.").arg(QLatin1String(key)));
        return false;
    }
    *out = value.toString();
    return true;
}

} // namespace

bool ShredOptions::wipesNames() const
{
    return removeMode == RemoveMode::Default || removeMode == RemoveMode::Wipe
        || removeMode == RemoveMode::WipeSync;
}

bool ShredOptions::syncsNames() const
{
    return removeMode == RemoveMode::Default || removeMode == RemoveMode::WipeSync;
}

QStringList ShredOptions::arguments() const
{
    QStringList args;
    if (force)
        args << QStringLiteral("-f");
    args << QStringLiteral("-n") << QString::number(iterations);
    if (useRandomSource)
        args << QStringLiteral("--random-source") << randomSource;
    if (useSize)
        args << QStringLiteral("-s") << size.trimmed();
    switch (removeMode) {
    case RemoveMode::None:
        break;
    case RemoveMode::Default:
        args << QStringLiteral("-u");
        break;
    case RemoveMode::Unlink:
        args << QStringLiteral("--remove=unlink");
        break;
    case RemoveMode::Wipe:
        args << QStringLiteral("--remove=wipe");
        break;
    case RemoveMode::WipeSync:
        args << QStringLiteral("--remove=wipesync");
        break;
    }
    if (verbose)
        args << QStringLiteral("-v");
    if (exact)
        args << QStringLiteral("-x");
    if (zero)
        args << QStringLiteral("-z");
    return args;
}

QStringList ShredOptions::arguments(const QString &file) const
{
    // "--" prevents names starting with '-' from being interpreted as options.
    return arguments() << QStringLiteral("--") << file;
}

QString ShredOptions::validate() const
{
    if (program.trimmed().isEmpty())
        return QObject::tr("No shred program specified.");
    const QString resolved = program.contains(QLatin1Char('/'))
        ? program
        : QStandardPaths::findExecutable(program);
    const QFileInfo programInfo(resolved);
    if (resolved.isEmpty() || !programInfo.isFile() || !programInfo.isExecutable())
        return QObject::tr("Program \"%1\" does not exist or is not executable.").arg(program);
    if (iterations < 0 || iterations > MaxIterations)
        return QObject::tr("The number of passes must be between 0 and %1.").arg(MaxIterations);
    if (iterations == 0 && !zero)
        return QObject::tr("With 0 passes enable the final zero pass (-z), otherwise the file will not be overwritten.");
    if (useSize && !isValidSize(size))
        return QObject::tr("Invalid size \"%1\" (examples: 4096, 512K, 10M, 1GiB).").arg(size);
    if (useRandomSource) {
        const QFileInfo source(randomSource);
        if (randomSource.isEmpty() || !source.exists() || !source.isReadable())
            return QObject::tr("Random source \"%1\" does not exist or is not readable.").arg(randomSource);
    }
    return {};
}

void ShredOptions::save(QSettings &settings) const
{
    settings.beginGroup(QStringLiteral("options"));
    settings.setValue(QStringLiteral("program"), program);
    settings.setValue(QStringLiteral("iterations"), iterations);
    settings.setValue(QStringLiteral("force"), force);
    settings.setValue(QStringLiteral("verbose"), verbose);
    settings.setValue(QStringLiteral("exact"), exact);
    settings.setValue(QStringLiteral("zero"), zero);
    settings.setValue(QStringLiteral("useSize"), useSize);
    settings.setValue(QStringLiteral("size"), size);
    settings.setValue(QStringLiteral("useRandomSource"), useRandomSource);
    settings.setValue(QStringLiteral("randomSource"), randomSource);
    settings.setValue(QStringLiteral("removeMode"), static_cast<int>(removeMode));
    settings.endGroup();
}

ShredOptions ShredOptions::load(QSettings &settings)
{
    ShredOptions o;
    settings.beginGroup(QStringLiteral("options"));
    o.program = settings.value(QStringLiteral("program"), o.program).toString();
    o.iterations = qBound(0, settings.value(QStringLiteral("iterations"), o.iterations).toInt(), MaxIterations);
    o.force = settings.value(QStringLiteral("force"), o.force).toBool();
    o.verbose = settings.value(QStringLiteral("verbose"), o.verbose).toBool();
    o.exact = settings.value(QStringLiteral("exact"), o.exact).toBool();
    o.zero = settings.value(QStringLiteral("zero"), o.zero).toBool();
    o.useSize = settings.value(QStringLiteral("useSize"), o.useSize).toBool();
    o.size = settings.value(QStringLiteral("size"), o.size).toString();
    o.useRandomSource = settings.value(QStringLiteral("useRandomSource"), o.useRandomSource).toBool();
    o.randomSource = settings.value(QStringLiteral("randomSource"), o.randomSource).toString();
    const int mode = settings.value(QStringLiteral("removeMode"), static_cast<int>(o.removeMode)).toInt();
    if (mode >= static_cast<int>(RemoveMode::None) && mode <= static_cast<int>(RemoveMode::WipeSync))
        o.removeMode = static_cast<RemoveMode>(mode);
    settings.endGroup();
    return o;
}

QJsonObject ShredOptions::toJson() const
{
    return QJsonObject{
        {QStringLiteral("program"), program},
        {QStringLiteral("iterations"), iterations},
        {QStringLiteral("force"), force},
        {QStringLiteral("verbose"), verbose},
        {QStringLiteral("exact"), exact},
        {QStringLiteral("zero"), zero},
        {QStringLiteral("useSize"), useSize},
        {QStringLiteral("size"), size},
        {QStringLiteral("useRandomSource"), useRandomSource},
        {QStringLiteral("randomSource"), randomSource},
        {QStringLiteral("removeMode"), removeModeName(removeMode)},
    };
}

ShredOptions ShredOptions::fromJson(const QJsonObject &json, QString *error)
{
    return fromJson(json, error, ShredOptions());
}

ShredOptions ShredOptions::fromJson(const QJsonObject &json, QString *error, const ShredOptions &base)
{
    ShredOptions o = base;
    setError(error, QString());

    const bool ok = readString(json, "program", &o.program, error)
        && readBool(json, "force", &o.force, error)
        && readBool(json, "verbose", &o.verbose, error)
        && readBool(json, "exact", &o.exact, error)
        && readBool(json, "zero", &o.zero, error)
        && readBool(json, "useSize", &o.useSize, error)
        && readString(json, "size", &o.size, error)
        && readBool(json, "useRandomSource", &o.useRandomSource, error)
        && readString(json, "randomSource", &o.randomSource, error);
    if (!ok)
        return {};

    const QJsonValue iterations = json.value(QStringLiteral("iterations"));
    if (!iterations.isUndefined()) {
        const double value = iterations.toDouble(-1);
        if (!iterations.isDouble() || value != int(value) || value < 0 || value > MaxIterations) {
            setError(error, QObject::tr("\"iterations\" must be an integer between 0 and %1.").arg(MaxIterations));
            return {};
        }
        o.iterations = int(value);
    }

    const QJsonValue remove = json.value(QStringLiteral("removeMode"));
    if (!remove.isUndefined() && !removeModeFromName(remove.toString(), &o.removeMode)) {
        QStringList names;
        for (const RemoveModeName &entry : kRemoveModeNames)
            names << QLatin1String(entry.name);
        setError(error, QObject::tr("\"removeMode\" must be one of: %1.").arg(names.join(QStringLiteral(", "))));
        return {};
    }
    return o;
}

bool ShredOptions::saveToFile(const QString &path, QString *error) const
{
    const QJsonObject root{
        {QStringLiteral("format"), kJsonFormat},
        {QStringLiteral("version"), JsonFormatVersion},
        {QStringLiteral("options"), toJson()},
    };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QObject::tr("Cannot open \"%1\" for writing: %2").arg(path, file.errorString()));
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, QObject::tr("Cannot write \"%1\": %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

ShredOptions ShredOptions::loadFromFile(const QString &path, QString *error)
{
    return loadFromFile(path, error, ShredOptions());
}

ShredOptions ShredOptions::loadFromFile(const QString &path, QString *error, const ShredOptions &base,
                                        QString *name)
{
    setError(error, QString());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QObject::tr("Cannot open \"%1\": %2").arg(path, file.errorString()));
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, QObject::tr("\"%1\" is not valid JSON: %2 (offset %3)")
                            .arg(path, parseError.errorString())
                            .arg(parseError.offset));
        return {};
    }
    if (!doc.isObject()) {
        setError(error, QObject::tr("\"%1\" does not contain a JSON object.").arg(path));
        return {};
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != kJsonFormat) {
        setError(error, QObject::tr("\"%1\" is not a Shred GUI settings file.").arg(path));
        return {};
    }
    const int version = root.value(QStringLiteral("version")).toInt(0);
    if (version < 1 || version > JsonFormatVersion) {
        setError(error, QObject::tr("Unsupported settings file version %1 (supported: %2).")
                            .arg(version)
                            .arg(JsonFormatVersion));
        return {};
    }
    const QJsonValue options = root.value(QStringLiteral("options"));
    if (!options.isObject()) {
        setError(error, QObject::tr("\"%1\" has no \"options\" object.").arg(path));
        return {};
    }
    QString fileName;
    if (!readString(root, "name", &fileName, error))
        return {};
    const ShredOptions result = fromJson(options.toObject(), error, base);
    if (name)
        *name = fileName;
    return result;
}

QStringList ShredOptions::presetSearchPaths()
{
    QStringList paths;
    // User and system data directories ($XDG_DATA_HOME, $XDG_DATA_DIRS).
    for (const QString &dir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        paths << dir + QStringLiteral("/shred-gui/presets");
    // Relative to the executable: <prefix>/bin/../share/... for any install prefix, and the build directory.
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        paths << QDir::cleanPath(appDir + QStringLiteral("/../share/shred-gui/presets"))
              << appDir + QStringLiteral("/presets");
    }
    paths.removeDuplicates();
    return paths;
}

QString ShredOptions::locatePreset(const QString &name, const QStringList &searchPaths)
{
    if (name.isEmpty())
        return {};
    if (name.contains(QLatin1Char('/')))
        return QFileInfo(name).isFile() ? QFileInfo(name).absoluteFilePath() : QString();
    const QString fileName = name.endsWith(QStringLiteral(".json")) ? name : name + QStringLiteral(".json");
    for (const QString &dir : searchPaths) {
        const QFileInfo file(QDir(dir).filePath(fileName));
        if (file.isFile())
            return file.absoluteFilePath();
    }
    return {};
}

QMap<QString, QString> ShredOptions::availablePresets(const QStringList &searchPaths)
{
    QMap<QString, QString> presets;
    for (const QString &dir : searchPaths) {
        const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QFileInfo &file : files) {
            if (!presets.contains(file.completeBaseName()))
                presets.insert(file.completeBaseName(), file.absoluteFilePath());
        }
    }
    return presets;
}

bool ShredOptions::isValidSize(const QString &size)
{
    // Format accepted by both GNU shred and uutils shred.
    static const QRegularExpression re(QStringLiteral("^[0-9]+([kKMGTPEZY](i?B)?)?$"));
    return re.match(size.trimmed()).hasMatch();
}

QString ShredOptions::removeModeName(RemoveMode mode)
{
    for (const RemoveModeName &entry : kRemoveModeNames) {
        if (entry.mode == mode)
            return QLatin1String(entry.name);
    }
    return {};
}

bool ShredOptions::removeModeFromName(const QString &name, RemoveMode *mode)
{
    for (const RemoveModeName &entry : kRemoveModeNames) {
        if (name == QLatin1String(entry.name)) {
            *mode = entry.mode;
            return true;
        }
    }
    return false;
}

bool ShredOptions::operator==(const ShredOptions &other) const
{
    return program == other.program && iterations == other.iterations && force == other.force
        && verbose == other.verbose && exact == other.exact && zero == other.zero && useSize == other.useSize
        && size == other.size && useRandomSource == other.useRandomSource && randomSource == other.randomSource
        && removeMode == other.removeMode;
}

QString shellQuote(const QString &arg)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (!arg.isEmpty() && safe.match(arg).hasMatch())
        return arg;
    QString quoted = arg;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString commandLine(const QString &program, const QStringList &args)
{
    QStringList parts{shellQuote(program)};
    for (const QString &arg : args)
        parts << shellQuote(arg);
    return parts.join(QLatin1Char(' '));
}
