#ifndef SCREEN_CAPTURE_DELETERS_HH
#define SCREEN_CAPTURE_DELETERS_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
extern "C"
{

#include <libswscale/swscale.h>

}

// X11 is Linux-only; on macOS/Windows the deleter (and the ScreenCapture
// class that uses it) stubs out — the whole screenshare feature is
// unavailable on those platforms.
#ifdef __linux__
#include <X11/Xlib.h>
#endif

// project headers

namespace OpenSocialNet::Video
{

#ifdef __linux__

    struct X11DisplayDeleter
    {

        void operator()(::Display* display) const noexcept
        {

            if (display != nullptr) ::XCloseDisplay(display);

        }

    };

    using X11DisplayPtr = std::unique_ptr<::Display, X11DisplayDeleter>;

#endif // __linux__

    struct SwsContextDeleter
    {

        void operator()(::SwsContext* ctx) const noexcept
        {

            if (ctx != nullptr) ::sws_freeContext(ctx);

        }

    };

    using SwsContextPtr = std::unique_ptr<::SwsContext, SwsContextDeleter>;

}

#endif // SCREEN_CAPTURE_DELETERS_HH
