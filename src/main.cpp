#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QMessageBox>

#include <cstdio>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ananicy-gui"));
    QApplication::setOrganizationName(QStringLiteral("ananicy-gui"));
    QApplication::setApplicationDisplayName(QStringLiteral("Ananicy Configurator"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));
    QApplication::setDesktopFileName(QStringLiteral("ananicy-gui")); // lets Wayland compositors find the icon

    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("ananicy-configurator"), icon));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("GUI editor for ananicy / ananicy-cpp configuration"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption rootOpt({QStringLiteral("r"), QStringLiteral("root")},
                               QStringLiteral("Configuration directory (default: /etc/ananicy.d)"),
                               QStringLiteral("dir"), QStringLiteral("/etc/ananicy.d"));
    parser.addOption(rootOpt);
    parser.process(app);

    // Refuse "/" and top-level folders such as /etc, /usr or /home as config directory, so
    // the program never even reads (let alone changes) files all over the system.
    const QString root = QDir::cleanPath(QDir(parser.value(rootOpt)).absolutePath());
    if (root.split(QLatin1Char('/'), Qt::SkipEmptyParts).size() < 2) {
        const QString msg = QStringLiteral("Refusing to use \"%1\" as configuration directory.\n"
                                           "Use the ananicy directory itself, e.g. /etc/ananicy.d.").arg(root);
        fprintf(stderr, "%s\n", qPrintable(msg));
        QMessageBox::critical(nullptr, QStringLiteral("Ananicy Configurator"), msg);
        return 2;
    }

    MainWindow w(root);
    w.show();
    return app.exec();
}
