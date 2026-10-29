#!/bin/sh
# Minimal KOS boot test builds (run inside KOS environment).
kos-cc -o dctest.elf  dctest.c -DFLAGS_VARIANT=0   # RSDKv4's init flags + /cd check
kos-cc -o dctest1.elf dctest.c -DFLAGS_VARIANT=1   # no CDROM
kos-cc -o dctest2.elf dctest.c -DFLAGS_VARIANT=2   # bare defaults
echo "now: mkdcdisc -e dctest.elf -o dctest.cdi   (repeat for variants if needed)"
