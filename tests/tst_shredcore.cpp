#include "combinedprogress.h"
#include "deviceerase.h"
#include "filelisting.h"
#include "localpath.h"
#include "runlog.h"
#include "shredjob.h"
#include "shredoptions.h"
#include "speedhistory.h"
#include "storagecheck.h"

#include <QDir>
#include <QJsonDocument>
#include <QProcess>
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
    void deviceStack();
    void flashWarnings();
    void trimPlans();
    void trimAfterShredding();
    void trimOption();
    void lsblkParsing();
    void eraseMethods();
    void eraseScripts();
    void inspectRealPaths();
    void runLogFiles();
    void scanFindsHardLinks();
    void scanFindsUnwritableDirs();
    void scanCanBeCancelled();
    void combinedProgressWeightsBySize();
    void combinedProgressExpires();
    void speedHistoryAverages();
    void speedHistoryGaps();
    void speedHistoryMergesBuckets();
    void speedHistoryNiceScale_data();
    void speedHistoryNiceScale();
    void fileListingKeepsNames();
    void fileListingFromDisplay_data();
    void fileListingFromDisplay();
    void fileListingHints();
    void fileListingMountPoints();
    void fileListingReplacedBytes();
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
    o.logToFile = true;
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
    QVERIFY2(job.summary().clean(), qPrintable(log.join('\n'))); // closes the window when finished
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
    QVERIFY(job.summary().clean());

    // A directory without removal: shredded, but "directory stays on disk" is a warning - the window stays open.
    QVERIFY(QDir().mkpath(tmp.filePath(QStringLiteral("dir"))));
    writeFile(tmp.filePath(QStringLiteral("dir/inner")), "x");
    ShredJob dirJob(ShredJob::scan(tmp.filePath(QStringLiteral("dir"))), options);
    QVERIFY(runJob(dirJob));
    QVERIFY(dirJob.summary().ok());
    QCOMPARE(dirJob.summary().warnings, 1);
    QVERIFY(!dirJob.summary().clean());
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
    QList<QPair<qsizetype, qsizetype>> files;
    connect(&job, &ShredJob::filesChanged, this, [&files](qsizetype done, qsizetype total) { files << qMakePair(done, total); });
    QStringList log;
    connect(&job, &ShredJob::log, this, [&log](ShredJob::LogLevel, const QString &t) { log << t; });
    QCOMPARE(job.totalFiles(), 4);
    QVERIFY(runJob(job));
    QVERIFY2(job.summary().ok(), qPrintable(log.join('\n')));
    QCOMPARE(job.summary().filesShredded, 4);
    // Counted over all targets: 0/4 at the start, then once per file.
    const QList<QPair<qsizetype, qsizetype>> expectedFiles{{0, 4}, {1, 4}, {2, 4}, {3, 4}, {4, 4}};
    QCOMPARE(files, expectedFiles);
    QCOMPARE(job.doneFiles(), 4);
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
    QTest::addColumn<int>("iterations");
    QTest::addColumn<bool>("zero");
    QTest::addColumn<bool>("trim");
    QTest::newRow("delete") << "shred-delete" << int(ShredOptions::RemoveMode::Default) << 3 << true << false;
    QTest::newRow("overwrite") << "shred-overwrite" << int(ShredOptions::RemoveMode::None) << 3 << true << false;
    // Flash storage: one pass (more only wear the device), then TRIM.
    QTest::newRow("ssd") << "shred-ssd" << int(ShredOptions::RemoveMode::Default) << 1 << false << true;
}

void TestShredCore::shippedPresets()
{
    QFETCH(QString, preset);
    QFETCH(int, mode);
    QFETCH(int, iterations);
    QFETCH(bool, zero);
    QFETCH(bool, trim);
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
    QCOMPARE(o.iterations, iterations);
    QCOMPARE(o.zero, zero);
    QCOMPARE(o.trimAfter, trim);
    QVERIFY(o.force && o.verbose);
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
    QCOMPARE(presets.size(), 3);
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

void TestShredCore::deviceStack()
{
    // A fake /sys: an SSD with a LUKS volume (discard passed), a VeraCrypt-style volume without discard, a hard disk.
    QTemporaryDir sys;
    QVERIFY(sys.isValid());
    auto mk = [&](const QString &dir, const QByteArray &rotational, const QByteArray &discard) {
        QVERIFY(QDir().mkpath(sys.filePath(dir + QStringLiteral("/queue"))));
        writeFile(sys.filePath(dir + QStringLiteral("/queue/rotational")), rotational + '\n');
        writeFile(sys.filePath(dir + QStringLiteral("/queue/discard_max_bytes")), discard + '\n');
    };
    auto dm = [&](const QString &dir, const QByteArray &name, const QByteArray &uuid, const QString &slave) {
        QVERIFY(QDir().mkpath(sys.filePath(dir + QStringLiteral("/dm"))));
        QVERIFY(QDir().mkpath(sys.filePath(dir + QStringLiteral("/slaves"))));
        writeFile(sys.filePath(dir + QStringLiteral("/dm/name")), name + '\n');
        writeFile(sys.filePath(dir + QStringLiteral("/dm/uuid")), uuid + '\n');
        QVERIFY(QFile::link(sys.filePath(slave), sys.filePath(dir + QStringLiteral("/slaves/") + QFileInfo(slave).fileName())));
    };
    mk(QStringLiteral("sda"), "0", "2147450880");
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("sda/sda1")))); // a partition: the disk's queue counts
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("sda/sda2"))));
    mk(QStringLiteral("sdc"), "1", "0");
    QVERIFY(QDir().mkpath(sys.filePath(QStringLiteral("sdc/sdc1"))));
    mk(QStringLiteral("dm-0"), "0", "2147450880");
    dm(QStringLiteral("dm-0"), "luks-test", "CRYPT-LUKS2-0123456789abcdef-luks-test", QStringLiteral("sda/sda1"));
    mk(QStringLiteral("dm-1"), "0", "0");
    dm(QStringLiteral("dm-1"), "veracrypt3", "", QStringLiteral("sda/sda2"));
    mk(QStringLiteral("dm-2"), "1", "0");
    dm(QStringLiteral("dm-2"), "vg-data", "LVM-abc", QStringLiteral("sdc/sdc1"));

    StorageCheck::DeviceStack stack = StorageCheck::inspectStack(sys.filePath(QStringLiteral("sda/sda1")));
    QVERIFY(stack.flash && !stack.encrypted && stack.discard);
    stack = StorageCheck::inspectStack(sys.filePath(QStringLiteral("dm-0")));
    QVERIFY(stack.flash && stack.encrypted && stack.discard);
    stack = StorageCheck::inspectStack(sys.filePath(QStringLiteral("dm-1"))); // discard not passed through
    QVERIFY(stack.flash && stack.encrypted && !stack.discard);
    stack = StorageCheck::inspectStack(sys.filePath(QStringLiteral("dm-2"))); // LVM on a hard disk
    QVERIFY(!stack.flash && !stack.encrypted && !stack.discard);
    QVERIFY(!StorageCheck::inspectStack(sys.filePath(QStringLiteral("missing"))).flash);

    QVERIFY(StorageCheck::isEncryptingFileSystem(QStringLiteral("ecryptfs")));
    QVERIFY(StorageCheck::isEncryptingFileSystem(QStringLiteral("fuse.gocryptfs")));
    QVERIFY(!StorageCheck::isEncryptingFileSystem(QStringLiteral("ext4")));
}

