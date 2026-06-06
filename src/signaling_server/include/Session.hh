#ifndef SIGNALING_SERVER_SESSION_HH
#define SIGNALING_SERVER_SESSION_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 3rd party headers
#include <uwebsockets/App.h>

// project headers


namespace OpenSocialNet::Signaling
{

    // Per-connection state. uWebSockets allocates one of these inside each
    // WS handle via the template parameter on `ws<>()`. Access via
    // `ws->getUserData()`.
    struct Session
    {

        std::string user_id { }; // authenticated app-level user id, set in on_hello
        std::string session_id { }; // stable per-connection id used for async lookups
        bool authenticated { false }; // flipped true once Hello + auth succeeds
        std::string current_voice_room_id { }; // populated by on_join_voice; non-empty means the peer is registered in the relay's room table and .close must tell the relay to drop them

    };

    // uWS template params: <SSL, isServer, UserData>
    using WebSocket = ::uWS::WebSocket<false, true, Session>;


    // Thread-safe registry mapping session_id -> live WebSocket*. Used by
    // async continuations (Scylla driver IO thread, SFU event reader thread)
    // to look the socket back up after bouncing onto the WS loop.
    class SessionRegistry
    {

    public:

        SessionRegistry() = default;

        SessionRegistry(const SessionRegistry&)            = delete;
        SessionRegistry& operator=(const SessionRegistry&) = delete;

        // Register a session after auth succeeds. Pre-auth connections are
        // deliberately invisible to lookups.
        void add(const std::string& session_id, WebSocket* ws);

        // Remove a session on disconnect.
        void remove(const std::string& session_id);

        // Returns the current WebSocket* for a session_id, or nullptr if
        // the session is no longer registered. Must be called from the WS
        // loop thread to be race-free with .close erasures.
        WebSocket* lookup(const std::string& session_id);

        // Snapshots the (session_id, WebSocket*) pairs under the registry
        // lock, then invokes `fn` for each WITHOUT holding the lock. Caller
        // is expected to run on the uWS loop thread so that dereferencing
        // each ws (to read Session userdata) is race-free with concurrent
        // .close erasures. Used by fanout paths that need to walk every
        // session in a given room (e.g. VoicePeerJoined broadcasts).
        void for_each(const std::function<void(const std::string&, WebSocket*)>& fn);

    private:

        std::mutex m_mu { }; // guards m_sessions
        std::unordered_map<std::string, WebSocket*> m_sessions { }; // session_id -> live WS handle

    };

    // 16 hex chars from a thread-local PRNG. Real impl: UUIDv4 from CSPRNG.
    std::string make_session_id();

}

#endif // SIGNALING_SERVER_SESSION_HH
