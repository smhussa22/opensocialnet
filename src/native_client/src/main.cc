// related headers

// c sys headers
#include <cstdio>
#include <cstdlib>

// cpp stdlib headers
#include <string>

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

}

int main()
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string user_name { env_str("OSN_USER", "alice") };
    const std::string room_name { env_str("OSN_ROOM", "general") };

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
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

    std::printf("native_client: user=%s room=%s\n", user_name.c_str(), room_name.c_str());

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
            ImGui::Text("Status: ready");
        }
        ImGui::End();

        if (ImGui::Begin("Controls"))
        {
            ImGui::TextUnformatted("Media");
            ImGui::Separator();

            if (ImGui::Button(audio_enabled ? "🔊 Mute" : "🔇 Unmute", ImVec2(-1, 40)))
            {
                audio_enabled = !audio_enabled;
                std::printf("[ui] audio %s\n", audio_enabled ? "enabled" : "disabled");
            }

            if (ImGui::Button(video_enabled ? "📹 Camera Off" : "📹 Camera On", ImVec2(-1, 40)))
            {
                video_enabled = !video_enabled;
                std::printf("[ui] video %s\n", video_enabled ? "enabled" : "disabled");
            }

            if (ImGui::Button(screenshare_enabled ? "🖥️ Stop Share" : "🖥️ Share Screen", ImVec2(-1, 40)))
            {
                screenshare_enabled = !screenshare_enabled;
                std::printf("[ui] screenshare %s\n", screenshare_enabled ? "enabled" : "disabled");
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

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;

}
