/*
 * Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QThread>
#include <QStandardPaths>
#include <QDebug>
#include <KLocalizedString>
#include "afploginwidget.h"

static QString defaultMountRoot() {
    return QDir::homePath() + QStringLiteral("/afp_mounts");
}

// Headless mount using afpfs-ng tools
static int mountAFP(const QString &server, const QString &share, const QString &user, 
                     const QString &pass, const QString &mountRoot, QString &mountPath) {
    // Ensure mount root exists
    QDir root(mountRoot);
    if (!root.exists()) {
        if (!root.mkpath(QStringLiteral("."))) {
            qWarning() << "Failed to create mount root:" << mountRoot;
            return 1;
        }
    }

    // Construct mount path: ~/afp_mounts/server[/share]
    mountPath = mountRoot + QStringLiteral("/") + server;
    if (!share.isEmpty()) {
        mountPath += QStringLiteral("/") + share;
    }

    // Create mount path directory
    QDir dir(mountPath);
    if (!dir.exists()) {
        if (!root.mkpath(dir.path())) {
            qWarning() << "Failed to create mount point:" << mountPath;
            return 2;
        }
    }

    // If already mounted (check /proc/mounts), return immediately
    QFile mounts(QStringLiteral("/proc/mounts"));
    if (mounts.open(QIODevice::ReadOnly)) {
        const QByteArray mountsData = mounts.readAll();
        if (mountsData.contains(mountPath.toUtf8())) {
            qDebug() << "Mount point already mounted:" << mountPath;
            return 0;
        }
    }

    // Build afp:// URL for mount_afpfs
    QString afpUrl = QStringLiteral("afp://");
    if (!user.isEmpty()) {
        afpUrl += user;
        if (!pass.isEmpty()) {
            afpUrl += QStringLiteral(":") + pass;
        }
        afpUrl += QStringLiteral("@");
    }
    afpUrl += server;
    if (!share.isEmpty()) {
        afpUrl += QStringLiteral("/") + share;
    }

    // Attempt to mount with retries; after mount, verify the mount is usable
    const int maxAttempts = 3;
    const int perAttemptTimeoutMs = 20000;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        QProcess proc;
        qDebug() << "Attempt" << attempt << "to mount" << afpUrl << "->" << mountPath;
        proc.start(QStringLiteral("mount_afpfs"), QStringList() << afpUrl << mountPath);
        if (!proc.waitForFinished(perAttemptTimeoutMs)) {
            qWarning() << "mount_afpfs timeout (attempt" << attempt << ")";
            proc.kill();
            if (attempt == maxAttempts) return 3;
            QThread::sleep(1);
            continue;
        }

        if (proc.exitCode() != 0) {
            QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
            qWarning() << "mount_afpfs failed (attempt" << attempt << "):" << err;
            if (attempt == maxAttempts) return proc.exitCode();
            QThread::sleep(1);
            continue;
        }

        // Verify the mount appears in /proc/mounts and is readable
        QFile mounts(QStringLiteral("/proc/mounts"));
        bool verified = false;
        if (mounts.open(QIODevice::ReadOnly)) {
            const QByteArray mountsData = mounts.readAll();
            if (mountsData.contains(mountPath.toUtf8())) {
                // probe readability
                QDir probe(mountPath);
                const QStringList entries = probe.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
                if (!entries.isEmpty() || probe.exists()) {
                    verified = true;
                }
            }
        }

        if (verified) {
            qDebug() << "Successfully mounted" << afpUrl << "to" << mountPath;
            return 0;
        }

        qWarning() << "Mount verification failed (attempt" << attempt << ") for" << mountPath;
        // Try unmount to clean up before retrying
        QProcess um;
        um.start(QStringLiteral("fusermount"), QStringList() << QStringLiteral("-u") << mountPath);
        um.waitForFinished(2000);
        if (attempt == maxAttempts) {
            return 4; // verification failure
        }
        QThread::sleep(1);
    }

    return 4;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("afp_connect"));
    KLocalizedString::setApplicationDomain("kio_afp");

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("AFP Connect helper using afpfs-ng"));
    parser.addHelpOption();
    QCommandLineOption serverOpt({QStringLiteral("s"),QStringLiteral("server")}, i18n("AFP server"), QStringLiteral("server"));
    QCommandLineOption shareOpt({QStringLiteral("r"),QStringLiteral("share")}, i18n("AFP share"), QStringLiteral("share"));
    QCommandLineOption userOpt({QStringLiteral("u"),QStringLiteral("user")}, i18n("Username"), QStringLiteral("user"));
    QCommandLineOption passOpt({QStringLiteral("p"),QStringLiteral("pass")}, i18n("Password"), QStringLiteral("pass"));
    QCommandLineOption mountOpt({QStringLiteral("m"),QStringLiteral("mount")}, i18n("Mountpoint root"), QStringLiteral("mountroot"), defaultMountRoot());
    parser.addOptions({serverOpt, shareOpt, userOpt, passOpt, mountOpt});
    parser.process(app);

    const QString server = parser.value(serverOpt);
    const QString share = parser.value(shareOpt);
    const QString user = parser.value(userOpt);
    const QString pass = parser.value(passOpt);
    const QString mountRoot = parser.value(mountOpt);

    // Headless: if server provided, perform mount and print path
    if (!server.isEmpty()) {
        QString mountPath;
        int ret = mountAFP(server, share, user, pass, mountRoot, mountPath);
        if (ret == 0) {
            QTextStream(stdout) << mountPath << '\n';
            return 0;
        } else {
            QTextStream(stderr) << i18n("Mount failed with code %1", ret) << '\n';
            return ret;
        }
    }

    // GUI: show login widget (placeholder for interactive mode)
    AfpLoginWidget w;
    w.show();
    return app.exec();
}
