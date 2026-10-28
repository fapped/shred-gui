#include "localpath.h"
#include "shredjob.h"
#include "shredoptions.h"
#include "storagecheck.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSettings>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <limits>
#include <numeric>

#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(data), data.size());
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

QString tmpMissingDir()
{
    return QDir::tempPath() + QStringLiteral("/shred-gui-test-missing-dir");
}

void writeRaw(const QByteArray &path, const QByteArray &data)
{
    const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    QVERIFY(fd >= 0);
    QCOMPARE(::write(fd, data.constData(), data.size()), ssize_t(data.size()));
    ::close(fd);
}

QByteArray readRaw(const QByteArray &path)
{
    const int fd = ::open(path.constData(), O_RDONLY);
    if (fd < 0)
        return "<missing>";
    QByteArray data(4096, '\0');
    const ssize_t n = ::read(fd, data.data(), data.size());
    ::close(fd);
    return data.left(qMax<ssize_t>(0, n));
}

bool rawExists(const QByteArray &path)
{
    struct stat st;
    return ::lstat(path.constData(), &st) == 0;
}

// QTemporaryDir cannot remove such names (Qt converts them), so they are removed with raw bytes.
void removeTree(const QByteArray &path)
{
    struct stat st;
    if (::lstat(path.constData(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = ::opendir(path.constData())) {
            QList<QByteArray> children;
            while (dirent *entry = ::readdir(d)) {
                if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                    children << path + '/' + entry->d_name;
            }
            ::closedir(d);
            for (const QByteArray &child : std::as_const(children))
                removeTree(child);
        }
        ::rmdir(path.constData());
    } else {
        ::unlink(path.constData());
    }
}

bool runJob(ShredJob &job)
{
    QSignalSpy spy(&job, &ShredJob::finished);
    job.start();
    return spy.count() > 0 || spy.wait(60000);
}

} // namespace

class TestShredCore : public QObject
{
    Q_OBJECT

private slots:
    void defaultArguments();
    void allArguments();
    void removeModes_data();
    void removeModes();
    void sizeValidation_data();
    void sizeValidation();
    void quoting();
    void validateOptions();
    void scanRejectsSymlinkAndProtected();
    void scanDirectory();
    void shredDirectory_data();
    void shredDirectory();
    void shredFileWithoutRemoving();
    void missingProgramAborts();
    void jsonRoundTrip();
    void jsonPartialKeepsDefaults();
    void jsonRejectsInvalidValues_data();
    void jsonRejectsInvalidValues();
    void jsonFileRoundTrip();
    void jsonFileRejectsInvalidFiles_data();
    void jsonFileRejectsInvalidFiles();
    void settingsRoundTrip();
    void jsonMissingKeysKeepBase();
    void scanAllSkipsDuplicatesAndNested();
    void shredMultipleTargets();
    void locatePresets();
    void shippedPresets_data();
    void shippedPresets();
    void localPathRoundTrip_data();
    void localPathRoundTrip();
    void localPathRandomRoundTrip();
    void localPathUrlsAndQuoting();
    void shredNonUtf8Names_data();
    void shredNonUtf8Names();
    void processedBytes();
    void writtenBytesOfProcess();
    void summaryFlags();
    void presetNamesMatchDolphinMenu();
    void mountInfo();
    void flashDevices();
    void storageWarnings();
    void inspectRealPaths();
    void scanFindsHardLinks();
    void scanCanBeCancelled();
};

namespace {

ShredOptions customOptions()
{
    ShredOptions o;
    o.program = QStringLiteral("/usr/bin/gnushred");
    o.iterations = 7;
    o.force = true;
    o.verbose = false;
    o.exact = true;
    o.zero = true;
    o.useSize = true;
    o.size = QStringLiteral("10M");
    o.useRandomSource = true;
    o.randomSource = QStringLiteral("/dev/urandom");
    o.removeMode = ShredOptions::RemoveMode::WipeSync;
    return o;
}

} // namespace

void TestShredCore::defaultArguments()
{
    const ShredOptions o;
    QCOMPARE(o.arguments(QStringLiteral("-file")),
             QStringList({"-n", "3", "-v", "--", "-file"}));
}

void TestShredCore::allArguments()
{
    ShredOptions o;
    o.force = true;
    o.iterations = 7;
    o.useRandomSource = true;
    o.randomSource = QStringLiteral("/dev/urandom");
    o.useSize = true;
    o.size = QStringLiteral(" 10M ");
    o.removeMode = ShredOptions::RemoveMode::Wipe;
    o.verbose = true;
    o.exact = true;
    o.zero = true;
    QCOMPARE(o.arguments(), QStringList({"-f", "-n", "7", "--random-source", "/dev/urandom", "-s", "10M",
                                         "--remove=wipe", "-v", "-x", "-z"}));

    // Disabled options with values must not end up in the command.
    o.useRandomSource = false;
    o.useSize = false;
    o.verbose = false;
    QCOMPARE(o.arguments(), QStringList({"-f", "-n", "7", "--remove=wipe", "-x", "-z"}));
}

void TestShredCore::removeModes_data()
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<QString>("arg");
    QTest::addColumn<bool>("wipes");
    QTest::addColumn<bool>("syncs");
    QTest::newRow("none") << int(ShredOptions::RemoveMode::None) << QString() << false << false;
    QTest::newRow("default") << int(ShredOptions::RemoveMode::Default) << "-u" << true << true;
    QTest::newRow("unlink") << int(ShredOptions::RemoveMode::Unlink) << "--remove=unlink" << false << false;
    QTest::newRow("wipe") << int(ShredOptions::RemoveMode::Wipe) << "--remove=wipe" << true << false;
    QTest::newRow("wipesync") << int(ShredOptions::RemoveMode::WipeSync) << "--remove=wipesync" << true << true;
}

