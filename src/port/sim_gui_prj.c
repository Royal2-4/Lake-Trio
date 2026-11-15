#include <nitro.h>
#include <simulator/sim.h>
#include "simulator/gui.h"
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "app_menu_actions.h"

#include <simulator/gui.h>
#include "sim_gui_prj.h"

#include "port/debug_field.h"

// The existing libntr input editor, made embeddable by our panel patch.
extern void GUI_PadDrawBindings(void);
extern void GUI_PadCancelCapture(void);
extern bool GUI_PadBlocksNavigation(void);

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
static bool s_backHeld;
// Controller focus regions: header actions, categories, and active page.
// A on a category enters its page. LB/RB cycle regions; B goes back.
static bool s_inHeader;
static bool s_focusHeaderNextFrame;
static bool s_leftShoulderHeld;
static bool s_rightShoulderHeld;
static bool s_focusNavOnOpen;
static bool s_focusPageNextFrame;
static bool s_inPage;
static bool s_quitPromptOpen;
static bool s_cancelQuitPrompt;
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

// Keep native desktop and game input untouched when the dashboard is closed.
// ImGui's SDL2 backend already understands A, D-pad, and left-stick navigation;
// we enable it only while the dashboard is visible and not capturing bindings.
static void DashboardSetOpen(bool open)
{
    s_menuOpen = open;
    if (open) {
        s_inPage = false;
        s_inHeader = false;
        s_focusHeaderNextFrame = false;
        s_focusNavOnOpen = true;
        s_focusPageNextFrame = false;
        s_leftShoulderHeld = s_rightShoulderHeld = false;
    } else {
        GUI_PadCancelCapture();
        s_inPage = false;
        s_inHeader = false;
        s_focusHeaderNextFrame = false;
        s_focusPageNextFrame = false;
        s_leftShoulderHeld = s_rightShoulderHeld = false;
        s_backHeld = false;
        // If a quit confirmation was showing, dismiss it on reopening.
        if (s_quitPromptOpen) s_cancelQuitPrompt = true;
    }
}

// Request navigation focus on the next selectable item in the page child.
// Call this immediately before its first interactive control, not before
// headings or separators, which do not participate in gamepad navigation.
static void DashboardFocusPageItem(void)
{
    if (s_focusPageNextFrame) {
        igSetKeyboardFocusHere(0);
        s_focusPageNextFrame = false;
    }
}

// B leaves either the header or the active page for the category list;
// B from categories closes the dashboard. Popups and binding capture win.
static int DashboardBackEvent(bool pressed)
{
    if (!pressed) {
        s_backHeld = false;
        return 0;
    }
    if (!s_menuOpen || GUI_PadBlocksNavigation()) return 0;
    if (s_backHeld) return 1; // Dedupe SDL joystick + controller events.
    if (s_quitPromptOpen) {
        s_backHeld = true;
        s_cancelQuitPrompt = true;
        return 1;
    }
    if (igIsPopupOpen_Str(NULL, ImGuiPopupFlags_AnyPopup)) return 0;
    s_backHeld = true;
    if (s_inPage || s_inHeader) {
        s_inPage = false;
        s_inHeader = false;
        s_focusHeaderNextFrame = false;
        s_focusPageNextFrame = false;
        s_focusNavOnOpen = true;
    } else {
        DashboardSetOpen(false);
    }
    return 1;
}

// Selecting a category always enters its corresponding content panel.
// The next selectable ImGui item in that child receives controller focus.
static void DashboardEnterPage(int page)
{
    if (s_menuPage == 2 && page != 2) GUI_PadCancelCapture();
    s_menuPage = page;
    s_inPage = true;
    s_inHeader = false;
    s_focusHeaderNextFrame = false;
    s_focusNavOnOpen = false;
    s_focusPageNextFrame = true;
}

// LB/RB moves among header, category list, and selected page. This gives the
// header buttons an explicit gamepad focus route even though they live in
// the dashboard's root window rather than a child panel.
static int DashboardShoulderEvent(bool right, bool pressed)
{
    bool *held = right ? &s_rightShoulderHeld : &s_leftShoulderHeld;
    if (!pressed) {
        *held = false;
        return 0;
    }
    if (!s_menuOpen || GUI_PadBlocksNavigation()) return 0;
    if (*held) return 1;
    *held = true;
    // A modal or dropdown owns focus until dismissed; do not leave it.
    if (s_quitPromptOpen || igIsPopupOpen_Str(NULL, ImGuiPopupFlags_AnyPopup)) return 1;

    const int current = s_inHeader ? 0 : (s_inPage ? 2 : 1);
    const int next = (current + (right ? 1 : 2)) % 3;
    s_inHeader = next == 0;
    s_inPage = next == 2;
    s_focusHeaderNextFrame = next == 0;
    s_focusNavOnOpen = next == 1;
    s_focusPageNextFrame = next == 2;
    return 1;
}

