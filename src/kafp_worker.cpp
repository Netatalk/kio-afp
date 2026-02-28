/*
 * Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <KIO/WorkerBase>
#include <KIO/AuthInfo>
#include <KLocalizedString>
#include <QUrl>
#include <QDebug>
#include <QLoggingCategory>
#include <QCoreApplication>
#include <QMimeDatabase>
#include <QThread>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <pwd.h>
#include <grp.h>
#include <cerrno>
#include <fcntl.h>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/file.h>
#include <QStandardPaths>

extern "C" {
    #include <afp.h>
    #include <afp_server.h>
    #include <afpsl.h>
}

Q_LOGGING_CATEGORY(logAfp, "kio.afp")

// Read buffer size for get/put operations (64 KiB)
static constexpr unsigned int READ_CHUNK = 64 * 1024;

struct ParsedUrl {
    struct afp_url afpUrl;
    QString server;
    QString volume;
    QString path;       // path within volume (no leading slash)
    bool hasVolume;
    bool hasPath;
};

class AfpWorker : public KIO::WorkerBase {
public:
    AfpWorker(const QByteArray &pool, const QByteArray &app)
        : KIO::WorkerBase("afp", pool, app) {}

    KIO::WorkerResult stat(const QUrl &url) override;
    KIO::WorkerResult listDir(const QUrl &url) override;
    KIO::WorkerResult get(const QUrl &url) override;
    KIO::WorkerResult put(const QUrl &url, int permissions, KIO::JobFlags flags) override;
    KIO::WorkerResult mkdir(const QUrl &url, int permissions) override;
    KIO::WorkerResult del(const QUrl &url, bool isFile) override;
    KIO::WorkerResult rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags) override;
    KIO::WorkerResult chmod(const QUrl &url, int permissions) override;
    KIO::WorkerResult fileSystemFreeSpace(const QUrl &url) override;

private:
    // --- State ---
    bool m_connSetupDone = false;
    QString m_cachedServer;
    serverid_t m_serverId = nullptr;
    QString m_cachedVolume;
    volumeid_t m_volumeId = nullptr;
    QByteArray m_cachedUser;
    QByteArray m_cachedPass;

    // --- URL parsing ---
    ParsedUrl parseAfpUrl(const QUrl &url);

    // --- Connection lifecycle ---
    KIO::WorkerResult ensureConnSetup();
    KIO::WorkerResult ensureConnected(ParsedUrl &pu);
    KIO::WorkerResult ensureAttached(ParsedUrl &pu);
    void invalidateSessionState(const char *reason);
    bool isRecoverableSessionError(int ret) const;

    // --- UDSEntry helpers ---
    KIO::UDSEntry statToUDS(const struct stat &st, const QString &name);
    KIO::UDSEntry serverOrVolumeEntry(const QString &name);
    KIO::UDSEntry volumeSummaryToUDS(const struct afp_volume_summary &vol);

    // --- Error mapping ---
    KIO::WorkerResult mapAfpError(int ret, const QString &path);
    KIO::WorkerResult mapAfpConnectError(int ret, const QString &server);
};

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

ParsedUrl AfpWorker::parseAfpUrl(const QUrl &url)
{
    ParsedUrl pu{};
    afp_default_url(&pu.afpUrl);

    pu.server = url.host();
    QByteArray serverBytes = pu.server.toUtf8();
    std::strncpy(pu.afpUrl.servername, serverBytes.constData(),
                 sizeof(pu.afpUrl.servername) - 1);

    if (url.port() > 0)
        pu.afpUrl.port = url.port();

    // Credentials from URL
    QByteArray user = url.userName().toUtf8();
    QByteArray pass = url.password().toUtf8();
    if (!user.isEmpty())
        std::strncpy(pu.afpUrl.username, user.constData(),
                     sizeof(pu.afpUrl.username) - 1);
    if (!pass.isEmpty())
        std::strncpy(pu.afpUrl.password, pass.constData(),
                     sizeof(pu.afpUrl.password) - 1);

    // Split path: first component is volume, rest is path within volume
    // url.path() is e.g. "/VolumeName/some/dir/file"
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) {
        pu.volume = parts.at(0);
        pu.hasVolume = true;
        QByteArray volBytes = pu.volume.toUtf8();
        std::strncpy(pu.afpUrl.volumename, volBytes.constData(),
                     sizeof(pu.afpUrl.volumename) - 1);

        if (parts.size() > 1) {
            QStringList subParts = parts.mid(1);
            pu.path = subParts.join(QLatin1Char('/'));
            pu.hasPath = true;
        }
    }

    // Set the path field in afp_url (absolute path within volume)
    QByteArray pathBytes = pu.path.toUtf8();
    if (!pathBytes.isEmpty()) {
        QByteArray absPath = QByteArray("/") + pathBytes;
        std::strncpy(pu.afpUrl.path, absPath.constData(),
                     sizeof(pu.afpUrl.path) - 1);
    }

    // Normalize: volume root should always have path "/"
    if (pu.hasVolume && !pu.hasPath) {
        std::strncpy(pu.afpUrl.path, "/", sizeof(pu.afpUrl.path) - 1);
    }

    return pu;
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

KIO::WorkerResult AfpWorker::ensureConnSetup()
{
    if (m_connSetupDone)
        return KIO::WorkerResult::pass();

    afp_sl_conn_setup();
    m_connSetupDone = true;
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::ensureConnected(ParsedUrl &pu)
{
    auto r = ensureConnSetup();
    if (!r.success())
        return r;

    // If we're already connected to a different server, disconnect first
    if (m_serverId && m_cachedServer != pu.server) {
        qCDebug(logAfp) << "kio-afp: disconnecting from" << m_cachedServer;
        afp_sl_disconnect(&m_serverId);
        m_serverId = nullptr;
        m_cachedServer.clear();
        m_cachedUser.clear();
        m_cachedPass.clear();
        m_volumeId = nullptr;
        m_cachedVolume.clear();
    }

    if (m_serverId) {
        qCDebug(logAfp) << "kio-afp: already connected to" << m_cachedServer;
        // Fill cached credentials into pu so subsequent AFP calls have them
        if (!m_cachedUser.isEmpty())
            std::strncpy(pu.afpUrl.username, m_cachedUser.constData(),
                         sizeof(pu.afpUrl.username) - 1);
        if (!m_cachedPass.isEmpty())
            std::strncpy(pu.afpUrl.password, m_cachedPass.constData(),
                         sizeof(pu.afpUrl.password) - 1);
        return KIO::WorkerResult::pass();
    }

    // Set up AuthInfo for credential caching / password dialog
    KIO::AuthInfo info;
    info.url.setScheme(QStringLiteral("afp"));
    info.url.setHost(pu.server);
    if (pu.afpUrl.port > 0 && pu.afpUrl.port != 548)
        info.url.setPort(pu.afpUrl.port);

    info.username = QString::fromUtf8(pu.afpUrl.username);
    info.password = QString::fromUtf8(pu.afpUrl.password);
    if (!info.username.isEmpty())
        info.url.setUserName(info.username);

    info.caption = i18n("AFP Login");
    info.prompt = i18n("Please enter your username and password.");
    info.comment = QStringLiteral("afp://") + pu.server;
    info.commentLabel = i18n("Server:");
    info.keepPassword = true;

    // Gather credentials: URL first, then KWallet / session cache, then prompt
    bool haveCreds = !info.username.isEmpty() && !info.password.isEmpty();
    bool dialogUsed = false;

    if (!haveCreds && checkCachedAuthentication(info)) {
        qCDebug(logAfp) << "kio-afp: using cached credentials for user=" << info.username;
        QByteArray u = info.username.toUtf8();
        QByteArray p = info.password.toUtf8();
        std::strncpy(pu.afpUrl.username, u.constData(),
                     sizeof(pu.afpUrl.username) - 1);
        std::strncpy(pu.afpUrl.password, p.constData(),
                     sizeof(pu.afpUrl.password) - 1);
        haveCreds = !info.username.isEmpty() && !info.password.isEmpty();
    }

    // No credentials from URL or cache — prompt before connecting
    if (!haveCreds) {
        info.setModified(false);
        int errCode = openPasswordDialog(info);
        if (errCode != 0)
            return KIO::WorkerResult::fail(KIO::ERR_USER_CANCELED, pu.server);

        QByteArray u = info.username.toUtf8();
        QByteArray p = info.password.toUtf8();
        std::strncpy(pu.afpUrl.username, u.constData(),
                     sizeof(pu.afpUrl.username) - 1);
        std::strncpy(pu.afpUrl.password, p.constData(),
                     sizeof(pu.afpUrl.password) - 1);
        dialogUsed = true;
    }

    // Paths for cross-process coordination, stored under the user's runtime dir.
    const QString runtimeDir =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QByteArray lockPath =
        (runtimeDir + QStringLiteral("/kio-afp-connect.lock")).toLocal8Bit();
    const QByteArray breakerPath =
        (runtimeDir + QStringLiteral("/kio-afp-connect.breaker")).toLocal8Bit();

    // Circuit breaker: if a recent worker already failed to connect,
    // don't even try — the daemon is likely in a bad state.
    constexpr int BREAKER_COOLDOWN_SECS = 30;
    struct ::stat breakerSt{};
    if (::stat(breakerPath.constData(), &breakerSt) == 0) {
        time_t age = time(nullptr) - breakerSt.st_mtime;
        if (age < BREAKER_COOLDOWN_SECS) {
            qWarning() << "kio-afp: connect circuit breaker active ("
                       << age << "s ago), failing fast";
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                       i18n("AFP daemon not responding (retry in %1 s)",
                            BREAKER_COOLDOWN_SECS - static_cast<int>(age)));
        }
        // Breaker expired — remove it and try normally
        ::unlink(breakerPath.constData());
    }

    // Serialize afp_sl_connect (including retries) across worker processes
    // to avoid overwhelming the afpsld daemon with concurrent connections.
    int lockFd = ::open(lockPath.constData(), O_CREAT | O_RDWR, 0600);
    if (lockFd >= 0)
        flock(lockFd, LOCK_EX);

    // After acquiring the lock, check the breaker again — the worker ahead
    // of us may have tripped it while we were waiting.
    if (::stat(breakerPath.constData(), &breakerSt) == 0) {
        time_t age = time(nullptr) - breakerSt.st_mtime;
        if (age < BREAKER_COOLDOWN_SECS) {
            qWarning() << "kio-afp: connect circuit breaker tripped while waiting";
            if (lockFd >= 0)
                ::close(lockFd);
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                       i18n("AFP daemon not responding (retry in %1 s)",
                            BREAKER_COOLDOWN_SECS - static_cast<int>(age)));
        }
        ::unlink(breakerPath.constData());
    }

    // Connect / retry loop
    constexpr int MAX_CONNECT_RETRIES = 3;
    constexpr int BASE_RETRY_DELAY_MS = 500;
    constexpr unsigned int CONNECT_TIMEOUT_SECS = 15;
    int transientRetries = 0;

    for (;;) {
        serverid_t sid = nullptr;
        char loginmesg[AFP_LOGINMESG_LEN] = {};
        int connectError = 0;
        unsigned int uamMask = default_uams_mask();

        // Hard timeout: if afp_sl_connect busy-loops inside the library,
        // SIGALRM will terminate this worker process so it doesn't spin
        // at 100% CPU forever.  KIO will clean up and show an error.
        std::signal(SIGALRM, SIG_DFL);
        alarm(CONNECT_TIMEOUT_SECS);

        qCDebug(logAfp) << "kio-afp: connect server=" << pu.server
                 << "user=" << pu.afpUrl.username;
        int ret = afp_sl_connect(&pu.afpUrl, uamMask, &sid, loginmesg,
                                 &connectError);

        alarm(0); // cancel timeout — connect returned normally
        qCDebug(logAfp) << "kio-afp: connect returned" << ret
                 << "sid=" << sid << "err=" << connectError;

        // Sanity check: if success reported but no session ID, treat as daemon error
        if ((ret == AFP_SERVER_RESULT_OKAY || ret == AFP_SERVER_RESULT_ALREADY_CONNECTED) && !sid) {
            qWarning() << "kio-afp: connect returned success but sid is null, treating as error";
            ret = AFP_SERVER_RESULT_DAEMON_ERROR;
        }

        if (ret == AFP_SERVER_RESULT_OKAY
            || ret == AFP_SERVER_RESULT_ALREADY_CONNECTED) {
            if (lockFd >= 0)
                ::close(lockFd);

            // Successful connect clears any stale breaker
            ::unlink(breakerPath.constData());

            m_serverId = sid;
            m_cachedServer = pu.server;
            m_cachedUser = QByteArray(pu.afpUrl.username);
            m_cachedPass = QByteArray(pu.afpUrl.password);

            if (std::strlen(loginmesg) > 0)
                qCDebug(logAfp) << "kio-afp: login message:" << loginmesg;

            // Only cache when user went through the password dialog
            if (dialogUsed && info.keepPassword) {
                info.username = QString::fromUtf8(pu.afpUrl.username);
                info.password = QString::fromUtf8(pu.afpUrl.password);
                cacheAuthentication(info);
            }

            return KIO::WorkerResult::pass();
        }

        if (ret == AFP_SERVER_RESULT_NOAUTHENT) {
            // Release lock during password dialog so other workers aren't blocked
            if (lockFd >= 0) {
                ::close(lockFd);
                lockFd = -1;
            }

            // Auth failed — re-prompt with error message
            info.setModified(false);
            int errCode = openPasswordDialog(info,
                i18n("Authentication failed. Please try again."));
            if (errCode != 0)
                return KIO::WorkerResult::fail(KIO::ERR_USER_CANCELED, pu.server);

            QByteArray u = info.username.toUtf8();
            QByteArray p = info.password.toUtf8();
            std::strncpy(pu.afpUrl.username, u.constData(),
                         sizeof(pu.afpUrl.username) - 1);
            std::strncpy(pu.afpUrl.password, p.constData(),
                         sizeof(pu.afpUrl.password) - 1);
            dialogUsed = true;

            // Re-acquire lock before next attempt
            lockFd = ::open(lockPath.constData(), O_CREAT | O_RDWR, 0600);
            if (lockFd >= 0)
                flock(lockFd, LOCK_EX);
            continue;
        }

        // Transient error (e.g. daemon overloaded) — retry with backoff
        // Lock stays held so other workers don't pile on while we wait.
        if (transientRetries < MAX_CONNECT_RETRIES) {
            int delay = BASE_RETRY_DELAY_MS * (1 << transientRetries);
            qWarning() << "kio-afp: connect failed (" << ret
                       << "), retrying in" << delay << "ms (attempt"
                       << (transientRetries + 1) << "of"
                       << MAX_CONNECT_RETRIES << ")";
            QThread::msleep(delay);
            ++transientRetries;
            continue;
        }

        // All retries exhausted — trip the circuit breaker so other
        // workers fail fast instead of also hammering the daemon.
        qWarning() << "kio-afp: tripping connect circuit breaker for"
                   << BREAKER_COOLDOWN_SECS << "s";
        int bfd = ::open(breakerPath.constData(), O_CREAT | O_WRONLY, 0600);
        if (bfd >= 0)
            ::close(bfd);

        if (lockFd >= 0)
            ::close(lockFd);
        return mapAfpConnectError(ret, pu.server);
    }
}

KIO::WorkerResult AfpWorker::ensureAttached(ParsedUrl &pu)
{
    if (!pu.hasVolume)
        return KIO::WorkerResult::fail(KIO::ERR_DOES_NOT_EXIST,
                   i18n("No volume specified in URL"));

    auto r = ensureConnected(pu);
    if (!r.success())
        return r;

    // If switching to a different volume, clear our local cache.
    // Don't call afp_sl_detach — the daemon handles concurrent volume
    // attachments, and detaching with a mismatched URL corrupts state.
    if (m_volumeId && m_cachedVolume != pu.volume) {
        qCDebug(logAfp) << "kio-afp: switching from volume" << m_cachedVolume
                 << "to" << pu.volume;
        m_volumeId = nullptr;
        m_cachedVolume.clear();
    }

    if (m_volumeId)
        return KIO::WorkerResult::pass();

    volumeid_t vid = nullptr;

    qCDebug(logAfp) << "kio-afp: attach volume=" << pu.volume;
    int ret = afp_sl_attach(&pu.afpUrl, 0, &vid);
    qCDebug(logAfp) << "kio-afp: attach returned" << ret << "vid=" << vid;

    if (ret == AFP_SERVER_RESULT_ALREADY_MOUNTED
        || ret == AFP_SERVER_RESULT_ALREADY_ATTACHED) {
        // Volume attached but daemon didn't return a handle.
        // Try to retrieve it, or reset the connection and re-attach.
        qCDebug(logAfp) << "kio-afp: volume already attached, trying getvolid";
        ret = afp_sl_getvolid(&pu.afpUrl, &vid);
        qCDebug(logAfp) << "kio-afp: getvolid returned" << ret << "vid=" << vid;

        if (ret != AFP_SERVER_RESULT_OKAY) {
            // Stale daemon state: volume is ALREADY_MOUNTED but no
            // server connection owns it.  Disconnect to clean up,
            // then reconnect and re-attach.
            qWarning() << "kio-afp: getvolid failed, resetting connection";
            afp_sl_disconnect(&m_serverId);
            m_serverId = nullptr;
            m_cachedServer.clear();

            auto rc = ensureConnected(pu);
            if (!rc.success())
                return rc;

            vid = nullptr;
            ret = afp_sl_attach(&pu.afpUrl, 0, &vid);
            qCDebug(logAfp) << "kio-afp: re-attach after reset returned" << ret
                     << "vid=" << vid;

            // Another worker may have re-attached between our
            // disconnect and re-attach; try getvolid once more.
            if (ret == AFP_SERVER_RESULT_ALREADY_MOUNTED
                || ret == AFP_SERVER_RESULT_ALREADY_ATTACHED) {
                ret = afp_sl_getvolid(&pu.afpUrl, &vid);
                qCDebug(logAfp) << "kio-afp: getvolid retry returned" << ret
                           << "vid=" << vid;
            }

            if (ret != AFP_SERVER_RESULT_OKAY)
                return mapAfpError(ret, pu.volume);
        }
    } else if (ret != AFP_SERVER_RESULT_OKAY) {
        return mapAfpError(ret, pu.volume);
    }

    m_volumeId = vid;
    m_cachedVolume = pu.volume;
    return KIO::WorkerResult::pass();
}

void AfpWorker::invalidateSessionState(const char *reason)
{
    qCDebug(logAfp) << "kio-afp: invalidating cached AFP session state:" << reason;

    if (m_serverId) {
        afp_sl_disconnect(&m_serverId);
    }

    m_serverId = nullptr;
    m_cachedServer.clear();
    m_cachedUser.clear();
    m_cachedPass.clear();
    m_volumeId = nullptr;
    m_cachedVolume.clear();
}

bool AfpWorker::isRecoverableSessionError(int ret) const
{
    switch (ret) {
    case AFP_SERVER_RESULT_NOTCONNECTED:
    case AFP_SERVER_RESULT_NOTATTACHED:
    case AFP_SERVER_RESULT_DAEMON_ERROR:
    case AFP_SERVER_RESULT_NOSERVER:
    case AFP_SERVER_RESULT_TIMEDOUT:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// UDS entry helpers
// ---------------------------------------------------------------------------

KIO::UDSEntry AfpWorker::statToUDS(const struct stat &st, const QString &name)
{
    KIO::UDSEntry entry;
    entry.reserve(8);

    entry.fastInsert(KIO::UDSEntry::UDS_NAME, name);
    entry.fastInsert(KIO::UDSEntry::UDS_SIZE, static_cast<long long>(st.st_size));
    entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, st.st_mode & S_IFMT);
    entry.fastInsert(KIO::UDSEntry::UDS_ACCESS, st.st_mode & 07777);
    entry.fastInsert(KIO::UDSEntry::UDS_MODIFICATION_TIME,
                     static_cast<long long>(st.st_mtime));

    struct passwd *pw = getpwuid(st.st_uid);
    entry.fastInsert(KIO::UDSEntry::UDS_USER,
                     pw ? QString::fromLocal8Bit(pw->pw_name)
                        : QString::number(st.st_uid));
    struct group *gr = getgrgid(st.st_gid);
    entry.fastInsert(KIO::UDSEntry::UDS_GROUP,
                     gr ? QString::fromLocal8Bit(gr->gr_name)
                        : QString::number(st.st_gid));

    return entry;
}

KIO::UDSEntry AfpWorker::serverOrVolumeEntry(const QString &name)
{
    KIO::UDSEntry entry;
    entry.reserve(5);
    entry.fastInsert(KIO::UDSEntry::UDS_NAME, name.isEmpty() ? QStringLiteral(".") : name);
    entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
    entry.fastInsert(KIO::UDSEntry::UDS_ACCESS,
                     S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    struct passwd *pw = getpwuid(geteuid());
    entry.fastInsert(KIO::UDSEntry::UDS_USER,
                     pw ? QString::fromLocal8Bit(pw->pw_name)
                        : QString::number(geteuid()));
    struct group *gr = getgrgid(getegid());
    entry.fastInsert(KIO::UDSEntry::UDS_GROUP,
                     gr ? QString::fromLocal8Bit(gr->gr_name)
                        : QString::number(getegid()));
    return entry;
}

KIO::UDSEntry AfpWorker::volumeSummaryToUDS(const struct afp_volume_summary &vol)
{
    KIO::UDSEntry entry;
    entry.reserve(5);
    entry.fastInsert(KIO::UDSEntry::UDS_NAME,
                     QString::fromUtf8(vol.volume_name_printable));
    entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
    entry.fastInsert(KIO::UDSEntry::UDS_ACCESS,
                     S_IRWXU | S_IRWXG | S_IRWXO);
    struct passwd *pw = getpwuid(geteuid());
    entry.fastInsert(KIO::UDSEntry::UDS_USER,
                     pw ? QString::fromLocal8Bit(pw->pw_name)
                        : QString::number(geteuid()));
    struct group *gr = getgrgid(getegid());
    entry.fastInsert(KIO::UDSEntry::UDS_GROUP,
                     gr ? QString::fromLocal8Bit(gr->gr_name)
                        : QString::number(getegid()));
    return entry;
}

// ---------------------------------------------------------------------------
// Error mapping
// ---------------------------------------------------------------------------

KIO::WorkerResult AfpWorker::mapAfpError(int ret, const QString &path)
{
    const QString separator = QStringLiteral("\n");
    switch (ret) {
    case AFP_SERVER_RESULT_OKAY:
        return KIO::WorkerResult::pass();
    case AFP_SERVER_RESULT_ENOENT:
        return KIO::WorkerResult::fail(KIO::ERR_DOES_NOT_EXIST, path);
    case AFP_SERVER_RESULT_ACCESS:
        return KIO::WorkerResult::fail(KIO::ERR_ACCESS_DENIED, path);
    case AFP_SERVER_RESULT_EXIST:
        return KIO::WorkerResult::fail(KIO::ERR_FILE_ALREADY_EXIST, path);
    case AFP_SERVER_RESULT_NOVOLUME:
        return KIO::WorkerResult::fail(KIO::ERR_DOES_NOT_EXIST,
                   i18n("Volume not found: %1", path));
    case AFP_SERVER_RESULT_NOSERVER:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   path + separator + i18n("AFP server not found"));
    case AFP_SERVER_RESULT_TIMEDOUT:
        return KIO::WorkerResult::fail(KIO::ERR_SERVER_TIMEOUT, path);
    case AFP_SERVER_RESULT_DAEMON_ERROR:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   path + separator + i18n("Cannot communicate with AFP server"));
    case AFP_SERVER_RESULT_NOTSUPPORTED:
        return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, path);
    case AFP_SERVER_RESULT_NOTCONNECTED:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   path + separator + i18n("Not connected to AFP server"));
    case AFP_SERVER_RESULT_NOTATTACHED:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   path + separator + i18n("Not attached to volume"));
    case AFP_SERVER_RESULT_NOAUTHENT:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_AUTHENTICATE,
                   i18n("Authentication with AFP server failed"));
    default:
        return KIO::WorkerResult::fail(KIO::ERR_INTERNAL,
                   i18n("AFP error %1", ret));
    }
}

KIO::WorkerResult AfpWorker::mapAfpConnectError(int ret, const QString &server)
{
    const QString separator = QStringLiteral("\n");
    switch (ret) {
    case AFP_SERVER_RESULT_NOAUTHENT:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_AUTHENTICATE,
                   server + separator + i18n("Authentication with AFP server failed"));
    case AFP_SERVER_RESULT_NOSERVER:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   server + separator + i18n("Could not find AFP server"));
    case AFP_SERVER_RESULT_TIMEDOUT:
        return KIO::WorkerResult::fail(KIO::ERR_SERVER_TIMEOUT,
                   server + separator + i18n("Connection timed out"));
    case AFP_SERVER_RESULT_DAEMON_ERROR:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   server + separator + i18n("Cannot communicate with AFP server"));
    default:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   server + separator + i18n("AFP connection error %1", ret));
    }
}

// ---------------------------------------------------------------------------
// KIO operations
// ---------------------------------------------------------------------------

KIO::WorkerResult AfpWorker::stat(const QUrl &url)
{
    qCDebug(logAfp) << "AfpWorker::stat()" << url;

    ParsedUrl pu = parseAfpUrl(url);

    // Server root: afp://server — return a synthetic directory entry.
    // Skip connecting: listDir() will establish the connection when it
    // actually needs to talk to the daemon, reducing connect call volume.
    if (!pu.hasVolume) {
        statEntry(serverOrVolumeEntry(QString()));
        return KIO::WorkerResult::pass();
    }

    // Volume root: afp://server/volume
    // Attach and do a real stat so Dolphin sees actual permissions
    // (needed for drag-and-drop writability checks on the listing view).
    // Fall back to a synthetic entry if attachment fails.
    if (!pu.hasPath) {
        auto r = ensureAttached(pu);
        if (r.success()) {
            struct stat st{};
            int ret = afp_sl_stat(&m_volumeId, "/", &pu.afpUrl, &st);
            if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
                invalidateSessionState("volume-root stat failed");
                auto rr = ensureAttached(pu);
                if (rr.success())
                    ret = afp_sl_stat(&m_volumeId, "/", &pu.afpUrl, &st);
            }
            if (ret == AFP_SERVER_RESULT_OKAY) {
                statEntry(statToUDS(st, pu.volume));
                return KIO::WorkerResult::pass();
            }
        }
        statEntry(serverOrVolumeEntry(pu.volume));
        return KIO::WorkerResult::pass();
    }

    // File/dir within volume
    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    struct stat st{};
    int ret = afp_sl_stat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &st);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("stat failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_stat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &st);
    }
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    // Determine the file name (last component)
    const QStringList parts = pu.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QString name = parts.isEmpty() ? pu.volume : parts.last();

    KIO::UDSEntry entry = statToUDS(st, name);

    // Add MIME type
    if (S_ISREG(st.st_mode)) {
        QMimeDatabase db;
        entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE,
                         db.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name());
    } else if (S_ISDIR(st.st_mode)) {
        entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, QStringLiteral("inode/directory"));
    }

    statEntry(entry);
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::listDir(const QUrl &url)
{
    qCDebug(logAfp) << "AfpWorker::listDir()" << url;

    ParsedUrl pu = parseAfpUrl(url);

    // Server root — list volumes
    if (!pu.hasVolume) {
        auto r = ensureConnected(pu);
        if (!r.success())
            return r;

        constexpr unsigned int MAX_VOLS = 64;
        struct afp_volume_summary vols[MAX_VOLS];
        unsigned int numVols = 0;

        int ret = afp_sl_getvols(&pu.afpUrl, 0, MAX_VOLS, &numVols, vols);
        qCDebug(logAfp) << "kio-afp: getvols returned" << ret
                 << "numVols=" << numVols;
        if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
            invalidateSessionState("getvols failed");
            auto rr = ensureConnected(pu);
            if (!rr.success())
                return rr;
            numVols = 0;
            ret = afp_sl_getvols(&pu.afpUrl, 0, MAX_VOLS, &numVols, vols);
            qWarning() << "kio-afp: getvols retry after reconnect returned"
                       << ret << "numVols=" << numVols;
        }

        // On a fresh daemon the volume list may not be ready yet.
        // Retry once after a short delay if we got zero volumes.
        if (ret == AFP_SERVER_RESULT_OKAY && numVols == 0) {
            qCDebug(logAfp) << "kio-afp: empty volume list, retrying after delay";
            QThread::msleep(500);
            numVols = 0;
            ret = afp_sl_getvols(&pu.afpUrl, 0, MAX_VOLS, &numVols, vols);
            qCDebug(logAfp) << "kio-afp: getvols retry returned" << ret
                     << "numVols=" << numVols;
        }

        if (ret != AFP_SERVER_RESULT_OKAY)
            return mapAfpError(ret, pu.server);

        KIO::UDSEntryList entries;
        entries.reserve(static_cast<int>(numVols));
        for (unsigned int i = 0; i < numVols; ++i)
            entries << volumeSummaryToUDS(vols[i]);

        listEntries(entries);
        return KIO::WorkerResult::pass();
    }

    // Directory within a volume (or volume root)
    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    // Path for readdir: "/" for volume root, or the absolute subpath
    const char *dirPath = pu.hasPath ? pu.afpUrl.path : "/";

    // Stat the directory itself and emit a "." entry so KDirLister has
    // the root item immediately, even if a separate stat job is still
    // queued behind this listDir in another worker process.  Without
    // this, Dolphin's drag-and-drop writability check on the view
    // background can fail because rootItem() is null.
    {
        struct stat dirSt{};
        int dirRet = afp_sl_stat(&m_volumeId, dirPath, &pu.afpUrl, &dirSt);
        if (dirRet == AFP_SERVER_RESULT_OKAY) {
            KIO::UDSEntry dotEntry = statToUDS(dirSt, QStringLiteral("."));
            if (S_ISDIR(dirSt.st_mode))
                dotEntry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE,
                                    QStringLiteral("inode/directory"));
            listEntry(dotEntry);
        }
    }

    constexpr int BATCH = 64;
    int start = 0;
    bool done = false;

    while (!done) {
        struct afp_file_info_basic *fpb = nullptr;
        unsigned int numFiles = 0;
        int eod = 0;

        qCDebug(logAfp) << "kio-afp: readdir path=" << dirPath
                 << "start=" << start << "vid=" << m_volumeId;
        int ret = afp_sl_readdir(&m_volumeId, dirPath, &pu.afpUrl,
                                 start, BATCH, &numFiles, &fpb, &eod);
        qCDebug(logAfp) << "kio-afp: readdir returned" << ret
                 << "numFiles=" << numFiles << "eod=" << eod;
        if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
            qCDebug(logAfp) << "kio-afp: readdir failed with recoverable error,"
                     << "reconnecting and retrying";
            invalidateSessionState("readdir failed");
            auto rr = ensureAttached(pu);
            if (!rr.success())
                return rr;
            numFiles = 0;
            fpb = nullptr;
            eod = 0;
            ret = afp_sl_readdir(&m_volumeId, dirPath, &pu.afpUrl,
                                 start, BATCH, &numFiles, &fpb, &eod);
            qCDebug(logAfp) << "kio-afp: readdir retry returned" << ret
                     << "numFiles=" << numFiles << "eod=" << eod;
        }
        if (ret != AFP_SERVER_RESULT_OKAY) {
            return mapAfpError(ret, pu.hasPath ? pu.path : pu.volume);
        }

        KIO::UDSEntryList entries;
        entries.reserve(static_cast<int>(numFiles));
        for (unsigned int i = 0; i < numFiles; ++i) {
            const struct afp_file_info_basic &fi = fpb[i];
            KIO::UDSEntry entry;
            entry.reserve(7);

            entry.fastInsert(KIO::UDSEntry::UDS_NAME,
                             QString::fromUtf8(fi.name));
            entry.fastInsert(KIO::UDSEntry::UDS_SIZE,
                             static_cast<long long>(fi.size));
            entry.fastInsert(KIO::UDSEntry::UDS_MODIFICATION_TIME,
                             static_cast<long long>(fi.modification_date));

            if (S_ISDIR(fi.unixprivs.permissions)) {
                entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
                entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE,
                                 QStringLiteral("inode/directory"));
            } else {
                entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
            }

            entry.fastInsert(KIO::UDSEntry::UDS_ACCESS,
                             fi.unixprivs.permissions & 07777);

            struct passwd *pw = getpwuid(fi.unixprivs.uid);
            entry.fastInsert(KIO::UDSEntry::UDS_USER,
                             pw ? QString::fromLocal8Bit(pw->pw_name)
                                : QString::number(fi.unixprivs.uid));
            struct group *gr = getgrgid(fi.unixprivs.gid);
            entry.fastInsert(KIO::UDSEntry::UDS_GROUP,
                             gr ? QString::fromLocal8Bit(gr->gr_name)
                                : QString::number(fi.unixprivs.gid));

            entries << entry;
        }
        listEntries(entries);

        start += static_cast<int>(numFiles);
        if (eod || numFiles == 0)
            done = true;
    }

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::get(const QUrl &url)
{
    qCDebug(logAfp) << "kio-afp: get()" << url;

    ParsedUrl pu = parseAfpUrl(url);
    if (!pu.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY,
                   pu.hasVolume ? pu.volume : pu.server);

    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    // Stat the file to get size
    struct stat st{};
    int ret = afp_sl_stat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &st);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("get stat failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_stat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &st);
    }
    if (ret != AFP_SERVER_RESULT_OKAY) {
        qCDebug(logAfp) << "kio-afp: get stat failed ret=" << ret;
        return mapAfpError(ret, pu.path);
    }

    if (S_ISDIR(st.st_mode))
        return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY, pu.path);

    qCDebug(logAfp) << "kio-afp: get file size=" << st.st_size;
    totalSize(static_cast<KIO::filesize_t>(st.st_size));

    // Set MIME type
    {
        QMimeDatabase db;
        const QStringList parts = pu.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QString name = parts.isEmpty() ? pu.path : parts.last();
        mimeType(db.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name());
    }

    // Open
    unsigned int fileId = 0;
    ret = afp_sl_open(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &fileId, O_RDONLY);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("get open failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_open(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &fileId, O_RDONLY);
    }
    if (ret != AFP_SERVER_RESULT_OKAY) {
        qCDebug(logAfp) << "kio-afp: get open failed ret=" << ret;
        return mapAfpError(ret, pu.path);
    }
    qCDebug(logAfp) << "kio-afp: get opened fileId=" << fileId;

    // Read loop
    unsigned long long offset = 0;
    char buf[READ_CHUNK];
    bool eof = false;

    while (!eof) {
        unsigned int received = 0;
        unsigned int eofFlag = 0;
        ret = afp_sl_read(&m_volumeId, fileId, 0 /* data fork */,
                          offset, READ_CHUNK, &received, &eofFlag, buf);
        if (ret != AFP_SERVER_RESULT_OKAY) {
            qCDebug(logAfp) << "kio-afp: get read failed at offset" << offset
                     << "ret=" << ret;
            afp_sl_close(&m_volumeId, fileId);
            return mapAfpError(ret, pu.path);
        }

        if (received > 0) {
            data(QByteArray(buf, static_cast<int>(received)));
            offset += received;
        }

        if (eofFlag || received == 0)
            eof = true;
    }

    afp_sl_close(&m_volumeId, fileId);
    qCDebug(logAfp) << "kio-afp: get complete, read" << offset << "bytes";
    data(QByteArray());  // signal end of data
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::put(const QUrl &url, int permissions, KIO::JobFlags flags)
{
    qCDebug(logAfp) << "kio-afp: put()" << url << "permissions=" << permissions
             << "flags=" << static_cast<int>(flags);

    ParsedUrl pu = parseAfpUrl(url);
    if (!pu.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_ACCESS_DENIED,
                   i18n("Cannot write to volume root"));

    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    // Check if file exists
    struct stat st{};
    int ret = afp_sl_stat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &st);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("put stat failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_stat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &st);
    }
    bool exists = (ret == AFP_SERVER_RESULT_OKAY);
    qCDebug(logAfp) << "kio-afp: put stat ret=" << ret << "exists=" << exists;

    if (exists && !(flags & KIO::Overwrite))
        return KIO::WorkerResult::fail(KIO::ERR_FILE_ALREADY_EXIST, pu.path);

    // Create file if it doesn't exist
    if (!exists) {
        mode_t mode = (permissions == -1) ? 0644 : static_cast<mode_t>(permissions);
        ret = afp_sl_creat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, mode);
        qCDebug(logAfp) << "kio-afp: put creat ret=" << ret;
        if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
            invalidateSessionState("put creat failed");
            auto rr = ensureAttached(pu);
            if (!rr.success())
                return rr;
            ret = afp_sl_creat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, mode);
            qCDebug(logAfp) << "kio-afp: put creat retry ret=" << ret;
        }
        if (ret != AFP_SERVER_RESULT_OKAY)
            return mapAfpError(ret, pu.path);
    }

    // Truncate before open when overwriting (matches reference implementation)
    if (exists && (flags & KIO::Overwrite)) {
        ret = afp_sl_truncate(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, 0);
        qCDebug(logAfp) << "kio-afp: put truncate ret=" << ret;
        if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
            invalidateSessionState("put truncate failed");
            auto rr = ensureAttached(pu);
            if (!rr.success())
                return rr;
            ret = afp_sl_truncate(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, 0);
            qCDebug(logAfp) << "kio-afp: put truncate retry ret=" << ret;
        }
        if (ret != AFP_SERVER_RESULT_OKAY)
            return mapAfpError(ret, pu.path);
    }

    // Open for read/write (AFP servers may not handle write-only correctly)
    unsigned int fileId = 0;
    ret = afp_sl_open(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &fileId, O_RDWR);
    qCDebug(logAfp) << "kio-afp: put open ret=" << ret << "fileId=" << fileId;
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("put open failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_open(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &fileId, O_RDWR);
        qCDebug(logAfp) << "kio-afp: put open retry ret=" << ret << "fileId=" << fileId;
    }
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    // Write loop — read data from KIO
    unsigned long long offset = 0;
    int readResult = 0;

    while (true) {
        QByteArray buf;
        dataReq();
        readResult = readData(buf);
        if (readResult < 0) {
            qCDebug(logAfp) << "kio-afp: put readData failed:" << readResult;
            afp_sl_close(&m_volumeId, fileId);
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_WRITE,
                       i18n("Error reading data from client"));
        }
        if (buf.isEmpty())
            break;

        unsigned int written = 0;
        ret = afp_sl_write(&m_volumeId, fileId, 0 /* data fork */,
                           offset, static_cast<unsigned int>(buf.size()),
                           &written, buf.constData());
        if (ret != AFP_SERVER_RESULT_OKAY) {
            qCDebug(logAfp) << "kio-afp: put write failed at offset" << offset
                     << "ret=" << ret;
            afp_sl_close(&m_volumeId, fileId);
            return mapAfpError(ret, pu.path);
        }
        offset += written;
    }

    afp_sl_close(&m_volumeId, fileId);
    qCDebug(logAfp) << "kio-afp: put complete, wrote" << offset << "bytes";

    // Set permissions after writing (non-fatal if it fails)
    if (permissions != -1) {
        ret = afp_sl_chmod(&m_volumeId, pu.afpUrl.path, &pu.afpUrl,
                           static_cast<mode_t>(permissions));
        if (ret != AFP_SERVER_RESULT_OKAY)
            qCDebug(logAfp) << "kio-afp: put chmod failed (non-fatal) ret=" << ret;
    }

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::mkdir(const QUrl &url, int permissions)
{
    qCDebug(logAfp) << "AfpWorker::mkdir()" << url;

    ParsedUrl pu = parseAfpUrl(url);
    if (!pu.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_ACCESS_DENIED,
                   i18n("Cannot create directory at volume level"));

    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    mode_t mode = (permissions == -1) ? 0755 : static_cast<mode_t>(permissions);
    int ret = afp_sl_mkdir(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, mode);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("mkdir failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_mkdir(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, mode);
    }
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::del(const QUrl &url, bool isFile)
{
    qCDebug(logAfp) << "AfpWorker::del()" << url << "isFile:" << isFile;

    ParsedUrl pu = parseAfpUrl(url);
    if (!pu.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_ACCESS_DENIED,
                   i18n("Cannot delete volume root"));

    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    int ret;
    if (isFile)
        ret = afp_sl_unlink(&m_volumeId, pu.afpUrl.path, &pu.afpUrl);
    else
        ret = afp_sl_rmdir(&m_volumeId, pu.afpUrl.path, &pu.afpUrl);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("delete failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        if (isFile)
            ret = afp_sl_unlink(&m_volumeId, pu.afpUrl.path, &pu.afpUrl);
        else
            ret = afp_sl_rmdir(&m_volumeId, pu.afpUrl.path, &pu.afpUrl);
    }

    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags)
{
    qCDebug(logAfp) << "AfpWorker::rename()" << src << "->" << dest;

    ParsedUrl puSrc = parseAfpUrl(src);
    ParsedUrl puDest = parseAfpUrl(dest);

    if (!puSrc.hasPath || !puDest.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION,
                   i18n("Cannot rename volume roots"));

    if (puSrc.server != puDest.server || puSrc.volume != puDest.volume)
        return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION,
                   i18n("Cannot rename across different volumes"));

    auto r = ensureAttached(puSrc);
    if (!r.success())
        return r;

    // Check if destination exists when Overwrite is not set
    if (!(flags & KIO::Overwrite)) {
        struct stat st{};
        int check = afp_sl_stat(&m_volumeId, puDest.afpUrl.path, &puDest.afpUrl, &st);
        if (check == AFP_SERVER_RESULT_OKAY)
            return KIO::WorkerResult::fail(KIO::ERR_FILE_ALREADY_EXIST, puDest.path);
    }

    int ret = afp_sl_rename(&m_volumeId, puSrc.afpUrl.path, puDest.afpUrl.path,
                            &puSrc.afpUrl);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("rename failed");
        auto rr = ensureAttached(puSrc);
        if (!rr.success())
            return rr;
        ret = afp_sl_rename(&m_volumeId, puSrc.afpUrl.path, puDest.afpUrl.path,
                            &puSrc.afpUrl);
    }
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, puSrc.path);

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::chmod(const QUrl &url, int permissions)
{
    qCDebug(logAfp) << "AfpWorker::chmod()" << url;

    ParsedUrl pu = parseAfpUrl(url);
    if (!pu.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION,
                   i18n("Cannot chmod volume root"));

    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    int ret = afp_sl_chmod(&m_volumeId, pu.afpUrl.path, &pu.afpUrl,
                           static_cast<mode_t>(permissions));
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("chmod failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_chmod(&m_volumeId, pu.afpUrl.path, &pu.afpUrl,
                           static_cast<mode_t>(permissions));
    }
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::fileSystemFreeSpace(const QUrl &url)
{
    qCDebug(logAfp) << "kio-afp: fileSystemFreeSpace()" << url;

    ParsedUrl pu = parseAfpUrl(url);
    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    struct statvfs svfs{};
    int ret = afp_sl_statfs(&m_volumeId, "/", &pu.afpUrl, &svfs);
    if (ret != AFP_SERVER_RESULT_OKAY && isRecoverableSessionError(ret)) {
        invalidateSessionState("statfs failed");
        auto rr = ensureAttached(pu);
        if (!rr.success())
            return rr;
        ret = afp_sl_statfs(&m_volumeId, "/", &pu.afpUrl, &svfs);
    }
    if (ret != AFP_SERVER_RESULT_OKAY) {
        qCDebug(logAfp) << "kio-afp: statfs failed ret=" << ret;
        return mapAfpError(ret, pu.volume);
    }

    const KIO::filesize_t total =
        static_cast<KIO::filesize_t>(svfs.f_blocks) * svfs.f_frsize;
    const KIO::filesize_t available =
        static_cast<KIO::filesize_t>(svfs.f_bavail) * svfs.f_frsize;

    qCDebug(logAfp) << "kio-afp: fileSystemFreeSpace total=" << total
             << "available=" << available;

    setMetaData(QStringLiteral("total"), QString::number(total));
    setMetaData(QStringLiteral("available"), QString::number(available));
    return KIO::WorkerResult::pass();
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

extern "C" {
int Q_DECL_EXPORT kdemain(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kio-afp"));
    KLocalizedString::setApplicationDomain(TRANSLATION_DOMAIN);
    if (argc < 4) {
        fprintf(stderr, "Usage: kio-afp protocol pool app\n");
        return 1;
    }

    AfpWorker worker(argv[2], argv[3]);
    worker.dispatchLoop();
    return 0;
}
}

int main(int argc, char **argv) {
    return kdemain(argc, argv);
}