void TestShredCore::removeModes()
{
    QFETCH(int, mode);
    QFETCH(QString, arg);
    QFETCH(bool, wipes);
    QFETCH(bool, syncs);
    ShredOptions o;
    o.verbose = false;
    o.removeMode = static_cast<ShredOptions::RemoveMode>(mode);
    QStringList expected{"-n", "3"};
    if (!arg.isEmpty())
        expected << arg;
    QCOMPARE(o.arguments(), expected);
    QCOMPARE(o.wipesNames(), wipes);
    QCOMPARE(o.syncsNames(), syncs);
    QCOMPARE(o.removesFiles(), !arg.isEmpty());
}

void TestShredCore::sizeValidation_data()
{
    QTest::addColumn<QString>("size");
    QTest::addColumn<bool>("valid");
    for (const char *s : {"10", "1K", "1k", "1KB", "1KiB", "1M", "2G", "3TiB"})
        QTest::newRow(s) << QString::fromLatin1(s) << true;
    for (const char *s : {"", "K", "-1", "1B", "1c", "1.5M", "1 M", "abc"})
        QTest::newRow(*s ? s : "empty") << QString::fromLatin1(s) << false;
}

void TestShredCore::sizeValidation()
{
    QFETCH(QString, size);
    QFETCH(bool, valid);
    QCOMPARE(ShredOptions::isValidSize(size), valid);
}

void TestShredCore::quoting()
{
    QCOMPARE(shellQuote(QStringLiteral("/tmp/a.txt")), QStringLiteral("/tmp/a.txt"));
    QCOMPARE(shellQuote(QStringLiteral("a b")), QStringLiteral("'a b'"));
    QCOMPARE(shellQuote(QStringLiteral("it's")), QStringLiteral("'it'\\''s'"));
    QCOMPARE(shellQuote(QString()), QStringLiteral("''"));
    QCOMPARE(commandLine(QStringLiteral("shred"), {QStringLiteral("-n"), QStringLiteral("1"), QStringLiteral("x y")}),
             QStringLiteral("shred -n 1 'x y'"));
}

void TestShredCore::validateOptions()
{
    ShredOptions o;
    o.program = QStringLiteral("/bin/sh");
    QVERIFY(o.validate().isEmpty());
    o.iterations = 0;
    QVERIFY(!o.validate().isEmpty());
    o.zero = true;
    QVERIFY(o.validate().isEmpty());
    o.useSize = true;
    o.size = QStringLiteral("zzz");
    QVERIFY(!o.validate().isEmpty());
    o.useSize = false;
    o.useRandomSource = true;
    o.randomSource = QStringLiteral("/nonexistent/random");
    QVERIFY(!o.validate().isEmpty());
    o.useRandomSource = false;
    o.program = QStringLiteral("definitely-not-a-shred-binary");
    QVERIFY(!o.validate().isEmpty());
}

void TestShredCore::scanRejectsSymlinkAndProtected()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString real = tmp.filePath(QStringLiteral("real"));
    QVERIFY(QDir().mkdir(real));
    const QString link = tmp.filePath(QStringLiteral("link"));
    QVERIFY(QFile::link(real, link));

    QCOMPARE(ShredJob::targetType(link), ShredJob::TargetType::Symlink);
    QVERIFY(!ShredJob::scan(link).error.isEmpty());
    // A trailing slash must not make the symlink be treated as a directory.
    QVERIFY(!ShredJob::scan(link + QLatin1Char('/')).error.isEmpty());

    QVERIFY(!ShredJob::scan(QStringLiteral("/")).error.isEmpty());
    QVERIFY(!ShredJob::scan(QDir::homePath()).error.isEmpty());
    QVERIFY(!ShredJob::scan(QStringLiteral("/usr")).error.isEmpty());
    // Under sudo, the home of the user who ran it (here: the current user, with HOME pointing elsewhere).
    {
        const QByteArray oldHome = qgetenv("HOME");
        const QByteArray oldSudoUser = qgetenv("SUDO_USER");
        const QString realHome = QDir::homePath();
        const passwd *pw = ::getpwuid(::getuid());
        QVERIFY(pw);
        qputenv("HOME", QFile::encodeName(tmp.path()));
        QVERIFY(!ShredJob::isProtectedPath(LocalPath::decode(pw->pw_dir)) || realHome == tmp.path());
        qputenv("SUDO_USER", pw->pw_name);
        QVERIFY(ShredJob::isProtectedPath(LocalPath::decode(pw->pw_dir)));
        qputenv("HOME", oldHome);
        if (oldSudoUser.isEmpty())
            qunsetenv("SUDO_USER");
        else
            qputenv("SUDO_USER", oldSudoUser);
    }
    QVERIFY(!ShredJob::scan(tmp.filePath(QStringLiteral("missing"))).error.isEmpty());
}

void TestShredCore::scanDirectory()
{
    QTemporaryDir outside;
    QTemporaryDir tmp;
    QVERIFY(outside.isValid() && tmp.isValid());
    const QString victim = outside.filePath(QStringLiteral("victim"));
    writeFile(victim, "keep me");

    const QString root = tmp.filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/a/b/c")));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/empty")));
    writeFile(root + QStringLiteral("/top.txt"), "1");
    writeFile(root + QStringLiteral("/.hidden"), "2");
    writeFile(root + QStringLiteral("/a/b/c/deep.bin"), "3");
    QVERIFY(QFile::link(victim, root + QStringLiteral("/a/link-to-outside")));
    QVERIFY(QFile::link(outside.path(), root + QStringLiteral("/a/link-to-dir")));
    QVERIFY(QFile::link(QStringLiteral("/nonexistent"), root + QStringLiteral("/broken")));
    QCOMPARE(::mkfifo(QFile::encodeName(root + QStringLiteral("/fifo")).constData(), 0600), 0);

    const ShredJob::Plan plan = ShredJob::scan(root + QLatin1Char('/'));
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.target, root);
    QVERIFY(plan.isDirectory());

    QStringList files = plan.files;
    files.sort();
    QCOMPARE(files, QStringList({root + "/.hidden", root + "/a/b/c/deep.bin", root + "/top.txt"}));
    QStringList others = plan.others;
    others.sort();
    QCOMPARE(others, QStringList({root + "/a/link-to-dir", root + "/a/link-to-outside", root + "/broken",
                                  root + "/fifo"}));
    QCOMPARE(plan.dirs.size(), 5); // target, a, a/b, a/b/c, empty
    QCOMPARE(plan.dirs.constLast(), root);
    QVERIFY(plan.dirs.indexOf(root + "/a/b/c") < plan.dirs.indexOf(root + "/a/b"));
    QVERIFY(plan.dirs.indexOf(root + "/a/b") < plan.dirs.indexOf(root + "/a"));
    QVERIFY(plan.skipped.isEmpty());
}

