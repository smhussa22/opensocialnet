#ifndef SIGNALING_SERVER_JWT_VERIFIER_HH
#define SIGNALING_SERVER_JWT_VERIFIER_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <string>

// 3rd party headers

// project headers


namespace OpenSocialNet::Signaling
{

    class JwksCache;


    // Outcome of verifying a Google ID token. On success, `sub` is the
    // user's permanent Google account id (we use it as our user_id key in
    // Scylla), `email` is their verified email, `name` is the display
    // name from their profile. On failure, `error` is a short reason.
    struct JwtVerifyResult
    {

        bool        ok    { false }; // overall verdict — only trust the other fields when true
        std::string sub   { };       // Google user id (= our user_id)
        std::string email { };       // verified email if the email scope was granted
        std::string name { };        // display name from the profile scope
        std::string error { };       // human-readable rejection reason on failure

    };


    // Stateless helper around jwt-cpp. Pulls the matching JWKS key by `kid`
    // header, verifies the RS256 signature, then checks the standard claims
    // (iss = "https://accounts.google.com" or "accounts.google.com", aud
    // = our client_id, exp > now). The expected audience is the OAuth
    // client_id we registered with Google — passed in so the verifier
    // doesn't have to know about gateway state.
    JwtVerifyResult verify_google_id_token(JwksCache& jwks, const std::string& jwt, const std::string& expected_audience);

}

#endif // SIGNALING_SERVER_JWT_VERIFIER_HH
