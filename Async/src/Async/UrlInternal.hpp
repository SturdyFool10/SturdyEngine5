#pragma once

// Private: strict parsing of the absolute URLs the client side accepts (http, https, ws, wss).

#include <Async/IoError.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <string>

namespace SFT::Async::Detail {

    struct ParsedUrl {
        std::string scheme;
        bool secure = false;
        std::string host;           ///< without brackets
        std::uint16_t port = 0;
        std::string target = "/";   ///< path + query, always starts with '/'
        std::string host_header;    ///< host[:port] as it belongs in a Host header
    };

    [[nodiscard]] inline bool has_line_break(const std::string &text) { return text.find_first_of("\r\n") != std::string::npos; }

    /// Accepts `scheme://host[:port][/path][?query]` for the given schemes. Rejects user-info, control characters,
    /// spaces and fragments' effects on the target (a fragment is dropped, never sent).
    [[nodiscard]] inline std::expected<ParsedUrl, IoError> parse_url(const std::string &url, std::initializer_list<const char *> schemes) {
        ParsedUrl parsed;
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos) {
            return std::unexpected(IoError{IoErrorCode::InvalidArgument, "URL needs a scheme://"});
        }
        parsed.scheme = url.substr(0, scheme_end);
        std::transform(parsed.scheme.begin(), parsed.scheme.end(), parsed.scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::none_of(schemes.begin(), schemes.end(), [&](const char *s) { return parsed.scheme == s; })) {
            return std::unexpected(IoError{IoErrorCode::InvalidArgument, "unsupported URL scheme '" + parsed.scheme + "'"});
        }
        parsed.secure = parsed.scheme == "https" || parsed.scheme == "wss";
        if (std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7F; })) {
            return std::unexpected(IoError{IoErrorCode::InvalidArgument, "URL contains spaces or control characters"});
        }
        std::string rest = url.substr(scheme_end + 3);
        if (const auto fragment = rest.find('#'); fragment != std::string::npos) rest.erase(fragment);
        const auto slash = rest.find_first_of("/?");
        const std::string authority = rest.substr(0, slash);
        if (slash != std::string::npos) {
            parsed.target = rest.substr(slash);
            if (parsed.target.front() == '?') parsed.target.insert(parsed.target.begin(), '/');
        }
        if (authority.empty() || authority.find('@') != std::string::npos) {
            return std::unexpected(IoError{IoErrorCode::InvalidArgument, "URL needs a host and must not carry user-info"});
        }
        unsigned long port = parsed.secure ? 443 : 80;
        std::string host = authority;
        if (authority.front() == '[') {
            const auto close = authority.find(']');
            if (close == std::string::npos) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "malformed IPv6 host in URL"});
            host = authority.substr(1, close - 1);
            if (close + 1 < authority.size()) {
                if (authority[close + 1] != ':') return std::unexpected(IoError{IoErrorCode::InvalidArgument, "malformed URL authority"});
                const std::string digits = authority.substr(close + 2);
                if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    return std::unexpected(IoError{IoErrorCode::InvalidArgument, "malformed port in URL"});
                }
                port = std::strtoul(digits.c_str(), nullptr, 10);
            }
        } else if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
            host = authority.substr(0, colon);
            const std::string digits = authority.substr(colon + 1);
            if (digits.empty() || digits.size() > 5 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
                return std::unexpected(IoError{IoErrorCode::InvalidArgument, "malformed port in URL"});
            }
            port = std::strtoul(digits.c_str(), nullptr, 10);
        }
        if (host.empty() || port == 0 || port > 65535) {
            return std::unexpected(IoError{IoErrorCode::InvalidArgument, "URL needs a host and a valid port"});
        }
        parsed.host = host;
        parsed.port = static_cast<std::uint16_t>(port);
        const bool default_port = port == (parsed.secure ? 443u : 80u);
        parsed.host_header = (authority.front() == '[' ? "[" + host + "]" : host) + (default_port ? "" : ":" + std::to_string(port));
        return parsed;
    }

} // namespace SFT::Async::Detail