// A standardized controller may also send raw joystick events. Map physical
// B to its raw index, falling back to the existing game.ini assignment.
static int DashboardRawBButton(SDL_JoystickID instance)
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) != instance) continue;
        if (!SDL_IsGameController(i)) break;
        SDL_GameController *pad = SDL_GameControllerOpen(i);
        if (!pad) break;
        SDL_GameControllerButtonBind b = SDL_GameControllerGetBindForButton(pad, SDL_CONTROLLER_BUTTON_B);
        SDL_GameControllerClose(pad);
        if (b.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON) return b.value.button;
        break;
    }
    const int fallback = SIM_GetConfigPtr()->padSettings.bJoyKey;
    return fallback >= 0 && fallback < 256 ? fallback : -1;
}

// SDL can report a controller press as both a raw joystick event and a
// standardized controller event. Resolve LB/RB from the same controller map
// used for L3/R3, and let the held flags deduplicate the two event streams.
// Unmapped joysticks retain their existing controls without guessing indices.
static int DashboardRawShoulder(SDL_JoystickID instance, int button)
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) != instance) continue;
        if (!SDL_IsGameController(i)) return -1;
        SDL_GameController *pad = SDL_GameControllerOpen(i);
        if (!pad) return -1;
        SDL_GameControllerButtonBind left = SDL_GameControllerGetBindForButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        SDL_GameControllerButtonBind right = SDL_GameControllerGetBindForButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        SDL_GameControllerClose(pad);
        if (left.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && left.value.button == button) return 0;
        if (right.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && right.value.button == button) return 1;
        return -1;
    }
    return -1;
}

// Write shortcut diagnostics only when explicitly requested. For Gaming Mode,
// set LAKE_TRIO_SHORTCUT_LOG to a writable path in Steam launch options.
static void DashboardShortcutLog(const char *format, ...)
{
    const char *path = getenv("LAKE_TRIO_SHORTCUT_LOG");
    if (!path || !*path) return;
    FILE *file = fopen(path, "a");
    if (!file) return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fclose(file);
}

// SDL_JOYBUTTON events carry device-specific button numbers. Ask SDL's
// GameController mapping which raw buttons correspond to L3 and R3; an Xbox
// controller, for example, need not use the numbers saved in game.ini.
// The saved bindings remain the fallback for generic (unmapped) joysticks.
static bool DashboardMappedStickButtons(SDL_JoystickID instance, int *l3, int *r3)
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) != instance) continue;
        if (!SDL_IsGameController(i)) return false;
        SDL_GameController *pad = SDL_GameControllerOpen(i);
        if (!pad) return false;
        SDL_GameControllerButtonBind left = SDL_GameControllerGetBindForButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK);
        SDL_GameControllerButtonBind right = SDL_GameControllerGetBindForButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        SDL_GameControllerClose(pad);
        if (left.bindType != SDL_CONTROLLER_BINDTYPE_BUTTON || right.bindType != SDL_CONTROLLER_BINDTYPE_BUTTON
            || left.value.button == right.value.button) return false;
        *l3 = left.value.button;
        *r3 = right.value.button;
        return true;
    }
    return false;
}