void TestShredCore::flashWarnings()
{
    StorageCheck::Info info;
    info.fileSystem = QStringLiteral("ext4");
    info.mountPoint = QStringLiteral("/data");
    info.flash = true;
    QString text = StorageCheck::warnings(info).first();
    QVERIFY2(text.contains(QStringLiteral("secure erase")) && text.contains(QStringLiteral("TRIM does not reach")),
             qPrintable(text));
    info.encrypted = true;
    info.discard = true;
    text = StorageCheck::warnings(info).first();
    QVERIFY2(text.contains(QStringLiteral("only with the key")) && text.contains(QStringLiteral("TRIM after removing")),
             qPrintable(text));
    // A device selected directly: nothing about TRIM of a file system.
    info.mountPoint.clear();
    QVERIFY(!StorageCheck::warnings(info).first().contains(QStringLiteral("TRIM")));
}

namespace {

ShredJob::Plan planOn(const QString &target, bool flash, bool discard, const QString &mountPoint)
{
    ShredJob::Plan plan = ShredJob::scan(target);
    plan.storage.flash = flash;
    plan.storage.discard = discard;
    plan.storage.mountPoint = mountPoint;
    return plan;
}

} // namespace

void TestShredCore::trimPlans()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    for (const char *name : {"a", "b", "c", "d"})
        writeFile(tmp.filePath(QString::fromLatin1(name)), "x");
    QList<ShredJob::Plan> plans{planOn(tmp.filePath("a"), true, true, "/data"),
                                planOn(tmp.filePath("b"), true, true, "/data"),  // the same file system once
                                planOn(tmp.filePath("c"), true, false, "/crypt"), // discard not passed
                                planOn(tmp.filePath("d"), false, true, "/hdd")};  // not flash: nothing to trim
    plans << planOn(tmp.filePath("missing"), true, true, "/gone");                // an error: skipped
    QStringList without;
    QCOMPARE(ShredJob::trimMountPoints(plans, &without), QStringList{"/data"});
    QCOMPARE(without, QStringList{"/crypt"});

    ShredOptions options;
    options.fstrimProgram = QStringLiteral("/sbin/fstrim");
    options.elevateProgram.clear();
    QStringList command = ShredJob::trimCommand(options, {"/data", "/my disk"});
    QCOMPARE(command.mid(0, 2), (QStringList{"/bin/sh", "-c"}));
    QVERIFY(command.at(2).contains(QStringLiteral("/sbin/fstrim -v -- \"$m\"")));
    QCOMPARE(command.mid(3), (QStringList{"sh", "/data", "/my disk"}));
    if (::geteuid() != 0) {
        options.elevateProgram = QStringLiteral("pkexec");
        command = ShredJob::trimCommand(options, {"/data"});
        QCOMPARE(command.first(), QStringLiteral("pkexec"));
        QCOMPARE(command.at(1), QStringLiteral("/bin/sh"));
    }
}

void TestShredCore::trimAfterShredding()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // A fake fstrim: records its arguments; fails for a mount point named "bad".
    const QString record = tmp.filePath(QStringLiteral("trimmed"));
    const QString fake = tmp.filePath(QStringLiteral("fstrim"));
    writeFile(fake, QStringLiteral("#!/bin/sh\necho \"$*\" >> '%1'\ncase \"$3\" in */bad) exit 32;; esac\n"
                                   "echo \"$3: 1 MiB (1048576 bytes) trimmed\"\n").arg(record).toUtf8());
    QVERIFY(QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    writeFile(tmp.filePath("f1"), "secret");
    writeFile(tmp.filePath("f2"), "secret");

    ShredOptions options;
    options.iterations = 1;
    options.removeMode = ShredOptions::RemoveMode::Unlink;
    options.trimAfter = true;
    options.fstrimProgram = fake;
    options.elevateProgram.clear();
    const QString good = tmp.filePath(QStringLiteral("good"));
    {
        ShredJob job({planOn(tmp.filePath("f1"), true, true, good)}, options);
        QStringList log;
        connect(&job, &ShredJob::log, this, [&log](ShredJob::LogLevel, const QString &t) { log << t; });
        QVERIFY(runJob(job));
        QVERIFY2(job.summary().clean(), qPrintable(log.join('\n')));
        QCOMPARE(job.summary().trimmed, 1);
        QCOMPARE(QString::fromUtf8(readFile(record)).trimmed(), QStringLiteral("-v -- ") + good);
        QVERIFY(log.join('\n').contains(QStringLiteral("TRIM done")));
        QVERIFY(log.join('\n').contains(QStringLiteral("trimmed"))); // fstrim's output is logged
    }
    {
        // fstrim fails: the files are removed (ok), but with a warning (the window stays open).
        ShredJob job({planOn(tmp.filePath("f2"), true, true, tmp.filePath(QStringLiteral("bad")))}, options);
        QVERIFY(runJob(job));
        QVERIFY(job.summary().ok());
        QVERIFY(!job.summary().clean());
        QCOMPARE(job.summary().trimmed, 0);
        QVERIFY(!QFileInfo::exists(tmp.filePath("f2")));
    }
    {
        // Nothing removed (overwrite only): no TRIM.
        writeFile(tmp.filePath("f3"), "secret");
        QFile::remove(record);
        options.removeMode = ShredOptions::RemoveMode::None;
        ShredJob job({planOn(tmp.filePath("f3"), true, true, good)}, options);
        QVERIFY(runJob(job));
        QVERIFY(job.summary().clean());
        QVERIFY(!QFileInfo::exists(record));
    }
}

