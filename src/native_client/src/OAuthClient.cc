// related headers
#include "OAuthClient.hh"

// c sys headers
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// cpp stdlib headers
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

// 3rd party headers
#include <httplib.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

// project headers


namespace
{

    // base64url *encoder*, no padding. Same alphabet as the JWKS path
    // but the inverse direction — used for both the PKCE verifier
    // (random bytes -> verifier string) and the challenge (SHA256 ->
    // challenge string).
    std::string base64url_encode(const unsigned char* data, std::size_t len)
    {

        static const char alphabet[] { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" };
        std::string out { };
        out.reserve(((len + 2) / 3) * 4);

        std::size_t i { 0 };
        while (i + 3 <= len)
        {

            const std::uint32_t n { static_cast<std::uint32_t>(data[i] << 16 | data[i + 1] << 8 | data[i + 2]) };
            out.push_back(alphabet[(n >> 18) & 0x3F]);
            out.push_back(alphabet[(n >> 12) & 0x3F]);
            out.push_back(alphabet[(n >> 6) & 0x3F]);
            out.push_back(alphabet[ n        & 0x3F]);
            i += 3;

        }
        if (i < len)
        {

            const std::uint32_t n { static_cast<std::uint32_t>(data[i] << 16 | (i + 1 < len ? data[i + 1] << 8 : 0)) };
            out.push_back(alphabet[(n >> 18) & 0x3F]);
            out.push_back(alphabet[(n >> 12) & 0x3F]);
            if (i + 1 < len) out.push_back(alphabet[(n >> 6) & 0x3F]);

        }
        return out;

    }


    // base64url *decoder*. Used to crack open the JWT payload locally so
    // the UI can pre-fill the user's name without waiting for the
    // gateway's Ready (the gateway is the one that VERIFIES the JWT;
    // here we're just reading the unauthenticated payload for display).
    std::string base64url_decode(std::string_view in)
    {

        std::array<std::int8_t, 256> tbl { };
        tbl.fill(-1);
        static constexpr char alphabet[] { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" };
        for (int i { 0 }; i < 64; ++i) tbl[static_cast<unsigned char>(alphabet[i])] = static_cast<std::int8_t>(i);

        std::string out { };
        out.reserve(in.size() * 3 / 4);

        std::uint32_t buf { 0 };
        int bits { 0 };
        for (char c : in)
        {

            if (c == '=') break;
            const int v { tbl[static_cast<unsigned char>(c)] };
            if (v < 0) continue;
            buf = (buf << 6) | static_cast<std::uint32_t>(v);
            bits += 6;
            if (bits >= 8)
            {

                bits -= 8;
                out.push_back(static_cast<char>((buf >> bits) & 0xFF));

            }

        }
        return out;

    }


    std::string sha256_b64url(const std::string& input)
    {

        unsigned char digest[SHA256_DIGEST_LENGTH] { };
        ::EVP_MD_CTX* ctx { ::EVP_MD_CTX_new() };
        if (ctx == nullptr) return { };
        unsigned int out_len { 0 };
        if (::EVP_DigestInit_ex(ctx, ::EVP_sha256(), nullptr) <= 0 or
            ::EVP_DigestUpdate(ctx, input.data(), input.size()) <= 0 or
            ::EVP_DigestFinal_ex(ctx, digest, &out_len) <= 0)
        {

            ::EVP_MD_CTX_free(ctx);
            return { };

        }
        ::EVP_MD_CTX_free(ctx);
        return base64url_encode(digest, out_len);

    }


    std::string random_pkce_verifier()
    {

        unsigned char raw[32] { };
        if (::RAND_bytes(raw, sizeof raw) != 1) return { };
        return base64url_encode(raw, sizeof raw);

    }


    // Best-effort browser launch. Tries the conventional opener per OS.
    // Returns whether the call succeeded enough to print the URL — we
    // also print the URL to stderr unconditionally so the user can paste
    // it manually if no opener is available (headless container, ssh,...).
    void open_browser(const std::string& url)
    {

#if defined(__APPLE__)
        const std::string cmd { "open '" + url + "' >/dev/null 2>&1 &" };
#elif defined(_WIN32)
        const std::string cmd { "start \"\" \"" + url + "\"" };
#else
        const std::string cmd { "xdg-open '" + url + "' >/dev/null 2>&1 &" };
#endif
        std::cerr << "[oauth] open this URL in a browser if it doesn't open automatically:\n   " << url << '\n';
        (void)std::system(cmd.c_str());

    }


    // URL-encode a single value going into a query string. Just enough
    // for the params we ship (alphanumerics + '.', '_', '-', '~' pass
    // unchanged; anything else gets %HH).
    std::string url_encode(std::string_view in)
    {

        static const char hex[] { "0123456789ABCDEF" };
        std::string out { };
        out.reserve(in.size());
        for (unsigned char c : in)
        {

            if ((c >= 'A' and c <= 'Z') or (c >= 'a' and c <= 'z') or (c >= '0' and c <= '9') or c == '-' or c == '.' or c == '_' or c == '~')
            {

                out.push_back(static_cast<char>(c));

            }
            else
            {

                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);

            }

        }
        return out;

    }


    // Pull a top-level string field out of a JSON object the dumb way
    // (substring scan for "\"key\":\"..."). Avoids dragging a JSON dep
    // into native_client purely for two fields in the token response.
    std::string extract_json_string(const std::string& body, const std::string& key)
    {

        const std::string needle { "\"" + key + "\"" };
        const auto k { body.find(needle) };
        if (k == std::string::npos) return { };
        const auto colon { body.find(':', k + needle.size()) };
        if (colon == std::string::npos) return { };
        const auto quote_open { body.find('"', colon) };
        if (quote_open == std::string::npos) return { };
        const auto quote_close { body.find('"', quote_open + 1) };
        if (quote_close == std::string::npos) return { };
        return body.substr(quote_open + 1, quote_close - quote_open - 1);

    }

}


namespace OpenSocialNet::NativeClient
{

