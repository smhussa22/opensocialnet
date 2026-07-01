// related headers

// c sys headers
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// 3rd party headers
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

// project headers
#include "Ipc.hh"
#include "OAuthClient.hh"

namespace
{

    // Window starts at this size; it IS resizable now since the new layout
    // has scrollable lists and a chat pane that benefit from screen real
    // estate. ImGui still pins the main window to the SDL viewport so
    // sub-regions reflow as the user resizes.
    constexpr int initial_window_width  { 1100 };
    constexpr int initial_window_height { 760 };

    constexpr float sidebar_width { 240.0f };

    constexpr Uint8 bg_r { 26 };
    constexpr Uint8 bg_g { 28 };
    constexpr Uint8 bg_b { 34 };

    constexpr std::size_t chat_input_capacity      { 512 };
    constexpr std::size_t chat_history_cap         { 1000 };
    constexpr std::size_t friend_input_capacity    { 128 };


    std::string env_str(const char* key, const std::string& default_val = "")
    {
        const char* val { std::getenv(key) };
        return val ? std::string { val } : default_val;
    }

    // HMAC-SHA256 token computation used only when OSN_GOOGLE_CLIENT_ID
    // is unset (dev / CI path). Production runs the OAuth flow instead.
    std::string compute_auth_token(const std::string& user, const std::string& secret)
    {
        const std::string cmd { "printf %s '" + user + "' | openssl dgst -sha256 -hmac '" + secret + "' | awk '{print $NF}'" };
        FILE* pipe { popen(cmd.c_str(), "r") };
        if (!pipe) return { };
        char buf[256] { };
        std::string token { };
        if (fgets(buf, sizeof buf, pipe)) token = buf;
        pclose(pipe);
        if (!token.empty() and token.back() == '\n') token.pop_back();
        return token;
    }

    pid_t network_pid { -1 };

    void cleanup_network()
    {
        if (network_pid > 0)
        {
            kill(network_pid, SIGTERM);
            waitpid(network_pid, nullptr, 0);
            network_pid = -1;
        }
    }


    // ----- shared UI state -----
    //
    // Reader thread writes; main (UI) thread reads. One mutex guards
    // everything — the data set is small and contention is one update
    // per envelope, ~100/sec worst case.

    struct ChatLine
    {

        std::string sender_id  { };
        std::string sender_name { }; // best-effort display name
        std::string content    { };

    };

    struct FriendEntry
    {

        std::string user_id        { };
        std::string username       { };
        std::string dm_channel_id  { };

    };

    struct PendingRequest
    {

        std::string from_user_id   { };
        std::string from_username  { };

    };

    // Last LookupUser response stashed for the Add Friend modal to show.
    // Empty user_id means either "no query in flight" or "no match" —
    // disambiguated by `looked_up`, set to true once any response lands.
    struct LookupResult
    {

        bool        looked_up { false };
        bool        found     { false };
        std::string user_id   { };
        std::string username  { };
        std::string email     { };

    };

    // Per-peer decoded video frame, shared between the video IPC reader thread
    // and the render (main) thread. The IPC thread writes packed YUV420P data;
    // the render thread picks up dirty frames and uploads them to SDL textures.
    struct VideoFrameStore
    {

        struct Frame
        {

            std::vector<std::uint8_t> yuv    { }; // packed YUV420P (Y then U then V, no padding)
            int                       width  { 0 };
            int                       height { 0 };
            bool                      is_screen { false };
            bool                      dirty  { false }; // new data since last render tick

        };

        std::mutex                                    mu     { };
        std::unordered_map<std::uint32_t, Frame>      frames { }; // peer_id -> latest frame

    };

    // SDL texture slot for one peer, owned by the render thread only.
    struct VideoTexEntry
    {

        SDL_Texture* texture { nullptr }; // SDL_PIXELFORMAT_IYUV, created lazily
        int          width   { 0 };
        int          height  { 0 };

    };


    struct AppState
    {

        std::mutex mu { };

