#ifndef SIGNALING_SERVER_JWKS_CACHE_HH
#define SIGNALING_SERVER_JWKS_CACHE_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// 3rd party headers

// project headers


namespace OpenSocialNet::Signaling
{

    // Per-kid public key entry. We store the key already converted to a
    // PEM-encoded RSA public key so jwt-cpp can consume it directly via
    // jwt::algorithm::rs256 — saves us redoing the (n, e) -> PEM dance on
    // every verification.
    struct JwksKey
    {

        std::string kid     { }; // matches the `kid` header on incoming JWTs
        std::string pem     { }; // PEM-encoded RSA public key for jwt-cpp

    };


    // Tiny in-memory cache of Google's JWKS (RSA public keys used to sign
    // ID tokens). Pulls https://www.googleapis.com/oauth2/v3/certs lazily,
    // caches the parsed keys for `ttl` (defaults to 1h — Google rotates
    // every few weeks but we don't want to risk stale-cache outages if
    // they rotate hot), and re-fetches on cache miss or TTL expiry.
    //
    // Thread-safe: a single mutex guards the cache and the in-flight
    // refresh state. Most calls hit the fast path (lookup under shared
    // mutex), so contention is one atomic load on the steady-state path.
    class JwksCache
    {

    public:

        explicit JwksCache(std::string jwks_url = "https://www.googleapis.com/oauth2/v3/certs",
                           std::chrono::seconds ttl = std::chrono::hours { 1 }) noexcept;

        JwksCache(const JwksCache&)            = delete;
        JwksCache& operator=(const JwksCache&) = delete;
        JwksCache(JwksCache&&)                 = delete;
        JwksCache& operator=(JwksCache&&)      = delete;


        // Return the PEM-encoded RSA public key for `kid`, refreshing the
        // cache from Google if `kid` is unknown or the cache has expired.
        // None on network failure or unknown kid (caller should treat as
        // "JWT not verifiable, deny auth").
        std::optional<std::string> key_pem_for_kid(const std::string& kid);


        // Force-refresh (mostly useful for tests and manual rotation).
        // Returns true on success; on failure the cache is left untouched.
        bool refresh_now();


    private:

        // Fetch + parse the JWKS endpoint, replace the in-memory map.
        // Caller MUST hold m_mu.
        bool refresh_locked();

        std::string                                  m_jwks_url   { }; // typically Google's well-known endpoint
        std::chrono::seconds                         m_ttl        { }; // how long a fetch stays fresh
        std::chrono::steady_clock::time_point        m_fetched_at { }; // 0 (epoch) means "never fetched"

        mutable std::mutex                           m_mu         { };
        std::unordered_map<std::string, JwksKey>     m_keys       { }; // kid -> JwksKey

    };

}

#endif // SIGNALING_SERVER_JWKS_CACHE_HH
