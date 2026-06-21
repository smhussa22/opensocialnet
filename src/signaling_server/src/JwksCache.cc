// related headers
#include "JwksCache.hh"

// c sys headers

// cpp stdlib headers
#include <iostream>
#include <utility>

// 3rd party headers
#include <httplib.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <picojson/picojson.h>

// project headers


namespace
{

    // base64url decoder. Google's JWKS encodes RSA modulus/exponent as
    // base64url-without-padding, but OpenSSL's BIO_b64 only does standard
    // base64. Cheap to roll: 64-byte table lookup + pad-then-decode.
    std::string base64url_decode(std::string_view in)
    {

        std::string s { in };
        for (auto& c : s)
        {

            if      (c == '-') c = '+';
            else if (c == '_') c = '/';

        }
        while (s.size() % 4 != 0) s.push_back('=');

        // Use a memory BIO chain: base64 -> mem.
        BIO* b64 { BIO_new(BIO_f_base64()) };
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO* mem { BIO_new_mem_buf(s.data(), static_cast<int>(s.size())) };
        BIO* chain { BIO_push(b64, mem) };

        std::string out(s.size(), '\0');
        const int n { BIO_read(chain, out.data(), static_cast<int>(out.size())) };
        BIO_free_all(chain);

        if (n <= 0) return { };
        out.resize(static_cast<std::size_t>(n));
        return out;

    }


    // Convert a JWKS RSA key (modulus_b64url, exponent_b64url) into a
    // PEM-encoded SubjectPublicKeyInfo string suitable for jwt-cpp's rs256
    // verifier. Uses the OpenSSL 3 EVP_PKEY_fromdata path so the legacy
    // RSA_* API (deprecated since 3.0) isn't dragged in. Returns an empty
    // string on any OpenSSL failure — caller treats that as "skip this
    // key entry".
    std::string rsa_components_to_pem(std::string_view n_b64url, std::string_view e_b64url)
    {

        const std::string n_bytes { base64url_decode(n_b64url) };
        const std::string e_bytes { base64url_decode(e_b64url) };
        if (n_bytes.empty() or e_bytes.empty()) return { };

        // Step 1: BIGNUMs from the raw modulus/exponent bytes.
        ::BIGNUM* bn_n { ::BN_bin2bn(reinterpret_cast<const unsigned char*>(n_bytes.data()), static_cast<int>(n_bytes.size()), nullptr) };
        ::BIGNUM* bn_e { ::BN_bin2bn(reinterpret_cast<const unsigned char*>(e_bytes.data()), static_cast<int>(e_bytes.size()), nullptr) };
        if (bn_n == nullptr or bn_e == nullptr)
        {

            if (bn_n != nullptr) ::BN_free(bn_n);
            if (bn_e != nullptr) ::BN_free(bn_e);
            return { };

        }

        // Step 2: build an OSSL_PARAM array describing the RSA public key.
        // OSSL_PARAM_BLD owns the references to the BIGNUMs while we use it,
        // but the final OSSL_PARAM array is independent — we free both.
        ::OSSL_PARAM_BLD* bld { ::OSSL_PARAM_BLD_new() };
        if (bld == nullptr)
        {

            ::BN_free(bn_n);
            ::BN_free(bn_e);
            return { };

        }
        if (::OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn_n) == 0 or
            ::OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bn_e) == 0)
        {

            ::OSSL_PARAM_BLD_free(bld);
            ::BN_free(bn_n);
            ::BN_free(bn_e);
            return { };

        }

        ::OSSL_PARAM* params { ::OSSL_PARAM_BLD_to_param(bld) };
        ::OSSL_PARAM_BLD_free(bld);
        ::BN_free(bn_n);
        ::BN_free(bn_e);
        if (params == nullptr) return { };

        // Step 3: feed the params into EVP_PKEY_fromdata to materialize
        // an EVP_PKEY holding the public key.
        ::EVP_PKEY_CTX* ctx { ::EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr) };
        if (ctx == nullptr or ::EVP_PKEY_fromdata_init(ctx) <= 0)
        {

            if (ctx != nullptr) ::EVP_PKEY_CTX_free(ctx);
            ::OSSL_PARAM_free(params);
            return { };

        }
        ::EVP_PKEY* pkey { nullptr };
        const int rc { ::EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) };
        ::EVP_PKEY_CTX_free(ctx);
        ::OSSL_PARAM_free(params);
        if (rc <= 0 or pkey == nullptr) return { };

        // Step 4: serialize to PEM via BIO. PEM_write_bio_PUBKEY writes
        // the SubjectPublicKeyInfo form ("-----BEGIN PUBLIC KEY-----"),
        // which is what jwt-cpp's algorithm::rs256 expects.
        ::BIO* bio { ::BIO_new(::BIO_s_mem()) };
        if (bio == nullptr or ::PEM_write_bio_PUBKEY(bio, pkey) == 0)
        {

            if (bio != nullptr) ::BIO_free(bio);
            ::EVP_PKEY_free(pkey);
            return { };

        }

        char* data { nullptr };
        const long len { ::BIO_get_mem_data(bio, &data) };
        std::string pem(data, static_cast<std::size_t>(len));

        ::BIO_free(bio);
        ::EVP_PKEY_free(pkey);
        return pem;

    }


    // Split "https://host/path" into ("host", "path") for httplib's
    // SSLClient which wants the scheme/host separately from the path. We
    // only ever talk to a single endpoint so this can stay dumb.
    std::pair<std::string, std::string> split_url(const std::string& url)
    {

        constexpr std::string_view scheme { "https://" };
        if (url.starts_with(scheme))
        {

            const std::string after_scheme { url.substr(scheme.size()) };
            const auto slash { after_scheme.find('/') };
            if (slash == std::string::npos) return { after_scheme, "/" };
            return { after_scheme.substr(0, slash), after_scheme.substr(slash) };

        }
        return { url, "/" };

    }

}


