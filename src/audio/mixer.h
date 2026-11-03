#pragma once

/* Star Fox's Acmd macros, redirected to the software audio microcode (the
   shared library's mixer): synthesis runs it directly instead of building a
   command list for the RSP. */

#include "PR/ultratypes.h"
#include "PR/abi.h"

/* Undefine all N64 Acmd macros so we can redirect them to software implementations */
#undef aSegment
#undef aClearBuffer
#undef aSetBuffer
#undef aLoadBuffer
#undef aSaveBuffer
#undef aDMEMMove
#undef aMix
#undef aEnvMixer
#undef aEnvSetup1
#undef aEnvSetup2
#undef aResample
#undef aInterleave
#undef aInterl
#undef aSetLoop
#undef aLoadADPCM
#undef aADPCMdec
#undef aS8Dec
#undef aHiLoGain
#undef aFilter
#undef aDuplicate
#undef aAddMixer
#undef aResampleZoh
#undef aUnkCmd3
#undef aUnkCmd19
#undef aSetVolume
#undef aSetVolume32
#undef aPoleFilter

/* Host memory: virtual == physical */
#undef VIRTUAL_TO_PHYSICAL2
#undef OS_K0_TO_PHYSICAL
#define VIRTUAL_TO_PHYSICAL2(x) (x)
#define OS_K0_TO_PHYSICAL(x) (x)

/* The software microcode (n64/src/mixer.c) */
#include "s30_mixer.h"

/* ---- Macro redirections (strip pkt, call Impl directly) ---- */

#define aSegment(pkt, s, b) \
    do { } while (0)

#define aClearBuffer(pkt, d, c) \
    aClearBufferImpl(d, c)

#define aLoadBuffer(pkt, s, d, c) \
    aLoadBufferImpl(s, d, c, 2) /* the decoders read the copy in DMEM */

#define aSaveBuffer(pkt, s, d, c) \
    aSaveBufferImpl(s, (s16*)(d), c)

#define aLoadADPCM(pkt, c, d) \
    aLoadADPCMImpl(c, d)

#define aSetBuffer(pkt, f, i, o, c) \
    aSetBufferImpl(f, i, o, c)

#define aDMEMMove(pkt, i, o, c) \
    aDMEMMoveImpl(i, o, c)

#define aSetLoop(pkt, a) \
    aSetLoopImpl(a)

#define aADPCMdec(pkt, f, s) \
    aADPCMdecImpl(f, s)

#define aResample(pkt, f, p, s) \
    aResampleImpl(f, p, s)

#define aEnvSetup1(pkt, a, b, c, d) \
    aEnvSetup1Impl(a, b, c, d)

#define aEnvSetup2(pkt, volLeft, volRight) \
    aEnvSetup2Impl(volLeft, volRight)

/* Star Fox aEnvMixer: 9 params (pkt, dmemi, count, swapLR, x0, x1, x2, x3, m) */
#define aEnvMixer(pkt, dmemi, count, swapReverb, x0, x1, x2, x3, m) \
    aEnvMixerImpl(dmemi, count, swapReverb, x0, x1, x2, x3, m, 0)

/* Star Fox aInterleave: 3 params (pkt, l, r); output and size from aSetBuffer */
#define aInterleave(pkt, l, r) \
    aInterleaveSetBufferImpl(l, r)

#define aInterl(pkt, dmemi, dmemo, count) \
    aInterlImpl(dmemi, dmemo, count)

#define aMix(pkt, f, g, i, o) \
    aMixImpl(f, g, i, o)

#define aFilter(pkt, f, countOrBuf, addr) \
    aFilterImpl(f, countOrBuf, (s16*)(addr))

/* OoT aHiLoGain: 5 params (pkt, gain, count, dmem, a4) */
#define aHiLoGain(pkt, gain, count, dmem, a4) \
    aHiLoGainImpl(gain, count, dmem)

#define aDuplicate(pkt, c, s, d) \
    aDuplicateImpl(c, s, d)

/* OoT aAddMixer: 5 params (pkt, count, dmemi, dmemo, a4) */
#define aAddMixer(pkt, count, dmemi, dmemo, a4) \
    aAddMixerImpl(count, dmemi, dmemo, a4)

#define aS8Dec(pkt, f, s) \
    aS8DecImpl(f, s)

#define aResampleZoh(pkt, pitch, startFract) \
    aResampleZohImpl(pitch, startFract)

#define aUnkCmd3(pkt, a1, a2, a3) \
    aUnkCmd3Impl(a1, a2, a3)

#define aUnkCmd19(pkt, a1, a2, a3, a4) \
    aUnkCmd19Impl(a1, a2, a3, a4)

/* No-ops in the software mixer; synthesis.c still references them */
#define aSetVolume(pkt, f, v, t, r)   do { } while (0)
#define aSetVolume32(pkt, f, v, tr)    do { } while (0)
#define aPoleFilter(pkt, f, g, s)      do { } while (0)