void TestShredCore::shredDirectory_data()
{
    QTest::addColumn<QString>("program");
    QTest::addColumn<int>("mode");
    for (const char *program : {"shred", "gnushred"}) {
        if (QStandardPaths::findExecutable(QString::fromLatin1(program)).isEmpty())
            continue;
        QTest::addRow("%s-default", program) << QString::fromLatin1(program) << int(ShredOptions::RemoveMode::Default);
        QTest::addRow("%s-unlink", program) << QString::fromLatin1(program) << int(ShredOptions::RemoveMode::Unlink);
        QTest::addRow("%s-wipe", program) << QString::fromLatin1(program) << int(ShredOptions::RemoveMode::Wipe);
    }
}

void TestShredCore::shredDirectory()
{
    QFETCH(QString, program);
    QFETCH(int, mode);
    if (QStandardPaths::findExecutable(program).isEmpty())
        QSKIP("shred program not available");

    QTemporaryDir outside;
    QTemporaryDir tmp;
    QVERIFY(outside.isValid() && tmp.isValid());
    const QString victim = outside.filePath(QStringLiteral("victim"));
    writeFile(victim, "keep me");

    const QString root = tmp.filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/a/b")));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/empty")));
    writeFile(root + QStringLiteral("/-starts-with-dash"), QByteArray(5000, 'x'));
    writeFile(root + QStringLiteral("/with space.txt"), "secret");
    writeFile(root + QStringLiteral("/a/b/deep"), "secret");
    QVERIFY(QFile::link(victim, root + QStringLiteral("/a/link-to-outside")));
    QVERIFY(QFile::link(outside.path(), root + QStringLiteral("/link-to-dir")));

    ShredOptions options;
    options.program = program;
    options.iterations = 1;
    options.zero = true;
    options.removeMode = static_cast<ShredOptions::RemoveMode>(mode);

    ShredJob job(ShredJob::scan(root), options);
    QStringList log;
    connect(&job, &ShredJob::log, this, [&log](ShredJob::LogLevel, const QString &t) { log << t; });
    QVERIFY(runJob(job));
    QVERIFY2(job.summary().ok(), qPrintable(log.join('\n')));
    QCOMPARE(job.summary().filesShredded, 3);
    QCOMPARE(job.summary().othersRemoved, 2);
    QCOMPARE(job.summary().dirsRemoved, 4);

    QVERIFY(!QFileInfo::exists(root));
    QVERIFY(QDir(tmp.path()).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot));
    // The file the symlink points to must stay untouched.
    QCOMPARE(readFile(victim), QByteArray("keep me"));
}

void TestShredCore::shredFileWithoutRemoving()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString file = tmp.filePath(QStringLiteral("file.txt"));
    const QByteArray original(4096, 'A');
    writeFile(file, original);

    ShredOptions options;
    options.iterations = 1;
    options.zero = true;
    options.exact = true;

    ShredJob job(ShredJob::scan(file), options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().filesShredded, 1);
    const QByteArray after = readFile(file);
    QCOMPARE(after.size(), original.size());
    QCOMPARE(after, QByteArray(original.size(), '\0'));
}

void TestShredCore::missingProgramAborts()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString file = tmp.filePath(QStringLiteral("file.txt"));
    writeFile(file, "data");

    ShredOptions options;
    options.program = tmp.filePath(QStringLiteral("no-such-shred"));
    options.removeMode = ShredOptions::RemoveMode::Unlink;

    ShredJob job(ShredJob::scan(file), options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().aborted);
    QVERIFY(!job.summary().ok());
    QCOMPARE(readFile(file), QByteArray("data"));
}

