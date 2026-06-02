// related headers
#include "KafkaBus.hh"

// c sys headers
#include <cstdlib>

// cpp stdlib headers
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// 3rd party headers
#include <librdkafka/rdkafkacpp.h>
#include <uwebsockets/App.h>

// project headers
#include "proto/signaling.pb.h"


namespace OpenSocialNet::Signaling
{

    KafkaBus::~KafkaBus()
    {

        shutdown();

    }

    void KafkaBus::init(const std::string& bootstrap_servers)
    {

        std::string err { };
        std::unique_ptr<::RdKafka::Conf> conf { ::RdKafka::Conf::create(::RdKafka::Conf::CONF_GLOBAL) };
        conf->set("bootstrap.servers", bootstrap_servers, err);

        // Wrap immediately so there's no raw-pointer window even on the error path.
        m_producer.reset(::RdKafka::Producer::create(conf.get(), err));
        if (!m_producer)
        {

            std::cerr << "kafka producer create failed: " << err << '\n';
            std::exit(1);

        }

    }

    void KafkaBus::start_consumer(::uWS::App* app, const std::string& bootstrap_servers)
    {

        if (m_running.exchange(true)) return;
        m_consumer_thread = std::thread { [this, app, bootstrap_servers]() { consumer_loop(app, bootstrap_servers); } };

    }

    void KafkaBus::shutdown()
    {

        if (!m_running.exchange(false)) return;
        if (m_consumer_thread.joinable()) m_consumer_thread.join();

    }

    ::RdKafka::Producer* KafkaBus::producer()
    {

        return m_producer.get();

    }

    const std::string& KafkaBus::topic_name() const
    {

        return m_topic_name;

    }

    void KafkaBus::consumer_loop(::uWS::App* app, std::string bootstrap_servers)
    {

        std::string err { };
        std::unique_ptr<::RdKafka::Conf> conf { ::RdKafka::Conf::create(::RdKafka::Conf::CONF_GLOBAL) };
        conf->set("bootstrap.servers", bootstrap_servers, err);

        const auto now = std::chrono::system_clock::now().time_since_epoch().count();
        conf->set("group.id", "gateway-" + std::to_string(now), err);
        conf->set("auto.offset.reset", "latest", err);

        std::unique_ptr<::RdKafka::KafkaConsumer> consumer { ::RdKafka::KafkaConsumer::create(conf.get(), err) };
        if (!consumer)
        {

            std::cerr << "kafka consumer create failed: " << err << '\n';
            return;

        }
        consumer->subscribe(std::vector { m_topic_name });

        while (m_running.load(std::memory_order_relaxed))
        {

            std::unique_ptr<::RdKafka::Message> msg { consumer->consume(1000) };
            if (msg->err() == ::RdKafka::ERR__TIMED_OUT) continue;
            if (msg->err() != ::RdKafka::ERR_NO_ERROR)
            {

                std::cerr << "kafka consume error: " << msg->errstr() << '\n';
                continue;

            }

            // The Kafka payload is already a serialized Envelope containing
            // a ChatMessageEvent. Route it to the channel_id topic for fanout.
            ::signaling::Envelope envelope { };
            if (!envelope.ParseFromArray(msg->payload(), static_cast<int>(msg->len()))) continue;
            if (!envelope.has_chat_message_event()) continue;

            const auto& evt = envelope.chat_message_event();
            // uWS::App::publish is safe to call from any thread; it defers
            // the actual sends onto the WS loop.
            app->publish(evt.channel_id(), std::string_view { static_cast<const char*>(msg->payload()), msg->len() }, ::uWS::OpCode::BINARY);

        }

        consumer->close();

    }

}
