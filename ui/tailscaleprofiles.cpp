/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "tailscaleprofiles.h"

#include "nm-tailscale.h"

#include <KLocalizedString>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <memory>

bool TailscaleProfile::matches(const QString &selector) const
{
    return id == selector || loginName.compare(selector, Qt::CaseInsensitive) == 0
        || tailnet.compare(selector, Qt::CaseInsensitive) == 0;
}

TailscaleProfiles::TailscaleProfiles(QObject *parent)
    : QObject(parent)
{
}

void TailscaleProfiles::fetch()
{
    const QByteArray envPath = qgetenv(NM_TAILSCALE_SOCKET_ENV);
    const QString path = envPath.isEmpty() ? QStringLiteral(NM_TAILSCALE_DEFAULT_SOCKET) : QString::fromLocal8Bit(envPath);
    auto socket = new QLocalSocket(this);
    auto response = std::make_shared<QByteArray>();

    // HTTP/1.0: tailscaled closes the connection after the (unchunked) response
    connect(socket, &QLocalSocket::connected, socket, [socket] {
        socket->write("GET /localapi/v0/profiles/ HTTP/1.0\r\nHost: local-tailscaled.sock\r\n\r\n");
    });
    connect(socket, &QLocalSocket::readyRead, socket, [socket, response] {
        response->append(socket->readAll());
    });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket, response] {
        response->append(socket->readAll());
        socket->deleteLater();
        finish(*response);
    });
    connect(socket, &QLocalSocket::errorOccurred, this, [this, socket](QLocalSocket::LocalSocketError error) {
        if (error == QLocalSocket::PeerClosedError) {
            return; // handled by disconnected
        }
        socket->disconnect(this);
        socket->deleteLater();
        Q_EMIT failed(i18n("Could not reach tailscaled: %1", socket->errorString()));
    });

    socket->connectToServer(path);
}

void TailscaleProfiles::finish(const QByteArray &response)
{
    const qsizetype headerEnd = response.indexOf("\r\n\r\n");
    const QByteArray statusLine = response.left(response.indexOf("\r\n"));

    if (headerEnd < 0 || !statusLine.contains(" 200 ")) {
        Q_EMIT failed(i18n("Unexpected reply from tailscaled: %1", QString::fromUtf8(statusLine)));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(response.mid(headerEnd + 4));
    if (!doc.isArray()) {
        Q_EMIT failed(i18n("Unexpected reply from tailscaled"));
        return;
    }

    QList<TailscaleProfile> profiles;
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject p = value.toObject();
        TailscaleProfile profile{
            .id = p.value(QLatin1String("ID")).toString(),
            .loginName = p.value(QLatin1String("UserProfile")).toObject().value(QLatin1String("LoginName")).toString(),
            .tailnet = p.value(QLatin1String("NetworkProfile")).toObject().value(QLatin1String("DomainName")).toString(),
        };
        if (profile.loginName.isEmpty()) {
            profile.loginName = p.value(QLatin1String("Name")).toString();
        }
        if (!profile.id.isEmpty()) {
            profiles.append(profile);
        }
    }
    Q_EMIT fetched(profiles);
}

#include "moc_tailscaleprofiles.cpp"
