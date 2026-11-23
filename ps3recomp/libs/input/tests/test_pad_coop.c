/* Local co-op pad assignment: controllers -> PS3 ports (pad_map_slots), the
 * connected flag and ASSIGN_CHANGES the game sees (pad_port_on /
 * pad_port_status), hotplug, and the PAD_COOP / PAD_P1|P2_CONTROLLER /
 * PAD_P2_KEYBOARD switches. No controller is read: the slot states are made up.
 *
 *   clang-cl /I include libs\input\tests\test_pad_coop.c   (from ps3recomp\)
 */
#include "../cellPad.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
volatile int g_port_intro_movie;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
u32 cellGcm_flip_request_count(void) { return 0; }
unsigned long long ps3_ms_now(void) { return 0; }
void ppu_guard_page(uint32_t a) { (void)a; }
void ppu_guest_callstack(const char* t) { (void)t; }
void ps3_ppu_dump_guest_stack(const char* t) { (void)t; }
void ps3_prof_start_sampler(void) {}

static PadHostState s_xs[PAD_MAX_SLOTS];

static void reset(const char* coop, const char* p1, const char* p2, const char* p2kb)
{
    _putenv_s("PAD_COOP", coop); _putenv_s("PAD_P1_CONTROLLER", p1);
    _putenv_s("PAD_P2_CONTROLLER", p2); _putenv_s("PAD_P2_KEYBOARD", p2kb);
    s_coop_init = 0; s_coop_on = 1; s_coop_p2kb = 0; s_coop_want[0] = s_coop_want[1] = -1;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) s_port_slot[p] = -1;
    memset(s_status_seen, 0, sizeof s_status_seen);
    memset(s_xs, 0, sizeof s_xs);
}
static void plug(int slot, int on, u16 buttons)
{
    s_xs[slot].connected = on;
    s_xs[slot].buttons = buttons;
    s_xs[slot].analog_lx = s_xs[slot].analog_ly = s_xs[slot].analog_rx = s_xs[slot].analog_ry = 128;
}

static void test_two_controllers_and_hotplug(void)
{
    reset("1", "", "", "");
    plug(0, 1, CELL_PAD_CTRL_CROSS);
    plug(1, 1, CELL_PAD_CTRL_START);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 0 && s_port_slot[1] == 1);
    assert(pad_port_on(0) && pad_port_on(1) && !pad_port_on(2));
    assert(s_host_state[1].buttons == CELL_PAD_CTRL_START);         /* player 2 presses START to join */
    /* The game is told once that port 1 changed. */
    assert(pad_port_status(1, 1) == (CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES));
    assert(pad_port_status(1, 1) == CELL_PAD_STATUS_CONNECTED);
    assert(pad_port_status(0, 1) == CELL_PAD_STATUS_CONNECTED);

    /* Player 2 unplugs: port 1 goes away and stays empty (no input) ... */
    plug(1, 0, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[1] == -1 && !pad_port_on(1));
    assert(pad_port_status(1, 0) == (CELL_PAD_STATUS_DISCONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES));
    assert(pad_port_status(1, 0) == CELL_PAD_STATUS_DISCONNECTED);
    /* ... player 1 keeps controller 1 ... */
    assert(s_port_slot[0] == 0 && pad_port_on(0));
    /* ... and a controller plugged in later takes the free port 1. */
    plug(2, 1, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[1] == 2 && pad_port_on(1));
    assert(pad_port_status(1, 1) & CELL_PAD_STATUS_ASSIGN_CHANGES);

    /* Player 1's controller gone: port 0 still counts as connected (keyboard). */
    plug(0, 0, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == -1 && pad_port_on(0) && s_port_slot[1] == 2);
}

static void test_chosen_controllers(void)
{
    reset("", "2", "1", "");
    plug(0, 1, 0); plug(1, 1, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 1 && s_port_slot[1] == 0);

    /* The chosen player 2 controller isn't there yet: a third one doesn't take its place. */
    reset("", "", "2", "");
    plug(0, 1, 0); plug(2, 1, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 0 && s_port_slot[1] == -1 && s_port_slot[2] == 2);
    plug(1, 1, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[1] == 1);
}

static void test_coop_off(void)
{
    reset("0", "", "", "1");                                         /* PAD_P2_KEYBOARD is ignored when off */
    plug(0, 1, 0); plug(1, 1, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 0 && s_port_slot[1] == -1);
    assert(pad_host_port(0) == 0 && pad_port_on(0) && !pad_port_on(1));
}

/* Default (PAD_COOP unset): one player, whatever is plugged in. A second
 * controller -- or a tool's virtual copy of the first one, pressing the same
 * START -- never connects port 1, so the game can't start split screen. */
static void test_default_single_player(void)
{
    reset("", "", "", "");
    plug(0, 1, 0); plug(1, 1, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 0 && s_port_slot[1] == -1 && !pad_port_on(1));
    plug(0, 1, CELL_PAD_CTRL_START); plug(1, 1, CELL_PAD_CTRL_START);   /* mirrored pads */
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 0 && !pad_port_on(1) && pad_port_status(1, pad_port_on(1)) == CELL_PAD_STATUS_DISCONNECTED);
    /* Player 1 follows the controller that is used while the current one is idle ... */
    plug(0, 1, 0); plug(1, 1, CELL_PAD_CTRL_CROSS);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 1 && s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);
    /* ... resting stick drift doesn't count, a stick pushed far does ... */
    plug(1, 1, 0); s_xs[0].analog_lx = 150;
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 1);
    s_xs[0].analog_lx = 250;
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 0);
    /* ... and both pads busy at once (a mirror) doesn't make it flip. */
    plug(0, 1, CELL_PAD_CTRL_UP); plug(1, 1, CELL_PAD_CTRL_UP);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 0);
    /* The controller is unplugged: the other one takes over. */
    plug(0, 0, 0); plug(1, 1, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[0] == 1 && pad_port_on(0) && !pad_port_on(1));
}

static void test_p2_keyboard(void)
{
    reset("", "", "", "1");
    plug(0, 1, CELL_PAD_CTRL_CIRCLE);
    pad_map_slots(s_xs, 4);
    /* Guest ports 0 and 1 swap: player 1 (guest 0) is the controller on internal port 1,
     * player 2 (guest 1) the keyboard on internal port 0. */
    assert(pad_host_port(0) == 1 && pad_host_port(1) == 0 && pad_host_port(2) == 2);
    assert(s_port_slot[0] == -1 && s_port_slot[1] == 0);
    assert(s_host_state[pad_host_port(0)].buttons & CELL_PAD_CTRL_CIRCLE);
    assert(pad_port_on(0) && pad_port_on(1) && !pad_port_on(2));
    /* Player 1's controller gone: guest 0 still reads as connected (port 0 always is,
     * as on a console with a pad), and the keyboard player stays. */
    plug(0, 0, 0);
    pad_map_slots(s_xs, 4);
    assert(s_port_slot[1] == -1 && pad_port_on(0) && pad_port_on(1));
}

int main(void)
{
    _putenv_s("PAD_BACKGROUND_INPUT", "1");   /* no game window here: keep the pads live */
    test_two_controllers_and_hotplug();
    test_chosen_controllers();
    test_coop_off();
    test_default_single_player();
    test_p2_keyboard();
    printf("test_pad_coop: all passed\n");
    return 0;
}