void TestShredCore::jsonRoundTrip()
{
    const ShredOptions original = customOptions();
    const QJsonObject json = original.toJson();
    QCOMPARE(json.value("removeMode").toString(), QStringLiteral("wipesync"));
    QCOMPARE(json.value("iterations").toInt(), 7);

    QString error = QStringLiteral("not cleared");
    const ShredOptions restored = ShredOptions::fromJson(json, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(restored == original);

    for (int mode = int(ShredOptions::RemoveMode::None); mode <= int(ShredOptions::RemoveMode::WipeSync); ++mode) {
        ShredOptions o;
        o.removeMode = static_cast<ShredOptions::RemoveMode>(mode);
        QCOMPARE(ShredOptions::fromJson(o.toJson()).removeMode, o.removeMode);
    }
}

void TestShredCore::jsonPartialKeepsDefaults()
{
    QString error;
    const ShredOptions o = ShredOptions::fromJson(QJsonObject{{"iterations", 1}, {"unknownKey", "ignored"}}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    ShredOptions expected;
    expected.iterations = 1;
    QVERIFY(o == expected);
}

void TestShredCore::jsonRejectsInvalidValues_data()
{
    QTest::addColumn<QJsonObject>("json");
    QTest::newRow("bool as string") << QJsonObject{{"zero", "yes"}};
    QTest::newRow("program as number") << QJsonObject{{"program", 5}};
    QTest::newRow("negative iterations") << QJsonObject{{"iterations", -1}};
    QTest::newRow("too many iterations") << QJsonObject{{"iterations", ShredOptions::MaxIterations + 1}};
    QTest::newRow("fractional iterations") << QJsonObject{{"iterations", 1.5}};
    QTest::newRow("iterations as string") << QJsonObject{{"iterations", "3"}};
    QTest::newRow("unknown remove mode") << QJsonObject{{"removeMode", "shred-harder"}};
    QTest::newRow("remove mode as number") << QJsonObject{{"removeMode", 2}};
}

void TestShredCore::jsonRejectsInvalidValues()
{
    QFETCH(QJsonObject, json);
    QString error;
    ShredOptions::fromJson(json, &error);
    QVERIFY(!error.isEmpty());
}

void TestShredCore::jsonFileRoundTrip()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("settings.json"));
    const ShredOptions original = customOptions();

    QString error;
    QVERIFY2(original.saveToFile(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(root.value("format").toString(), QStringLiteral("shred-gui-settings"));
    QCOMPARE(root.value("version").toInt(), ShredOptions::JsonFormatVersion);
    QVERIFY(root.value("options").isObject());

    const ShredOptions restored = ShredOptions::loadFromFile(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(restored == original);

    QVERIFY(!original.saveToFile(tmp.filePath(QStringLiteral("missing-dir/settings.json")), &error));
    QVERIFY(!error.isEmpty());
}

void TestShredCore::jsonFileRejectsInvalidFiles_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("not json") << QByteArray("{ this is not json");
    QTest::newRow("array") << QByteArray("[1, 2, 3]");
    QTest::newRow("missing format") << QByteArray(R"({"version": 1, "options": {}})");
    QTest::newRow("wrong format") << QByteArray(R"({"format": "other-app", "version": 1, "options": {}})");
    QTest::newRow("newer version") << QByteArray(R"({"format": "shred-gui-settings", "version": 99, "options": {}})");
    QTest::newRow("missing options") << QByteArray(R"({"format": "shred-gui-settings", "version": 1})");
    QTest::newRow("invalid option")
        << QByteArray(R"({"format": "shred-gui-settings", "version": 1, "options": {"force": 1}})");
}

void TestShredCore::jsonFileRejectsInvalidFiles()
{
    QFETCH(QByteArray, content);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("settings.json"));
    writeFile(path, content);

    QString error;
    ShredOptions::loadFromFile(path, &error);
    QVERIFY(!error.isEmpty());

    ShredOptions::loadFromFile(tmp.filePath(QStringLiteral("does-not-exist.json")), &error);
    QVERIFY(!error.isEmpty());
}

void TestShredCore::settingsRoundTrip()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("settings.ini"));
    const ShredOptions original = customOptions();
    {
        QSettings settings(path, QSettings::IniFormat);
        original.save(settings);
    }
    QSettings settings(path, QSettings::IniFormat);
    QVERIFY(ShredOptions::load(settings) == original);
}

void TestShredCore::jsonMissingKeysKeepBase()
{
    ShredOptions base = customOptions();
    QString error;
    const ShredOptions o = ShredOptions::fromJson(QJsonObject{{"iterations", 1}, {"removeMode", "none"}}, &error, base);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    base.iterations = 1;
    base.removeMode = ShredOptions::RemoveMode::None;
    QVERIFY(o == base);
}

void TestShredCore::scanAllSkipsDuplicatesAndNested()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("dir"));
    QVERIFY(QDir().mkpath(dir + QStringLiteral("/sub")));
    writeFile(dir + QStringLiteral("/sub/inner.txt"), "x");
    const QString outsideFile = tmp.filePath(QStringLiteral("outside.txt"));
    writeFile(outsideFile, "y");
    const QString missing = tmp.filePath(QStringLiteral("missing"));

    QStringList notes;
    const QList<ShredJob::Plan> plans = ShredJob::scanAll(
        {dir + QStringLiteral("/sub/inner.txt"), dir, outsideFile, dir + QStringLiteral("/"), dir + QStringLiteral("/sub"),
         missing},
        &notes);

    QStringList planTargets;
    for (const ShredJob::Plan &plan : plans)
        planTargets << plan.target;
    QCOMPARE(planTargets, QStringList({dir, outsideFile, missing}));
    QVERIFY(!plans.at(2).error.isEmpty());
    QCOMPARE(notes.size(), 3); // inner.txt and sub are nested, "dir/" is a duplicate
}

void TestShredCore::shredMultipleTargets()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString first = tmp.filePath(QStringLiteral("first.txt"));
    const QString second = tmp.filePath(QStringLiteral("second file.txt"));
    const QString dir = tmp.filePath(QStringLiteral("dir"));
    writeFile(first, "1");
    writeFile(second, "2");
    QVERIFY(QDir().mkpath(dir + QStringLiteral("/a")));
    writeFile(dir + QStringLiteral("/a/x"), "3");
    writeFile(dir + QStringLiteral("/y"), "4");

    ShredOptions options;
    options.iterations = 1;
    options.removeMode = ShredOptions::RemoveMode::Default;

    ShredJob job(ShredJob::scanAll({first, dir, second}), options);
    QList<int> progress;
    connect(&job, &ShredJob::progress, this, [&progress](int value) { progress << value; });
    QStringList log;
    connect(&job, &ShredJob::log, this, [&log](ShredJob::LogLevel, const QString &t) { log << t; });
    QVERIFY(runJob(job));
    QVERIFY2(job.summary().ok(), qPrintable(log.join('\n')));
    QCOMPARE(job.summary().filesShredded, 4);
    QCOMPARE(job.summary().dirsRemoved, 2);
    QVERIFY(QDir(tmp.path()).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot));

    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.constLast(), 1000);
    QVERIFY(std::is_sorted(progress.cbegin(), progress.cend()));
}

