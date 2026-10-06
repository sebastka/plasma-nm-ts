/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef PLASMA_NM_TAILSCALE_WIDGET_H
#define PLASMA_NM_TAILSCALE_WIDGET_H

#include "settingwidget.h"
#include "tailscaleprofiles.h"

#include <NetworkManagerQt/VpnSetting>

class QComboBox;
class QLabel;

class TailscaleWidget : public SettingWidget
{
    Q_OBJECT

public:
    explicit TailscaleWidget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent = nullptr);

    void loadConfig(const NetworkManager::Setting::Ptr &setting) override;
    QVariantMap setting() const override;
    bool isValid() const override;

private:
    void populate(const QList<TailscaleProfile> &profiles);
    void showError(const QString &error);

    NetworkManager::VpnSetting::Ptr m_setting;
    QString m_selector; // stored profile selector, kept if it matches no profile
    QComboBox *m_profile;
    QLabel *m_status;
};

#endif // PLASMA_NM_TAILSCALE_WIDGET_H
