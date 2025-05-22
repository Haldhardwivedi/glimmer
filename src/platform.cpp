#include "platform.h"
#include "context.h"
#include "renderer.h"

#include <cstring>

#ifndef GLIMMER_MAX_CLIPBOARD_TEXTSZ
#define GLIMMER_MAX_CLIPBOARD_TEXTSZ 4096
#endif

#ifndef GLIMMER_IMGUI_MAINWINDOW_NAME
#define GLIMMER_IMGUI_MAINWINDOW_NAME "main-window"
#endif

#if GLIMMER_PLATFORM == GLIMMER_IMGUI_GLFW_PLATFORM
#include "libs/inc/imgui/imgui_impl_glfw.h"
#include "libs/inc/imgui/imgui_impl_opengl3.h"

#include <stdio.h>
#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <libs/inc/GLFW/glfw3.h> // Will drag system OpenGL headers

#ifdef __EMSCRIPTEN__
#include "../libs/emscripten/emscripten_mainloop_stub.h"
#endif
#endif

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinUser.h>
#undef CreateWindow

static void DetermineKeyStatus(glimmer::IODescriptor& desc)
{
    desc.capslock = GetAsyncKeyState(VK_CAPITAL) < 0;
    desc.insert = false;
}
#elif __linux__
#include <ctsdio>
#include <unistd.h>

static std::string exec(const char* cmd) 
{
    char buffer[128];
    std::string result = "";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) throw std::runtime_error("popen() failed!");

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        result += buffer;

    pclose(pipe);
    return result;
}

static void DetermineKeyStatus(glimmer::IODescriptor& desc)
{
    std::string xset_output = exec("xset -q | grep Caps | sed -n 's/^.*Caps Lock:\\s*\\(\\S*\\).*$/\\1/p'");
    desc.capslock = xset_output.find("on") != std::string::npos;
    desc.insert = false;
}
#endif

namespace glimmer
{
    static void RegisterKeyBindings()
    {
        KeyMappings.resize(512, { 0, 0 });
        KeyMappings[Key_0] = { '0', ')' }; KeyMappings[Key_1] = { '1', '!' }; KeyMappings[Key_2] = { '2', '@' };
        KeyMappings[Key_3] = { '3', '#' }; KeyMappings[Key_4] = { '4', '$' }; KeyMappings[Key_5] = { '5', '%' };
        KeyMappings[Key_6] = { '6', '^' }; KeyMappings[Key_7] = { '7', '&' }; KeyMappings[Key_8] = { '8', '*' };
        KeyMappings[Key_9] = { '9', '(' };

        KeyMappings[Key_A] = { 'A', 'a' }; KeyMappings[Key_B] = { 'B', 'b' }; KeyMappings[Key_C] = { 'C', 'c' };
        KeyMappings[Key_D] = { 'D', 'd' }; KeyMappings[Key_E] = { 'E', 'e' }; KeyMappings[Key_F] = { 'F', 'f' };
        KeyMappings[Key_G] = { 'G', 'g' }; KeyMappings[Key_H] = { 'H', 'h' }; KeyMappings[Key_I] = { 'I', 'i' };
        KeyMappings[Key_J] = { 'J', 'j' }; KeyMappings[Key_K] = { 'K', 'k' }; KeyMappings[Key_L] = { 'L', 'l' };
        KeyMappings[Key_M] = { 'M', 'm' }; KeyMappings[Key_N] = { 'N', 'n' }; KeyMappings[Key_O] = { 'O', 'o' };
        KeyMappings[Key_P] = { 'P', 'p' }; KeyMappings[Key_Q] = { 'Q', 'q' }; KeyMappings[Key_R] = { 'R', 'r' };
        KeyMappings[Key_S] = { 'S', 's' }; KeyMappings[Key_T] = { 'T', 't' }; KeyMappings[Key_U] = { 'U', 'u' };
        KeyMappings[Key_V] = { 'V', 'v' }; KeyMappings[Key_W] = { 'W', 'w' }; KeyMappings[Key_X] = { 'X', 'x' };
        KeyMappings[Key_Y] = { 'Y', 'y' }; KeyMappings[Key_Z] = { 'Z', 'z' };

        KeyMappings[Key_Apostrophe] = { '\'', '"' }; KeyMappings[Key_Backslash] = { '\\', '|' };
        KeyMappings[Key_Slash] = { '/', '?' }; KeyMappings[Key_Comma] = { ',', '<' };
        KeyMappings[Key_Minus] = { '-', '_' }; KeyMappings[Key_Period] = { '.', '>' };
        KeyMappings[Key_Semicolon] = { ';', ':' }; KeyMappings[Key_Equal] = { '=', '+' };
        KeyMappings[Key_LeftBracket] = { '[', '{' }; KeyMappings[Key_RightBracket] = { ']', '}' };
        KeyMappings[Key_Space] = { ' ', ' ' }; KeyMappings[Key_Tab] = { '\t', '\t' };
        KeyMappings[Key_GraveAccent] = { '`', '~' };
    }

#if GLIMMER_PLATFORM == GLIMMER_IMGUI_GLFW_PLATFORM