void TestShredCore::locatePresets()
{
    QTemporaryDir user;
    QTemporaryDir system;
    QVERIFY(user.isValid() && system.isValid());
    writeFile(system.filePath(QStringLiteral("common.json")), "{}");
    writeFile(system.filePath(QStringLiteral("system-only.json")), "{}");
    writeFile(user.filePath(QStringLiteral("common.json")), "{}");
    writeFile(user.filePath(QStringLiteral("not-a-preset.txt")), "");
    const QStringList dirs{user.path(), tmpMissingDir(), system.path()};

    QCOMPARE(ShredOptions::locatePreset(QStringLiteral("common"), dirs), user.filePath(QStringLiteral("common.json")));
    QCOMPARE(ShredOptions::locatePreset(QStringLiteral("common.json"), dirs), user.filePath(QStringLiteral("common.json")));
    QCOMPARE(ShredOptions::locatePreset(QStringLiteral("system-only"), dirs),
             system.filePath(QStringLiteral("system-only.json")));
    QVERIFY(ShredOptions::locatePreset(QStringLiteral("nope"), dirs).isEmpty());
    QVERIFY(ShredOptions::locatePreset(QString(), dirs).isEmpty());
    // A path is used directly instead of searching the preset directories.
    QCOMPARE(ShredOptions::locatePreset(system.filePath(QStringLiteral("common.json")), dirs),
             system.filePath(QStringLiteral("common.json")));

    const QMap<QString, QString> presets = ShredOptions::availablePresets(dirs);
    QCOMPARE(presets.keys(), QStringList({"common", "system-only"}));
    QCOMPARE(presets.value("common"), user.filePath(QStringLiteral("common.json")));
}

void TestShredCore::shippedPresets_data()
{
    QTest::addColumn<QString>("preset");
    QTest::addColumn<int>("mode");
    QTest::newRow("delete") << "shred-delete" << int(ShredOptions::RemoveMode::Default);
    QTest::newRow("overwrite") << "shred-overwrite" << int(ShredOptions::RemoveMode::None);
}

void TestShredCore::shippedPresets()
{
    QFETCH(QString, preset);
    QFETCH(int, mode);
    const QString file = ShredOptions::locatePreset(preset, {QStringLiteral(SHRED_GUI_SOURCE_PRESETS_DIR)});
    QVERIFY(!file.isEmpty());

    // Presets must not choose the program - the user's choice is kept.
    ShredOptions base;
    base.program = QStringLiteral("/opt/custom/shred");
    QString error;
    QString name;
    const ShredOptions o = ShredOptions::loadFromFile(file, &error, base, &name);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!name.isEmpty());
    QCOMPARE(o.program, base.program);
    QCOMPARE(int(o.removeMode), mode);
    QCOMPARE(o.iterations, 3);
    QVERIFY(o.force && o.zero && o.verbose);
}

void TestShredCore::localPathRoundTrip_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("plain");
    QTest::addColumn<QString>("display");
    QTest::newRow("ascii") << QByteArray("/tmp/a b") << true << "/tmp/a b";
    QTest::newRow("utf-8") << QByteArray("/tmp/na\xc3\xafve") << true << QString::fromUtf8("/tmp/naïve");
    QTest::newRow("emoji") << QByteArray("\xf0\x9f\x98\x80") << true << QString::fromUtf8("\xf0\x9f\x98\x80");
    QTest::newRow("latin-1 byte") << QByteArray("/tmp/caf\xe9") << false << "/tmp/caf\\xe9";
    QTest::newRow("0xff") << QByteArray("\xff\xfe") << false << "\\xff\\xfe";
    QTest::newRow("overlong") << QByteArray("\xc0\xaf") << false << "\\xc0\\xaf";
    QTest::newRow("surrogate") << QByteArray("\xed\xa0\x80") << false << "\\xed\\xa0\\x80";
    QTest::newRow("truncated") << QByteArray("a\xe2\x82") << false << "a\\xe2\\x82";
    QTest::newRow("above U+10FFFF") << QByteArray("\xf4\x90\x80\x80") << false << "\\xf4\\x90\\x80\\x80";
    // U+F780..U+F7FF are the escape characters themselves - stored escaped, so they survive too.
    QTest::newRow("escape range") << QByteArray("\xef\x9e\x80") << false << "\\xef\\x9e\\x80";
    QTest::newRow("empty") << QByteArray() << true << "";
}

void TestShredCore::localPathRoundTrip()
{
    QFETCH(QByteArray, bytes);
    QFETCH(bool, plain);
    QFETCH(QString, display);
    const QString decoded = LocalPath::decode(bytes);
    QCOMPARE(LocalPath::encode(decoded), bytes);
    QCOMPARE(LocalPath::isPlain(decoded), plain);
    QCOMPARE(LocalPath::display(decoded), display);
    if (plain)
        QCOMPARE(decoded, QString::fromUtf8(bytes));
}

void TestShredCore::localPathRandomRoundTrip()
{
    QRandomGenerator random(20261028);
    for (int i = 0; i < 20000; ++i) {
        QByteArray bytes(int(random.bounded(24)), '\0');
        for (char &c : bytes)
            c = char(random.bounded(i % 2 ? 256 : 64) + (i % 2 ? 0 : 0xC0)); // half biased to UTF-8 lead bytes
        QCOMPARE(LocalPath::encode(LocalPath::decode(bytes)), bytes);
    }
}

void TestShredCore::localPathUrlsAndQuoting()
{
    // file:// URLs (Dolphin, drag & drop, clipboard) keep the bytes - QUrl::toLocalFile() would not.
    QCOMPARE(LocalPath::encode(LocalPath::fromFileUrl(QStringLiteral("file:///tmp/a%FFb%20c"))),
             QByteArray("/tmp/a\xff" "b c"));
    QCOMPARE(LocalPath::fromFileUrl(QStringLiteral("file:///tmp/na%C3%AF")), QString::fromUtf8("/tmp/naï"));

    // The command line in the log uses bash's $'...' for such names, so it can be pasted into a shell.
    QCOMPARE(shellQuote(LocalPath::decode("/tmp/a\xff'b\\c")), QStringLiteral("$'/tmp/a\\xff\\'b\\\\c'"));
}