// Both normalized controller events and raw joystick events use the same
// latched state machine. This prevents a single chord from toggling twice when
// SDL emits both event types for the same physical input.
static int DashboardStickEvent(SDL_JoystickID instance, int button, int l3, int r3, bool pressed)
{
    if (button != l3 && button != r3) return 0;
    if (s_shortcutDevice != instance) {
        s_shortcutDevice = instance;
        s_l3Held = false;
        s_r3Held = false;
        s_comboLatched = false;
    }
    if (button == l3) s_l3Held = pressed;
    if (button == r3) s_r3Held = pressed;
    if (!pressed && !s_l3Held && !s_r3Held) s_comboLatched = false;
    if (s_l3Held && s_r3Held && !s_comboLatched) {
        s_comboLatched = true;
        DashboardSetOpen(!s_menuOpen);
        DashboardShortcutLog("dashboard %s via L3+R3 (device=%d)\n", s_menuOpen ? "open" : "closed", (int)instance);
        return 1;
    }
    return 0;
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
                DashboardSetOpen(!s_menuOpen);
            }
            return 1;
        }
    }
    if (event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP) {
        if (GUI_PadBlocksNavigation()) {
            s_l3Held = s_r3Held = s_comboLatched = s_backHeld = false;
            s_leftShoulderHeld = s_rightShoulderHeld = false;
            return 0;
        }
        const SIM_config_type *config = SIM_GetConfigPtr();
        int l3 = config->padSettings.l3JoyKey;
        int r3 = config->padSettings.r3JoyKey;
        const bool mapped = DashboardMappedStickButtons(event->jbutton.which, &l3, &r3);
        DashboardShortcutLog("joystick %s device=%d button=%u L3=%d R3=%d source=%s\n",
            event->type == SDL_JOYBUTTONDOWN ? "down" : "up", (int)event->jbutton.which,
            (unsigned)event->jbutton.button, l3, r3, mapped ? "SDL-map" : "game.ini");
        if (l3 < 0 || r3 < 0 || l3 == r3 || l3 > 255 || r3 > 255) return 0;
        const int stickToggle = DashboardStickEvent(event->jbutton.which, event->jbutton.button, l3, r3,
            event->type == SDL_JOYBUTTONDOWN);
        if (stickToggle) return 1;
        if (s_menuOpen) {
            const int shoulder = DashboardRawShoulder(event->jbutton.which, event->jbutton.button);
            if (shoulder >= 0 && DashboardShoulderEvent(shoulder == 1,
                    event->type == SDL_JOYBUTTONDOWN)) return 1;
        }
        if (s_menuOpen && event->jbutton.button != l3 && event->jbutton.button != r3
            && event->jbutton.button == DashboardRawBButton(event->jbutton.which)) {
            return DashboardBackEvent(event->type == SDL_JOYBUTTONDOWN);
        }
        return 0;
    }
    if (event->type == SDL_CONTROLLERBUTTONDOWN || event->type == SDL_CONTROLLERBUTTONUP) {
        if (GUI_PadBlocksNavigation()) {
            s_l3Held = s_r3Held = s_comboLatched = s_backHeld = false;
            s_leftShoulderHeld = s_rightShoulderHeld = false;
            return 0;
        }
        DashboardShortcutLog("controller %s device=%d button=%u (SDL L3=%d R3=%d)\n",
            event->type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up", (int)event->cbutton.which,
            (unsigned)event->cbutton.button, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        if (event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER
            || event->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
            const bool right = event->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
            if (DashboardShoulderEvent(right, event->type == SDL_CONTROLLERBUTTONDOWN)) return 1;
        }
        const int stickToggle = DashboardStickEvent(event->cbutton.which, event->cbutton.button,
            SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
            event->type == SDL_CONTROLLERBUTTONDOWN);
        if (stickToggle) return 1;
        if (event->cbutton.button == SDL_CONTROLLER_BUTTON_B && s_menuOpen) {
            return DashboardBackEvent(event->type == SDL_CONTROLLERBUTTONDOWN);
        }
        return 0;
    }
    if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        // Avoid a held shoulder/back press staying latched after a hot-unplug.
        s_leftShoulderHeld = s_rightShoulderHeld = s_backHeld = false;
    }
    if ((event->type == SDL_JOYDEVICEREMOVED && event->jdevice.which == s_shortcutDevice)
        || (event->type == SDL_CONTROLLERDEVICEREMOVED && event->cdevice.which == s_shortcutDevice)) {
        s_shortcutDevice = -1;
        s_l3Held = s_r3Held = s_comboLatched = s_backHeld = false;
        s_leftShoulderHeld = s_rightShoulderHeld = false;
    }
    return 0;
}

// List the *selected display's* supported pixel dimensions instead of
// renaming the simulator's x1-x4 window multipliers. SDL can report multiple
// refresh rates for the same resolution: show each size only once.
#define LAKE_TRIO_MAX_RESOLUTIONS 48

typedef struct DashboardResolution {
    int width;
    int height;
    char label[80];
} DashboardResolution;

