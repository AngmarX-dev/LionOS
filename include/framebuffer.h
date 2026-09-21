#ifndef LIONOS_FRAMEBUFFER_H
#define LIONOS_FRAMEBUFFER_H

#include <stdint.h>

int framebuffer_init(uint32_t multiboot_info);
int framebuffer_available(void);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
void framebuffer_clear(uint32_t color);
void framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void framebuffer_console_clear(void);
void framebuffer_console_putc(char c, uint32_t row, uint32_t col, uint32_t fg, uint32_t bg);
int framebuffer_begin_desktop(void);
int framebuffer_set_mode(uint32_t width, uint32_t height);
int framebuffer_mode_supported(uint32_t width, uint32_t height);
void framebuffer_present(void);
void framebuffer_blit_rgba32(const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t size);
void framebuffer_blit_rgb565_cover(const uint16_t *pixels, uint32_t width, uint32_t height);

#endif
