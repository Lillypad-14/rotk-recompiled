// A plain host for a cartridge other than ROTK: The Two Towers (docs/SUPER_GAME.md). Its PC
// extras are color vision correction and ROTK hero sprites (src/rotk_sprites.cpp), both carried over
// from ROTK's settings. The ROTK
// host's hooks are tied to ROTK addresses, so it can't run another cartridge.
//
// Started by the ROTK host for the super game, it takes over ROTK's window: the window opens hidden
// (GBARECOMP_WINDOW_HANDOFF="x,y,w,h,fullscreen"), and once a frame is drawn it takes ROTK's place,
// size and fullscreen state, shows itself and signals ROTK_SUPER_GAME_READY, the event ROTK waits on
// before it hides its own window. Started that way it skips the language menu (English) and the logos,
// and shows itself once its main menu has faded in. Until then it runs at fast-forward speed, silent. Its main menu then has a fifth row, Return of the
// King, which hands the window back: it writes its place, size and fullscreen state to
// ROTK_SUPER_GAME_BACK_FILE, sets ROTK_SUPER_GAME_BACK (ROTK shows itself there) and quits.
#include "runtime.h"

#include "color_vision.h"
#include "rotk_sprites.h"

#include "gba_bus.h"
#include "mod_function_hooks.h"
#include "mod_runtime.h"
#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#endif
// this file has the program's main
#define SDL_MAIN_HANDLED
#include <SDL.h>

