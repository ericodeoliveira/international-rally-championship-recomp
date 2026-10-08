/* DirectPlay: network play is intentionally not supported; report no service providers. */
#include "runtime.h"

#define DPERR_UNAVAILABLE 0x887700DCu

static void dplay_DirectPlayEnumerateA(CPU *c) { hle_return(c, 0, 2); }

static void dplay_DirectPlayCreate(CPU *c)
{
    if (ARG(1)) wr32(ARG(1), 0);
    hle_return(c, DPERR_UNAVAILABLE, 3);
}

const HleDef hle_dplay[] = {
    {"DPLAYX", "ord9", dplay_DirectPlayEnumerateA},
    {"DPLAYX", "ord1", dplay_DirectPlayCreate},
    {0, 0, 0},
};
