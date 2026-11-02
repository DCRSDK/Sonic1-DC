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
// Sonic sign from dreamcast/assets/sonicsign.bmp, decoded at its native 48x32.
// 1 bit/pixel, 6 bytes/row, MSB = leftmost, 1 = dark/ink.
static const unsigned char dcVmuArt[192] = {
    0xFF,0xE0,0x14,0x2F,0xFF,0xFF,
    0xC0,0x1F,0xE4,0x20,0x00,0x03,
    0xBF,0xE0,0x1F,0xFF,0xFF,0xFD,
    0x43,0x00,0x01,0xC1,0x80,0x02,
    0x8C,0x03,0x00,0x3E,0x7F,0xF1,
    0x98,0x04,0x80,0x0C,0x5F,0xF9,
    0x98,0x04,0x40,0x00,0x5F,0xF9,
    0x90,0x04,0x20,0x00,0x5F,0xF9,
    0x97,0x04,0x10,0x00,0x5F,0xF9,
    0x98,0xC4,0x20,0x00,0x5E,0x79,
    0x96,0x64,0xDE,0x00,0x4D,0xB9,
    0x9D,0x85,0x21,0x06,0x72,0x59,
    0x9A,0x02,0x40,0x89,0x4A,0x59,
    0x94,0x00,0x48,0x48,0xCA,0x59,
    0x98,0x00,0x5C,0x4C,0xC6,0x59,
    0x90,0x00,0x5C,0x2C,0xE6,0x59,
    0x90,0x00,0x5C,0x3C,0x56,0x59,
    0xA0,0x00,0x5C,0x1C,0x52,0xD9,
    0xA3,0x00,0x5C,0x1C,0xAA,0x29,
    0xAC,0x80,0x28,0x0C,0xAC,0x69,
    0x92,0x80,0x60,0x33,0x72,0x99,
    0x9D,0x00,0x90,0xFF,0x22,0x19,
    0x9A,0x01,0x0F,0x1E,0x27,0x59,
    0x9A,0x01,0x1F,0x80,0x59,0xD9,
    0x94,0x01,0x08,0x60,0x51,0x19,
    0x94,0x02,0x84,0x40,0xA8,0x29,
    0x98,0x01,0xC3,0x81,0x56,0x59,
    0x98,0x00,0x30,0x06,0xD2,0xD9,
    0x99,0xE0,0x08,0x19,0xDF,0xB9,
    0x8F,0xFF,0xFF,0xFF,0xFF,0xF1,
    0x40,0x00,0x00,0x00,0x00,0x02,
    0xBF,0xFF,0xFF,0xFF,0xFF,0xFD,
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
