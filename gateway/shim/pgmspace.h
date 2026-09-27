// pgmspace.h -- flash-string helpers are no-ops natively (one address space).
#pragma once
#include <string.h>
#include <stdint.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#define PGM_P const char*
#define PGM_VOID_P const void*
#define PSTR(s) (s)
#define pgm_read_byte(p)  (*(const uint8_t*)(p))
#define pgm_read_byte_near(p) (*(const uint8_t*)(p))
#define pgm_read_word(p)  (*(const uint16_t*)(p))
#define pgm_read_dword(p) (*(const uint32_t*)(p))
#define pgm_read_ptr(p)   (*(void* const*)(p))
#define strlen_P  strlen
#define strcpy_P  strcpy
#define strncpy_P strncpy
#define memcpy_P  memcpy
#define memccpy_P memccpy
#define strcmp_P  strcmp
#define strncmp_P strncmp
#define strstr_P  strstr
#define snprintf_P snprintf
#define vsnprintf_P vsnprintf
class __FlashStringHelper;
#define FPSTR(p) (reinterpret_cast<const __FlashStringHelper*>(p))
#define F(s) FPSTR(s)
