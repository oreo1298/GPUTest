#include "gt/app_info.h"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromUtf8(gt::app::kId));
    QApplication::setApplicationDisplayName(QString::fromUtf8(gt::app::kDisplayName));
    QApplication::setApplicationVersion(QString::fromUtf8(gt::app::kVersion));
    QMainWindow window;
    window.setWindowTitle(QString::fromUtf8(gt::app::kDisplayName));
    window.setCentralWidget(new QLabel(QStringLiteral("%1 %2").arg(QString::fromUtf8(gt::app::kDisplayName),
                                                                     QString::fromUtf8(gt::app::kVersion))));
    window.resize(960, 640);
    window.show();
    return QApplication::exec();
}
