#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ananicy-gui"));
    QApplication::setOrganizationName(QStringLiteral("ananicy-gui"));
    QApplication::setApplicationDisplayName(QStringLiteral("Ananicy Configurator"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("GUI editor for ananicy / ananicy-cpp configuration"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption rootOpt({QStringLiteral("r"), QStringLiteral("root")},
                               QStringLiteral("Configuration directory (default: /etc/ananicy.d)"),
                               QStringLiteral("dir"), QStringLiteral("/etc/ananicy.d"));
    parser.addOption(rootOpt);
    parser.process(app);

    MainWindow w(parser.value(rootOpt));
    w.show();
    return app.exec();
}