static int DashboardAddResolution(DashboardResolution *entries, int count,
    int width, int height, const char *suffix)
{
    if (width < 640 || height < 480 || width > 8192 || height > 4320)
        return count;
    for (int i = 0; i < count; ++i) {
        if (entries[i].width == width && entries[i].height == height)
            return count;
    }
    if (count >= LAKE_TRIO_MAX_RESOLUTIONS) return count;
    DashboardResolution *entry = &entries[count];
    entry->width = width;
    entry->height = height;
    const char *standard = width == 1920 && height == 1080 ? " (1080p)"
        : width == 1280 && height == 720 ? " (720p)"
        : width == 2560 && height == 1440 ? " (1440p)"
        : width == 3840 && height == 2160 ? " (4K)" : "";
    snprintf(entry->label, sizeof(entry->label), "%d x %d%s%s",
        width, height, standard, suffix ? suffix : "");
    return count + 1;
}

static void DashboardDisplaySettings(void)
{
    SIM_config_type *config = SIM_GetConfigPtr();
    const char *fpsModes[] = { "30 FPS", "60 FPS", "Auto (display refresh)", "Unlimited" };
    const char *renderModes[] = { "1x", "2x", "3x", "4x" };
    const char *layoutNames[] = { "Vertical", "Horizontal", "Large Screen" };

    igTextUnformatted("Display & Performance", NULL);
    igSeparator();
    // ROM import initializes the simulator's normal update/rendering path.
    if (SIM_GetMissingResourcePath() != NULL) {
        igTextWrapped("Import ROM data first; display preferences are available before starting a new game.");
        return;
    }
    DashboardFocusPageItem();
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
    igSeparator();
    igTextUnformatted("Window resolution", NULL);

    DashboardResolution modes[LAKE_TRIO_MAX_RESOLUTIONS];
    int count = 0;
    const int display = (int)SIM_GetMonitor() - 1;
    SDL_DisplayMode desktop;
    const bool hasDesktop = SDL_GetDesktopDisplayMode(display, &desktop) == 0;
    if (hasDesktop) {
        count = DashboardAddResolution(modes, count, desktop.w, desktop.h, " (Desktop)");
    }
    const int modeCount = SDL_GetNumDisplayModes(display);
    for (int i = 0; i < modeCount; ++i) {
        SDL_DisplayMode mode;
        if (SDL_GetDisplayMode(display, i, &mode) != 0) continue;
        // Windowed sizes bigger than the selected desktop are difficult to
        // use, especially in SteamOS Gaming Mode. Keep the list usable.
        if (hasDesktop && (mode.w > desktop.w || mode.h > desktop.h)) continue;
        count = DashboardAddResolution(modes, count, mode.w, mode.h, NULL);
    }
    if (config->customWindowWidth >= 640 && config->customWindowHeight >= 480) {
        count = DashboardAddResolution(modes, count,
            (int)config->customWindowWidth, (int)config->customWindowHeight,
            " (Saved)");
    }

    // Index zero is an explicit placeholder for legacy x1-x4 sizes. It is
    // never offered as a new resolution: selecting the first real entry must
    // always issue a resize request.
    const char *labels[LAKE_TRIO_MAX_RESOLUTIONS + 1];
    labels[0] = "Choose a resolution...";
    int chosen = 0;
    for (int i = 0; i < count; ++i) {
        labels[i + 1] = modes[i].label;
        if ((SIM_GetWindowSizeMode() == 6 || SIM_GetWindowSizeMode() == 5)
            && config->customWindowWidth == (u32)modes[i].width
            && config->customWindowHeight == (u32)modes[i].height)
            chosen = i + 1;
    }
    if (count > 0) {
        if (igCombo_Str_arr("Window resolution##lake_trio", &chosen,
                labels, count + 1, 12) && chosen > 0) {
            const DashboardResolution *res = &modes[chosen - 1];
            SIM_RequestWindowResolution((u32)res->width, (u32)res->height);
            s_lastWindowedMode = 6;
            // The simulator applies and saves this on its windowing thread.
        }
    } else {
        igTextWrapped("The selected display did not report usable resolutions.");
    }

    int window = (int)SIM_GetWindowSizeMode();
    if (window >= 1 && window <= 4) s_lastWindowedMode = window;
    if (window == 6) s_lastWindowedMode = 6;
    bool fullscreen = window == 5;
    if (igCheckbox("Fullscreen (desktop)", &fullscreen)) {
        SIM_RequestWindowSizeMode(fullscreen ? 5u : (u32)s_lastWindowedMode);
        s_saveConfigNextFrame = true;
    }
    igTextWrapped("These are window sizes reported by your selected display. Desktop is the current desktop resolution, which may differ from the panel's physical native resolution.");

    igSpacing();
    igSeparator();
    igTextUnformatted("Nintendo DS screens", NULL);
    int layout = (int)config->screenLayout;
    if (layout >= 0 && layout < 3
        && igCombo_Str_arr("Screen layout", &layout, layoutNames, 3, 3)) {
        config->screenLayout = (SIM_config_screen_layout_type)layout;
        s_saveConfigNextFrame = true;
    }
    bool swapped = config->swapScreens != 0;
    if (igCheckbox("Swap screens", &swapped)) {
        config->swapScreens = swapped;
        SIM_SetScreensSwapped(swapped);
        s_saveConfigNextFrame = true;
    }

    igSpacing();
    igSeparator();
    int scale = (int)SIM_GetInternalResolutionScale() - 1;
    if (scale >= 0 && scale < 4
        && igCombo_Str_arr("Internal 3D resolution", &scale, renderModes, 4, 4)) {
        SIM_RequestInternalResolutionScale((u32)scale + 1u);
        s_saveConfigNextFrame = true;
    }
    igTextWrapped("Internal 3D resolution changes rendering quality, not the window resolution. Your in-game Display menu remains available.");
    if (config->frameRateMode == SIM_CONFIG_FRAME_RATE_INF)
        igTextWrapped("Unlimited FPS overrides VSync in the simulator.");
}