void TestShredCore::shredNonUtf8Names_data()
{
    QTest::addColumn<QString>("program");
    for (const char *program : {"shred", "gnushred"}) {
        if (!QStandardPaths::findExecutable(QString::fromLatin1(program)).isEmpty())
            QTest::newRow(program) << QString::fromLatin1(program);
    }
}

void TestShredCore::shredNonUtf8Names()
{
    QFETCH(QString, program);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QByteArray base = QFile::encodeName(tmp.path());
    const QByteArray dir = base + "/dir\xff";
    const QByteArray single = base + "/single\xfc";
    const auto cleanup = qScopeGuard([&] { removeTree(dir); removeTree(single); });
    QCOMPARE(::mkdir(dir.constData(), 0755), 0);
    QCOMPARE(::mkdir((dir + "/sub\xfd").constData(), 0755), 0);
    writeRaw(dir + "/f\xfe.txt", "secret");
    writeRaw(dir + "/sub\xfd/g", "secret");
    QCOMPARE(::symlink("/nonexistent", (dir + "/link\xfb").constData()), 0);
    writeRaw(single, QByteArray(4096, 'A'));

    const QString target = LocalPath::decode(dir);
    QCOMPARE(ShredJob::targetType(target), ShredJob::TargetType::Directory);
    const ShredJob::Plan plan = ShredJob::scan(target);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.target, target);
    QCOMPARE(plan.files.size(), 2);
    QCOMPARE(plan.others.size(), 1);
    QCOMPARE(plan.dirs.size(), 2);
    QVERIFY(plan.files.contains(LocalPath::decode(dir + "/f\xfe.txt")));

    // A file overwritten in place: shred gets the exact bytes of the name.
    ShredOptions keep;
    keep.program = program;
    keep.iterations = 1;
    keep.zero = true;
    keep.exact = true;
    ShredJob overwrite(ShredJob::scan(LocalPath::decode(single)), keep);
    QStringList log;
    connect(&overwrite, &ShredJob::log, this, [&log](ShredJob::LogLevel, const QString &t) { log << t; });
    QVERIFY(runJob(overwrite));
    QVERIFY2(overwrite.summary().ok(), qPrintable(log.join('\n')));
    QCOMPARE(readRaw(single), QByteArray(4096, '\0'));
    // The command line in the log shows the name with \xNN.
    QVERIFY2(log.first().contains(QStringLiteral("single\\xfc")), qPrintable(log.first()));

    // A directory shredded and removed, including the names of its subdirectories.
    ShredOptions remove = keep;
    remove.removeMode = ShredOptions::RemoveMode::Wipe;
    ShredJob job(plan, remove);
    log.clear();
    connect(&job, &ShredJob::log, this, [&log](ShredJob::LogLevel, const QString &t) { log << t; });
    QVERIFY(runJob(job));
    QVERIFY2(job.summary().ok(), qPrintable(log.join('\n')));
    QCOMPARE(job.summary().filesShredded, 2);
    QCOMPARE(job.summary().othersRemoved, 1);
    QCOMPARE(job.summary().dirsRemoved, 2);
    QVERIFY(!rawExists(dir));
}

void TestShredCore::processedBytes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("dir"));
    QVERIFY(QDir().mkpath(dir + QStringLiteral("/sub")));
    writeFile(dir + QStringLiteral("/a"), QByteArray(1000, 'a'));
    writeFile(dir + QStringLiteral("/sub/b"), QByteArray(234, 'b'));
    const QString file = tmp.filePath(QStringLiteral("file"));
    writeFile(file, QByteArray(5, 'f'));

    const ShredJob::Plan plan = ShredJob::scan(dir);
    QCOMPARE(plan.fileSizes.size(), plan.files.size());
    QCOMPARE(std::accumulate(plan.fileSizes.cbegin(), plan.fileSizes.cend(), qint64(0)), qint64(1234));

    ShredOptions options;
    options.iterations = 2;
    options.zero = true; // 3 passes
    options.removeMode = ShredOptions::RemoveMode::Unlink;
    ShredJob job(QList<ShredJob::Plan>{plan, ShredJob::scan(file)}, options);
    QCOMPARE(job.totalBytes(), qint64(1239 * 3));

    QList<qint64> processed;
    connect(&job, &ShredJob::processedBytesChanged, this, [&processed](qint64 bytes) { processed << bytes; });
    QList<int> progress;
    connect(&job, &ShredJob::progress, this, [&progress](int permille) { progress << permille; });
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QVERIFY(!processed.isEmpty());
    QVERIFY(std::is_sorted(processed.cbegin(), processed.cend()));
    QCOMPARE(processed.last(), job.totalBytes());
    // The progress follows the amount written.
    QVERIFY(std::is_sorted(progress.cbegin(), progress.cend()));
    QCOMPARE(progress.last(), 1000);
}

void TestShredCore::writtenBytesOfProcess()
{
    const qint64 pid = QCoreApplication::applicationPid();
    const qint64 before = ShredJob::writtenBytes(pid);
    QVERIFY(before >= 0);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath(QStringLiteral("data")), QByteArray(1 << 20, 'x'));
    QVERIFY(ShredJob::writtenBytes(pid) >= before + (1 << 20));
    QCOMPARE(ShredJob::writtenBytes(0), qint64(-1));
    QCOMPARE(ShredJob::writtenBytes(std::numeric_limits<int>::max()), qint64(-1)); // no such process
}

void TestShredCore::summaryFlags()
{
    ShredOptions o;
    QCOMPARE(o.summaryFlags(), QStringLiteral("-n 3"));
    o.zero = true;
    o.removeMode = ShredOptions::RemoveMode::Default;
    QCOMPARE(o.label(QStringLiteral("Delete")), QStringLiteral("Delete (-n 3 -z -u)"));
    o.iterations = 1;
    o.useSize = true;
    o.size = QStringLiteral(" 1M ");
    o.removeMode = ShredOptions::RemoveMode::Wipe;
    o.force = true; // -f, -v, -x and the random source do not decide what is destroyed
    o.exact = true;
    QCOMPARE(o.summaryFlags(), QStringLiteral("-n 1 -z -s 1M --remove=wipe"));
}

