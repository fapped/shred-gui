#include "localpath.h"
#include "mainwindow.h"
#include "shredoptions.h"

#include <QApplication>
#include <QCommandLineParser>

#include <cstdio>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("fapped"));
    QApplication::setApplicationName(QStringLiteral("shred-gui"));
    QApplication::setApplicationDisplayName(QStringLiteral("Shred GUI"));
    QApplication::setApplicationVersion(QStringLiteral(SHRED_GUI_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("shred-gui"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Graphical front-end for shred.\n\n"
        "Options loaded with --preset or --config apply to this session only "
        "and do not replace the remembered options."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption presetOption({QStringLiteral("p"), QStringLiteral("preset")},
                                          QStringLiteral("Load the preset <name> (see --list-presets)."),
                                          QStringLiteral("name"));
    const QCommandLineOption configOption({QStringLiteral("c"), QStringLiteral("config")},
                                          QStringLiteral("Load options from the JSON settings <file>."),
                                          QStringLiteral("file"));
    const QCommandLineOption listPresetsOption(QStringLiteral("list-presets"),
                                               QStringLiteral("List available presets and exit."));
    parser.addOption(presetOption);
    parser.addOption(configOption);
    parser.addOption(listPresetsOption);
    parser.addPositionalArgument(QStringLiteral("paths"), QStringLiteral("Files or directories to add to the list (paths or file:// URLs)."),
                                 QStringLiteral("[paths...]"));
    // QCoreApplication::arguments() would replace bytes that are not valid UTF-8 in file names;
    // the raw argv (Qt's own options already removed by QApplication) is decoded losslessly.
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << LocalPath::decode(QByteArray(argv[i]));
    parser.process(arguments);

    if (parser.isSet(listPresetsOption)) {
        const QList<ShredPreset> presets = ShredOptions::presets();
        for (const ShredPreset &preset : presets)
            std::printf("%s\t%s\t%s\n", qPrintable(preset.id), qPrintable(preset.label), qPrintable(preset.file));
        if (presets.isEmpty()) {
            std::fprintf(stderr, "No presets found. Searched in:\n%s\n",
                         qPrintable(ShredOptions::presetSearchPaths().join(QLatin1Char('\n'))));
        }
        return 0;
    }

    MainWindow window;
    window.show();
    if (parser.isSet(presetOption))
        window.loadPreset(parser.value(presetOption));
    if (parser.isSet(configOption))
        window.importSettingsFile(parser.value(configOption), MainWindow::Persistence::SessionOnly);
    window.addTargets(parser.positionalArguments());
    return app.exec();
}
