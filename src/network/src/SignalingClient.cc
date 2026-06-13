// related headers
#include "SignalingClient.hh"

// c sys headers

// cpp stdlib headers
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// 3rd party headers
#include "proto/signaling.pb.h"

// project headers


namespace OpenSocialNet::Network
{

    namespace
    {

        bool send_envelope_impl(WebSocketClient& ws, const ::signaling::Envelope& env)
        {

            std::string buf { };
            if (!env.SerializeToString(&buf)) return false;
            return ws.send_binary(std::span<const std::uint8_t> { reinterpret_cast<const std::uint8_t*>(buf.data()), buf.size() });

        }

        bool recv_envelope_impl(WebSocketClient& ws, ::signaling::Envelope& env)
        {

            std::vector<std::uint8_t> raw { };
            if (!ws.recv_binary(raw)) return false;
            if (!env.ParseFromArray(raw.data(), static_cast<int>(raw.size())))
            {

                std::fprintf(stderr, "[signaling] failed to parse envelope (%zu bytes)\n", raw.size());
                return false;

            }
            return true;

        }

    }


    SignalingClient::~SignalingClient() { close(); }


    bool SignalingClient::connect_and_hello(const std::string& host, std::uint16_t port, const std::string& path, const std::string& user_id, const std::string& auth_token)
    {

        if (!m_ws.connect(host, port, path)) return false;
        m_ws.set_recv_timeout_ms(10'000); // 10s — server should reply Ready immediately

        ::signaling::Envelope out_env { };
        auto* hello = out_env.mutable_hello();
        hello->set_user_id(user_id);
        hello->set_auth_token(auth_token);
        if (!send_envelope_impl(m_ws, out_env))
        {

            std::fprintf(stderr, "[signaling] failed to send Hello\n");
            m_ws.close();
            return false;

        }

        ::signaling::Envelope in_env { };
        if (!recv_envelope_impl(m_ws, in_env))
        {

            std::fprintf(stderr, "[signaling] no Ready response\n");
            m_ws.close();
            return false;

        }

        if (in_env.payload_case() == ::signaling::Envelope::kError)
        {

            std::fprintf(stderr, "[signaling] server error %u: %s\n", in_env.error().code(), in_env.error().message().c_str());
            m_ws.close();
            return false;

        }

        if (in_env.payload_case() != ::signaling::Envelope::kReady)
        {

            std::fprintf(stderr, "[signaling] expected Ready, got payload_case=%d\n", static_cast<int>(in_env.payload_case()));
            m_ws.close();
            return false;

        }

        m_user_id    = user_id;
        m_session_id = in_env.ready().session_id();
        std::printf("[signaling] hello ok: session=%s\n", m_session_id.c_str());
        return true;

    }


    bool SignalingClient::join_voice(const std::string& channel_id, VoicePeerInfo& self_out, std::vector<VoicePeerInfo>& others_out)
    {

        if (!m_ws.is_open()) return false;
        self_out   = { };
        others_out.clear();

        ::signaling::Envelope out_env { };
        out_env.mutable_join_voice()->set_channel_id(channel_id);
        if (!send_envelope_impl(m_ws, out_env))
        {

            std::fprintf(stderr, "[signaling] failed to send JoinVoice\n");
            return false;

        }

        // Protocol contract (matches signaling_server's on_join_voice):
        //   1. VoicePeerJoined where peer.user_id == us — our slot.
        //   2. Zero or more VoicePeerJoined for already-present peers.
        // The third class of events (peers that join AFTER us) arrives on
        // the reader thread; this function is only the initial sync round.
        bool found_self { false };
        for (int i { 0 }; i < 32 and !found_self; ++i)
        {

            ::signaling::Envelope in_env { };
            if (!recv_envelope_impl(m_ws, in_env)) return false;

            if (in_env.payload_case() == ::signaling::Envelope::kError)
            {

                std::fprintf(stderr, "[signaling] join_voice rejected: %u %s\n", in_env.error().code(), in_env.error().message().c_str());
                return false;

            }

            if (in_env.payload_case() != ::signaling::Envelope::kVoicePeerJoined) continue;

            const auto& vp { in_env.voice_peer_joined().peer() };
            VoicePeerInfo info { };
            info.user_id = vp.user_id();
            info.ip      = vp.ip();
            info.port    = static_cast<std::uint16_t>(vp.port());
            info.ssrc    = vp.ssrc();

            if (info.user_id == m_user_id)
            {

                self_out   = info;
                found_self = true;

            }
            else
            {

                others_out.push_back(std::move(info));

            }

        }

        if (!found_self)
        {

            std::fprintf(stderr, "[signaling] never received self in VoicePeerJoined sequence\n");
            return false;

        }

        // Pre-populate the live peer list so the first peers() poll sees
        // everyone that was here at join time. The reader thread will
        // keep this in sync as people come and go.
        {

            std::scoped_lock<std::mutex> lock { m_peers_mu };
            m_peers = others_out;

        }

        std::printf("[signaling] joined channel=%s self_ssrc=%u relay=%s:%u others=%zu\n", channel_id.c_str(), self_out.ssrc, self_out.ip.c_str(), self_out.port, others_out.size());
        return true;

    }


    void SignalingClient::start_event_reader()
    {

        if (m_reader.joinable()) return; // already running
        m_running.store(true, std::memory_order_release);

        // Reader uses indefinite recv since it's purely event-driven.
        // The handshake's 10s timeout would just churn EAGAIN here.
        m_ws.set_recv_timeout_ms(0);

        m_reader = std::thread { [this] { run_reader(); } };

    }


    void SignalingClient::run_reader()
    {

        while (m_running.load(std::memory_order_acquire))
        {

            ::signaling::Envelope env { };
            if (!recv_envelope_impl(m_ws, env)) break;

            switch (env.payload_case())
            {

                case ::signaling::Envelope::kVoicePeerJoined:
                {

                    const auto& vp { env.voice_peer_joined().peer() };
                    VoicePeerInfo info { };
                    info.user_id = vp.user_id();
                    info.ip      = vp.ip();
                    info.port    = static_cast<std::uint16_t>(vp.port());
                    info.ssrc    = vp.ssrc();

                    // Skip self-events (the server doesn't send us self
                    // outside join_voice, but be defensive — protocol
                    // could evolve to broadcast self-info on rejoin).
                    if (info.user_id == m_user_id) break;

                    {

                        std::scoped_lock<std::mutex> lock { m_peers_mu };
                        // Idempotency: a re-broadcast of the same peer
                        // shouldn't double-count. Replace by user_id.
                        std::erase_if(m_peers, [&](const VoicePeerInfo& p) { return p.user_id == info.user_id; });
                        m_peers.push_back(info);

                    }
                    std::printf("[signaling] peer joined: %s (ssrc=%u)\n", info.user_id.c_str(), info.ssrc);
                    break;

                }

                case ::signaling::Envelope::kVoicePeerLeft:
                {

                    const std::string left_user { env.voice_peer_left().user_id() };
                    {

                        std::scoped_lock<std::mutex> lock { m_peers_mu };
                        std::erase_if(m_peers, [&](const VoicePeerInfo& p) { return p.user_id == left_user; });

                    }
                    std::printf("[signaling] peer left: %s\n", left_user.c_str());
                    break;

                }

                case ::signaling::Envelope::kChatMessageEvent:
                {

                    const auto& evt { env.chat_message_event() };
                    if (evt.sender_id() == m_user_id) break; // our own echo / fanout copy
                    if (m_on_chat) m_on_chat(evt.sender_id(), evt.content());
                    break;

                }

                case ::signaling::Envelope::kHeartbeatAck:
                case ::signaling::Envelope::kReady:
                    // expected / harmless
                    break;

                case ::signaling::Envelope::kError:
                    std::fprintf(stderr, "[signaling] server error %u: %s\n", env.error().code(), env.error().message().c_str());
                    break;

                default:
                    // Unknown frame types are ignored.
                    break;

            }

        }

        m_running.store(false, std::memory_order_release);

    }


    bool SignalingClient::send_heartbeat()
    {

        if (!m_ws.is_open()) return false;

        ::signaling::Envelope env { };
        env.mutable_heartbeat()->set_nonce(0);

        std::scoped_lock<std::mutex> lock { m_send_mu };
        return send_envelope_impl(m_ws, env);

    }


    bool SignalingClient::send_chat(const std::string& channel_id, const std::string& content)
    {

        if (!m_ws.is_open()) return false;

        std::scoped_lock<std::mutex> lock { m_send_mu };

        ::signaling::Envelope env { };
        auto* msg = env.mutable_send_message();
        msg->set_client_nonce(m_session_id + "-" + std::to_string(m_nonce++));
        msg->set_channel_id(channel_id);
        msg->set_content(content);
        return send_envelope_impl(m_ws, env);

    }


    std::vector<VoicePeerInfo> SignalingClient::peers() const
    {

        std::scoped_lock<std::mutex> lock { m_peers_mu };
        return m_peers;

    }


    void SignalingClient::close() noexcept
    {

        m_running.store(false, std::memory_order_release);
        m_ws.close();   // unblocks the reader thread by EBADF-ing its recv
        if (m_reader.joinable()) m_reader.join();

    }

}
