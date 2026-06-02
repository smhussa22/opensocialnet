#ifndef SIGNALING_SERVER_KAFKA_BUS_HH
#define SIGNALING_SERVER_KAFKA_BUS_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <atomic>
#include <memory>
#include <string>
#include <thread>

// 3rd party headers
#include <librdkafka/rdkafkacpp.h>
#include <uwebsockets/App.h>

// project headers


namespace OpenSocialNet::Signaling
{

    // Owns the Kafka producer and a dedicated consumer thread. Each gateway
    // instance has its own consumer group (unique group.id), so each
    // instance receives every event and publishes only to clients connected
    // to itself. uWS::App::publish is safe to call from any thread, so the
    // consumer thread can republish directly.
    class KafkaBus
    {

    public:

        KafkaBus() = default;
        ~KafkaBus();

        KafkaBus(const KafkaBus&)            = delete;
        KafkaBus& operator=(const KafkaBus&) = delete;

        // Create the producer against `bootstrap_servers`. Exits on failure.
        void init(const std::string& bootstrap_servers);

        // Spin up the consumer thread that fans Kafka messages into the WS
        // App's pub/sub. `app` is borrowed; it must outlive the bus.
        void start_consumer(::uWS::App* app, const std::string& bootstrap_servers);

        // Signal the consumer thread to exit and join it. Idempotent.
        void shutdown();

        // Producer accessor used by EnvelopeHandlers to publish on the send
        // path. Borrowed pointer; the bus retains ownership.
        ::RdKafka::Producer* producer();

        // Topic name used for chat fanout.
        const std::string& topic_name() const;

    private:

        // Consumer thread entry point.
        void consumer_loop(::uWS::App* app, std::string bootstrap_servers);

        std::unique_ptr<::RdKafka::Producer> m_producer { }; // shared by handlers
        std::string m_topic_name { "chat_events" }; // single topic for chat fanout
        std::thread m_consumer_thread { }; // pulls from Kafka, publishes to WS topics
        std::atomic<bool> m_running { false }; // gate for consumer loop exit

    };

}

#endif // SIGNALING_SERVER_KAFKA_BUS_HH
