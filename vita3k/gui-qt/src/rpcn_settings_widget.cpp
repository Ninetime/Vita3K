// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <gui-qt/rpcn_settings_widget.h>

#include <config/state.h>
#include <rpcn/account.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFuture>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// Countries of the PlayStation Network, as reported by sceNpManagerGetAccountRegion
constexpr std::pair<const char *, const char *> COUNTRIES[] = {
    { "ae", "United Arab Emirates" }, { "ar", "Argentina" }, { "at", "Austria" }, { "au", "Australia" },
    { "be", "Belgium" }, { "bg", "Bulgaria" }, { "bh", "Bahrain" }, { "bo", "Bolivia" }, { "br", "Brazil" },
    { "ca", "Canada" }, { "ch", "Switzerland" }, { "cl", "Chile" }, { "cn", "China" }, { "co", "Colombia" },
    { "cr", "Costa Rica" }, { "cy", "Cyprus" }, { "cz", "Czech Republic" }, { "de", "Germany" }, { "dk", "Denmark" },
    { "ec", "Ecuador" }, { "es", "Spain" }, { "fi", "Finland" }, { "fr", "France" }, { "gb", "United Kingdom" },
    { "gr", "Greece" }, { "gt", "Guatemala" }, { "hk", "Hong Kong" }, { "hn", "Honduras" }, { "hr", "Croatia" },
    { "hu", "Hungary" }, { "id", "Indonesia" }, { "ie", "Ireland" }, { "il", "Israel" }, { "in", "India" },
    { "is", "Iceland" }, { "it", "Italy" }, { "jp", "Japan" }, { "kr", "South Korea" }, { "kw", "Kuwait" },
    { "lb", "Lebanon" }, { "lu", "Luxembourg" }, { "mt", "Malta" }, { "mx", "Mexico" }, { "my", "Malaysia" },
    { "ni", "Nicaragua" }, { "nl", "Netherlands" }, { "no", "Norway" }, { "nz", "New Zealand" }, { "om", "Oman" },
    { "pa", "Panama" }, { "pe", "Peru" }, { "ph", "Philippines" }, { "pl", "Poland" }, { "pt", "Portugal" },
    { "py", "Paraguay" }, { "qa", "Qatar" }, { "ro", "Romania" }, { "ru", "Russia" }, { "sa", "Saudi Arabia" },
    { "se", "Sweden" }, { "sg", "Singapore" }, { "si", "Slovenia" }, { "sk", "Slovakia" }, { "sv", "El Salvador" },
    { "th", "Thailand" }, { "tr", "Turkey" }, { "tw", "Taiwan" }, { "ua", "Ukraine" }, { "us", "United States" },
    { "uy", "Uruguay" }, { "za", "South Africa" },
};

QLineEdit *password_edit(QWidget *parent) {
    auto *edit = new QLineEdit(parent);
    edit->setEchoMode(QLineEdit::Password);
    return edit;
}

QString to_qstring(const rpcn::AccountResult &result) {
    return QString::fromStdString(result.message());
}

// Runs an account request on a worker thread and hands the result to `done` on the UI thread,
// unless `context` was destroyed in the meantime
template <typename Work, typename Done>
void run_async(QObject *context, Work work, Done done) {
    QtConcurrent::run(std::move(work)).then(context, std::move(done));
}

} // namespace

