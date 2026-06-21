// related headers
#include "IceTransport.hh"

// c sys headers
#include <arpa/inet.h>
#include <netdb.h>
#include <cstdio>

// cpp stdlib headers
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

// 3rd party headers

// project headers

namespace
{

    // libnice wants STUN/TURN servers as literal IPs; resolve a hostname
    // to dotted IPv4 (first A record), empty string on failure.
    std::string resolve_ipv4(const std::string& host) noexcept
    {

        ::addrinfo hints { };
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        ::addrinfo* result { nullptr };
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 or result == nullptr) return { };

        char buf[INET_ADDRSTRLEN] { };
        const auto* addr { reinterpret_cast<const ::sockaddr_in*>(result->ai_addr) };
        const char* text { ::inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) };
        ::freeaddrinfo(result);

        return text != nullptr ? std::string { text } : std::string { };

    }

    const char* candidate_type_name(::NiceCandidateType type) noexcept
    {

        switch (type)
        {

            case NICE_CANDIDATE_TYPE_HOST:             return "host";
            case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE: return "srflx";
            case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:   return "prflx";
            case NICE_CANDIDATE_TYPE_RELAYED:          return "relay";

        }
        return "?";

    }

    // Drop every "a=candidate:..." line whose type token isn't "relay".
    // libnice's SDP format is one candidate per line, the type is the
    // word right after " typ ". Non-candidate lines pass through
    // unchanged so credentials (ufrag/pwd) and stream metadata survive.
    std::string filter_relay_only(const std::string& sdp) noexcept
    {

        std::string out { };
        out.reserve(sdp.size());

        std::size_t i { 0 };
        while (i < sdp.size())
        {

            const std::size_t nl { sdp.find('\n', i) };
            const std::size_t end { nl == std::string::npos ? sdp.size() : nl + 1 };
            const std::string_view line { sdp.data() + i, end - i };

            bool keep { true };
            if (line.rfind("a=candidate:", 0) == 0)
            {

                const std::size_t typ { line.find(" typ ") };
                if (typ != std::string_view::npos)
                {

                    const std::string_view tail { line.substr(typ + 5) };
                    const std::size_t stop { tail.find_first_of(" \r\n") };
                    const std::string_view type { tail.substr(0, stop) };
                    if (type != "relay") keep = false;

                }

            }

            if (keep) out.append(line);
            i = end;

        }
        return out;

    }

    // Poll an atomic until predicate-true or the deadline passes. The
    // flags are flipped by the loop thread; 20ms granularity is plenty
    // for one-shot negotiation steps.
    template <typename Predicate>
    bool wait_until(Predicate&& done, int timeout_ms) noexcept
    {

        const auto deadline { std::chrono::steady_clock::now() + std::chrono::milliseconds { timeout_ms } };
        while (std::chrono::steady_clock::now() < deadline)
        {

            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds { 20 });

        }
        return done();

    }

}

namespace OpenSocialNet::Network
{

    bool IceTransport::init(const IceConfig& config, RecvCallback recv_callback) noexcept
    {

        on_recv = std::move(recv_callback);
        force_relay = config.force_relay;
        if (force_relay) std::printf("[ice] force-relay mode — host/srflx candidates will be stripped from SDPs (debug knob)\n");

        context.reset(::g_main_context_new());
        agent.reset(::nice_agent_new(context.get(), NICE_COMPATIBILITY_RFC5245));
        if (!agent)
        {

            std::printf("[ice] nice_agent_new failed\n");
            shutdown();
            return false;

        }

        // UDP only: the media path is loss-tolerant datagrams; TCP
        // candidates would just add head-of-line latency cliffs.
        ::g_object_set(G_OBJECT(agent.get()), "ice-tcp", FALSE, nullptr);

        if (!config.stun_host.empty())
        {

            const std::string stun_ip { resolve_ipv4(config.stun_host) };
            if (stun_ip.empty()) std::printf("[ice] cannot resolve STUN host %s — host candidates only\n", config.stun_host.c_str());
            else
            {

                ::g_object_set(G_OBJECT(agent.get()), "stun-server", stun_ip.c_str(), "stun-server-port", static_cast<::guint>(config.stun_port), nullptr);
                std::printf("[ice] STUN %s:%u\n", stun_ip.c_str(), config.stun_port);

            }

        }

        stream_id = ::nice_agent_add_stream(agent.get(), 1);
        if (stream_id == 0)
        {

            std::printf("[ice] nice_agent_add_stream failed\n");
            shutdown();
            return false;

        }

        // generate/parse_local_sdp skip nameless streams.
        ::nice_agent_set_stream_name(agent.get(), stream_id, "application");

        if (!config.turn_host.empty())
        {

            const std::string turn_ip { resolve_ipv4(config.turn_host) };
            if (turn_ip.empty()) std::printf("[ice] cannot resolve TURN host %s — no relayed candidates\n", config.turn_host.c_str());
            else if (!::nice_agent_set_relay_info(agent.get(), stream_id, 1, turn_ip.c_str(), static_cast<::guint>(config.turn_port), config.turn_user.c_str(), config.turn_pass.c_str(), NICE_RELAY_TYPE_TURN_UDP)) std::printf("[ice] set_relay_info failed for %s\n", config.turn_host.c_str());
            else std::printf("[ice] TURN %s:%u user=%s\n", turn_ip.c_str(), config.turn_port, config.turn_user.c_str());

        }

        g_signal_connect(G_OBJECT(agent.get()), "candidate-gathering-done", G_CALLBACK(&IceTransport::on_gathering_done), this);
        g_signal_connect(G_OBJECT(agent.get()), "component-state-changed", G_CALLBACK(&IceTransport::on_state_changed), this);
        g_signal_connect(G_OBJECT(agent.get()), "new-selected-pair-full", G_CALLBACK(&IceTransport::on_pair_selected), this);

        ::nice_agent_attach_recv(agent.get(), stream_id, 1, context.get(), &IceTransport::on_data, this);

        loop_running.store(true, std::memory_order_release);
        loop_thread = std::thread { [this]
        {

            while (loop_running.load(std::memory_order_acquire)) ::g_main_context_iteration(context.get(), TRUE);

        } };

        if (!::nice_agent_gather_candidates(agent.get(), stream_id))
        {

            std::printf("[ice] gather_candidates failed\n");
            shutdown();
            return false;

        }

        return true;

    }

