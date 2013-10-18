/*************************************************************************************
 *  Copyright (C) 2012 by Alejandro Fiestas Olivares <afiestas@kde.org>              *
 *  Copyright (C) 2013 by Daniel Vrátil <dvratil@redhat.com>                         *
 *                                                                                   *
 *  This program is free software; you can redistribute it and/or                    *
 *  modify it under the terms of the GNU General Public License                      *
 *  as published by the Free Software Foundation; either version 2                   *
 *  of the License, or (at your option) any later version.                           *
 *                                                                                   *
 *  This program is distributed in the hope that it will be useful,                  *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of                   *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                    *
 *  GNU General Public License for more details.                                     *
 *                                                                                   *
 *  You should have received a copy of the GNU General Public License                *
 *  along with this program; if not, write to the Free Software                      *
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA   *
 *************************************************************************************/

#include "serializer.h"

#include <QtCore/QStringList>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QVariant>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>

#include <qjson/serializer.h>
#include <qjson/parser.h>

#include <kdebug.h>

#include <kscreen/config.h>
#include <kscreen/output.h>
#include <kscreen/edid.h>

#include <KStandardDirs>
#include <KLocalizedString>

QString Serializer::currentConfigId()
{
    KScreen::OutputList outputs = KScreen::Config::current()->outputs();

    QStringList hashList;
    Q_FOREACH(const KScreen::Output* output, outputs) {
        if (!output->isConnected()) {
            continue;
        }

        kDebug() << "Part of the Id: " << Serializer::outputId(output);
        hashList.insert(0, Serializer::outputId(output));
    }

    qSort(hashList.begin(), hashList.end());

    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(hashList.join(QString()).toAscii());
    return hash.result().toHex();
}

bool Serializer::configExists()
{
    return Serializer::configExists(Serializer::currentConfigId());
}

bool Serializer::configExists(const QString &configId)
{
    QString path = KStandardDirs::locateLocal("data", "kscreen/" + configId);
    return QFile::exists(path);
}

KScreen::Config* Serializer::config(const QString &configId, const QString &profileId)
{
    QJson::Parser parser;
    KScreen::Config* config = KScreen::Config::current();
    if (!config) {
        return 0;
    }

    KScreen::OutputList outputList = config->outputs();
    QFile file(KStandardDirs::locateLocal("data", "kscreen/" + configId));
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }

    bool ok = false;
    const QVariant v = parser.parse(file.readAll(), &ok);
    if (!ok || !v.isValid()) {
        return 0;
    }

    QVariantList outputs;

    const QVariantMap map = v.toMap();
    if (!map.contains(QLatin1String("version"))) {
        outputs = v.toList();
    } else {
        const QVariantList profiles = map[QLatin1String("profiles")].toList();
        if (!profileId.isEmpty()) {
            Q_FOREACH (const QVariant &profile, profiles) {
                const QVariantMap info = profile.toMap();
                if (info[QLatin1String("id")].toString() == profileId) {
                    outputs = info[QLatin1String("outputs")].toList();
                    break;
                }
            }
        }

        // No profile was chosen, or invalid profile ID was given - just get
        // the first profile and continue
        if (profileId.isEmpty() || outputs.isEmpty()) {
            const QVariantMap profile = profiles.first().toMap();
            outputs = profile[QLatin1String("outputs")].toList();
        }
    }

    Q_FOREACH(KScreen::Output* output, outputList) {
        if (!output->isConnected() && output->isEnabled()) {
            output->setEnabled(false);
        }
    }

    Q_FOREACH(const QVariant &info, outputs) {
        KScreen::Output* output = Serializer::findOutput(info.toMap());
        if (!output) {
            continue;
        }

        delete outputList.take(output->id());
        outputList.insert(output->id(), output);
    }
    config->setOutputs(outputList);

    return config;
}

bool Serializer::saveConfig(KScreen::Config* config)
{
    KScreen::OutputList outputs = config->outputs();

    QVariantList outputList;
    Q_FOREACH(KScreen::Output *output, outputs) {
        if (!output->isConnected()) {
            continue;
        }

        QVariantMap info;

        info["id"] = Serializer::outputId(output);
        info["primary"] = output->isPrimary();
        info["enabled"] = output->isEnabled();
        info["rotation"] = output->rotation();

        QVariantMap pos;
        pos["x"] = output->pos().x();
        pos["y"] = output->pos().y();
        info["pos"] = pos;

        if (output->isEnabled()) {
            KScreen::Mode *mode = output->currentMode();

            QVariantMap modeInfo;
            modeInfo["refresh"] = mode->refreshRate();

            QVariantMap modeSize;
            modeSize["width"] = mode->size().width();
            modeSize["height"] = mode->size().height();
            modeInfo["size"] = modeSize;

            info["mode"] = modeInfo;
        }

        info["metadata"] = Serializer::metadata(output);

        outputList.append(info);
    }

    QJson::Serializer serializer;
    QByteArray json = serializer.serialize(outputList);

    QString path = KStandardDirs::locateLocal("data", "kscreen/"+ Serializer::currentConfigId());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(json);
    file.close();

    kDebug() << "Config saved on: " << path;
    return true;
}

