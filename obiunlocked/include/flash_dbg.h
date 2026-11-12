// flash_dbg.h — raw SPI-flash debug access for the gateway (all boards).
// ---------------------------------------------------------------------------
// Lets the device read & write its OWN flash chip (whole chip, bypassing the partition table). Used by
// the /debug web hex editor and by a UART console that runs ALONGSIDE the normal Serial logging, so on
// the sealed OBI gateway you can inspect/patch flash over UART0 (GPIO20/21) too.
//
// ⚠ DANGER: writing or erasing the bootloader / partition table / running app can brick the device.
// Nothing here is guarded — that is the point of a debug tool. Know the offset before you write.
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// Force the runtime esp_flash driver to DIO reads. The stock Arduino C3 libs are built QIO
// (CONFIG_ESPTOOLPY_FLASHMODE_QIO=y), but the OBI gateway's flash is wired for 2 data lines only, so QIO
// reads return garbage (breaks NVS, OTA verify, and flash-debug reads). Call FIRST in setup(). No-op
// unless OBI_FLASH_FORCE_DIO is defined (the obi_gateway_c3 env). See flash_dbg.cpp.
void flash_dbg_force_dio();

uint32_t flash_dbg_size();                                                  // total chip size in bytes
bool     flash_dbg_read (uint32_t addr, uint8_t *buf, size_t len);          // raw read (any offset)
bool     flash_dbg_write(uint32_t addr, const uint8_t *buf, size_t len);    // raw write (region must be pre-erased)
bool     flash_dbg_erase(uint32_t addr, size_t len);                        // erase, expanded to 4 KB sectors
bool     flash_dbg_patch(uint32_t addr, const uint8_t *buf, size_t len);    // arbitrary bytes: read-modify-erase-write

// eFuse / chip security readout (shared by the /debug page and the UART 'efuse' command).
String flash_dbg_efuse_json();          // {chip,rev,mac,dl_locked,fuses:{...}}
void   flash_dbg_efuse_print(Print &o); // human-readable dump
bool   flash_dbg_download_locked();     // true if the ROM download mode is disabled by eFuse
bool   flash_dbg_enter_download();      // jump to ROM UART download mode (false if locked); reboots on success
bool   flash_dbg_md5(uint32_t addr, uint32_t len, char out[33]);  // MD5 hex of a flash range

// UART console — call once in setup(), then poll every loop(). It consumes typed command lines
// (rd/wr/er/patch/info/size/efuse/md5/download/help) without disturbing the concurrent log output.
void flash_dbg_uart_begin();
void flash_dbg_uart_poll();
