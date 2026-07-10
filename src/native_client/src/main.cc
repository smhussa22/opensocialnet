// related headers

// c sys headers
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <memory>
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
#include <httplib.h>

// stb_image decodes the Google profile picture bytes (JPEG/PNG) we
// download over HTTPS; SDL3 has no image decoder of its own.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_FAILURE_STRINGS
#include <stb/stb_image.h>

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

    constexpr Uint8 bg_r { 30 }; // matches the theme's window bg (#1e1f22)
    constexpr Uint8 bg_g { 31 };
    constexpr Uint8 bg_b { 34 };

    constexpr std::size_t chat_input_capacity      { 512 };
    constexpr std::size_t chat_history_cap         { 1000 };
    constexpr std::size_t friend_input_capacity    { 128 };


    std::string env_str(const char* key, const std::string& default_val = "")
    {
        const char* val { std::getenv(key) };
        return val ? std::string { val } : default_val;
    }

    // On a HIGH_PIXEL_DENSITY (Retina) window the backbuffer is larger than the
    // logical window by the display scale, but the ImGui SDL_Renderer backend
    // emits geometry in logical coords while scaling clip rects by
    // io.DisplayFramebufferScale. Unless we set a matching render scale, the two
    // live in different coordinate spaces — widgets draw at 1x but are scissored
    // to a 2x rect and get clipped away, leaving only stray slivers on screen.
    // Sync the renderer scale to the framebuffer scale each frame (also renders
    // crisply at native resolution). Must run after ImGui_ImplSDL3_NewFrame,
    // which refreshes DisplayFramebufferScale, and before RenderDrawData.
    void sync_render_scale(SDL_Renderer* renderer)
    {
        const ImVec2 s { ImGui::GetIO().DisplayFramebufferScale };
        SDL_SetRenderScale(renderer, s.x, s.y);
    }

    // Discord-flavoured dark theme: near-black window, slightly lighter
    // panels, blurple accent. Called once after CreateContext; the login
    // screen and main UI both inherit it.
    void apply_theme()
    {

        ImGui::StyleColorsDark();
        ImGuiStyle& style { ImGui::GetStyle() };

        constexpr ImVec4 col_window  { 0.118f, 0.122f, 0.133f, 1.00f }; // #1e1f22 outermost background
        constexpr ImVec4 col_panel   { 0.169f, 0.176f, 0.192f, 1.00f }; // #2b2d31 sidebar / child panes
        constexpr ImVec4 col_field   { 0.192f, 0.200f, 0.220f, 1.00f }; // #313338 inputs / chat surface
        constexpr ImVec4 col_hover   { 0.239f, 0.251f, 0.275f, 1.00f }; // #3d4046 hovered rows
        constexpr ImVec4 col_accent  { 0.345f, 0.396f, 0.949f, 1.00f }; // #5865f2 blurple
        constexpr ImVec4 col_accent2 { 0.278f, 0.325f, 0.882f, 1.00f }; // #4753e1 pressed blurple
        constexpr ImVec4 col_text    { 0.949f, 0.953f, 0.961f, 1.00f }; // #f2f3f5 primary text
        constexpr ImVec4 col_muted   { 0.580f, 0.608f, 0.643f, 1.00f }; // #949ba4 secondary text

        ImVec4* c { style.Colors };
        c[ImGuiCol_Text] = col_text;
        c[ImGuiCol_TextDisabled] = col_muted;
        c[ImGuiCol_WindowBg] = col_window;
        c[ImGuiCol_ChildBg] = col_panel;
        c[ImGuiCol_PopupBg] = col_panel;
        c[ImGuiCol_Border] = ImVec4 { 0.0f, 0.0f, 0.0f, 0.35f };
        c[ImGuiCol_FrameBg] = col_field;
        c[ImGuiCol_FrameBgHovered] = col_hover;
        c[ImGuiCol_FrameBgActive] = col_hover;
        c[ImGuiCol_TitleBg] = col_window;
        c[ImGuiCol_TitleBgActive] = col_window;
        c[ImGuiCol_ScrollbarBg] = ImVec4 { 0.0f, 0.0f, 0.0f, 0.0f };
        c[ImGuiCol_ScrollbarGrab] = ImVec4 { 0.10f, 0.10f, 0.11f, 1.0f };
        c[ImGuiCol_ScrollbarGrabHovered] = col_hover;
        c[ImGuiCol_ScrollbarGrabActive] = col_hover;
        c[ImGuiCol_CheckMark] = col_accent;
        c[ImGuiCol_SliderGrab] = col_accent;
        c[ImGuiCol_SliderGrabActive] = col_accent2;
        c[ImGuiCol_Button] = col_accent;
        c[ImGuiCol_ButtonHovered] = ImVec4 { 0.408f, 0.455f, 0.957f, 1.0f };
        c[ImGuiCol_ButtonActive] = col_accent2;
        c[ImGuiCol_Header] = col_hover;
        c[ImGuiCol_HeaderHovered] = col_hover;
        c[ImGuiCol_HeaderActive] = col_accent2;
        c[ImGuiCol_Separator] = ImVec4 { 0.0f, 0.0f, 0.0f, 0.45f };
        c[ImGuiCol_ResizeGrip] = col_hover;
        c[ImGuiCol_ResizeGripHovered] = col_accent;
        c[ImGuiCol_ResizeGripActive] = col_accent2;
        c[ImGuiCol_TextSelectedBg] = ImVec4 { 0.345f, 0.396f, 0.949f, 0.35f };
        c[ImGuiCol_NavHighlight] = col_accent;
        c[ImGuiCol_ModalWindowDimBg] = ImVec4 { 0.0f, 0.0f, 0.0f, 0.60f };

        style.WindowRounding = 8.0f;
        style.ChildRounding = 8.0f;
        style.PopupRounding = 8.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.ScrollbarRounding = 9.0f;
        style.WindowPadding = ImVec2 { 10.0f, 10.0f };
        style.FramePadding = ImVec2 { 8.0f, 5.0f };
        style.ItemSpacing = ImVec2 { 8.0f, 6.0f };
        style.ScrollbarSize = 12.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        style.PopupBorderSize = 1.0f;

    }

    // Byte length of the UTF-8 code point starting with this lead byte —
    // initials/truncation must never split one.
    std::size_t utf8_cp_len(unsigned char c)
    {

        if ((c >> 5) == 0x6) return 2;
        if ((c >> 4) == 0xE) return 3;
        if ((c >> 3) == 0x1E) return 4;
        return 1;

    }

    // Avatar circle at an absolute screen position: the Google profile
    // picture when a texture exists for this user, else a hash-colored
    // disc with the user's initials (whole code points, so CJK renders).
    void draw_avatar_circle(ImDrawList* dl, const std::unordered_map<std::string, SDL_Texture*>& textures, const std::string& user_id, const std::string& label, ImVec2 center, float r)
    {

        SDL_Texture* avatar { nullptr };
        if (const auto it { textures.find(user_id) }; it != textures.end()) avatar = it->second;

        if (avatar != nullptr)
        {

            dl->AddImageRounded((ImTextureID)(uintptr_t)avatar,
                                { center.x - r, center.y - r }, { center.x + r, center.y + r },
                                { 0, 0 }, { 1, 1 }, IM_COL32_WHITE, r);
            return;

        }

        const std::size_t h { std::hash<std::string>{}(label) };
        const ImU32 bg { IM_COL32(
            static_cast<int>(50 + (h & 0x30)),
            static_cast<int>(80 + ((h >> 6) & 0x30)),
            static_cast<int>(140 + ((h >> 12) & 0x50)), 255) };
        dl->AddCircleFilled(center, r, bg);

        // First two code points, ASCII uppercased; multibyte glyphs are
        // copied whole so CJK/emoji names render.
        std::string init { };
        for (std::size_t i { 0 }, taken { 0 }; i < label.size() and taken < 2; ++taken)
        {

            const std::size_t n { std::min(utf8_cp_len(static_cast<unsigned char>(label[i])), label.size() - i) };
            if (n == 1) init.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(label[i]))));
            else init.append(label, i, n);
            i += n;

        }
        if (init.empty()) init = "?";
        const ImVec2 tsz { ImGui::CalcTextSize(init.c_str()) };
        dl->AddText({ center.x - tsz.x * 0.5f, center.y - tsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 230), init.c_str());

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


    // ----- login screen -----

    struct LoginOutcome
    {

        bool        ok           { false }; // false = user closed the window before signing in
        std::string user_name    { };       // gateway identity (google sub or dev username)
        std::string auth_token   { };       // JWT or HMAC token shipped in the Hello envelope
        std::string display_name { };       // what the UI shows for "you"

    };

    // Result slot shared with the detached Google sign-in thread. The
    // browser flow can outlive the login screen (user quits mid-flow),
    // so the slot lives in a shared_ptr the worker thread co-owns.
    struct GoogleLoginJob
    {

        std::mutex        mu        { };       // guards result
        std::atomic<bool> in_flight { false }; // browser flow currently running
        std::atomic<bool> done      { false }; // result below is valid
        OpenSocialNet::NativeClient::OAuthResult result { }; // filled by the worker thread

    };

    // Parses simple KEY=VALUE lines (comments/blanks skipped, optional
    // `export ` prefix and surrounding quotes stripped) and setenv()s
    // them WITHOUT overriding values already in the environment — a bare
    // launch picks up the OAuth ids, sourcing the file first still wins.
    void load_env_file(const char* path)
    {

        std::ifstream file { path };
        if (!file.is_open()) return;

        std::string line { };
        while (std::getline(file, line))
        {

            const auto first { line.find_first_not_of(" \t") };
            if (first == std::string::npos or line[first] == '#') continue;
            if (line.compare(first, 7, "export ") == 0) line.erase(first, 7);

            const auto eq { line.find('=', first) };
            if (eq == std::string::npos) continue;

            std::string key { line.substr(first, eq - first) };
            std::string val { line.substr(eq + 1) };
            while (!key.empty() and (key.back() == ' ' or key.back() == '\t')) key.pop_back();
            while (!val.empty() and (val.back() == ' ' or val.back() == '\t' or val.back() == '\r')) val.pop_back();
            if (val.size() >= 2 and (val.front() == '"' or val.front() == '\'') and val.back() == val.front()) val = val.substr(1, val.size() - 2);
            if (!key.empty()) setenv(key.c_str(), val.c_str(), 0);

        }

    }

    // Modal pre-app phase: draws a centered sign-in card and blocks until
    // the user authenticates or closes the window (ok=false). Google is
    // the primary path; an HMAC dev sign-in sits under a collapsing
    // header, expanded automatically when no OAuth client id is set.
    LoginOutcome run_login_screen(SDL_Window* window, SDL_Renderer* renderer, const std::string& google_client_id, const std::string& google_client_secret)
    {

        auto job { std::make_shared<GoogleLoginJob>() };

        char dev_user[64] { };
        std::snprintf(dev_user, sizeof dev_user, "%s", env_str("OSN_USER", "").c_str());
        std::string google_error { };
        std::string dev_error    { };

        while (true)
        {

            SDL_Event event { };
            while (SDL_PollEvent(&event))
            {

                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) return { };
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED and event.window.windowID == SDL_GetWindowID(window)) return { };

            }

            // harvest a finished Google flow before starting the frame —
            // returning here is safe (no ImGui frame in progress)
            if (job->done.exchange(false, std::memory_order_acq_rel))
            {

                std::scoped_lock<std::mutex> lock { job->mu };
                if (job->result.ok)
                {

                    LoginOutcome out { };
                    out.ok           = true;
                    out.user_name    = job->result.sub;
                    out.auth_token   = job->result.id_token;
                    out.display_name = job->result.name.empty() ? (job->result.email.empty() ? job->result.sub : job->result.email) : job->result.name;
                    return out;

                }
                google_error = job->result.error;

            }

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            LoginOutcome outcome { };

            const ImGuiViewport* viewport { ImGui::GetMainViewport() };
            ImGui::SetNextWindowPos(ImVec2 { viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f }, ImGuiCond_Always, ImVec2 { 0.5f, 0.5f });
            ImGui::SetNextWindowSize(ImVec2 { 420.0f, 0.0f });
            if (ImGui::Begin("##login", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse))
            {

                ImGui::SetWindowFontScale(1.5f);
                ImGui::TextUnformatted("OpenSocialNet");
                ImGui::SetWindowFontScale(1.0f);
                ImGui::TextDisabled("Sign in to continue");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (google_client_id.empty())
                {

                    ImGui::TextWrapped("Google sign-in unavailable: OSN_GOOGLE_CLIENT_ID is not set and .secrets/google_oauth.env was not found.");

                }
                else if (job->in_flight.load(std::memory_order_acquire))
                {

                    ImGui::TextDisabled("Waiting for the browser sign-in to finish...");

                }
                else if (ImGui::Button("Sign in with Google", ImVec2 { -1.0f, 36.0f }))
                {

                    google_error.clear();
                    job->in_flight.store(true, std::memory_order_release);
                    std::thread { [job, google_client_id, google_client_secret]()
                    {

                        auto res { OpenSocialNet::NativeClient::google_login(google_client_id, google_client_secret) };
                        {

                            std::scoped_lock<std::mutex> lock { job->mu };
                            job->result = std::move(res);

                        }
                        job->in_flight.store(false, std::memory_order_release);
                        job->done.store(true, std::memory_order_release);

                    } }.detach();

                }
                if (!google_error.empty()) ImGui::TextColored(ImVec4 { 1.0f, 0.5f, 0.3f, 1.0f }, "Sign-in failed: %s", google_error.c_str());

                ImGui::Spacing();

                if (ImGui::CollapsingHeader("Developer sign-in (HMAC)", google_client_id.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
                {

                    ImGui::TextDisabled("Local testing only — token = HMAC(username, shared secret).");
                    ImGui::SetNextItemWidth(-1.0f);
                    const bool enter_pressed { ImGui::InputTextWithHint("##dev_user", "username", dev_user, sizeof dev_user, ImGuiInputTextFlags_EnterReturnsTrue) };
                    if ((ImGui::Button("Sign in as dev user", ImVec2 { -1.0f, 0.0f }) or enter_pressed) and dev_user[0] != '\0')
                    {

                        const std::string token { compute_auth_token(dev_user, env_str("OPENSOCIALNET_AUTH_SECRET", "devsecret123")) };
                        if (token.empty()) dev_error = "failed to compute HMAC token (is openssl installed?)";
                        else
                        {

                            outcome.ok           = true;
                            outcome.user_name    = dev_user;
                            outcome.auth_token   = token;
                            outcome.display_name = dev_user;

                        }

                    }
                    if (!dev_error.empty()) ImGui::TextColored(ImVec4 { 1.0f, 0.5f, 0.3f, 1.0f }, "%s", dev_error.c_str());

                }

            }
            ImGui::End();

            ImGui::Render();
            sync_render_scale(renderer);
            SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);

            // dev sign-in resolves inside the frame — return only after
            // the frame is fully ended so the main loop starts clean
            if (outcome.ok) return outcome;

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


    // Decoded profile pictures fetched off-thread. The fetch worker fills
    // rgba; the render loop turns dirty entries into SDL textures, since
    // texture creation must happen on the render thread.
    struct AvatarStore
    {

        struct Entry
        {

            std::vector<std::uint8_t> rgba { }; // decoded RGBA pixels
            int  width  { 0 };     // decoded image width
            int  height { 0 };     // decoded image height
            bool dirty  { false }; // decoded but no texture yet
            bool failed { false }; // fetch/decode failed — initials fallback

        };

        std::mutex mu { };
        std::unordered_map<std::string, Entry> entries   { }; // user_id -> decoded state
        std::unordered_map<std::string, bool>  requested { }; // user_id -> fetch already launched

    };

    // Fire-and-forget HTTPS fetch + decode of one profile picture. The
    // store is static-lifetime so the detached worker can safely outlive
    // the frame that launched it.
    void request_avatar(AvatarStore& store, const std::string& user_id, const std::string& url)
    {

        if (url.empty() or user_id.empty()) return;
        {

            std::scoped_lock<std::mutex> lock { store.mu };
            if (store.requested.contains(user_id)) return;
            store.requested[user_id] = true;

        }

        std::thread { [&store, user_id, url]()
        {

            auto fail = [&]()
            {

                std::scoped_lock<std::mutex> lock { store.mu };
                store.entries[user_id].failed = true;

            };

            // Split "https://host/path" into origin + path for httplib.
            const std::size_t scheme_end { url.find("://") };
            if (scheme_end == std::string::npos) { fail(); return; }
            const std::size_t path_start { url.find('/', scheme_end + 3) };
            const std::string origin { path_start == std::string::npos ? url : url.substr(0, path_start) };
            const std::string path   { path_start == std::string::npos ? std::string { "/" } : url.substr(path_start) };

            ::httplib::Client client { origin };
            client.set_follow_location(true);
            client.set_connection_timeout(5);
            client.set_read_timeout(5);
            const auto res { client.Get(path) };
            if (!res or res->status != 200 or res->body.empty()) { fail(); return; }

            int w { 0 }, h { 0 }, comp { 0 };
            ::stbi_uc* pixels { ::stbi_load_from_memory(reinterpret_cast<const ::stbi_uc*>(res->body.data()), static_cast<int>(res->body.size()), &w, &h, &comp, 4) };
            if (pixels == nullptr or w <= 0 or h <= 0) { fail(); return; }

            {

                std::scoped_lock<std::mutex> lock { store.mu };
                auto& entry { store.entries[user_id] };
                entry.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
                entry.width  = w;
                entry.height = h;
                entry.dirty  = true;

            }
            ::stbi_image_free(pixels);

        } }.detach();

    }


    // Latest ClientStats tick from the network child, copied under the
    // AppState mutex for the overlay window to render.
    struct StatsSnapshot
    {

        bool                     valid { false }; // false until the first tick arrives
        ::signaling::ClientStats data  { };       // raw envelope payload from the last tick

    };


    struct AppState
    {

        std::mutex mu { };

        std::string self_user_id         { };
        std::string self_display         { };
        std::unordered_map<std::string, std::string> avatar_urls { }; // user_id -> Google pfp URL (self, friends, callers)
        std::string active_channel       { };
        std::string active_voice_channel { }; // which voice room we're in, empty = not in call
        std::vector<std::string>                              channels      { };
        std::vector<FriendEntry>                              friends_       { };
        std::vector<PendingRequest>                           pending        { };
        std::unordered_map<std::string, std::deque<ChatLine>> chat_history  { };
        LookupResult                                          last_lookup   { };
        StatsSnapshot                                         stats         { }; // latest network-child stats tick
        bool                                                  gateway_ready { false }; // true once the gateway's Ready envelope arrives via IPC

        // ---- call flow (Discord-style ring/join) ----
        struct IncomingCall
        {

            bool        active        { false }; // an unanswered ring is on screen
            std::string channel_id    { };       // voice room to join on accept
            std::string from_user_id  { };       // whom to send CallAccept/Decline back to
            std::string from_username { };       // display name for the ringing UI
            std::chrono::steady_clock::time_point started { }; // when the ring arrived (auto-dismiss)

        };
        IncomingCall incoming_call         { }; // set by kCallInvite on the reader thread
        std::string  outgoing_call_channel { }; // non-empty while we're ringing someone
        std::string  outgoing_call_user    { }; // whom we're ringing
        std::chrono::steady_clock::time_point outgoing_call_started { }; // when we started ringing (timeout)
        std::string  call_peer_user_id     { }; // other party while a call is live (CallEnd target)
        bool         call_accepted_flag    { false }; // reader thread saw CallAccept for our ring
        bool         call_ended_flag       { false }; // reader thread saw CallEnd for the live call
        std::string  call_status_line      { }; // transient status ("X declined the call")

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
                state.gateway_ready = true;
                state.channels.clear();
                for (const auto& c : env.ready().channel_ids()) state.channels.push_back(c);
                if (!env.ready().avatar_url().empty()) state.avatar_urls[state.self_user_id] = env.ready().avatar_url();
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
                if (!evt.sender_username().empty())
                {

                    line.sender_name = evt.sender_username();

                }
                else
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
                    if (!f.avatar_url().empty()) state.avatar_urls[f.user_id()] = f.avatar_url();

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

            case ::signaling::Envelope::kChannelJoinedEvent:
            {

                const auto& evt { env.channel_joined_event() };
                std::scoped_lock<std::mutex> lock { state.mu };
                bool known { false };
                for (const auto& ch : state.channels) if (ch == evt.channel_id()) { known = true; break; }
                if (!known) state.channels.push_back(evt.channel_id());
                state.active_channel = evt.channel_id();
                break;

            }

            case ::signaling::Envelope::kClientStats:
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                state.stats.valid = true;
                state.stats.data  = env.client_stats();
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
                    line.sender_name = !msg.sender_username().empty() ? msg.sender_username() : display_for(state, msg.sender_id());
                    line.content     = msg.content();
                    dq.push_front(std::move(line));

                }
                break;

            }

            case ::signaling::Envelope::kCallInvite:
            {

                const auto& inv { env.call_invite() };
                std::scoped_lock<std::mutex> lock { state.mu };
                state.incoming_call.active        = true;
                state.incoming_call.channel_id    = inv.channel_id();
                state.incoming_call.from_user_id  = inv.from_user_id();
                state.incoming_call.from_username = inv.from_username();
                state.incoming_call.started      = std::chrono::steady_clock::now();
                if (!inv.from_avatar_url().empty()) state.avatar_urls[inv.from_user_id()] = inv.from_avatar_url();
                break;

            }

            case ::signaling::Envelope::kCallAccept:
            {

                const auto& acc { env.call_accept() };
                std::scoped_lock<std::mutex> lock { state.mu };
                if (acc.channel_id() == state.outgoing_call_channel) state.call_accepted_flag = true;
                break;

            }

            case ::signaling::Envelope::kCallDecline:
            {

                const auto& dec { env.call_decline() };
                std::scoped_lock<std::mutex> lock { state.mu };
                if (dec.channel_id() == state.outgoing_call_channel)
                {

                    state.call_status_line = display_for(state, dec.from_user_id()) + " declined the call";
                    state.outgoing_call_channel.clear();
                    state.outgoing_call_user.clear();

                }
                break;

            }

            case ::signaling::Envelope::kCallEnd:
            {

                const auto& end { env.call_end() };
                std::scoped_lock<std::mutex> lock { state.mu };
                // Caller cancelled the ring before we answered.
                if (state.incoming_call.active and end.channel_id() == state.incoming_call.channel_id) state.incoming_call = { };
                // Other side hung up a live call.
                if (!state.active_voice_channel.empty() and end.channel_id() == state.active_voice_channel) state.call_ended_flag = true;
                // Or gave up on a ring we had outstanding (shouldn't happen — they'd decline — but be tolerant).
                if (end.channel_id() == state.outgoing_call_channel)
                {

                    state.outgoing_call_channel.clear();
                    state.outgoing_call_user.clear();
                    state.call_status_line = "Call ended";

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


// Font Awesome 6 Solid — UTF-8 encoded codepoints used for call control icons.
// Generated via: https://fontawesome.com/icons (solid free set)
constexpr const char* FA_MIC          { "\xef\x84\xb0" }; // U+F130 fa-microphone
constexpr const char* FA_MIC_SLASH    { "\xef\x84\xb1" }; // U+F131 fa-microphone-slash
constexpr const char* FA_HEADPHONES   { "\xef\x80\xa5" }; // U+F025 fa-headphones (deafen off)
constexpr const char* FA_VOLUME_XMRK  { "\xef\x9a\xa9" }; // U+F6A9 fa-volume-xmark (deafened)
constexpr const char* FA_VIDEO        { "\xef\x80\xbd" }; // U+F03D fa-video
constexpr const char* FA_VIDEO_SLASH  { "\xef\x93\xa2" }; // U+F4E2 fa-video-slash
constexpr const char* FA_DESKTOP      { "\xef\x84\x88" }; // U+F108 fa-desktop (screenshare)
constexpr const char* FA_PHONE_SLASH  { "\xef\x8f\x9d" }; // U+F3DD fa-phone-slash (hang up)


int main()
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ---- config ----
    // Pull the OAuth client id/secret out of .secrets/google_oauth.env so
    // a bare `./native_client` launch from the repo root gets Google
    // sign-in without the user remembering to source the file first.
    load_env_file(".secrets/google_oauth.env");

    const std::string google_client_id     { env_str("OSN_GOOGLE_CLIENT_ID",     "") };
    const std::string google_client_secret { env_str("OSN_GOOGLE_CLIENT_SECRET", "") };
    const std::string signaling_host       { env_str("OSN_SIGNALING_HOST", "3.144.229.204") };
    const std::string room_name            { env_str("OSN_ROOM", "") };

    // ---- SDL + ImGui ----
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("native_client: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window { nullptr };
    SDL_Renderer* renderer { nullptr };
    if (!SDL_CreateWindowAndRenderer("OpenSocialNet", initial_window_width, initial_window_height,
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

    apply_theme();

    // ---- font ----
    // Noto Sans (OFL) is the closest free match to Discord's gg sans.
    // Rasterize at display-scale-multiplied size and shrink back via
    // FontGlobalScale so glyphs stay crisp on HiDPI/Retina where the
    // render scale is synced to the framebuffer scale each frame.
    {

        const float display_scale { SDL_GetWindowDisplayScale(window) };
        const float scale { display_scale > 0.0f ? display_scale : 1.0f };
        const char* font_path { "src/native_client/assets/fonts/NotoSans-Regular.ttf" };
        if (std::ifstream { font_path, std::ios::binary })
        {

            // Default ranges stop at Latin Supplement, so names with
            // extended Latin / Greek / Cyrillic showed as missing glyphs.
            static ImVector<ImWchar> font_ranges { };
            ImFontGlyphRangesBuilder builder { };
            builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
            builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
            builder.AddRanges(io.Fonts->GetGlyphRangesGreek());
            static const ImWchar latin_ext[] { 0x0100, 0x024F, 0x1E00, 0x1EFF, 0x2018, 0x2019, 0 };
            builder.AddRanges(latin_ext);
            builder.BuildRanges(&font_ranges);
            io.Fonts->AddFontFromFileTTF(font_path, 16.0f * scale, nullptr, font_ranges.Data);
            io.FontGlobalScale = 1.0f / scale;

        }
        else std::printf("native_client: %s not found (run from the repo root) — using ImGui default font\n", font_path);

        // Merge Font Awesome 6 Solid icons into the same atlas so FA_* constants render as icons.
        const char* fa_path { "src/native_client/assets/fonts/fa-solid-900.ttf" };
        if (std::ifstream { fa_path, std::ios::binary })
        {

            static const ImWchar fa_ranges[] { 0xE000, 0xF8FF, 0 };
            ImFontConfig fa_cfg { };
            fa_cfg.MergeMode        = true;
            fa_cfg.PixelSnapH       = true;
            fa_cfg.GlyphMinAdvanceX = 14.0f;
            io.Fonts->AddFontFromFileTTF(fa_path, 16.0f * scale, &fa_cfg, fa_ranges);

        }

    }

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // ---- auth ----
    // OSN_DEV_AUTH=1 skips the login screen entirely (scripted/dev runs);
    // otherwise the login screen blocks until sign-in or window close.
    LoginOutcome login { };
    if (env_str("OSN_DEV_AUTH", "0") == "1")
    {

        login.user_name    = env_str("OSN_USER", "alice");
        login.auth_token   = compute_auth_token(login.user_name, env_str("OPENSOCIALNET_AUTH_SECRET", "devsecret123"));
        login.display_name = login.user_name;
        login.ok           = !login.auth_token.empty();
        if (login.ok) std::printf("native_client: HMAC dev auth as user=%s\n", login.user_name.c_str());
        else std::printf("native_client: failed to compute HMAC auth token\n");

    }
    else
    {

        login = run_login_screen(window, renderer, google_client_id, google_client_secret);

    }

    if (!login.ok)
    {

        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;

    }

    const std::string user_name    { login.user_name };
    const std::string auth_token   { login.auth_token };
    const std::string display_name { login.display_name };
    SDL_SetWindowTitle(window, ("OpenSocialNet — " + display_name).c_str());
    std::printf("native_client: signed in as %s (user_id=%s)\n", display_name.c_str(), user_name.c_str());

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

    // ---- spawn network child ----
    std::printf("native_client: spawning network client (user=%s, signaling=%s)\n", user_name.c_str(), signaling_host.c_str());

    network_pid = fork();
    if (network_pid == 0)
    {

        ::close(parent_ipc_fd);

        // Redirect the child's stdout/stderr into logs/ so codec and
        // signaling chatter never reaches the user's terminal; the CSV
        // benchmarks the child writes already live in the same folder.
        ::mkdir("logs", 0755);
        {

            char ts_buf[32] { };
            const std::time_t now { std::time(nullptr) };
            std::strftime(ts_buf, sizeof ts_buf, "%Y%m%d_%H%M%S", std::localtime(&now));
            const std::string log_path { std::string { "logs/" } + ts_buf + "_" + user_name + "_network.log" };
            const int log_fd { ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644) };
            if (log_fd >= 0)
            {

                ::dup2(log_fd, STDOUT_FILENO);
                ::dup2(log_fd, STDERR_FILENO);
                if (log_fd > STDERR_FILENO) ::close(log_fd);

            }

        }

        const std::string relay_host { signaling_host };
        const std::string local_port { "0" };

        // Idle relay room is per-user: there is no channel concept anymore,
        // so idle clients must never share a media room. Real rooms are
        // joined dynamically when a call starts (JoinVoice over IPC).
        const std::string idle_room { room_name.empty() ? "idle:" + user_name : room_name };
        setenv("OSN_SIGNALING_HOST", signaling_host.c_str(), 1);
        setenv("OSN_ROOM",           idle_room.c_str(),      1);
        setenv("OSN_USER",           user_name.c_str(),      1);
        setenv("OSN_AUTH_TOKEN",     auth_token.c_str(),     1);
        setenv("OSN_LOCAL_PORT",     local_port.c_str(),     1);
        setenv("OSN_VIDEO",          "1",                    1);
        setenv("OSN_SCREEN",         "1",                    1);
        // Discord-style: no media flows until the user actually joins a
        // call. The child starts muted with the camera off; the GUI
        // unmutes via SIGUSR1 once a call is accepted.
        setenv("OSN_START_MUTED",    "1",                    1);
        setenv("OSN_VIDEO_START",    "0",                    1);
        // Relay path by default: the startup ICE dance would block the
        // capture loop (and stats ticks) up to 60s waiting for peer SDP
        // in the initial room, but GUI voice rooms are joined dynamically
        // later anyway. OSN_ICE=1 in the parent env still opts in.
        setenv("OSN_ICE",            "0",                    1); // always off from GUI; run network directly for ICE testing
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

                // Resync instead of dying: if the stream ever desyncs,
                // slide byte-by-byte until the 'OVF' magic lines up again.
                if (std::memcmp(hdr, expected_magic, 4) != 0)
                {

                    bool eof { false };
                    while (std::memcmp(hdr, expected_magic, 4) != 0)
                    {

                        std::memmove(hdr, hdr + 1, sizeof(hdr) - 1);
                        if (!read_exact(hdr + sizeof(hdr) - 1, 1)) { eof = true; break; }

                    }
                    if (eof) break;

                }

                std::uint32_t peer_id { 0 };
                std::memcpy(&peer_id, hdr + 4, 4);
                const bool is_screen { (hdr[8] & 1) != 0 };
                std::uint16_t w { 0 }, h { 0 };
                std::memcpy(&w, hdr + 9,  2);
                std::memcpy(&h, hdr + 11, 2);
                std::uint32_t data_size { 0 };
                std::memcpy(&data_size, hdr + 13, 4);

                // Insane size means we latched onto stray bytes that merely
                // looked like magic — skip this header and resync again.
                if (data_size == 0 or data_size > 8u * 1024u * 1024u) continue;
                if (data_size != static_cast<std::uint32_t>(w) * h * 3u / 2u) continue;

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

    // Profile pictures: static so detached fetch workers can safely
    // outlive the render loop; textures are render-thread only.
    static AvatarStore avatar_store { };
    std::unordered_map<std::string, SDL_Texture*> avatar_textures { };

    // ---- per-frame UI state ----
    // All media starts OFF — the network child is launched muted
    // (OSN_START_MUTED / OSN_VIDEO_START) and only a joined call flips
    // the mic on. These mirror the child's toggle state 1:1.
    bool audio_enabled       { false };
    bool video_enabled       { false };
    bool screenshare_enabled { false };
    bool deafened            { false }; // true when user has silenced incoming audio

    char chat_draft[chat_input_capacity]      { };
    char add_friend_input[friend_input_capacity] { };
    char new_group_input[friend_input_capacity] { };
    char screen_window_input[32] { };
    bool show_add_friend_modal   { false };
    bool show_requests_modal     { false };
    bool show_new_group_modal { false };
    bool show_camera_modal       { false };
    bool show_screen_modal       { false };
    bool show_stats_overlay      { false };

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

    // Join the voice room and open the mic. Called only once a call is
    // actually established (we accepted, or our ring was accepted).
    auto start_call_media = [&](const std::string& channel)
    {

        ::signaling::Envelope env { };
        env.mutable_join_voice()->set_channel_id(channel);
        ipc.send_envelope(env);
        if (!audio_enabled and network_pid > 0) { kill(network_pid, SIGUSR1); audio_enabled = true; }

    };

    // Tear down all media: leave the voice room, close the mic, and kill
    // camera/screenshare if they were live. Idempotent.
    auto stop_call_media = [&]()
    {

        std::string channel { };
        {

            std::scoped_lock<std::mutex> lock { state.mu };
            channel = state.active_voice_channel;
            state.active_voice_channel.clear();
            state.call_peer_user_id.clear();

        }
        if (!channel.empty())
        {

            ::signaling::Envelope env { };
            env.mutable_leave_voice()->set_channel_id(channel);
            ipc.send_envelope(env);

        }
        if (audio_enabled and network_pid > 0)       { kill(network_pid, SIGUSR1); audio_enabled = false; }
        if (video_enabled and network_pid > 0)       { kill(network_pid, SIGUSR2); video_enabled = false; }
        if (screenshare_enabled and network_pid > 0) { kill(network_pid, SIGURG);  screenshare_enabled = false; }

    };


    bool running { true };
    bool network_child_alive { true };
    while (running)
    {

        SDL_Event event { };
        while (SDL_PollEvent(&event))
        {

            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED and event.window.windowID == SDL_GetWindowID(window)) running = false;

        }

        // A dead network child means no gateway, no voice, no video — the
        // failure mode is otherwise silent (exec failure prints only to the
        // launching terminal), so poll and surface it in the sidebar.
        if (network_child_alive and network_pid > 0 and waitpid(network_pid, nullptr, WNOHANG) == network_pid)
        {

            network_child_alive = false;
            network_pid = -1;
            std::printf("native_client: network child exited — gateway offline\n");

        }

        // Caller side: our ring was accepted — join the voice room now.
        {

            bool accepted { false };
            std::string acc_channel { };
            std::string acc_peer { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                if (state.call_accepted_flag)
                {

                    accepted    = true;
                    acc_channel = state.outgoing_call_channel;
                    acc_peer    = state.outgoing_call_user;
                    state.call_accepted_flag = false;
                    state.outgoing_call_channel.clear();
                    state.outgoing_call_user.clear();

                }

            }
            if (accepted)
            {

                start_call_media(acc_channel);
                std::scoped_lock<std::mutex> lock { state.mu };
                state.active_voice_channel = acc_channel;
                state.call_peer_user_id    = acc_peer;
                state.active_channel       = acc_channel;
                state.call_status_line.clear();

            }

        }

        // Either side: the other party hung up — tear our media down.
        {

            bool ended { false };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                if (state.call_ended_flag) { ended = true; state.call_ended_flag = false; }

            }
            if (ended)
            {

                stop_call_media();
                std::scoped_lock<std::mutex> lock { state.mu };
                state.call_status_line = "Call ended";

            }

        }

        // Ring timeouts: an unanswered outgoing ring auto-cancels after 30s
        // (CallEnd tells the callee to stop ringing); a stale incoming ring
        // modal dismisses itself after 45s in case the caller's CallEnd was
        // lost.
        {

            const auto now { std::chrono::steady_clock::now() };
            std::string timeout_channel { };
            std::string timeout_user    { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                if (!state.outgoing_call_channel.empty() and now - state.outgoing_call_started > std::chrono::seconds { 30 })
                {

                    timeout_channel = state.outgoing_call_channel;
                    timeout_user    = state.outgoing_call_user;
                    state.outgoing_call_channel.clear();
                    state.outgoing_call_user.clear();
                    state.call_status_line = "No answer";

                }
                if (state.incoming_call.active and now - state.incoming_call.started > std::chrono::seconds { 45 }) state.incoming_call = { };

            }
            if (!timeout_channel.empty())
            {

                ::signaling::Envelope env { };
                auto* end { env.mutable_call_end() };
                end->set_channel_id(timeout_channel);
                end->set_to_user_id(timeout_user);
                ipc.send_envelope(env);

            }

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

            // Self header: avatar + name, status dot underneath (green once
            // Ready arrives, yellow while handshaking, red if the child died).
            {

                bool ready { };
                {

                    std::scoped_lock<std::mutex> lock { state.mu };
                    ready = state.gateway_ready;

                }

                ImDrawList* dl { ImGui::GetWindowDrawList() };
                const float av_d { 34.0f };
                const ImVec2 pos { ImGui::GetCursorScreenPos() };
                draw_avatar_circle(dl, avatar_textures, state.self_user_id, state.self_display, ImVec2 { pos.x + av_d * 0.5f, pos.y + av_d * 0.5f }, av_d * 0.5f);
                ImGui::Dummy(ImVec2 { av_d, av_d });
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::Text("%s", state.self_display.c_str());

                ImU32 dot_col { };
                const char* dot_label { };
                if (!network_child_alive) { dot_col = IM_COL32(237, 66, 69, 255); dot_label = "Offline"; }
                else if (ready)           { dot_col = IM_COL32(59, 165, 93, 255); dot_label = "Online"; }
                else                      { dot_col = IM_COL32(240, 178, 50, 255); dot_label = "Connecting..."; }
                const float lh { ImGui::GetTextLineHeight() };
                const ImVec2 dp { ImGui::GetCursorScreenPos() };
                dl->AddCircleFilled(ImVec2 { dp.x + 5.0f, dp.y + lh * 0.55f }, 4.5f, dot_col);
                ImGui::Dummy(ImVec2 { 13.0f, lh });
                ImGui::SameLine();
                ImGui::TextDisabled("%s", dot_label);
                ImGui::EndGroup();

            }
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
            ImGui::TextDisabled("Group Chats");

            {

                std::scoped_lock<std::mutex> lock { state.mu };
                bool any_group { false };
                for (const auto& ch : state.channels)
                {

                    if (ch.starts_with("dm:")) continue; // DMs render in the Friends list below
                    any_group = true;
                    const bool selected { ch == state.active_channel };
                    if (ImGui::Selectable(ch.c_str(), selected)) state.active_channel = ch;

                }
                if (!any_group) ImGui::TextDisabled("No group chats yet.");

            }
            if (ImGui::Button("+ New Group Chat", ImVec2 { -1, 0 })) show_new_group_modal = true;

            ImGui::Separator();
            ImGui::TextDisabled("Friends");

            // Capture ring/cancel/hangup requests outside the lock so we
            // can send IPC envelopes without holding state.mu.
            std::string ring_channel   { };
            std::string ring_user      { };
            std::string cancel_channel { };
            std::string cancel_user    { };
            std::string hangup_channel { };
            std::string hangup_peer    { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                for (const auto& f : state.friends_)
                {

                    ImGui::PushID(f.user_id.c_str());
                    const bool sel     { f.dm_channel_id == state.active_channel };
                    const bool in_call { f.dm_channel_id == state.active_voice_channel };
                    const bool ringing { f.dm_channel_id == state.outgoing_call_channel };
                    const std::string label { f.username.empty() ? f.user_id : f.username };
                    const float av_d { 20.0f };
                    const ImVec2 fp { ImGui::GetCursorScreenPos() };
                    draw_avatar_circle(ImGui::GetWindowDrawList(), avatar_textures, f.user_id, label, ImVec2 { fp.x + av_d * 0.5f, fp.y + av_d * 0.5f }, av_d * 0.5f);
                    ImGui::Dummy(ImVec2 { av_d, av_d });
                    ImGui::SameLine();
                    if (ImGui::Selectable(label.c_str(), sel, 0, ImVec2 { sidebar_width - 100.0f, 0 })) state.active_channel = f.dm_channel_id;
                    ImGui::SameLine();
                    if (in_call)      { if (ImGui::SmallButton("Hang Up")) { hangup_channel = f.dm_channel_id; hangup_peer = state.call_peer_user_id; } }
                    else if (ringing) { if (ImGui::SmallButton("Cancel"))  { cancel_channel = f.dm_channel_id; cancel_user = f.user_id; } }
                    else              { if (ImGui::SmallButton("Call"))    { ring_channel = f.dm_channel_id; ring_user = f.user_id; } }
                    ImGui::PopID();

                }
                if (state.friends_.empty()) ImGui::TextDisabled("No friends yet.\nUse + Add Friend to\nfind people by email.");

            }

            if (!ring_channel.empty())
            {

                ::signaling::Envelope env { };
                auto* inv { env.mutable_call_invite() };
                inv->set_channel_id(ring_channel);
                inv->set_to_user_id(ring_user);
                ipc.send_envelope(env);
                std::scoped_lock<std::mutex> lock { state.mu };
                state.outgoing_call_channel = ring_channel;
                state.outgoing_call_user    = ring_user;
                state.outgoing_call_started = std::chrono::steady_clock::now();
                state.call_status_line.clear();

            }
            if (!cancel_channel.empty())
            {

                ::signaling::Envelope env { };
                auto* end { env.mutable_call_end() };
                end->set_channel_id(cancel_channel);
                end->set_to_user_id(cancel_user);
                ipc.send_envelope(env);
                std::scoped_lock<std::mutex> lock { state.mu };
                state.outgoing_call_channel.clear();
                state.outgoing_call_user.clear();

            }
            if (!hangup_channel.empty())
            {

                ::signaling::Envelope env { };
                auto* end { env.mutable_call_end() };
                end->set_channel_id(hangup_channel);
                end->set_to_user_id(hangup_peer);
                ipc.send_envelope(env);
                stop_call_media();

            }

            ImGui::Separator();
            ImGui::Checkbox("Stats overlay", &show_stats_overlay);

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
                if (active_label.empty()) active_label = active_channel;

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

            // Kick off profile picture fetches for anyone we have a URL
            // for, and turn freshly decoded ones into SDL textures.
            {

                std::vector<std::pair<std::string, std::string>> wanted { };
                {

                    std::scoped_lock<std::mutex> lock { state.mu };
                    for (const auto& [uid, url] : state.avatar_urls) wanted.emplace_back(uid, url);

                }
                for (const auto& [uid, url] : wanted) request_avatar(avatar_store, uid, url);

                std::scoped_lock<std::mutex> lock { avatar_store.mu };
                for (auto& [uid, entry] : avatar_store.entries)
                {

                    if (!entry.dirty) continue;
                    entry.dirty = false;
                    auto& tex { avatar_textures[uid] };
                    if (tex != nullptr) { SDL_DestroyTexture(tex); tex = nullptr; }
                    tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, entry.width, entry.height);
                    if (tex != nullptr) SDL_UpdateTexture(tex, nullptr, entry.rgba.data(), entry.width * 4);

                }

            }

            ImGui::Text("%s", active_label.empty() ? "Home" : active_label.c_str());
            ImGui::Separator();

            // Snapshot call state for this frame.
            std::string in_call_channel { };
            std::string in_call_peer_id { };
            std::string in_call_peer_label { };
            std::string ringing_label { };
            std::string status_line { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                in_call_channel = state.active_voice_channel;
                in_call_peer_id = state.call_peer_user_id;
                if (!state.call_peer_user_id.empty()) in_call_peer_label = display_for(state, state.call_peer_user_id);
                if (!state.outgoing_call_user.empty()) ringing_label = display_for(state, state.outgoing_call_user);
                status_line = state.call_status_line;

            }

            // ---- Discord-style call view (only while a live call exists) ----
            if (!in_call_channel.empty())
            {

                constexpr float tile_sz    { 96.0f };
                constexpr float ctrl_bar_h { 56.0f };

                // ---- Participant + video tiles area -------------------------
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4 { 0.08f, 0.08f, 0.11f, 1.0f });
                if (ImGui::BeginChild("##call_tiles",
                    ImVec2 { 0, tile_sz + 32.0f }, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_HorizontalScrollbar))
                {

                    const ImVec2 origin { ImGui::GetCursorScreenPos() };
                    ImDrawList* dl      { ImGui::GetWindowDrawList() };

                    // Draw an avatar circle tile at screen position (origin.x + x_off, origin.y + 4).
                    auto draw_tile = [&](float x_off, const std::string& user_id, const std::string& label, bool speaking, bool muted)
                    {

                        const ImVec2 pos { origin.x + x_off, origin.y + 4.0f };
                        const float cx   { pos.x + tile_sz * 0.5f };
                        const float cy   { pos.y + tile_sz * 0.5f };
                        const float r    { tile_sz * 0.44f };

                        if (speaking)
                            dl->AddCircle({ cx, cy }, r + 3.5f, IM_COL32(59, 165, 93, 220), 48, 3.0f);

                        draw_avatar_circle(dl, avatar_textures, user_id, label, ImVec2 { cx, cy }, r);

                        if (muted)
                            dl->AddCircleFilled({ cx + r * 0.65f, cy + r * 0.65f }, 7.0f,
                                                IM_COL32(237, 66, 69, 255));

                        std::string disp { label };
                        if (label.size() > 18)
                        {

                            // Truncate on a code point boundary, never mid-sequence.
                            std::size_t cut { 0 };
                            while (cut < 16) cut += std::min(utf8_cp_len(static_cast<unsigned char>(label[cut])), label.size() - cut);
                            disp = label.substr(0, cut) + "..";

                        }
                        const ImVec2 nsz { ImGui::CalcTextSize(disp.c_str()) };
                        dl->AddText({ pos.x + (tile_sz - nsz.x) * 0.5f, pos.y + tile_sz + 4.0f },
                                    IM_COL32(210, 210, 210, 255), disp.c_str());

                        ImGui::Dummy({ tile_sz + 4.0f, tile_sz + 24.0f });

                    };

                    // Self tile
                    const std::string self_label { state.self_display.empty() ? state.self_user_id : state.self_display };
                    draw_tile(12.0f, state.self_user_id, self_label, audio_enabled, !audio_enabled);

                    // Peer tile: video texture if their camera is on, else avatar circle
                    if (!in_call_peer_label.empty())
                    {

                        ImGui::SameLine(0, 16.0f);
                        const auto first_tex { video_textures.begin() };
                        if (first_tex != video_textures.end() and first_tex->second.texture)
                        {

                            const float asp { first_tex->second.height > 0
                                ? static_cast<float>(first_tex->second.width) / static_cast<float>(first_tex->second.height)
                                : 1.333f };
                            ImGui::Image((ImTextureID)(uintptr_t)first_tex->second.texture,
                                         ImVec2 { tile_sz * asp, tile_sz });

                        }
                        else
                        {

                            const float x_off { 12.0f + tile_sz + 4.0f + 16.0f };
                            draw_tile(x_off, in_call_peer_id, in_call_peer_label, true, false);

                        }

                    }

                    // Any additional video feeds (screenshare or second stream)
                    bool is_first { true };
                    for (auto& [pid, entry] : video_textures)
                    {

                        if (is_first) { is_first = false; continue; }
                        if (!entry.texture) continue;
                        ImGui::SameLine(0, 8.0f);
                        const float asp { entry.height > 0
                            ? static_cast<float>(entry.width) / static_cast<float>(entry.height)
                            : 1.333f };
                        ImGui::Image((ImTextureID)(uintptr_t)entry.texture,
                                     ImVec2 { tile_sz * asp, tile_sz });

                    }

                }
                ImGui::EndChild();
                ImGui::PopStyleColor();

                // ---- Control bar -------------------------------------------
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4 { 0.06f, 0.06f, 0.09f, 1.0f });
                if (ImGui::BeginChild("##call_bar", ImVec2 { 0, ctrl_bar_h }, false, ImGuiWindowFlags_NoScrollbar))
                {

                    constexpr float btn_sz  { 40.0f };
                    constexpr float n_btns  { 5.0f };
                    const float     spacing { ImGui::GetStyle().ItemSpacing.x };
                    const float     total_w { n_btns * btn_sz + (n_btns - 1.0f) * spacing };
                    ImGui::SetCursorPos(ImVec2 {
                        (ImGui::GetContentRegionAvail().x - total_w) * 0.5f,
                        (ctrl_bar_h - btn_sz) * 0.5f
                    });

                    // Microphone
                    if (!audio_enabled) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 { 0.75f, 0.18f, 0.18f, 1.0f });
                    if (ImGui::Button(!audio_enabled ? FA_MIC_SLASH : FA_MIC, ImVec2 { btn_sz, btn_sz }))
                    {
                        audio_enabled = !audio_enabled;
                        if (network_pid > 0) kill(network_pid, SIGUSR1);
                    }
                    if (!audio_enabled) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(audio_enabled ? "Mute" : "Unmute");
                    ImGui::SameLine();

                    // Deafen
                    if (deafened) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 { 0.75f, 0.18f, 0.18f, 1.0f });
                    if (ImGui::Button(deafened ? FA_VOLUME_XMRK : FA_HEADPHONES, ImVec2 { btn_sz, btn_sz }))
                    {
                        deafened = !deafened;
                        if (deafened and audio_enabled) { audio_enabled = false; if (network_pid > 0) kill(network_pid, SIGUSR1); }
                    }
                    if (deafened) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(deafened ? "Undeafen" : "Deafen");
                    ImGui::SameLine();

                    // Camera
                    if (video_enabled) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 { 0.18f, 0.48f, 0.80f, 1.0f });
                    if (ImGui::Button(video_enabled ? FA_VIDEO : FA_VIDEO_SLASH, ImVec2 { btn_sz, btn_sz }))
                    {
                        if (video_enabled) { video_enabled = false; if (network_pid > 0) kill(network_pid, SIGUSR2); }
                        else show_camera_modal = true;
                    }
                    if (video_enabled) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(video_enabled ? "Turn Off Camera" : "Turn On Camera");
                    ImGui::SameLine();

                    // Screenshare
                    if (screenshare_enabled) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 { 0.18f, 0.48f, 0.80f, 1.0f });
                    if (ImGui::Button(FA_DESKTOP, ImVec2 { btn_sz, btn_sz }))
                    {
                        if (screenshare_enabled) { screenshare_enabled = false; if (network_pid > 0) kill(network_pid, SIGURG); }
                        else show_screen_modal = true;
                    }
                    if (screenshare_enabled) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(screenshare_enabled ? "Stop Sharing" : "Share Screen");
                    ImGui::SameLine();

                    // Hang Up (red)
                    ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4 { 0.85f, 0.18f, 0.18f, 1.0f });
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4 { 0.95f, 0.28f, 0.28f, 1.0f });
                    if (ImGui::Button(FA_PHONE_SLASH, ImVec2 { btn_sz, btn_sz }))
                    {

                        std::string peer { };
                        { std::scoped_lock<std::mutex> lock { state.mu }; peer = state.call_peer_user_id; }
                        ::signaling::Envelope env { };
                        auto* end { env.mutable_call_end() };
                        end->set_channel_id(in_call_channel);
                        end->set_to_user_id(peer);
                        ipc.send_envelope(env);
                        stop_call_media();

                    }
                    ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("End Call");

                }
                ImGui::EndChild();
                ImGui::PopStyleColor();

                ImGui::Separator();

            }
            else if (!ringing_label.empty())
            {

                ImGui::TextColored(ImVec4 { 0.95f, 0.75f, 0.30f, 1.0f }, "Ringing %s ...", ringing_label.c_str());
                ImGui::Separator();

            }
            else if (!status_line.empty())
            {

                ImGui::TextDisabled("%s", status_line.c_str());
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
                else if (active_channel.empty())
                {

                    // Nothing selected — centered empty state.
                    const ImVec2 avail { ImGui::GetContentRegionAvail() };
                    const char* line1 { "It's quiet in here." };
                    const char* line2 { "Pick a friend or group chat on the left to start talking." };
                    ImGui::Dummy(ImVec2 { 0.0f, avail.y * 0.5f - ImGui::GetTextLineHeightWithSpacing() });
                    ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize(line1).x) * 0.5f);
                    ImGui::TextDisabled("%s", line1);
                    ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize(line2).x) * 0.5f);
                    ImGui::TextDisabled("%s", line2);

                }
                else
                {

                    ImGui::TextDisabled("This is the beginning of your conversation.");
                    ImGui::TextDisabled("Say hi!");

                }

            }
            if (state.chat_scroll_request.exchange(false, std::memory_order_acq_rel)) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();

            // Chat input — Enter sends; disabled until a chat is open
            ImGui::BeginDisabled(active_channel.empty());
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
            ImGui::EndDisabled();

            ImGui::EndChild();

        }
        ImGui::End();

        // ---- MODALS ----

        // Incoming call — Discord-style ring. Nothing joins until the
        // user explicitly hits Join; Decline (or the caller cancelling)
        // dismisses it.
        {

            bool has_incoming { false };
            AppState::IncomingCall inc { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                has_incoming = state.incoming_call.active;
                inc = state.incoming_call;

            }
            if (has_incoming and !ImGui::IsPopupOpen("Incoming Call")) ImGui::OpenPopup("Incoming Call");
            ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2 { 0.5f, 0.5f });
            if (ImGui::BeginPopupModal("Incoming Call", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {

                if (!has_incoming)
                {

                    // Caller cancelled the ring while the modal was up.
                    ImGui::CloseCurrentPopup();

                }
                else
                {

                    const std::string who { inc.from_username.empty() ? inc.from_user_id : inc.from_username };
                    const float av_d { 56.0f };
                    const ImVec2 mp { ImGui::GetCursorScreenPos() };
                    draw_avatar_circle(ImGui::GetWindowDrawList(), avatar_textures, inc.from_user_id, who, ImVec2 { mp.x + av_d * 0.5f, mp.y + av_d * 0.5f }, av_d * 0.5f);
                    ImGui::Dummy(ImVec2 { av_d, av_d });
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Dummy(ImVec2 { 0.0f, av_d * 0.5f - ImGui::GetTextLineHeight() });
                    ImGui::Text("%s", who.c_str());
                    ImGui::TextDisabled("Incoming call...");
                    ImGui::EndGroup();
                    ImGui::Spacing();

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 { 0.24f, 0.65f, 0.36f, 1.0f });
                    if (ImGui::Button("Join", ImVec2 { 120, 0 }))
                    {

                        ::signaling::Envelope env { };
                        auto* acc { env.mutable_call_accept() };
                        acc->set_channel_id(inc.channel_id);
                        acc->set_to_user_id(inc.from_user_id);
                        ipc.send_envelope(env);
                        start_call_media(inc.channel_id);
                        {

                            std::scoped_lock<std::mutex> lock { state.mu };
                            state.active_voice_channel = inc.channel_id;
                            state.call_peer_user_id    = inc.from_user_id;
                            state.active_channel       = inc.channel_id;
                            state.incoming_call        = { };

                        }
                        ImGui::CloseCurrentPopup();

                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4 { 0.85f, 0.25f, 0.25f, 1.0f });
                    if (ImGui::Button("Decline", ImVec2 { 120, 0 }))
                    {

                        ::signaling::Envelope env { };
                        auto* dec { env.mutable_call_decline() };
                        dec->set_channel_id(inc.channel_id);
                        dec->set_to_user_id(inc.from_user_id);
                        ipc.send_envelope(env);
                        {

                            std::scoped_lock<std::mutex> lock { state.mu };
                            state.incoming_call = { };

                        }
                        ImGui::CloseCurrentPopup();

                    }
                    ImGui::PopStyleColor();

                }
                ImGui::EndPopup();

            }

        }

        // Camera picker — enumerate /dev/video* and let the user choose
        // which device goes live before anything transmits.
        if (show_camera_modal)
        {

            ImGui::OpenPopup("Select Camera");
            show_camera_modal = false;

        }
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2 { 0.5f, 0.5f });
        if (ImGui::BeginPopupModal("Select Camera", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {

            ImGui::TextDisabled("Choose a camera to turn on:");
            ImGui::Spacing();

            std::string chosen_device { };
#ifdef __APPLE__
            // macOS: AVFoundation devices are referenced by index ("0", "1", …).
            // Show up to 5 slots — the network child passes the index string to
            // VideoCapture which opens it via avfoundation. Unknown indices fail
            // gracefully and fall back to synthetic video.
            for (int i { 0 }; i < 5; ++i)
            {

                const std::string label { "Camera " + std::to_string(i) };
                if (ImGui::Button(label.c_str(), ImVec2 { 220, 0 })) chosen_device = std::to_string(i);

            }
            constexpr bool any_device { true };
#else
            bool any_device { false };
            for (int i { 0 }; i < 10; ++i)
            {

                const std::string dev { "/dev/video" + std::to_string(i) };
                if (::access(dev.c_str(), F_OK) != 0) continue;
                any_device = true;
                if (ImGui::Button(dev.c_str(), ImVec2 { 220, 0 })) chosen_device = dev;

            }
            if (!any_device) ImGui::TextDisabled("No cameras found — attach a USB camera or use Test pattern.");
#endif
            if (ImGui::Button("Test pattern", ImVec2 { 220, 0 })) chosen_device = "synthetic";

            if (!chosen_device.empty())
            {

                ::signaling::Envelope env { };
                env.mutable_select_devices()->set_camera_device(chosen_device);
                ipc.send_envelope(env);
                if (!video_enabled and network_pid > 0) { kill(network_pid, SIGUSR2); video_enabled = true; }
                ImGui::CloseCurrentPopup();

            }

            ImGui::Spacing();
            if (ImGui::Button("Cancel", ImVec2 { 220, 0 })) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();

        }

        // Screenshare picker — grab everything (auto = largest window,
        // since the WSLg root is always black) or a specific X11 window id.
        if (show_screen_modal)
        {

            ImGui::OpenPopup("Share Screen");
            screen_window_input[0] = '\0';
            show_screen_modal      = false;

        }
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2 { 0.5f, 0.5f });
        if (ImGui::BeginPopupModal("Share Screen", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {

            ImGui::TextDisabled("What do you want to share?");
            ImGui::Spacing();

            bool          share_now  { false };
            std::uint64_t target_win { 0 };

            if (ImGui::Button("Entire screen (auto)", ImVec2 { 260, 0 })) share_now = true;

#ifndef __APPLE__
            // avfoundation grabs whole displays only, so the X11 window-id
            // path is Linux-only.
            ImGui::Spacing();
            ImGui::TextDisabled("...or a specific window (xwininfo id):");
            ImGui::SetNextItemWidth(160);
            ImGui::InputTextWithHint("##win_id", "0x3a00007", screen_window_input, sizeof screen_window_input);
            ImGui::SameLine();
            if (ImGui::Button("Share window") and screen_window_input[0] != '\0')
            {

                target_win = std::strtoull(screen_window_input, nullptr, 0);
                if (target_win != 0) share_now = true;

            }
#endif

            if (share_now)
            {

                ::signaling::Envelope env { };
                auto* sel { env.mutable_select_devices() };
                sel->set_screen_window(target_win);
                sel->set_set_screen(true);
                ipc.send_envelope(env);
                if (!screenshare_enabled and network_pid > 0) { kill(network_pid, SIGURG); screenshare_enabled = true; }
                ImGui::CloseCurrentPopup();

            }

            ImGui::Spacing();
            if (ImGui::Button("Cancel", ImVec2 { 260, 0 })) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();

        }

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

            // A search with no gateway session would just hang on "(no
            // result yet)" forever — say why instead.
            bool gateway_up { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                gateway_up = state.gateway_ready;

            }
            if (!network_child_alive) ImGui::TextColored(ImVec4 { 0.95f, 0.35f, 0.35f, 1.0f }, "Offline: the network process died — search is unavailable.");
            else if (!gateway_up) ImGui::TextColored(ImVec4 { 0.95f, 0.75f, 0.30f, 1.0f }, "Still connecting to the gateway — search will work once connected.");

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

        if (show_new_group_modal)
        {

            ImGui::OpenPopup("New Group Chat");
            new_group_input[0] = '\0';
            show_new_group_modal = false;

        }
        if (ImGui::BeginPopupModal("New Group Chat", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {

            ImGui::TextDisabled("Name the group — friends who enter the same name join it.");
            ImGui::SetNextItemWidth(360);
            const bool enter_pressed { ImGui::InputTextWithHint("##group_name", "group name", new_group_input, sizeof new_group_input, ImGuiInputTextFlags_EnterReturnsTrue) };

            const bool create_clicked { ImGui::Button("Create", ImVec2 { 100, 0 }) };
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2 { 100, 0 })) ImGui::CloseCurrentPopup();

            if ((create_clicked or enter_pressed) and new_group_input[0] != '\0')
            {

                ::signaling::Envelope env { };
                env.mutable_join_channel()->set_name(new_group_input);
                ipc.send_envelope(env);
                new_group_input[0] = '\0';
                ImGui::CloseCurrentPopup();

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

                        // The server only pushes an accepted-event to the
                        // requester, so refetch our own friends list to pick
                        // up the new row + DM channel.
                        ::signaling::Envelope refresh { };
                        refresh.mutable_fetch_friends();
                        ipc.send_envelope(refresh);

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

        // ---- STATS OVERLAY ----
        // Floating semi-transparent window fed by the network child's
        // ClientStats IPC ticks. The same numbers land in logs/*.csv for
        // offline benchmark analysis; this is the live view.
        if (show_stats_overlay)
        {

            StatsSnapshot snap { };
            {

                std::scoped_lock<std::mutex> lock { state.mu };
                snap = state.stats;

            }

            ImGui::SetNextWindowPos(ImVec2 { viewport->WorkPos.x + viewport->WorkSize.x - 10.0f, viewport->WorkPos.y + 10.0f }, ImGuiCond_Always, ImVec2 { 1.0f, 0.0f });
            ImGui::SetNextWindowBgAlpha(0.75f);
            constexpr ImGuiWindowFlags overlay_flags
            {
                ImGuiWindowFlags_NoDecoration
              | ImGuiWindowFlags_AlwaysAutoResize
              | ImGuiWindowFlags_NoSavedSettings
              | ImGuiWindowFlags_NoFocusOnAppearing
              | ImGuiWindowFlags_NoNav
              | ImGuiWindowFlags_NoMove
            };
            if (ImGui::Begin("##stats_overlay", nullptr, overlay_flags))
            {

                if (!snap.valid)
                {

                    ImGui::TextDisabled("waiting for first stats tick...");

                }
                else
                {

                    const auto& s { snap.data };
                    const double loss_pct { s.packets_observed() > 0 ? 100.0 * static_cast<double>(s.packets_lost()) / static_cast<double>(s.packets_observed()) : 0.0 };
                    ImGui::Text("session  t=%.0fs  peers=%u", s.elapsed_s(), s.peers());
                    ImGui::Separator();
                    ImGui::Text("audio    src=%u  sent=%llu", s.audio_sources(), static_cast<unsigned long long>(s.packets_sent()));
                    ImGui::Text("recv     obs=%llu  lost=%llu (%.2f%%)  ooo=%llu", static_cast<unsigned long long>(s.packets_observed()), static_cast<unsigned long long>(s.packets_lost()), loss_pct, static_cast<unsigned long long>(s.packets_ooo()));
                    ImGui::Text("jitter   %.2f ms  jb=%llu/%llu  adapts=%llu", s.jitter_ms(), static_cast<unsigned long long>(s.jb_buffered()), static_cast<unsigned long long>(s.jb_threshold()), static_cast<unsigned long long>(s.jb_adaptations()));
                    ImGui::Text("plc      %llu  drop_ovf=%llu", static_cast<unsigned long long>(s.plc_concealments()), static_cast<unsigned long long>(s.drop_overflow()));
                    if (!s.rtt_summary().empty()) ImGui::Text("rtt      %s", s.rtt_summary().c_str());
                    if (s.video_sources() > 0 or s.vid_frames_sent() > 0 or s.scr_frames_sent() > 0)
                    {

                        ImGui::Separator();
                        ImGui::Text("video    src=%u  lost=%llu", s.video_sources(), static_cast<unsigned long long>(s.vid_packets_lost()));
                        ImGui::Text("frames   dec=%llu  plc=%llu  drawn=%llu", static_cast<unsigned long long>(s.vid_frames_decoded()), static_cast<unsigned long long>(s.vid_frames_concealed()), static_cast<unsigned long long>(s.vid_frames_rendered()));
                        ImGui::Text("sent     cam=%llu  screen=%llu", static_cast<unsigned long long>(s.vid_frames_sent()), static_cast<unsigned long long>(s.scr_frames_sent()));

                    }

                }

            }
            ImGui::End();

        }

        ImGui::Render();

        sync_render_scale(renderer);
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
