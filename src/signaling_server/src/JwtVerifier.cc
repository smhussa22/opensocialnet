// related headers
#include "JwtVerifier.hh"

// c sys headers

// cpp stdlib headers
#include <exception>
#include <string>

// 3rd party headers
#include <jwt-cpp/jwt.h>

// project headers
#include "JwksCache.hh"


namespace OpenSocialNet::Signaling
{

    JwtVerifyResult verify_google_id_token(JwksCache& jwks, const std::string& jwt_str, const std::string& expected_audience)
    {

        JwtVerifyResult result { };

        if (jwt_str.empty())
        {

            result.error = "empty token";
            return result;

        }

        try
        {

            // Step 1: structural decode. Doesn't validate signature yet —
            // we need the `kid` header to know which JWKS key to use.
            const auto decoded { ::jwt::decode(jwt_str) };

            std::string kid { decoded.get_key_id() };

            // Step 2: pull the matching RSA public key out of the JWKS
            // cache. None means either network failure or Google has
            // rotated and the kid is unknown even after a refresh.
            const auto pem_opt { jwks.key_pem_for_kid(kid) };
            if (!pem_opt.has_value())
            {

                result.error = "no JWKS key for kid " + kid;
                return result;

            }

            // Step 3: issuer check by hand. Google publishes both
            // "https://accounts.google.com" and "accounts.google.com" as
            // valid iss values; jwt-cpp's with_issuer is single-valued so
            // we compare ourselves rather than chain two verifiers.
            const std::string iss { decoded.get_issuer() };
            if (iss != "https://accounts.google.com" and iss != "accounts.google.com")
            {

                result.error = "unexpected issuer: " + iss;
                return result;

            }

            // Step 4: signature + audience + expiry verification via
            // jwt-cpp. Exp is enforced automatically as part of the
            // claims pass; we set the leeway to zero (Google's tokens
            // are short-lived but their server clocks are accurate).
            const auto verifier { ::jwt::verify()
                .allow_algorithm(::jwt::algorithm::rs256 { *pem_opt, "", "", "" })
                .with_audience(expected_audience)
                .leeway(0) };
            verifier.verify(decoded);

            // Step 5: claim extraction. sub is mandatory; email/name are
            // best-effort (only present when those scopes were granted).
            result.sub = decoded.get_payload_claim("sub").as_string();
            try { result.email = decoded.get_payload_claim("email").as_string(); } catch (...) { }
            try { result.name  = decoded.get_payload_claim("name").as_string();  } catch (...) { }

            result.ok = true;
            return result;

        }
        catch (const std::exception& ex)
        {

            result.error = std::string { "JWT verify failed: " } + ex.what();
            return result;

        }

    }

}