namespace {

// The Two Towers' scene state and its scenes (scene table at 0x080561C0, 12 bytes a scene).
constexpr std::uint32_t kSceneCurrent = 0x03003A50;
constexpr std::uint32_t kSceneArgCursor = kSceneCurrent + 8;
constexpr std::uint32_t kSceneMainMenu = 4;
constexpr std::uint32_t kLanguageEnglish = 0;
constexpr std::uint32_t kSceneLanguageSelect = 0x12;
constexpr std::uint32_t kCopyrightTick = 0x0802626C;
constexpr std::uint32_t kLanguageSelectTick = 0x08025EC8;
// The main menu's state (args[3]): 0 is "Press START", which its tick leaves through the Start
// handler.
constexpr std::uint32_t kSceneArgMenuState = kSceneCurrent + 4 + 3 * 4;
constexpr std::uint32_t kMenuPressStart = 0;
// its rows drawn, waiting for the player
constexpr std::uint32_t kMenuIdle = 2;
constexpr std::uint32_t kMainMenuTick = 0x0803F054;
constexpr std::uint32_t kMainMenuStart = 0x0803F32C;
constexpr std::uint32_t kLanguageSelectConfirm = 0x08025F88;
// g_SceneNext as game_requestSceneChange (0x0802B110) fills it: the scene id with the request flag,
// then its five args.
constexpr std::uint32_t kSceneNext = 0x03003A90;
constexpr std::uint32_t kSceneRequested = 0x800;
constexpr std::uint32_t kSceneArgCount = 5;
constexpr const char* kSkipIntroHookId = "ttt.super-game-skip-intro";
constexpr const char* kSkipCopyrightHookId = "ttt.super-game-skip-copyright";
constexpr const char* kSkipTitleHookId = "ttt.super-game-skip-title";
constexpr const char* kMenuRowHookId = "ttt.super-game-menu-row";
constexpr const char* kMenuCursorHookId = "ttt.super-game-menu-cursor";
constexpr const char* kMenuConfirmHookId = "ttt.super-game-menu-confirm";

// The main menu's rows (scene_mainMenu_drawEntry 0x0803F2EC): text 0x0D + row in font style 0, 16 BG
// tiles a row from tile 1, centered at x 120, 8 pixels apart, color 0 selected, else 3. The game's
// four start at y 96. Five start a tile higher, as a fifth at y 128 would be under the bottom border.
constexpr std::uint32_t kMainMenuDrawEntry = 0x0803F2EC;
constexpr std::uint32_t kMainMenuMoveCursor = 0x0803F180;
constexpr std::uint32_t kMainMenuConfirm = 0x0803F0FC;
constexpr std::uint32_t kStockRows = 4;
constexpr std::uint32_t kRowReturn = kStockRows;
constexpr std::uint32_t kRowTextBase = 0x0D;
constexpr std::uint32_t kRowTiles = 0x10;
constexpr std::uint32_t kRowX = 0x78;
constexpr std::uint32_t kRowY = 0x58;
constexpr std::uint32_t kRowHeight = 8;
constexpr std::uint32_t kColorSelected = 0;
constexpr std::uint32_t kColor = 3;
constexpr std::uint32_t kFontAlignCenter = 1;
constexpr std::uint32_t kFontSetStyle = 0x080156B0;
constexpr std::uint32_t kFontDrawString = 0x080151F4;
constexpr std::uint32_t kTextGetString = 0x0803EA24;
// plays a sound and returns its handle (the menu keeps it in g_MainMenuSfxHandle)
constexpr std::uint32_t kSoundPlay = 0x0803DCA0;
constexpr std::uint32_t kMainMenuSfxHandle = 0x03002618;
constexpr std::uint32_t kSoundCursor = 0x1A;
constexpr std::uint32_t kSoundConfirm = 0x1B;
constexpr std::uint32_t kKeysPressed = 0x030038E0;
constexpr std::uint32_t kActivePlayerIndex = 0x03003641;
constexpr std::uint32_t kKeyUp = 1u << 6;
constexpr std::uint32_t kKeyDown = 1u << 7;
// The row's text, in host RAM (0x12000000, the PC-only work RAM this cartridge leaves alone).
constexpr std::uint32_t kReturnText = 0x12000000;
constexpr const char* kReturnTextValue = "Return of the King";
// The window shows once the main menu's rows have been up this long, or after the fallback, whatever
// the game is doing.
constexpr int kMenuShownFrames = 4;
constexpr int kRevealFallbackFrames = 900;
constexpr int kMaxSdlWindowId = 16;

bool g_handoff = false;
// started by ROTK for the super game, for the whole session
bool g_fromRotk = false;
int g_frames = 0;
int g_menuFrames = 0;

void requestScene(std::uint32_t scene)
{
    // as game_requestSceneChange does
    bus_write_u32(kSceneNext, scene | kSceneRequested);
    for (std::uint32_t arg = 0; arg < kSceneArgCount; ++arg) bus_write_u32(kSceneNext + 4 + arg * 4, 0);
}

int returnToCaller(ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    cpu->R[15] = lr & ~1u;
    if (lr & 1u) cpu->cpsr |= 0x20u;
    return 1;
}

// The copyright screen's tick: on to the language menu at once.
int skipCopyright(std::uint32_t, int, ArmCpuState* cpu)
{
    requestScene(kSceneLanguageSelect);
    return returnToCaller(cpu);
}

// Calls a Thumb guest function as a BL from `ret` would (the generated code's call sequence): its
// return comes back here. False when it left some other way.
bool callGuest(ArmCpuState* cpu, std::uint32_t target, std::uint32_t ret)
{
    cpu->R[14] = ret | 1u;
    cpu->R[15] = target;
    runtime_call_push_return(ret);
    runtime_dispatch(target);
    if (cpu->R[15] != ret) {
        runtime_call_cancel_return(ret);
        return false;
    }
    return true;
}

// The language menu's first tick: confirm English the way the menu does (it sets the language and
// stamps the save), then ask for the main menu over the logos the confirm leads to.
int skipIntro(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    const std::uint32_t ret = lr & ~1u;
    bus_write_u32(kSceneArgCursor, kLanguageEnglish);
    callGuest(cpu, kLanguageSelectConfirm, ret);
    // written over the confirm's request
    requestScene(kSceneMainMenu);
    cpu->R[14] = lr;
    return returnToCaller(cpu);
}

// The title's tick while it says "Press START": Start is pressed for the player.
int skipTitle(std::uint32_t, int, ArmCpuState* cpu)
{
    if (bus_read_u32(kSceneArgMenuState) != kMenuPressStart) return 0;
    const std::uint32_t lr = cpu->R[14];
    callGuest(cpu, kMainMenuStart, lr & ~1u);
    cpu->R[14] = lr;
    return returnToCaller(cpu);
}

// A Thumb guest function's call with a fifth argument, on the guest stack as the caller would pass it.
bool callGuest5(ArmCpuState* cpu, std::uint32_t target, std::uint32_t ret, std::uint32_t fifth)
{
    cpu->R[13] -= 4;
    bus_write_u32(cpu->R[13], fifth);
    const bool returned = callGuest(cpu, target, ret);
    cpu->R[13] += 4;
    return returned;
}

void drawRow(ArmCpuState* cpu, std::uint32_t ret, std::uint32_t row, std::uint32_t selected)
{
    std::uint32_t text = kReturnText;
    if (row == kRowReturn) {
        // written here, as the boot clears the RAM
        for (std::uint32_t i = 0;; ++i) {
            bus_write_u8(kReturnText + i, static_cast<std::uint8_t>(kReturnTextValue[i]));
            if (!kReturnTextValue[i]) break;
        }
    } else {
        cpu->R[0] = kRowTextBase + row;
        callGuest(cpu, kTextGetString, ret);
        text = cpu->R[0];
    }
    cpu->R[0] = 0;
    cpu->R[1] = row == selected ? kColorSelected : kColor;
    cpu->R[2] = 0;
    callGuest(cpu, kFontSetStyle, ret);
    cpu->R[0] = row * kRowTiles + 1;
    cpu->R[1] = kRowX;
    cpu->R[2] = kRowY + row * kRowHeight;
    cpu->R[3] = text;
    callGuest5(cpu, kFontDrawString, ret, kFontAlignCenter);
}

void playMenuSound(ArmCpuState* cpu, std::uint32_t ret, std::uint32_t sound)
{
    cpu->R[0] = sound;
    callGuest(cpu, kSoundPlay, ret);
    bus_write_u32(kMainMenuSfxHandle, cpu->R[0]);
}

// One main menu row. The redraw loop stops at the stock four, so the last of them also draws Return of
// the King.
int menuRow(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    const std::uint32_t row = cpu->R[0], selected = cpu->R[1];
    drawRow(cpu, lr & ~1u, row, selected);
    if (row == kStockRows - 1) drawRow(cpu, lr & ~1u, kRowReturn, selected);
    cpu->R[14] = lr;
    return returnToCaller(cpu);
}

// The d-pad on the main menu: the cursor wraps over the five rows.
int menuCursor(std::uint32_t, int, ArmCpuState* cpu)
{
    const std::uint32_t lr = cpu->R[14];
    const std::uint32_t keys = bus_read_u16(kKeysPressed + bus_read_u8(kActivePlayerIndex) * 2);
    const std::uint32_t rows = kStockRows + 1;
    const std::uint32_t previous = bus_read_u32(kSceneArgCursor);
    std::uint32_t cursor = previous;
    if (keys & kKeyDown) cursor = (cursor + 1) % rows;
    else if (keys & kKeyUp) cursor = (cursor + rows - 1) % rows;
    bus_write_u32(kSceneArgCursor, cursor);
    drawRow(cpu, lr & ~1u, previous, cursor);
    drawRow(cpu, lr & ~1u, cursor, cursor);
    playMenuSound(cpu, lr & ~1u, kSoundCursor);
    cpu->R[14] = lr;
    return returnToCaller(cpu);
}

SDL_Window* shownWindow()
{
    for (int id = 1; id <= kMaxSdlWindowId; ++id)
        if (SDL_Window* window = SDL_GetWindowFromID(static_cast<Uint32>(id)))
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) return window;
    return nullptr;
}

