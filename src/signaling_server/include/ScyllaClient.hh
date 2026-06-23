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
        // `email` is denormalized into users_by_email so the GUI can do
        // friend lookup by email instead of forcing the Google sub.
        bool upsert_user(const std::string& user_id, const std::string& username, const std::string& email = "");


        // One row out of users_by_email. user_id is empty when no match.
        struct UserByEmail
        {

            std::string user_id  { };
            std::string username { };
            std::string email    { };

        };

        // Exact-match email -> user lookup. Returns a row with empty
        // user_id if there's no match; caller treats that as "not found".
        UserByEmail lookup_user_by_email(const std::string& email);

        // Look up a user's display name. Empty string if the row doesn't
        // exist; used when joining friend rows with their friend's name
        // for the FriendListResponse / FriendRequestEvent payloads.
        std::string username_for(const std::string& user_id);


        // ---- channel bootstrap (Part A: auto-join default channel) ----

        // Idempotent INSERT into `channels`. Called once on gateway
        // startup to make sure the default `general` channel exists
        // before any user tries to join it.
        bool ensure_channel(const std::string& channel_id, const std::string& name, const std::string& kind);

        // Idempotent two-write INSERT — adds the user to both the
        // forward (channel_members) and reverse (user_channels) indices
        // so subsequent `user_channels()` reads see the membership.
        // Called from on_hello after a successful sign-in so the user
        // lands in `general` automatically on first login.
        bool add_user_to_channel(const std::string& channel_id, const std::string& user_id);


        // ---- friends / DMs (Part B) ----

        // Pending row insertion. Idempotent: re-sending the same request
        // just refreshes created_at. The recipient drives the request
        // lifecycle via accept_friend_request / delete_friend_request.
        bool insert_friend_request(const std::string& to_user_id, const std::string& from_user_id);

        // One pending entry as it lives in the DB.
        struct PendingFriendRequest
        {

            std::string from_user_id  { };
            std::int64_t created_at_ms { 0 };

        };

        // List every pending request currently waiting on the user.
        // Used for FetchFriends and at sign-in if we ever want to fire
        // a deferred FriendRequestEvent on Hello.
        std::vector<PendingFriendRequest> list_friend_requests(const std::string& to_user_id);

        // Remove a pending row. Called on both accept (after writing the
        // friendships rows) and outright reject. Idempotent — deleting a
        // row that isn't there is a successful no-op.
        bool delete_friend_request(const std::string& to_user_id, const std::string& from_user_id);

        // Write both directions of the friendship in one synchronous
        // batch. dm_channel_id is the same in both rows so either side
        // can derive it from one read.
        bool insert_friendship_pair(const std::string& user_a, const std::string& user_b, const std::string& dm_channel_id);

        // Friendship row as returned by list_friendships.
        struct Friendship
        {

            std::string friend_user_id { };
            std::string dm_channel_id  { };
            std::int64_t since_ms      { 0 };

        };

        std::vector<Friendship> list_friendships(const std::string& user_id);

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
        CassPreparedPtr m_prep_upsert_user { }; // INSERT INTO users (user_id, username, email, created_at) VALUES (?, ?, ?, toTimestamp(now()))
        CassPreparedPtr m_prep_upsert_user_email { }; // INSERT INTO users_by_email ...
        CassPreparedPtr m_prep_username_for { }; // SELECT username FROM users WHERE user_id = ?
        CassPreparedPtr m_prep_lookup_user_by_email { }; // SELECT user_id, username FROM users_by_email WHERE email = ?
        CassPreparedPtr m_prep_ensure_channel { }; // INSERT INTO channels (channel_id, name, kind) VALUES (?, ?, ?)
        CassPreparedPtr m_prep_add_channel_member { }; // INSERT INTO channel_members (channel_id, user_id, joined_at) VALUES (?, ?, toTimestamp(now()))
        CassPreparedPtr m_prep_add_user_channel { }; // INSERT INTO user_channels (user_id, channel_id, joined_at) VALUES (?, ?, toTimestamp(now()))
        CassPreparedPtr m_prep_insert_friend_request { }; // INSERT INTO friend_requests ...
        CassPreparedPtr m_prep_list_friend_requests { }; // SELECT from_user_id, created_at FROM friend_requests WHERE to_user_id = ?
        CassPreparedPtr m_prep_delete_friend_request { }; // DELETE FROM friend_requests WHERE to_user_id = ? AND from_user_id = ?
        CassPreparedPtr m_prep_insert_friendship { }; // INSERT INTO friendships ...
        CassPreparedPtr m_prep_list_friendships { }; // SELECT friend_user_id, dm_channel_id, since FROM friendships WHERE user_id = ?

    };

}

#endif // SIGNALING_SERVER_SCYLLA_CLIENT_HH