void TestShredCore::trimOption()
{
    ShredOptions o;
    o.removeMode = ShredOptions::RemoveMode::Default;
    o.iterations = 1;
    o.trimAfter = true;
    QCOMPARE(o.summaryFlags(), QStringLiteral("-n 1 -u + TRIM"));
    QVERIFY(ShredOptions::fromJson(o.toJson()).trimAfter);
    o.removeMode = ShredOptions::RemoveMode::None; // TRIM only after removing
    QCOMPARE(o.summaryFlags(), QStringLiteral("-n 1"));
    QString error;
    ShredOptions::fromJson(QJsonObject{{"trimAfter", "yes"}}, &error);
    QVERIFY(!error.isEmpty());
}

void TestShredCore::lsblkParsing()
{
    // util-linux 2.37+: numbers and booleans as JSON types. A laptop: an NVMe SSD with a mounted root and an open
    // LUKS partition, a SATA SSD with an unused LUKS partition, a USB stick, zram and a loop device (left out).
    const QByteArray json = R"({"blockdevices": [
      {"name":"nvme0n1", "path":"/dev/nvme0n1", "type":"disk", "tran":"nvme", "model":"Fast SSD", "serial":"S123",
       "size":1000204886016, "rota":false, "rm":false, "fstype":null, "mountpoint":null, "disc-max":2199023255040,
       "children": [
         {"name":"nvme0n1p1", "path":"/dev/nvme0n1p1", "type":"part", "tran":"nvme", "model":null, "serial":null,
          "size":536870912, "rota":false, "rm":false, "fstype":"vfat", "mountpoint":"/boot/efi", "disc-max":2199023255040},
         {"name":"nvme0n1p2", "path":"/dev/nvme0n1p2", "type":"part", "tran":"nvme", "model":null, "serial":null,
          "size":999666221056, "rota":false, "rm":false, "fstype":"crypto_LUKS", "mountpoint":null, "disc-max":2199023255040,
          "children": [
            {"name":"luks-root", "path":"/dev/mapper/luks-root", "type":"crypt", "size":999649443840, "rota":false,
             "rm":false, "fstype":"ext4", "mountpoint":"/", "disc-max":2199023255040}
          ]}
       ]},
      {"name":"sda", "path":"/dev/sda", "type":"disk", "tran":"sata", "model":"Some SSD", "serial":"X9",
       "size":500107862016, "rota":false, "rm":false, "fstype":null, "mountpoint":null, "disc-max":0,
       "children": [
         {"name":"sda1", "path":"/dev/sda1", "type":"part", "tran":null, "size":500106813440, "rota":false, "rm":false,
          "fstype":"crypto_LUKS", "mountpoint":null, "disc-max":0}
       ]},
      {"name":"sdb", "path":"/dev/sdb", "type":"disk", "tran":"usb", "model":"Stick", "serial":"U1",
       "size":32010928128, "rota":true, "rm":true, "fstype":"vfat", "mountpoint":null, "disc-max":0},
      {"name":"zram0", "path":"/dev/zram0", "type":"disk", "size":8589934592, "rota":false, "rm":false,
       "mountpoint":"[SWAP]", "disc-max":4096},
      {"name":"loop0", "path":"/dev/loop0", "type":"loop", "size":1048576, "rota":false, "rm":false}
    ]})";
    QString error;
    const QList<DeviceErase::Device> disks = DeviceErase::parseLsblk(json, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(disks.size(), 3);
    const DeviceErase::Device &nvme = disks.at(0);
    QCOMPARE(nvme.path, QStringLiteral("/dev/nvme0n1"));
    QCOMPARE(nvme.size, Q_INT64_C(1000204886016));
    QVERIFY(nvme.isFlash() && nvme.discard);
    QCOMPARE(nvme.children.size(), 2);                 // the partitions; the crypt device is not one
    QVERIFY(nvme.inUse.join('\n').contains(QStringLiteral("/boot/efi")));
    QVERIFY(nvme.inUse.join('\n').contains(QStringLiteral("mounted at /"))); // from the crypt device below
    QVERIFY(nvme.children.at(1).inUse.join('\n').contains(QStringLiteral("open as luks-root")));
    QCOMPARE(nvme.children.at(1).transport, QStringLiteral("nvme"));
    QCOMPARE(nvme.children.at(1).model, QStringLiteral("Fast SSD")); // inherited from the disk

    const DeviceErase::Device &sata = disks.at(1);
    QVERIFY(sata.inUse.isEmpty());
    QCOMPARE(sata.children.first().transport, QStringLiteral("sata"));
    QVERIFY(sata.children.first().inUse.isEmpty());
    QVERIFY(disks.at(2).removable && disks.at(2).isFlash()); // a stick that claims to be rotational

    // Older util-linux: everything as strings.
    const QByteArray old = R"({"blockdevices": [
      {"name":"sdc", "type":"disk", "tran":"sata", "model":"Old", "serial":"O1", "size":"1000", "rota":"1", "rm":"0",
       "fstype":null, "mountpoint":null, "disc-max":"0"}]})";
    const QList<DeviceErase::Device> oldDisks = DeviceErase::parseLsblk(old, &error);
    QCOMPARE(oldDisks.size(), 1);
    QCOMPARE(oldDisks.first().path, QStringLiteral("/dev/sdc"));
    QCOMPARE(oldDisks.first().size, 1000);
    QVERIFY(oldDisks.first().rotational && !oldDisks.first().isFlash());

    QVERIFY(DeviceErase::parseLsblk("not json", &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestShredCore::eraseMethods()
{
    using M = DeviceErase::Method;
    DeviceErase::Device nvme;
    nvme.type = QStringLiteral("disk");
    nvme.transport = QStringLiteral("nvme");
    nvme.discard = true;
    QCOMPARE(DeviceErase::methods(nvme), (QList<M>{M::NvmeSanitizeCrypto, M::NvmeFormatCrypto, M::NvmeSanitizeBlock,
                                                   M::NvmeFormatUserData, M::BlkdiscardSecure, M::Blkdiscard}));
    DeviceErase::Device sata;
    sata.type = QStringLiteral("disk");
    sata.transport = QStringLiteral("sata");
    QCOMPARE(DeviceErase::methods(sata), (QList<M>{M::AtaEnhanced, M::AtaNormal}));
    // Over USB no ATA security commands; a LUKS partition can lose its keys.
    DeviceErase::Device usbPart;
    usbPart.type = QStringLiteral("part");
    usbPart.transport = QStringLiteral("usb");
    usbPart.fileSystem = QStringLiteral("crypto_LUKS");
    QCOMPARE(DeviceErase::methods(usbPart), (QList<M>{M::LuksErase}));
    // A partition of an NVMe drive: the drive's commands work on the whole drive only.
    DeviceErase::Device nvmePart = nvme;
    nvmePart.type = QStringLiteral("part");
    QCOMPARE(DeviceErase::methods(nvmePart), (QList<M>{M::BlkdiscardSecure, M::Blkdiscard}));
    for (M method : {M::NvmeSanitizeCrypto, M::AtaEnhanced, M::LuksErase, M::Blkdiscard}) {
        QVERIFY(!DeviceErase::title(method).isEmpty());
        QVERIFY(!DeviceErase::description(method).isEmpty());
        QVERIFY(!DeviceErase::package(method).isEmpty());
    }
    QCOMPARE(DeviceErase::package(M::NvmeFormatCrypto), QStringLiteral("nvme-cli"));
}

void TestShredCore::eraseScripts()
{
    // The scripts run with fake tools only (they record their calls) and a device path that is not a device:
    // nothing real is ever touched. Each fake tool behaves as the FAKE_* variables say.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString calls = tmp.filePath(QStringLiteral("calls"));
    const auto tool = [&](const QString &name, const QString &body) -> QString {
        const QString path = tmp.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)
            || file.write((QStringLiteral("#!/bin/sh\necho \"%1 $*\" >> '%2'\n").arg(name, calls) + body).toUtf8()) < 0)
            return {};
        file.close();
        return QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner) ? path : QString();
    };
    DeviceErase::Tools tools;
    tools.lsblk = tool(QStringLiteral("lsblk"), QStringLiteral(
        "case \"$*\" in\n"
        "  *SIZE*) echo \"$FAKE_SIZE\";;\n"
        "  *SERIAL*) echo \"  $FAKE_SERIAL \";;\n"
        "  *MOUNTPOINT*) printf \"$FAKE_MOUNTS\";;\n"
        "  *TYPE*) printf \"$FAKE_TYPES\";;\n"
        "esac\n"));
    tools.nvme = tool(QStringLiteral("nvme"), QStringLiteral(
        "case \"$1\" in\n"
        "  id-ctrl) echo \"$FAKE_CAPS\";;\n"
        "  sanitize-log) echo \"$FAKE_SANLOG\";;\n"
        "  version) echo 'nvme version 2.8';;\n"
        "esac\n"));
    tools.hdparm = tool(QStringLiteral("hdparm"), QStringLiteral(
        "case \"$*\" in\n"
        "  -I*) printf \"$FAKE_HDPARM\";;\n"
        "  *--security-erase*) exit ${FAKE_ERASE_RC:-0};;\n"
        "esac\n"));
    tools.cryptsetup = tool(QStringLiteral("cryptsetup"), QString());
    tools.blkdiscard = tool(QStringLiteral("blkdiscard"), QString());

    DeviceErase::Device device;
    device.name = QStringLiteral("fake0");
    device.path = tmp.filePath(QStringLiteral("fake0")); // not a block device
    device.size = 1000;
    device.serial = QStringLiteral("SN1");

    struct Result { int exitCode; QString output; QStringList calls; };
    const auto run = [&](DeviceErase::Method method, const QMap<QString, QString> &env) {
        QFile::remove(calls);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("FAKE_SIZE"), QStringLiteral("1000"));
        environment.insert(QStringLiteral("FAKE_SERIAL"), QStringLiteral("SN1"));
        environment.insert(QStringLiteral("FAKE_MOUNTS"), QStringLiteral("\\n\\n"));
        environment.insert(QStringLiteral("FAKE_TYPES"), QStringLiteral("disk\\npart\\n"));
        for (auto it = env.cbegin(); it != env.cend(); ++it)
            environment.insert(it.key(), it.value());
        const QStringList command = DeviceErase::command(DeviceErase::script(method, device, tools), QString());
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(command.first(), command.mid(1));
        process.waitForFinished(20000);
        return Result{process.exitCode(), QString::fromUtf8(process.readAll()),
                      QString::fromUtf8(readFile(calls)).split('\n', Qt::SkipEmptyParts)};
    };
    const auto called = [](const Result &r, const QString &prefix) {
        return std::any_of(r.calls.cbegin(), r.calls.cend(), [&](const QString &c) { return c.startsWith(prefix); });
    };
    using M = DeviceErase::Method;

    // The safety checks: another size, another serial number, mounted, an open device on it - nothing runs.
    for (const QMap<QString, QString> &env : {QMap<QString, QString>{{"FAKE_SIZE", "2000"}},
                                              QMap<QString, QString>{{"FAKE_SERIAL", "OTHER"}},
                                              QMap<QString, QString>{{"FAKE_MOUNTS", "\\n/media/x\\n"}},
                                              QMap<QString, QString>{{"FAKE_MOUNTS", "[SWAP]\\n"}},
                                              QMap<QString, QString>{{"FAKE_TYPES", "part\\ncrypt\\n"}}}) {
        const Result r = run(M::Blkdiscard, env);
        QCOMPARE(r.exitCode, 3);
        QVERIFY2(r.output.startsWith(QStringLiteral("ERROR: ")), qPrintable(r.output));
        QVERIFY2(!called(r, QStringLiteral("blkdiscard")), qPrintable(r.calls.join('\n')));
    }

    Result r = run(M::Blkdiscard, {});
    QCOMPARE(r.exitCode, 0);
    QVERIFY(r.calls.contains(QStringLiteral("blkdiscard -f -v ") + device.path));
    r = run(M::BlkdiscardSecure, {});
    QVERIFY(r.calls.contains(QStringLiteral("blkdiscard -f --secure -v ") + device.path));

    r = run(M::LuksErase, {});
    QCOMPARE(r.exitCode, 0);
    QVERIFY(r.calls.contains(QStringLiteral("cryptsetup luksErase -q ") + device.path));

    // NVMe sanitize: refused when the drive says it is not supported; otherwise started and followed to the end.
    r = run(M::NvmeSanitizeCrypto, {{"FAKE_CAPS", "  [0:0] : 0\tCrypto Erase Sanitize Operation Not Supported"}});
    QCOMPARE(r.exitCode, 3);
    QVERIFY(!called(r, QStringLiteral("nvme sanitize ")));
    r = run(M::NvmeSanitizeCrypto, {{"FAKE_CAPS", "  [0:0] : 0x1\tCrypto Erase Sanitize Operation Supported"},
                                    {"FAKE_SANLOG", "Sanitize Progress (SPROG) : 65535\n"
                                                    "Most Recent Sanitize Command Completed Successfully"}});
    QCOMPARE(r.exitCode, 0);
    QVERIFY(r.calls.contains(QStringLiteral("nvme sanitize -a 4 ") + device.path));
    QVERIFY(r.output.contains(QStringLiteral("Sanitize completed")));
    r = run(M::NvmeSanitizeBlock, {{"FAKE_SANLOG", "Sanitize Progress (SPROG) : 1\nSanitize Failed"}});
    QCOMPARE(r.exitCode, 3);
    QVERIFY(r.calls.contains(QStringLiteral("nvme sanitize -a 2 ") + device.path));

    r = run(M::NvmeFormatCrypto, {{"FAKE_CAPS", "  [2:2] : 0\tCrypto Erase Not Supported as part of Secure Erase"}});
    QCOMPARE(r.exitCode, 3);
    QVERIFY(!called(r, QStringLiteral("nvme format")));
    r = run(M::NvmeFormatCrypto, {{"FAKE_CAPS", "  [2:2] : 0x1\tCrypto Erase Supported as part of Secure Erase"}});
    QCOMPARE(r.exitCode, 0);
    QVERIFY(r.calls.contains(QStringLiteral("nvme format ") + device.path + QStringLiteral(" --ses=2 --force")));
    r = run(M::NvmeFormatUserData, {});
    QVERIFY(r.calls.contains(QStringLiteral("nvme format ") + device.path + QStringLiteral(" --ses=1 --force")));

    // ATA Secure Erase.
    const QString security = QStringLiteral("Model: X\\nSecurity: \\n\\tMaster password revision code = 65534\\n"
                                            "\\t\\tsupported\\n\\tnot\\tenabled\\n\\tnot\\tlocked\\n%1"
                                            "\\tnot\\texpired: security count\\n%2Logical Unit WWN Device Identifier\\n");
    const QString ready = security.arg(QStringLiteral("\\tnot\\tfrozen\\n"),
                                       QStringLiteral("\\t\\tsupported: enhanced erase\\n"));
    r = run(M::AtaEnhanced, {{"FAKE_HDPARM", security.arg(QStringLiteral("\\t\\tfrozen\\n"),
                                                          QStringLiteral("\\t\\tsupported: enhanced erase\\n"))}});
    QCOMPARE(r.exitCode, 3);
    QVERIFY2(r.output.contains(QStringLiteral("frozen")), qPrintable(r.output));
    QVERIFY(!called(r, QStringLiteral("hdparm --user-master")));
    r = run(M::AtaEnhanced, {{"FAKE_HDPARM", security.arg(QStringLiteral("\\tnot\\tfrozen\\n"), QString())}});
    QCOMPARE(r.exitCode, 3); // no enhanced erase
    r = run(M::AtaEnhanced, {{"FAKE_HDPARM", ready}});
    QVERIFY2(r.exitCode == 0, qPrintable(r.output));
    QCOMPARE(r.calls.filter(QStringLiteral("--user-master")),
             (QStringList{QStringLiteral("hdparm --user-master u --security-set-pass shred-gui ") + device.path,
                          QStringLiteral("hdparm --user-master u --security-erase-enhanced shred-gui ") + device.path}));
    // A failed erase removes the temporary password again.
    r = run(M::AtaNormal, {{"FAKE_HDPARM", ready}, {"FAKE_ERASE_RC", "5"}});
    QCOMPARE(r.exitCode, 3);
    QVERIFY(r.calls.contains(QStringLiteral("hdparm --user-master u --security-disable shred-gui ") + device.path));

    // Paths with spaces and quotes stay one argument.
    device.path = tmp.filePath(QStringLiteral("it's a disk"));
    r = run(M::Blkdiscard, {});
    QCOMPARE(r.exitCode, 0);
    QVERIFY(r.calls.contains(QStringLiteral("blkdiscard -f -v ") + device.path));
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