// Return of the King: the window goes back to ROTK where this one is, and this game quits.
void handBack()
{
    int x = 0, y = 0, w = 0, h = 0, fullscreen = 0;
    if (SDL_Window* window = shownWindow()) {
        SDL_GetWindowPosition(window, &x, &y);
        SDL_GetWindowSize(window, &w, &h);
        fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    }
    if (const char* path = std::getenv("ROTK_SUPER_GAME_BACK_FILE")) {
        if (std::FILE* file = std::fopen(path, "w")) {
            std::fprintf(file, "%d,%d,%d,%d,%d\n", x, y, w, h, fullscreen);
            std::fclose(file);
        }
    }
#if defined(_WIN32)
    if (const char* name = std::getenv("ROTK_SUPER_GAME_BACK")) {
        if (HANDLE back = OpenEventA(EVENT_MODIFY_STATE, FALSE, name)) {
            SetEvent(back);
            CloseHandle(back);
        }
    }
#endif
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

// A on the main menu: Return of the King hands the window back. The other rows run as the game has them.
int menuConfirm(std::uint32_t, int, ArmCpuState* cpu)
{
    if (bus_read_u32(kSceneArgCursor) != kRowReturn) return 0;
    const std::uint32_t lr = cpu->R[14];
    playMenuSound(cpu, lr & ~1u, kSoundConfirm);
    handBack();
    cpu->R[14] = lr;
    return returnToCaller(cpu);
}

void enableSkipIntro(void)
{
    gba_mod_set_function_hook_enabled(kMenuRowHookId, g_fromRotk ? 1 : 0);
    gba_mod_set_function_hook_enabled(kMenuCursorHookId, g_fromRotk ? 1 : 0);
    gba_mod_set_function_hook_enabled(kMenuConfirmHookId, g_fromRotk ? 1 : 0);
    gba_mod_set_function_hook_enabled(kSkipIntroHookId, g_handoff ? 1 : 0);
    gba_mod_set_function_hook_enabled(kSkipCopyrightHookId, g_handoff ? 1 : 0);
    gba_mod_set_function_hook_enabled(kSkipTitleHookId, g_handoff ? 1 : 0);
}

SDL_Window* hiddenWindow()
{
    for (int id = 1; id <= kMaxSdlWindowId; ++id)
        if (SDL_Window* window = SDL_GetWindowFromID(static_cast<Uint32>(id)))
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) return window;
    return nullptr;
}

