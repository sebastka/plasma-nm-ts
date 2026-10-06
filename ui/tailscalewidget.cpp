/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "tailscalewidget.h"

#include "nm-tailscale.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QDBusMetaType>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>

TailscaleWidget::TailscaleWidget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent)
    : SettingWidget(setting, parent)
    , m_setting(setting)
    , m_profile(new QComboBox(this))
    , m_status(new QLabel(this))
{
    qDBusRegisterMetaType<NMStringMap>();

    m_profile->setPlaceholderText(i18n("Loading accounts…"));
    m_profile->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_status->setWordWrap(true);
    m_status->setText(i18n("Tailscale accounts are added with <tt>tailscale login</tt>. Connecting switches to the "
                           "selected account; routes and DNS stay managed by Tailscale."));

    auto layout = new QFormLayout(this);
    layout->addRow(i18n("Tailscale account:"), m_profile);
    layout->addRow(m_status);

    watchChangedSetting();
    connect(m_profile, &QComboBox::currentIndexChanged, this, &TailscaleWidget::slotWidgetChanged);
    KAcceleratorManager::manage(this);

    if (setting && !setting->isNull()) {
        loadConfig(setting);
    }

    auto profiles = new TailscaleProfiles(this);
    connect(profiles, &TailscaleProfiles::fetched, this, &TailscaleWidget::populate);
    connect(profiles, &TailscaleProfiles::failed, this, &TailscaleWidget::showError);
    profiles->fetch();
}

void TailscaleWidget::loadConfig(const NetworkManager::Setting::Ptr &setting)
{
    Q_UNUSED(setting)

    // Programmatic changes must not mark the connection as modified
    // (watchChangedSetting() treats any combo box change as an edit)
    const QSignalBlocker blocker(m_profile);

    m_selector = m_setting->data().value(QStringLiteral(NM_TAILSCALE_KEY_PROFILE));
    if (!m_selector.isEmpty() && m_profile->count() == 0) {
        // Until tailscaled answers, show what is stored
        m_profile->addItem(m_selector, m_selector);
        m_profile->setCurrentIndex(0);
    }
}

void TailscaleWidget::populate(const QList<TailscaleProfile> &profiles)
{
    const QSignalBlocker blocker(m_profile);
    int selected = -1;

    m_profile->clear();
    for (const TailscaleProfile &p : profiles) {
        const QString label = p.tailnet.isEmpty() || p.tailnet == p.loginName
            ? p.loginName
            : i18nc("Tailscale login name (tailnet)", "%1 (%2)", p.loginName, p.tailnet);
        m_profile->addItem(label, p.id);
        if (!m_selector.isEmpty() && (selected < 0 || p.id == m_selector) && p.matches(m_selector)) {
            selected = m_profile->count() - 1;
        }
    }

    if (selected >= 0) {
        // Saving unchanged keeps the stored form, e.g. a tailnet name set with nmcli
        m_profile->setItemData(selected, m_selector);
    } else if (!m_selector.isEmpty()) {
        m_profile->addItem(i18nc("stored Tailscale profile", "%1 (not found)", m_selector), m_selector);
        selected = m_profile->count() - 1;
    }
    if (profiles.isEmpty()) {
        m_profile->setPlaceholderText(i18n("No Tailscale accounts"));
    } else {
        m_profile->setPlaceholderText(i18n("Select an account"));
    }
    m_profile->setCurrentIndex(selected);
    slotWidgetChanged();
}

void TailscaleWidget::showError(const QString &error)
{
    m_status->setText(error);
    if (m_profile->count() == 0) {
        m_profile->setPlaceholderText(i18n("No Tailscale accounts"));
    }
}

QVariantMap TailscaleWidget::setting() const
{
    NetworkManager::VpnSetting setting;
    setting.setServiceType(QStringLiteral(NM_DBUS_SERVICE_TAILSCALE));

    // Keep keys this page does not edit, such as "interface"
    NMStringMap data = m_setting ? m_setting->data() : NMStringMap();
    const QString profile = m_profile->currentData().toString();
    if (profile.isEmpty()) {
        data.remove(QStringLiteral(NM_TAILSCALE_KEY_PROFILE));
    } else {
        data.insert(QStringLiteral(NM_TAILSCALE_KEY_PROFILE), profile);
    }
    setting.setData(data);

    return setting.toMap();
}

bool TailscaleWidget::isValid() const
{
    return !m_profile->currentData().toString().isEmpty();
}

#include "moc_tailscalewidget.cpp"
