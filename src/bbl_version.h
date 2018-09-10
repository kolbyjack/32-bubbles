#ifndef __242b757a_5f32_4739_a2e3_c0598183b657__
#define __242b757a_5f32_4739_a2e3_c0598183b657__

#define _S2(x) #x
#define _S(x) _S2(x)

#define MAJORVER 0
#define MINORVER 0
#define REVISION 0
#define PATCHLVL 1

#if PATCHLVL > 0
# define BBL_VERSION _S(MAJORVER) "." _S(MINORVER) "." _S(REVISION) "." _S(PATCHLVL)
#else
# define BBL_VERSION _S(MAJORVER) "." _S(MINORVER) "." _S(REVISION)
#endif

#define BBL_BUILD_DATE __DATE__ " " __TIME__

#define BBL_SOURCE_HASH "5b344e4a1ebca4a31df7465132d62a567a7af895"

#endif