    OAuthResult google_login(const std::string& client_id, int auth_timeout_seconds)
    {

        OAuthResult result { };

        if (client_id.empty())
        {

            result.error = "empty client_id";
            return result;

        }

        // PKCE setup — verifier stays in this process, challenge goes
        // into the authorization URL. The token endpoint matches them
        // when we exchange the code.
        const std::string verifier { random_pkce_verifier() };
        if (verifier.empty())
        {

            result.error = "PKCE verifier RNG failed";
            return result;

        }
        const std::string challenge { sha256_b64url(verifier) };

        // Anti-CSRF state token — opaque to Google, we verify it comes
        // back unchanged in the redirect.
        const std::string state { random_pkce_verifier() };

        // Start the loopback server. Port 0 lets the kernel pick a free
        // ephemeral port; we read it back via bind_to_any_port().
        ::httplib::Server srv { };
        std::atomic<bool> have_response { false };
        std::string received_code  { };
        std::string received_state { };
        std::string received_error { };

        srv.Get("/callback", [&](const ::httplib::Request& req, ::httplib::Response& res)
        {

            if (req.has_param("error"))
            {

                received_error = req.get_param_value("error");

            }
            else
            {

                received_code  = req.get_param_value("code");
                received_state = req.get_param_value("state");

            }

            res.set_content(
                "<html><body style=\"font-family: sans-serif; text-align: center; padding: 4em;\">"
                "<h2>You can close this window.</h2>"
                "<p>OpenSocialNet has received your sign-in.</p>"
                "</body></html>",
                "text/html");

            have_response = true;

        });

        // Bind to 0.0.0.0 rather than 127.0.0.1 so WSL2's localhost
        // forwarding works: ports bound to WSL's own 127.0.0.1 aren't
        // reachable from Windows-side browsers, but ports on 0.0.0.0
        // are. The redirect_uri we hand Google is still 127.0.0.1 —
        // only the bind interface changes. Same behaviour on real
        // Linux / macOS / Windows where 0.0.0.0 == "accept on all".
        const int port { srv.bind_to_any_port("0.0.0.0") };
        if (port < 0)
        {

            result.error = "could not bind loopback HTTP server";
            return result;

        }
        const std::string redirect_uri { "http://127.0.0.1:" + std::to_string(port) + "/callback" };

        // Run the server on a worker thread so this function can
        // synchronously wait for have_response.
        std::thread server_thread { [&] { srv.listen_after_bind(); } };

        // Authorization URL. `access_type=offline` + `prompt=consent`
        // would also issue a refresh token; we deliberately skip that
        // for v1 so we're not storing long-lived secrets. Re-auth every
        // launch is acceptable until we have OS keystore integration.
        std::ostringstream auth_url { };
        auth_url << "https://accounts.google.com/o/oauth2/v2/auth"
                 << "?response_type=code"
                 << "&client_id="             << url_encode(client_id)
                 << "&redirect_uri="          << url_encode(redirect_uri)
                 << "&scope="                 << url_encode("openid email profile")
                 << "&state="                 << url_encode(state)
                 << "&code_challenge="        << url_encode(challenge)
                 << "&code_challenge_method=" << "S256";

        open_browser(auth_url.str());

        // Wait up to auth_timeout_seconds for the callback to fire.
        const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { auth_timeout_seconds } };
        while (!have_response.load(std::memory_order_acquire) and std::chrono::steady_clock::now() < deadline)
        {

            std::this_thread::sleep_for(std::chrono::milliseconds { 100 });

        }
        srv.stop();
        if (server_thread.joinable()) server_thread.join();