RpcnSettingsWidget::RpcnSettingsWidget(QWidget *parent)
    : QGroupBox(tr("RPCN"), parent) {
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("RPCN is the online server used by RPCS3. Signing in allows playing online, including with PS3 players. "
                                "Changes apply the next time an app starts."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_enable = new QCheckBox(tr("Enable RPCN"), this);
    layout->addWidget(m_enable);

    auto *form = new QFormLayout();
    m_host = new QLineEdit(this);
    m_host->setPlaceholderText(tr("np.rpcs3.net"));
    m_host->setToolTip(tr("Server address, optionally followed by :port (31313 by default)."));
    form->addRow(tr("Server"), m_host);

    m_npid = new QLineEdit(this);
    m_npid->setMaxLength(16);
    m_npid->setToolTip(tr("Your RPCN username (3 to 16 letters, digits, '-' or '_')."));
    form->addRow(tr("Username"), m_npid);

    m_password = password_edit(this);
    form->addRow(tr("Password"), m_password);

    m_token = new QLineEdit(this);
    m_token->setPlaceholderText(tr("Only needed if the server sent you one by email"));
    form->addRow(tr("Token"), m_token);

    m_country = new QComboBox(this);
    for (const auto &[code, name] : COUNTRIES)
        m_country->addItem(QStringLiteral("%1 (%2)").arg(tr(name), QString::fromLatin1(code)), QString::fromLatin1(code));
    m_country->setToolTip(tr("Country of your account. Games show it to other players and may use it to find matches."));
    form->addRow(tr("Country"), m_country);

    m_p2p_port = new QSpinBox(this);
    m_p2p_port->setRange(1, 65535);
    m_p2p_port->setToolTip(tr("UDP port used for connections with other players (3658 by default). "
                              "If it is in use, the next free port is taken."));
    form->addRow(tr("P2P port"), m_p2p_port);
    layout->addLayout(form);

    auto *buttons = new QHBoxLayout();
    m_test_button = new QPushButton(tr("Test Sign In"), this);
    m_create_button = new QPushButton(tr("Create Account..."), this);
    m_resend_button = new QPushButton(tr("Resend Token"), this);
    m_reset_button = new QPushButton(tr("Reset Password..."), this);
    m_friends_button = new QPushButton(tr("Friends..."), this);
    m_resend_button->setToolTip(tr("Sends the sign in token to your email address again."));
    m_friends_button->setToolTip(tr("See your friends, answer friend requests and send new ones."));
    buttons->addWidget(m_test_button);
    buttons->addWidget(m_friends_button);
    buttons->addWidget(m_create_button);
    buttons->addWidget(m_resend_button);
    buttons->addWidget(m_reset_button);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    connect(m_enable, &QCheckBox::toggled, this, [this] {
        update_enabled();
        emit changed();
    });
    for (auto *edit : { m_host, m_npid, m_password, m_token })
        connect(edit, &QLineEdit::textEdited, this, &RpcnSettingsWidget::changed);
    connect(m_country, &QComboBox::currentIndexChanged, this, &RpcnSettingsWidget::changed);
    connect(m_p2p_port, &QSpinBox::valueChanged, this, &RpcnSettingsWidget::changed);

    connect(m_test_button, &QPushButton::clicked, this, &RpcnSettingsWidget::test_login);
    connect(m_create_button, &QPushButton::clicked, this, &RpcnSettingsWidget::create_account);
    connect(m_resend_button, &QPushButton::clicked, this, &RpcnSettingsWidget::resend_token);
    connect(m_reset_button, &QPushButton::clicked, this, &RpcnSettingsWidget::reset_password);
    connect(m_friends_button, &QPushButton::clicked, this, &RpcnSettingsWidget::open_friends);

    update_enabled();
}

void RpcnSettingsWidget::load(const Config &cfg) {
    m_enable->setChecked(cfg.rpcn_enable);
    m_host->setText(QString::fromStdString(cfg.rpcn_host));
    m_npid->setText(QString::fromStdString(cfg.rpcn_npid));
    m_saved_password = cfg.rpcn_password;
    m_password->clear();
    m_password->setPlaceholderText(m_saved_password.empty() ? QString() : tr("Saved (type to change it)"));
    m_token->setText(QString::fromStdString(cfg.rpcn_token));

    QString country = QString::fromStdString(cfg.rpcn_country).toLower();
    int index = m_country->findData(country);
    if (index < 0 && !country.isEmpty()) {
        m_country->addItem(country, country);
        index = m_country->count() - 1;
    }
    m_country->setCurrentIndex(index < 0 ? m_country->findData(QStringLiteral("us")) : index);

    m_p2p_port->setValue(cfg.rpcn_p2p_port);
    update_enabled();
}

void RpcnSettingsWidget::save(Config &cfg) const {
    cfg.rpcn_enable = m_enable->isChecked();
    cfg.rpcn_host = host();
    cfg.rpcn_npid = npid();
    cfg.rpcn_password = derived_password();
    cfg.rpcn_token = m_token->text().trimmed().toStdString();
    cfg.rpcn_country = m_country->currentData().toString().toStdString();
    cfg.rpcn_p2p_port = m_p2p_port->value();
}

std::string RpcnSettingsWidget::host() const {
    return m_host->text().trimmed().toStdString();
}

std::string RpcnSettingsWidget::npid() const {
    return m_npid->text().trimmed().toStdString();
}

std::string RpcnSettingsWidget::derived_password() const {
    const QString typed = m_password->text();
    if (typed.isEmpty())
        return m_saved_password;
    if (typed != m_derived_for) {
        m_derived = rpcn::derive_password(typed.toStdString());
        m_derived_for = typed;
    }
    return m_derived;
}

void RpcnSettingsWidget::update_enabled() {
    const bool enabled = m_enable->isChecked();
    for (QWidget *w : std::initializer_list<QWidget *>{ m_host, m_npid, m_password, m_token, m_country, m_p2p_port })
        w->setEnabled(enabled);
    for (QWidget *w : std::initializer_list<QWidget *>{ m_test_button, m_create_button, m_resend_button, m_reset_button, m_friends_button })
        w->setEnabled(enabled && !m_busy);
}

void RpcnSettingsWidget::set_status(const QString &text, bool error) {
    m_status->setStyleSheet(error ? QStringLiteral("color: #d9534f;") : QString());
    m_status->setText(text);
}

void RpcnSettingsWidget::set_busy(bool busy) {
    m_busy = busy;
    update_enabled();
}

void RpcnSettingsWidget::test_login() {
    if (host().empty() || npid().empty() || derived_password().empty()) {
        set_status(tr("Enter the server, your username and your password first."), true);
        return;
    }

    const rpcn::Config config{ host(), npid(), derived_password(), m_token->text().trimmed().toStdString() };
    set_busy(true);
    set_status(tr("Signing in to %1...").arg(QString::fromStdString(config.host)), false);
    run_async(
        this, [config] { return rpcn::test_login(config); },
        [this](const rpcn::AccountResult &result) {
            set_busy(false);
            if (result.state == rpcn::State::Authenticated)
                set_status(tr("Signed in successfully."), false);
            else if (result.state == rpcn::State::FailureLoginToken)
                set_status(tr("Sign in failed: the account needs the token sent to your email address. Enter it under Token."), true);
            else
                set_status(tr("Sign in failed: %1").arg(to_qstring(result)), true);
        });
}

void RpcnSettingsWidget::create_account() {
    if (host().empty()) {
        set_status(tr("Enter the server first."), true);
        return;
    }

    RpcnCreateAccountDialog dialog(host(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_npid->setText(dialog.npid());
    m_password->setText(dialog.password());
    m_token->clear();
    emit changed();
    set_status(tr("Account %1 created. If the server sent a token to your email address, enter it under Token, then save.").arg(dialog.npid()), false);
}

void RpcnSettingsWidget::resend_token() {
    if (host().empty() || npid().empty() || derived_password().empty()) {
        set_status(tr("Enter the server, your username and your password first."), true);
        return;
    }

    const std::string server = host(), user = npid(), password = derived_password();
    set_busy(true);
    set_status(tr("Requesting a new token..."), false);
    run_async(
        this, [server, user, password] { return rpcn::resend_token(server, user, password); },
        [this](const rpcn::AccountResult &result) {
            set_busy(false);
            if (result.ok())
                set_status(tr("The token was sent to your email address."), false);
            else
                set_status(tr("Could not send the token: %1").arg(to_qstring(result)), true);
        });
}

void RpcnSettingsWidget::reset_password() {
    if (host().empty()) {
        set_status(tr("Enter the server first."), true);
        return;
    }

    RpcnResetPasswordDialog dialog(host(), m_npid->text().trimmed(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_npid->setText(dialog.npid());
    m_password->setText(dialog.password());
    emit changed();
    set_status(tr("Password changed. Save the settings to use it."), false);
}

void RpcnSettingsWidget::open_friends() {
    auto running = m_client_provider ? m_client_provider() : nullptr;
    if (running && !running->is_authenticated())
        running.reset();
    if (!running && (host().empty() || npid().empty() || derived_password().empty())) {
        set_status(tr("Enter the server, your username and your password first."), true);
        return;
    }

    RpcnFriendsDialog dialog(running, host(), npid(), derived_password(), m_token->text().trimmed().toStdString(), this);
    dialog.exec();
}

RpcnFriendsDialog::RpcnFriendsDialog(std::shared_ptr<rpcn::Client> running, const std::string &host, const std::string &npid,
    const std::string &password, const std::string &token, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("RPCN Friends"));
    auto *layout = new QVBoxLayout(this);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_tabs = new QTabWidget(this);
    const auto add_tab = [this](QListWidget *&list, const QString &title, std::initializer_list<QPushButton **> buttons, std::initializer_list<QString> labels) {
        auto *page = new QWidget(m_tabs);
        auto *page_layout = new QVBoxLayout(page);
        list = new QListWidget(page);
        page_layout->addWidget(list);
        auto *row = new QHBoxLayout();
        row->addStretch();
        auto label = labels.begin();
        for (QPushButton **button : buttons) {
            *button = new QPushButton(*label++, page);
            row->addWidget(*button);
        }
        page_layout->addLayout(row);
        m_tabs->addTab(page, title);
    };
    add_tab(m_friends, tr("Friends"), { &m_remove }, { tr("Remove Friend") });
    add_tab(m_received, tr("Requests Received"), { &m_accept, &m_decline }, { tr("Accept"), tr("Decline") });
    add_tab(m_sent, tr("Requests Sent"), { &m_cancel }, { tr("Cancel Request") });
    layout->addWidget(m_tabs);

    auto *add_row = new QHBoxLayout();
    m_add_name = new QLineEdit(this);
    m_add_name->setMaxLength(16);
    m_add_name->setPlaceholderText(tr("Username"));
    m_add = new QPushButton(tr("Send Friend Request"), this);
    add_row->addWidget(m_add_name, 1);
    add_row->addWidget(m_add);
    layout->addLayout(add_row);

    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_add, &QPushButton::clicked, this, [this] { run_command(m_add_name->text().trimmed(), true); });
    connect(m_add_name, &QLineEdit::returnPressed, m_add, &QPushButton::click);
    connect(m_remove, &QPushButton::clicked, this, [this] { run_command(selected(m_friends), false); });
    connect(m_accept, &QPushButton::clicked, this, [this] { run_command(selected(m_received), true); });
    connect(m_decline, &QPushButton::clicked, this, [this] { run_command(selected(m_received), false); });
    connect(m_cancel, &QPushButton::clicked, this, [this] { run_command(selected(m_sent), false); });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &RpcnFriendsDialog::refresh);
    resize(460, 420);

    for (QWidget *w : std::initializer_list<QWidget *>{ m_tabs, m_add_name, m_add })
        w->setEnabled(false);

    if (running) {
        connected(running, {});
        return;
    }

    // Own connection while the dialog is open
    m_own_client = true;
    m_status->setText(tr("Signing in to %1...").arg(QString::fromStdString(host)));
    const rpcn::Config config{ host, npid, password, token };
    run_async(
        this, [config]() -> std::shared_ptr<rpcn::Client> {
            auto client = std::make_shared<rpcn::Client>(config);
            if (client->connect_and_login() != rpcn::State::Authenticated)
                client->disconnect();
            return client;
        },
        [this](std::shared_ptr<rpcn::Client> client) {
            if (client->is_authenticated())
                connected(client, {});
            else
                connected(nullptr, QString::fromStdString(rpcn::to_string(client->state())));
        });
}

RpcnFriendsDialog::~RpcnFriendsDialog() {
    if (m_own_client && m_client) {
        // Signing out waits for the server, keep it off the UI thread
        QtConcurrent::run([client = std::move(m_client)] { client->disconnect(); });
    }
}

void RpcnFriendsDialog::connected(std::shared_ptr<rpcn::Client> client, const QString &error) {
    if (!client) {
        m_status->setStyleSheet(QStringLiteral("color: #d9534f;"));
        m_status->setText(tr("Could not sign in: %1").arg(error));
        return;
    }
    m_client = std::move(client);
    m_status->setText(tr("Signed in as %1.").arg(QString::fromStdString(m_client->npid())));
    for (QWidget *w : std::initializer_list<QWidget *>{ m_tabs, m_add_name, m_add })
        w->setEnabled(true);
    refresh();
    m_timer->start(500);
}

void RpcnFriendsDialog::refresh() {
    if (!m_client)
        return;
    if (!m_client->is_authenticated()) {
        m_timer->stop();
        m_status->setStyleSheet(QStringLiteral("color: #d9534f;"));
        m_status->setText(tr("Disconnected from RPCN."));
        for (QWidget *w : std::initializer_list<QWidget *>{ m_tabs, m_add_name, m_add })
            w->setEnabled(false);
        return;
    }
    const uint64_t version = m_client->friend_list_version();
    if (version == m_version)
        return;
    m_version = version;

    const auto fill = [](QListWidget *list, const std::vector<std::string> &names) {
        const QString current = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        for (const auto &name : names) {
            auto *item = new QListWidgetItem(QString::fromStdString(name), list);
            item->setData(Qt::UserRole, QString::fromStdString(name));
            if (item->data(Qt::UserRole).toString() == current)
                list->setCurrentItem(item);
        }
    };

    const rpcn::FriendList list = m_client->friend_list();
    std::vector<std::string> names;
    for (const auto &info : list.friends)
        names.push_back(info.npid);
    fill(m_friends, names);
    for (int i = 0; i < m_friends->count(); i++) {
        const auto &info = list.friends[i];
        QString text = QString::fromStdString(info.npid) + QStringLiteral(" - ") + (info.online ? tr("Online") : tr("Offline"));
        if (info.online && !info.presence.title.empty())
            text += QStringLiteral(" - ") + QString::fromStdString(info.presence.title);
        m_friends->item(i)->setText(text);
    }
    fill(m_received, list.requests_received);
    fill(m_sent, list.requests_sent);

    m_tabs->setTabText(0, tr("Friends (%1)").arg(list.friends.size()));
    m_tabs->setTabText(1, tr("Requests Received (%1)").arg(list.requests_received.size()));
    m_tabs->setTabText(2, tr("Requests Sent (%1)").arg(list.requests_sent.size()));
}

QString RpcnFriendsDialog::selected(QListWidget *list) const {
    return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
}

void RpcnFriendsDialog::run_command(const QString &npid, bool add) {
    if (!m_client)
        return;
    const std::string name = npid.toStdString();
    if (!rpcn::is_valid_npid(name)) {
        m_status->setStyleSheet(QStringLiteral("color: #d9534f;"));
        m_status->setText(npid.isEmpty() ? tr("Select or enter a username first.") : tr("%1 is not a valid username.").arg(npid));
        return;
    }
    if (add && name == m_client->npid())
        return;

    m_status->setStyleSheet(QString());
    m_status->setText(tr("Please wait..."));
    run_async(
        this, [client = m_client, name, add] { return add ? client->add_friend(name) : client->remove_friend(name); },
        [this, npid, add](std::optional<rpcn::ErrorType> error) {
            if (error == rpcn::ErrorType::NoError) {
                m_status->setStyleSheet(QString());
                m_status->setText(add ? tr("Done: %1 is your friend or has your request.").arg(npid) : tr("Done: %1 was removed.").arg(npid));
                if (add)
                    m_add_name->clear();
                m_version = ~0ull;
                refresh();
                return;
            }
            m_status->setStyleSheet(QStringLiteral("color: #d9534f;"));
            if (!error)
                m_status->setText(tr("No answer from the server."));
            else if (*error == rpcn::ErrorType::NotFound)
                m_status->setText(tr("There is no user named %1.").arg(npid));
            else if (*error == rpcn::ErrorType::AlreadyFriend)
                m_status->setText(tr("%1 is already your friend, or already has your request.").arg(npid));
            else if (*error == rpcn::ErrorType::Blocked)
                m_status->setText(tr("%1 does not accept your requests.").arg(npid));
            else
                m_status->setText(tr("The request failed: %1").arg(QString::fromStdString(rpcn::to_string(*error))));
        });
}

RpcnCreateAccountDialog::RpcnCreateAccountDialog(const std::string &host, QWidget *parent)
    : QDialog(parent)
    , m_host(host) {
    setWindowTitle(tr("Create RPCN Account"));
    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(tr("Creates an account on %1. The username is visible to other players and cannot be changed later.")
                                .arg(QString::fromStdString(host)),
        this);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *form = new QFormLayout();
    m_npid = new QLineEdit(this);
    m_npid->setMaxLength(16);
    m_npid->setPlaceholderText(tr("3 to 16 letters, digits, '-' or '_'"));
    form->addRow(tr("Username"), m_npid);
    m_password = password_edit(this);
    form->addRow(tr("Password"), m_password);
    m_confirm = password_edit(this);
    form->addRow(tr("Confirm Password"), m_confirm);
    m_email = new QLineEdit(this);
    m_email->setToolTip(tr("Used to send the sign in token and to reset the password."));
    form->addRow(tr("Email"), m_email);
    layout->addLayout(form);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_create, &QPushButton::clicked, this, &RpcnCreateAccountDialog::submit);
    resize(420, sizeHint().height());
}

QString RpcnCreateAccountDialog::npid() const {
    return m_npid->text().trimmed();
}

QString RpcnCreateAccountDialog::password() const {
    return m_password->text();
}

void RpcnCreateAccountDialog::submit() {
    const std::string user = npid().toStdString();
    const std::string email = m_email->text().trimmed().toStdString();
    if (!rpcn::is_valid_npid(user)) {
        m_status->setText(tr("The username must be 3 to 16 letters, digits, '-' or '_'."));
        return;
    }
    if (m_password->text().isEmpty()) {
        m_status->setText(tr("Enter a password."));
        return;
    }
    if (m_password->text() != m_confirm->text()) {
        m_status->setText(tr("The passwords do not match."));
        return;
    }
    if (email.find('@') == std::string::npos) {
        m_status->setText(tr("Enter a valid email address."));
        return;
    }

    m_create->setEnabled(false);
    m_status->setText(tr("Creating the account..."));
    run_async(
        this, [host = m_host, user, password = password().toStdString(), email] { return rpcn::create_account(host, user, password, email); },
        [this](const rpcn::AccountResult &result) {
            m_create->setEnabled(true);
            if (result.ok())
                accept();
            else
                m_status->setText(tr("The account was not created: %1").arg(to_qstring(result)));
        });
}

RpcnResetPasswordDialog::RpcnResetPasswordDialog(const std::string &host, const QString &npid, QWidget *parent)
    : QDialog(parent)
    , m_host(host) {
    setWindowTitle(tr("Reset RPCN Password"));
    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(tr("First request a reset token, which the server sends to the email address of the account. "
                               "Then enter it with your new password."),
        this);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *request_form = new QFormLayout();
    m_npid = new QLineEdit(npid, this);
    m_npid->setMaxLength(16);
    request_form->addRow(tr("Username"), m_npid);
    m_email = new QLineEdit(this);
    request_form->addRow(tr("Email"), m_email);
    layout->addLayout(request_form);
    m_send = new QPushButton(tr("Send Reset Token"), this);
    layout->addWidget(m_send, 0, Qt::AlignRight);

    auto *reset_form = new QFormLayout();
    m_reset_token = new QLineEdit(this);
    reset_form->addRow(tr("Reset Token"), m_reset_token);
    m_password = password_edit(this);
    reset_form->addRow(tr("New Password"), m_password);
    m_confirm = password_edit(this);
    reset_form->addRow(tr("Confirm Password"), m_confirm);
    layout->addLayout(reset_form);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_set = buttons->addButton(tr("Set Password"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_send, &QPushButton::clicked, this, &RpcnResetPasswordDialog::send_token);
    connect(m_set, &QPushButton::clicked, this, &RpcnResetPasswordDialog::submit);
    resize(420, sizeHint().height());
}

QString RpcnResetPasswordDialog::npid() const {
    return m_npid->text().trimmed();
}

QString RpcnResetPasswordDialog::password() const {
    return m_password->text();
}

void RpcnResetPasswordDialog::set_busy(bool busy) {
    m_send->setEnabled(!busy);
    m_set->setEnabled(!busy);
}

void RpcnResetPasswordDialog::send_token() {
    const std::string user = npid().toStdString();
    const std::string email = m_email->text().trimmed().toStdString();
    if (!rpcn::is_valid_npid(user) || email.empty()) {
        m_status->setText(tr("Enter the username and the email address of the account."));
        return;
    }

    set_busy(true);
    m_status->setText(tr("Requesting a reset token..."));
    run_async(
        this, [host = m_host, user, email] { return rpcn::send_reset_token(host, user, email); },
        [this](const rpcn::AccountResult &result) {
            set_busy(false);
            if (result.ok())
                m_status->setText(tr("A reset token was sent to your email address."));
            else
                m_status->setText(tr("Could not send a reset token: %1").arg(to_qstring(result)));
        });
}

void RpcnResetPasswordDialog::submit() {
    const std::string user = npid().toStdString();
    const std::string reset_token = m_reset_token->text().trimmed().toStdString();
    if (!rpcn::is_valid_npid(user) || reset_token.empty()) {
        m_status->setText(tr("Enter the username and the reset token."));
        return;
    }
    if (m_password->text().isEmpty() || m_password->text() != m_confirm->text()) {
        m_status->setText(tr("Enter the new password twice."));
        return;
    }

    set_busy(true);
    m_status->setText(tr("Changing the password..."));
    run_async(
        this, [host = m_host, user, reset_token, password = password().toStdString()] { return rpcn::reset_password(host, user, reset_token, password); },
        [this](const rpcn::AccountResult &result) {
            set_busy(false);
            if (result.ok())
                accept();
            else
                m_status->setText(tr("The password was not changed: %1").arg(to_qstring(result)));
        });
}
