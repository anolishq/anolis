#pragma once

#include <string>
#include <string_view>

namespace anolis::http {

/// True if @p remote_addr is a loopback address (127.x / ::1).
bool is_loopback(const std::string &remote_addr);

/// Length- and content-constant string comparison (avoids leaking the token via
/// early-exit timing). Returns false for differing lengths.
bool constant_time_equals(std::string_view a, std::string_view b);

/// Decide whether an incoming request is authorized.
/// - @p auth_enabled false             -> always authorized.
/// - @p exempt_loopback && loopback    -> authorized (localhost dev stays open).
/// - otherwise the @p authorization_header must equal "Bearer <expected_token>".
/// An empty @p expected_token never authorizes a non-loopback request.
bool authorize(bool auth_enabled, bool exempt_loopback, const std::string &expected_token,
               const std::string &remote_addr, const std::string &authorization_header);

}  // namespace anolis::http
