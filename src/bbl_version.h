#ifndef __242b757a_5f32_4739_a2e3_c0598183b657__
#define __242b757a_5f32_4739_a2e3_c0598183b657__

#define _S2(x) #x
#define _S(x) _S2(x)

#define MAJORVER 0
#define MINORVER 1
#define REVISION 1
#define PATCHLVL 0

#if PATCHLVL > 0
# define BBL_VERSION _S(MAJORVER) "." _S(MINORVER) "." _S(REVISION) "." _S(PATCHLVL)
#else
# define BBL_VERSION _S(MAJORVER) "." _S(MINORVER) "." _S(REVISION)
#endif

#define BBL_BUILD_DATE __DATE__ " " __TIME__

#define BBL_SOURCE_HASH "6c4430f470ca4ca65d86cfd9834e2b95a1e1a20f"

#endif
