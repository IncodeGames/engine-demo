#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;
typedef int32_t b32;
typedef size_t usize;
#define KB(x) ((usize)(x) * 1024ull)
#define MB(x) (KB(x) * 1024ull)
#define GB(x) (MB(x) * 1024ull)
#define ArrayCount(a) (sizeof(a) / sizeof((a)[0]))
#define AlignUp(x, a) (((usize)(x) + ((usize)(a) - 1)) & ~((usize)(a) - 1))
#define Min(a, b) ((a) < (b) ? (a) : (b))
#define Max(a, b) ((a) > (b) ? (a) : (b))
#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#define THREAD_LOCAL __declspec(thread)
#else
#define FORCE_INLINE inline
#define THREAD_LOCAL _Thread_local
#endif
#if defined(__cplusplus)
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif

#if defined(GAME_DLL_BUILD)
#define ENGINE_API EXTERN_C __declspec(dllimport)
#define GAME_API EXTERN_C __declspec(dllexport)
#else
#define ENGINE_API EXTERN_C __declspec(dllexport)
#define GAME_API EXTERN_C __declspec(dllimport)
#endif