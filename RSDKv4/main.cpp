#include "RetroEngine.hpp"

#if !RETRO_USE_ORIGINAL_CODE

#if RETRO_PLATFORM == RETRO_WIN
#include "Windows.h"
#endif

void parseArguments(int argc, char *argv[])
{
    for (int a = 0; a < argc; ++a) {
        const char *find = "";

        find = strstr(argv[a], "stage=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneFolder[b++] = find[c++];
            Engine.startSceneFolder[b] = 0;
        }

        find = strstr(argv[a], "scene=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneID[b++] = find[c++];
            Engine.startSceneID[b] = 0;
        }

        find = strstr(argv[a], "console=true");
        if (find) {
            engineDebugMode       = true;
            Engine.devMenu        = true;
            Engine.consoleEnabled = true;
#if RETRO_PLATFORM == RETRO_WIN
            AllocConsole();
            freopen_s((FILE **)stdin, "CONIN$", "w", stdin);
            freopen_s((FILE **)stdout, "CONOUT$", "w", stdout);
            freopen_s((FILE **)stderr, "CONOUT$", "w", stderr);
#endif
        }

        find = strstr(argv[a], "usingCWD=true");
        if (find) {
            usingCWD = true;
        }
    }
}
#endif

#if RETRO_USING_KOS
#include <kos.h>
// INIT_FS_PTY is required for stdio (printf) to reach the debug console —
// confirmed by the working RSDKv5 DC port, whose logs appear in emulators.
KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_VMU | INIT_CDROM | INIT_FS_PTY);

// Pre-main probe: static constructors run after KOS subsystem init but
// before main(). If this prints and main()'s line doesn't, the hang is in
// a later ctor; if neither prints, KOS init itself (maple/cdrom/fs) hangs.
void DC_Probe(int phase);

extern "C" __attribute__((constructor(101))) void DC_CtorProbe()
{
    printf("[DC] static ctors running\n");
    fflush(stdout);
    DC_Probe(0);
}
#endif

int main(int argc, char *argv[])
{
#if RETRO_USING_KOS
    printf("[DC] main() entered\n");
    fflush(stdout);
    DC_Probe(1);
#endif
#if !RETRO_USE_ORIGINAL_CODE
    parseArguments(argc, argv);
#endif

#if RETRO_USING_KOS
    printf("[DC] RSDKv4 booting\n");
    fflush(stdout);
    // engine PrintLog output is valuable during bring-up: always on for DC
    Engine.consoleEnabled = true;
#if defined(RSDK_DEBUG) && RSDK_DEBUG
    engineDebugMode = true;
    Engine.devMenu  = true;
#endif
#endif

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
    SDL_SetHint(SDL_HINT_WINRT_HANDLE_BACK_BUTTON, "1");
#endif
    Engine.Init();
#if RETRO_USING_KOS
    printf("[DC] engine init done: running=%d gameMode=%d\n", Engine.running, Engine.gameMode);
    fflush(stdout);
#endif
    Engine.Run();

#if !RETRO_USE_ORIGINAL_CODE
    if (Engine.consoleEnabled) {
#if RETRO_PLATFORM == RETRO_WIN
        FreeConsole();
#endif
    }
#endif

    return 0;
}

#if RETRO_PLATFORM == RETRO_UWP
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return SDL_WinRTRunApp(main, NULL); }
#endif
