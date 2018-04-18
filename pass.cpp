/******************************************************************************
 *  Copyright (C) 2017 by Lukas Fürmetz <fuermetz@mailbox.org>                *
 *                                                                            *
 *  This library is free software; you can redistribute it and/or modify      *
 *  it under the terms of the GNU General Public License as published         *
 *  by the Free Software Foundation; either version 3 of the License or (at   *
 *  your option) any later version.                                           *
 *                                                                            *
 *  This library is distributed in the hope that it will be useful,           *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU         *
 *  Library General Public License for more details.                          *
 *                                                                            *
 *  You should have received a copy of the GNU General Public License         *
 *  along with this library; see the file LICENSE.                            *
 *  If not, see <http://www.gnu.org/licenses/>.                               *
 *****************************************************************************/
#include <KSharedConfig>
#include <KLocalizedString>
#include <knotification.h>

#include <QAction>
#include <QDirIterator>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QMessageBox>
#include <QClipboard>
#include <QDebug>


// extern "C" {
// #include <xdo.h>
// }
// #include <X11/Xlib.h>
// #include <X11/keysym.h>
// #include <X11/extensions/XTest.h>

#include <stdlib.h>

#include "pass.h"
#include "config.h"

using namespace std;


Pass::Pass(QObject *parent, const QVariantList &args)
    : Plasma::AbstractRunner(parent, args)
{
    Q_UNUSED(args);

    // General runner configuration
    setObjectName(QString("Pass"));
    setSpeed(AbstractRunner::NormalSpeed);
    setPriority(HighestPriority);
    auto comment = i18n("Looks for a password matching :q:. Pressing ENTER copies the password to the clipboard.");
    setDefaultSyntax(Plasma::RunnerSyntax(QString(":q:"), comment));
}

Pass::~Pass() {}

void Pass::reloadConfiguration()
{
    actions().clear();
    orderedActions.clear();

    KConfigGroup cfg = config();
    this->showActions = cfg.readEntry(Config::showActions, false);

    if (showActions) {
        auto configActions = cfg.group(Config::Group::Actions);

        // Create actions for every additional field
        auto groups = configActions.groupList();
        Q_FOREACH (auto name, groups) {
            auto group = configActions.group(name);
            auto passAction = PassAction::fromConfig(group);

            auto icon = QIcon::fromTheme(passAction.icon, QIcon::fromTheme("object-unlocked"));
            QAction *act = addAction(passAction.name, icon , passAction.name);
            act->setData(passAction.regex);
            this->orderedActions << act;
        }

    }

    if (cfg.readEntry(Config::showFileContentAction, false)) {
        QAction *act = addAction(Config::showFileContentAction, QIcon::fromTheme("document-new"),
                                 i18n("Show password file contents"));
        act->setData(Config::showFileContentAction);
        this->orderedActions << act;
    }
}

void Pass::init()
{
    reloadConfiguration();

    this->baseDir = QDir(QDir::homePath() + "/.password-store");
    auto baseDir = getenv("PASSWORD_STORE_DIR");
    if (baseDir != nullptr) {
        this->baseDir = QDir(baseDir);
    }

    this->timeout = 45;
    auto timeout = getenv("PASSWORD_STORE_CLIP_TIME");
    if (timeout != nullptr) {
        QString str(timeout);
        bool ok;
        auto timeout = str.toInt(&ok);
        if (ok) {
            this->timeout = timeout;
        }
    }

    this->passOtpIdentifier = "totp::";
    auto passOtpIdentifier = getenv("PASSWORD_STORE_OTP_IDENTIFIER");
    if (passOtpIdentifier != nullptr) {
        this->passOtpIdentifier = passOtpIdentifier;
    }

    initPasswords();

    connect(&watcher, SIGNAL(directoryChanged(QString)), this, SLOT(reinitPasswords(QString)));
}

void Pass::initPasswords() {
    passwords.clear();

    watcher.addPath(this->baseDir.absolutePath());
    QDirIterator it(this->baseDir, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        auto fileInfo = it.fileInfo();
        if (fileInfo.isFile() && fileInfo.suffix() == "gpg") {
            QString password = this->baseDir.relativeFilePath(fileInfo.absoluteFilePath());
            // Remove suffix ".gpg"
            password.chop(4);
            passwords.append(password);
        } else if (fileInfo.isDir() && it.fileName() != "." && it.fileName() != "..") {
            watcher.addPath(it.filePath());
        }
    }
}

