/*
 * SimpletonOS UI - is this track DST? (0.31j, 19 Oct 2026)
 *
 * DST is the compressed form of DSD on SACDs. Decoding it is the one job
 * that runs the RK3566 close to its limit at the 1.1 GHz ceiling (the
 * decoder's threads at ~70 % of a core each; "Decoder is too slow" from MPD
 * at skips with anything else going on). Ian, 19 Oct 00:13: visualizers
 * don't run on DST tracks. MPD's status can't tell - a DST track decodes
 * to "dsd64:2" like any other - so the file is asked:
 *
 *   SACD ISO   the master TOC (sector 510) points at the stereo area's TOC,
 *              whose frame_format byte is 0 for DST (the SACD patch's own
 *              reading of the disc: scarletbook's area_toc_t)
 *   DFF        the PROP chunk's CMPR says "DST " or "DSD "
 *
 * Anything else (DSF, PCM) is never DST. Results are kept per container
 * file, so the check costs one read per album.
 */
#ifndef SIMPLETON_DST_H
#define SIMPLETON_DST_H

#include <stdbool.h>

/* `uri` as MPD gives it ("card-123/Album/disc.iso/2C_AUDIO__TRACK011.iso"). */
bool dst_track(const char * uri);

#endif
