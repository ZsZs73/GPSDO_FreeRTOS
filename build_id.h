/*
 * build_id.h — one number, bumped on every change, whose only job is to make
 * the Arduino builder recompile the sketch.
 *
 * Part of GPSDO FreeRTOS v1.06
 *
 * WHY THIS EXISTS
 * ---------------
 * __DATE__ and __TIME__ are baked into the translation unit that mentions them,
 * and that is GPSDO_FreeRTOS.ino. The Arduino builder tracks dependencies
 * properly and skips any translation unit whose sources have not changed — so
 * editing GPSDO_algorithms.cpp and uploading leaves the sketch's object file
 * untouched, with the timestamp from whenever the sketch itself last changed.
 * The banner then reports a compile that is genuinely older than the binary. On
 * 26.08 that cost an hour: two captures from two different builds carried the
 * same stamp, and the log was blamed for it.
 *
 * The sketch includes this file, so touching it is enough to make the builder
 * recompile the sketch and nothing else — a fraction of a second, against a
 * full rebuild if the same thing were done with a flag in build_opt.h.
 *
 * TWO WAYS TO BUMP IT, and both are fine:
 *   - tools/bumpbuild.py, run before a compile (it edits the line below)
 *   - by hand, or by any edit at all: the builder keys on the file, not on
 *     what is in it
 *
 * AND IF IT IS NOT BUMPED, nothing breaks and nothing lies. The banner also
 * prints a CRC-32 of the flash image, computed at boot from the flash itself
 * (gpsdo_build.h). That one cannot be stale, because nothing computes it until
 * the board is running. The serial and the timestamp are the convenience; the
 * CRC is the fact.
 */
#ifndef GPSDO_BUILD_ID_H
#define GPSDO_BUILD_ID_H

#define BUILD_SERIAL 42
#endif /* GPSDO_BUILD_ID_H */