void TestShredCore::runLogFiles()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("state/app/logs"));
    const QDateTime start(QDate(2026, 11, 14), QTime(12, 17, 50));

    RunLog first;
    QString error;
    QVERIFY2(first.open(dir, start, &error), qPrintable(error));
    QCOMPARE(first.path(), dir + QStringLiteral("/2026-11-14_12-17-50.log"));
    // A second run started in the same second gets a name of its own.
    RunLog second;
    QVERIFY2(second.open(dir, start, &error), qPrintable(error));
    QCOMPARE(second.path(), dir + QStringLiteral("/2026-11-14_12-17-50_2.log"));
    second.close();

    first.write(start, QStringLiteral("=== Start ===\nshred: 'zażółć': pass 1/4"));
    first.write(start.addSecs(1), QStringLiteral("done"));
    first.close();
    QVERIFY(!first.isOpen());
    first.write(start, QStringLiteral("ignored after close"));

    QFile file(dir + QStringLiteral("/2026-11-14_12-17-50.log"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(file.readAll()),
             QStringLiteral("2026-11-14 12:17:50 === Start ===\n2026-11-14 12:17:50 shred: 'zażółć': pass 1/4\n"
                            "2026-11-14 12:17:51 done\n"));
    // The log names the processed files: readable by the user only.
    QCOMPARE(file.permissions() & ~(QFileDevice::ReadUser | QFileDevice::WriteUser),
             QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QCOMPARE(QFile::permissions(dir) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::ExeOther),
             QFileDevice::Permissions());

    // Off by default, kept by the settings and the JSON like the other options.
    QVERIFY(!ShredOptions().logToFile);
    QCOMPARE(ShredOptions().toJson().value("logToFile").toBool(true), false);
    ShredOptions base;
    base.logToFile = true;
    QVERIFY(ShredOptions::fromJson(QJsonObject{{"iterations", 1}}, &error, base).logToFile);

    QVERIFY(RunLog::defaultDirectory(QStringLiteral("shred-gui")).endsWith(QStringLiteral("/shred-gui/logs")));
    // Shown next to the checkbox with the home directory as "~".
    const QString home = QDir::homePath();
    QCOMPARE(RunLog::displayDirectory(home + QStringLiteral("/.local/state/app/logs")), QStringLiteral("~/.local/state/app/logs"));
    QCOMPARE(RunLog::displayDirectory(home), QStringLiteral("~"));
    QCOMPARE(RunLog::displayDirectory(home + QStringLiteral("x/logs")), home + QStringLiteral("x/logs"));
    QCOMPARE(RunLog::displayDirectory(QStringLiteral("/var/log")), QStringLiteral("/var/log"));
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

void TestShredCore::scanFindsUnwritableDirs()
{
    if (::geteuid() == 0)
        QSKIP("root ignores directory permissions");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.filePath(QStringLiteral("target"));
    const QString locked = root + QStringLiteral("/locked");
    const QString emptyLocked = root + QStringLiteral("/empty-locked");
    QVERIFY(QDir().mkpath(locked));
    QVERIFY(QDir().mkpath(emptyLocked));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/open")));
    writeFile(locked + QStringLiteral("/file"), "1");
    writeFile(root + QStringLiteral("/open/file"), "2");
    const auto setMode = [](const QString &path, mode_t mode) {
        return ::chmod(QFile::encodeName(path).constData(), mode) == 0;
    };
    QVERIFY(setMode(locked, 0555));
    QVERIFY(setMode(emptyLocked, 0555)); // empty - removed through its (writable) parent
    const auto restore = qScopeGuard([&] { setMode(locked, 0755); setMode(emptyLocked, 0755); setMode(root, 0755); });

    ShredJob::Plan plan = ShredJob::scan(root);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.unwritableDirs, QStringList{locked});

    // A file in a locked directory, and a target whose parent is locked.
    QCOMPARE(ShredJob::scan(locked + QStringLiteral("/file")).unwritableDirs, QStringList{locked});
    QVERIFY(ShredJob::scan(root + QStringLiteral("/open/file")).unwritableDirs.isEmpty());
    QVERIFY(setMode(root, 0555));
    QVERIFY(ShredJob::scan(root + QStringLiteral("/open")).unwritableDirs.contains(root));
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

void TestShredCore::combinedProgressWeightsBySize()
{
    CombinedProgress combined;
    QVERIFY(combined.isEmpty());
    QCOMPARE(combined.value(), 0.0);

    // A large job at 2% and a small one at 60%: the bytes written of all bytes to write.
    combined.set(QString(), 0.02, 100'000, 0);
    combined.set(QStringLiteral(":1.42"), 0.60, 1'000, 0);
    QVERIFY(qAbs(combined.value() - (0.02 * 100'000 + 0.60 * 1'000) / 101'000) < 1e-12);

    // Updating an instance replaces its entry; the end of the small job leaves the large one alone.
    combined.set(QStringLiteral(":1.42"), 1.0, 1'000, 10);
    QCOMPARE(combined.size(), 2);
    combined.remove(QStringLiteral(":1.42"));
    QCOMPARE(combined.value(), 0.02);

    // Out-of-range values are clamped; a job without bytes still counts.
    combined.set(QString(), 1.5, 0, 20);
    QCOMPARE(combined.value(), 1.0);
    combined.set(QString(), std::numeric_limits<double>::quiet_NaN(), 0, 20);
    QCOMPARE(combined.value(), 0.0);

    // Byte counts of several terabytes do not overflow.
    CombinedProgress large;
    large.set(QStringLiteral("a"), 0.5, std::numeric_limits<qint64>::max() / 2, 0);
    large.set(QStringLiteral("b"), 0.5, std::numeric_limits<qint64>::max() / 2, 0);
    QVERIFY(qAbs(large.value() - 0.5) < 1e-9);
}

void TestShredCore::combinedProgressExpires()
{
    CombinedProgress combined;
    combined.set(QStringLiteral("crashed"), 0.3, 10, 0);
    combined.set(QStringLiteral("alive"), 0.7, 10, 0);
    QVERIFY(!combined.expire(CombinedProgress::StaleMs));
    combined.set(QStringLiteral("alive"), 0.8, 10, CombinedProgress::StaleMs);
    QVERIFY(combined.expire(CombinedProgress::StaleMs + 1));
    QVERIFY(!combined.contains(QStringLiteral("crashed")));
    QCOMPARE(combined.value(), 0.8);
    QVERIFY(combined.expire(2 * CombinedProgress::StaleMs + 1));
    QVERIFY(combined.isEmpty());
}

void TestShredCore::speedHistoryAverages()
{
    SpeedHistory history(512, 250, 5000);
    qint64 bucketMs = 0;
    QVERIFY(history.averages(1000, &bucketMs).isEmpty());
    QCOMPARE(history.peak(1000), 0.0);

    // Each sample holds until the next one; the last one until "now".
    history.addSample(0, 1000);
    history.addSample(500, 3000);
    QCOMPARE(history.averages(1000, &bucketMs), QList<double>({1000, 1000, 3000, 3000}));
    QCOMPARE(bucketMs, qint64(250));
    QCOMPARE(history.peak(1000), 3000.0);

    // A change in the middle of a bucket: the average weighted by time.
    history.clear();
    history.addSample(0, 1000);
    history.addSample(125, 3000);
    QCOMPARE(history.averages(250), QList<double>({2000}));
    // Negative speeds (unknown) count as 0.
    history.clear();
    history.addSample(0, -1);
    QCOMPARE(history.averages(250), QList<double>({0}));
}

void TestShredCore::speedHistoryGaps()
{
    SpeedHistory history(512, 250, 5000);
    history.addSample(0, 1000);
    history.pause(500);
    history.addSample(1000, 2000);
    QCOMPARE(history.averages(1500), QList<double>({1000, 1000, -1, -1, 2000, 2000}));
    history.pause(1500);
    QCOMPARE(history.averages(3000), QList<double>({1000, 1000, -1, -1, 2000, 2000}));

    // A sample does not hold for longer than 5 s.
    history.clear();
    history.addSample(0, 1000);
    QCOMPARE(history.averages(10000), QList<double>(20, 1000));
    history.addSample(10000, 500);
    const QList<double> averages = history.averages(10250);
    QCOMPARE(averages.size(), 41);
    QCOMPARE(averages.mid(0, 20), QList<double>(20, 1000));
    QCOMPARE(averages.mid(20, 20), QList<double>(20, -1));
    QCOMPARE(averages.last(), 500.0);
}

void TestShredCore::speedHistoryMergesBuckets()
{
    SpeedHistory history(4, 100, 100000);
    history.addSample(0, 1000);
    history.addSample(400, 3000);
    qint64 bucketMs = 0;
    QCOMPARE(history.averages(400, &bucketMs), QList<double>(4, 1000));
    QCOMPARE(bucketMs, qint64(100));
    // "Now" beyond 4 buckets: the result has longer buckets, the history itself does not change.
    QCOMPARE(history.averages(800, &bucketMs), QList<double>({1000, 1000, 3000, 3000}));
    QCOMPARE(bucketMs, qint64(200));
    QCOMPARE(history.averages(400, &bucketMs), QList<double>(4, 1000));
    QCOMPARE(bucketMs, qint64(100));
    // Merged twice; the last bucket has a known speed for half of it.
    history.addSample(1000, 0);
    QCOMPARE(history.averages(1000, &bucketMs), QList<double>({1000, 3000, 3000}));
    QCOMPARE(bucketMs, qint64(400));
    history.clear();
    history.addSample(0, 1);
    QCOMPARE(history.averages(100, &bucketMs), QList<double>({1}));
    QCOMPARE(bucketMs, qint64(100));
}

void TestShredCore::speedHistoryNiceScale_data()
{
    QTest::addColumn<double>("speed");
    QTest::addColumn<qint64>("scale");
    const qint64 KiB = 1024;
    const qint64 MiB = 1024 * KiB;
    const qint64 GiB = 1024 * MiB;
    QTest::newRow("0") << 0.0 << qint64(1);
    QTest::newRow("1.2 B") << 1.2 << qint64(2);
    QTest::newRow("3 B") << 3.0 << qint64(3);
    QTest::newRow("14 B") << 14.0 << qint64(15);
    QTest::newRow("999 B") << 999.0 << KiB;
    QTest::newRow("1 KiB") << double(KiB) << KiB;
    QTest::newRow("1.2 KiB") << 1.2 * double(KiB) << KiB * 3 / 2;
    QTest::newRow("700 KiB") << double(700 * KiB) << 800 * KiB;
    QTest::newRow("900 KiB") << double(900 * KiB) << MiB;
    QTest::newRow("1 MiB") << double(MiB) << MiB;
    QTest::newRow("37 MiB") << double(37 * MiB) << 40 * MiB;
    QTest::newRow("101 MiB") << double(101 * MiB) << 150 * MiB;
    QTest::newRow("600 MiB") << double(600 * MiB) << 600 * MiB;
    QTest::newRow("1.5 GiB") << 1.5 * double(GiB) << GiB * 3 / 2;
    QTest::newRow("1.6 GiB") << 1.6 * double(GiB) << 2 * GiB;
}

void TestShredCore::speedHistoryNiceScale()
{
    QFETCH(double, speed);
    QFETCH(qint64, scale);
    QCOMPARE(SpeedHistory::niceScale(speed), scale);
}

void TestShredCore::fileListingKeepsNames()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QByteArray dir = QFile::encodeName(tmp.path());
    auto cleanup = qScopeGuard([&] {
        for (const char *name : {"caf\xe9.txt", "caf\xe8.txt", "dir\xff", "link\xfe"})
            removeTree(dir + '/' + name);
    });
    writeRaw(dir + "/caf\xe9.txt", "12345");
    writeRaw(dir + "/caf\xe8.txt", "1");
    writeRaw(dir + "/r\xc3\xa9sum\xc3\xa9.txt", "");
    QCOMPARE(::mkdir((dir + "/dir\xff").constData(), 0755), 0);
    QCOMPARE(::symlink("dir\xff", (dir + "/link\xfe").constData()), 0);

    int error = -1;
    QList<FileListing::Entry> entries = FileListing::list(LocalPath::decode(dir), &error);
    QCOMPARE(error, 0);
    std::sort(entries.begin(), entries.end(),
              [](const FileListing::Entry &a, const FileListing::Entry &b) { return LocalPath::encode(a.name) < LocalPath::encode(b.name); });
    QStringList names;
    for (const FileListing::Entry &entry : std::as_const(entries))
        names << LocalPath::display(entry.name);
    QCOMPARE(names, (QStringList{QStringLiteral("caf\\xe8.txt"), QStringLiteral("caf\\xe9.txt"), QStringLiteral("dir\\xff"), QStringLiteral("link\\xfe"), QStringLiteral("résumé.txt")}));
    // The names give the exact bytes back.
    QCOMPARE(LocalPath::encode(entries.at(1).name), QByteArray("caf\xe9.txt"));
    QCOMPARE(entries.at(1).size, 5);
    QVERIFY(!entries.at(1).isDir);
    QVERIFY(entries.at(2).isDir);
    QVERIFY(!entries.at(2).isSymLink);
    QVERIFY(entries.at(3).isDir); // a symlink to a directory
    QVERIFY(entries.at(3).isSymLink);
    QVERIFY(LocalPath::isPlain(entries.at(4).name));
    QVERIFY(entries.at(1).modified > 0);

    // A directory that is not valid UTF-8 itself, and one that does not exist.
    writeRaw(dir + "/dir\xff/inside\xfd", "x");
    entries = FileListing::list(LocalPath::decode(dir + "/dir\xff/"), &error);
    QCOMPARE(error, 0);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(LocalPath::encode(entries.first().name), QByteArray("inside\xfd"));
    QVERIFY(FileListing::list(LocalPath::decode(dir + "/missing"), &error).isEmpty());
    QCOMPARE(error, ENOENT);
}

void TestShredCore::fileListingFromDisplay_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("plain") << QStringLiteral("/a/b c") << QByteArray("/a/b c");
    QTest::newRow("byte") << QStringLiteral("/a/caf\\xe9.txt") << QByteArray("/a/caf\xe9.txt");
    QTest::newRow("upper case") << QStringLiteral("/a/\\xFF") << QByteArray("/a/\xff");
    QTest::newRow("several") << QStringLiteral("\\xfc\\xfd-\\x80") << QByteArray("\xfc\xfd-\x80");
    QTest::newRow("valid UTF-8 as bytes") << QStringLiteral("r\\xc3\\xa9sum\\xc3\\xa9") << QByteArray("r\xc3\xa9sum\xc3\xa9");
    QTest::newRow("ASCII escape stays text") << QStringLiteral("a\\x41") << QByteArray("a\\x41");
    QTest::newRow("incomplete") << QStringLiteral("a\\xe") << QByteArray("a\\xe");
    QTest::newRow("non-ASCII text") << QStringLiteral("ü\\xe9") << QByteArray("\xc3\xbc\xe9");
}

void TestShredCore::fileListingFromDisplay()
{
    QFETCH(QString, text);
    QFETCH(QByteArray, bytes);
    const QString path = FileListing::fromDisplay(text);
    QCOMPARE(LocalPath::encode(path), bytes);
    // The same string as a directory listing gives for these bytes.
    QCOMPARE(path, LocalPath::decode(bytes));
}

void TestShredCore::fileListingHints()
{
    const QString name = LocalPath::decode("M\xfcnchen \xe9t\xe9 \xc3\xbc.txt");
    QCOMPARE(FileListing::invalidBytes(name), QStringLiteral("FC E9 E9"));
    QCOMPARE(FileListing::latin1Reading(name), QStringLiteral("München été ü.txt"));
    QCOMPARE(FileListing::invalidBytes(QStringLiteral("plain ü")), QString());
    QCOMPARE(FileListing::latin1Reading(QStringLiteral("plain ü")), QStringLiteral("plain ü"));
}

void TestShredCore::fileListingReplacedBytes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QByteArray dir = QFile::encodeName(tmp.path());
    auto cleanup = qScopeGuard([&] { removeTree(dir + "/d"); });
    QCOMPARE(::mkdir((dir + "/d").constData(), 0755), 0);
    const QByteArray d = dir + "/d";
    writeRaw(d + "/caf\xe9.txt", "");
    writeRaw(d + "/caf\xe8.txt", "");
    writeRaw(d + "/caf.txt", "");
    writeRaw(d + "/na\xefve.txt", "");
    writeRaw(d + "/two\xe9\xfc.txt", "");
    writeRaw(d + "/real\xef\xbf\xbd.txt", ""); // U+FFFD in the name itself (valid UTF-8)
    writeRaw(d + "/dot.\xe9", "");
    QCOMPARE(::mkdir((d + "/sub\xfc").constData(), 0755), 0);
    writeRaw(d + "/sub\xfc/x\xff", "");
    writeRaw(d + "/sub\xfd", ""); // a file: not a directory in the middle of the path

    // As Dolphin passes them: U+FFFD (EF BF BD) in place of the bytes.
    const auto passed = [&](const QByteArray &name) { return LocalPath::decode(d + '/' + name); };
    const auto exact = [&](const QList<QByteArray> &names) {
        QStringList paths;
        for (const QByteArray &name : names)
            paths << LocalPath::decode(d + '/' + name);
        paths.sort();
        return paths;
    };
    QCOMPARE(FileListing::replacedBytesMatches(passed("na\xef\xbf\xbdve.txt")), exact({"na\xefve.txt"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("caf\xef\xbf\xbd.txt")), exact({"caf\xe9.txt", "caf\xe8.txt"}));
    // One U+FFFD can stand for several bytes (depending on the decoder), and several for several.
    QCOMPARE(FileListing::replacedBytesMatches(passed("two\xef\xbf\xbd.txt")), exact({"two\xe9\xfc.txt"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("two\xef\xbf\xbd\xef\xbf\xbd.txt")), exact({"two\xe9\xfc.txt"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("real\xef\xbf\xbd.txt")), exact({"real\xef\xbf\xbd.txt"}));
    // The rest of the name is matched literally ("." is not a pattern).
    QCOMPARE(FileListing::replacedBytesMatches(passed("dot\xef\xbf\xbd\xef\xbf\xbd")), QStringList());
    QCOMPARE(FileListing::replacedBytesMatches(passed("dot.\xef\xbf\xbd")), exact({"dot.\xe9"}));
    // In a directory of the path too.
    QCOMPARE(FileListing::replacedBytesMatches(passed("sub\xef\xbf\xbd/x\xef\xbf\xbd")), exact({"sub\xfc/x\xff"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("sub\xef\xbf\xbd")), exact({"sub\xfc", "sub\xfd"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("nothing\xef\xbf\xbd")), QStringList());
    QCOMPARE(FileListing::replacedBytesMatches(passed("caf.txt")), QStringList()); // no U+FFFD
    QCOMPARE(FileListing::replacedBytesMatches(QStringLiteral("relative/caf�.txt")), QStringList());
    QCOMPARE(FileListing::replacedBytesMatches(passed("caf\xef\xbf\xbd.txt"), 1).size(), 1);
}

void TestShredCore::fileListingMountPoints()
{
    const QByteArray mounts = "/dev/sda1 / ext4 rw,relatime 0 0\n"
                              "proc /proc proc rw 0 0\n"
                              "/dev/sdb1 /media/user/OLD\\040DISK vfat rw 0 0\n"
                              "/dev/sdc1 /media/user/caf\\351 vfat rw 0 0\n"
                              "/dev/sdd1 /mnt/back\\134slash ext4 rw 0 0\n"
                              "\n";
    const QStringList points = FileListing::mountPoints(mounts);
    QCOMPARE(points.size(), 5);
    QCOMPARE(points.at(0), QStringLiteral("/"));
    QCOMPARE(points.at(2), QStringLiteral("/media/user/OLD DISK"));
    QCOMPARE(LocalPath::encode(points.at(3)), QByteArray("/media/user/caf\xe9"));
    QCOMPARE(points.at(4), QStringLiteral("/mnt/back\\slash"));
}

QTEST_GUILESS_MAIN(TestShredCore)
#include "tst_shredcore.moc"
