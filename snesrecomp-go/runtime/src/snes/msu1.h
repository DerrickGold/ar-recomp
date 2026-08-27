#ifndef SNESRECOMP_MSU1_H
#define SNESRECOMP_MSU1_H

#include <stdbool.h>
#include <stdint.h>

void msu1_init(void);
void msu1_shutdown(void);
bool msu1_enabled(void);
void msu1_set_rom_path(const char *rom_path);
uint8_t msu1_read(uint16_t register_address);
void msu1_write(uint16_t register_address, uint8_t value);
void msu1_mix(int16_t *output, int output_frames, int output_rate);

/* Explicit host configuration, also used by hermetic tests. NULL or an empty
 * string disables the device. Directory paths are resolved to the most common
 * <name>-<track>.pcm prefix just like SNESRECOMP_MSU1. */
bool msu1_configure_base(const char *path_prefix);

#endif
