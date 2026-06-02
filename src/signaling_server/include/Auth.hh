#ifndef SIGNALING_SERVER_AUTH_HH
#define SIGNALING_SERVER_AUTH_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <string>

// 3rd party headers

// project headers


namespace OpenSocialNet::Signaling
{

    // HMAC-SHA256(secret, user_id) as lowercase hex. Output is always 64
    // chars; on OpenSSL failure we return an empty string so callers can
    // distinguish that from a valid digest.
    std::string compute_auth_token(const std::string& secret, const std::string& user_id);

    // Constant-time compare of `provided_token` against the digest derived
    // from (secret, user_id). Returns false if the secret is empty so a
    // misconfigured server denies everyone instead of accepting anything.
    bool verify_hello_auth(const std::string& secret, const std::string& user_id, const std::string& provided_token);

}

#endif // SIGNALING_SERVER_AUTH_HH
