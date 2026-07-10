// related headers
#include "ScyllaClient.hh"

// c sys headers
#include <cctype>
#include <cstdlib>

// cpp stdlib headers
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// 3rd party headers
#include <cassandra.h>

// project headers
#include "CassandraDeleters.hh"


namespace OpenSocialNet::Signaling
{

    namespace
    {

        [[noreturn]] void scylla_die(const char* what, ::CassFuture* f)
        {

            // msg points into the future's internal buffer -- borrowed view, so a
            // string_view is the right wrapper (not unique_ptr).
            const char* msg { nullptr };
            std::size_t msg_len { 0 };
            ::cass_future_error_message(f, &msg, &msg_len);
            std::cerr << what << " failed: " << std::string_view { msg, msg_len } << '\n';
            std::exit(1);

        }

        // Emails are the partition key of users_by_email — normalize case on
        // both write and read so "Foo@Gmail.com" typed into the search box
        // still hits the row Google reported as "foo@gmail.com".
        std::string lower_email(const std::string& email)
        {

            std::string out { email };
            while (!out.empty() and (out.front() == ' ' or out.front() == '\t')) out.erase(out.begin());
            while (!out.empty() and (out.back() == ' ' or out.back() == '\t')) out.pop_back();
            for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;

        }

    }

    void ScyllaClient::init(const std::string& contact_point)
    {

        m_cluster.reset(::cass_cluster_new());
        ::cass_cluster_set_contact_points(m_cluster.get(), contact_point.c_str());

        m_session.reset(::cass_session_new());
        m_uuid_gen.reset(::cass_uuid_gen_new());

        CassFuturePtr fut { ::cass_session_connect_keyspace(m_session.get(), m_cluster.get(), "opensocialnet") };
        if (::cass_future_error_code(fut.get()) != CASS_OK) scylla_die("scylla connect", fut.get());

        // Prepared statements: parsed once on Scylla, cached, reused per call.
        // Server keeps a md5 -> AST map; we just send the hash + bound params.
        m_prep_insert_message = prepare("INSERT INTO messages (channel_id, message_id, sender_id, content) VALUES (?, ?, ?, ?)");
        m_prep_fetch_history = prepare("SELECT message_id, sender_id, content FROM messages WHERE channel_id = ? AND message_id < ? LIMIT ?");
        m_prep_user_channels = prepare("SELECT channel_id FROM user_channels WHERE user_id = ?");
        // Idempotent: overwriting an existing row with the same primary
        // key just refreshes username + created_at, which is fine for our
        // "first login wins, every subsequent login is a no-op" semantics.
        m_prep_upsert_user        = prepare("INSERT INTO users (user_id, username, email, avatar_url, created_at) VALUES (?, ?, ?, ?, toTimestamp(now()))");
        m_prep_upsert_user_email  = prepare("INSERT INTO users_by_email (email, user_id, username) VALUES (?, ?, ?)");
        m_prep_username_for       = prepare("SELECT username FROM users WHERE user_id = ?");
        m_prep_avatar_for         = prepare("SELECT avatar_url FROM users WHERE user_id = ?");
        m_prep_lookup_user_by_email = prepare("SELECT user_id, username FROM users_by_email WHERE email = ?");

        m_prep_ensure_channel     = prepare("INSERT INTO channels (channel_id, name, kind) VALUES (?, ?, ?)");
        m_prep_list_channels      = prepare("SELECT channel_id, name, kind FROM channels");
        m_prep_add_channel_member = prepare("INSERT INTO channel_members (channel_id, user_id, joined_at) VALUES (?, ?, toTimestamp(now()))");
        m_prep_add_user_channel   = prepare("INSERT INTO user_channels (user_id, channel_id, joined_at) VALUES (?, ?, toTimestamp(now()))");

        m_prep_insert_friend_request = prepare("INSERT INTO friend_requests (to_user_id, from_user_id, created_at) VALUES (?, ?, toTimestamp(now()))");
        m_prep_list_friend_requests  = prepare("SELECT from_user_id, created_at FROM friend_requests WHERE to_user_id = ?");
        m_prep_delete_friend_request = prepare("DELETE FROM friend_requests WHERE to_user_id = ? AND from_user_id = ?");
        m_prep_insert_friendship     = prepare("INSERT INTO friendships (user_id, friend_user_id, dm_channel_id, since) VALUES (?, ?, ?, toTimestamp(now()))");
        m_prep_list_friendships      = prepare("SELECT friend_user_id, dm_channel_id, since FROM friendships WHERE user_id = ?");

    }

