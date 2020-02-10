#pragma once

#include <cstdint>
#include <cstdlib>  // for byte swapping

#ifdef _WIN32
#pragma warning(disable:4244)
#pragma warning(disable:4996)
#endif

#ifndef DISALLOW_COPY_AND_ASSIGN
#define DISALLOW_COPY_AND_ASSIGN(t) \
	t(const t &other) = delete;  \
	void operator =(const t &other) = delete;
#endif

#ifdef _WIN32

typedef intptr_t ssize_t;

#include <tchar.h>

#endif  // _WIN32

// Byteswapping
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif

inline uint8_t swap8(uint8_t _data) {return _data;}

// Just in case this has been defined by platform
#undef swap16
#undef swap32
#undef swap64

#ifdef _WIN32
inline uint16_t swap16(uint16_t _data) {return _byteswap_ushort(_data);}
inline uint32_t swap32(uint32_t _data) {return _byteswap_ulong (_data);}
inline uint64_t swap64(uint64_t _data) {return _byteswap_uint64(_data);}
#elif defined(__GNUC__)
inline uint16_t swap16(uint16_t _data) {return __builtin_bswap16(_data);}
inline uint32_t swap32(uint32_t _data) {return __builtin_bswap32(_data);}
inline uint64_t swap64(uint64_t _data) {return __builtin_bswap64(_data);}
#else
// Slow generic implementation. Hopefully this never hits
inline uint16_t swap16(uint16_t data) {return (data >> 8) | (data << 8);}
inline uint32_t swap32(uint32_t data) {return (swap16(data) << 16) | swap16(data >> 16);}
inline uint64_t swap64(uint64_t data) {return ((uint64_t)swap32(data) << 32) | swap32(data >> 32);}
#endif

inline uint16_t swap16(const uint8_t* _pData) {return swap16(*(const uint16_t*)_pData);}
inline uint32_t swap32(const uint8_t* _pData) {return swap32(*(const uint32_t*)_pData);}
inline uint64_t swap64(const uint8_t* _pData) {return swap64(*(const uint64_t*)_pData);}

// Thread local storage
#ifdef _MSC_VER
#define __THREAD __declspec( thread ) 
#else
#define __THREAD __thread
#endif

// For really basic windows code compat
#ifndef _TCHAR_DEFINED
typedef char TCHAR;
#endif

