/*
 * Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <KIO/WorkerBase>
#include <KLocalizedString>
#include <QUrl>
#include <QDebug>
#include <QCoreApplication>
#include <QMimeDatabase>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <pwd.h>
#include <grp.h>
#include <cerrno>
#include <fcntl.h>
#include <cstring>

extern "C" {
    #include <afp.h>
    #include <afpsl.h>
    #include <afpfsd.h>
}

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

private:
    // --- State ---
    bool m_connSetupDone = false;
    QString m_cachedServer;
    serverid_t m_serverId = nullptr;
    QString m_cachedVolume;
    volumeid_t m_volumeId = nullptr;

    // --- URL parsing ---
    ParsedUrl parseAfpUrl(const QUrl &url);

    // --- Connection lifecycle ---
    KIO::WorkerResult ensureConnSetup();
    KIO::WorkerResult ensureConnected(ParsedUrl &pu);
    KIO::WorkerResult ensureAttached(ParsedUrl &pu);

    // --- UDSEntry helpers ---
    KIO::UDSEntry statToUDS(const struct stat &st, const QString &name);
    KIO::UDSEntry serverOrVolumeEntry(const QString &name);
    KIO::UDSEntry volumeSummaryToUDS(const struct afp_volume_summary &vol);

    // --- Error mapping ---
    KIO::WorkerResult mapAfpError(int ret, const QString &path);
    KIO::WorkerResult mapAfpConnectError(int ret);
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

    // Set the path field in afp_url (relative path within volume)
    QByteArray pathBytes = pu.path.toUtf8();
    if (!pathBytes.isEmpty()) {
        std::strncpy(pu.afpUrl.path, pathBytes.constData(),
                     sizeof(pu.afpUrl.path) - 1);
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
        qWarning() << "kio_afp: disconnecting from" << m_cachedServer;
        afp_sl_disconnect(&m_serverId);
        m_serverId = nullptr;
        m_cachedServer.clear();
        m_volumeId = nullptr;
        m_cachedVolume.clear();
    }

    if (m_serverId) {
        qWarning() << "kio_afp: already connected to" << m_cachedServer;
        return KIO::WorkerResult::pass();
    }

    serverid_t sid = nullptr;
    char loginmesg[AFP_LOGINMESG_LEN] = {};
    int connectError = 0;
    unsigned int uamMask = default_uams_mask();

    qWarning() << "kio_afp: connect server=" << pu.server
               << "user=" << pu.afpUrl.username;
    int ret = afp_sl_connect(&pu.afpUrl, uamMask, &sid, loginmesg, &connectError);
    qWarning() << "kio_afp: connect returned" << ret
               << "sid=" << sid << "err=" << connectError;

    if (ret == AFP_SERVER_RESULT_ALREADY_CONNECTED) {
        qWarning() << "kio_afp: already connected, sid=" << sid;
        m_serverId = sid;
        m_cachedServer = pu.server;
        return KIO::WorkerResult::pass();
    }

    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpConnectError(ret);

    m_serverId = sid;
    m_cachedServer = pu.server;

    if (std::strlen(loginmesg) > 0)
        qWarning() << "kio_afp: login message:" << loginmesg;

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::ensureAttached(ParsedUrl &pu)
{
    if (!pu.hasVolume)
        return KIO::WorkerResult::fail(KIO::ERR_DOES_NOT_EXIST,
                   i18n("No volume specified in URL"));

    auto r = ensureConnected(pu);
    if (!r.success())
        return r;

    // If attached to a different volume, detach first
    if (m_volumeId && m_cachedVolume != pu.volume) {
        afp_sl_detach(&m_volumeId, &pu.afpUrl);
        m_volumeId = nullptr;
        m_cachedVolume.clear();
    }

    if (m_volumeId)
        return KIO::WorkerResult::pass();

    volumeid_t vid = nullptr;

    qWarning() << "kio_afp: attach volume=" << pu.volume;
    int ret = afp_sl_attach(&pu.afpUrl, 0, &vid);
    qWarning() << "kio_afp: attach returned" << ret << "vid=" << vid;

    if (ret == AFP_SERVER_RESULT_ALREADY_MOUNTED
        || ret == AFP_SERVER_RESULT_ALREADY_ATTACHED) {
        // Volume already known to daemon from a previous worker/session.
        // Detach, then re-attach to get a fresh volume ID.
        qWarning() << "kio_afp: stale volume state, detaching first";
        volumeid_t stale = nullptr;
        afp_sl_detach(&stale, &pu.afpUrl);

        vid = nullptr;
        ret = afp_sl_attach(&pu.afpUrl, 0, &vid);
        qWarning() << "kio_afp: attach retry returned" << ret << "vid=" << vid;
    }

    if (ret == AFP_SERVER_RESULT_ALREADY_ATTACHED) {
        qWarning() << "kio_afp: still attached, calling getvolid";
        ret = afp_sl_getvolid(&pu.afpUrl, &vid);
        qWarning() << "kio_afp: getvolid returned" << ret << "vid=" << vid;
        if (ret != AFP_SERVER_RESULT_OKAY)
            return mapAfpError(ret, pu.volume);
    } else if (ret != AFP_SERVER_RESULT_OKAY) {
        return mapAfpError(ret, pu.volume);
    }

    m_volumeId = vid;
    m_cachedVolume = pu.volume;
    return KIO::WorkerResult::pass();
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
                     S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    entry.fastInsert(KIO::UDSEntry::UDS_USER, QStringLiteral("root"));
    entry.fastInsert(KIO::UDSEntry::UDS_GROUP, QStringLiteral("root"));
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
                     S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    entry.fastInsert(KIO::UDSEntry::UDS_USER, QStringLiteral("root"));
    entry.fastInsert(KIO::UDSEntry::UDS_GROUP, QStringLiteral("root"));
    return entry;
}

// ---------------------------------------------------------------------------
// Error mapping
// ---------------------------------------------------------------------------

KIO::WorkerResult AfpWorker::mapAfpError(int ret, const QString &path)
{
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
                   i18n("Server not found"));
    case AFP_SERVER_RESULT_TIMEDOUT:
        return KIO::WorkerResult::fail(KIO::ERR_SERVER_TIMEOUT, path);
    case AFP_SERVER_RESULT_AFPFSD_ERROR:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   i18n("Cannot communicate with afpsld daemon"));
    case AFP_SERVER_RESULT_NOTSUPPORTED:
        return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, path);
    case AFP_SERVER_RESULT_NOTCONNECTED:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   i18n("Not connected to server"));
    case AFP_SERVER_RESULT_NOTATTACHED:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   i18n("Not attached to volume"));
    case AFP_SERVER_RESULT_NOAUTHENT:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_AUTHENTICATE,
                   i18n("Authentication failed"));
    default:
        return KIO::WorkerResult::fail(KIO::ERR_INTERNAL,
                   i18n("AFP error %1", ret));
    }
}

KIO::WorkerResult AfpWorker::mapAfpConnectError(int ret)
{
    switch (ret) {
    case AFP_SERVER_RESULT_NOAUTHENT:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_AUTHENTICATE,
                   i18n("Authentication failed"));
    case AFP_SERVER_RESULT_NOSERVER:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   i18n("Could not find AFP server"));
    case AFP_SERVER_RESULT_TIMEDOUT:
        return KIO::WorkerResult::fail(KIO::ERR_SERVER_TIMEOUT,
                   i18n("Connection timed out"));
    case AFP_SERVER_RESULT_AFPFSD_ERROR:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   i18n("Cannot communicate with afpsld daemon"));
    default:
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT,
                   i18n("AFP connect error %1", ret));
    }
}

// ---------------------------------------------------------------------------
// KIO operations
// ---------------------------------------------------------------------------

KIO::WorkerResult AfpWorker::stat(const QUrl &url)
{
    qDebug() << "AfpWorker::stat()" << url;

    ParsedUrl pu = parseAfpUrl(url);

    // Server root: afp://server
    if (!pu.hasVolume) {
        auto r = ensureConnected(pu);
        if (!r.success())
            return r;
        statEntry(serverOrVolumeEntry(QString()));
        return KIO::WorkerResult::pass();
    }

    // Volume root: afp://server/volume
    if (!pu.hasPath) {
        auto r = ensureAttached(pu);
        if (!r.success())
            return r;
        statEntry(serverOrVolumeEntry(pu.volume));
        return KIO::WorkerResult::pass();
    }

    // File/dir within volume
    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    struct stat st{};
    int ret = afp_sl_stat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &st);
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
    qDebug() << "AfpWorker::listDir()" << url;

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

    // Path for readdir: empty string for volume root, or the subpath
    const char *dirPath = pu.hasPath ? pu.afpUrl.path : "";

    constexpr int BATCH = 64;
    int start = 0;
    bool done = false;

    while (!done) {
        struct afp_file_info_basic *fpb = nullptr;
        unsigned int numFiles = 0;
        int eod = 0;

        qWarning() << "kio_afp: readdir path=" << dirPath
                   << "start=" << start << "vid=" << m_volumeId;
        int ret = afp_sl_readdir(&m_volumeId, dirPath, &pu.afpUrl,
                                 start, BATCH, &numFiles, &fpb, &eod);
        qWarning() << "kio_afp: readdir returned" << ret
                   << "numFiles=" << numFiles << "eod=" << eod;
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
    qDebug() << "AfpWorker::get()" << url;

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
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    if (S_ISDIR(st.st_mode))
        return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY, pu.path);

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
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

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
    data(QByteArray());  // signal end of data
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::put(const QUrl &url, int permissions, KIO::JobFlags flags)
{
    qDebug() << "AfpWorker::put()" << url;

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
    bool exists = (ret == AFP_SERVER_RESULT_OKAY);

    if (exists && !(flags & KIO::Overwrite))
        return KIO::WorkerResult::fail(KIO::ERR_FILE_ALREADY_EXIST, pu.path);

    // Create file if it doesn't exist
    if (!exists) {
        mode_t mode = (permissions == -1) ? 0644 : static_cast<mode_t>(permissions);
        ret = afp_sl_creat(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, mode);
        if (ret != AFP_SERVER_RESULT_OKAY)
            return mapAfpError(ret, pu.path);
    }

    // Open for writing
    unsigned int fileId = 0;
    ret = afp_sl_open(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, &fileId, O_WRONLY);
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    // If overwriting, truncate
    if (exists && (flags & KIO::Overwrite)) {
        afp_sl_truncate(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, 0);
    }

    // Write loop — read data from KIO
    unsigned long long offset = 0;
    int readResult = 0;

    while (true) {
        QByteArray buf;
        dataReq();
        readResult = readData(buf);
        if (readResult < 0) {
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
            afp_sl_close(&m_volumeId, fileId);
            return mapAfpError(ret, pu.path);
        }
        offset += written;
    }

    afp_sl_close(&m_volumeId, fileId);
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::mkdir(const QUrl &url, int permissions)
{
    qDebug() << "AfpWorker::mkdir()" << url;

    ParsedUrl pu = parseAfpUrl(url);
    if (!pu.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_ACCESS_DENIED,
                   i18n("Cannot create directory at volume level"));

    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    mode_t mode = (permissions == -1) ? 0755 : static_cast<mode_t>(permissions);
    int ret = afp_sl_mkdir(&m_volumeId, pu.afpUrl.path, &pu.afpUrl, mode);
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::del(const QUrl &url, bool isFile)
{
    qDebug() << "AfpWorker::del()" << url << "isFile:" << isFile;

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

    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags)
{
    qDebug() << "AfpWorker::rename()" << src << "->" << dest;

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
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, puSrc.path);

    return KIO::WorkerResult::pass();
}

KIO::WorkerResult AfpWorker::chmod(const QUrl &url, int permissions)
{
    qDebug() << "AfpWorker::chmod()" << url;

    ParsedUrl pu = parseAfpUrl(url);
    if (!pu.hasPath)
        return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION,
                   i18n("Cannot chmod volume root"));

    auto r = ensureAttached(pu);
    if (!r.success())
        return r;

    int ret = afp_sl_chmod(&m_volumeId, pu.afpUrl.path, &pu.afpUrl,
                           static_cast<mode_t>(permissions));
    if (ret != AFP_SERVER_RESULT_OKAY)
        return mapAfpError(ret, pu.path);

    return KIO::WorkerResult::pass();
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

extern "C" {
int Q_DECL_EXPORT kdemain(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kio_afp"));
    KLocalizedString::setApplicationDomain("kio_afp");
    if (argc < 4) {
        fprintf(stderr, "Usage: kio_afp protocol pool app\n");
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
