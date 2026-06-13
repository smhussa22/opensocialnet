#ifndef SCREEN_CAPTURE_DELETERS_HH
#define SCREEN_CAPTURE_DELETERS_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <X11/Xlib.h>
extern "C"
{

#include <libswscale/swscale.h>

}

// project headers

namespace OpenSocialNet::Video
{

    struct X11DisplayDeleter
    {

        void operator()(::Display* display) const noexcept
        {

            if (display != nullptr) ::XCloseDisplay(display);

        }

    };

    struct SwsContextDeleter
    {

        void operator()(::SwsContext* ctx) const noexcept
        {

            if (ctx != nullptr) ::sws_freeContext(ctx);

        }

    };

    using X11DisplayPtr = std::unique_ptr<::Display, X11DisplayDeleter>;
    using SwsContextPtr = std::unique_ptr<::SwsContext, SwsContextDeleter>;

}

#endif // SCREEN_CAPTURE_DELETERS_HH
