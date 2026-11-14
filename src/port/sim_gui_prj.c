#include <nitro.h>
#include <simulator/sim.h>
#include "simulator/gui.h"
#include <stdbool.h>
#include <stdlib.h>

#include "app_menu_actions.h"

#include <simulator/gui.h>
#include "sim_gui_prj.h"

#include "port/debug_field.h"

// The existing libntr input editor, made embeddable by our panel patch.
extern void GUI_PadDrawBindings(void);
extern void GUI_PadCancelCapture(void);

static ImVec2 s_btnSize = {100, 20};

static bool s_createMonOpen;
static bool s_createItemOpen;
static bool s_fieldInfoOpen;
static bool s_mapJumpOpen;

void SIM_GUI_Prj_main(bool * p_open) {
    igBegin("pokeplatinum", p_open, 0);
    SIM_GUI_AppButton("Create Pokemon", &s_createMonOpen, GUI_CreateMon_Init, GUI_CreateMon_Main);
    SIM_GUI_AppButton("Create Item", &s_createItemOpen, GUI_CreateItem_Init, GUI_CreateItem_Main);
    if(DEBUG_GetFieldSystem()) {
        SIM_GUI_AppButton("Field Info", &s_fieldInfoOpen, GUI_FieldInfo_Init, GUI_FieldInfo_Main);
        SIM_GUI_AppButton("MapJump", &s_mapJumpOpen, GUI_MapJump_Init, GUI_MapJump_Main);
    }
    igEnd();
}

// Stage 1C: full-viewport Lake-Trio dashboard, hidden during normal gameplay.
// The existing native File/Input menus are retained until the dashboard has
// passed Windows and Steam Deck regression testing.
static bool s_menuOpen;
static int s_menuPage; // 0 Game, 1 Display, 2 Controls
static bool s_saveConfigNextFrame;
static SDL_JoystickID s_shortcutDevice = -1;
static bool s_l3Held;
static bool s_r3Held;
static bool s_comboLatched;
static int s_lastWindowedMode = 2;

static bool DashboardEnabled(void)
{
    const char *disable = getenv("LAKE_TRIO_UI_DISABLE");
    return !(disable && disable[0] == '1' && disable[1] == '\0');
}

int SIM_GUI_Prj_OverlayIsOpen(void)
{
    return DashboardEnabled() && s_menuOpen;
}

// Called before the simulator's existing GUI key/joystick dispatcher. F1 is
// already the default GUIKey in game.ini: consume it here so that one press
// cannot also toggle the legacy in-window menubar.
int SIM_GUI_Prj_OverlayHandleEvent(const SDL_Event *event)
{
    if (!DashboardEnabled()) return 0;
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        if (event->key.keysym.sym == SDLK_F1) {
            if (event->type == SDL_KEYDOWN && !event->key.repeat) {
                s_menuOpen = !s_menuOpen;
                if (!s_menuOpen) GUI_PadCancelCapture();
            }
            return 1;
        }
    }

    // Match the joystick mapping used by the simulator rather than assuming
    // the SDL_GameController button numbers match the user's Steam Input map.
    if (event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP) {
        const SIM_config_type *config = SIM_GetConfigPtr();
        int l3 = config->padSettings.l3JoyKey;
        int r3 = config->padSettings.r3JoyKey;
        if (l3 < 0 || r3 < 0 || l3 == r3 || l3 > 255 || r3 > 255) return 0;
        const int button = event->jbutton.button;
        if (button != l3 && button != r3) return 0;
        if (s_shortcutDevice != event->jbutton.which) {
            s_shortcutDevice = event->jbutton.which;
            s_l3Held = false;
            s_r3Held = false;
            s_comboLatched = false;
        }
        const bool pressed = event->type == SDL_JOYBUTTONDOWN;
        if (button == l3) s_l3Held = pressed;
        if (button == r3) s_r3Held = pressed;
        if (!pressed && !s_l3Held && !s_r3Held) s_comboLatched = false;
        if (s_l3Held && s_r3Held && !s_comboLatched) {
            s_comboLatched = true;
            s_menuOpen = !s_menuOpen;
            if (!s_menuOpen) GUI_PadCancelCapture();
            return 1;
        }
    }
    if (event->type == SDL_JOYDEVICEREMOVED && event->jdevice.which == s_shortcutDevice) {
        s_shortcutDevice = -1;
        s_l3Held = s_r3Held = s_comboLatched = false;
    }
    return 0;
}

