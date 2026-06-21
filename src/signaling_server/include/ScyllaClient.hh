#ifndef SIGNALING_SERVER_SCYLLA_CLIENT_HH
#define SIGNALING_SERVER_SCYLLA_CLIENT_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <string>
#include <vector>

// 3rd party headers
#include <cassandra.h>

// project headers
#include "CassandraDeleters.hh"


namespace OpenSocialNet::Signaling
{

    // RAII holder around the Scylla connection + prepared-statement cache.
    // One instance is constructed in main() and shared across handlers via
    // GatewayState. Declaration order of the smart-ptr members matters for
    // destruction: preparedss must release before the session, and the
    // session before the cluster (which is what the DataStax driver
    // requires).
    class ScyllaClient
    {

    public:

        ScyllaClient() = default;

        ScyllaClient(const ScyllaClient&)            = delete;
        ScyllaClient& operator=(const ScyllaClient&) = delete;

        // Connect to the given contact point on keyspace "opensocialnet" and
        // prepare all statements. Exits the process on failure.
        void init(const std::string& contact_point);

        // Runs the synchronous SELECT to list every channel a user belongs
        // to. Called exactly once per connection (in on_hello).
        std::vector<std::string> user_channels(const std::string& user_id);


        // Idempotent INSERT into the `users` table — on Hello via JWT, the
        // gateway upserts the row keyed by the Google `sub` so a fresh
        // login becomes a first-class user record without a separate
        // signup flow. Synchronous because it happens exactly once per
        // connection on the WS loop thread and the latency is negligible
        // against the round-trip the client is already waiting on.
        // Returns false on driver/CQL failure (caller logs + proceeds).
        bool upsert_user(const std::string& user_id, const std::string& username);

        // Raw accessors. EnvelopeHandlers needs these for async bind/execute
        // since the Cass driver works in terms of these handle types.
        ::CassSession* session();
        ::CassUuidGen* uuid_gen();
        const ::CassPrepared* prep_insert_message();
        const ::CassPrepared* prep_fetch_history();

    private:

        // Prepare a single CQL statement, exiting the process on failure.
        CassPreparedPtr prepare(const char* cql);

        CassClusterPtr m_cluster { }; // long-lived cluster handle
        CassSessionPtr m_session { }; // connected session bound to the keyspace
        CassUuidGenPtr m_uuid_gen { }; // shared TimeUUID generator
        CassPreparedPtr m_prep_insert_message { }; // INSERT INTO messages ...
        CassPreparedPtr m_prep_fetch_history { }; // SELECT ... FROM messages ...
        CassPreparedPtr m_prep_user_channels { }; // SELECT channel_id FROM user_channels ...
        CassPreparedPtr m_prep_upsert_user { }; // INSERT INTO users (user_id, username, created_at) VALUES (?, ?, toTimestamp(now()))

    };

}

#endif // SIGNALING_SERVER_SCYLLA_CLIENT_HH
