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

// RPCN account management, as offered by RPCS3's RPCN settings. Each call opens its own connection to the
// server and blocks until the server answers (up to about 20 seconds), so run them off the UI thread.

#include <rpcn/client.h>

#include <string>

namespace rpcn {

struct AccountResult {
    State state = State::Disconnected; // Connection outcome
    ErrorType error = ErrorType::NoError; // Server answer, when the connection worked

    bool ok() const;
    std::string message() const; // Human readable outcome
};

// Online IDs are 3 to 16 characters among letters, digits, '-' and '_'
bool is_valid_npid(std::string_view npid);

// Signs in with the given settings and signs out again
AccountResult test_login(const Config &config);

// The password is the one the user typed; the server receives it derived (see derive_password()).
// If the server validates emails, the token needed to sign in is sent to the email address.
AccountResult create_account(const std::string &host, const std::string &npid, const std::string &password, const std::string &email);

// Sends the sign in token to the account's email address again
AccountResult resend_token(const std::string &host, const std::string &npid, const std::string &derived_password);

// Emails a token that allows setting a new password with reset_password()
AccountResult send_reset_token(const std::string &host, const std::string &npid, const std::string &email);
AccountResult reset_password(const std::string &host, const std::string &npid, const std::string &reset_token, const std::string &new_password);

} // namespace rpcn
