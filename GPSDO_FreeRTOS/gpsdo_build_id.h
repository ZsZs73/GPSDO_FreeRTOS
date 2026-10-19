/*
 * gpsdo_build_id.h — identifies distinct firmware builds that are worth
 * flashing, testing, comparing or distributing, and forces the Arduino
 * builder to recompile the sketch when the number changes.
 *
 * Part of GPSDO FreeRTOS v1.07
 *
 * WHY THIS EXISTS
 * ---------------
 * __DATE__ and __TIME__ are baked into the translation unit that mentions them,
 * and that is GPSDO_FreeRTOS.ino. The Arduino builder tracks dependencies
 * properly and skips any translation unit whose sources have not changed — so
 * editing gpsdo_algorithms.cpp and uploading leaves the sketch's object file
 * untouched, with the timestamp from whenever the sketch itself last changed.
 * The banner then reports a compile that is genuinely older than the binary. On
 * 26.08 that cost an hour: two captures from two different builds carried the
 * same stamp, and the log was blamed for it.
 *
 * The sketch includes this file, so touching it is enough to make the builder
 * recompile the sketch and nothing else — a fraction of a second, against a
 * full rebuild if the same thing were done with a flag in build_opt.h.
 *
 * WHEN TO BUMP IT:
 *   Increment BUILD_SERIAL before producing a distinct firmware binary that is
 *   worth flashing, testing, comparing or distributing. Several source changes
 *   may therefore share one bump; documentation/tool-only changes need none.
 *
 * HOW TO BUMP IT:
 *   - tools/bumpbuild.py, run before the compile
 *   - edit BUILD_SERIAL below by hand
 *
 * Merely touching this file also forces the sketch to recompile, but does not
 * create a new build number.
 *
 * AND IF IT IS NOT BUMPED, nothing breaks and nothing lies. The banner also
 * prints a CRC-32 of the flash image, computed at boot from the flash itself
 * (gpsdo_build.h). That one cannot be stale, because nothing computes it until
 * the board is running. The serial and the timestamp are the convenience; the
 * CRC is the fact.
 */
#ifndef GPSDO_BUILD_ID_H
#define GPSDO_BUILD_ID_H

#define BUILD_SERIAL 43
#endif /* GPSDO_BUILD_ID_H */
