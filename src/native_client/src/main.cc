// related headers

// c sys headers
#include <cstdio>

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

    constexpr int initial_window_width  { 1280 };
    constexpr int initial_window_height {  800 };

    // colour the renderer clears to before ImGui draws over it. matches the
    // default ImGui dark theme background so the seams between window and
    // first ImGui panel are invisible.
    constexpr Uint8 bg_r { 26 };
    constexpr Uint8 bg_g { 28 };
    constexpr Uint8 bg_b { 34 };

}

// One-window skeleton. Opens an SDL3 + SDL_Renderer window, initialises
// ImGui (docking branch), shows three placeholder panels: room list, video
// grid, chat. Real wiring (WebSocket gateway client, audio capture, V4L2
// camera, UDP relay client) goes in over the next phases — this exists
// so the build pipeline is in place + the user can see something open.
int main()
{

    // unbuffered stdout — Docker / journald capture log lines as soon as
    // they're printed instead of waiting for a full buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {

        std::printf("native_client: SDL_Init failed: %s\n", SDL_GetError());
        return 1;

    }

    SDL_Window*   window   { nullptr };
    SDL_Renderer* renderer { nullptr };
    if (!SDL_CreateWindowAndRenderer("OpenSocialNet", initial_window_width, initial_window_height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer))
    {

        std::printf("native_client: CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;

    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io { ImGui::GetIO() };
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;        // dockable panels
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;    // arrow-key + tab nav between widgets

    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

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

        // full-window dockspace so the user can drag panels around.
        ImGui::DockSpaceOverViewport(ImGui::GetMainViewport()->ID, ImGui::GetMainViewport());

        // ---- room list (placeholder) ----
        if (ImGui::Begin("Rooms"))
        {

            ImGui::TextUnformatted("OpenSocialNet — native client");
            ImGui::Separator();
            ImGui::TextDisabled("(placeholder — server list / DM list lives here)");
            ImGui::BulletText("#general");
            ImGui::BulletText("#voice-lobby");
            ImGui::BulletText("Direct messages");

        }
        ImGui::End();

        // ---- video grid (placeholder) ----
        if (ImGui::Begin("Video"))
        {

            ImGui::TextDisabled("(placeholder — V4L2 capture → H264 encode → relay → decode → SDL texture grid)");
            ImGui::Separator();
            const ImVec2 avail { ImGui::GetContentRegionAvail() };
            ImGui::Dummy(avail);

        }
        ImGui::End();

        // ---- chat (placeholder) ----
        if (ImGui::Begin("Chat"))
        {

            ImGui::TextDisabled("(placeholder — WebSocket gateway client → Scylla history + Kafka live messages)");
            ImGui::Separator();
            ImGui::BulletText("alice: hello world");
            ImGui::BulletText("bob:   good morning");
            static char draft[512] { };
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##draft", draft, sizeof draft);

        }
        ImGui::End();

        // ---- net stats (placeholder) ----
        if (ImGui::Begin("Network stats"))
        {

            ImGui::TextDisabled("(placeholder — wired to JitterStats once the voice path is live)");
            ImGui::BulletText("obs=0  lost=0  ooo=0  jitter_ms=0.00  rtt_ms=0");

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