    static void glfw_error_callback(int error, const char* description)
    {
        fprintf(stderr, "GLFW Error %d: %s\n", error, description);
    }

    struct ImGuiGLFWPlatform final : public IPlatform
    {
        ImGuiGLFWPlatform()
        {
            DetermineKeyStatus(desc);
        }

        void SetClipboardText(std::string_view input)
        {
            static char buffer[GLIMMER_MAX_CLIPBOARD_TEXTSZ];

            auto sz = std::min(input.size(), (size_t)(GLIMMER_MAX_CLIPBOARD_TEXTSZ - 1));
            std::strncpy(buffer, input.data(), sz);
            buffer[sz] = 0;

            ImGui::SetClipboardText(buffer);
        }

        std::string_view GetClipboardText()
        {
            auto str = ImGui::GetClipboardText();
            return std::string_view{ str };
        }

        IODescriptor CurrentIO()
        {
            auto& context = GetContext();
            IODescriptor result;

            if (!context.activePopUpRegion.Contains(desc.mousepos)) result = desc;

            return result;
        }

        void SetMouseCursor(MouseCursor cursor)
        {
            ImGui::SetMouseCursor((ImGuiMouseCursor)cursor);
        }

        void EnterFrame()
        {
            auto& context = GetContext();
            context.InsideFrame = true;
            context.adhocLayout.push();

            auto& io = ImGui::GetIO();
            auto rollover = 0;
            auto escape = false, clicked = false;

            desc.deltaTime = io.DeltaTime;
            desc.mousepos = io.MousePos;
            desc.mouseWheel = io.MouseWheel;
            desc.modifiers = io.KeyMods;
            totalTime += io.DeltaTime;

            for (auto idx = 0; idx < ImGuiMouseButton_COUNT; ++idx)
            {
                desc.mouseButtonStatus[idx] =
                    ImGui::IsMouseDown(idx) ? ButtonStatus::Pressed :
                    ImGui::IsMouseReleased(idx) ? ButtonStatus::Released :
                    ImGui::IsMouseDoubleClicked(idx) ? ButtonStatus::DoubleClicked :
                    ButtonStatus::Default;
                clicked = clicked || ImGui::IsMouseDown(idx);
            }

            for (int key = Key_Tab; key != Key_Total; ++key)
            {
                auto imkey = ImGuiKey_NamedKey_BEGIN + key;
                if (ImGui::IsKeyPressed((ImGuiKey)imkey))
                {
                    if ((ImGuiKey)imkey == ImGuiKey_CapsLock) desc.capslock = !desc.capslock;
                    else if ((ImGuiKey)imkey == ImGuiKey_Insert) desc.insert = !desc.insert;
                    else
                    {
                        if (rollover < GLIMMER_NKEY_ROLLOVER_MAX)
                            desc.key[rollover++] = (Key)key;
                        desc.keyStatus[key] = ButtonStatus::Pressed;
                        escape = imkey == ImGuiKey_Escape;
                    }
                }
                else if (ImGui::IsKeyReleased((ImGuiKey)imkey))
                    desc.keyStatus[key] = ButtonStatus::Released;
                else
                    desc.keyStatus[key] = ButtonStatus::Default;
            }

            while (rollover <= GLIMMER_NKEY_ROLLOVER_MAX)
                desc.key[rollover++] = Key_Invalid;

            if (clicked || escape)
            {
                ResetActivePopUps(desc.mousepos, escape);
            }
        }

        void ExitFrame()
        {
            ++frameCount;

            for (auto idx = 0; idx < GLIMMER_NKEY_ROLLOVER_MAX; ++idx)
                desc.key[idx] = Key_Invalid;

            ResetAllContexts();
        }

