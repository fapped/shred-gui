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
    parser.addPositionalArgument(QStringLiteral("paths"), QStringLiteral("Files or directories to add to the list."),
                                 QStringLiteral("[paths...]"));
    parser.process(app);

    if (parser.isSet(listPresetsOption)) {
        const QMap<QString, QString> presets = ShredOptions::availablePresets();
        for (auto it = presets.cbegin(); it != presets.cend(); ++it)
            std::printf("%s\t%s\n", qPrintable(it.key()), qPrintable(it.value()));
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
