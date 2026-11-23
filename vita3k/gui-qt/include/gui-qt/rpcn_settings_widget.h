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

#pragma once

#include <QDialog>
#include <QGroupBox>

#include <functional>
#include <memory>
#include <string>

namespace rpcn {
class Client;
}

struct Config;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QSpinBox;
class QTabWidget;
class QTimer;

// RPCN (online play server shared with RPCS3) account settings, shown in the Network settings.
// Stores the same keys as config.yml: rpcn-enable, rpcn-host, rpcn-npid, rpcn-password (derived), rpcn-token,
// rpcn-p2p-port and rpcn-country.
class RpcnSettingsWidget : public QGroupBox {
    Q_OBJECT

public:
    explicit RpcnSettingsWidget(QWidget *parent = nullptr);

    void load(const Config &cfg);
    void save(Config &cfg) const;

    // The RPCN connection of the running app, if any (the server allows one sign in per account)
    using ClientProvider = std::function<std::shared_ptr<rpcn::Client>()>;
    void set_client_provider(ClientProvider provider) { m_client_provider = std::move(provider); }

signals:
    void changed();

private:
    std::string host() const;
    std::string npid() const;
    // The password to store: derived from the typed one, or the saved one when nothing was typed
    std::string derived_password() const;
    void update_enabled();
    void set_status(const QString &text, bool error);
    void set_busy(bool busy);

    void test_login();
    void create_account();
    void resend_token();
    void reset_password();
    void open_friends();

    QCheckBox *m_enable;
    QLineEdit *m_host;
    QLineEdit *m_npid;
    QLineEdit *m_password;
    QLineEdit *m_token;
    QComboBox *m_country;
    QSpinBox *m_p2p_port;
    QPushButton *m_test_button;
    QPushButton *m_create_button;
    QPushButton *m_resend_button;
    QPushButton *m_reset_button;
    QPushButton *m_friends_button;
    QLabel *m_status;

    std::string m_saved_password;
    // Derivation of the last typed password, so it is only computed once
    mutable QString m_derived_for;
    mutable std::string m_derived;
    bool m_busy = false;
    ClientProvider m_client_provider;
};

// Friend list management: friends with their status, friend requests received and sent, sending requests
class RpcnFriendsDialog : public QDialog {
    Q_OBJECT

public:
    // Uses `running` when it is signed in, else signs in with `config` while the dialog is open
    RpcnFriendsDialog(std::shared_ptr<rpcn::Client> running, const std::string &host, const std::string &npid,
        const std::string &password, const std::string &token, QWidget *parent = nullptr);
    ~RpcnFriendsDialog() override;

private:
    void connected(std::shared_ptr<rpcn::Client> client, const QString &error);
    void refresh();
    void run_command(const QString &npid, bool add);
    QString selected(QListWidget *list) const;

    std::shared_ptr<rpcn::Client> m_client;
    bool m_own_client = false;
    uint64_t m_version = ~0ull;
    QLabel *m_status;
    QTabWidget *m_tabs;
    QListWidget *m_friends;
    QListWidget *m_received;
    QListWidget *m_sent;
    QLineEdit *m_add_name;
    QPushButton *m_add;
    QPushButton *m_remove;
    QPushButton *m_accept;
    QPushButton *m_decline;
    QPushButton *m_cancel;
    QTimer *m_timer;
};

// Creates an RPCN account on the given server
class RpcnCreateAccountDialog : public QDialog {
    Q_OBJECT

public:
    RpcnCreateAccountDialog(const std::string &host, QWidget *parent = nullptr);

    QString npid() const;
    QString password() const;

private:
    void submit();

    std::string m_host;
    QLineEdit *m_npid;
    QLineEdit *m_password;
    QLineEdit *m_confirm;
    QLineEdit *m_email;
    QPushButton *m_create;
    QLabel *m_status;
};

// Sends a password reset token by email, then sets a new password with it
class RpcnResetPasswordDialog : public QDialog {
    Q_OBJECT

public:
    RpcnResetPasswordDialog(const std::string &host, const QString &npid, QWidget *parent = nullptr);

    QString npid() const;
    QString password() const;

private:
    void send_token();
    void submit();
    void set_busy(bool busy);

    std::string m_host;
    QLineEdit *m_npid;
    QLineEdit *m_email;
    QPushButton *m_send;
    QLineEdit *m_reset_token;
    QLineEdit *m_password;
    QLineEdit *m_confirm;
    QPushButton *m_set;
    QLabel *m_status;
};