namespace OpenSocialNet::Signaling
{

    JwksCache::JwksCache(std::string jwks_url, std::chrono::seconds ttl) noexcept
        : m_jwks_url { std::move(jwks_url) }, m_ttl { ttl }
    {

    }


    std::optional<std::string> JwksCache::key_pem_for_kid(const std::string& kid)
    {

        std::scoped_lock<std::mutex> lock { m_mu };

        // Fast path: cache is fresh AND we have the kid.
        const auto now { std::chrono::steady_clock::now() };
        const bool fresh { m_fetched_at != std::chrono::steady_clock::time_point { } and (now - m_fetched_at) < m_ttl };
        if (fresh)
        {

            const auto it { m_keys.find(kid) };
            if (it != m_keys.end()) return it->second.pem;

        }

        // Either expired or unknown kid: refresh and try once more.
        if (!refresh_locked()) return std::nullopt;
        const auto it { m_keys.find(kid) };
        if (it == m_keys.end()) return std::nullopt;
        return it->second.pem;

    }


    bool JwksCache::refresh_now()
    {

        std::scoped_lock<std::mutex> lock { m_mu };
        return refresh_locked();

    }


    bool JwksCache::refresh_locked()
    {

        const auto [host, path] { split_url(m_jwks_url) };

        ::httplib::SSLClient client { host };
        client.set_connection_timeout(5);
        client.set_read_timeout(5);
        // Google's certs chain through a public CA — enable cert verify so
        // an attacker on path can't substitute their own JWKS.
        client.enable_server_certificate_verification(true);

        const auto res { client.Get(path.c_str()) };
        if (!res or res->status != 200)
        {

            std::cerr << "[jwks] fetch " << m_jwks_url << " failed (status=" << (res ? res->status : -1) << ")\n";
            return false;

        }

        ::picojson::value root { };
        const std::string parse_err { ::picojson::parse(root, res->body) };
        if (!parse_err.empty() or !root.is<::picojson::object>())
        {

            std::cerr << "[jwks] parse failed: " << parse_err << '\n';
            return false;

        }

        const auto& obj { root.get<::picojson::object>() };
        const auto keys_it { obj.find("keys") };
        if (keys_it == obj.end() or !keys_it->second.is<::picojson::array>())
        {

            std::cerr << "[jwks] response missing 'keys' array\n";
            return false;

        }

        std::unordered_map<std::string, JwksKey> next { };
        for (const auto& entry : keys_it->second.get<::picojson::array>())
        {

            if (!entry.is<::picojson::object>()) continue;
            const auto& key_obj { entry.get<::picojson::object>() };

            const auto kty_it { key_obj.find("kty") };
            const auto kid_it { key_obj.find("kid") };
            const auto n_it   { key_obj.find("n")   };
            const auto e_it   { key_obj.find("e")   };
            if (kty_it == key_obj.end() or kid_it == key_obj.end() or n_it == key_obj.end() or e_it == key_obj.end()) continue;

            // We only handle RSA keys today; Google ID tokens are RS256
            // exclusively, so skipping non-RSA entries is correct.
            if (!kty_it->second.is<std::string>() or kty_it->second.get<std::string>() != "RSA") continue;

            const std::string kid { kid_it->second.get<std::string>() };
            const std::string n   { n_it->second.get<std::string>()   };
            const std::string e   { e_it->second.get<std::string>()   };

            std::string pem { rsa_components_to_pem(n, e) };
            if (pem.empty()) continue;
            next.emplace(kid, JwksKey { kid, std::move(pem) });

        }

        if (next.empty())
        {

            std::cerr << "[jwks] no usable RSA keys in response\n";
            return false;

        }

        m_keys       = std::move(next);
        m_fetched_at = std::chrono::steady_clock::now();
        std::cerr << "[jwks] refreshed " << m_keys.size() << " key(s) from " << m_jwks_url << '\n';
        return true;

    }

}
