#pragma once
#if defined(_MSC_VER)
#include <intrin.h>
#include <stdio.h>
#define DEBUG_BREAK() __debugbreak()
#else
#define DEBUG_BREAK() __builtin_trap()
#endif
#if ENGINE_DEBUG
#define ASSERT(expr)                                                                    \
    do                                                                                  \
    {                                                                                   \
        if (!(expr))                                                                    \
        {                                                                               \
            fprintf(stderr, "ASSERT(%s) failed at %s:%d\n", #expr, __FILE__, __LINE__); \
            DEBUG_BREAK();                                                              \
        }                                                                               \
    } while (0)
#else
#define ASSERT(expr) ((void)0)
#endif
#define INVALID_CODE_PATH ASSERT(!"invalid code path")
#define NOT_IMPLEMENTED ASSERT(!"not implemented")