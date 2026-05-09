// related headers

// c sys headers

// cpp stdlib headers
#include <string>
#include <memory>
#include <iostream>
#include <chrono>
#include <stdexcept>

// 3rd party headers
#include <grpcpp/grpcpp.h>
#include <librdkafka/rdkafkacpp.h>

// project headers
#include "proto/signaling.grpc.pb.h"
#include "proto/signaling.pb.h"

class ChatServer final : public ::signaling::SignalingService::Service
{
public:
  ChatServer ()
  {

    std::string errstr {};
    std::unique_ptr<RdKafka::Conf> kafka_configuration_object{
      RdKafka::Conf::create(RdKafka::Conf::ConfType::CONF_GLOBAL)
    };

    kafka_configuration_object->set("bootstrap.servers", "localhost:9092", errstr);
    auto raw_producer { RdKafka::Producer::create(kafka_configuration_object.get(), errstr) };
    if (raw_producer == nullptr)
    {

      throw std::runtime_error("Producer could not be created: " + errstr);

    }
    producer = std::unique_ptr<RdKafka::Producer>(raw_producer);


  }

  grpc::Status SendMessage(grpc::ServerContext *context, const signaling::ChatMessage *request, signaling::MessageAcknowledged *response) override
  {

    std::string serialized_data {};
    request->SerializeToString(&serialized_data);

    auto produce_status {
      producer->produce(
        topic_name,
        RdKafka::Topic::PARTITION_UA,
        RdKafka::Producer::MSG_COPY,
        const_cast<char*>(serialized_data.c_str()),
        serialized_data.size(),
        const_cast<char*>(request->target_id().c_str()),
        request->target_id().size(),
        request->timestamp_ms(),
        nullptr
      )
    };

    producer->poll(0); // flush delivery events

    if (produce_status != RdKafka::ERR_NO_ERROR)
    {

      std::cerr << "Kafka produce failed: " << RdKafka::err2str(produce_status) << std::endl;
      response->set_success(false);

      return grpc::Status(grpc::StatusCode::INTERNAL, RdKafka::err2str(produce_status));

    }

    response->set_success(true);
    return grpc::Status::OK;

  }

  grpc::Status StreamEvents(grpc::ServerContext *context, const signaling::UserIdentity *request, grpc::ServerWriter<signaling::ChatMessage> *writer) override
  {

    std::string user_id { request->user_id() };
    std::string errstr {};

    std::unique_ptr<RdKafka::Conf> kafka_configuration_object{ RdKafka::Conf::create(RdKafka::Conf::ConfType::CONF_GLOBAL) };
    kafka_configuration_object->set("bootstrap.servers", "localhost:9092", errstr);
    kafka_configuration_object->set("auto.offset.reset", "latest", errstr);
    const auto now { std::chrono::system_clock::now().time_since_epoch().count() };
    // @todo fix potential edge case where two devices hit now at theex
    std::string unique_group_id { user_id + "_" + std::to_string(now) }; // so that two devices like a laptop and phone dont get different partitions and thus different messages
    kafka_configuration_object->set("group.id", unique_group_id, errstr);

    auto raw_consumer { RdKafka::KafkaConsumer::create(kafka_configuration_object.get(), errstr) };
    if (!raw_consumer)
    {

      return grpc::Status(grpc::StatusCode::INTERNAL, errstr);

    }

    std::unique_ptr<RdKafka::KafkaConsumer> consumer { raw_consumer };
    consumer->subscribe(std::vector{ topic_name }); // has to be a std vector for multiple topics, but we just need the one

    while (!context->IsCancelled())
    {

      std::unique_ptr<RdKafka::Message> message { consumer->consume(1000) }; // consumes topic for 1 s
      if (message->err() == RdKafka::ERR_NO_ERROR) // no error
      {

        signaling::ChatMessage chat_message {};
        if (chat_message.ParseFromArray(message->payload(), static_cast<int>(message->len())))
        {

          if (chat_message.target_id() == user_id)
          {

           if (!writer->Write(chat_message)) break;

          }

        }

      }
      else if (message->err() != RdKafka::ERR__TIMED_OUT) // not timed out
      {

        std::cerr << "Consumer error: " << message->errstr() << std::endl;

      }

    }

    consumer->close();
    return grpc::Status::OK;

  }

private:
  std::unique_ptr<RdKafka::Producer> producer { };
  std::string topic_name { "chat_events" };

};

int main()
{

  std::string server_address { "0.0.0.0:50051" };
  ChatServer service { };
  grpc::ServerBuilder grpc_server_builder { };
  grpc_server_builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  grpc_server_builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(grpc_server_builder.BuildAndStart());
  std::cout << "Signaling Server listening on " << server_address << '\n';

  server->Wait();
  return 0;

}