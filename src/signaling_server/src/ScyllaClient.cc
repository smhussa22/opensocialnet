// related headers
#include "ScyllaClient.hh"

// c sys headers
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

}
