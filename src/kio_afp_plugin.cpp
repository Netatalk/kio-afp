/*
 * Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

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