void TestShredCore::presetNamesMatchDolphinMenu()
{
    const QList<ShredPreset> presets = ShredOptions::presets({QStringLiteral(SHRED_GUI_SOURCE_PRESETS_DIR)});
    QCOMPARE(presets.size(), 2);
    for (const ShredPreset &preset : presets) {
        QVERIFY2(preset.error.isEmpty(), qPrintable(preset.error));
        QVERIFY2(preset.name != preset.id, qPrintable(preset.id)); // every shipped preset has a name
    }

    // Every Dolphin action is "<preset label>…" and runs that preset; every preset has an action.
    // Dolphin shows the actions of the Shred submenu sorted by their ids, so the action ids must be
    // in the same order as the preset ids (the order of File > Load preset).
    // (QSettings cannot read .desktop files: ';' starts a comment there.)
    QMap<QString, QPair<QString, QString>> actions; // id -> (Name, Exec)
    const QDir dir(QStringLiteral(SHRED_GUI_SOURCE_SERVICEMENUS_DIR));
    for (const QString &fileName : dir.entryList({QStringLiteral("*.desktop.in")}, QDir::Files)) {
        QString group;
        QHash<QString, QString> entries; // "group/key" -> value
        const QStringList lines = QString::fromUtf8(readFile(dir.filePath(fileName))).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
                group = line.mid(1, line.size() - 2);
            else if (const qsizetype eq = line.indexOf(QLatin1Char('=')); eq > 0)
                entries.insert(group + QLatin1Char('/') + line.left(eq), line.mid(eq + 1));
        }
        QCOMPARE(entries.value(QStringLiteral("Desktop Entry/X-KDE-Submenu")), QStringLiteral("Shred"));
        const QStringList ids = entries.value(QStringLiteral("Desktop Entry/Actions")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString &id : ids) {
            const QString prefix = QStringLiteral("Desktop Action ") + id + QLatin1Char('/');
            actions.insert(id, {entries.value(prefix + QStringLiteral("Name")), entries.value(prefix + QStringLiteral("Exec"))});
        }
    }
    QCOMPARE(actions.size(), presets.size());
    qsizetype i = 0;
    for (auto it = actions.cbegin(); it != actions.cend(); ++it, ++i) {
        QCOMPARE(it.value().first, presets.at(i).label + QStringLiteral("…"));
        QVERIFY2(it.value().second.contains(QStringLiteral("--preset ") + presets.at(i).id + QLatin1Char(' ')),
                 qPrintable(it.value().second));
    }
}

void TestShredCore::mountInfo()
{
    const QByteArray text =
        "22 1 253:1 / / rw,relatime shared:1 - ext4 /dev/mapper/root rw\n"
        "30 22 0:24 / /tmp rw,noatime shared:5 - tmpfs tmpfs rw,inode64\n"
        "40 22 0:38 / /media/my\\040share rw,relatime shared:11 - autofs systemd-1 rw,fd=33\n"
        "41 40 0:57 / /media/my\\040share rw,relatime shared:12 - cifs //server/share rw,vers=3.1.1\n"
        "50 22 0:60 /@home /home rw,relatime shared:30 - btrfs /dev/nvme0n1p3 rw,subvol=/@home\n"
        "garbage line\n";

    // By the device number.
    StorageCheck::Info info = StorageCheck::parseMountInfo(text, 253, 1, QStringLiteral("/etc/fstab"));
    QCOMPARE(info.fileSystem, QStringLiteral("ext4"));
    QCOMPARE(info.source, QStringLiteral("/dev/mapper/root"));
    QCOMPARE(info.mountPoint, QStringLiteral("/"));
    // A mount point with an escaped space; the later mount over autofs wins.
    info = StorageCheck::parseMountInfo(text, 0, 57, QStringLiteral("/media/my share/a.txt"));
    QCOMPARE(info.fileSystem, QStringLiteral("cifs"));
    QCOMPARE(info.mountPoint, QStringLiteral("/media/my share"));
    // A btrfs subvolume that is not mounted on its own (another device number): the longest prefix.
    info = StorageCheck::parseMountInfo(text, 0, 77, QStringLiteral("/home/user/.snapshots/1/x"));
    QCOMPARE(info.fileSystem, QStringLiteral("btrfs"));
    QCOMPARE(info.source, QStringLiteral("/dev/nvme0n1p3"));
    // "/tmpfoo" is not under "/tmp".
    info = StorageCheck::parseMountInfo(text, 0, 99, QStringLiteral("/tmpfoo/x"));
    QCOMPARE(info.fileSystem, QStringLiteral("ext4"));
    QVERIFY(StorageCheck::parseMountInfo(QByteArray(), 253, 1, QStringLiteral("/")).fileSystem.isEmpty());
}

