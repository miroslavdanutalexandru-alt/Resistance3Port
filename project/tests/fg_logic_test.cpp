// Unit test for the frame generation decision logic shared by the three ports (no GPU, no game).
// R2/R3: clang-cl /nologo /O1 /EHsc /std:c++17 /I ps3recomp\third_party\rpcs3\rpcs3\Emu\RSX\VK project\tests\fg_logic_test.cpp /Fe:scratch\fg_logic_test.exe /Fo:scratch\fg_logic_test.obj
// R1:    clang-cl /nologo /O1 /EHsc /std:c++17 /DPFG_R1 /I ps3recomp\libs\video project\tests\fg_logic_test.cpp /Fe:scratch\fg_logic_test.exe /Fo:scratch\fg_logic_test.obj
#ifdef PFG_R1
#include "port_fg_logic.h"
#else
#include "VKR2FrameGenLogic.h"
#include "VKR2DeformLogic.h"
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- the R2/R3 fgcfg() parse before PORT_FG, verbatim (penv returns nullptr for unset or empty)
static bool same_word(const char* a, const char* b)
{
	if (!a || !b) return false;
	for (; *a && *b; ++a, ++b)
		if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
	return *a == *b;
}
static void old_parse(const char* fg, const char* b, int& want, int& backend, int& bad_mult, int& bad_backend)
{
	if (fg && !*fg) fg = nullptr;
	if (b && !*b) b = nullptr;
	const bool fsr_word = same_word(fg, "fsr3") || same_word(fg, "fsr");
	const int v = fsr_word ? 2 : (fg ? std::atoi(fg) : 0);
	want = (v >= 2 && v <= 4) ? v : 0;
	bad_mult = (v && !want) ? v : 0;
	backend = fsr_word ? 2 : same_word(b, "dlss") ? 1 : (same_word(b, "fsr3") || same_word(b, "fsr")) ? 2 : 0;
	bad_backend = b && !backend && !same_word(b, "auto");
}

static void test_legacy_equivalence()
{
	const char* fgs[] = { nullptr, "", "0", "1", "2", "3", "4", "5", "-1", "fsr3", "FSR3", "fsr", "Fsr", "fsr4", "on", "2x", " 2" };
	const char* bes[] = { nullptr, "", "auto", "AUTO", "dlss", "DLSS", "fsr3", "fsr", "xess", "dlssg" };
	for (const char* fg : fgs)
		for (const char* b : bes)
		{
			int w0, b0, m0, x0, w1, b1, m1, x1;
			old_parse(fg, b, w0, b0, m0, x0);
			pfg_parse_vk_legacy(fg, b, &w1, &b1, &m1, &x1);
			CHECK(w0 == w1 && b0 == b1 && m0 == m1 && x0 == x1);
		}
}

