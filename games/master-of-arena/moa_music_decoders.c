/* The decoders for Rogue Arena's custom music (moa_music.cpp), compiled once.
 * third_party/README.md has their sources and licences. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD_INTRINSICS_WARNING
#include "third_party/dr_mp3.h"
