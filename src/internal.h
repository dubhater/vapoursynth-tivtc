#ifndef __Internal_H__
#define __Internal_H__

#include <stdexcept>

// these settings control whether the included code comes from old asm or newer simd/C rewrites
#define USE_C_NO_ASM
// USE_C_NO_ASM: inline non-simd asm


#ifdef _WIN32
#define AVS_FORCEINLINE __forceinline
#else
#define AVS_FORCEINLINE __attribute__((always_inline)) inline
#endif


// Frame properties set by TFM:
#define PROP_TFMDisplay "TFMDisplay"
#define PROP_TFMMATCH "TFMMatch"
#define PROP_TFMMics "TFMMics"
#define PROP_Combed "_Combed"
#define PROP_TFMD2VFilm "TFMD2VFilm"
#define PROP_TFMField "TFMField"
#define PROP_TFMPP "TFMPP"

// Frame properties set by TDecimate:
#define PROP_TDecimateDisplay "TDecimateDisplay"
#define PROP_TDecimateCycleStart "TDecimateCycleStart"
#define PROP_TDecimateCycleMaxBlockDiff "TDecimateCycleMaxBlockDiff" // uint64_t[]
#define PROP_TDecimateOriginalFrame "TDecimateOriginalFrame"

class TIVTCError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};


constexpr int ISP = 0x00000000; // p
constexpr int ISC = 0x00000001; // c
constexpr int ISN = 0x00000002; // n
constexpr int ISB = 0x00000003; // b
constexpr int ISU = 0x00000004; // u
constexpr int ISDB = 0x00000005; // l = (deinterlaced c bottom field)
constexpr int ISDT = 0x00000006; // h = (deinterlaced c top field)

#define MTC(n) n == 0 ? 'p' : n == 1 ? 'c' : n == 2 ? 'n' : n == 3 ? 'b' : n == 4 ? 'u' : \
               n == 5 ? 'l' : n == 6 ? 'h' : 'x'

constexpr int TOP_FIELD = 0x00000008;
constexpr int COMBED = 0x00000010;
constexpr int D2VFILM = 0x00000020;

constexpr int FILE_COMBED = 0x00000030;
constexpr int FILE_NOTCOMBED = 0x00000020;
constexpr int FILE_ENTRY = 0x00000080;
constexpr int FILE_D2V = 0x00000008;
constexpr int D2VARRAY_DUP_MASK = 0x03;
constexpr int D2VARRAY_MATCH_MASK = 0x3C;

constexpr int DROP_FRAME = 0x00000001; // ovr array - bit 1
constexpr int KEEP_FRAME = 0x00000002; // ovr array - 2
constexpr int FILM = 0x00000004; // ovr array - bit 3
constexpr int VIDEO = 0x00000008; // ovr array - bit 4
constexpr int ISMATCH = 0x00000070; // ovr array - bits 5-7
constexpr int ISD2VFILM = 0x00000080; // ovr array - bit 8

#define cfps(n) n == 1 ? "119.880120" : n == 2 ? "59.940060" : n == 3 ? "39.960040" : \
                n == 4 ? "29.970030" : n == 5 ? "23.976024" : "unknown"


#ifdef VERSION
#undef VERSION
#endif
#define VERSION "v1.0.7"


#endif  // __Internal_H__
