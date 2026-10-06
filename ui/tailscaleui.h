/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef PLASMA_NM_TAILSCALE_UI_H
#define PLASMA_NM_TAILSCALE_UI_H

#include "vpnuiplugin.h"

class Q_DECL_EXPORT TailscaleUiPlugin : public VpnUiPlugin
{
    Q_OBJECT

public:
    explicit TailscaleUiPlugin(QObject *parent = nullptr, const QVariantList & = QVariantList());

    SettingWidget *widget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent) override;
    SettingWidget *askUser(const NetworkManager::VpnSetting::Ptr &setting, const QStringList &hints, QWidget *parent) override;
    QString suggestedFileName(const NetworkManager::ConnectionSettings::Ptr &connection) const override;
};

#endif // PLASMA_NM_TAILSCALE_UI_H
