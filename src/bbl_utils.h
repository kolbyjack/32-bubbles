// Copyright (C) Jonathan Kolb

#ifndef __a895da1a_08c2_41fb_935f_0d7668bafa94__
#define __a895da1a_08c2_41fb_935f_0d7668bafa94__

#include <stddef.h>
#include <stdint.h>

#define __BBL_GLUE(a, b) a ## b
#define _BBL_GLUE(a, b) __BBL_GLUE(a, b)
#define _BBL_SASSERT(expr, msg) typedef char _BBL_GLUE(compiler_verify_, msg)[(expr) ? (+1) : (-1)]
#define BBL_STATIC_ASSERT(exp) _BBL_SASSERT(exp, __LINE__)
#define BBL_SIZEOF_ARRAY(x) (sizeof(x) / sizeof((x)[0]))
#define BBL_SIZEOF_FIELD(s, m) (sizeof((((s*)0)->m)))

#define BBL_DECLARE_RESOURCE(identifier)                            \
    extern uint8_t _bbl_resource_##identifier##_begin[];            \
    extern const uint8_t _bbl_resource_##identifier##_end[]

#define BBL_INCLUDE_RESOURCE(identifier, filename)                  \
    asm("\t.pushsection .rodata\n"                                  \
        "\t.global _bbl_resource_" #identifier "_begin\n"           \
        "\t.type _bbl_resource_" #identifier "_begin, @object\n"    \
        "\t.align 16\n"                                             \
        "_bbl_resource_" #identifier "_begin:\n"                    \
        "\t.incbin \"" filename "\"\n\n"                            \
        "\t.global _bbl_resource_" #identifier "_end\n"             \
        "\t.type _bbl_resource_" #identifier "_end, @object\n"      \
        "\t.align 1\n"                                              \
        "_bbl_resource_" #identifier "_end:\n"                      \
        "\t.byte 0\n"                                               \
        "\t.popsection\n"                                           \
    )

#define BBL_RESOURCE(identifier) _bbl_resource_##identifier##_begin
#define BBL_SIZEOF_RESOURCE(identifier) (_bbl_resource_##identifier##_end - _bbl_resource_##identifier##_begin)

uint32_t bbl_millis();
void bbl_sleep(uint32_t ms);
size_t bbl_snprintf(char *buf, size_t bufsiz, const char *fmt, ...);

#endif