static void DashboardDisplaySettings(void)
{
    const SIM_config_type *config = SIM_GetConfigPtr();
    const char *fpsModes[] = { "30 FPS", "60 FPS", "Auto (display refresh)", "Unlimited" };
    const char *windowModes[] = { "1x window", "2x window", "3x window", "4x window" };
    const char *renderModes[] = { "1x", "2x", "3x", "4x" };

    igTextUnformatted("Display & Performance", NULL);
    igSeparator();
    // ROM import initializes the normal simulator update path. Pending display
    // requests are not applied while the missing-resource screen is active.
    if (SIM_GetMissingResourcePath() != NULL) {
        igTextWrapped("Import ROM data first; display preferences are available before starting a new game.");
        return;
    }
    int fps = (int)SIM_GetFrameRateMode();
    if (fps >= 0 && fps < 4 && igCombo_Str_arr("Frame rate", &fps, fpsModes, 4, 4)) {
        SIM_RequestFrameRateMode((u32)fps);
        s_saveConfigNextFrame = true;
    }
    bool showCounter = SIM_GetFpsCounterEnabled() != 0;
    if (igCheckbox("FPS counter", &showCounter)) {
        SIM_RequestFpsCounterEnabled(showCounter);
        s_saveConfigNextFrame = true;
    }
    bool vsync = SIM_GetVsyncEnabled() != 0;
    if (igCheckbox("VSync", &vsync)) {
        SIM_RequestVsyncEnabled(vsync);
        s_saveConfigNextFrame = true;
    }
    igSpacing();
    int window = (int)SIM_GetWindowSizeMode();
    if (window >= 1 && window <= 4) s_lastWindowedMode = window;
    int selectedWindow = s_lastWindowedMode - 1;
    if (igCombo_Str_arr("Window size", &selectedWindow, windowModes, 4, 4)) {
        s_lastWindowedMode = selectedWindow + 1;
        if (window != 5) {
            SIM_RequestWindowSizeMode((u32)s_lastWindowedMode);
            s_saveConfigNextFrame = true;
        }
    }
    bool fullscreen = window == 5;
    if (igCheckbox("Fullscreen", &fullscreen)) {
        SIM_RequestWindowSizeMode(fullscreen ? 5u : (u32)s_lastWindowedMode);
        s_saveConfigNextFrame = true;
    }
    int scale = (int)SIM_GetInternalResolutionScale() - 1;
    if (scale >= 0 && scale < 4 && igCombo_Str_arr("Internal 3D resolution", &scale, renderModes, 4, 4)) {
        SIM_RequestInternalResolutionScale((u32)scale + 1u);
        s_saveConfigNextFrame = true;
    }
    igSpacing();
    igTextWrapped("Window size uses the project's existing 1x-4x presets, not your monitor's display-mode list. Changes are saved to game.ini.");
    if (config->frameRateMode == SIM_CONFIG_FRAME_RATE_INF)
        igTextWrapped("Unlimited FPS overrides VSync in the simulator.");
}

