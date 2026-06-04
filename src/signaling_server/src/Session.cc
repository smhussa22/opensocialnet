// related headers
#include "Session.hh"

// c sys headers
#include <cstdio>

// cpp stdlib headers
#include <mutex>
#include <random>
#include <string>

// 3rd party headers

// project headers


namespace OpenSocialNet::Signaling
{

    void SessionRegistry::add(const std::string& session_id, WebSocket* ws)
    {

        std::lock_guard<std::mutex> lock { m_mu };
        m_sessions[session_id] = ws;

    }

    void SessionRegistry::remove(const std::string& session_id)
    {

        std::lock_guard<std::mutex> lock { m_mu };
        m_sessions.erase(session_id);

    }

    WebSocket* SessionRegistry::lookup(const std::string& session_id)
    {

        if (session_id.empty()) return nullptr;
        std::lock_guard<std::mutex> lock { m_mu };
        auto it = m_sessions.find(session_id);
        return it != m_sessions.end() ? it->second : nullptr;

    }

    void SessionRegistry::for_each(const std::function<void(const std::string&, WebSocket*)>& fn)
    {

        // snapshot under lock; iterate outside so fn can safely call back
        // into the registry (lookup / remove) without re-entrant deadlock,
        // and so a slow fn doesn't stall other gateway threads.
        std::vector<std::pair<std::string, WebSocket*>> snapshot { };
        {

            std::lock_guard<std::mutex> lock { m_mu };
            snapshot.reserve(m_sessions.size());
            for (const auto& kv : m_sessions) snapshot.emplace_back(kv.first, kv.second);

        }

        for (auto& kv : snapshot) fn(kv.first, kv.second);

    }

    std::string make_session_id()
    {

        // 16 hex chars from a thread-local PRNG. Real impl: UUIDv4 from CSPRNG.
        thread_local std::mt19937_64 rng { std::random_device { }() };
        char buf[17] { };
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
        return std::string(buf, 16);

    }

}