void reveal()
{
    gbarecomp::set_host_fast_forward(false);
    int x = 0, y = 0, w = 0, h = 0, fullscreen = 0;
    const char* geometry = std::getenv("GBARECOMP_WINDOW_HANDOFF");
    SDL_Window* window = hiddenWindow();
    if (window && geometry && std::sscanf(geometry, "%d,%d,%d,%d,%d", &x, &y, &w, &h, &fullscreen) == 5) {
        if (fullscreen) {
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else {
            // ROTK's window can be widescreen; this game has no widescreen, so its window takes the
            // GBA's 3:2 shape at that height, centered where ROTK's was.
            if (w > 0 && h > 0) {
                const int gbaWidth = h * 3 / 2;
                x += (w - gbaWidth) / 2;
                w = gbaWidth;
                SDL_SetWindowSize(window, w, h);
            }
            SDL_SetWindowPosition(window, x, y);
        }
    }
    if (window) {
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
    }
#if defined(_WIN32)
    if (const char* name = std::getenv("ROTK_SUPER_GAME_READY")) {
        if (HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, name)) {
            SetEvent(ready);
            CloseHandle(ready);
        }
    }
#endif
}

void presentDecorate(const gbarecomp::PresentFrame* frame)
{
    rotk::colorVisionDecorate(frame);
    if (!g_handoff) return;
    ++g_frames;
    if (frame->bus && frame->bus->read32(kSceneCurrent) == kSceneMainMenu &&
        frame->bus->read32(kSceneArgMenuState) == kMenuIdle)
        ++g_menuFrames;
    if (g_menuFrames >= kMenuShownFrames || g_frames >= kRevealFallbackFrames) {
        reveal();
        g_handoff = false;
    }
}

bool presentDecorateActive()
{
    return g_handoff || rotk::colorVisionActive();
}

}  // namespace

int main(int argc, char** argv)
{
    gbarecomp::RunOptions options;
    options.builtin_game_name = ROTK_GAME_NAME;
    options.builtin_rom_sha1 = ROTK_ROM_SHA1;
    options.builtin_rom_crc32 = ROTK_ROM_CRC32;
    options.mod_game_id = ROTK_MOD_GAME_ID;
    options.expose_assist_tools = true;
    g_handoff = std::getenv("GBARECOMP_WINDOW_HANDOFF") != nullptr;
    g_fromRotk = g_handoff;
    // out of sight it boots at fast-forward speed, so the swap comes quickly
    if (g_handoff) gbarecomp::set_host_fast_forward(true);
    gba_mod_register_function_entry_plugin(kMenuRowHookId, kMainMenuDrawEntry, 1, menuRow);
    gba_mod_register_function_entry_plugin(kMenuCursorHookId, kMainMenuMoveCursor, 1, menuCursor);
    gba_mod_register_function_entry_plugin(kMenuConfirmHookId, kMainMenuConfirm, 1, menuConfirm);
    gba_mod_register_function_entry_plugin(kSkipIntroHookId, kLanguageSelectTick, 1, skipIntro);
    gba_mod_register_function_entry_plugin(kSkipCopyrightHookId, kCopyrightTick, 1, skipCopyright);
    gba_mod_register_function_entry_plugin(kSkipTitleHookId, kMainMenuTick, 1, skipTitle);
    gba_mod_register_reset_callback(enableSkipIntro);
    // ROTK_PC_COLOR_VISION, which ROTK sets to its own choice when it starts this game
    rotk::colorVisionRegister();
    rotk::rotkSpritesRegister();
    options.present_decorate = presentDecorate;
    options.present_decorate_active = presentDecorateActive;
    return gbarecomp::run_game(argc, argv, options);
}
