/* Keyboard keys follow Resistance 3's controller layout (kbm_layout.h): the
 * trigger presets and a Customize layout.
 *
 *   clang-cl /I include libs\input\tests\test_kbm_layout.c   (from ps3recomp\)
 */
#include "../kbm_layout.h"
#include "../cellPad.h"
#include <assert.h>
#include <stdio.h>

/* func_003C7F38: the presets move fire (+4), secondary fire (+8), zoom (+0xC)
 * and throw grenade (+0x34) */
static void preset(uint32_t cur[KBM_LAYOUT_SLOTS], int idx)
{
    static const uint16_t p[4][4] = {
        { 0x0080, 0x0020, 0x0040, 0x0010 },
        { 0x0020, 0x0080, 0x0010, 0x0040 },
        { 0x0080, 0x0040, 0x0020, 0x0010 },
        { 0x0010, 0x0040, 0x0020, 0x0080 },
    };
    for (int i = 0; i < KBM_LAYOUT_SLOTS; i++) cur[i] = kbm_def_layout[i];
    cur[1] = p[idx][0]; cur[2] = p[idx][1]; cur[3] = p[idx][2]; cur[13] = p[idx][3];
}

int main(void)
{
    uint32_t cur[KBM_LAYOUT_SLOTS];

    /* the shipped layout as func_003C8D30 stores it (offset, word), written out
     * from the recompiled stores, not from kbm_def_layout: +0x2C is 0x8000 (Left;
     * R_5 = R_11 | 0x8000 comes before that store, the 0x20 went to +0x8) */
    {
        static const uint32_t game[][2] = {
            { 0x00, 0x0004 }, { 0x04, 0x0080 }, { 0x08, 0x0020 }, { 0x0C, 0x0040 },
            { 0x10, 0x0002 }, { 0x14, 0x0002 }, { 0x18, 0x0800 }, { 0x1C, 0x0000 },
            { 0x20, 0x0008 }, { 0x24, 0x0008 }, { 0x28, 0x2000 }, { 0x2C, 0x8000 },
            { 0x30, 0x4000 }, { 0x34, 0x0010 }, { 0x38, 0x0400 }, { 0x3C, 0x0001 },
            { 0x40, 0x0000 }, { 0x44, 0x1000 },
        };
        for (int i = 0; i < KBM_LAYOUT_SLOTS; i++) cur[game[i][0] / 4] = game[i][1];
        for (int i = 0; i < KBM_LAYOUT_SLOTS; i++) assert(kbm_def_layout[i] == cur[i]);
        assert(kbm_layout_valid(cur));
        /* every key sends its own button, d-pad Left included */
        assert(kbm_layout_translate(CELL_PAD_CTRL_LEFT, cur) == CELL_PAD_CTRL_LEFT);
        for (int b = 0; b < 16; b++)
            assert(kbm_layout_translate((uint16_t)(1u << b), cur) == (uint16_t)(1u << b));
    }

    /* shipped layout: nothing moves */
    preset(cur, 0);
    assert(kbm_layout_valid(cur));
    assert(kbm_layout_translate(0xFFFF, cur) == 0xFFFF);

    /* preset 1: fire key (R1) -> R2, secondary (R2) -> R1, zoom (L1) -> L2, grenade (L2) -> L1 */
    preset(cur, 1);
    assert(kbm_layout_valid(cur));
    assert(kbm_layout_translate(CELL_PAD_CTRL_R1, cur) == CELL_PAD_CTRL_R2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R2, cur) == CELL_PAD_CTRL_R1);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L1, cur) == CELL_PAD_CTRL_L2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L2, cur) == CELL_PAD_CTRL_L1);
    /* crouch (Circle) and sprint (L3) are their own buttons and stay */
    assert(kbm_layout_translate(CELL_PAD_CTRL_CIRCLE | CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_CROSS, cur)
           == (CELL_PAD_CTRL_CIRCLE | CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_CROSS));

    /* preset 2: secondary (R2) -> L1, zoom (L1) -> R2 */
    preset(cur, 2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R2 | CELL_PAD_CTRL_L1, cur) == (CELL_PAD_CTRL_L1 | CELL_PAD_CTRL_R2));
    assert(kbm_layout_translate(CELL_PAD_CTRL_R1, cur) == CELL_PAD_CTRL_R1);

    /* preset 3: fire -> L2, grenade -> R1 */
    preset(cur, 3);
    assert(kbm_layout_translate(CELL_PAD_CTRL_R1, cur) == CELL_PAD_CTRL_L2);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L2, cur) == CELL_PAD_CTRL_R1);

    /* Customize: crouch on L3 and sprint on Circle (independent); reload / use move together;
     * the unread second Circle word does not hide the move; Start and Select are kept */
    preset(cur, 0);
    cur[4] = 0x0400; cur[14] = 0x0002; cur[8] = cur[9] = 0x0001; cur[15] = 0x0008;
    assert(kbm_layout_translate(CELL_PAD_CTRL_CIRCLE, cur) == CELL_PAD_CTRL_L3);
    assert(kbm_layout_translate(CELL_PAD_CTRL_L3, cur) == CELL_PAD_CTRL_CIRCLE);
    assert(kbm_layout_translate(CELL_PAD_CTRL_SQUARE, cur) == CELL_PAD_CTRL_TRIANGLE);
    assert(kbm_layout_translate(CELL_PAD_CTRL_TRIANGLE, cur) == CELL_PAD_CTRL_SQUARE);
    assert(kbm_layout_translate(CELL_PAD_CTRL_START | CELL_PAD_CTRL_SELECT, cur)
           == (CELL_PAD_CTRL_START | CELL_PAD_CTRL_SELECT));

    /* an action left without a button keeps its key's own button (d-pad up still zooms a scope) */
    preset(cur, 0); cur[17] = 0;
    assert(kbm_layout_valid(cur));
    assert(kbm_layout_translate(CELL_PAD_CTRL_UP, cur) == CELL_PAD_CTRL_UP);

    /* garbage is refused (the caller keeps the default layout) */
    preset(cur, 0); cur[4] = 0x1001;
    assert(!kbm_layout_valid(cur));
    preset(cur, 0); cur[1] = 0;
    assert(!kbm_layout_valid(cur));
    preset(cur, 0); cur[6] = 0x10000;
    assert(!kbm_layout_valid(cur));

    printf("test_kbm_layout: ok\n");
    return 0;
}
