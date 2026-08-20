#pragma once
// The interface wears the same clothes as the maps it makes: aged parchment,
// brown ink, old gold and a little moss green. The palette is taken straight
// from styles/_base.json, so the app and its output look like one thing.
#include <imgui.h>

namespace dnd {

inline void SetupDarkFantasyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Base geometry & polish
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;

    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    // -- ink and parchment ------------------------------------------------
    // Backgrounds: dark warm brown-black, the colour of a map board in a dim
    // room, never the blue-grey of a code editor.
    colors[ImGuiCol_WindowBg]             = ImVec4(0.098f, 0.086f, 0.075f, 1.00f); // #191614
    colors[ImGuiCol_ChildBg]              = ImVec4(0.133f, 0.114f, 0.098f, 1.00f); // #221D19
    colors[ImGuiCol_PopupBg]              = ImVec4(0.118f, 0.102f, 0.086f, 0.98f);
    colors[ImGuiCol_Border]               = ImVec4(0.290f, 0.251f, 0.220f, 0.75f); // #4A4038
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Text: parchment cream, so it reads as ink on paper inverted.
    colors[ImGuiCol_Text]                 = ImVec4(0.910f, 0.863f, 0.769f, 1.00f); // #E8DCC4
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.580f, 0.529f, 0.447f, 1.00f); // #948772

    // Frames & inputs: worn leather.
    colors[ImGuiCol_FrameBg]              = ImVec4(0.180f, 0.153f, 0.125f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.239f, 0.204f, 0.161f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.294f, 0.243f, 0.184f, 1.00f);

    // Title & menus
    colors[ImGuiCol_TitleBg]              = ImVec4(0.086f, 0.075f, 0.063f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.157f, 0.133f, 0.106f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.086f, 0.075f, 0.063f, 0.75f);
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.118f, 0.102f, 0.086f, 1.00f);

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.098f, 0.086f, 0.075f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.290f, 0.251f, 0.220f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.400f, 0.345f, 0.286f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.788f, 0.635f, 0.196f, 1.00f);

    // Sliders & checkmarks: old gilding.
    colors[ImGuiCol_CheckMark]            = ImVec4(0.851f, 0.694f, 0.259f, 1.00f); // #D9B142
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.741f, 0.596f, 0.196f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.898f, 0.757f, 0.353f, 1.00f);

    // Buttons: dark leather that warms to gold under the hand.
    colors[ImGuiCol_Button]               = ImVec4(0.204f, 0.173f, 0.141f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.600f, 0.463f, 0.169f, 0.90f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.788f, 0.635f, 0.212f, 1.00f);

    // Headers & tree nodes
    colors[ImGuiCol_Header]               = ImVec4(0.231f, 0.196f, 0.157f, 0.85f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.322f, 0.271f, 0.204f, 0.90f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.694f, 0.541f, 0.196f, 0.90f);

    // Tabs
    colors[ImGuiCol_Tab]                  = ImVec4(0.141f, 0.122f, 0.102f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.694f, 0.541f, 0.184f, 0.85f);
    colors[ImGuiCol_TabSelected]          = ImVec4(0.259f, 0.220f, 0.176f, 1.00f);
    colors[ImGuiCol_TabDimmed]            = ImVec4(0.106f, 0.094f, 0.078f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected]    = ImVec4(0.176f, 0.153f, 0.125f, 1.00f);

    // Separators: ruled lines in brown ink.
    colors[ImGuiCol_Separator]            = ImVec4(0.290f, 0.251f, 0.220f, 0.70f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.788f, 0.635f, 0.196f, 0.78f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(0.788f, 0.635f, 0.196f, 1.00f);

    // Resize grip
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.788f, 0.635f, 0.196f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.788f, 0.635f, 0.196f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.788f, 0.635f, 0.196f, 0.95f);
}

// The accent used for headings and highlights throughout the app.
inline ImVec4 AccentGold() { return ImVec4(0.851f, 0.694f, 0.259f, 1.00f); }

// The colour of a primary action - the moss green from the shared palette,
// which sits beside parchment without shouting.
inline ImVec4 GoButton()        { return ImVec4(0.290f, 0.361f, 0.243f, 1.00f); }
inline ImVec4 GoButtonHovered() { return ImVec4(0.384f, 0.478f, 0.322f, 1.00f); }

} // namespace dnd
