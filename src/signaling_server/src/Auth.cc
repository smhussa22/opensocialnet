// related headers
#include "Auth.hh"

// c sys headers

// cpp stdlib headers
#include <string>

// 3rd party headers
#include <openssl/hmac.h>
#include <openssl/crypto.h>

// project headers


namespace OpenSocialNet::Signaling
{

    std::string compute_auth_token(const std::string& secret, const std::string& user_id)
    {

        unsigned char mac[EVP_MAX_MD_SIZE] { };
        unsigned int mac_len { 0 };
        const auto* out = ::HMAC(::EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(user_id.data()), user_id.size(), mac, &mac_len);
        if (!out) return std::string { };

        std::string hex { };
        hex.resize(static_cast<std::size_t>(mac_len) * 2);
        static constexpr char digits[] { "0123456789abcdef" };
        for (unsigned int i { 0 }; i < mac_len; ++i)
        {

            hex[i * 2] = digits[(mac[i] >> 4) & 0xF];
            hex[i * 2 + 1] = digits[mac[i] & 0xF];

        }
        return hex;

    }

    bool verify_hello_auth(const std::string& secret, const std::string& user_id, const std::string& provided_token)
    {

        // Constant-time compare so a network attacker can't time-leak which
        // prefix of the expected token they got right.
        const std::string expected { compute_auth_token(secret, user_id) };
        return !expected.empty() && expected.size() == provided_token.size() && ::CRYPTO_memcmp(expected.data(), provided_token.data(), expected.size()) == 0;

    }


    bool looks_like_jwt(const std::string& token) noexcept
    {

        // JWTs are exactly two dots between three non-empty base64url
        // segments. HMAC tokens are 64 lowercase hex chars with no dots.
        // We don't validate the segment contents here; the verifier will
        // reject malformed payloads — this only routes the request.
        if (token.empty()) return false;

        int dots { 0 };
        for (char c : token) if (c == '.') ++dots;
        return dots == 2;

    }

}
