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

namespace
{

    constexpr int initial_window_width { 1280 };
    constexpr int initial_window_height { 800 };

    constexpr Uint8 bg_r { 26 };
    constexpr Uint8 bg_g { 28 };
    constexpr Uint8 bg_b { 34 };

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

}

int main()
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string user_name { env_str("OSN_USER", "alice") };
    const std::string signaling_host { env_str("OSN_SIGNALING_HOST", "3.144.229.204") };
    const std::string auth_secret { env_str("OPENSOCIALNET_AUTH_SECRET", "devsecret123") };
    const std::string room_name { env_str("OSN_ROOM", "general") };
    const std::string auth_token { compute_auth_token(user_name, auth_secret) };
    if (auth_token.empty())
    {
        std::printf("native_client: failed to compute auth token (is openssl installed?)\n");
        return 1;
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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool audio_enabled { true };
    bool video_enabled { true };
    bool screenshare_enabled { false };
    std::string connection_status { "Starting network..." };
    std::atomic<bool> network_connected { false };

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

        ImGui::DockSpaceOverViewport(ImGui::GetMainViewport()->ID, ImGui::GetMainViewport());

        if (ImGui::Begin("Info"))
        {
            ImGui::TextUnformatted("OpenSocialNet — native client");
            ImGui::Separator();
            ImGui::Text("User: %s", user_name.c_str());
            ImGui::Text("Room: %s", room_name.c_str());
            ImGui::Text("Signaling: %s", signaling_host.c_str());
            ImGui::Separator();
            ImGui::TextUnformatted(connection_status.c_str());
            if (network_connected)
            {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
            }
        }
        ImGui::End();

        if (ImGui::Begin("Controls"))
        {
            ImGui::TextUnformatted("Media");
            ImGui::Separator();

            // SIGUSR1/SIGUSR2/SIGURG flip atomic toggles inside the network
            // child. Encoders are already initialised so flipping is instant.
            if (ImGui::Button(audio_enabled ? "Mute" : "Unmute", ImVec2(-1, 40)))
            {
                audio_enabled = !audio_enabled;
                if (network_pid > 0) kill(network_pid, SIGUSR1);
                std::printf("[ui] audio %s -> SIGUSR1\n", audio_enabled ? "enabled" : "disabled");
            }

            if (ImGui::Button(video_enabled ? "Camera Off" : "Camera On", ImVec2(-1, 40)))
            {
                video_enabled = !video_enabled;
                if (network_pid > 0) kill(network_pid, SIGUSR2);
                std::printf("[ui] video %s -> SIGUSR2\n", video_enabled ? "enabled" : "disabled");
            }

            if (ImGui::Button(screenshare_enabled ? "Stop Share" : "Share Screen", ImVec2(-1, 40)))
            {
                screenshare_enabled = !screenshare_enabled;
                if (network_pid > 0) kill(network_pid, SIGURG);
                std::printf("[ui] screenshare %s -> SIGURG\n", screenshare_enabled ? "enabled" : "disabled");
            }
        }
        ImGui::End();

        if (ImGui::Begin("Video"))
        {
            ImGui::TextDisabled("(remote video tiles render here)");
            ImGui::Separator();
            const ImVec2 avail { ImGui::GetContentRegionAvail() };
            ImGui::Dummy(avail);
        }
        ImGui::End();

        if (ImGui::Begin("Chat"))
        {
            ImGui::TextDisabled("(room chat)");
            ImGui::Separator();
            static char draft[512] { };
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##draft", draft, sizeof draft, ImGuiInputTextFlags_EnterReturnsTrue))
            {
                std::printf("[ui] chat: %s\n", draft);
                draft[0] = '\0';
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