void TestShredCore::flashDevices()
{
    // A fake /sys: disks with partitions, a device mapper device on a hard disk and on an SSD, a USB stick.
    QTemporaryDir sys;
    QVERIFY(sys.isValid());
    auto mkdev = [&](const QString &dir, const QByteArray &rotational, const QByteArray &removable) {
        QVERIFY(QDir().mkpath(sys.filePath(dir + QStringLiteral("/queue"))));
        writeFile(sys.filePath(dir + QStringLiteral("/queue/rotational")), rotational + '\n');
        writeFile(sys.filePath(dir + QStringLiteral("/removable")), removable + '\n');
    };
    mkdev(QStringLiteral("sda"), "1", "0");
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("sda/sda1"))));
    mkdev(QStringLiteral("nvme0n1"), "0", "0");
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("nvme0n1/nvme0n1p2"))));
    mkdev(QStringLiteral("sdb"), "1", "1");
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("sdb/sdb1"))));
    mkdev(QStringLiteral("dm-0"), "0", "0"); // claims to be non-rotational
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("dm-0/slaves"))));
    QVERIFY(QFile::link(sys.filePath(QStringLiteral("sda/sda1")), sys.filePath(QStringLiteral("dm-0/slaves/sda1"))));
    mkdev(QStringLiteral("dm-1"), "1", "0");
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("dm-1/slaves"))));
    QVERIFY(QFile::link(sys.filePath(QStringLiteral("sda/sda1")), sys.filePath(QStringLiteral("dm-1/slaves/sda1"))));
    QVERIFY(QFile::link(sys.filePath(QStringLiteral("nvme0n1/nvme0n1p2")),
                        sys.filePath(QStringLiteral("dm-1/slaves/nvme0n1p2"))));

    QVERIFY(!StorageCheck::isFlashDevice(sys.filePath(QStringLiteral("sda"))));
    QVERIFY(!StorageCheck::isFlashDevice(sys.filePath(QStringLiteral("sda/sda1"))));
    QVERIFY(StorageCheck::isFlashDevice(sys.filePath(QStringLiteral("nvme0n1/nvme0n1p2"))));
    QVERIFY(StorageCheck::isFlashDevice(sys.filePath(QStringLiteral("sdb/sdb1"))));     // removable
    QVERIFY(!StorageCheck::isFlashDevice(sys.filePath(QStringLiteral("dm-0"))));        // decided by sda1
    QVERIFY(StorageCheck::isFlashDevice(sys.filePath(QStringLiteral("dm-1"))));         // RAID with an SSD
    QVERIFY(!StorageCheck::isFlashDevice(sys.filePath(QStringLiteral("missing"))));
}

void TestShredCore::storageWarnings()
{
    StorageCheck::Info info;
    info.fileSystem = QStringLiteral("ext4");
    info.mountPoint = QStringLiteral("/data");
    QVERIFY(StorageCheck::warnings(info).isEmpty());
    info.flash = true;
    QStringList warnings = StorageCheck::warnings(info);
    QCOMPARE(warnings.size(), 1);
    QVERIFY(warnings.first().startsWith(QStringLiteral("/data (ext4): flash storage")));

    info.fileSystem = QStringLiteral("btrfs");
    warnings = StorageCheck::warnings(info);
    QCOMPARE(warnings.size(), 2);
    QVERIFY(warnings.first().contains(QStringLiteral("copy-on-write")));

    info.flash = false;
    for (const char *fs : {"zfs", "bcachefs", "f2fs", "nfs4", "cifs", "fuse.sshfs", "overlay", "fuse.gocryptfs"}) {
        info.fileSystem = QString::fromLatin1(fs);
        QCOMPARE(StorageCheck::warnings(info).size(), 1);
    }
    for (const char *fs : {"ext4", "xfs", "vfat", "exfat", "ntfs3", "fuseblk", "tmpfs", ""}) {
        info.fileSystem = QString::fromLatin1(fs);
        QVERIFY2(StorageCheck::warnings(info).isEmpty(), fs);
    }

    // A block device selected directly is named by its device.
    StorageCheck::Info device;
    device.source = QStringLiteral("/dev/sdb");
    device.flash = true;
    QVERIFY(StorageCheck::warnings(device).first().startsWith(QStringLiteral("/dev/sdb: flash storage")));
}

void TestShredCore::inspectRealPaths()
{
    // Whatever the machine has, the root file system is found.
    const StorageCheck::Info root = StorageCheck::inspect(QStringLiteral("/"));
    QVERIFY(!root.fileSystem.isEmpty());
    QCOMPARE(root.mountPoint, QStringLiteral("/"));
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(!StorageCheck::inspect(tmp.path()).fileSystem.isEmpty());
    QVERIFY(StorageCheck::inspect(tmp.filePath(QStringLiteral("missing"))).fileSystem.isEmpty());
    // /proc is not on a block device.
    const StorageCheck::Info proc = StorageCheck::inspect(QStringLiteral("/proc/self"));
    QCOMPARE(proc.fileSystem, QStringLiteral("proc"));
    QVERIFY(!proc.flash);
}

void TestShredCore::scanFindsHardLinks()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkpath(root));
    writeFile(root + QStringLiteral("/single"), "1");
    writeFile(root + QStringLiteral("/linked"), "2");
    const QString outsideName = tmp.filePath(QStringLiteral("other-name"));
    QCOMPARE(::link(QFile::encodeName(root + QStringLiteral("/linked")).constData(),
                    QFile::encodeName(outsideName).constData()), 0);

    const ShredJob::Plan dir = ShredJob::scan(root);
    QVERIFY2(dir.error.isEmpty(), qPrintable(dir.error));
    QCOMPARE(dir.files.size(), 2);
    QCOMPARE(dir.hardLinked, QStringList{root + QStringLiteral("/linked")});

    QCOMPARE(ShredJob::scan(outsideName).hardLinked, QStringList{outsideName});
    QVERIFY(ShredJob::scan(root + QStringLiteral("/single")).hardLinked.isEmpty());
}

void TestShredCore::scanCanBeCancelled()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.filePath(QStringLiteral("target"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/sub")));
    for (int i = 0; i < 5; ++i)
        writeFile(root + QStringLiteral("/sub/f%1").arg(i), "x");

    ShredJob::ScanControl control;
    QList<ShredJob::Plan> plans = ShredJob::scanAll({root}, nullptr, &control);
    QCOMPARE(plans.size(), 1);
    QCOMPARE(plans.first().files.size(), 5);
    QCOMPARE(control.entries.load(), 6); // sub + 5 files

    control.cancelled = true;
    QVERIFY(ShredJob::scanAll({root}, nullptr, &control).isEmpty());
    const ShredJob::Plan plan = ShredJob::scan(root, &control);
    QVERIFY(!plan.error.isEmpty());
}

QTEST_GUILESS_MAIN(TestShredCore)
#include "tst_shredcore.moc"
