/*
 * test_avconf_speakers - PORT_SPEAKERS through cellAudioOutGetSoundAvailability.
 *
 * Resistance 3 configures (and mixes for) the highest channel count reported
 * for coding types 0 / 1 / 6 at 48 kHz, so the launcher's Speaker layout must cap it.
 * Run once per layout: test_avconf_speakers <2|6|8|-> ("-" = unset).
 *
 *   cc -std=gnu17 -I include -o test_avconf_speakers libs/misc/tests/test_avconf_speakers.c
 */

#include "../cellAvconfExt.c"

#include <assert.h>

uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) { (void)nid; (void)name; (void)fn; }
/* cellAudio.c: the host output; TEST_HOST_CH=6/8 = a surround device */
int cellAudio_host_channels(void) { const char* e = getenv("TEST_HOST_CH"); return e ? atoi(e) : 2; }

int main(int argc, char** argv)
{
    const char* v = argc > 1 ? argv[1] : "2";
    if (strcmp(v, "-") != 0) {
#ifdef _WIN32
        char kv[32]; snprintf(kv, sizeof kv, "PORT_SPEAKERS=%s", v); _putenv(kv);
#else
        setenv("PORT_SPEAKERS", v, 1);
#endif
    }
    const int host = cellAudio_host_channels();   /* unset PORT_SPEAKERS follows a 7.1 host */
    const s32 want = strcmp(v, "2") == 0 ? 2 : strcmp(v, "8") == 0 ? 8 :
                     (strcmp(v, "6") != 0 && host >= 8) ? 8 : 6;

    /* the game's selection (R3 0x008140F8 loop): the first type with the highest count */
    const u32 types[3] = { CELL_AUDIO_OUT_CODING_TYPE_LPCM, CELL_AUDIO_OUT_CODING_TYPE_AC3,
                            6 };
    s32 best = -1; u32 best_type = 0;
    for (int i = 0; i < 3; i++) {
        const s32 n = cellAudioOutGetSoundAvailability(0, types[i], CELL_AUDIO_OUT_FS_48KHZ, 0);
        if (n > best) { best = n; best_type = types[i]; }
    }
    assert(best == want);
    assert(best_type == CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    printf("test_avconf_speakers %s: OK (%d ch)\n", v, best);
    return 0;
}
