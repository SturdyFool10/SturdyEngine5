#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Async/Http.hpp>
#include <Async/HttpServer.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {

    /// A request after routing: decoded path, route parameters and query access.
    struct RouteRequest {
        HttpRequestMessage http;
        std::string path;   ///< percent-decoded path without the query ("/users/42")
        std::string query;  ///< raw query string without '?'
        std::vector<std::pair<std::string, std::string>> params; ///< `:name` / `*name` captures, decoded

        [[nodiscard]] std::optional<std::string> param(std::string_view name) const;
        /// First `name=value` pair of the query, percent- and '+'-decoded.
        [[nodiscard]] std::optional<std::string> query_param(std::string_view name) const;
        [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const { return header_value(http.headers, name); }
    };

    [[nodiscard]] HttpResponseMessage text_response(unsigned status, std::string body, std::string content_type = "text/plain; charset=utf-8");
    [[nodiscard]] HttpResponseMessage html_response(unsigned status, std::string body);
    /// `body` must already be valid JSON text.
    [[nodiscard]] HttpResponseMessage json_response(unsigned status, std::string body);
    [[nodiscard]] HttpResponseMessage redirect_response(std::string location, unsigned status = 302);

    /// Percent-decodes `text`; fails (nullopt) on malformed escapes or an encoded NUL. `plus_is_space` is for
    /// query strings. Exposed for handlers that parse their own inputs.
    [[nodiscard]] std::optional<std::string> percent_decode(std::string_view text, bool plus_is_space = false);

    struct StaticFilesOptions {
        /// Directory served. Files outside it are unreachable, including through symlinks (see `follow_symlinks`).
        std::string root;
        /// Served for a request that names a directory.
        std::string index_file = "index.html";
        /// Sent as Cache-Control on every file response.
        std::string cache_control = "public, max-age=300";
        /// Files and directories whose name starts with '.' are hidden (404) unless set.
        bool allow_dotfiles = false;
        /// Allow symlinks that point outside `root`. Off by default: a link planted in the tree must not expose
        /// the rest of the disk.
        bool follow_symlinks = false;
        /// Serve `name.br` / `name.gz` siblings (with Content-Encoding and Vary) when the client accepts them.
        bool serve_precompressed = true;
        /// Served (with status 200) when nothing matches a GET -- single-page-app routing. Empty: 404.
        std::string spa_fallback;
        /// Extra or overriding extension -> MIME mappings (".glb" -> "model/gltf-binary").
        std::map<std::string, std::string> mime_types;
    };

    /// URL router. Patterns are '/'-separated: literals, `:name` (one segment) and a final `*name` (the rest).
    /// Literal segments beat parameters, which beat wildcards. Build it fully, then serve `handler()`; the
    /// returned handler holds an immutable snapshot, so later edits to the Router do not affect a running server.
    class Router {
      public:
        using Handler = std::function<HttpResponseMessage(const RouteRequest &)>;
        using AsyncHandler = std::function<TaskHandle<HttpResponseMessage>(const RouteRequest &)>;
        /// Runs before the handler of every matching route; return a response to stop processing (auth, rate limits).
        using Middleware = std::function<std::optional<HttpResponseMessage>(RouteRequest &)>;

        Router();

        Router &route(std::string method, std::string pattern, Handler handler);
        Router &route_async(std::string method, std::string pattern, AsyncHandler handler);
        Router &get(std::string pattern, Handler handler) { return route("GET", std::move(pattern), std::move(handler)); }
        Router &post(std::string pattern, Handler handler) { return route("POST", std::move(pattern), std::move(handler)); }
        Router &put(std::string pattern, Handler handler) { return route("PUT", std::move(pattern), std::move(handler)); }
        Router &patch(std::string pattern, Handler handler) { return route("PATCH", std::move(pattern), std::move(handler)); }
        Router &del(std::string pattern, Handler handler) { return route("DELETE", std::move(pattern), std::move(handler)); }
        Router &use(Middleware middleware);
        /// Serves files under `mount` (e.g. "/assets") from `options.root`, GET and HEAD only.
        Router &static_files(std::string mount, StaticFilesOptions options);
        /// Replaces the default 404 body/behaviour.
        Router &not_found(Handler handler);

        /// A handler to give to `HttpServer`. HEAD requests reuse GET routes; unmatched methods on known paths
        /// get 405 with an Allow header; malformed paths get 400.
        [[nodiscard]] HttpServer::AsyncHandler handler() const;
        /// Same, for synchronous use and tests.
        [[nodiscard]] HttpResponseMessage dispatch(HttpRequestMessage request) const;

      private:
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };

} // namespace SFT::Async
