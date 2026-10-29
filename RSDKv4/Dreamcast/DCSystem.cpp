// RSDKv4 Dreamcast system helpers: timing
#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>

#include "DCCommon.hpp"

// GCC's prebuilt runtime may still call the external mutex_lock symbol.
// Newer KOS headers inline it, leaving libgcc's weak failure stub to satisfy
// those calls. Route them to the current KOS implementation instead.
extern "C" int __wrap_mutex_lock(mutex_t *mutex)
{
    return mutex_lock_timed(mutex, 0);
}

unsigned long long DC_GetTicksPerSecond() { return 1000000000ULL; }

unsigned long long DC_GetTicks() { return timer_ns_gettime64(); }

void DC_Probe(int phase)
{
    // Boot-phase breadcrumb (log-only now that serial logging works).
    DC_LOG("[DC] probe %d\n", phase);
}

// ---------------------------------------------------------------------
// VMU LCD icon
// ---------------------------------------------------------------------
// Sonic 1 emblem, drawn to the 48x32 VMU LCD: the 32x32 save icon thresholded
// to 1bpp and centred, 8px margins left and right (the art is 1:1, unlike the
// Sonic 2 winged emblem which filled the LCD's 1.5:1). 48 wide x 32 tall,
// 1 bit/pixel, 6 bytes/row, MSB = leftmost, 1 = dark/ink.
static const unsigned char dcVmuArt[192] = {
    0x00,0xFF,0xFF,0xFF,0xFF,0x00, 0x00,0xFF,0xFF,0xFF,0xFF,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x0F,0xFF,0xC0,0x00,0x00, 0x00,0x0F,0xFF,0xC0,0x00,0x00,
    0x00,0x03,0xC3,0xFC,0x30,0x00, 0x00,0x03,0xC3,0xFC,0x30,0x00, 0x00,0x00,0xC0,0xFF,0xF0,0x00,
    0x00,0x00,0xC0,0xFF,0xF0,0x00, 0x00,0x00,0xC0,0xFF,0xF0,0x00, 0x00,0x00,0xC0,0xFF,0xF0,0x00,
    0x00,0x0F,0xFF,0xFF,0xF0,0x00, 0x00,0x0F,0xFF,0xFF,0xF0,0x00, 0x00,0x3F,0xFF,0x0F,0xFC,0x00,
    0x00,0x3F,0xFF,0x0F,0xFC,0x00, 0x00,0x3F,0xFC,0xF3,0xCC,0x00, 0x00,0x3F,0xFC,0xF3,0xCC,0x00,
    0x00,0xFF,0xFC,0xF3,0xCC,0x00, 0x00,0xFF,0xFC,0xF3,0xCC,0x00, 0x00,0x00,0xFC,0xF3,0x0C,0x00,
    0x00,0x00,0xFC,0xF3,0x0C,0x00, 0x00,0x0F,0xF3,0x0F,0xF0,0x00, 0x00,0x0F,0xF3,0x0F,0xF0,0x00,
    0x00,0x3F,0xF0,0xF0,0x30,0x00, 0x00,0x3F,0xF0,0xF0,0x30,0x00, 0x00,0x3F,0xFC,0x00,0xC0,0x00,
    0x00,0x3F,0xFC,0x00,0xC0,0x00, 0x00,0x30,0xFC,0xFF,0x00,0x00, 0x00,0x30,0xFC,0xFF,0x00,0x00,
    0x00,0xFF,0xFF,0xFF,0xFF,0x00, 0x00,0xFF,0xFF,0xFF,0xFF,0x00
};

// The VMU LCD framebuffer is addressed from the bottom-right (origin flipped
// both ways), 6 bytes per row. Built once, pushed to every connected VMU.
static unsigned char dcVmuLcd[192];
static bool dcVmuLcdReady = false;

void DC_VmuShowIcon()
{
    if (!dcVmuLcdReady) {
        memset(dcVmuLcd, 0, sizeof(dcVmuLcd));
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 48; ++x) {
                if (dcVmuArt[y * 6 + (x >> 3)] & (0x80 >> (x & 7)))
                    dcVmuLcd[(31 - y) * 6 + (47 - x) / 8] |= (unsigned char)(0x80 >> ((47 - x) & 7));
            }
        }
        dcVmuLcdReady = true;
    }

    // Push to every connected LCD (both controller slots, both VMU slots).
    maple_device_t *dev;
    for (int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_LCD)) != NULL; ++i) vmu_draw_lcd(dev, dcVmuLcd);
}

// Called once per frame from DC_ProcessInput.
//
// The icon used to be pushed only from LoadStageGIFFile (Scene.cpp), so it did
// not appear until the first STAGE loaded — i.e. not until you picked a save
// and started Sonic 2. The whole front end (SEGA screen, the Whitehead logo,
// the main menu) is native objects rather than stages, so a VMU sat blank
// through all of it. Polling here instead puts the emblem up on the very first
// frame, as soon as maple has finished its boot enumeration.
//
// Enumerating twice a second and pushing only when the LCD count changes keeps
// the steady-state cost at one walk of the maple device table per 30 frames,
// and still covers hot-plugging a VMU mid-game. The Scene.cpp call is left in
// place; it is harmless and costs nothing on a screen that is already loading.
void DC_VmuTick()
{
    static int cooldown  = 0;
    static int lastCount = -1;

    if (--cooldown > 0)
        return;
    cooldown = 30;

    int count = 0;
    while (maple_enum_type(count, MAPLE_FUNC_LCD) != NULL) ++count;

    if (count == lastCount)
        return;
    lastCount = count;

    if (count > 0)
        DC_VmuShowIcon();
}

#endif // RETRO_USING_KOS