static void test_selector()
{
	CHECK(pfg_parse_sel(nullptr) == PFG_SEL_LEGACY);
	CHECK(pfg_parse_sel("") == PFG_SEL_LEGACY);
	CHECK(pfg_parse_sel("auto") == PFG_SEL_LEGACY);
	CHECK(pfg_parse_sel("dlssg") == PFG_SEL_DLSSG);
	CHECK(pfg_parse_sel("DLSS-G") == PFG_SEL_DLSSG);
	CHECK(pfg_parse_sel("dlss") == PFG_SEL_DLSSG);
	CHECK(pfg_parse_sel("FSR3") == PFG_SEL_FSR3);
	CHECK(pfg_parse_sel("fsr") == PFG_SEL_FSR3);
	CHECK(pfg_parse_sel("off") == PFG_SEL_OFF);
	CHECK(pfg_parse_sel("0") == PFG_SEL_OFF);
	CHECK(pfg_parse_sel("none") == PFG_SEL_OFF);
	CHECK(pfg_parse_sel("xess") == PFG_SEL_BAD);
	CHECK(pfg_parse_sel("fsr31") == PFG_SEL_BAD);

	// legacy untouched
	int w = 3, b = 0;
	pfg_apply_sel(PFG_SEL_LEGACY, &w, &b); CHECK(w == 3 && b == 0);
	pfg_apply_sel(PFG_SEL_BAD, &w, &b); CHECK(w == 3 && b == 0);
	// dlssg keeps 3x / 4x, else 2x, DLSS-G only
	w = 3; b = 2; pfg_apply_sel(PFG_SEL_DLSSG, &w, &b); CHECK(w == 3 && b == PFG_BE_DLSS);
	w = 0; b = 0; pfg_apply_sel(PFG_SEL_DLSSG, &w, &b); CHECK(w == 2 && b == PFG_BE_DLSS);
	// fsr3 is 2x on FSR 3, even when R?_DLSS_FG=4
	w = 4; b = 1; pfg_apply_sel(PFG_SEL_FSR3, &w, &b); CHECK(w == 2 && b == PFG_BE_FSR3);
	w = 0; b = 0; pfg_apply_sel(PFG_SEL_FSR3, &w, &b); CHECK(w == 2 && b == PFG_BE_FSR3);
	// off wins over R?_DLSS_FG=fsr3
	w = 2; b = 2; pfg_apply_sel(PFG_SEL_OFF, &w, &b); CHECK(w == 0);

	CHECK(pfg_hudless(nullptr, 1) == 1);
	CHECK(pfg_hudless(nullptr, 0) == 0);
	CHECK(pfg_hudless("", 0) == 0);
	CHECK(pfg_hudless("1", 0) == 1);
	CHECK(pfg_hudless("0", 1) == 0);
	CHECK(pfg_hudless("off", 1) == 0);
	CHECK(pfg_hudless("on", 0) == 1);

	CHECK(pfg_parse_pace(nullptr) == PFG_PACE_VRR);
	CHECK(pfg_parse_pace("vrr") == PFG_PACE_VRR);
	CHECK(pfg_parse_pace("junk") == PFG_PACE_VRR);
	CHECK(pfg_parse_pace("auto") == PFG_PACE_AUTO);
	CHECK(pfg_parse_pace("VSYNC") == PFG_PACE_AUTO);
	CHECK(pfg_parse_pace("off") == PFG_PACE_OFF);
}

// Rendered presents per second for a game capped at cap_fps, through the FSR 3 pacer (x2 = output fps).
// Mirrors fx_queue_present: wait until next, then next = pfg_pace_next(now, next, step). Units: microseconds.
static double sim_rendered_fps(double cap_fps, double step_s, double seconds = 10.0, long long stall_at = -1, long long stall_us = 0)
{
	const long long frame = (long long)std::llround(1e6 / cap_fps), step = (long long)std::llround(step_s * 1e6);
	const long long end = (long long)(seconds * 1e6);
	long long t = 0, next = 0, n = 0, game_next = 0;
	while (t < end)
	{
		t = std::max(t, game_next);                    // the game's own limiter
		if (stall_at >= 0 && n == stall_at) t += stall_us;
		if (step > 0)
		{
			if (next > t) t = next;
			next = pfg_pace_next(t, next, step);
		}
		game_next = t + frame;
		++n;
	}
	return n / seconds;
}

