// related headers
#include "SignalingClient.hh"

// c sys headers

// cpp stdlib headers
#include <cstdio>
#include <string>
#include <vector>

// 3rd party headers
#include "proto/signaling.pb.h"

// project headers


namespace OpenSocialNet::Network
{

    namespace
    {

        bool send_envelope(WebSocketClient& ws, const ::signaling::Envelope& env)
        {

            std::string buf { };
            if (!env.SerializeToString(&buf)) return false;
            return ws.send_binary(std::span<const std::uint8_t> { reinterpret_cast<const std::uint8_t*>(buf.data()), buf.size() });

        }

        bool recv_envelope(WebSocketClient& ws, ::signaling::Envelope& env)
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


    bool SignalingClient::connect_and_hello(const std::string& host, std::uint16_t port, const std::string& path, const std::string& user_id, const std::string& auth_token)
    {

        if (!m_ws.connect(host, port, path)) return false;
        m_ws.set_recv_timeout_ms(10'000); // 10s — server should reply Ready immediately

        ::signaling::Envelope out_env { };
        auto* hello = out_env.mutable_hello();
        hello->set_user_id(user_id);
        hello->set_auth_token(auth_token);
        if (!send_envelope(m_ws, out_env))
        {

            std::fprintf(stderr, "[signaling] failed to send Hello\n");
            m_ws.close();
            return false;

        }

        ::signaling::Envelope in_env { };
        if (!recv_envelope(m_ws, in_env))
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
        if (!send_envelope(m_ws, out_env))
        {

            std::fprintf(stderr, "[signaling] failed to send JoinVoice\n");
            return false;

        }

        // The server's expected response sequence is:
        //   1. VoicePeerJoined where peer.user_id == us (our slot in the relay).
        //   2. Zero or more VoicePeerJoined for already-present peers.
        // We can't tell from the wire how many "existing peers" frames will
        // arrive, so the protocol the server commits to is: SELF comes first
        // and is the last frame we're guaranteed to see before the call
        // settles. Read until we find self, capture anything else along the
        // way, then return. (If the server later adds an "end of list" marker
        // we'd switch to that, but for now first-self-then-done is the rule.)
        bool found_self { false };
        for (int i { 0 }; i < 32 and !found_self; ++i)
        {

            ::signaling::Envelope in_env { };
            if (!recv_envelope(m_ws, in_env)) return false;

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

        std::printf("[signaling] joined channel=%s self_ssrc=%u relay=%s:%u others=%zu\n", channel_id.c_str(), self_out.ssrc, self_out.ip.c_str(), self_out.port, others_out.size());
        return true;

    }

}
