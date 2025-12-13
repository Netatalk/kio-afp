#include <KIO/WorkerBase>
#include <KLocalizedString>
#include <QUrl>
#include <QDebug>
#include <QCoreApplication>
#include <QProcess>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QMimeDatabase>

extern "C" {
    #include <libafpclient.h>
    #include <afp.h>
    #include <afpsl.h>
    #include <afpfsd.h>
    #include <map_def.h>
}

class AfpWorker : public KIO::WorkerBase {
public:
    AfpWorker(const QByteArray &pool, const QByteArray &app)
        : KIO::WorkerBase("afp", pool, app) {}

    // AFP state
    bool m_loggedOn = false;
    bool m_attached = false;
    struct afp_url m_url{};

    KIO::WorkerResult get(const QUrl &url) override {
        qDebug() << "AfpWorker::get()" << url;
        if (!url.isValid() || url.scheme().isEmpty() || url.scheme() != QStringLiteral("afp")) {
            qDebug() << "AfpWorker::get(): invalid or wrong-scheme URL" << url;
            return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, i18n("Unsupported URL or protocol"));
        }
        QString stderrOut;
        const QString mountPath = resolveMountPath(url, stderrOut);
        if (mountPath.isEmpty()) {
            qDebug() << "resolveMountPath failed:" << stderrOut;
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_MOUNT, i18n("AFP mount path unavailable"));
        }
        const QString localPath = mapUrlToLocal(url, mountPath);
        QFile file(localPath);
        if (!file.exists()) {
            return KIO::WorkerResult::fail(KIO::ERR_DOES_NOT_EXIST, i18n("File does not exist"));
        }
        if (!file.open(QIODevice::ReadOnly)) {
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_OPEN_FOR_READING, i18n("Cannot open file"));
        }

        const qint64 total = file.size();
        totalSize(total);

        const qint64 chunk = 64 * 1024;
        while (!file.atEnd()) {
            const QByteArray buf = file.read(chunk);
            if (buf.isEmpty() && !file.atEnd()) {
                return KIO::WorkerResult::fail(KIO::ERR_CANNOT_READ, i18n("Error reading file"));
            }
            this->data(buf);
        }
        return KIO::WorkerResult::pass();
    }

    KIO::WorkerResult stat(const QUrl &url) override {
        qDebug() << "AfpWorker::stat()" << url;
        if (!url.isValid() || url.scheme().isEmpty() || url.scheme() != QStringLiteral("afp")) {
            qDebug() << "AfpWorker::stat(): invalid or wrong-scheme URL" << url;
            return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, i18n("Unsupported URL or protocol"));
        }
        // Resolve a local mount for this AFP URL and return info about the
        // target file/dir from the mounted filesystem.
        QString stderrOut;
        const QString mountPath = resolveMountPath(url, stderrOut);
        if (mountPath.isEmpty()) {
            qDebug() << "resolveMountPath failed:" << stderrOut;
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_MOUNT, i18n("AFP mount path unavailable"));
        }

        // Build local path for the requested entity
        const QString localPath = mapUrlToLocal(url, mountPath);
        QFileInfo fi(localPath);
        if (!fi.exists()) {
            return KIO::WorkerResult::fail(KIO::ERR_DOES_NOT_EXIST, i18n("File does not exist"));
        }

        KIO::UDSEntry entry;
        entry.reserve(6);
        entry.fastInsert(KIO::UDSEntry::UDS_NAME, fi.fileName());
        entry.fastInsert(KIO::UDSEntry::UDS_SIZE, (qulonglong)fi.size());
        entry.fastInsert(KIO::UDSEntry::UDS_ACCESS, (uint)fi.permissions());
        entry.fastInsert(KIO::UDSEntry::UDS_USER, fi.owner());
        entry.fastInsert(KIO::UDSEntry::UDS_GROUP, fi.group());
        // MIME type
        QMimeDatabase db;
        const QString mt = db.mimeTypeForFile(fi.absoluteFilePath()).name();
        entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, mt);

        statEntry(entry);
        return KIO::WorkerResult::pass();
    }

    KIO::WorkerResult listDir(const QUrl &url) override {
        qDebug() << "AfpWorker::listDir()" << url;
        if (!url.isValid() || url.scheme().isEmpty() || url.scheme() != QStringLiteral("afp")) {
            qDebug() << "AfpWorker::listDir(): invalid or wrong-scheme URL" << url;
            return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, i18n("Unsupported URL or protocol"));
        }
        // Try to get or create a local mount path and list it directly. This avoids
        // relying on a redirection being accepted by the core (some setups reject
        // redirects to local file URIs). If we can mount, enumerate the directory
        // and return entries.
        const QString server = url.host();
        const QString share = url.path().isEmpty() ? QString() : url.path().mid(1);
        QStringList args;
        args << QStringLiteral("--server") << server;
        if (!share.isEmpty()) { args << QStringLiteral("--share") << share; }
        QProcess proc;
        proc.start(QStringLiteral("afp_connect"), args);
        if (!proc.waitForFinished(15000)) {
            proc.kill();
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_CONNECT, i18n("AFP helper did not respond"));
        }
        const QString mountPath = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        if (mountPath.isEmpty()) {
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_MOUNT, i18n("AFP mount path unavailable"));
        }
        QDir dir(mountPath);
        if (!dir.exists() || !dir.isReadable()) {
            return KIO::WorkerResult::fail(KIO::ERR_DOES_NOT_EXIST, i18n("AFP mount path not readable"));
        }
        KIO::UDSEntryList entries;
        const QFileInfoList files = dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
        for (const QFileInfo &fi : files) {
            KIO::UDSEntry e;
            e.reserve(5);
            e.fastInsert(KIO::UDSEntry::UDS_NAME, fi.fileName());
            e.fastInsert(KIO::UDSEntry::UDS_SIZE, (qulonglong)fi.size());
            e.fastInsert(KIO::UDSEntry::UDS_ACCESS, (uint)fi.permissions());
            e.fastInsert(KIO::UDSEntry::UDS_USER, fi.owner());
            e.fastInsert(KIO::UDSEntry::UDS_GROUP, fi.group());
            entries << e;
        }
        listEntries(entries);
        return KIO::WorkerResult::pass();
    }

    // Attempt to log into the AFP server for this URL using libafpclient.
    // Returns 0 on success, negative on errors. Error string is set in errmsg.
    int afpLogin(const QUrl &url, QString &errmsg) {
        // Convert QUrl and parse with library helper
        const QByteArray kafpurl = url.toString(QUrl::RemoveUserInfo | QUrl::RemoveFragment | QUrl::RemoveQuery).toUtf8();
        afp_default_url(&m_url);
        afp_parse_url(&m_url, kafpurl.constData(), 0);

        if (m_loggedOn) return 0;

        // Build a connection request and ask libafpclient to perform a full connect
        struct afp_connection_request req;
        memset(&req, 0, sizeof(req));
        req.uam_mask = default_uams_mask();
        memcpy(&req.url, &m_url, sizeof(struct afp_url));

        struct afp_server *server = afp_server_full_connect(NULL, &req);
        if (!server) {
            errmsg = i18n("Could not connect to AFP server");
            return -1;
        }

        char loginmesg[1024] = {0};
        unsigned int l = 0;
        int ret = afp_server_login(server, loginmesg, &l, sizeof(loginmesg));
        if (ret != 0) {
            errmsg = handleConnectErrors(ret);
            afp_free_server(&server);
            m_loggedOn = false;
            return -1;
        }

        if (strlen(loginmesg) > 0) {
            qDebug() << "Login message:" << loginmesg;
        }

        m_loggedOn = true;
        afp_free_server(&server);
        return 0;
    }

    // Map legacy handle_connect_errors: translate return value into human text.
    QString handleConnectErrors(int ret) {
        switch (ret) {
        case 0:
            return QString();
        case -EACCES:
            return i18n("Login incorrect");
        case -ENONET:
            return i18n("Could not get address of server");
        case -ETIMEDOUT:
            return i18n("Timeout connecting to server");
        case -EHOSTUNREACH:
            return i18n("No route to host");
        case -ECONNREFUSED:
            return i18n("Connection refused");
        case -ENETUNREACH:
            return i18n("Server unreachable");
        default:
            return i18n("Internal error");
        }
    }


