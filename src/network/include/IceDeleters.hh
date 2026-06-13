#ifndef ICE_DELETERS_HH
#define ICE_DELETERS_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <agent.h>

// project headers

namespace OpenSocialNet::Network
{

    struct NiceAgentDeleter
    {

        void operator()(::NiceAgent* agent) const noexcept
        {

            if (agent != nullptr) ::g_object_unref(agent);

        }

    };

    struct GMainContextDeleter
    {

        void operator()(::GMainContext* context) const noexcept
        {

            if (context != nullptr) ::g_main_context_unref(context);

        }

    };

    struct GCharDeleter
    {

        void operator()(::gchar* str) const noexcept
        {

            if (str != nullptr) ::g_free(str);

        }

    };

    using NiceAgentPtr = std::unique_ptr<::NiceAgent, NiceAgentDeleter>;
    using GMainContextPtr = std::unique_ptr<::GMainContext, GMainContextDeleter>;
    using GCharPtr = std::unique_ptr<::gchar, GCharDeleter>;

}

#endif // ICE_DELETERS_HH
