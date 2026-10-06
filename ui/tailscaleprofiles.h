/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef PLASMA_NM_TAILSCALE_PROFILES_H
#define PLASMA_NM_TAILSCALE_PROFILES_H

#include <QList>
#include <QObject>
#include <QString>

struct TailscaleProfile {
    QString id;
    QString loginName;
    QString tailnet;

    /* Same matching rules as the service: ID, then name, login name or tailnet. */
    bool matches(const QString &selector) const;
};

/* Lists the Tailscale profiles known to tailscaled, through its LocalAPI. */
class TailscaleProfiles : public QObject
{
    Q_OBJECT

public:
    explicit TailscaleProfiles(QObject *parent = nullptr);

    void fetch();

Q_SIGNALS:
    void fetched(const QList<TailscaleProfile> &profiles);
    void failed(const QString &error);

private:
    void finish(const QByteArray &response);
};

#endif // PLASMA_NM_TAILSCALE_PROFILES_H
