#include <nitro.h>
#include <simulator/sim.h>
#include "simulator/gui.h"
#include <stdbool.h>
#include <stdlib.h>

#include "app_menu_actions.h"

#include <simulator/gui.h>
#include "sim_gui_prj.h"

#include "port/debug_field.h"

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

// Stage 1B: lightweight, shared ImGui toolbar.
// The original native File/Input menu remains available as a fallback.
// Do not move ROM import or control configuration into this UI: dispatch the
// same commands that the native Windows/Qt/AppKit menus already use.
void SIM_GUI_Prj_Toolbar(int canImport)
{
    // Set LAKE_TRIO_UI_DISABLE=1 to compare behavior with the original UI.
    const char *disable = getenv("LAKE_TRIO_UI_DISABLE");
    if (disable != NULL && disable[0] == '1' && disable[1] == '\0') {
        return;
    }

    ImVec2 pos = { 12.0f, 12.0f };
    ImVec2 pivot = { 0.0f, 0.0f };
    ImVec2 buttonSize = { 128.0f, 30.0f };
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_AlwaysAutoResize;

    igSetNextWindowPos(pos, ImGuiCond_Always, pivot);
    igSetNextWindowBgAlpha(0.96f);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2){ 12.0f, 10.0f });
    igPushStyleVar_Float(ImGuiStyleVar_WindowRounding, 9.0f);
    igPushStyleColor_Vec4(ImGuiCol_WindowBg, (ImVec4){ 0.065f, 0.082f, 0.13f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){ 0.15f, 0.27f, 0.44f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, (ImVec4){ 0.22f, 0.39f, 0.63f, 1.0f });
    igPushStyleColor_Vec4(ImGuiCol_ButtonActive, (ImVec4){ 0.12f, 0.32f, 0.57f, 1.0f });

    if (igBegin("Lake-Trio##application_toolbar", NULL, flags)) {
        ImGuiIO *io = igGetIO_ContextPtr(igGetCurrentContext());
        const bool compact = io->DisplaySize.x < 440.0f;
        igTextUnformatted("LAKE-TRIO", NULL);
        if (!compact) igSameLine(0.0f, 14.0f);

        if (!canImport) {
            igPushStyleColor_Vec4(ImGuiCol_Button, (ImVec4){ 0.14f, 0.16f, 0.21f, 1.0f });
        }
        bool importClicked = igButton("File / Import ROM", buttonSize);
        if (!canImport) igPopStyleColor(1);
        if (importClicked && canImport) {
            PlatformMenu_RequestAction(LAKE_TRIO_MENU_IMPORT_ROM);
        }

        if (!compact) igSameLine(0.0f, 8.0f);
        if (igButton("Input / Controls", buttonSize)) {
            PlatformMenu_RequestAction(LAKE_TRIO_MENU_CONFIGURE_CONTROLS);
        }
    }
    igEnd();
    igPopStyleColor(4);
    igPopStyleVar(2);
}