        std::string self_user_id         { };
        std::string self_display         { };
        std::string active_channel       { "general" };
        std::string active_voice_channel { }; // which voice room we're in, empty = not in call
        std::vector<std::string>                              channels      { };
        std::vector<FriendEntry>                              friends_       { };
        std::vector<PendingRequest>                           pending        { };
        std::unordered_map<std::string, std::deque<ChatLine>> chat_history  { };
        LookupResult                                          last_lookup   { };

        // UI signal flags — toggled by the reader thread, consumed by the
        // UI thread when it next renders. Cheap booleans guard whether
        // the chat pane auto-scrolls to bottom on a new arrival.
        std::atomic<bool> chat_scroll_request { false };

    };

    void push_chat(AppState& s, const std::string& channel, ChatLine line)
    {

        std::scoped_lock<std::mutex> lock { s.mu };
        auto& dq { s.chat_history[channel] };
        if (dq.size() >= chat_history_cap) dq.pop_front();
        dq.push_back(std::move(line));
        s.chat_scroll_request.store(true, std::memory_order_release);

    }


    // Look up a display name for a peer user_id by checking the friends
    // list first (we have their username from FriendSummary). Falls back
    // to the user_id itself when we don't know them yet.
    std::string display_for(const AppState& s, const std::string& user_id)
    {

        for (const auto& f : s.friends_) if (f.user_id == user_id) return f.username;
        if (user_id == s.self_user_id) return s.self_display.empty() ? user_id : s.self_display;
        return user_id;

    }


    // Dispatch one inbound envelope from the network child. Runs on the
    // IPC reader thread. Only state mutations are guarded; everything
    // else is local to the lambda.
    void on_envelope_from_network(AppState& state, const ::signaling::Envelope& env)
    {

        switch (env.payload_case())
        {

            case ::signaling::Envelope::kReady:
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                state.channels.clear();
                for (const auto& c : env.ready().channel_ids()) state.channels.push_back(c);
                break;

            }

            case ::signaling::Envelope::kChatMessageEvent:
            {

                const auto& evt { env.chat_message_event() };

                // Filter wire-protocol cruft: ICE SDP exchange piggybacks
                // on the chat channel for SDP blobs, so the network child
                // republishes "OSN-ICE1|..." every 2s. Those are not user
                // chat — drop before they hit the history pane.
                if (evt.content().starts_with("OSN-ICE1|")) break;

                // Drop the gateway's reflection of our own send — we
                // already pushed a local echo to feel instant. Without
                // this every send would render twice.
                {

                    std::scoped_lock<std::mutex> lock { state.mu };
                    if (evt.sender_id() == state.self_user_id) break;

                }

                ChatLine line { };
                line.sender_id   = evt.sender_id();
                {

                    std::scoped_lock<std::mutex> lock { state.mu };
                    line.sender_name = display_for(state, evt.sender_id());

                }
                line.content = evt.content();
                push_chat(state, evt.channel_id(), std::move(line));
                break;

            }

            case ::signaling::Envelope::kFriendListResponse:
            {

                const auto& resp { env.friend_list_response() };
                std::scoped_lock<std::mutex> lock { state.mu };
                state.friends_.clear();
                for (const auto& f : resp.friends())
                {

                    state.friends_.push_back({ f.user_id(), f.username(), f.dm_channel_id() });

                }
                state.pending.clear();
                for (const auto& p : resp.incoming())
                {

                    state.pending.push_back({ p.from_user_id(), p.from_username() });

                }
                break;

            }

            case ::signaling::Envelope::kFriendRequestEvent:
            {

                const auto& evt { env.friend_request_event() };
                std::scoped_lock<std::mutex> lock { state.mu };
                state.pending.push_back({ evt.from_user_id(), evt.from_username() });
                break;

            }

            case ::signaling::Envelope::kFriendRequestAcceptedEvent:
            {

                const auto& evt { env.friend_request_accepted_event() };
                std::scoped_lock<std::mutex> lock { state.mu };
                state.friends_.push_back({ evt.user_id(), evt.username(), evt.dm_channel_id() });
                // The new DM is implicitly subscribed on the gateway —
                // pull it into the channel list locally so the sidebar
                // surfaces it without waiting for a re-Ready.
                state.channels.push_back(evt.dm_channel_id());
                break;

            }

            case ::signaling::Envelope::kLookupUserResponse:
            {

                const auto& resp { env.lookup_user_response() };
                std::scoped_lock<std::mutex> lock { state.mu };
                state.last_lookup.looked_up = true;
                state.last_lookup.found     = resp.found();
                state.last_lookup.user_id   = resp.user_id();
                state.last_lookup.username  = resp.username();
                state.last_lookup.email     = resp.email();
                break;

            }

            case ::signaling::Envelope::kHistoryResponse:
            {

                // Prepend history messages older than what we already have.
                // request_id echoes the channel_id we sent, so we know where
                // to slot the rows without a separate correlation map.
                const auto& resp { env.history_response() };
                if (resp.request_id().empty()) break;
                std::scoped_lock<std::mutex> lock { state.mu };
                auto& dq { state.chat_history[resp.request_id()] };
                for (int i { resp.msgs_size() - 1 }; i >= 0; --i)
                {

                    if (dq.size() >= chat_history_cap) break;
                    const auto& msg { resp.msgs(i) };
                    ChatLine line { };
                    line.sender_id   = msg.sender_id();
                    line.sender_name = display_for(state, msg.sender_id());
                    line.content     = msg.content();
                    dq.push_front(std::move(line));

                }
                break;

            }

            case ::signaling::Envelope::kError:
            {

                const auto& err { env.error() };
                std::fprintf(stderr, "[gui] gateway error %u: %s\n", err.code(), err.message().c_str());
                break;

            }

            default:
                break; // voice / heartbeat / unknown — ignored by the GUI

        }

    }

}