// Called once per ImGui frame by libntr after pending simulator requests have
// been applied. Save on the following frame so game.ini receives new values.
void SIM_GUI_Prj_Toolbar(int canImport)
{
    if (!DashboardEnabled()) return;
    if (s_saveConfigNextFrame) {
        SIM_Config_SaveGameConfigFile(SIM_GetConfigPtr());
        s_saveConfigNextFrame = false;
    }
    if (!s_menuOpen) return;

    ImGuiIO *io = igGetIO_ContextPtr(igGetCurrentContext());
    const float width = io->DisplaySize.x;
    const float height = io->DisplaySize.y;
    // Independent child regions reset ImGui's own layout cursor for each
    // column. Setting the root cursor once isn't enough: widgets like
    // Separator() can reset its X back to the root window's left margin.
    const bool sideNav = width >= 540.0f;
    const bool tabRow = !sideNav && width >= 380.0f;
    const float outerMargin = 12.0f;
    const float gutter = 12.0f;
    const float navWidth = 180.0f;
    const float navTop = 76.0f;
    const float navHeight = sideNav ? height - navTop - outerMargin : (tabRow ? 58.0f : 158.0f);
    const float pageX = sideNav ? outerMargin + navWidth + gutter : outerMargin;
    const float pageY = sideNav ? navTop : navTop + navHeight + gutter;
    const float pageWidth = width - pageX - outerMargin;
    const float pageHeight = height - pageY - outerMargin;
    ImVec2 origin = { 0, 0 };
    ImVec2 size = { width, height };
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoResize;
    igSetNextWindowPos(origin, ImGuiCond_Always, origin);
    igSetNextWindowSize(size, ImGuiCond_Always);
    igSetNextWindowBgAlpha(0.975f);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 20.0f, 20.0f });
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f);
    igPushStyleColor_Vec4(ImGuiCol_WindowBg, (ImVec4){ 0.047f, 0.065f, 0.110f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){ 0.13f, 0.22f, 0.34f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){ 0.20f, 0.37f, 0.53f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, (ImVec4){ 0.13f, 0.43f, 0.60f, 1.0f });

    if (igBegin("Lake-Trio##dashboard", NULL, flags)) {
        igTextUnformatted("Lake Trio", NULL);
        // Keep the resume button on its own row on very small windows.
        if (!sideNav && !tabRow) {
            igSpacing();
        } else {
            igSameLine(0.0f, 20.0f);
        }
        if (igButton("Resume [F1]", (ImVec2){ 125.0f, 32.0f })) {
            s_menuOpen = false;
            GUI_PadCancelCapture();
        }
        igSeparator();

        const char *labels[] = { "Game", "Display", "Controls" };
        igSetCursorPos((ImVec2){ outerMargin, navTop });
        if (igBeginChild_Str("##dashboard_nav", (ImVec2){ sideNav ? navWidth : width - outerMargin * 2.0f, navHeight }, 1, 0)) {
            const float cellWidth = sideNav ? navWidth - 16.0f
                : tabRow ? (width - outerMargin * 2.0f - 32.0f - 16.0f) / 3.0f
                : width - outerMargin * 2.0f - 16.0f;
            for (int i = 0; i < 3; ++i) {
                if (i > 0 && tabRow) igSameLine(0.0f, 8.0f);
                const bool selected = i == s_menuPage;
                if (selected) {
                    igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){ 0.20f, 0.37f, 0.55f, 1.0f });
                }
                const bool clicked = igButton(labels[i], (ImVec2){ cellWidth, 38.0f });
                if (selected) igPopStyleColor(1);
                if (clicked) {
                    if (s_menuPage == 2 && i != 2) GUI_PadCancelCapture();
                    s_menuPage = i;
                }
            }
        }
        igEndChild();

        if (pageWidth > 32.0f && pageHeight > 32.0f) {
            igSetCursorPos((ImVec2){ pageX, pageY });
            if (igBeginChild_Str("##dashboard_page", (ImVec2){ pageWidth, pageHeight }, 1, 0)) {
                switch (s_menuPage) {
                case 0:
                    igTextUnformatted("Game", NULL);
                    igSeparator();
                    igTextWrapped("Import the ROM data needed by the existing game installation.");
                    if (igButton("Import", (ImVec2){ 170.0f, 38.0f }) && canImport) {
                        PlatformMenu_RequestAction(LAKE_TRIO_MENU_IMPORT_ROM);
                        s_menuOpen = false;
                    }
                    if (!canImport) igTextUnformatted("Import is currently unavailable.", NULL);
                    break;
                case 1:
                    DashboardDisplaySettings();
                    break;
                case 2:
                    igTextUnformatted("Controls", NULL);
                    igSeparator();
                    igTextWrapped("Select a Keyboard or Controller binding below, then press the new input. Escape clears a binding.");
                    GUI_PadDrawBindings();
                    break;
                }
            }
            igEndChild();
        }
    }
    igEnd();
    igPopStyleColor(4);
    igPopStyleVar(2);
}