        bool CreateWindow(const WindowParams& params)
        {
            glfwSetErrorCallback(glfw_error_callback);
            if (!glfwInit()) return false;

            // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
            // GL ES 2.0 + GLSL 100
            const char* glsl_version = "#version 100";
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
            // GL 3.2 + GLSL 150
            const char* glsl_version = "#version 150";
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
            // GL 3.0 + GLSL 130
            const char* glsl_version = "#version 130";
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
            //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
            //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif

            // Create window with graphics context
            m_window = glfwCreateWindow((int)params.size.x, (int)params.size.y, params.title.data(), nullptr, nullptr);
            if (m_window == nullptr) return false;

            glfwMakeContextCurrent(m_window);
            glfwSwapInterval(1); // Enable vsync

            // Setup Dear ImGui context
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.IniFilename = nullptr;

            // Setup Platform/Renderer backends
            ImGui_ImplGlfw_InitForOpenGL(m_window, true);
#ifdef __EMSCRIPTEN__
            ImGui_ImplGlfw_InstallEmscriptenCallbacks(m_window, "#canvas");
#endif
            ImGui_ImplOpenGL3_Init(glsl_version);

            bgcolor[0] = (float)params.bgcolor[0] / 255.f;
            bgcolor[1] = (float)params.bgcolor[1] / 255.f;
            bgcolor[2] = (float)params.bgcolor[2] / 255.f;
            bgcolor[3] = (float)params.bgcolor[3] / 255.f;
            softwareCursor = params.softwareCursor;
        }

        bool PollEvents(bool (*runner)(ImVec2, void*), void* data)
        {
            auto close = false;
            AddBaseStyleFontPtrs();

#ifdef __EMSCRIPTEN__
            // For an Emscripten build we are disabling file-system access, so let's not attempt to do a fopen() of the imgui.ini file.
            // You may manually call LoadIniSettingsFromMemory() to load settings from your own storage.

            EMSCRIPTEN_MAINLOOP_BEGIN
#else
            while (!glfwWindowShouldClose(m_window) && !close)
#endif
            {
                glfwPollEvents();
                if (glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0)
                {
                    ImGui_ImplGlfw_Sleep(10);
                    continue;
                }

                int width, height;
                glfwGetWindowSize(m_window, &width, &height);

                // Start the Dear ImGui frame
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                ImGui::GetIO().MouseDrawCursor = softwareCursor;

                ImVec2 winsz{ (float)width, (float)height };
                ImGui::SetNextWindowSize(winsz, ImGuiCond_Always);
                ImGui::SetNextWindowPos(ImVec2{ 0, 0 });
                EnterFrame();

                if (ImGui::Begin(GLIMMER_IMGUI_MAINWINDOW_NAME, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
                {
                    auto dl = ImGui::GetWindowDrawList();
                    Config.renderer->UserData = dl;
                    dl->AddRectFilled(ImVec2{ 0, 0 }, winsz, ImColor{ bgcolor[0], bgcolor[1], bgcolor[2], bgcolor[3] });
                    close = !runner(winsz, data);
                }

                ImGui::End();
                ExitFrame();

                // Rendering
                ImGui::Render();
                int display_w, display_h;
                glfwGetFramebufferSize(m_window, &display_w, &display_h);
                glViewport(0, 0, display_w, display_h);
                glClearColor(bgcolor[0], bgcolor[1], bgcolor[2], bgcolor[3]);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                glfwSwapBuffers(m_window);

#ifdef __EMSCRIPTEN__
            EMSCRIPTEN_MAINLOOP_END;
#else
            }
#endif
            return true;
        }

        GLFWwindow* m_window = nullptr;
        float bgcolor[4];
        bool softwareCursor = false;
    };

    IPlatform* GetPlatform(ImVec2 size)
    {
        static ImGuiGLFWPlatform platform;
        static bool initialized = false;

        if (!initialized)
        {
            initialized = true;
            RegisterKeyBindings();
            PushContext(-1);
        }
        
        return &platform;
    }
#else
    IPlatform* GetPlatform(ImVec2 size)
    {
        return nullptr;
    }
#endif

    IODescriptor::IODescriptor()
    {
        for (auto k = 0; k <= GLIMMER_NKEY_ROLLOVER_MAX; ++k) key[k] = Key_Invalid;
        for (auto ks = 0; ks < (GLIMMER_KEY_ENUM_END - GLIMMER_KEY_ENUM_START + 1); ++ks)
            keyStatus[ks] = ButtonStatus::Default;
    }

    float IPlatform::fps() const
    {
        return (float)frameCount / totalTime;
    }
}
