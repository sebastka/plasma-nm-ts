/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "tailscaleui.h"

#include "tailscalewidget.h"

#include <KLocalizedString>
#include <KPluginFactory>

#include <QLabel>
#include <QVBoxLayout>

K_PLUGIN_CLASS_WITH_JSON(TailscaleUiPlugin, "plasmanetworkmanagement_tailscaleui.json")

namespace
{
/* Tailscale connections carry no secrets, but plasma-nm's secret agent uses
 * askUser()'s widget unconditionally if NetworkManager ever asks. */
class NoSecretsWidget : public SettingWidget
{
public:
    NoSecretsWidget(const NetworkManager::VpnSetting::Ptr &setting, const QStringList &hints, QWidget *parent)
        : SettingWidget(setting, hints, parent)
    {
        auto layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(i18n("Tailscale connections need no credentials."), this));
    }

    QVariantMap setting() const override
    {
        return {};
    }
};
}

TailscaleUiPlugin::TailscaleUiPlugin(QObject *parent, const QVariantList &)
    : VpnUiPlugin(parent)
{
}

SettingWidget *TailscaleUiPlugin::widget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent)
{
    return new TailscaleWidget(setting, parent);
}

SettingWidget *TailscaleUiPlugin::askUser(const NetworkManager::VpnSetting::Ptr &setting, const QStringList &hints, QWidget *parent)
{
    return new NoSecretsWidget(setting, hints, parent);
}

QString TailscaleUiPlugin::suggestedFileName(const NetworkManager::ConnectionSettings::Ptr &connection) const
{
    Q_UNUSED(connection)
    return {};
}

#include "tailscaleui.moc"
#include "moc_tailscaleui.cpp"
