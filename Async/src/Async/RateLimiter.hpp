#pragma once

#include <Async/Http.hpp>
#include <Async/Mutex.hpp>
#include <Async/Router.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace SFT::Async {

    /// A keyed token-bucket rate limiter: each key (a client address, an API token, a user) may use `rate_per_second`
    /// operations per second on average, with bursts up to `burst`. Thread-safe; idle keys are forgotten so memory is bounded
    /// by `max_keys` even under a flood of distinct keys (the oldest are evicted first).
    class RateLimiter {
      public:
        using Clock = std::chrono::steady_clock;

        RateLimiter(double rate_per_second, double burst, std::size_t max_keys = 10'000)
            : rate_(rate_per_second > 0 ? rate_per_second : 1.0), burst_((std::max)(burst, 1.0)), max_keys_((std::max)(max_keys, std::size_t{1})) {}

        /// Takes `cost` tokens for `key` if available.
        [[nodiscard]] bool try_acquire(const std::string &key, double cost = 1.0) {
            const auto now = Clock::now();
            auto guard = buckets_.lock();
            if (guard->size() >= max_keys_ && !guard->contains(key)) evict(*guard, now);
            Bucket &bucket = refill(*guard, key, now);
            if (bucket.tokens < cost) return false;
            bucket.tokens -= cost;
            return true;
        }

        /// How long until `key` could next take `cost` tokens (zero if it could now).
        [[nodiscard]] std::chrono::milliseconds retry_after(const std::string &key, double cost = 1.0) {
            const auto now = Clock::now();
            auto guard = buckets_.lock();
            Bucket &bucket = refill(*guard, key, now);
            if (bucket.tokens >= cost) return std::chrono::milliseconds::zero();
            return std::chrono::milliseconds(static_cast<long long>(std::ceil((cost - bucket.tokens) / rate_ * 1000.0)));
        }

        [[nodiscard]] std::size_t tracked_keys() const { return buckets_.lock()->size(); }

      private:
        struct Bucket {
            double tokens;
            Clock::time_point updated;
        };
        using Map = std::unordered_map<std::string, Bucket>;

        Bucket &refill(Map &map, const std::string &key, Clock::time_point now) {
            auto [it, inserted] = map.try_emplace(key, Bucket{burst_, now});
            Bucket &bucket = it->second;
            if (!inserted) {
                const double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
                bucket.tokens = (std::min)(burst_, bucket.tokens + elapsed * rate_);
                bucket.updated = now;
            }
            return bucket;
        }

        /// Drops buckets that have refilled completely (no information lost); if everything is active, the stalest half.
        void evict(Map &map, Clock::time_point now) {
            for (auto it = map.begin(); it != map.end();) {
                const double refilled = it->second.tokens + std::chrono::duration<double>(now - it->second.updated).count() * rate_;
                it = refilled >= burst_ ? map.erase(it) : std::next(it);
            }
            if (map.size() < max_keys_) return;
            std::vector<std::pair<Clock::time_point, std::string>> by_age;
            for (const auto &[key, bucket] : map) by_age.emplace_back(bucket.updated, key);
            std::sort(by_age.begin(), by_age.end());
            for (std::size_t i = 0; i < by_age.size() / 2; ++i) map.erase(by_age[i].second);
        }

        double rate_;
        double burst_;
        std::size_t max_keys_;
        mutable Mutex<Map> buckets_;
    };

    /// Router middleware that answers 429 Too Many Requests (with Retry-After) once a client exceeds `limiter`. The key
    /// defaults to the client address; pass `key_of` to limit by API key, user, route, ....
    [[nodiscard]] inline Router::Middleware rate_limit_middleware(std::shared_ptr<RateLimiter> limiter,
                                                                  std::function<std::string(const RouteRequest &)> key_of = {}) {
        return [limiter = std::move(limiter), key_of = std::move(key_of)](RouteRequest &request) -> std::optional<HttpResponseMessage> {
            const std::string key = key_of ? key_of(request) : request.http.remote_address;
            if (limiter->try_acquire(key)) return std::nullopt;
            const auto wait = limiter->retry_after(key);
            HttpResponseMessage response = text_response(429, "too many requests");
            response.headers.push_back({"Retry-After", std::to_string((wait.count() + 999) / 1000)});
            return response;
        };
    }

} // namespace SFT::Async
