#include <QObject>
#include <KPluginFactory>

class AfpMeta : public QObject
{
    Q_OBJECT
public:
    AfpMeta(QObject *parent, const QVariantList &)
        : QObject(parent) {}
};

K_PLUGIN_FACTORY_WITH_JSON(AfpMetaFactory, "afp.json", registerPlugin<AfpMeta>();)

#include "kio_afp_plugin.moc"
