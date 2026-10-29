/* Minimal KOS boot test — bisects the RSDKv4-DC pre-main hang.
 *
 * Build (in KOS environment):   make
 * Package:                      mkdcdisc -e dctest.elf -o dctest.cdi
 * Run in redream and watch the [SH4] log lines + screen color.
 *
 * Outcomes:
 *  - "[TEST] main reached" + RED screen  -> KOS init flags are fine under
 *    the emulator; the hang is specific to the RSDKv4 binary (ctors/size).
 *  - No "[TEST]" lines at all            -> KOS 2.3-git + emulator HLE BIOS
 *    can't get through these init flags; rebuild with FLAGS_VARIANT to
 *    find which flag is at fault (CDROM is the prime suspect).
 */
#include <kos.h>

/* Variant 0: exactly the flags RSDKv4-DC uses.
 * Variant 1: no CDROM.
 * Variant 2: bare default. */
#ifndef FLAGS_VARIANT
#define FLAGS_VARIANT 0
#endif

#if FLAGS_VARIANT == 0
KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_VMU | INIT_CDROM);
#elif FLAGS_VARIANT == 1
KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_VMU);
#else
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif

__attribute__((constructor)) static void ctor_probe(void) { dbglog(DBG_CRITICAL, "[TEST] ctors running\n"); }

int main(int argc, char **argv)
{
    dbgio_dev_select("scif"); /* without this, output goes to the null device */
    dbglog(DBG_CRITICAL, "[TEST] main reached (flags variant %d)\n", FLAGS_VARIANT);

    vid_set_mode(DM_320x240, PM_RGB565);

    /* solid red = proof of life on screen */
    for (int i = 0; i < 320 * 240; i++) vram_s[i] = 0xF800;

    dbglog(DBG_CRITICAL, "[TEST] screen filled\n");

#if FLAGS_VARIANT == 0
    /* also prove /cd works: look for the data pack */
    file_t f = fs_open("/cd/Data.rsdk", O_RDONLY);
    if (f != FILEHND_INVALID) {
        dbglog(DBG_CRITICAL, "[TEST] /cd/Data.rsdk opened, size=%d\n", (int)fs_total(f));
        fs_close(f);
    }
    else {
        dbglog(DBG_CRITICAL, "[TEST] /cd/Data.rsdk NOT FOUND\n");
    }
#endif

    dbglog(DBG_CRITICAL, "[TEST] idle loop\n");
    for (;;) thd_sleep(100);

    return 0;
}
