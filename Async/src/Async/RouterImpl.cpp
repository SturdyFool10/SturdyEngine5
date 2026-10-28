#include <Async/Router.hpp>

#include <Async/Promise.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <set>
#include <system_error>

namespace SFT::Async {

    namespace fs = std::filesystem;

    // =============================================================================================
    // Small helpers
    // =============================================================================================
    namespace {

        std::string lower(std::string_view text) {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        int hex_value(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        std::string trim(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
            return std::string(text);
        }

        std::vector<std::string_view> split(std::string_view text, char separator) {
            std::vector<std::string_view> out;
            std::size_t start = 0;
            while (start <= text.size()) {
                auto end = text.find(separator, start);
                if (end == std::string_view::npos) end = text.size();
                out.push_back(text.substr(start, end - start));
                start = end + 1;
            }
            return out;
        }

        std::string http_date(std::chrono::system_clock::time_point time) {
            const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
            std::tm parts{};
#if defined(_WIN32)
            gmtime_s(&parts, &seconds);
#else
            gmtime_r(&seconds, &parts);
#endif
            char buffer[64];
            std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &parts);
            return buffer;
        }

        /// Stable file-clock -> system-clock mapping (the offset is sampled once, so a file's Last-Modified never flaps).
        std::chrono::system_clock::time_point to_system(fs::file_time_type time) {
            static const auto offset = std::chrono::system_clock::now().time_since_epoch() -
                                       std::chrono::duration_cast<std::chrono::system_clock::duration>(fs::file_time_type::clock::now().time_since_epoch());
            return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(time.time_since_epoch()) + offset);
        }

        std::optional<std::chrono::system_clock::time_point> parse_http_date(const std::string &text) {
            std::tm parts{};
            // IMF-fixdate only (the one form servers must generate); anything else is ignored.
            const char *end = nullptr;
#if defined(_WIN32)
            (void)end;
            return std::nullopt;
#else
            end = strptime(text.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &parts);
            if (end == nullptr) return std::nullopt;
            return std::chrono::system_clock::from_time_t(timegm(&parts));
#endif
        }

