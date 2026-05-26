#ifndef CASSANDRA_DELETERS_HH
#define CASSANDRA_DELETERS_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <cassandra.h>

// project headers

namespace OpenSocialNet::Signaling
{

    struct CassClusterDeleter
    {

        void operator()(CassCluster* p) const noexcept
        {

            if (p != nullptr) cass_cluster_free(p);

        }

    };

    struct CassSessionDeleter
    {

        void operator()(CassSession* p) const noexcept
        {

            if (p != nullptr) cass_session_free(p);

        }

    };

    struct CassUuidGenDeleter
    {

        void operator()(CassUuidGen* p) const noexcept
        {

            if (p != nullptr) cass_uuid_gen_free(p);

        }

    };

    struct CassPreparedDeleter
    {

        void operator()(const CassPrepared* p) const noexcept
        {

            if (p != nullptr) cass_prepared_free(p);

        }

    };

    struct CassStatementDeleter
    {

        void operator()(CassStatement* p) const noexcept
        {

            if (p != nullptr) cass_statement_free(p);

        }

    };

    struct CassFutureDeleter
    {

        void operator()(CassFuture* p) const noexcept
        {

            if (p != nullptr) cass_future_free(p);

        }

    };

    struct CassResultDeleter
    {

        void operator()(const CassResult* p) const noexcept
        {

            if (p != nullptr) cass_result_free(p);

        }

    };

    struct CassIteratorDeleter
    {

        void operator()(CassIterator* p) const noexcept
        {

            if (p != nullptr) cass_iterator_free(p);

        }

    };

    using CassClusterPtr = std::unique_ptr<CassCluster, CassClusterDeleter>;
    using CassSessionPtr = std::unique_ptr<CassSession, CassSessionDeleter>;
    using CassUuidGenPtr = std::unique_ptr<CassUuidGen, CassUuidGenDeleter>;
    using CassPreparedPtr = std::unique_ptr<const CassPrepared, CassPreparedDeleter>;
    using CassStatementPtr = std::unique_ptr<CassStatement, CassStatementDeleter>;
    using CassFuturePtr = std::unique_ptr<CassFuture, CassFutureDeleter>;
    using CassResultPtr = std::unique_ptr<const CassResult, CassResultDeleter>;
    using CassIteratorPtr = std::unique_ptr<CassIterator, CassIteratorDeleter>;

}

#endif // CASSANDRA_DELETERS_HH