void Pass::reinitPasswords(const QString &path) {
    Q_UNUSED(path);

    lock.lockForWrite();
    initPasswords();
    lock.unlock();
}

void Pass::match(Plasma::RunnerContext &context)
{
    if (!context.isValid()) return;

    auto input = context.query();

    QList<Plasma::QueryMatch> matches;

    lock.lockForRead();
    Q_FOREACH (auto password, passwords) {
        QRegularExpression re(".*" + input + ".*", QRegularExpression::CaseInsensitiveOption);
        if (re.match(password).hasMatch()) {
            Plasma::QueryMatch match(this);
            if (input.length() == password.length()) {
                match.setType(Plasma::QueryMatch::ExactMatch);
            } else {
                match.setType(Plasma::QueryMatch::CompletionMatch);
            }
            match.setIcon(QIcon::fromTheme("object-locked"));
            match.setText(password);
            matches.append(match);
        }
    }
    lock.unlock();

    context.addMatches(matches);
}

void Pass::clip(const QString &msg)
{
    QClipboard *cb = QApplication::clipboard();
    cb->setText(msg);
    QTimer::singleShot(timeout * 1000, cb, [cb]() {
            cb->clear();
        });
}

void Pass::run(const Plasma::RunnerContext &context, const Plasma::QueryMatch &match)
{
    Q_UNUSED(context);
    auto regexp = QRegularExpression("^" + QRegularExpression::escape(this->passOtpIdentifier) + ".*");
    auto isOtp = match.text().split('/').filter(regexp).size() > 0;

    QProcess *pass = new QProcess();
    QStringList args;
    if (isOtp) {
        args << "otp" << "show" << match.text();
    } else {
        args << "show" << match.text();
    }
    pass->start("pass", args);

    connect(pass, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            [=](int exitCode, QProcess::ExitStatus exitStatus) {
                Q_UNUSED(exitStatus);

                if (exitCode == 0) {

                    const auto output = pass->readAllStandardOutput();

                    if (match.selectedAction() != nullptr) {
                        const auto data = match.selectedAction()->data().toString();

                        if (data == Config::showFileContentAction) {
                            QMessageBox::information(nullptr, match.text(), output);
                        } else {
                            QRegularExpression re(data, QRegularExpression::MultilineOption);
                            auto matchre = re.match(output);

                            if (matchre.hasMatch()) {
                                clip(matchre.captured(1));
                                this->showNotification(match.text(), match.selectedAction()->text());
                            } else {
                                // Show some information to understand what went wrong.
                                qInfo() << "Regexp: " << data;
                                qInfo() << "Is regexp valid? " << re.isValid();
                                qInfo() << "The file: " << match.text();
                                // qInfo() << "Content: " << output;
                            }
                        }
                    } else {
                        auto string = QString::fromUtf8(output.data());
                        auto lines = string.split('\n', QString::SkipEmptyParts);
                        if (lines.count() > 0) {
                            clip(lines[0]);
                            type(lines[0].toStdString().c_str());
                            this->showNotification(match.text());
                        }
                    }
                }

                pass->close();
                pass->deleteLater();
            });
}

QList<QAction *> Pass::actionsForMatch(const Plasma::QueryMatch &match)
{
    Q_UNUSED(match)

        if (showActions)
            return this->orderedActions;

    return QList<QAction *>();
}

void Pass::showNotification(const QString &text, const QString &actionName /* = "" */)
{
    QString msgPrefix = actionName.isEmpty() ? "":actionName + i18n(" of ");
    QString msg = i18n("Password %1 copied to clipboard for %2 seconds", text, timeout);
    auto notification = KNotification::event("password-unlocked", "Pass", msgPrefix + msg,
                                             "object-unlocked", nullptr, KNotification::CloseOnTimeout,
                                             "krunner_pass");
    QTimer::singleShot(timeout * 1000, notification, SLOT(quit));
}

K_EXPORT_PLASMA_RUNNER(pass, Pass)

#include "pass.moc"


// Down here because Qt and Xlib DEFINES clash
extern "C" {
#include <xdo.h>
}

void type(const char* string) {
    xdo_t* xdo = xdo_new(NULL);
    xdo_enter_text_window(xdo, CURRENTWINDOW, string, 12000);
    xdo_free(xdo);
}
