/**
 * @file clut16.h
 * @brief Apple-Inspired Industrial 16-Color Palette Table (CLUT)
 * @standards Apple Human Interface Guidelines / ISA-101 Situational Awareness
 *
 * Formatted as 0x00RRGGBB for native Win32 GDI RGBQUAD and direct framebuffer mapping.
 */

#ifndef INDUSTRIAL_HMI_CLUT16_H
#define INDUSTRIAL_HMI_CLUT16_H

#include "../config.h"

/* 16-Color Palette Table (0x00RRGGBB) */
static const uint32_t g_hmi_palette_dark[16] = {
    [COLOR_OBSIDIAN_BG]    = 0x0B0F17, /* Deep calming obsidian dark space */
    [COLOR_TITANIUM_CARD]  = 0x161F2E, /* Subtle titanium slate card */
    [COLOR_CARD_BORDER]    = 0x26334D, /* Sleek card perimeter border */
    [COLOR_CHALK_WHITE]    = 0xF8FAFC, /* Crisp primary text */
    [COLOR_SILVER_MUTED]   = 0x94A3B8, /* Calm secondary label text */
    [COLOR_DARK_GRAY]      = 0x334155, /* Muted gridlines & inactive tracks */
    [COLOR_CYAN_ACCENT]    = 0x00D2FF, /* Electric Apple Cyan */
    [COLOR_BLUE_ACCENT]    = 0x38BDF8, /* Calm status blue */
    [COLOR_EMERALD_OK]     = 0x10B981, /* Emerald green (Plant Nominal) */
    [COLOR_AMBER_WARN]     = 0xF59E0B, /* Warm amber (Caution / Limit) */
    [COLOR_CRIMSON_TRIP]   = 0xEF4444, /* Vibrant crimson (Trip / E-Stop) */
    [COLOR_VIBRANT_ORANGE] = 0xFB923C, /* Active setpoint marker */
    [COLOR_PURPLE_SECURE]  = 0xA855F7, /* RBAC security badge */
    [COLOR_SURFACE_HOVER]  = 0x1E293B, /* Interactive pressed surface */
    [COLOR_NEON_GREEN]     = 0x00FF66, /* Live motor run indicator */
    [COLOR_WHITE_PURE]     = 0xFFFFFF  /* Pure highlight */
};

/* High-Contrast Outdoor Theme for Sunlight / Glare Environments */
static const uint32_t g_hmi_palette_contrast[16] = {
    [COLOR_OBSIDIAN_BG]    = 0x000000, /* Pure black */
    [COLOR_TITANIUM_CARD]  = 0x111111, /* Dark charcoal */
    [COLOR_CARD_BORDER]    = 0x444444, /* High contrast white-gray border */
    [COLOR_CHALK_WHITE]    = 0xFFFFFF, /* Pure white text */
    [COLOR_SILVER_MUTED]   = 0xCCCCCC, /* Light silver text */
    [COLOR_DARK_GRAY]      = 0x333333, /* Dark gray */
    [COLOR_CYAN_ACCENT]    = 0x00FFFF, /* Pure cyan */
    [COLOR_BLUE_ACCENT]    = 0x3399FF, /* Vivid blue */
    [COLOR_EMERALD_OK]     = 0x00FF00, /* Pure green */
    [COLOR_AMBER_WARN]     = 0xFFFF00, /* Pure yellow */
    [COLOR_CRIMSON_TRIP]   = 0xFF0000, /* Pure red */
    [COLOR_VIBRANT_ORANGE] = 0xFF8800, /* Vivid orange */
    [COLOR_PURPLE_SECURE]  = 0xCC00FF, /* Vivid magenta */
    [COLOR_SURFACE_HOVER]  = 0x222222, /* Surface */
    [COLOR_NEON_GREEN]     = 0x00FF00, /* Green */
    [COLOR_WHITE_PURE]     = 0xFFFFFF  /* Pure white */
};

#endif /* INDUSTRIAL_HMI_CLUT16_H */
