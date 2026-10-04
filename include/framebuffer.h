#ifndef LIONOS_FRAMEBUFFER_H
#define LIONOS_FRAMEBUFFER_H

#include <stdint.h>

int framebuffer_prepare(uint32_t multiboot_info);
int framebuffer_init(uint32_t multiboot_info);
void framebuffer_boot_splash(uint32_t progress, const char *status);
int framebuffer_available(void);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
void framebuffer_clear(uint32_t color);
void framebuffer_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void framebuffer_blend_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color, uint8_t alpha);
void framebuffer_blend_round_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t radius, uint32_t color, uint8_t alpha);
void framebuffer_blend_circle(uint32_t cx, uint32_t cy, uint32_t radius, uint32_t color, uint8_t alpha);
void framebuffer_console_clear(void);
void framebuffer_console_putc(char c, uint32_t row, uint32_t col, uint32_t fg, uint32_t bg);
int framebuffer_begin_desktop(void);
void framebuffer_end_desktop(void);
int framebuffer_cursor_overlay_supported(void);
void framebuffer_cursor_hide(void);
void framebuffer_cursor_move(uint32_t x, uint32_t y);
void framebuffer_set_clip(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void framebuffer_clear_clip(void);
void framebuffer_restore_wallpaper_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void framebuffer_present_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void framebuffer_present(void);
void framebuffer_blit_rgba32(const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t size);
void framebuffer_blit_rgb565_cover(const uint16_t *pixels, uint32_t width, uint32_t height);
void framebuffer_restore_wallpaper(void);

#endif