int main()
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ---- auth ----
    const std::string google_client_id     { env_str("OSN_GOOGLE_CLIENT_ID",     "") };
    const std::string google_client_secret { env_str("OSN_GOOGLE_CLIENT_SECRET", "") };
    const std::string signaling_host       { env_str("OSN_SIGNALING_HOST", "3.144.229.204") };
    const std::string room_name            { env_str("OSN_ROOM", "general") };

    std::string user_name  { };
    std::string auth_token { };
    std::string display_name { };

    if (!google_client_id.empty())
    {

        std::printf("native_client: starting Google sign-in (client_id=%s...)\n", google_client_id.substr(0, 12).c_str());
        const auto oauth { OpenSocialNet::NativeClient::google_login(google_client_id, google_client_secret) };
        if (!oauth.ok)
        {

            std::printf("native_client: Google sign-in failed: %s\n", oauth.error.c_str());
            return 1;

        }
        user_name    = oauth.sub;
        auth_token   = oauth.id_token;
        display_name = oauth.name.empty() ? (oauth.email.empty() ? oauth.sub : oauth.email) : oauth.name;
        std::printf("native_client: signed in as %s (sub=%s)\n", display_name.c_str(), oauth.sub.c_str());

    }
    else
    {

        user_name = env_str("OSN_USER", "alice");
        const std::string auth_secret { env_str("OPENSOCIALNET_AUTH_SECRET", "devsecret123") };
        auth_token = compute_auth_token(user_name, auth_secret);
        if (auth_token.empty())
        {

            std::printf("native_client: failed to compute HMAC auth token\n");
            return 1;

        }
        display_name = user_name;
        std::printf("native_client: HMAC dev auth as user=%s\n", user_name.c_str());

    }

    // ---- IPC pipe to the network child (signaling envelopes) ----
    int ipc_fds[2] { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ipc_fds) < 0)
    {

        std::perror("native_client: socketpair");
        return 1;

    }
    const int parent_ipc_fd { ipc_fds[0] };
    const int child_ipc_fd  { ipc_fds[1] };

    // ---- Video IPC pipe (raw YUV420P frames from network child to GUI) ----
    // Non-fatal if it fails — video tiles just won't appear.
    int video_fds[2] { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, video_fds) < 0)
        std::perror("native_client: video socketpair (non-fatal)");
    const int parent_video_fd { video_fds[0] };
    const int child_video_fd  { video_fds[1] };

    // ---- SDL + ImGui ----
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("native_client: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window { nullptr };
    SDL_Renderer* renderer { nullptr };
    const std::string window_title { "OpenSocialNet — " + display_name };
    if (!SDL_CreateWindowAndRenderer(window_title.c_str(), initial_window_width, initial_window_height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer))
    {
        std::printf("native_client: CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io { ImGui::GetIO() };
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // ---- spawn network child ----
    std::printf("native_client: spawning network client (user=%s, signaling=%s)\n", user_name.c_str(), signaling_host.c_str());

    network_pid = fork();
    if (network_pid == 0)
    {

        ::close(parent_ipc_fd);

        const std::string relay_host { signaling_host };
        const std::string local_port { "0" };

        setenv("OSN_SIGNALING_HOST", signaling_host.c_str(), 1);
        setenv("OSN_ROOM",           room_name.c_str(),      1);
        setenv("OSN_USER",           user_name.c_str(),      1);
        setenv("OSN_AUTH_TOKEN",     auth_token.c_str(),     1);
        setenv("OSN_LOCAL_PORT",     local_port.c_str(),     1);
        setenv("OSN_VIDEO",          "1",                    1);
        setenv("OSN_SCREEN",         "1",                    1);
        setenv("OSN_ICE",            "1",                    1);
        setenv("OSN_IPC_FD",         std::to_string(child_ipc_fd).c_str(), 1);
        // GUI parent renders video tiles; child runs headless decode+IPC.
        setenv("OSN_VIDEO_RENDER",   "0",                    1);
        if (child_video_fd >= 0) setenv("OSN_VIDEO_IPC_FD", std::to_string(child_video_fd).c_str(), 1);

        char cwd[1024] { };
        if (!getcwd(cwd, sizeof cwd))
        {
            std::printf("native_client: getcwd failed\n");
            exit(1);
        }
        std::string network_bin { std::string(cwd) + "/src/network/build/network" };
        execl(network_bin.c_str(), network_bin.c_str(), nullptr);
        std::printf("native_client: failed to exec %s: %m\n", network_bin.c_str());
        exit(1);

    }
    else if (network_pid < 0)
    {

        std::printf("native_client: fork failed\n");
        ::close(parent_ipc_fd);
        ::close(child_ipc_fd);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;

    }
    ::close(child_ipc_fd);  // parent doesn't need its end of the child's fd
    if (child_video_fd >= 0) ::close(child_video_fd);

    std::printf("native_client: network pid=%d\n", network_pid);

    // ---- shared state + IPC bridge ----
    AppState state { };
    {

        std::scoped_lock<std::mutex> lock { state.mu };
        state.self_user_id = user_name;
        state.self_display = display_name;

    }

    OpenSocialNet::Ipc::Channel ipc { };
    ipc.attach(parent_ipc_fd);
    ipc.start_reader([&state](const ::signaling::Envelope& env)
    {

        on_envelope_from_network(state, env);

    });

    // Request an initial friends snapshot once the child has had a moment
    // to handshake with the gateway. The gateway also pushes Ready over
    // IPC so we'll get the channel list before the user has time to
    // notice — this just fills the friends sidebar in parallel.
    std::thread bootstrap_thread { [&ipc]()
    {

        std::this_thread::sleep_for(std::chrono::milliseconds { 1500 });
        ::signaling::Envelope env { };
        env.mutable_fetch_friends();
        ipc.send_envelope(env);

    } };

    // ---- video frame store + IPC reader ----
    VideoFrameStore video_frames { };

    // Reads packed YUV420P frames from the network child over the video
    // socketpair. Each frame is 17-byte header + width*height*3/2 bytes.
    // Writes go into VideoFrameStore under its mutex; the render thread
    // creates/updates SDL textures from dirty entries each frame.
    std::thread video_reader_thread { };
    if (parent_video_fd >= 0)
    {

        video_reader_thread = std::thread { [parent_video_fd, &video_frames]()
        {

            constexpr std::uint8_t expected_magic[4] { 'O', 'V', 'F', '\0' };
            std::vector<std::uint8_t> frame_buf { };

            auto read_exact = [&](void* buf, std::size_t n) -> bool
            {
                auto* p { static_cast<std::uint8_t*>(buf) };
                std::size_t done { 0 };
                while (done < n)
                {
                    const ::ssize_t r { ::read(parent_video_fd, p + done, n - done) };
                    if (r <= 0) return false;
                    done += static_cast<std::size_t>(r);
                }
                return true;
            };

            while (true)
            {

                std::uint8_t hdr[17] { };
                if (!read_exact(hdr, sizeof hdr)) break;
                if (std::memcmp(hdr, expected_magic, 4) != 0) break;

                std::uint32_t peer_id { 0 };
                std::memcpy(&peer_id, hdr + 4, 4);
                const bool is_screen { (hdr[8] & 1) != 0 };
                std::uint16_t w { 0 }, h { 0 };
                std::memcpy(&w, hdr + 9,  2);
                std::memcpy(&h, hdr + 11, 2);
                std::uint32_t data_size { 0 };
                std::memcpy(&data_size, hdr + 13, 4);

                if (data_size == 0 || data_size > 4u * 1024u * 1024u) break;

                frame_buf.resize(data_size);
                if (!read_exact(frame_buf.data(), data_size)) break;

                std::scoped_lock<std::mutex> lock { video_frames.mu };
                auto& frame { video_frames.frames[peer_id] };
                frame.yuv       = frame_buf;
                frame.width     = static_cast<int>(w);
                frame.height    = static_cast<int>(h);
                frame.is_screen = is_screen;
                frame.dirty     = true;

            }

            std::printf("native_client: video IPC reader exiting\n");

        } };

    }

    // SDL textures for remote peers. Render-thread only — no mutex needed.
    std::unordered_map<std::uint32_t, VideoTexEntry> video_textures { };

    // ---- per-frame UI state ----
    bool audio_enabled       { true };
    bool video_enabled       { true };
    bool screenshare_enabled { false };

    char chat_draft[chat_input_capacity]      { };
    char add_friend_input[friend_input_capacity] { };
    bool show_add_friend_modal { false };
    bool show_requests_modal   { false };

    // Track which channel we last fetched history for; request once on switch.
    std::string last_history_channel { };

    auto send_chat_now = [&ipc, &state](const std::string& text)
    {

        std::string channel { };
        {

            std::scoped_lock<std::mutex> lock { state.mu };
            channel = state.active_channel;

        }
        if (channel.empty() or text.empty()) return;

        ::signaling::Envelope env { };
        auto* msg { env.mutable_send_message() };
        msg->set_channel_id(channel);
        msg->set_content(text);
        msg->set_client_nonce(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
        ipc.send_envelope(env);

        // Echo locally so the UI feels instant; the gateway will also fan
        // our own message back as ChatMessageEvent (and we'll dedup by the
        // local echo we already pushed).
        ChatLine line { };
        line.sender_id   = state.self_user_id;
        line.sender_name = state.self_display.empty() ? state.self_user_id : state.self_display;
        line.content     = text;
        push_chat(state, channel, std::move(line));

    };


    bool running { true };
    while (running)
    {

        SDL_Event event { };
        while (SDL_PollEvent(&event))
        {

            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED and event.window.windowID == SDL_GetWindowID(window)) running = false;

        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Single full-viewport window — the SDL window is the canvas.
        const ImGuiViewport* viewport { ImGui::GetMainViewport() };
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags main_flags
        {
            ImGuiWindowFlags_NoTitleBar
          | ImGuiWindowFlags_NoResize
          | ImGuiWindowFlags_NoMove
          | ImGuiWindowFlags_NoCollapse
          | ImGuiWindowFlags_NoBringToFrontOnFocus
          | ImGuiWindowFlags_NoNavFocus
        };

        if (ImGui::Begin("##main", nullptr, main_flags))
        {

            // ---- LEFT SIDEBAR ----
            ImGui::BeginChild("##sidebar", ImVec2 { sidebar_width, 0 }, true);

            ImGui::TextDisabled("Signed in as");
            ImGui::TextWrapped("%s", state.self_display.c_str());
            ImGui::Separator();

            // Friend requests inbox button. Pending count shown inline.
            {

                std::size_t pending_count { };
                {

                    std::scoped_lock<std::mutex> lock { state.mu };
                    pending_count = state.pending.size();

                }
                char btn[64] { };
                std::snprintf(btn, sizeof btn, "Requests%s", pending_count > 0 ? (" (" + std::to_string(pending_count) + ")").c_str() : "");
                if (ImGui::Button(btn, ImVec2 { -1, 0 })) show_requests_modal = true;

            }
            if (ImGui::Button("+ Add Friend", ImVec2 { -1, 0 })) show_add_friend_modal = true;

            ImGui::Separator();
            ImGui::TextDisabled("Channels");

            {

                std::scoped_lock<std::mutex> lock { state.mu };
                for (const auto& ch : state.channels)
                {

                    const bool selected { ch == state.active_channel };
                    if (ImGui::Selectable(ch.c_str(), selected)) state.active_channel = ch;

                }

            }

            ImGui::Separator();
            ImGui::TextDisabled("Friends");

            // Capture call/leave requests outside the lock so we can
            // send IPC envelopes without holding state.mu.
            std::string call_request  { };
            std::string leave_request { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                for (const auto& f : state.friends_)
                {

                    ImGui::PushID(f.user_id.c_str());
                    const bool sel     { f.dm_channel_id == state.active_channel };
                    const bool in_call { f.dm_channel_id == state.active_voice_channel };
                    const std::string label { f.username.empty() ? f.user_id : f.username };
                    if (ImGui::Selectable(label.c_str(), sel, 0, ImVec2 { sidebar_width - 75.0f, 0 })) state.active_channel = f.dm_channel_id;
                    ImGui::SameLine();
                    if (in_call) { if (ImGui::SmallButton("Leave")) leave_request = f.dm_channel_id; }
                    else         { if (ImGui::SmallButton("Call"))  call_request  = f.dm_channel_id; }
                    ImGui::PopID();

                }
                if (state.friends_.empty()) ImGui::TextDisabled("(none yet)");

            }

            if (!call_request.empty())
            {

                ::signaling::Envelope env { };
                env.mutable_join_voice()->set_channel_id(call_request);
                ipc.send_envelope(env);
                std::scoped_lock<std::mutex> lock { state.mu };
                state.active_voice_channel = call_request;

            }
            if (!leave_request.empty())
            {

                ::signaling::Envelope env { };
                env.mutable_leave_voice()->set_channel_id(leave_request);
                ipc.send_envelope(env);
                std::scoped_lock<std::mutex> lock { state.mu };
                state.active_voice_channel.clear();

            }

            ImGui::EndChild();

            // ---- MAIN PANE ----
            ImGui::SameLine();
            ImGui::BeginChild("##main_pane", ImVec2 { 0, 0 }, false);

            std::string active_channel { };
            std::string active_label    { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                active_channel = state.active_channel;
                for (const auto& f : state.friends_) if (f.dm_channel_id == active_channel) { active_label = "DM with " + (f.username.empty() ? f.user_id : f.username); break; }
                if (active_label.empty()) active_label = "#" + active_channel;

            }

            // Fetch history once when the user switches to a new channel.
            if (!active_channel.empty() && active_channel != last_history_channel)
            {

                last_history_channel = active_channel;
                ::signaling::Envelope env { };
                auto* hist { env.mutable_fetch_history() };
                hist->set_request_id(active_channel);
                hist->set_channel_id(active_channel);
                hist->set_limit(50);
                ipc.send_envelope(env);

            }

            // Upload any dirty video frames to SDL textures (render-thread only).
            {

                std::vector<std::pair<std::uint32_t, VideoFrameStore::Frame>> dirty { };
                {
                    std::scoped_lock<std::mutex> lock { video_frames.mu };
                    for (auto& [pid, frame] : video_frames.frames)
                    {
                        if (frame.dirty) { dirty.emplace_back(pid, frame); frame.dirty = false; }
                    }
                }
                for (auto& [pid, snap] : dirty)
                {
                    auto& entry { video_textures[pid] };
                    if (entry.texture == nullptr || entry.width != snap.width || entry.height != snap.height)
                    {
                        if (entry.texture) SDL_DestroyTexture(entry.texture);
                        entry.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                                          SDL_TEXTUREACCESS_STREAMING,
                                                          snap.width, snap.height);
                        entry.width  = snap.width;
                        entry.height = snap.height;
                    }
                    if (entry.texture && !snap.yuv.empty())
                    {
                        const int w { snap.width }, h { snap.height };
                        const std::uint8_t* y { snap.yuv.data() };
                        const std::uint8_t* u { y + static_cast<std::size_t>(w) * h };
                        const std::uint8_t* v { u + static_cast<std::size_t>(w / 2) * (h / 2) };
                        SDL_UpdateYUVTexture(entry.texture, nullptr, y, w, u, w / 2, v, w / 2);
                    }
                }

            }

            ImGui::Text("%s", active_label.c_str());
            ImGui::Separator();

            // Media buttons row.
            const float btn_w { (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f };
            if (ImGui::Button(audio_enabled ? "Mute" : "Unmute", ImVec2 { btn_w, 30 }))
            {
                audio_enabled = !audio_enabled;
                if (network_pid > 0) kill(network_pid, SIGUSR1);
            }
            ImGui::SameLine();
            if (ImGui::Button(video_enabled ? "Camera Off" : "Camera On", ImVec2 { btn_w, 30 }))
            {
                video_enabled = !video_enabled;
                if (network_pid > 0) kill(network_pid, SIGUSR2);
            }
            ImGui::SameLine();
            if (ImGui::Button(screenshare_enabled ? "Stop Share" : "Share Screen", ImVec2 { btn_w, 30 }))
            {
                screenshare_enabled = !screenshare_enabled;
                if (network_pid > 0) kill(network_pid, SIGURG);
            }

            ImGui::Separator();

            // Video tiles — one per remote peer's camera/screen stream.
            // Textures are keyed by peer_id; new sources appear automatically.
            if (!video_textures.empty())
            {

                constexpr float tile_h { 135.0f };
                if (ImGui::BeginChild("##video_tiles", ImVec2 { 0, tile_h + 8.0f }, false, ImGuiWindowFlags_HorizontalScrollbar))
                {

                    for (auto& [pid, entry] : video_textures)
                    {

                        if (entry.texture == nullptr) continue;
                        const float aspect { entry.height > 0 ? static_cast<float>(entry.width) / static_cast<float>(entry.height) : 1.333f };
                        ImGui::Image((ImTextureID)(uintptr_t)entry.texture, ImVec2 { tile_h * aspect, tile_h });
                        ImGui::SameLine();

                    }

                }
                ImGui::EndChild();
                ImGui::Separator();

            }

            // Chat history — scrollable, auto-snap to bottom on new arrival
            const float input_h { ImGui::GetFrameHeightWithSpacing() };
            const float chat_h  { ImGui::GetContentRegionAvail().y - input_h - ImGui::GetStyle().ItemSpacing.y };
            ImGui::BeginChild("##chat_history", ImVec2 { 0, chat_h }, true);
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                const auto it { state.chat_history.find(active_channel) };
                if (it != state.chat_history.end())
                {

                    for (const auto& l : it->second)
                    {

                        ImGui::TextDisabled("%s:", (l.sender_name.empty() ? l.sender_id : l.sender_name).c_str());
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", l.content.c_str());

                    }

                }
                else
                {

                    ImGui::TextDisabled("(no messages yet)");

                }

            }
            if (state.chat_scroll_request.exchange(false, std::memory_order_acq_rel)) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();

            // Chat input — Enter sends
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##chat_input", chat_draft, sizeof chat_draft, ImGuiInputTextFlags_EnterReturnsTrue))
            {

                if (chat_draft[0] != '\0')
                {

                    send_chat_now(chat_draft);
                    chat_draft[0] = '\0';

                }
                ImGui::SetKeyboardFocusHere(-1);

            }

            ImGui::EndChild();

        }
        ImGui::End();

        // ---- MODALS ----

        if (show_add_friend_modal)
        {

            ImGui::OpenPopup("Add Friend");
            // Clear the last lookup result so a stale match from a
            // previous open doesn't haunt the new search.
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                state.last_lookup = { };

            }
            add_friend_input[0]   = '\0';
            show_add_friend_modal = false;

        }
        if (ImGui::BeginPopupModal("Add Friend", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {

            ImGui::TextDisabled("Look up a friend by their Google account email.");
            ImGui::SetNextItemWidth(360);
            const bool enter_pressed { ImGui::InputText("##friend_email", add_friend_input, sizeof add_friend_input, ImGuiInputTextFlags_EnterReturnsTrue) };

            const bool search_clicked { ImGui::Button("Search", ImVec2 { 100, 0 }) };
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2 { 100, 0 })) ImGui::CloseCurrentPopup();

            if ((search_clicked or enter_pressed) and add_friend_input[0] != '\0')
            {

                ::signaling::Envelope env { };
                auto* lookup { env.mutable_lookup_user() };
                lookup->set_email(add_friend_input);
                lookup->set_request_id(std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
                ipc.send_envelope(env);
                std::scoped_lock<std::mutex> lock { state.mu };
                state.last_lookup = { }; // mark in-flight: looked_up stays false until response

            }

            ImGui::Separator();

            // Snapshot the lookup result so the modal can render it
            // without holding the lock across UI calls.
            LookupResult snap { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                snap = state.last_lookup;

            }

            if (!snap.looked_up)
            {

                ImGui::TextDisabled("(no result yet)");

            }
            else if (!snap.found)
            {

                ImGui::TextColored(ImVec4 { 1.0f, 0.5f, 0.3f, 1.0f }, "No user found with that email.");

            }
            else
            {

                ImGui::Text("%s", snap.username.empty() ? snap.user_id.c_str() : snap.username.c_str());
                ImGui::TextDisabled("%s", snap.email.c_str());
                if (ImGui::Button("Send Friend Request", ImVec2 { 200, 0 }))
                {

                    ::signaling::Envelope env { };
                    env.mutable_send_friend_request()->set_to_user_id(snap.user_id);
                    ipc.send_envelope(env);
                    {

                        std::scoped_lock<std::mutex> lock { state.mu };
                        state.last_lookup = { };

                    }
                    add_friend_input[0] = '\0';
                    ImGui::CloseCurrentPopup();

                }

            }

            ImGui::EndPopup();

        }

        if (show_requests_modal)
        {

            ImGui::OpenPopup("Friend Requests");
            show_requests_modal = false;

        }
        if (ImGui::BeginPopupModal("Friend Requests", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {

            std::vector<PendingRequest> snapshot { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                snapshot = state.pending;

            }
            if (snapshot.empty()) ImGui::TextDisabled("(none)");
            else
            {

                for (auto it { snapshot.begin() }; it != snapshot.end(); ++it)
                {

                    ImGui::PushID(it->from_user_id.c_str());
                    ImGui::TextUnformatted((it->from_username.empty() ? it->from_user_id : it->from_username).c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Accept"))
                    {

                        ::signaling::Envelope env { };
                        env.mutable_accept_friend_request()->set_from_user_id(it->from_user_id);
                        ipc.send_envelope(env);
                        std::scoped_lock<std::mutex> lock { state.mu };
                        std::erase_if(state.pending, [&](const PendingRequest& p) { return p.from_user_id == it->from_user_id; });

                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reject"))
                    {

                        ::signaling::Envelope env { };
                        env.mutable_reject_friend_request()->set_from_user_id(it->from_user_id);
                        ipc.send_envelope(env);
                        std::scoped_lock<std::mutex> lock { state.mu };
                        std::erase_if(state.pending, [&](const PendingRequest& p) { return p.from_user_id == it->from_user_id; });

                    }
                    ImGui::PopID();

                }

            }
            if (ImGui::Button("Close", ImVec2 { 100, 0 })) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();

        }

        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

    }

    if (bootstrap_thread.joinable()) bootstrap_thread.join();
    ipc.stop();
    cleanup_network();

    // Close parent video fd so the reader thread sees EOF and exits.
    if (parent_video_fd >= 0) ::close(parent_video_fd);
    if (video_reader_thread.joinable()) video_reader_thread.join();

    // Destroy SDL textures before the renderer is torn down.
    for (auto& [pid, entry] : video_textures) if (entry.texture) SDL_DestroyTexture(entry.texture);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;

}