KScreen::Output* Serializer::findOutput(const QVariantMap& info)
{
    KScreen::Config *config = KScreen::Config::current();
    if (!config) {
        return 0;
    }

    KScreen::OutputList outputs = config->outputs();
    Q_FOREACH(KScreen::Output* output, outputs) {
        if (!output->isConnected()) {
            continue;
        }
        if (Serializer::outputId(output) != info["id"].toString()) {
            continue;
        }

        QVariantMap posInfo = info["pos"].toMap();
        QPoint point(posInfo["x"].toInt(), posInfo["y"].toInt());
        output->setPos(point);
        output->setPrimary(info["primary"].toBool());
        output->setEnabled(info["enabled"].toBool());
        output->setRotation(static_cast<KScreen::Output::Rotation>(info["rotation"].toInt()));

        QVariantMap modeInfo = info["mode"].toMap();
        QVariantMap modeSize = modeInfo["size"].toMap();
        QSize size(modeSize["width"].toInt(), modeSize["height"].toInt());

        kDebug() << "Finding a mode with: ";
        kDebug() << size;
        kDebug() << modeInfo["refresh"].toString();

        KScreen::ModeList modes = output->modes();
        Q_FOREACH(KScreen::Mode* mode, modes) {
            if (mode->size() != size) {
                continue;
            }
            if (QString::number(mode->refreshRate()) != modeInfo["refresh"].toString()) {
                continue;
            }

            kDebug() << "Found: " << mode->id() << " " << mode->name();
            output->setCurrentModeId(mode->id());
            break;
        }
        return output;
    }

    return 0;
}

QString Serializer::outputId(const KScreen::Output* output)
{
    if (output->edid() && output->edid()->isValid()) {
        return output->edid()->hash();
    }

    return output->name();
}

QVariantMap Serializer::metadata(const KScreen::Output* output)
{
    QVariantMap metadata;
    metadata["name"] = output->name();
    if (!output->edid() || !output->edid()->isValid()) {
        return metadata;
    }

    metadata["fullname"] = output->edid()->deviceId();
    return metadata;
}

QMap<QString, QString> Serializer::listProfiles(const QString &configId)
{
    if (!Serializer::configExists(configId)) {
        return QMap<QString,QString>();
    }

    QJson::Parser parser;
    QFile file(KStandardDirs::locateLocal("data", "kscreen/" + configId));
    if (!file.open(QIODevice::ReadOnly)) {
        return QMap<QString,QString>();
    }

    QJson::Parser parser;
    bool ok = false;
    const QVariant v = parser.parse(file.readAll(), &ok);
    if (!ok || !v.isValid()) {
        return QMap<QString,QString>();
    }

    QMap<QString,QString> profiles;

    const QVariantMap map = v.toMap();
    // Version 1
    if (!map.contains(QLatin1String("version"))) {
        profiles.insert(QString(), i18n("Default"));
        return;
    }

    const QVariantList profilesList = map[QLatin1String("profiles")].toList();
    Q_FOREACH (const QVariant &profile, profilesList) {
        const QVariantMap info = profile.toMap();
        profiles.insert(info[QLatin1String("id")].toString(),
                        info[QLatin1String("name")].toString());
    }

    return profiles;
}

void Serializer::removeProfile(const QString &configId, const QString &profileId)
{
    if (!Serializer::configExists(configId)) {
        return;
    }

    QFile file(KStandardDirs::locateLocal("data", "kscreen/" + configId));
    if (!file.open(QIODevice::ReadWrite)) {
        return QMap<QString,QString>();
    }

    QJson::Parser parser;
    bool ok = false;
    const QVariant v = parser.parse(file.readAll(), &ok);
    if (!ok || !v.isValid()) {
        return;
    }

    QVariantMap map = v.toMap();

    // Version 1 did not support profiles
    if (!map.contains(QLatin1String("version"))) {
        return;
    }

    QVariantList profiles = map[QLatin1String("profiles")].toList();
    // Find the profile, remove it from the list
    for (int i = 0; i < profiles.count(); i++) {
        const QVariantMap info = profiles.at(i).toMap();
        if (info[QLatin1String("id")].toString() == profileId) {
            profiles.removeAt(i);
            break;
        }
    }

    // Update the parent map
    map[QLatin1String("profiles")] = profiles;

    // Write it back to file
    QJson::Serializer serializer;
    file.resize(0);
    file.write(serializer.serialize(map));
}