    CassPreparedPtr ScyllaClient::prepare(const char* cql)
    {

        CassFuturePtr f { ::cass_session_prepare(m_session.get(), cql) };
        if (::cass_future_error_code(f.get()) != CASS_OK) scylla_die("scylla prepare", f.get());
        return CassPreparedPtr { ::cass_future_get_prepared(f.get()) };

    }

    std::vector<std::string> ScyllaClient::user_channels(const std::string& user_id)
    {

        std::vector<std::string> out { };

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_user_channels.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, user_id.c_str());
        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());

        if (::cass_future_error_code(fut.get()) == CASS_OK)
        {

            CassResultPtr result { ::cass_future_get_result(fut.get()) };
            CassIteratorPtr it { ::cass_iterator_from_result(result.get()) };
            while (::cass_iterator_next(it.get()))
            {

                // row/value are borrowed from the iterator/result -- non-owning.
                const auto* row = ::cass_iterator_get_row(it.get());
                const char* s { nullptr };
                std::size_t len { 0 };
                ::cass_value_get_string(::cass_row_get_column(row, 0), &s, &len);
                out.emplace_back(s, len);

            }

        }

        return out;

    }

    ::CassSession* ScyllaClient::session()
    {

        return m_session.get();

    }

    ::CassUuidGen* ScyllaClient::uuid_gen()
    {

        return m_uuid_gen.get();

    }

    const ::CassPrepared* ScyllaClient::prep_insert_message()
    {

        return m_prep_insert_message.get();

    }

    const ::CassPrepared* ScyllaClient::prep_fetch_history()
    {

        return m_prep_fetch_history.get();

    }


    bool ScyllaClient::upsert_user(const std::string& user_id, const std::string& username, const std::string& email, const std::string& avatar_url)
    {

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_upsert_user.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, user_id.c_str());
        ::cass_statement_bind_string(stmt.get(), 1, username.c_str());
        ::cass_statement_bind_string(stmt.get(), 2, email.c_str());
        ::cass_statement_bind_string(stmt.get(), 3, avatar_url.c_str());

        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        if (::cass_future_error_code(fut.get()) != CASS_OK)
        {

            const char* msg { nullptr };
            std::size_t msg_len { 0 };
            ::cass_future_error_message(fut.get(), &msg, &msg_len);
            std::cerr << "[users] upsert " << user_id << " failed: " << std::string_view { msg, msg_len } << '\n';
            return false;

        }

        // Mirror into the email index when we have one. Empty email means
        // the user signed in without the email scope (or HMAC dev path);
        // no point indexing a blank key.
        if (!email.empty())
        {

            const std::string email_key { lower_email(email) };
            CassStatementPtr e_stmt { ::cass_prepared_bind(m_prep_upsert_user_email.get()) };
            ::cass_statement_bind_string(e_stmt.get(), 0, email_key.c_str());
            ::cass_statement_bind_string(e_stmt.get(), 1, user_id.c_str());
            ::cass_statement_bind_string(e_stmt.get(), 2, username.c_str());
            CassFuturePtr e_fut { ::cass_session_execute(m_session.get(), e_stmt.get()) };
            ::cass_future_wait(e_fut.get());
            if (::cass_future_error_code(e_fut.get()) != CASS_OK)
            {

                std::cerr << "[users] upsert email index for " << email << " failed (non-fatal)\n";
                // not fatal — main row landed, lookup-by-email will just miss

            }

        }
        return true;

    }


    ScyllaClient::UserByEmail ScyllaClient::lookup_user_by_email(const std::string& email)
    {

        UserByEmail out { };
        if (email.empty()) return out;

        const std::string email_key { lower_email(email) };
        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_lookup_user_by_email.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, email_key.c_str());
        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        if (::cass_future_error_code(fut.get()) != CASS_OK) return out;

        CassResultPtr result { ::cass_future_get_result(fut.get()) };
        const auto* row { ::cass_result_first_row(result.get()) };
        if (row == nullptr) return out;

        const char* s { nullptr };
        std::size_t len { 0 };
        ::cass_value_get_string(::cass_row_get_column(row, 0), &s, &len);
        out.user_id = std::string { s, len };
        ::cass_value_get_string(::cass_row_get_column(row, 1), &s, &len);
        out.username = std::string { s, len };
        out.email = email;
        return out;

    }


    std::string ScyllaClient::username_for(const std::string& user_id)
    {

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_username_for.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, user_id.c_str());

        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        if (::cass_future_error_code(fut.get()) != CASS_OK) return { };

        CassResultPtr result { ::cass_future_get_result(fut.get()) };
        const auto* row { ::cass_result_first_row(result.get()) };
        if (row == nullptr) return { };

        const char* s { nullptr };
        std::size_t len { 0 };
        ::cass_value_get_string(::cass_row_get_column(row, 0), &s, &len);
        return std::string { s, len };

    }


    std::string ScyllaClient::avatar_for(const std::string& user_id)
    {

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_avatar_for.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, user_id.c_str());

        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        if (::cass_future_error_code(fut.get()) != CASS_OK) return { };

        CassResultPtr result { ::cass_future_get_result(fut.get()) };
        const auto* row { ::cass_result_first_row(result.get()) };
        if (row == nullptr) return { };

        const char* s { nullptr };
        std::size_t len { 0 };
        if (::cass_value_get_string(::cass_row_get_column(row, 0), &s, &len) != CASS_OK) return { };
        return std::string { s, len };

    }


    bool ScyllaClient::ensure_channel(const std::string& channel_id, const std::string& name, const std::string& kind)
    {

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_ensure_channel.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, channel_id.c_str());
        ::cass_statement_bind_string(stmt.get(), 1, name.c_str());
        ::cass_statement_bind_string(stmt.get(), 2, kind.c_str());

        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        return ::cass_future_error_code(fut.get()) == CASS_OK;

    }


    bool ScyllaClient::add_user_to_channel(const std::string& channel_id, const std::string& user_id)
    {

        // Both denormalized indices get the same row. Cheap inline writes
        // (sub-ms each on local Scylla) so doing them sequentially on the
        // WS thread is fine.
        CassStatementPtr stmt_member { ::cass_prepared_bind(m_prep_add_channel_member.get()) };
        ::cass_statement_bind_string(stmt_member.get(), 0, channel_id.c_str());
        ::cass_statement_bind_string(stmt_member.get(), 1, user_id.c_str());
        CassFuturePtr fut_member { ::cass_session_execute(m_session.get(), stmt_member.get()) };
        ::cass_future_wait(fut_member.get());
        const bool ok_member { ::cass_future_error_code(fut_member.get()) == CASS_OK };

        CassStatementPtr stmt_user { ::cass_prepared_bind(m_prep_add_user_channel.get()) };
        ::cass_statement_bind_string(stmt_user.get(), 0, user_id.c_str());
        ::cass_statement_bind_string(stmt_user.get(), 1, channel_id.c_str());
        CassFuturePtr fut_user { ::cass_session_execute(m_session.get(), stmt_user.get()) };
        ::cass_future_wait(fut_user.get());
        const bool ok_user { ::cass_future_error_code(fut_user.get()) == CASS_OK };

        return ok_member and ok_user;

    }


    bool ScyllaClient::insert_friend_request(const std::string& to_user_id, const std::string& from_user_id)
    {

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_insert_friend_request.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, to_user_id.c_str());
        ::cass_statement_bind_string(stmt.get(), 1, from_user_id.c_str());

        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        return ::cass_future_error_code(fut.get()) == CASS_OK;

    }


    std::vector<ScyllaClient::PendingFriendRequest> ScyllaClient::list_friend_requests(const std::string& to_user_id)
    {

        std::vector<PendingFriendRequest> out { };

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_list_friend_requests.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, to_user_id.c_str());
        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        if (::cass_future_error_code(fut.get()) != CASS_OK) return out;

        CassResultPtr result { ::cass_future_get_result(fut.get()) };
        CassIteratorPtr it { ::cass_iterator_from_result(result.get()) };
        while (::cass_iterator_next(it.get()))
        {

            const auto* row { ::cass_iterator_get_row(it.get()) };

            const char* s { nullptr };
            std::size_t len { 0 };
            ::cass_value_get_string(::cass_row_get_column(row, 0), &s, &len);
            std::string from { s, len };

            ::cass_int64_t ts_ms { 0 };
            ::cass_value_get_int64(::cass_row_get_column(row, 1), &ts_ms);

            out.push_back(PendingFriendRequest { std::move(from), static_cast<std::int64_t>(ts_ms) });

        }
        return out;

    }


    bool ScyllaClient::delete_friend_request(const std::string& to_user_id, const std::string& from_user_id)
    {

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_delete_friend_request.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, to_user_id.c_str());
        ::cass_statement_bind_string(stmt.get(), 1, from_user_id.c_str());
        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        return ::cass_future_error_code(fut.get()) == CASS_OK;

    }


    bool ScyllaClient::insert_friendship_pair(const std::string& user_a, const std::string& user_b, const std::string& dm_channel_id)
    {

        // Forward + reverse rows. Same dm_channel_id in both — either
        // side can read just their own partition and know the channel.
        const auto write { [this](const std::string& uid, const std::string& fid, const std::string& dm)
        {

            CassStatementPtr stmt { ::cass_prepared_bind(m_prep_insert_friendship.get()) };
            ::cass_statement_bind_string(stmt.get(), 0, uid.c_str());
            ::cass_statement_bind_string(stmt.get(), 1, fid.c_str());
            ::cass_statement_bind_string(stmt.get(), 2, dm.c_str());
            CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
            ::cass_future_wait(fut.get());
            return ::cass_future_error_code(fut.get()) == CASS_OK;

        } };

        const bool ok_a { write(user_a, user_b, dm_channel_id) };
        const bool ok_b { write(user_b, user_a, dm_channel_id) };
        return ok_a and ok_b;

    }


    std::vector<ScyllaClient::ChannelRow> ScyllaClient::list_channels()
    {

        std::vector<ChannelRow> out { };

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_list_channels.get()) };
        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        if (::cass_future_error_code(fut.get()) != CASS_OK) return out;

        CassResultPtr result { ::cass_future_get_result(fut.get()) };
        CassIteratorPtr it { ::cass_iterator_from_result(result.get()) };
        while (::cass_iterator_next(it.get()))
        {

            const auto* row { ::cass_iterator_get_row(it.get()) };

            const char* s { nullptr };
            std::size_t len { 0 };
            ::cass_value_get_string(::cass_row_get_column(row, 0), &s, &len);
            std::string channel_id { s, len };

            ::cass_value_get_string(::cass_row_get_column(row, 1), &s, &len);
            std::string name { s, len };

            ::cass_value_get_string(::cass_row_get_column(row, 2), &s, &len);
            std::string kind { s, len };

            out.push_back(ChannelRow { std::move(channel_id), std::move(name), std::move(kind) });

        }
        return out;

    }


    std::vector<ScyllaClient::Friendship> ScyllaClient::list_friendships(const std::string& user_id)
    {

        std::vector<Friendship> out { };

        CassStatementPtr stmt { ::cass_prepared_bind(m_prep_list_friendships.get()) };
        ::cass_statement_bind_string(stmt.get(), 0, user_id.c_str());
        CassFuturePtr fut { ::cass_session_execute(m_session.get(), stmt.get()) };
        ::cass_future_wait(fut.get());
        if (::cass_future_error_code(fut.get()) != CASS_OK) return out;

        CassResultPtr result { ::cass_future_get_result(fut.get()) };
        CassIteratorPtr it { ::cass_iterator_from_result(result.get()) };
        while (::cass_iterator_next(it.get()))
        {

            const auto* row { ::cass_iterator_get_row(it.get()) };

            const char* s { nullptr };
            std::size_t len { 0 };
            ::cass_value_get_string(::cass_row_get_column(row, 0), &s, &len);
            std::string friend_id { s, len };

            ::cass_value_get_string(::cass_row_get_column(row, 1), &s, &len);
            std::string dm_channel { s, len };

            ::cass_int64_t since_ms { 0 };
            ::cass_value_get_int64(::cass_row_get_column(row, 2), &since_ms);

            out.push_back(Friendship { std::move(friend_id), std::move(dm_channel), static_cast<std::int64_t>(since_ms) });

        }
        return out;

    }

}
