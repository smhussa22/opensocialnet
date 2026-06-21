#ifndef NATIVE_CLIENT_OAUTH_CLIENT_HH
#define NATIVE_CLIENT_OAUTH_CLIENT_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <string>

// 3rd party headers

// project headers


namespace OpenSocialNet::NativeClient
{

    // What we got back from Google after a successful sign-in. id_token is
    // the JWT we ship to the gateway; sub / email / name are extracted
    // from its payload for the local UI so we can avoid a second decode
    // in main(). On failure, `ok` is false and `error` carries a short
    // human-readable reason.
    struct OAuthResult
    {

        bool        ok       { false }; // overall success
        std::string id_token { };       // RS256 JWT — passed as auth_token in the Hello envelope
        std::string sub      { };       // Google user id, stable across email changes
        std::string email    { };       // verified email if scope granted
        std::string name     { };       // display name from the profile scope
        std::string error    { };       // rejection reason on failure

    };


    // One-shot Google OAuth 2.0 desktop loopback flow (RFC 8252):
    //
    //   1. Generate a PKCE verifier (random 32 bytes -> base64url) and
    //      the matching SHA256 challenge.
    //   2. Spin up an httplib::Server on 127.0.0.1:<random ephemeral
    //      port> with a single GET /callback handler.
    //   3. Open the user's default browser at the Google authorization
    //      endpoint (the redirect_uri is the loopback URL we just bound).
    //   4. The browser redirects back to /callback with ?code=... ; the
    //      handler captures it, replies with a friendly HTML page, then
    //      stops the server.
    //   5. POST the code + verifier to https://oauth2.googleapis.com/token
    //      and parse out id_token. Decode its payload (no signature
    //      check here — the gateway does that) to extract sub/email/name.
    //
    // Blocks until the user authorizes, the browser fails to launch, or
    // the timeout (`auth_timeout_seconds`) elapses.
    OAuthResult google_login(const std::string& client_id, int auth_timeout_seconds = 120);

}

#endif // NATIVE_CLIENT_OAUTH_CLIENT_HH
