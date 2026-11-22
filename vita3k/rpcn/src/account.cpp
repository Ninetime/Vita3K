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

#include <rpcn/account.h>
#include <rpcn/stream.h>

#include <algorithm>

namespace rpcn {

namespace {

// Avatar RPCS3 gives to new accounts
constexpr const char *DEFAULT_AVATAR_URL = "https://rpcs3.net/cdn/netplay/DefaultAvatar.png";

// Sends one account command on a connection of its own, and reads the error code the server answers with
AccountResult run_command(const std::string &host, CommandType command, const std::vector<uint8_t> &payload) {
    Client client(Config{ host, {}, {}, {} });

    AccountResult result;
    result.state = client.connect_only();
    if (result.state != State::Connected)
        return result;

    const auto reply = client.request(command, payload, std::chrono::seconds(20));
    if (!reply || reply->empty()) {
        result.state = State::FailureOther;
    } else {
        result.error = static_cast<ErrorType>((*reply)[0]);
    }

    client.disconnect();
    return result;
}

} // namespace

bool AccountResult::ok() const {
    return (state == State::Connected || state == State::Authenticated) && error == ErrorType::NoError;
}

std::string AccountResult::message() const {
    if (state != State::Connected && state != State::Authenticated)
        return to_string(state);

    switch (error) {
    case ErrorType::NoError: return "Success";
    case ErrorType::InvalidInput: return "The server rejected the input (check the username and email address)";
    case ErrorType::TooSoon: return "Too soon, try again later";
    case ErrorType::LoginError: return "Login failed";
    case ErrorType::LoginInvalidUsername: return "Unknown username";
    case ErrorType::LoginInvalidPassword: return "Wrong password";
    case ErrorType::LoginInvalidToken: return "Wrong token";
    case ErrorType::CreationError: return "The account could not be created";
    case ErrorType::CreationExistingUsername: return "This username is already taken";
    case ErrorType::CreationBannedEmailProvider: return "This email provider is not allowed";
    case ErrorType::CreationExistingEmail: return "This email address is already used by another account";
    case ErrorType::EmailFail: return "The server could not send the email";
    case ErrorType::DbFail: return "Server database error";
    case ErrorType::Unauthorized: return "Not authorized";
    default: return to_string(error);
    }
}

bool is_valid_npid(std::string_view npid) {
    return npid.size() >= 3 && npid.size() <= 16 && std::all_of(npid.begin(), npid.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

AccountResult test_login(const Config &config) {
    Client client(config);

    AccountResult result;
    result.state = client.connect_and_login();
    switch (result.state) {
    case State::FailureLoginAlreadyLoggedIn: result.error = ErrorType::LoginAlreadyLoggedIn; break;
    case State::FailureLoginUsername: result.error = ErrorType::LoginInvalidUsername; break;
    case State::FailureLoginPassword: result.error = ErrorType::LoginInvalidPassword; break;
    case State::FailureLoginToken: result.error = ErrorType::LoginInvalidToken; break;
    default: break;
    }

    client.disconnect();
    return result;
}

AccountResult create_account(const std::string &host, const std::string &npid, const std::string &password, const std::string &email) {
    StreamWriter req;
    // The online name starts out as the username, as in RPCS3
    req.put_string(npid).put_string(derive_password(password)).put_string(npid).put_string(DEFAULT_AVATAR_URL).put_string(email);
    return run_command(host, CommandType::Create, req.data);
}

AccountResult resend_token(const std::string &host, const std::string &npid, const std::string &derived_password) {
    StreamWriter req;
    req.put_string(npid).put_string(derived_password);
    return run_command(host, CommandType::SendToken, req.data);
}

AccountResult send_reset_token(const std::string &host, const std::string &npid, const std::string &email) {
    StreamWriter req;
    req.put_string(npid).put_string(email);
    return run_command(host, CommandType::SendResetToken, req.data);
}

AccountResult reset_password(const std::string &host, const std::string &npid, const std::string &reset_token, const std::string &new_password) {
    StreamWriter req;
    req.put_string(npid).put_string(reset_token).put_string(derive_password(new_password));
    return run_command(host, CommandType::ResetPassword, req.data);
}

} // namespace rpcn
