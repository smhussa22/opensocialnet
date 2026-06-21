// related headers

// c sys headers
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

// cpp stdlib headers
#include <string>
#include <vector>
#include <thread>
#include <atomic>

// 3rd party headers
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

// project headers
#include "OAuthClient.hh"

namespace
{

    // Compact single-window layout. Fixed size, not resizable — the GUI
    // is meant to be a small "call dock", not a desktop-spanning surface.
    constexpr int window_width  { 720 };
    constexpr int window_height { 880 };

    constexpr Uint8 bg_r { 26 };
    constexpr Uint8 bg_g { 28 };
    constexpr Uint8 bg_b { 34 };

    constexpr std::size_t chat_input_capacity { 512 };
    constexpr std::size_t chat_history_cap    { 256 };

    std::string env_str(const char* key, const std::string& default_val = "")
    {
        const char* val { std::getenv(key) };
        return val ? std::string { val } : default_val;
    }

    // Shell out to openssl to compute HMAC-SHA256(user, secret) — same
    // recipe the gateway runs on its side to verify the Hello frame. The
    // network binary takes the already-computed hex token via OSN_AUTH_TOKEN.
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

    // Drop-in: render a labelled camera tile. Colour borders + centred
    // status text instead of real frames — frames live in the network
    // child's process space; piping them across IPC into the GUI is a
    // separate task. For now this is a faithful UI placeholder.
    void render_camera_tile(const char* corner_label, bool is_on, ImVec2 size)
    {

        const ImVec4 bg_on  { 0.05f, 0.10f, 0.05f, 1.0f };
        const ImVec4 bg_off { 0.10f, 0.10f, 0.12f, 1.0f };
        const ImVec4 br_on  { 0.30f, 0.70f, 0.40f, 1.0f };
        const ImVec4 br_off { 0.40f, 0.40f, 0.45f, 1.0f };

        ImGui::PushStyleColor(ImGuiCol_ChildBg, is_on ? bg_on : bg_off);
        ImGui::PushStyleColor(ImGuiCol_Border, is_on ? br_on : br_off);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);

        ImGui::BeginChild(corner_label, size, true);

        ImGui::SetCursorPos(ImVec2(8, 6));
        ImGui::TextDisabled("%s", corner_label);

        const char*  status { is_on ? "CAMERA ON" : "CAMERA OFF" };
        const ImVec2 ts     { ImGui::CalcTextSize(status) };
        ImGui::SetCursorPos(ImVec2((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f));
        ImGui::TextUnformatted(status);

        ImGui::EndChild();

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

    }

}