// Lake-Trio's dashboard-only palette. Exact requested colors:
// background #212830, accent #F29EC4. The render frame itself remains opaque;
// these style changes are popped before any other simulator UI is drawn.
static void DashboardPushTheme(void)
{
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 20.0f, 20.0f });
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 0.0f); // Full viewport.
    igPushStyleVar_Float(ImGuiStyleVar_ChildRounding, 10.0f);
    igPushStyleVar_Float(ImGuiStyleVar_FrameRounding, 8.0f);
    igPushStyleVar_Float(ImGuiStyleVar_PopupRounding, 8.0f);
    igPushStyleVar_Float(ImGuiStyleVar_ScrollbarRounding, 7.0f);
    igPushStyleVar_Float(ImGuiStyleVar_GrabRounding, 6.0f);
    igPushStyleColor_Vec4(ImGuiCol_Text, (ImVec4){ 0.95f, 0.96f, 0.98f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_TextDisabled, (ImVec4){ 0.70f, 0.74f, 0.79f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_WindowBg, (ImVec4){ 33/255.0f, 40/255.0f, 48/255.0f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ChildBg, (ImVec4){ 42/255.0f, 51/255.0f, 61/255.0f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_PopupBg, (ImVec4){ 42/255.0f, 51/255.0f, 61/255.0f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_Border, (ImVec4){ 0.34f, 0.40f, 0.46f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_FrameBg, (ImVec4){ 0.23f, 0.28f, 0.33f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_FrameBgHovered, (ImVec4){ 0.34f, 0.31f, 0.38f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_FrameBgActive, (ImVec4){ 0.42f, 0.32f, 0.41f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){ 0.23f, 0.29f, 0.34f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){ 0.44f, 0.31f, 0.40f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, (ImVec4){ 0.58f, 0.38f, 0.49f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_Header, (ImVec4){ 0.40f, 0.30f, 0.38f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_HeaderHovered, (ImVec4){ 0.50f, 0.34f, 0.45f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_HeaderActive, (ImVec4){ 0.60f, 0.40f, 0.51f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_CheckMark, (ImVec4){ 242/255.0f, 158/255.0f, 196/255.0f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_SliderGrab, (ImVec4){ 242/255.0f, 158/255.0f, 196/255.0f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_SliderGrabActive, (ImVec4){ 1.0f, 0.77f, 0.88f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_NavCursor, (ImVec4){ 242/255.0f, 158/255.0f, 196/255.0f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_Separator, (ImVec4){ 0.32f, 0.38f, 0.44f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ScrollbarBg, (ImVec4){ 33/255.0f, 40/255.0f, 48/255.0f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ScrollbarGrab, (ImVec4){ 0.38f, 0.43f, 0.49f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ScrollbarGrabHovered, (ImVec4){ 0.52f, 0.41f, 0.49f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ScrollbarGrabActive, (ImVec4){ 0.65f, 0.48f, 0.57f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_TableHeaderBg, (ImVec4){ 0.26f, 0.32f, 0.37f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_TableRowBg, (ImVec4){ 0.18f, 0.22f, 0.27f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_TableRowBgAlt, (ImVec4){ 0.20f, 0.25f, 0.30f, 1.0f });
}

static void DashboardPopTheme(void)
{
    igPopStyleColor(27);
    igPopStyleVar(7);
}

// Called once per ImGui frame by libntr after pending simulator requests have
// been applied. Save on the following frame so game.ini receives new values.
void SIM_GUI_Prj_Toolbar(int canImport)
{
    if (!DashboardEnabled()) return;
    // Enable Dear ImGui's existing SDL2 gamepad navigation while the overlay
    // is visible. During input rebinding, the next button belongs to the editor.
    ImGuiIO *io = igGetIO_ContextPtr(igGetCurrentContext());
    if (s_menuOpen && !GUI_PadBlocksNavigation())
        io->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else
        io->ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    if (s_saveConfigNextFrame) {
        SIM_Config_SaveGameConfigFile(SIM_GetConfigPtr());
        s_saveConfigNextFrame = false;
    }
    if (!s_menuOpen) return;

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
    // Keep both header buttons clear of the navigation region on narrow views.
    const bool compactHeader = width < 390.0f;
    const float navTop = compactHeader ? 116.0f : 76.0f;
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
    igSetNextWindowBgAlpha(1.0f);
    DashboardPushTheme();

    if (igBegin("Lake-Trio##dashboard", NULL, flags)) {
        igTextUnformatted("Lake Trio", NULL);
        // Resume and Quit are adjacent on normal viewports; stack them on
        // narrow windows so neither overlaps the navigation panel.
        if (compactHeader) igSpacing();
        else igSameLine(0.0f, 20.0f);
        // Focus the first header action on the next frame after LB/RB.
        // D-pad right/down then reaches Quit Game using standard ImGui nav.
        if (s_focusHeaderNextFrame) {
            igSetKeyboardFocusHere(0);
            s_focusHeaderNextFrame = false;
        }
        if (igButton("Resume", (ImVec2){ 115.0f, 32.0f })) {
            DashboardSetOpen(false);
        }
        if (!compactHeader) igSameLine(0.0f, 8.0f);
        if (igButton("Quit Game", (ImVec2){ 115.0f, 32.0f })) {
            s_quitPromptOpen = true;
            s_cancelQuitPrompt = false;
            igOpenPopup_Str("Quit Lake Trio?", 0);
        }
        igSeparator();

        // Send SDL_QUIT through the same event loop used by window-close.
        // The confirmation prevents accidentally ending an unsaved session.
        if (igBeginPopupModal("Quit Lake Trio?", &s_quitPromptOpen,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            igTextWrapped("Quit Lake Trio? Unsaved game progress may be lost.");
            if (s_cancelQuitPrompt) {
                s_cancelQuitPrompt = false;
                s_quitPromptOpen = false;
                igCloseCurrentPopup();
            } else {
                if (igButton("Cancel", (ImVec2){ 115.0f, 32.0f })) {
                    s_quitPromptOpen = false;
                    igCloseCurrentPopup();
                }
                igSameLine(0.0f, 8.0f);
                if (igButton("Quit##confirm", (ImVec2){ 115.0f, 32.0f })) {
                    SDL_Event event = {0};
                    event.type = SDL_QUIT;
                    SDL_PushEvent(&event);
                    s_quitPromptOpen = false;
                    igCloseCurrentPopup();
                }
            }
            igEndPopup();
        }

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
                    igPushStyleColor_Vec4(ImGuiCol_Button,
                        (ImVec4){ 242/255.0f, 158/255.0f, 196/255.0f, 1.0f });
                    igPushStyleColor_Vec4(ImGuiCol_Text,
                        (ImVec4){ 33/255.0f, 40/255.0f, 48/255.0f, 1.0f });
                }
                if (s_focusNavOnOpen && selected) {
                    igSetKeyboardFocusHere(0);
                    s_focusNavOnOpen = false;
                }
                const bool clicked = igButton(labels[i], (ImVec2){ cellWidth, 38.0f });
                if (selected) igPopStyleColor(2);
                if (clicked) DashboardEnterPage(i);
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
                    DashboardFocusPageItem();
                    if (igButton("Import", (ImVec2){ 170.0f, 38.0f }) && canImport) {
                        PlatformMenu_RequestAction(LAKE_TRIO_MENU_IMPORT_ROM);
                        DashboardSetOpen(false);
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
                    DashboardFocusPageItem();
                    GUI_PadDrawBindings();
                    break;
                }
            }
            igEndChild();
        }
    }
    igEnd();
    DashboardPopTheme();
}
