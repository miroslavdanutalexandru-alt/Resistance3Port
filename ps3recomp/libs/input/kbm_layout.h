/* Keyboard keys and the game's controller layout (Resistance 3).
 *
 * Keys are bound to ACTIONS in the launcher, named after the PS3's default
 * layout (R1 fire, R2 secondary fire, L1 zoom, L2 grenade, Circle crouch, L3
 * sprint, ...). Options > Controls can pick another preset (1-3: the triggers
 * swap around) or a custom layout from the Customize screen. The game then maps
 * buttons to actions through that layout, so a key that pressed the default
 * button would do another action. Each key's button is translated: the default
 * layout's action for the button -> the button the active layout puts it on.
 *
 * A layout is one 32-bit game word per action, 0xA8 bytes per layout:
 *   0x0001 Triangle 0x0002 Circle 0x0004 Cross 0x0008 Square
 *   0x0010 L2 0x0020 R2 0x0040 L1 0x0080 R1
 *   0x0100 Select 0x0200 Start 0x0400 L3 0x0800 R3
 *   0x1000 Up 0x2000 Right 0x4000 Down 0x8000 Left
 * Defaults: func_003C8D30. Presets: func_003C7F38 (table 0x01197CA0). Lookup:
 * func_003C8074. Action mask -> buttons: func_003C8910 (it never reads +0x14). */
#ifndef KBM_LAYOUT_H
#define KBM_LAYOUT_H

#include <stdint.h>

#define KBM_LAYOUT_SLOTS   18
#define KBM_SLOT_UNREAD    5    /* +0x14: a second Circle word the game's lookup never reads */

/* func_003C8D30: jump, fire, secondary fire, zoom, crouch, (unread), melee, -,
 * reload, use, next grenade, previous grenade, call partner, throw grenade,
 * sprint, weapon select, -, d-pad up (scope zoom) */
static const uint16_t kbm_def_layout[KBM_LAYOUT_SLOTS] = {
    0x0004, 0x0080, 0x0020, 0x0040, 0x0002, 0x0002, 0x0800, 0x0000, 0x0008,
    0x0008, 0x2000, 0x8000, 0x4000, 0x0010, 0x0400, 0x0001, 0x0000, 0x1000,
};

/* CELL_PAD_CTRL_* bit n (cellPad.h) -> game word */
static const uint16_t kbm_sdk_game[16] = {
    0x0100 /* SELECT */, 0x0400 /* L3 */, 0x0800 /* R3 */, 0x0200 /* START */,
    0x1000 /* UP */, 0x2000 /* RIGHT */, 0x4000 /* DOWN */, 0x8000 /* LEFT */,
    0x0010 /* L2 */, 0x0020 /* R2 */, 0x0040 /* L1 */, 0x0080 /* R1 */,
    0x0001 /* TRIANGLE */, 0x0002 /* CIRCLE */, 0x0004 /* CROSS */, 0x0008 /* SQUARE */,
};

static inline uint16_t kbm_game_to_sdk_bit(uint16_t g)
{
    for (int b = 0; b < 16; b++) if (kbm_sdk_game[b] == g) return (uint16_t)(1u << b);
    return 0;
}

/* a usable layout: each action on one button (0 = none), and the main ones set */
static inline int kbm_layout_valid(const uint32_t cur[KBM_LAYOUT_SLOTS])
{
    for (int i = 0; i < KBM_LAYOUT_SLOTS; i++) {
        const uint32_t w = cur[i];
        if (w > 0xFFFF || (w & (w - 1))) return 0;
    }
    return cur[0] && cur[1] && cur[2] && cur[3];
}

/* Keyboard buttons (CELL_PAD_CTRL_*) meant with the default layout -> the buttons
 * that do the same actions with layout `cur`. A button with no default action
 * (Select, Start), or whose action the layout left without a button, is kept
 * (the d-pad also zooms a scope directly). */
static inline uint16_t kbm_layout_translate(uint16_t btns, const uint32_t cur[KBM_LAYOUT_SLOTS])
{
    uint16_t out = 0;
    for (int b = 0; b < 16; b++) {
        const uint16_t sdk = (uint16_t)(1u << b);
        if (!(btns & sdk)) continue;
        const uint16_t g = kbm_sdk_game[b];
        int slot = -1;
        for (int i = 0; i < KBM_LAYOUT_SLOTS; i++)
            if (i != KBM_SLOT_UNREAD && kbm_def_layout[i] == g) { slot = i; break; }
        const uint16_t to = slot >= 0 && cur[slot] ? kbm_game_to_sdk_bit((uint16_t)cur[slot]) : 0;
        out |= to ? to : sdk;
    }
    return out;
}

#endif
