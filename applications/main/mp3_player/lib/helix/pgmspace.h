/* Stub for Arduino-style pgmspace.h. On ESP32 the entire flash address
 * space is directly memory-mapped, so PROGMEM and pgm_read_* are no-ops. */
#ifndef _PGMSPACE_H_STUB
#define _PGMSPACE_H_STUB

#define PROGMEM
#define PGM_VOID_P const void*

#define pgm_read_byte(addr)      (*(const unsigned char*)(addr))
#define pgm_read_word(addr)      (*(const unsigned short*)(addr))
#define pgm_read_dword(addr)     (*(const unsigned int*)(addr))
#define pgm_read_float(addr)     (*(const float*)(addr))

#define pgm_read_byte_near(addr) pgm_read_byte(addr)
#define pgm_read_word_near(addr) pgm_read_word(addr)

#endif
