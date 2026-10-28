#include <Async/HttpClient.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace SFT::Async {

    namespace {

        constexpr std::size_t kMaxCookieBytes = 4096;
        constexpr std::size_t kMaxPerDomain = 50;
        constexpr std::size_t kMaxTotal = 3000;

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string trim(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
            return std::string(text);
        }

        bool is_ip_literal(const std::string &host) {
            if (host.find(':') != std::string::npos) return true; // IPv6
            return !host.empty() && std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; });
        }

        bool domain_matches(const std::string &host, const std::string &domain) {
            if (host == domain) return true;
            return host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
                   host[host.size() - domain.size() - 1] == '.' && !is_ip_literal(host);
        }

        bool path_matches(const std::string &request_path, const std::string &cookie_path) {
            if (request_path == cookie_path) return true;
            if (request_path.rfind(cookie_path, 0) != 0) return false;
            return cookie_path.back() == '/' || request_path[cookie_path.size()] == '/';
        }

        /// IMF-fixdate ("Wed, 21 Oct 2026 07:28:00 GMT") -> time point. Other formats are treated as session cookies.
        std::optional<std::chrono::system_clock::time_point> parse_expires(const std::string &text) {
            static constexpr const char *months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
            char month_name[8] = {};
            int day = 0, year = 0, hour = 0, minute = 0, second = 0;
            if (std::sscanf(text.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, month_name, &year, &hour, &minute, &second) != 6) return std::nullopt;
            int month = -1;
            const std::string name = lower(month_name);
            for (int i = 0; i < 12; ++i)
                if (name == months[i]) month = i;
            if (month < 0 || day < 1 || day > 31 || year < 1970) return std::nullopt;
            // Days from civil (Howard Hinnant's algorithm): portable, no timegm needed.
            const int y = year - (month < 2 ? 1 : 0);
            const int era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (month + (month > 1 ? -2 : 10)) + 2) / 5 + static_cast<unsigned>(day) - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            const long long days = static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
            return std::chrono::system_clock::time_point(std::chrono::seconds(days * 86400 + hour * 3600 + minute * 60 + second));
        }

        bool expired(const Cookie &cookie, std::chrono::system_clock::time_point now) { return cookie.expires && *cookie.expires <= now; }

    } // namespace

    void CookieJar::add(Cookie cookie) {
        cookie.domain = lower(cookie.domain);
        const auto now = std::chrono::system_clock::now();
        auto guard = cookies_.lock();
        auto &jar = *guard;
        std::erase_if(jar, [&](const Cookie &c) { return expired(c, now) || (c.name == cookie.name && c.domain == cookie.domain && c.path == cookie.path); });
        if (expired(cookie, now)) return; // a Set-Cookie with a past expiry is a deletion
        if (std::count_if(jar.begin(), jar.end(), [&](const Cookie &c) { return c.domain == cookie.domain; }) >= static_cast<long>(kMaxPerDomain)) {
            auto oldest = std::find_if(jar.begin(), jar.end(), [&](const Cookie &c) { return c.domain == cookie.domain; });
            jar.erase(oldest);
        }
        if (jar.size() >= kMaxTotal) jar.erase(jar.begin());
        jar.push_back(std::move(cookie));
    }

    void CookieJar::store_from_response(const std::string &host_in, const std::string &path, bool https, const std::vector<HttpHeader> &headers) {
        (void)https;
        const std::string host = lower(host_in);
        for (const HttpHeader &header : headers) {
            if (lower(header.name) != "set-cookie" || header.value.size() > kMaxCookieBytes) continue;
            Cookie cookie;
            std::size_t start = 0;
            bool first = true;
            std::optional<long long> max_age;
            std::string domain_attribute;
            std::string path_attribute;
            bool valid = true;
            while (start <= header.value.size()) {
                auto semicolon = header.value.find(';', start);
                if (semicolon == std::string::npos) semicolon = header.value.size();
                const std::string part = trim(std::string_view(header.value).substr(start, semicolon - start));
                start = semicolon + 1;
                if (first) {
                    first = false;
                    const auto equals = part.find('=');
                    if (equals == std::string::npos || equals == 0) {
                        valid = false;
                        break;
                    }
                    cookie.name = trim(std::string_view(part).substr(0, equals));
                    cookie.value = trim(std::string_view(part).substr(equals + 1));
                    continue;
                }
                const auto equals = part.find('=');
                const std::string key = lower(trim(std::string_view(part).substr(0, equals)));
                const std::string value = equals == std::string::npos ? std::string{} : trim(std::string_view(part).substr(equals + 1));
                if (key == "domain") domain_attribute = lower(value);
                else if (key == "path") path_attribute = value;
                else if (key == "secure") cookie.secure = true;
                else if (key == "httponly") cookie.http_only = true;
                else if (key == "max-age") {
                    char *end = nullptr;
                    const long long seconds = std::strtoll(value.c_str(), &end, 10);
                    if (end != value.c_str()) max_age = seconds;
                } else if (key == "expires") cookie.expires = parse_expires(value);
            }
            if (!valid) continue;
            if (max_age) cookie.expires = std::chrono::system_clock::now() + std::chrono::seconds(*max_age); // Max-Age wins over Expires

            if (!domain_attribute.empty()) {
                if (domain_attribute.front() == '.') domain_attribute.erase(0, 1);
                // The server may only widen a cookie to a parent domain of the host it answered as, never to a bare TLD.
                if (is_ip_literal(host) || domain_attribute.find('.') == std::string::npos || !domain_matches(host, domain_attribute)) continue;
                cookie.domain = domain_attribute;
                cookie.host_only = false;
            } else {
                cookie.domain = host;
                cookie.host_only = true;
            }
            if (!path_attribute.empty() && path_attribute.front() == '/') {
                cookie.path = path_attribute;
            } else { // default-path: the directory of the request path
                std::string directory = path.substr(0, path.find('?'));
                directory = directory.empty() || directory.front() != '/' ? "/" : directory.substr(0, directory.rfind('/'));
                cookie.path = directory.empty() ? "/" : directory;
            }
            add(std::move(cookie));
        }
    }

    std::string CookieJar::header_for(const std::string &host_in, const std::string &path_in, bool https) const {
        const std::string host = lower(host_in);
        const std::string path = path_in.substr(0, path_in.find('?'));
        const auto now = std::chrono::system_clock::now();
        std::vector<Cookie> matching;
        {
            auto guard = cookies_.lock();
            for (const Cookie &cookie : *guard) {
                if (expired(cookie, now)) continue;
                if (cookie.secure && !https) continue;
                if (cookie.host_only ? host != cookie.domain : !domain_matches(host, cookie.domain)) continue;
                if (!path_matches(path.empty() ? "/" : path, cookie.path)) continue;
                matching.push_back(cookie);
            }
        }
        // Longer paths first, as RFC 6265 asks.
        std::stable_sort(matching.begin(), matching.end(), [](const Cookie &a, const Cookie &b) { return a.path.size() > b.path.size(); });
        std::string out;
        for (const Cookie &cookie : matching) {
            if (!out.empty()) out += "; ";
            out += cookie.name + "=" + cookie.value;
        }
        return out;
    }

    std::vector<Cookie> CookieJar::all() const {
        auto guard = cookies_.lock();
        return *guard;
    }

    void CookieJar::clear() { cookies_.lock()->clear(); }

} // namespace SFT::Async
