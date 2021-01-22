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
#define PROP_D2VFilm "TFMD2VFilm"
#define PROP_TFMField "TFMField"
#define PROP_TFMPP "TFMPP"

class TIVTCError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

#endif  // __Internal_H__