static void test_pacing()
{
	// default (vrr): 0.95 margin, like before PORT_FG_PACE
	CHECK(std::fabs(pfg_fx_step_s(1, PFG_PACE_VRR, 120.0, 1) - 2.0 / 114.0) < 1e-12);
	CHECK(pfg_fx_step_s(0, PFG_PACE_VRR, 120.0, 1) == 0.0);        // R?_DLSS_FG_CAP=0
	CHECK(pfg_fx_step_s(1, PFG_PACE_VRR, 0.0, 0) == 0.0);          // unknown refresh
	CHECK(pfg_fx_step_s(1, PFG_PACE_OFF, 120.0, 0) == 0.0);
	CHECK(pfg_fx_step_s(1, PFG_PACE_AUTO, 120.0, 1) == 0.0);       // vsync: the presenter paces
	CHECK(pfg_fx_step_s(1, PFG_PACE_AUTO, 120.0, 0) > 0.0);        // immediate / mailbox: still paced

	double fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 120.0, 0));
	std::printf("  60 cap, 120 Hz, vrr : %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 56.5 && fps < 57.5);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_AUTO, 120.0, 1));
	std::printf("  60 cap, 120 Hz, auto: %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 59.5 && fps < 60.5);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 144.0, 0));
	std::printf("  60 cap, 144 Hz, vrr : %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 59.5 && fps < 60.5);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 60.0, 0));
	std::printf("  60 cap,  60 Hz, vrr : %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 28.0 && fps < 29.0);

	// catch-up: after a stall the deadline restarts from now (no burst of back-to-back presents)
	CHECK(pfg_pace_next(1000, 900, 50) == 1050);      // fell behind by 100 > step: now + step
	CHECK(pfg_pace_next(1000, 980, 50) == 1030);      // within one step: keep the cadence
	CHECK(pfg_pace_next(1000, 1000, 50) == 1050);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 144.0, 0), 10.0, 100, 500000);
	CHECK(fps > 56.5 && fps < 60.5);

	CHECK(pfg_reflex_limit_us(1, 120.0, 1, 0) == (unsigned)(1e6 / 114.0));
	CHECK(pfg_reflex_limit_us(1, 120.0, 0, 1) == (unsigned)(1e6 / 114.0));
	CHECK(pfg_reflex_limit_us(1, 120.0, 0, 0) == 0);
	CHECK(pfg_reflex_limit_us(0, 120.0, 1, 0) == 0);
	CHECK(pfg_reflex_limit_us(1, 1.0, 1, 0) == 0);
}

static void test_hold()
{
	unsigned hold = 0;
	int prev = 0;
	// first frame on: enabled, reset (no history)
	int gen = pfg_fx_hold_step(&hold, 1, prev, 1);
	CHECK(gen && hold == PFG_FX_HOLD && pfg_fx_reset(prev, 0, 0, 1));
	prev = gen;
	gen = pfg_fx_hold_step(&hold, 1, prev, 1);
	CHECK(gen && !pfg_fx_reset(prev, 0, 0, 1));
	CHECK(pfg_fx_reset(prev, 1, 0, 1));   // new swapchain (resize / alt-tab fullscreen)
	CHECK(pfg_fx_reset(prev, 0, 1, 1));   // camera cut
	CHECK(pfg_cut_reset(1, PFG_CUT_UNTRUSTED) && !pfg_cut_reset(1, PFG_CUT_HELD) && !pfg_cut_reset(1, PFG_CUT_NONE));
	CHECK(pfg_cut_reset(2, PFG_CUT_HELD) && !pfg_cut_reset(0, PFG_CUT_UNTRUSTED));
	prev = gen;
	// pause / short gap: held enabled (as reset frames) for PFG_FX_HOLD - 1 frames, then off
	int held = 0;
	for (int i = 0; i < 40; ++i)
	{
		gen = pfg_fx_hold_step(&hold, 0, prev, 1);
		if (gen) { ++held; CHECK(pfg_fx_reset(prev, 0, 0, 0)); }
		prev = gen;
	}
	CHECK(held == (int)PFG_FX_HOLD - 1 && !gen && hold == 0);
	// once off it stays off until wanted again (prev_gen = 0)
	hold = 5;
	CHECK(!pfg_fx_hold_step(&hold, 0, 0, 1));
	// losing depth / motion vectors (movie, loading) ends the hold at once
	hold = 0; prev = pfg_fx_hold_step(&hold, 1, 0, 1);
	CHECK(!pfg_fx_hold_step(&hold, 0, prev, 0));
	// back on: enabled immediately, reset first frame
	CHECK(pfg_fx_hold_step(&hold, 1, 0, 1) && pfg_fx_reset(0, 0, 0, 1));
}

static void test_gate()
{
	pfg_gate_in g{};
	g.scene = 1; g.local_players = 1; g.now_ms = 10000; g.activity_ms = 9900; g.streak = 3; g.menu_off = 1;
	CHECK(pfg_gate(&g) == PFG_GATE_ON);
	{ auto h = g; h.streak = 2; CHECK(pfg_gate(&h) == PFG_GATE_WARMUP); }
	{ auto h = g; h.scene = 0; h.paused = 1; CHECK(pfg_gate(&h) == PFG_GATE_NO_SCENE); }        // order: scene first
	{ auto h = g; h.paused = 1; h.local_players = 2; CHECK(pfg_gate(&h) == PFG_GATE_PAUSED); }
	{ auto h = g; h.local_players = 2; CHECK(pfg_gate(&h) == PFG_GATE_SPLITSCREEN); }
	{ auto h = g; h.scrgb = 1; h.srgb = 1; CHECK(pfg_gate(&h) == PFG_GATE_SCRGB); }
	{ auto h = g; h.srgb = 1; CHECK(pfg_gate(&h) == PFG_GATE_SRGB); }
	{ auto h = g; h.fx_sc_bad = 1; h.pause_ui = 1; CHECK(pfg_gate(&h) == PFG_GATE_FX_SWAPCHAIN); }
	{ auto h = g; h.pause_ui = 1; h.bad_status = 4; CHECK(pfg_gate(&h) == PFG_GATE_MENU); }
	{ auto h = g; h.activity_ms = 8500; CHECK(pfg_gate(&h) == PFG_GATE_ON); }                   // exactly 1.5 s: still on
	{ auto h = g; h.activity_ms = 8499; CHECK(pfg_gate(&h) == PFG_GATE_MENU); }
	{ auto h = g; h.activity_ms = 0; CHECK(pfg_gate(&h) == PFG_GATE_ON); }                      // never had input
	{ auto h = g; h.activity_ms = 1; h.menu_off = 0; h.pause_ui = 1; CHECK(pfg_gate(&h) == PFG_GATE_ON); }
	{ auto h = g; h.bad_status = 1; h.streak = 0; CHECK(pfg_gate(&h) == PFG_GATE_BAD_STATUS); }

	// streak: a scene-less frame (movie / loading) restarts the warm-up
	unsigned s = 0;
	s = pfg_streak(s, 1); s = pfg_streak(s, 1); CHECK(s == 2);
	s = pfg_streak(s, 0); CHECK(s == 0);
	for (int i = 0; i < 3; ++i) s = pfg_streak(s, 1);
	CHECK(s == PFG_WARMUP_FRAMES);
}

#ifndef PFG_R1
// R?_DLSS_FG_GAP: a 1-2 frame scene gap mid-play resumes without the warm-up, longer gaps / short streaks warm up
static unsigned gap_run(unsigned streak_before, int gap_frames, unsigned max_gap, unsigned* gap)
{
	unsigned s = 0;
	*gap = 0;
	for (unsigned i = 0; i < streak_before; ++i) s = pfg_streak_gap(s, 1, gap, max_gap);
	for (int i = 0; i < gap_frames; ++i) s = pfg_streak_gap(s, 0, gap, max_gap);
	return pfg_streak_gap(s, 1, gap, max_gap);
}

static void test_gap()
{
	unsigned gap = 0;
	CHECK(gap_run(20, 1, 2, &gap) == PFG_WARMUP_FRAMES && gap == 0);
	CHECK(gap_run(PFG_GAP_MIN_STREAK, 2, 2, &gap) == PFG_WARMUP_FRAMES);
	CHECK(gap_run(20, 3, 2, &gap) == 1 && gap == 0);           // loading / movie: warm up
	CHECK(gap_run(PFG_GAP_MIN_STREAK - 1, 1, 2, &gap) == 1);  // flickering scene: warm up
	CHECK(gap_run(20, 1, 0, &gap) == 1);                       // R?_DLSS_FG_GAP=0: old behaviour
	CHECK(gap_run(20, 40, 2, &gap) == 1 && gap == 0);
	// a second gap right after a bridged one is not bridged (streak restarted at the warm-up length)
	unsigned s = gap_run(20, 1, 2, &gap);
	s = pfg_streak_gap(s, 0, &gap, 2);
	CHECK(s == 0 && gap == 0);
	CHECK(pfg_streak_gap(s, 1, &gap, 2) == 1);
	// with max_gap 0 it matches pfg_streak
	unsigned a = 0, b = 0;
	gap = 0;
	for (int i = 0; i < 30; ++i) { const int sc = (i % 7) != 3; a = pfg_streak(a, sc); b = pfg_streak_gap(b, sc, &gap, 0); CHECK(a == b); }
}

// RSX_DLSS_OBJMV_DEFORM capture policy (VKR2DeformLogic.h, VKR2Upscaler.cpp om2 record path)
static void test_deform()
{
	// dage: unpaired -> 255, changed -> 0, unchanged -> +1, saturating at 254 / staying 255
	CHECK(r2om_dage_next(false, true, 3) == 255);
	CHECK(r2om_dage_next(true, true, 255) == 0);
	CHECK(r2om_dage_next(true, false, 0) == 1);
	CHECK(r2om_dage_next(true, false, 253) == 254);
	CHECK(r2om_dage_next(true, false, 254) == 254);
	CHECK(r2om_dage_next(true, false, 255) == 255);
	// old behaviour unchanged for paired draws (new_cap / program history irrelevant)
	for (uint32_t nc : { 0u, 128u })
		for (bool pd : { false, true })
		{
			r2om_dwant w = r2om_deform_want(true, true, 0, 12, 0, 0, 768, nc, pd);
			CHECK(w.want && !w.carry && !w.fresh);
			w = r2om_deform_want(true, false, 12, 12, 0, 0, 768, nc, pd);
			CHECK(w.want && w.carry && !w.fresh);
			w = r2om_deform_want(true, false, 13, 12, 0, 0, 768, nc, pd);
			CHECK(!w.want);
			w = r2om_deform_want(true, false, 255, 12, 0, 0, 768, nc, pd);
			CHECK(!w.want);
			w = r2om_deform_want(true, false, 3, 0, 0, 0, 768, nc, pd);
			CHECK(!w.want);
		}
	// unpaired: off with new_cap 0 (old behaviour) or a program never seen deforming; on within its cap, never from
	// the reserved tail of the draw budget
	CHECK(!r2om_deform_want(false, false, 255, 12, 0, 0, 768, 0, true).want);
	CHECK(!r2om_deform_want(false, false, 255, 12, 0, 0, 768, 128, false).want);
	r2om_dwant f = r2om_deform_want(false, false, 255, 12, 0, 0, 768, 128, true);
	CHECK(f.want && f.fresh && !f.carry);
	CHECK(!r2om_deform_want(false, false, 255, 12, 10, 128, 768, 128, true).want);
	CHECK(r2om_deform_want(false, false, 255, 12, 639, 5, 768, 128, true).want);
	CHECK(!r2om_deform_want(false, false, 255, 12, 640, 5, 768, 128, true).want);
	CHECK(!r2om_deform_want(false, false, 255, 12, 4, 0, 8, 128, true).want); // reserve = max / 2
	CHECK(r2om_deform_want(false, false, 255, 12, 3, 0, 8, 128, true).want);
	CHECK(!r2om_deform_want(false, false, 255, 12, 0, 0, 0, 128, true).want);
	// bytes: first half of the buffer only, 1 MB per copy, rounded to the 256-byte copy alignment
	const uint64_t half = 32u << 20;
	CHECK(r2om_fresh_fits(0, 4096, half));
	CHECK(!r2om_fresh_fits(0, 0, half));
	CHECK(r2om_fresh_fits(0, R2OM_FRESH_MAX_BYTES, half));
	CHECK(!r2om_fresh_fits(0, R2OM_FRESH_MAX_BYTES + 1, half));
	CHECK(r2om_fresh_fits(half / 2 - 256, 256, half));
	CHECK(!r2om_fresh_fits(half / 2 - 256, 257, half));
	CHECK(!r2om_fresh_fits(half / 2, 16, half));
	// a frame of unpaired 1 MB copies leaves the paired draws at least half of the buffer
	{
		uint64_t used = 0; uint32_t dn = 0, dnn = 0;
		for (int i = 0; i < 1000; ++i)
			if (r2om_deform_want(false, false, 255, 12, dn, dnn, 768, 128, true).fresh && r2om_fresh_fits(used, R2OM_FRESH_MAX_BYTES, half))
			{ used += R2OM_FRESH_MAX_BYTES; dn++; dnn++; }
		CHECK(used <= half / 2 && dnn == 16);
	}
	CHECK(r2om_noprev_reason(false, 0) == R2OM_NP_UNPAIRED);
	CHECK(r2om_noprev_reason(true, 255) == R2OM_NP_FIRST);
	CHECK(r2om_noprev_reason(true, 20) == R2OM_NP_AGED);

	// Simulation: a skinned draw whose vertices change every frame and whose pairing flickers (paired on odd frames).
	// A frame's draw gets real motion when it is paired, changed and its previous-frame draw kept a copy.
	for (uint32_t nc : { 0u, 128u })
	{
		bool prev_copy = false, prev_matched = false;
		uint8_t prev_dage = 255;
		int used = 0, unp = 0;
		for (int fr = 0; fr < 40; ++fr)
		{
			const bool matched = (fr & 1) != 0;
			const bool dyn = matched; // changed vs its pair (unpaired: unknown)
			const uint8_t dage = matched ? r2om_dage_next(true, dyn, prev_dage) : 255;
			const r2om_dwant w = r2om_deform_want(matched, dyn, dage, 12, 0, 0, 768, nc, true);
			if (matched && dyn)
			{
				if (prev_copy) used++;
				else if (r2om_noprev_reason(prev_matched, prev_dage) == R2OM_NP_UNPAIRED) unp++;
			}
			prev_copy = w.want;
			prev_matched = matched;
			prev_dage = dage;
		}
		CHECK(nc ? (used == 20 && unp == 0) : (used == 0 && unp == 20));
	}
	// RSX_DLSS_OBJMV_DEFORM_ID: exact identities only
	{
		const uint32_t i5 = 5, i9 = 9;
		auto one = [&](uint32_t k) -> const uint32_t* { return k == 0 ? &i5 : nullptr; };
		auto two = [&](uint32_t k) -> const uint32_t* { return k == 0 ? &i5 : k == 1 ? &i9 : nullptr; };
		auto none = [&](uint32_t) -> const uint32_t* { return nullptr; };
		CHECK(r2om_id_prev(true, 77, true, 66, one) == 5);
		CHECK(r2om_id_prev(false, 77, true, 66, one) == -1);  // off
		CHECK(r2om_id_prev(true, 77, true, 66, two) == -1);   // two previous draws had it: ambiguous
		CHECK(r2om_id_prev(true, 77, true, 66, none) == -1);  // nobody had it
		CHECK(r2om_id_prev(true, 77, false, 66, one) == -1);  // second draw of the identity this frame
		CHECK(r2om_id_prev(true, 77, true, 77, one) == -1);   // the address pair is that identity already
		CHECK(r2om_id_prev(true, 0, true, 66, one) == -1);    // no identity (indexed / loose matching off)
	// spawn: A drawn alone last frame; new identical B drawn first this frame looks unique when it decides and takes A's copy -> RSX_DLSS_OBJMV_DEFORM_ID defaults off
	CHECK(r2om_id_prev(true, 77, true, 66, one) == 5);
	}
}
// ---- r2vp_most_common vs the old first-seen table (unbounded), randomized with many ties; plus its cost
static r2vp_pick vp_pick_linear(const std::vector<std::pair<uint64_t, uint32_t>>& in, bool& ok)
{
	std::vector<r2vp_pick> cs;
	for (const auto& p : in)
	{
		size_t j = 0;
		while (j < cs.size() && cs[j].h != p.first) ++j;
		if (j < cs.size()) cs[j].n++;
		else cs.push_back({ p.first, 1, p.second });
	}
	ok = !cs.empty();
	size_t best = 0;
	for (size_t j = 1; j < cs.size(); ++j) if (cs[j].n > cs[best].n) best = j;
	return ok ? cs[best] : r2vp_pick{};
}

static void test_vp_pick()
{
	std::mt19937_64 rng(12345);
	for (int t = 0; t < 3000; ++t)
	{
		const uint32_t n = static_cast<uint32_t>(rng() % 300), alpha = 1 + static_cast<uint32_t>(rng() % (t % 3 ? 12 : 400));
		std::vector<std::pair<uint64_t, uint32_t>> v;
		uint32_t idx = 0;
		for (uint32_t k = 0; k < n; ++k)
		{
			idx += 1 + static_cast<uint32_t>(rng() % 3); // skipped (unverified) draws leave gaps
			v.emplace_back(0x9e3779b97f4a7c15ull * (1 + rng() % alpha), idx);
		}
		bool ok0;
		const r2vp_pick a = vp_pick_linear(v, ok0);
		r2vp_pick b{};
		const bool ok1 = r2vp_most_common(v, b);
		CHECK(ok0 == ok1 && (!ok0 || (a.h == b.h && a.n == b.n && a.i == b.i)));
	}
	// R2 worst case: 3000 verified draws, all distinct but a shared VP on 300 of them
	std::vector<std::pair<uint64_t, uint32_t>> big, w;
	for (uint32_t k = 0; k < 3000; ++k) big.emplace_back(k % 10 == 9 ? 7ull : 0x9e3779b97f4a7c15ull * (k + 100), k);
	bool ok;
	const int reps = 50;
	auto t0 = std::chrono::steady_clock::now();
	uint64_t sink = 0;
	for (int r = 0; r < reps; ++r) sink += vp_pick_linear(big, ok).n;
	auto t1 = std::chrono::steady_clock::now();
	for (int r = 0; r < reps; ++r) { w = big; r2vp_pick p{}; r2vp_most_common(w, p); sink += p.n; }
	auto t2 = std::chrono::steady_clock::now();
	CHECK(sink == 2ull * reps * 300);
	std::printf("r2vp_most_common, 3000 draws: %.1f us (linear table %.1f us)\n",
		std::chrono::duration<double, std::micro>(t2 - t1).count() / reps, std::chrono::duration<double, std::micro>(t1 - t0).count() / reps);
}
// RSX_DLSS_OBJMV_PAIR_NEAREST: instance re-pairing
static void test_pair_nearest()
{
	const double NaN = std::nan("");
	// previous-frame occurrence distances to the current draw (index = ordinal; < 0 missing)
	auto run = [](std::vector<double> d, uint32_t ord, uint32_t win, double d0, double* od = nullptr, int* calls = nullptr) {
		return r2om_pair_nearest(ord, win, d0, 0.5, [&](uint32_t k, double) { if (calls) ++*calls; return k < d.size() ? d[k] : -1.0; }, od);
	};
	double od = -1;
	CHECK(run({ 9, 5, 10, 2, 7 }, 2, 4, 10, &od) == 3 && od == 2); // the closest, not the first under the bound
	CHECK(run({ 9, 6, 10, 7, 8 }, 2, 4, 10) == -1);                  // nothing under half d0: keep the ordinal pair
	CHECK(run({ 9, 3, 10, 3, 8 }, 2, 4, 10) == 1);                   // tie: the lower ordinal (tried first)
	int calls = 0;
	CHECK(run({ 9, 0, 10, 0, 1 }, 2, 4, 10, &od, &calls) == 1 && od == 0 && calls == 1); // exact: stops
	CHECK(run({ -1, -1, 10, -1, 4 }, 2, 4, 10) == 4);                // missing occurrences skipped
	CHECK(run({ 10, 1 }, 0, 4, 10) == 1);                            // ordinal 0: no underflow to 2^32-1
	CHECK(run({ 1, 10, 1 }, 1, 0, 10) == -1);                        // window 0
	CHECK(run({ 1, 10 }, 1, 4, 0) == -1);                            // the pair is identical (d0 0): nothing to do
	CHECK(run({ 1, 10 }, 1, 4, NaN) == -1);                          // NaN-safe
	CHECK(run({ NaN, 10, 1 }, 1, 4, 10) == 2);                       // a NaN candidate is never chosen
	CHECK(run({ 1, 10 }, 1, 4, HUGE_VAL) == 0);                      // incomparable pair: any candidate
	CHECK(run({ 0, 0, 0, 0, 0, 0, 0 }, 3, 2, 10, &od) == 2 && od == 0);
	calls = 0;
	CHECK(run(std::vector<double>(40, 9.0), 20, 16, 10, nullptr, &calls) == -1 && calls == 32);
}
#endif

int main()
{
	test_legacy_equivalence();
	test_selector();
	test_pacing();
	test_hold();
	test_gate();
#ifndef PFG_R1
	test_gap();
	test_deform();
	test_vp_pick();
	test_pair_nearest();
#endif
	std::printf("fg_logic_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
