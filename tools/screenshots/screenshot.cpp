// Takes the README screenshots from inside the real application (its own main() runs): a startup
// function drives the window with timers and saves QWidget::grab() of single widgets, so nothing
// else on the screen ends up in the images. Started by tools/make-screenshots.sh.

#include "mainwindow.h"

#include <QApplication>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

#include <cstdio>
#include <functional>

namespace {

MainWindow *window()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *main = qobject_cast<MainWindow *>(widget))
            return main;
    }
    return nullptr;
}

QPushButton *button(QWidget *parent, const QString &text)
{
    for (QPushButton *b : parent->findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

void save(QWidget *widget, const QString &name)
{
    const QString file = qEnvironmentVariable("SCREENSHOT_DIR") + QLatin1Char('/') + name;
    const bool ok = widget->grab().save(file);
    std::printf("%s %s\n", ok ? "saved" : "FAILED", qPrintable(file));
}

void after(int ms, std::function<void()> step)
{
    QTimer::singleShot(ms, qApp, std::move(step));
}

// Calls `then` once shredding has finished (the Cancel button is disabled again).
void whenFinished(QPushButton *cancel, std::function<void()> then)
{
    if (!cancel->isEnabled()) {
        after(500, std::move(then));
        return;
    }
    after(200, [cancel, then] { whenFinished(cancel, then); });
}

// Saves the first visible message box, then answers it with the button that has `role`.
void catchMessageBox(const QString &name, QMessageBox::ButtonRole role, std::function<void()> then)
{
    after(300, [name, role, then] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (!box || !box->isVisible())
                continue;
            save(box, name);
            for (QAbstractButton *b : box->buttons()) {
                if (box->buttonRole(b) == role) {
                    b->click();
                    then();
                    return;
                }
            }
            box->reject();
            then();
            return;
        }
        catchMessageBox(name, role, then); // not shown yet
    });
}

void run()
{
    MainWindow *w = window();
    if (!w) {
        std::fprintf(stderr, "no main window\n");
        QApplication::exit(1);
        return;
    }
    // With Plasma's global menu the menu bar is an empty strip in the window - leave it out.
    w->menuBar()->hide();
    // The log starts with "Loaded ... from <path of the preset>" - not needed in the images.
    w->findChild<QPlainTextEdit *>()->clear();
    w->resize(860, 940);

    // 1. The window with the "shred-delete" preset and the targets (from the command line).
    after(800, [w] {
        save(w, QStringLiteral("shred-gui-main.png"));
        // 2. The confirmation, then shredding the demo files, 3. the log afterwards.
        QPushButton *cancel = button(w, QStringLiteral("Cancel"));
        catchMessageBox(QStringLiteral("shred-gui-confirm.png"), QMessageBox::DestructiveRole, [w, cancel] {
            after(300, [w, cancel] {
                whenFinished(cancel, [w] {
                    save(w, QStringLiteral("shred-gui-finished.png"));
                    QApplication::quit();
                });
            });
        });
        button(w, QStringLiteral("Shred"))->click();
    });
}

void start()
{
    after(0, run); // after main() has created and filled the window
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(start)