    bool IceTransport::wait_gathered(int timeout_ms) noexcept
    {

        return wait_until([this] { return gathered.load(std::memory_order_acquire); }, timeout_ms);

    }

    std::string IceTransport::local_description() const noexcept
    {

        if (!agent) return { };

        const GCharPtr raw { ::nice_agent_generate_local_sdp(agent.get()) };
        if (raw == nullptr) return { };
        std::string sdp { raw.get() };
        if (force_relay) sdp = filter_relay_only(sdp);
        return sdp;

    }

    bool IceTransport::start_checks(const std::string& remote_sdp, bool controlling) noexcept
    {

        if (!agent) return false;

        ::g_object_set(G_OBJECT(agent.get()), "controlling-mode", controlling ? TRUE : FALSE, nullptr);

        const std::string sdp_to_parse { force_relay ? filter_relay_only(remote_sdp) : remote_sdp };
        const int added { ::nice_agent_parse_remote_sdp(agent.get(), sdp_to_parse.c_str()) };
        if (added <= 0)
        {

            std::printf("[ice] parse_remote_sdp failed (%d)\n", added);
            return false;

        }

        std::printf("[ice] %d remote candidates ingested, %s role, checks running\n", added, controlling ? "controlling" : "controlled");
        return true;

    }

    bool IceTransport::wait_ready(int timeout_ms) noexcept
    {

        const bool ok { wait_until([this]
        {

            const unsigned s { state.load(std::memory_order_acquire) };
            return s == NICE_COMPONENT_STATE_CONNECTED or s == NICE_COMPONENT_STATE_READY or s == NICE_COMPONENT_STATE_FAILED;

        }, timeout_ms) };

        return ok and connected();

    }

    bool IceTransport::send(std::span<const std::uint8_t> data) noexcept
    {

        if (!connected()) return false;
        return ::nice_agent_send(agent.get(), stream_id, 1, static_cast<::guint>(data.size()), reinterpret_cast<const ::gchar*>(data.data())) == static_cast<::gint>(data.size());

    }

    bool IceTransport::connected() const noexcept
    {

        const unsigned s { state.load(std::memory_order_acquire) };
        return s == NICE_COMPONENT_STATE_CONNECTED or s == NICE_COMPONENT_STATE_READY;

    }

    void IceTransport::shutdown() noexcept
    {

        loop_running.store(false, std::memory_order_release);
        if (context) ::g_main_context_wakeup(context.get());
        if (loop_thread.joinable()) loop_thread.join();

        // agent before the context it schedules on
        agent.reset();
        context.reset();
        stream_id = 0;
        gathered.store(false, std::memory_order_release);
        state.store(0, std::memory_order_release);
        on_recv = nullptr;

    }

    void IceTransport::on_gathering_done(::NiceAgent*, ::guint, ::gpointer self) noexcept
    {

        auto* transport { static_cast<IceTransport*>(self) };
        transport->gathered.store(true, std::memory_order_release);
        std::printf("[ice] candidate gathering done\n");

    }

    void IceTransport::on_state_changed(::NiceAgent*, ::guint, ::guint, ::guint new_state, ::gpointer self) noexcept
    {

        auto* transport { static_cast<IceTransport*>(self) };
        transport->state.store(new_state, std::memory_order_release);
        std::printf("[ice] component state -> %s\n", ::nice_component_state_to_string(static_cast<::NiceComponentState>(new_state)));

    }

    void IceTransport::on_pair_selected(::NiceAgent*, ::guint, ::guint, ::NiceCandidate* local, ::NiceCandidate* remote, ::gpointer) noexcept
    {

        char local_addr[NICE_ADDRESS_STRING_LEN] { };
        char remote_addr[NICE_ADDRESS_STRING_LEN] { };
        ::nice_address_to_string(&local->addr, local_addr);
        ::nice_address_to_string(&remote->addr, remote_addr);

        std::printf("[ice] selected pair: local %s %s:%u <-> remote %s %s:%u\n",
            candidate_type_name(local->type), local_addr, ::nice_address_get_port(&local->addr),
            candidate_type_name(remote->type), remote_addr, ::nice_address_get_port(&remote->addr));

    }

    void IceTransport::on_data(::NiceAgent*, ::guint, ::guint, ::guint size, ::gchar* buffer, ::gpointer self) noexcept
    {

        auto* transport { static_cast<IceTransport*>(self) };
        if (transport->on_recv) transport->on_recv(std::span<const std::uint8_t> { reinterpret_cast<const std::uint8_t*>(buffer), size });

    }

}