        const std::map<std::string, std::string> &builtin_mime_types() {
            static const std::map<std::string, std::string> types = {
                {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
                {".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"}, {".json", "application/json"},
                {".map", "application/json"}, {".wasm", "application/wasm"}, {".txt", "text/plain; charset=utf-8"},
                {".md", "text/markdown; charset=utf-8"}, {".csv", "text/csv; charset=utf-8"}, {".xml", "application/xml"},
                {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
                {".webp", "image/webp"}, {".avif", "image/avif"}, {".ico", "image/x-icon"}, {".bmp", "image/bmp"}, {".ktx2", "image/ktx2"},
                {".pdf", "application/pdf"}, {".zip", "application/zip"}, {".gz", "application/gzip"},
                {".mp3", "audio/mpeg"}, {".ogg", "audio/ogg"}, {".wav", "audio/wav"}, {".flac", "audio/flac"}, {".opus", "audio/opus"},
                {".mp4", "video/mp4"}, {".webm", "video/webm"},
                {".ttf", "font/ttf"}, {".otf", "font/otf"}, {".woff", "font/woff"}, {".woff2", "font/woff2"},
                {".glb", "model/gltf-binary"}, {".gltf", "model/gltf+json"}, {".bin", "application/octet-stream"},
            };
            return types;
        }

    } // namespace

    std::optional<std::string> percent_decode(std::string_view text, bool plus_is_space) {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '%') {
                if (i + 2 >= text.size()) return std::nullopt; // both hex digits must exist
                const int high = hex_value(text[i + 1]);
                const int low = hex_value(text[i + 2]);
                if (high < 0 || low < 0) return std::nullopt;
                const char decoded = static_cast<char>(high * 16 + low);
                if (decoded == '\0') return std::nullopt;
                out.push_back(decoded);
                i += 2;
            } else if (c == '+' && plus_is_space) {
                out.push_back(' ');
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    std::optional<std::string> RouteRequest::param(std::string_view name) const {
        for (const auto &[key, value] : params)
            if (key == name) return value;
        return std::nullopt;
    }

    std::optional<std::string> RouteRequest::query_param(std::string_view name) const {
        for (std::string_view pair : split(query, '&')) {
            const auto equals = pair.find('=');
            const auto key = percent_decode(pair.substr(0, equals), true);
            if (key && *key == name) {
                return equals == std::string_view::npos ? std::string{} : percent_decode(pair.substr(equals + 1), true).value_or(std::string{});
            }
        }
        return std::nullopt;
    }

    HttpResponseMessage text_response(unsigned status, std::string body, std::string content_type) {
        HttpResponseMessage response;
        response.status = status;
        response.headers.push_back({"Content-Type", std::move(content_type)});
        response.headers.push_back({"X-Content-Type-Options", "nosniff"});
        response.body = std::move(body);
        return response;
    }
    HttpResponseMessage html_response(unsigned status, std::string body) { return text_response(status, std::move(body), "text/html; charset=utf-8"); }
    HttpResponseMessage json_response(unsigned status, std::string body) { return text_response(status, std::move(body), "application/json"); }
    HttpResponseMessage redirect_response(std::string location, unsigned status) {
        // A Location with a line break would split the response; refuse rather than emit it.
        if (location.find_first_of("\r\n") != std::string::npos) return text_response(500, "invalid redirect target");
        HttpResponseMessage response = text_response(status, "");
        response.headers.push_back({"Location", std::move(location)});
        return response;
    }

    // =============================================================================================
    // Router
    // =============================================================================================
    struct Router::Impl {
        struct Segment {
            bool parameter = false;
            std::string text;
        };
        struct Route {
            std::string method;
            std::vector<Segment> segments;
            std::string wildcard; // non-empty: trailing *name capture
            Router::Handler sync;
            Router::AsyncHandler async;
            int specificity = 0;
        };
        struct StaticMount {
            std::string mount;
            StaticFilesOptions options;
            fs::path canonical_root;
        };

        std::vector<Route> routes;
        std::vector<Router::Middleware> middleware;
        std::vector<StaticMount> statics;
        Router::Handler not_found_handler;

        static std::vector<std::string> split_path(const std::string &path) {
            std::vector<std::string> parts;
            for (std::string_view part : split(path, '/')) {
                if (!part.empty()) parts.emplace_back(part);
            }
            return parts;
        }

        void add(std::string method, const std::string &pattern, Router::Handler sync, Router::AsyncHandler async) {
            Route route;
            std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            route.method = std::move(method);
            route.sync = std::move(sync);
            route.async = std::move(async);
            auto parts = split_path(pattern);
            for (std::size_t i = 0; i < parts.size(); ++i) {
                const std::string &part = parts[i];
                if (!part.empty() && part.front() == '*') {
                    if (i + 1 != parts.size()) throw std::invalid_argument("a '*wildcard' must be the last segment of a route");
                    route.wildcard = part.substr(1).empty() ? "path" : part.substr(1);
                } else if (!part.empty() && part.front() == ':') {
                    route.segments.push_back({true, part.substr(1)});
                    route.specificity += 2;
                } else {
                    route.segments.push_back({false, part});
                    route.specificity += 4;
                }
            }
            if (route.wildcard.empty()) route.specificity += 1;
            routes.push_back(std::move(route));
        }

        /// Matches decoded `segments`; fills `params` on success.
        static bool match(const Route &route, const std::vector<std::string> &segments, std::vector<std::pair<std::string, std::string>> &params) {
            if (route.wildcard.empty() ? segments.size() != route.segments.size() : segments.size() < route.segments.size()) return false;
            params.clear();
            for (std::size_t i = 0; i < route.segments.size(); ++i) {
                if (route.segments[i].parameter) params.emplace_back(route.segments[i].text, segments[i]);
                else if (route.segments[i].text != segments[i]) return false;
            }
            if (!route.wildcard.empty()) {
                std::string rest;
                for (std::size_t i = route.segments.size(); i < segments.size(); ++i) {
                    if (!rest.empty()) rest += '/';
                    rest += segments[i];
                }
                params.emplace_back(route.wildcard, std::move(rest));
            }
            return true;
        }

        HttpResponseMessage serve_static(const StaticMount &mount, const RouteRequest &request, const std::string &relative) const;
        TaskHandle<HttpResponseMessage> run(HttpRequestMessage request) const;
    };

    Router::Router() : impl_(std::make_shared<Impl>()) {}

    Router &Router::route(std::string method, std::string pattern, Handler handler) {
        impl_->add(std::move(method), pattern, std::move(handler), nullptr);
        return *this;
    }
    Router &Router::route_async(std::string method, std::string pattern, AsyncHandler handler) {
        impl_->add(std::move(method), pattern, nullptr, std::move(handler));
        return *this;
    }
    Router &Router::use(Middleware middleware) {
        impl_->middleware.push_back(std::move(middleware));
        return *this;
    }
    Router &Router::not_found(Handler handler) {
        impl_->not_found_handler = std::move(handler);
        return *this;
    }
    Router &Router::static_files(std::string mount, StaticFilesOptions options) {
        std::error_code error;
        fs::path root = fs::canonical(options.root, error);
        if (error || !fs::is_directory(root, error)) {
            throw std::invalid_argument("static_files: root '" + options.root + "' is not an existing directory");
        }
        while (mount.size() > 1 && mount.back() == '/') mount.pop_back();
        if (mount == "/") mount.clear();
        impl_->statics.push_back({std::move(mount), std::move(options), std::move(root)});
        return *this;
    }

    // ---- static files ---------------------------------------------------------------------------
    namespace {

        std::string content_type_for(const fs::path &path, const std::map<std::string, std::string> &overrides) {
            const std::string extension = lower(path.extension().string());
            if (auto it = overrides.find(extension); it != overrides.end()) return it->second;
            const auto &builtin = builtin_mime_types();
            if (auto it = builtin.find(extension); it != builtin.end()) return it->second;
            return "application/octet-stream";
        }

        bool etag_matches(std::string_view header, const std::string &etag) {
            if (trim(header) == "*") return true;
            const auto strip_weak = [](std::string value) { return value.rfind("W/", 0) == 0 ? value.substr(2) : value; };
            const std::string ours = strip_weak(etag);
            for (std::string_view item : split(header, ',')) {
                if (strip_weak(trim(item)) == ours) return true;
            }
            return false;
        }

        struct ByteRange {
            std::uint64_t first;
            std::uint64_t last; // inclusive
        };
        enum class RangeParse { None, Ok, Unsatisfiable, Ignore };

        RangeParse parse_range(const std::string &header, std::uint64_t size, ByteRange &out) {
            if (header.rfind("bytes=", 0) != 0) return RangeParse::Ignore;
            const auto spec = header.substr(6);
            if (spec.find(',') != std::string::npos) return RangeParse::Ignore; // multipart ranges: serve the whole file
            const auto dash = spec.find('-');
            if (dash == std::string::npos) return RangeParse::Ignore;
            const std::string left = trim(spec.substr(0, dash));
            const std::string right = trim(spec.substr(dash + 1));
            auto number = [](const std::string &text, std::uint64_t &value) {
                if (text.empty() || text.size() > 19) return false;
                return std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{};
            };
            std::uint64_t a = 0, b = 0;
            if (left.empty()) { // suffix: last N bytes
                if (!number(right, b) || b == 0) return RangeParse::Unsatisfiable;
                if (size == 0) return RangeParse::Unsatisfiable;
                out = {b >= size ? 0 : size - b, size - 1};
                return RangeParse::Ok;
            }
            if (!number(left, a)) return RangeParse::Ignore;
            if (a >= size) return RangeParse::Unsatisfiable;
            if (right.empty()) {
                out = {a, size - 1};
            } else {
                if (!number(right, b) || b < a) return RangeParse::Ignore;
                out = {a, (std::min)(b, size - 1)};
            }
            return RangeParse::Ok;
        }

    } // namespace

    HttpResponseMessage Router::Impl::serve_static(const StaticMount &mount, const RouteRequest &request, const std::string &relative) const {
        const StaticFilesOptions &options = mount.options;
        if (request.http.method != "GET" && request.http.method != "HEAD") {
            HttpResponseMessage response = text_response(405, "method not allowed");
            response.headers.push_back({"Allow", "GET, HEAD"});
            return response;
        }

        // 1. The decoded path may only name files below the root: no "..", no backslashes, no drive letters.
        std::vector<std::string> segments;
        for (std::string_view part : split(relative, '/')) {
            if (part.empty() || part == ".") continue;
            if (part == ".." || part.find('\\') != std::string_view::npos || part.find(':') != std::string_view::npos) return text_response(404, "not found");
            if (!options.allow_dotfiles && part.front() == '.') return text_response(404, "not found");
            segments.emplace_back(part);
        }
        fs::path candidate = mount.canonical_root;
        for (const auto &segment : segments) candidate /= segment;

        std::error_code error;
        auto resolve = [&](fs::path path) -> std::optional<fs::path> {
            // 2. Resolve symlinks, then re-verify containment: a link must not lead out of the root.
            fs::path canonical = fs::weakly_canonical(path, error);
            if (error) return std::nullopt;
            if (!options.follow_symlinks) {
                const fs::path relative_path = canonical.lexically_relative(mount.canonical_root);
                if (relative_path.empty() || *relative_path.begin() == "..") return std::nullopt;
            }
            return canonical;
        };

        auto target = resolve(candidate);
        if (target && fs::is_directory(*target, error)) {
            target = options.index_file.empty() ? std::nullopt : resolve(*target / options.index_file);
        }
        if (!target || !fs::is_regular_file(*target, error)) {
            if (!options.spa_fallback.empty() && segments.empty() == false && lower(fs::path(segments.back()).extension().string()).empty()) {
                target = resolve(mount.canonical_root / options.spa_fallback);
            } else {
                target.reset();
            }
            if (!target || !fs::is_regular_file(*target, error)) return text_response(404, "not found");
        }

        // 3. Pick a precompressed sibling when the client can use it.
        fs::path file = *target;
        std::string content_encoding;
        if (options.serve_precompressed) {
            const std::string accepted = lower(request.header("accept-encoding").value_or(""));
            for (const auto &[token, suffix] : {std::pair{"br", ".br"}, std::pair{"gzip", ".gz"}}) {
                if (accepted.find(token) == std::string::npos) continue;
                fs::path sibling = file;
                sibling += suffix;
                if (fs::is_regular_file(sibling, error)) {
                    if (auto checked = resolve(sibling)) {
                        file = *checked;
                        content_encoding = token;
                        break;
                    }
                }
            }
        }

        const std::uint64_t size = fs::file_size(file, error);
        if (error) return text_response(404, "not found");
        const auto modified = fs::last_write_time(file, error);
        if (error) return text_response(404, "not found");
        char etag_buffer[80];
        std::snprintf(etag_buffer, sizeof(etag_buffer), "W/\"%llx-%llx\"", static_cast<unsigned long long>(size),
                      static_cast<unsigned long long>(modified.time_since_epoch().count()));
        const std::string etag = etag_buffer;
        const auto modified_sys = to_system(modified);
        const std::string last_modified = http_date(modified_sys);

        HttpResponseMessage response;
        response.headers.push_back({"Content-Type", content_type_for(*target, options.mime_types)});
        response.headers.push_back({"ETag", etag});
        response.headers.push_back({"Last-Modified", last_modified});
        response.headers.push_back({"Accept-Ranges", "bytes"});
        response.headers.push_back({"X-Content-Type-Options", "nosniff"});
        if (!options.cache_control.empty()) response.headers.push_back({"Cache-Control", options.cache_control});
        if (!content_encoding.empty()) {
            response.headers.push_back({"Content-Encoding", content_encoding});
            response.headers.push_back({"Vary", "Accept-Encoding"});
        } else if (options.serve_precompressed) {
            response.headers.push_back({"Vary", "Accept-Encoding"});
        }

        // 4. Conditional requests.
        if (auto none_match = request.header("if-none-match")) {
            if (etag_matches(*none_match, etag)) {
                response.status = 304;
                return response;
            }
        } else if (auto since = request.header("if-modified-since")) {
            // HTTP dates have one-second resolution, so compare at that resolution.
            if (auto when = parse_http_date(std::string(*since));
                when && std::chrono::floor<std::chrono::seconds>(modified_sys) <= std::chrono::floor<std::chrono::seconds>(*when)) {
                response.status = 304;
                return response;
            }
        }

        // 5. Ranges (single range; If-Range must match, else the whole file is sent).
        response.body_file = file.string();
        response.status = 200;
        response.body_length = size;
        bool honour_range = true;
        if (auto if_range = request.header("if-range")) {
            honour_range = trim(*if_range) == etag || trim(*if_range) == last_modified;
        }
        if (honour_range) {
            if (auto range_header = request.header("range")) {
                ByteRange range{};
                switch (parse_range(std::string(*range_header), size, range)) {
                    case RangeParse::Ok:
                        response.status = 206;
                        response.body_offset = range.first;
                        response.body_length = range.last - range.first + 1;
                        response.headers.push_back({"Content-Range", "bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) + "/" + std::to_string(size)});
                        break;
                    case RangeParse::Unsatisfiable: {
                        HttpResponseMessage bad = text_response(416, "range not satisfiable");
                        bad.headers.push_back({"Content-Range", "bytes */" + std::to_string(size)});
                        return bad;
                    }
                    default:
                        break;
                }
            }
        }
        return response;
    }

    // ---- dispatch -------------------------------------------------------------------------------
    TaskHandle<HttpResponseMessage> Router::Impl::run(HttpRequestMessage http) const {
        const auto finish = [](HttpResponseMessage response) { return ready(std::move(response)); };

        RouteRequest request;
        request.http = std::move(http);
        const std::string &target = request.http.target;
        const auto question = target.find('?');
        const std::string raw_path = target.substr(0, question);
        if (question != std::string::npos) request.query = target.substr(question + 1);
        if (raw_path.empty() || raw_path.front() != '/') return finish(text_response(400, "bad request target"));

        std::vector<std::string> segments;
        for (std::string_view part : split(raw_path, '/')) {
            if (part.empty()) continue;
            auto decoded = percent_decode(part);
            if (!decoded || decoded->find('/') != std::string::npos) return finish(text_response(400, "bad percent-encoding in path"));
            segments.push_back(std::move(*decoded));
        }
        request.path = "/";
        for (std::size_t i = 0; i < segments.size(); ++i) request.path += (i ? "/" : "") + segments[i];

        const std::string& method = request.http.method;
        const bool head = method == "HEAD";

        // Best route for this method; remember the methods other routes offer for a 405.
        const Route *best = nullptr;
        std::vector<std::pair<std::string, std::string>> best_params, scratch;
        std::set<std::string> allowed;
        for (const Route &route : routes) {
            if (!match(route, segments, scratch)) continue;
            allowed.insert(route.method);
            const bool applies = route.method == method || (head && route.method == "GET");
            if (applies && (best == nullptr || route.specificity > best->specificity || (route.specificity == best->specificity && route.method == method && best->method != method))) {
                best = &route;
                best_params = scratch;
            }
        }

        auto apply_middleware = [&]() -> std::optional<HttpResponseMessage> {
            for (const auto &step : middleware) {
                if (auto stop = step(request)) return stop;
            }
            return std::nullopt;
        };

        // For HEAD answered by a GET route: same headers and Content-Length, no body.
        auto strip_body_for_head = [head](HttpResponseMessage response) {
            if (head && response.body_file.empty()) {
                response.headers.push_back({"Content-Length", std::to_string(response.body.size())});
                response.body.clear();
            }
            return response;
        };

        if (best != nullptr) {
            request.params = std::move(best_params);
            if (auto stop = apply_middleware()) return finish(std::move(*stop));
            if (best->async) {
                auto pending = best->async(request);
                if (!head) return pending;
                return then(std::move(pending), [strip_body_for_head](HttpResponseMessage response) { return strip_body_for_head(std::move(response)); });
            }
            return finish(strip_body_for_head(best->sync(request)));
        }

        for (const StaticMount &mount : statics) {
            const bool under = mount.mount.empty() || request.path == mount.mount || request.path.rfind(mount.mount + "/", 0) == 0;
            if (!under) continue;
            if (auto stop = apply_middleware()) return finish(std::move(*stop));
            return finish(serve_static(mount, request, request.path.substr(mount.mount.size())));
        }

        if (!allowed.empty()) {
            allowed.insert("OPTIONS");
            if (method == "OPTIONS") {
                HttpResponseMessage response = text_response(204, "");
                std::string list;
                for (const auto &m : allowed) list += (list.empty() ? "" : ", ") + m;
                response.headers.push_back({"Allow", list});
                return finish(std::move(response));
            }
            HttpResponseMessage response = text_response(405, "method not allowed");
            std::string list;
            for (const auto &m : allowed) list += (list.empty() ? "" : ", ") + m;
            response.headers.push_back({"Allow", list});
            return finish(std::move(response));
        }
        if (not_found_handler) return finish(not_found_handler(request));
        return finish(text_response(404, "not found"));
    }

    HttpServer::AsyncHandler Router::handler() const {
        auto snapshot = std::make_shared<const Impl>(*impl_);
        return [snapshot](HttpRequestMessage request) { return snapshot->run(std::move(request)); };
    }

    HttpResponseMessage Router::dispatch(HttpRequestMessage request) const { return impl_->run(std::move(request)).wait(); }

} // namespace SFT::Async