        if (!have_response.load(std::memory_order_acquire))
        {

            result.error = "OAuth flow timed out (" + std::to_string(auth_timeout_seconds) + "s)";
            return result;

        }
        if (!received_error.empty())
        {

            result.error = "Google returned: " + received_error;
            return result;

        }
        if (received_code.empty())
        {

            result.error = "missing code in callback";
            return result;

        }
        if (received_state != state)
        {

            // CSRF: the state we got back doesn't match what we sent —
            // someone tried to feed us a redirect from a different flow.
            result.error = "state mismatch (possible CSRF)";
            return result;

        }

        // Exchange the code + verifier for tokens. The desktop client
        // type has no client_secret — that's correct, not an oversight.
        ::httplib::SSLClient token_client { "oauth2.googleapis.com" };
        token_client.set_connection_timeout(10);
        token_client.set_read_timeout(10);
        token_client.enable_server_certificate_verification(true);

        ::httplib::Params params { };
        params.emplace("code",          received_code);
        params.emplace("client_id",     client_id);
        params.emplace("redirect_uri",  redirect_uri);
        params.emplace("grant_type",    "authorization_code");
        params.emplace("code_verifier", verifier);

        const auto res { token_client.Post("/token", params) };
        if (!res or res->status != 200)
        {

            result.error = "token exchange failed (status=" + std::to_string(res ? res->status : -1) + ")";
            if (res) result.error += " body=" + res->body.substr(0, 200);
            return result;

        }

        result.id_token = extract_json_string(res->body, "id_token");
        if (result.id_token.empty())
        {

            result.error = "missing id_token in token response";
            return result;

        }

        // Decode the JWT payload (middle segment, base64url) so the UI
        // knows the user's name without round-tripping. NOT a signature
        // check — the gateway re-verifies on Hello, and that's where
        // trust is rooted.
        const auto first_dot  { result.id_token.find('.') };
        const auto second_dot { result.id_token.find('.', first_dot + 1) };
        if (first_dot != std::string::npos and second_dot != std::string::npos)
        {

            const std::string payload_b64 { result.id_token.substr(first_dot + 1, second_dot - first_dot - 1) };
            const std::string payload     { base64url_decode(payload_b64) };
            result.sub   = extract_json_string(payload, "sub");
            result.email = extract_json_string(payload, "email");
            result.name  = extract_json_string(payload, "name");

        }

        result.ok = true;
        return result;

    }

}