private:
    // Resolve the mounted path using the helper; returns empty on failure and stderr in out arg
    QString resolveMountPath(const QUrl &url, QString &stderrOut) {
        const QString server = url.host();
        const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        const QString share = parts.size() ? parts.at(0) : QString();
        // Keep using the helper to manage mounts and authentication. We avoid
        // calling libafpclient directly here because it spawns background
        // threads and file descriptors that are not safe to create on every
        // KIO request and can lead to crashes when the worker is repeatedly
        // started/stopped by the core.
        QStringList args;
        args << QStringLiteral("--server") << server;
        if (!share.isEmpty()) args << QStringLiteral("--share") << share;
        QProcess proc;
        proc.start(QStringLiteral("afp_connect"), args);
        if (!proc.waitForFinished(15000)) {
            proc.kill();
            stderrOut = i18n("AFP helper did not respond");
            return QString();
        }
        const QString stdoutOut = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        stderrOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return stdoutOut;
    }

    // Map an AFP URL into the local filesystem path under the mount root
    QString mapUrlToLocal(const QUrl &url, const QString &mountRoot) {
        const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString rel;
        if (parts.size() > 1) {
            QStringList sub = parts.mid(1);
            rel = QDir::cleanPath(sub.join(QStringLiteral("/")));
        }
        if (rel.isEmpty()) return mountRoot;
        return QDir(mountRoot).filePath(rel);
    }
};

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