int main()
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Two auth modes share this main() depending on env:
    //
    //   OSN_GOOGLE_CLIENT_ID set -> Google OAuth desktop loopback flow.
    //     Browser pops, user signs in, we get an RS256 JWT back. user_id
    //     becomes the Google `sub`, display name is profile name / email.
    //
    //   OSN_GOOGLE_CLIENT_ID unset -> HMAC dev fallback. Uses the
    //     existing OSN_USER + OPENSOCIALNET_AUTH_SECRET pair so tests
    //     and CI keep working without going through Google.
    const std::string google_client_id { env_str("OSN_GOOGLE_CLIENT_ID", "") };
    const std::string signaling_host   { env_str("OSN_SIGNALING_HOST", "3.144.229.204") };
    const std::string room_name        { env_str("OSN_ROOM", "general") };

    std::string user_name  { };
    std::string auth_token { };

    if (!google_client_id.empty())
    {

        std::printf("native_client: starting Google sign-in (client_id=%s...)\n", google_client_id.substr(0, 12).c_str());
        const auto oauth { OpenSocialNet::NativeClient::google_login(google_client_id) };
        if (!oauth.ok)
        {

            std::printf("native_client: Google sign-in failed: %s\n", oauth.error.c_str());
            return 1;

        }

        // sub is the stable user_id; the network child re-uses this for
        // routing (fnv1a_32(user_id) -> peer_id) and for the Hello
        // envelope. Display name preference: profile name > email > sub.
        user_name  = oauth.sub;
        auth_token = oauth.id_token;

        std::string display { oauth.name };
        if (display.empty()) display = oauth.email;
        if (display.empty()) display = oauth.sub;
        std::printf("native_client: signed in as %s (sub=%s)\n", display.c_str(), oauth.sub.c_str());

    }
    else
    {

        user_name = env_str("OSN_USER", "alice");
        const std::string auth_secret { env_str("OPENSOCIALNET_AUTH_SECRET", "devsecret123") };
        auth_token = compute_auth_token(user_name, auth_secret);
        if (auth_token.empty())
        {

            std::printf("native_client: failed to compute HMAC auth token (is openssl installed?)\n");
            return 1;

        }
        std::printf("native_client: HMAC dev auth as user=%s\n", user_name.c_str());

    }

    // VIDEO only — the audio subsystem is owned by the spawned network child,
    // and initialising it twice on the same machine collides with WSL's
    // pulseaudio/ALSA layer and crashes the second instance.
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("native_client: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window { nullptr };
    SDL_Renderer* renderer { nullptr };
    const std::string window_title { "OpenSocialNet — " + user_name };
    // No SDL_WINDOW_RESIZABLE — the layout is hand-sized for window_width x window_height.
    if (!SDL_CreateWindowAndRenderer(window_title.c_str(), window_width, window_height,
                                      SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer))
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
    // Intentionally no DockingEnable — the new layout is one flat window.

    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool audio_enabled       { true };
    bool video_enabled       { true };
    bool screenshare_enabled { false };
    std::string connection_status { "Starting network..." };
    std::atomic<bool> network_connected { false };

    // Local-only chat scratch state. Real signaling-server chat round-trip
    // needs an IPC link to the network child (it owns the WebSocket); for
    // now Enter just appends to the visible history so the layout reads
    // right. Wire it up after the chat IPC channel lands.
    std::vector<std::string> chat_history { };
    chat_history.reserve(chat_history_cap);
    char chat_draft[chat_input_capacity] { };
    bool chat_scroll_to_bottom { false };

    // Spawn the network binary as a child process
    std::printf("native_client: spawning network client (user=%s, signaling=%s)\n", user_name.c_str(), signaling_host.c_str());

    network_pid = fork();
    if (network_pid == 0)
    {
        // Child process — run the network binary
        const std::string relay_host { signaling_host };
        const std::string local_port { "0" };

        // Carry over the parent's env then layer on the runtime config. We
        // initialise both video and screen *capabilities* (OSN_VIDEO=1 /
        // OSN_SCREEN=1) so the encoders boot. The actual send is gated at
        // runtime by signals from the parent — flipping back on costs nothing.
        setenv("OSN_SIGNALING_HOST",          signaling_host.c_str(), 1);
        setenv("OSN_ROOM",                    room_name.c_str(),      1);
        setenv("OSN_USER",                    user_name.c_str(),      1);
        setenv("OSN_AUTH_TOKEN",              auth_token.c_str(),     1);
        setenv("OSN_LOCAL_PORT",              local_port.c_str(),     1);
        setenv("OSN_VIDEO",                   "1",                    1);
        setenv("OSN_SCREEN",                  "1",                    1);
        setenv("OSN_ICE",                     "1",                    1);

        // Construct absolute path to network binary
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
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::printf("native_client: network pid=%d\n", network_pid);
    connection_status = "Network running (pid=" + std::to_string(network_pid) + ")";
    network_connected = true;

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

        // Single full-viewport window — no docking, no titlebar, no
        // resize handles. ImGui treats the SDL window as the canvas.
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

            // ---- header ----
            ImGui::Text("OpenSocialNet — %s   #%s", user_name.c_str(), room_name.c_str());
            ImGui::TextDisabled("signaling: %s", signaling_host.c_str());
            if (network_connected) ImGui::TextColored(ImVec4 { 0.0f, 1.0f, 0.0f, 1.0f }, "Connected");
            else                    ImGui::TextColored(ImVec4 { 1.0f, 0.5f, 0.2f, 1.0f }, "%s", connection_status.c_str());

            ImGui::Separator();

            // ---- camera grid: 2 columns, 4:3 tiles ----
            const float  inner_w  { ImGui::GetContentRegionAvail().x };
            const float  spacing  { ImGui::GetStyle().ItemSpacing.x };
            const float  tile_w   { (inner_w - spacing) * 0.5f };
            const float  tile_h   { tile_w * 0.75f };
            const ImVec2 tile_sz  { tile_w, tile_h };

            const std::string you_label    { "You — " + user_name };
            const std::string remote_label { "Remote" };

            render_camera_tile(you_label.c_str(), video_enabled, tile_sz);
            ImGui::SameLine();
            // Remote tile reuses the same renderer; "off" placeholder until
            // we have a real frame stream from the peer over IPC.
            render_camera_tile(remote_label.c_str(), false, tile_sz);

            ImGui::Separator();

            // ---- button row ----
            const float btn_spacing { ImGui::GetStyle().ItemSpacing.x };
            const float btn_w       { (inner_w - 2 * btn_spacing) / 3.0f };
            const ImVec2 btn_sz     { btn_w, 34 };

            if (ImGui::Button(audio_enabled ? "Mute" : "Unmute", btn_sz))
            {

                audio_enabled = !audio_enabled;
                if (network_pid > 0) kill(network_pid, SIGUSR1);

            }
            ImGui::SameLine();
            if (ImGui::Button(video_enabled ? "Camera Off" : "Camera On", btn_sz))
            {

                video_enabled = !video_enabled;
                if (network_pid > 0) kill(network_pid, SIGUSR2);

            }
            ImGui::SameLine();
            if (ImGui::Button(screenshare_enabled ? "Stop Share" : "Share Screen", btn_sz))
            {

                screenshare_enabled = !screenshare_enabled;
                if (network_pid > 0) kill(network_pid, SIGURG);

            }

            ImGui::Separator();

            // ---- chat: scrollable history above a single-line input ----
            ImGui::TextDisabled("Chat");

            const float input_h { ImGui::GetFrameHeightWithSpacing() };
            const float chat_h  { ImGui::GetContentRegionAvail().y - input_h - ImGui::GetStyle().ItemSpacing.y };

            ImGui::BeginChild("##chat_history", ImVec2 { 0.0f, chat_h }, true);
            for (const auto& line : chat_history) ImGui::TextWrapped("%s", line.c_str());
            if (chat_scroll_to_bottom)
            {

                ImGui::SetScrollHereY(1.0f);
                chat_scroll_to_bottom = false;

            }
            ImGui::EndChild();

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##chat_input", chat_draft, sizeof chat_draft, ImGuiInputTextFlags_EnterReturnsTrue))
            {

                if (chat_draft[0] != '\0')
                {

                    if (chat_history.size() >= chat_history_cap) chat_history.erase(chat_history.begin());
                    chat_history.emplace_back(user_name + ": " + chat_draft);
                    chat_draft[0] = '\0';
                    chat_scroll_to_bottom = true;

                }
                // Keep focus on the input so the user can keep typing
                ImGui::SetKeyboardFocusHere(-1);

            }

        }
        ImGui::End();

        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

    }

    cleanup_network();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;

}
