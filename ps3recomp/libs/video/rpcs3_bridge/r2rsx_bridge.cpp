// r2rsx_bridge: drives RPCS3's RSX core from the recompiled runtime (see r2rsx_bridge.h).
// Built as part of the rpcs3_rsx library (third_party/rpcs3/CMakeLists.txt) so it sees the
// RPCS3 headers and compile flags; the game only links the C API. GPLv2, like RPCS3.
#include "stdafx.h"
#ifndef PORT_ZCULL_RELAXED_DEFAULT
#define PORT_ZCULL_RELAXED_DEFAULT 1 // R3: relaxed ZCULL sync unless R2_ZCULL=sync (third_party/rpcs3/CMakeLists.txt)
#endif
#ifndef PORT_ZCULL_COUNT_APPROX_DEFAULT
#define PORT_ZCULL_COUNT_APPROX_DEFAULT 0 // R3: exact Z-pass counts unless RSX_ZCULL_COUNT=approx (lens flares fade by count)
#endif
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/emu_callbacks.h"
#include "Emu/Memory/vm.h"
#include "Emu/Cell/lv2/sys_rsx.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Core/RSXContext.h"
#include "Emu/RSX/Core/RSXRendererFactory.h"
#include "Emu/RSX/port_prof.h"
#include "Emu/RSX/port_texdiag.h" // R2PORT: [texcache]
#include "Emu/RSX/port_rsx_stall.h" // R3PORT: RSX_STALL_DIAG
#include "Emu/RSX/gcm_printing.h"
#include "../../../include/ps3emu/port_hitch.h" // R2PORT: [hitch] logger
#include "util/logs.hpp"
#include "util/fixed_typemap.hpp"

#include "r2rsx_bridge.h"

#include <windows.h>
// ---- R2PORT autoconfig (launcher/autoconfig owner): shared R2_PRESET mapping, used only in apply_launcher_overrides ----
#include "../../../../project/r2_autoconfig.h"
#include "../../../../project/port_native_summary.h"
// ---- end R2PORT autoconfig ----
#include <tlhelp32.h> // R2PORT perf: R2_RSX_PROF thread times
#include "r2rsx_host_sampler.h" // R3PORT perf r8: RSX_HOST_SAMPLE=1 rsx::thread host sampler
#include "r2rsx_pipeline_seed.h" // PORT: tools\pipeline_seed.bin -> shader cache
#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

extern "C" uint8_t* vm_base;
extern "C" uint8_t* vm_sudo_base;
extern "C" void cpu_replay_rsx_replay(uint32_t dma_control_ea); // cpu-replay: rpcs3_shim/cpu_replay_rsx.cpp
extern "C" int (*g_vm_access_violation_hook)(uint32_t guest_addr, int is_write);
// project/stubs.cpp: R2_FPS target (0 = uncapped) and the display refresh, as main.cpp uses them
extern "C" double r2_fps_target(void);
extern "C" double r2_display_refresh_hz(void);

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;
static bool s_prof_overlay = false; // r2rsx_prof_enable (F3 overlay)

namespace r2port
{
	extern std::string g_rsx_cache_dir;
	extern int g_pipe_compiler_threads; // VK/VKPipelineCompiler.cpp
	extern int g_shader_preload;        // VK/VKPipelineCompiler.cpp: PORT_SHADER_PRELOAD
	extern int g_gpu_texdecode; // VK/VKTexture.cpp (RSX_GPU_TEXDECODE)
}
namespace rsx::FIFO { bool inline_active(); } // RSXFIFO.cpp (RSX_FIFO_INLINE)
namespace rsx { bool r2_zcull_idle_flush_enabled(); } // RSXThread.cpp (RSX_ZCULL_IDLE_FLUSH)
namespace vk { bool r2_sampler_cache_active(); bool r2_vtx_reuse_active(); bool r2_ibind_skip_active(); bool r2_vprun_fast_active(); } // VKDraw.cpp / VKVertexBuffers.cpp (RSX_SAMPLER_CACHE, RSX_VTX_REUSE, RSX_IBIND_SKIP, RSX_VPRUN_FAST)

namespace rsx
{
	extern std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;
}
namespace r2qa { void on_screenshot(std::vector<u8>&& data, u32 w, u32 h, bool bgra); } // R2PORT QA: rpcs3_shim/r2_qa_capture.cpp

namespace
{
	int s_enabled = -1;
	bool s_up = false;
	rsx::thread* s_render = nullptr;

	HWND s_hwnd = nullptr;
	std::mutex s_win_mtx;
	std::condition_variable s_win_cv;

	std::unique_ptr<logs::listener> s_log;

	// Guest handlers run here, like on RPCS3's _gcm_intr_thread: in order, off the RSX thread.
	std::mutex s_intr_mtx;
	std::condition_variable s_intr_cv;
	std::deque<std::pair<u32, u32>> s_intr_queue;

	void intr_thread_main()
	{
		for (;;)
		{
			std::pair<u32, u32> call;
			{
				std::unique_lock lock(s_intr_mtx);
				s_intr_cv.wait(lock, [] { return !s_intr_queue.empty(); });
				call = s_intr_queue.front();
				s_intr_queue.pop_front();
			}

			if (g_ps3_guest_caller && call.first)
			{
				g_ps3_guest_caller(call.first, call.second, 0, 0, 0, 0, 0, 0, 0);
			}
		}
	}

	class r2_gs_frame final : public GSFrameBase
	{
	public:
		void close() override {}
		void reset() override {}
		video_renderer renderer() const override { return video_renderer::vulkan; }
		bool shown() override { return true; }
		void hide() override {}
		void show() override {}
		void toggle_fullscreen() override {}
		void delete_context(draw_context_t) override {}
		draw_context_t make_context() override { return nullptr; }
		void set_current(draw_context_t) override {}
		void flip(draw_context_t, bool) override {}

		int client_width() override
		{
			RECT r{};
			return GetClientRect(s_hwnd, &r) ? std::max<int>(1, r.right - r.left) : 1280;
		}

		int client_height() override
		{
			RECT r{};
			return GetClientRect(s_hwnd, &r) ? std::max<int>(1, r.bottom - r.top) : 720;
		}

		f64 client_display_rate() override { const double hz = r2_display_refresh_hz(); return hz >= 1.0 ? hz : 60.; }
		bool has_alpha() override { return false; }
		display_handle_t handle() const override { return s_hwnd; }
		bool can_consume_frame() const override { return false; }
		void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
		void take_screenshot(std::vector<u8>&& d, u32 w, u32 h, bool bgra) override { r2qa::on_screenshot(std::move(d), w, h, bgra); } // R2PORT QA: R2_QA_CAPTURE
		void update_title(double) override {}
	};

	std::string exe_dir()
	{
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buf, MAX_PATH);
		return std::filesystem::path(buf).parent_path().string();
	}

	// Launcher graphics settings (settings.bat): applied over rpcs3_config.yml. Unset = yml value.
	void apply_launcher_overrides()
	{
		auto env = [](const char* n) -> const char* { const char* e = std::getenv(n); return e && *e ? e : nullptr; };
		// ---- R2PORT autoconfig: R2_PRESET=auto|low|medium|high|ultra (project/r2_autoconfig.h) ----
		// Without the launcher: fills the renderer vars below (RSX_RES_SCALE/FILTER/SHARPEN/ANISO/FSR/UPSCALE)
		// from the same mapping Launcher.exe uses. Only unset vars are filled, so explicit env vars always win.
		// R2_FPS, RSX_VSYNC, PORT_CPU_PROFILE and R2_SHADER_MODE are read before the renderer starts; they
		// need r2ac::apply_preset_env(getenv("R2_PRESET"), r2ac::Phase::startup) at the top of main().
		if (const char* p = env("R2_PRESET"))
		{
			char sum[512] = {};
			r2ac::apply_preset_env(p, r2ac::Phase::renderer, sum, sizeof sum);
			std::printf("[r2rsx] R2_PRESET=%s: %s\n", p, sum);
		}
		// ---- end R2PORT autoconfig ----
		if (const char* e = env("RSX_RES_SCALE"))
		{
			const double s = std::atof(e);
			if (s >= 0.25 && s <= 8.0) g_cfg.video.resolution_scale_percent.set(static_cast<u32>(s * 100.0 + 0.5));
		}
		if (const char* e = env("RSX_FILTER"))
		{
			if (!_stricmp(e, "point") || !_stricmp(e, "nearest")) g_cfg.video.output_scaling.set(output_scaling_mode::nearest);
			else if (!_stricmp(e, "fsr")) g_cfg.video.output_scaling.set(output_scaling_mode::fsr);
			else g_cfg.video.output_scaling.set(output_scaling_mode::bilinear);
		}
		if (const char* e = env("RSX_SHARPEN"))
		{
			const double s = std::atof(e);
			if (s >= 0.0 && s <= 1.0) g_cfg.video.rcas_sharpening_intensity.set(static_cast<u32>(s * 100.0 + 0.5));
		}
		if (const char* e = env("RSX_ANISO"))
		{
			const int a = std::atoi(e);
			if (a >= 0 && a <= 16) g_cfg.video.anisotropic_level_override.set(static_cast<u32>(a));
		}
		// RSX_ADAPTER: the Vulkan GPU by its exact name (as listed in logs\rpcs3_rsx.log, "Found Vulkan-compatible GPU"),
		// over rpcs3_config.yml's Video/Vulkan/Adapter. Unset = the first discrete GPU. Lets a hybrid laptop run on the
		// iGPU (AMD-only rendering bugs) without editing the yml.
		if (const char* e = env("RSX_ADAPTER"))
		{
			g_cfg.video.vk.adapter.set(e);
			std::printf("[r2rsx] Vulkan adapter: '%s' (RSX_ADAPTER)\n", e);
		}
		// FSR 3.1 / DLAA (RSX_FSR=1/2, VK/VKR2Upscaler.cpp) replace the game's 2x MSAA: it needs single-sample scene targets.
		if (const char* e = env("RSX_FSR"); e && std::atoi(e) > 0)
			g_cfg.video.antialiasing_level.set(msaa_level::none);
		// Host VSync (swapchain present mode): 0 off, 1 on, adaptive.
		if (const char* e = env("RSX_VSYNC"))
			g_cfg.video.vsync.set(!_stricmp(e, "adaptive") ? vsync_mode::adaptive : std::atoi(e) > 0 ? vsync_mode::full : vsync_mode::off);
		// R2_PRESENT names the Vulkan present mode directly (wins over RSX_VSYNC): immediate = tearing allowed
		// (VRR / G-Sync / FreeSync with an fps cap), mailbox = no tearing, no vsync wait (falls back to relaxed
		// FIFO), fifo = classic vsync.
		if (const char* e = env("R2_PRESENT"))
		{
			if (!_stricmp(e, "immediate") || !_stricmp(e, "tearing") || !_stricmp(e, "vrr")) g_cfg.video.vsync.set(vsync_mode::off);
			else if (!_stricmp(e, "mailbox")) g_cfg.video.vsync.set(vsync_mode::adaptive);
			else if (!_stricmp(e, "fifo") || !_stricmp(e, "vsync")) g_cfg.video.vsync.set(vsync_mode::full);
		}
		// R3_ULTRAWIDE (Hor+, project/r3_patches.cpp): the game still renders 16:9 targets with a wider frustum,
		// so the picture is stretched over the whole window instead of pillarboxed (RSX_HUD_FIX=1: HUD kept 16:9).
		if (const char* e = env("R3_ULTRAWIDE"); e && _stricmp(e, "off") && std::strcmp(e, "0") && _stricmp(e, "16:9"))
		{
			g_cfg.video.stretch_to_display_area.set(true);
			std::printf("[r2rsx] R3_ULTRAWIDE=%s: Hor+ frustum, picture stretched to the window\n", e);
		}
		// RSX_DISPLAY=exclusive: the fullscreen window (rsx_null_backend.c) may take Vulkan exclusive fullscreen.
		if (const char* e = env("RSX_DISPLAY"))
			g_cfg.video.vk.exclusive_fullscreen_mode.set(!_stricmp(e, "exclusive") ? vk_exclusive_fs_mode::enable : vk_exclusive_fs_mode::disable);
		// RSX_VRAM_LIMIT_MB (test switch, R3 #33): caps the renderer's video memory as if the GPU had only this much
		// (256..65536), to reproduce out-of-VRAM handling on a big GPU. Unset = no cap.
		if (const char* e = env("RSX_VRAM_LIMIT_MB"))
		{
			const int mb = std::atoi(e);
			if (mb >= 256 && mb <= 65536)
			{
				g_cfg.video.vk.vram_allocation_limit.set(static_cast<u32>(mb));
				std::printf("[r2rsx] RSX_VRAM_LIMIT_MB=%d: video memory capped at %d MB (test)\n", mb, mb);
			}
		}
		std::printf("[r2rsx] video: scale %u%%, output scaling %d, sharpen %u, aniso %u, vsync %d, exclusive fs %d\n",
			g_cfg.video.resolution_scale_percent.get(), static_cast<int>(g_cfg.video.output_scaling.get()),
			g_cfg.video.rcas_sharpening_intensity.get(), g_cfg.video.anisotropic_level_override.get(),
			static_cast<int>(g_cfg.video.vsync.get()), static_cast<int>(g_cfg.video.vk.exclusive_fullscreen_mode.get()));
	}

	// Shader compilation and logging defaults (env overrides, applied over rpcs3_config.yml).
	//  R2_SHADER_MODE = sync (default: a pipeline missing from the cache is built on the render thread: a short
	//                   hitch the first time, no skipped draws / purple-green patches; GPL parts + fast link)
	//                 | async (async recompiler, draws skipped until ready)
	//                 | interp (async recompiler + shader interpreter until ready; untested) | interpreter
	//  R2_SHADER_INTERP=1 (default 0): same as R2_SHADER_MODE=interp; R2_SHADER_MODE wins when set.
	//  R2_SHADER_CACHE=0 disables the on-disk shader cache (shadercache\rpcs3\shaders_cache).
	void apply_shader_defaults()
	{
		const char* m = std::getenv("R2_SHADER_MODE");
		shader_mode mode = shader_mode::recompiler;
		if (m && *m)
		{
			if (!_stricmp(m, "async")) mode = shader_mode::async_recompiler;
			else if (!_stricmp(m, "interp")) mode = shader_mode::async_with_interpreter;
			else if (!_stricmp(m, "sync") || !_stricmp(m, "legacy")) mode = shader_mode::recompiler;
			else if (!_stricmp(m, "interpreter")) mode = shader_mode::interpreter_only;
		}
		else if (const char* i = std::getenv("R2_SHADER_INTERP"); i && *i && *i != '0' && _stricmp(i, "off"))
		{
			mode = shader_mode::async_with_interpreter;
		}
		g_cfg.video.shadermode.set(mode);
		const char* c = std::getenv("R2_SHADER_CACHE");
		g_cfg.video.disable_on_disk_shader_cache.set(c && *c == '0');
		g_cfg.video.log_programs.set(false);
		// PORT_SHADER_PRELOAD (default on; launcher "Compile shaders before playing", the same switch in all three ports):
		// on = every cached pipeline is built before the first frame, off = none at start-up or in the background, on demand only (issue #10)
		// (VKGSRender.cpp preload_shaders; R2_PRELOAD_BOOT still overrides).
		r2port::g_shader_preload = port_native_parse_bool(std::getenv("PORT_SHADER_PRELOAD"), 1);
		std::printf("[r2rsx] shaders: mode %d, disk cache %s, preload %s (PORT_SHADER_PRELOAD)\n", static_cast<int>(mode),
			g_cfg.video.disable_on_disk_shader_cache.get() ? "off" : "on", r2port::g_shader_preload ? "on" : "off");
	}

	// ---- R2PORT perf (RPCS3 path speed / pacing / quality switches) ----
	const char* penv(const char* n) { const char* e = std::getenv(n); return e && *e ? e : nullptr; }

	// 1/0 for "1/on/true/yes" and "0/off/false/no"; -1 when unset or unrecognised.
	int penv_bool(const char* n)
	{
		return port_native_parse_bool(penv(n), -1);   // the shared switch rules (port_native_summary.h)
	}

	// Host CPU as project/cpu_profile.inc classifies it: PORT_CPU_PROFILE, else cores available to the
	// process (hybrid: P + E/2) <= 4 low, < 8 mid, else high.
	enum { cpu_off = 0, cpu_low, cpu_mid, cpu_high };
	struct host_cpu_t { int logical = 1; double eff_cores = 1; int profile = cpu_high; bool forced = false; };

	host_cpu_t detect_host_cpu()
	{
		host_cpu_t c;
		DWORD_PTR pm = 0, sm = 0;
		if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) pm = ~DWORD_PTR{0};
		c.logical = std::max(1, std::popcount(static_cast<unsigned long long>(pm)));
		c.logical = std::min<int>(c.logical, static_cast<int>(std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : c.logical));

		DWORD len = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
		std::vector<u8> buf(len);
		int cores = 0, pcores = 0, ecores = 0, max_class = 0;
		if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
		{
			for (int pass = 0; pass < 2; pass++)
			{
				for (DWORD o = 0; o < len;)
				{
					const auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + o);
					if (p->Relationship == RelationProcessorCore && p->Processor.GroupMask[0].Group == 0 &&
						(p->Processor.GroupMask[0].Mask & pm))
					{
						const int cls = p->Processor.EfficiencyClass;
						if (pass == 0) { max_class = std::max(max_class, cls); cores++; }
						else if (max_class > 0 && cls == 0) ecores++;
						else pcores++;
					}
					o += p->Size;
				}
			}
		}
		c.eff_cores = cores ? (max_class > 0 ? pcores + ecores / 2.0 : static_cast<double>(cores)) : c.logical;
		c.profile = c.eff_cores <= 4.0 ? cpu_low : c.eff_cores < 8.0 ? cpu_mid : cpu_high;
		if (const char* e = penv("PORT_CPU_PROFILE"); e && _stricmp(e, "auto"))
		{
			c.forced = true;
			if (!_stricmp(e, "low")) c.profile = cpu_low;
			else if (!_stricmp(e, "mid")) c.profile = cpu_mid;
			else if (!_stricmp(e, "high")) c.profile = cpu_high;
			else if (!_stricmp(e, "off") || !std::strcmp(e, "0")) c.profile = cpu_off;
			else c.forced = false;
		}
		return c;
	}

	// RPCS3's vblank / frame limit follow R2_FPS the way project/main.cpp sets them for the other
	// renderers (frame_clock): above 60 fps the vblank rate is the target, uncapped uses
	// max(display refresh, 240) Hz; the frame limit is one vblank ("Auto") at 30 fps and off otherwise,
	// where the game limiter (r2_frame_period) paces. RSX_VBLANK_US, RSX_CFG_LEGACY,
	// GCM_VBLANK_FOLLOW_FPS=0 and GCM_FRAME_LIMIT=off|vblank|<Hz> override as they do there.
	void apply_pacing()
	{
		const double fps = r2_fps_target();
		const char* vbe = penv("RSX_VBLANK_US");
		const char* leg = penv("RSX_CFG_LEGACY");
		const char* follow = penv("GCM_VBLANK_FOLLOW_FPS");
		double vb_hz = 0.; // 0: keep rpcs3_config.yml (Vblank Rate 60)
		if (vbe && std::atoll(vbe) >= 1000)
		{
			vb_hz = 1e6 / static_cast<double>(std::atoll(vbe));
		}
		else if (!(follow && *follow == '0'))
		{
			double hz = fps;
			if (fps <= 0.0) { hz = r2_display_refresh_hz(); if (hz < 240.0) hz = 240.0; }
			if (hz > 60.5) vb_hz = std::min(hz, 1000.0); // main.cpp: vblank period >= 1000 us
		}
		if (vb_hz > 0.)
		{
			g_cfg.video.vblank_rate.set(std::clamp<s64>(std::llround(vb_hz), 1, 6000));
			g_cfg.video.vblank_ntsc.set(false);
		}
		else if (leg && *leg != '0')
		{
			// 59.94 Hz: RPCS3 period (1'000'000 + 1000) / 60 = 16683 us, as RSX_CFG_LEGACY in main.cpp
			g_cfg.video.vblank_rate.set(60);
			g_cfg.video.vblank_ntsc.set(true);
		}

		const char* fl = penv("GCM_FRAME_LIMIT");
		if (fl && (!_stricmp(fl, "off") || !std::strcmp(fl, "0") || !_stricmp(fl, "none")))
		{
			g_cfg.video.frame_limit.set(frame_limit_type::none);
			g_cfg.video.second_frame_limit.set(0);
		}
		else if (fl && !_stricmp(fl, "vblank"))
		{
			g_cfg.video.frame_limit.set(frame_limit_type::_auto);
		}
		else if (fl && std::atof(fl) >= 1.0)
		{
			g_cfg.video.frame_limit.set(frame_limit_type::none);
			g_cfg.video.second_frame_limit.set(std::min(std::atof(fl), 1000.0));
		}
		else if (!(fps > 0.0 && fps <= 60.5) || (fps != 30.0 && !(fl && *fl)))
		{
			// Above 60 fps / uncapped: the game limiter paces. R2PORT: also 31..60 fps -- there the game
			// limiter (r2_frame_period) already paces the frame loop, and the RSX flip limiter on top of it
			// was a second 60 Hz clock: the RSX thread slept in the flip while the PPU waited on its labels
			// ([hitch] guestwait ~30 ms, then a frame of limiter debt run at once). GCM_FRAME_LIMIT=vblank:
			// the old double pacing.
			g_cfg.video.frame_limit.set(frame_limit_type::none);
			g_cfg.video.second_frame_limit.set(0);
		}
		// else: 30 fps (the game's own vsync wait paces) keeps the yml limit (Auto = one vblank)

		std::printf("[r2rsx] pacing: R2_FPS %.1f -> vblank %d Hz%s, frame limit %d, second limit %.1f\n",
			fps, static_cast<int>(g_cfg.video.vblank_rate.get()), g_cfg.video.vblank_ntsc.get() ? " (NTSC 59.94)" : "",
			static_cast<int>(g_cfg.video.frame_limit.get()), static_cast<double>(g_cfg.video.second_frame_limit.get()));
	}

	// Speed and quality switches over rpcs3_config.yml. Unset = yml value (no visual change by default).
	void apply_port_perf()
	{
		const host_cpu_t cpu = detect_host_cpu();
		static const char* const pn[] = { "off", "low", "mid", "high" };

		// Async pipeline compiler pool (draws with an uncompiled pipeline are skipped until it is ready).
		// RPCS3's auto: 1 below 8 threads, 2 at 8, 4/6/8/12 above. Port: low keeps cpu_profile.inc's
		// RSX_VK_PIPELINE_THREADS (1, or 2 with >= 8 threads); mid/high use the threads the game leaves
		// free (logical - 8, 2..4 mid, 2..12 high). R2_SHADER_THREADS=N overrides; profile off = RPCS3's.
		int sct = 0;
		if (const char* e = penv("R2_SHADER_THREADS")) sct = std::clamp(std::atoi(e), 0, 16);
		else if (cpu.profile == cpu_low)
		{
			const char* p = penv("RSX_VK_PIPELINE_THREADS");
			sct = p ? std::clamp(std::atoi(p), 1, 4) : (cpu.logical >= 8 ? 2 : 1);
		}
		else if (cpu.profile == cpu_mid) sct = std::clamp(cpu.logical - 8, 2, 4);
		else if (cpu.profile == cpu_high) sct = std::clamp(cpu.logical - 8, 2, 12);
		if (g_cfg.video.shader_compiler_threads_count.get() == 0)
			r2port::g_pipe_compiler_threads = sct;

		// Multithreaded RSX (the "RSX Offloader": guest->host copies off the RSX thread). yml: on.
		if (const int b = penv_bool("R2_MTRSX"); b >= 0) g_cfg.video.multithreaded_rsx.set(b != 0);

		// A/B switches that may change the image; defaults keep the yml.
		if (const int b = penv_bool("R2_WCB"); b >= 0) g_cfg.video.write_color_buffers.set(b != 0);
		// R3PORT (R2 parity): "Handle RSX Memory Tiling" forced on by default (RPCS3 #19676 / #19785; R3 ships it on and
		// its ADS/DOF main-memory targets are tiled -- off gives striped garbage). tools/update.ps1 keeps a user-edited
		// rpcs3_config.yml, which may still have it off. RSX_TILING=0 (alias R2_RSX_TILING) turns it off;
		// project/main.cpp r3_yml_tiling applies the same rule to the native GCM path.
		{
			int b = penv_bool("RSX_TILING");
			if (b < 0) b = penv_bool("R2_RSX_TILING");
			const bool yml = g_cfg.video.handle_tiled_memory.get();
			g_cfg.video.handle_tiled_memory.set(b != 0);
			std::printf("[r2rsx] tiling: Handle RSX Memory Tiling %s (%s; yml %s)\n", b != 0 ? "on" : "off",
				b < 0 ? "default, RSX_TILING=0 turns it off" : "RSX_TILING", yml ? "on" : "off");
		}
		// RSX_NO_BLIT_UPSCALE=1: RPCS3's Debug option "Disable Blit Engine Upscaling" (yml default false). A blit target
		// that is in no cache becomes a surface-cache render target since RPCS3 PR #19013; normally it is created at the
		// resolution scale, with this at 100%. Workaround listed in RPCS3 issue #19391 (Insomniac blit regression).
		if (const int b = penv_bool("RSX_NO_BLIT_UPSCALE"); b >= 0) g_cfg.video.disable_blit_engine_upscaling.set(b != 0);
		// R3PORT: RSX_ZCULL_COUNT=approx|precise (default precise since 2026-11-25: approx drew lens flares at full
		// strength through wall/roof gaps in Haven; the carpet stayed stable with precise in the user test; PORT_ZCULL_COUNT_APPROX_DEFAULT). Resistance 3 hides
		// an object when its occlusion proxy's Z-pass count is <= a per-object threshold (func_0074EAB0:
		// flag = max(count - thr, 0)). Here the count is rasterised at the DLSS/FSR reduced scale with a per-frame
		// sub-pixel jitter and rescaled to guest pixels, so a proxy near its threshold reads a different count from
		// frame to frame and with the camera angle: the object (e.g. Haven's carpet) clips in and out. Approximate
		// (RPCS3 "ZCULL Accuracy: Approximate", recommended for R3 in RPCS3 #12051/#15928) reports 0xFFFF for any
		// visible pixel: never hidden while any pixel passes, never hidden by count noise. Keeps relaxed sync.
		{
			int approx = -1; // -1 = no RSX_ZCULL_COUNT
			if (const char* e = penv("RSX_ZCULL_COUNT"))
			{
				char w[16];
				if (port_native_word(e, w, sizeof w) && (!std::strcmp(w, "approx") || !std::strcmp(w, "approximate"))) approx = 1;
				else if (port_native_word(e, w, sizeof w) && (!std::strcmp(w, "precise") || !std::strcmp(w, "exact"))) approx = 0;
				else std::printf("[r2rsx] RSX_ZCULL_COUNT=\"%s\" not recognised (approx|precise); using the default\n", e);
			}
			const char* why = "RSX_ZCULL_COUNT";
			if (approx < 0)
			{
				if (const int b = penv_bool("R2_ZCULL_ACCURATE"); b >= 0) { approx = b ? 0 : 1; why = "R2_ZCULL_ACCURATE"; }
				else { approx = PORT_ZCULL_COUNT_APPROX_DEFAULT ? 1 : 0; why = "default"; }
			}
			g_cfg.video.precise_zpass_count.set(approx == 0);
			std::printf("[r2rsx] ZCULL counts: %s (%s; RSX_ZCULL_COUNT=approx|precise)\n",
				approx ? "approximate (any visible pixel reports visible)" : "precise (exact pixel counts)", why);
		}
		// R3PORT: PORT_ZCULL_RELAXED_DEFAULT (CMake, per port; R3 = 1 since the 2026-11-12 play test, paired with
		// the blocking occlusion wait R3_OCC_BLOCKING=1 (default) so late reports cannot pop objects; R2 = 1) picks the default.
		// Relaxed (the RPCS3 wiki's Resistance 3 setting) was the old default here: Occlusion results are written
		// once the GPU has them instead of stalling rsx::thread at every report sync point (bench r3a: 3 syncs and
		// 8.5 ms per frame); a report the game reads may be up to a frame old. The Z-pass counts themselves stay as
		// precise as R2_ZCULL_ACCURATE / the yml say. R2_ZCULL=sync restores the yml (strict) behaviour.
		if (const char* e = penv("R2_ZCULL"))
		{
			// relaxed: report reads do not wait on the GPU (results may lag a frame);
			// off: no occlusion queries at all (every query reports visible); sync/anything else: strict
			g_cfg.video.relaxed_zcull_sync.set(!_stricmp(e, "relaxed"));
			g_cfg.video.disable_zcull_queries.set(!_stricmp(e, "off"));
		}
		else
		{
			g_cfg.video.relaxed_zcull_sync.set(PORT_ZCULL_RELAXED_DEFAULT != 0);
		}
		// R3PORT: R3_ZCULL_APPROX=1 = RPCS3's "ZCULL Accuracy: Approximate" (RPCS3 #15928: fixes R3's zoom/ADS fps
		// drop seen with Relaxed). Strict report sync, and a Z-pass report writes 0xFFFF for any hit instead of the
		// exact sample count: a report can only grow (any visible pixel -> max), never read as occluded when the exact
		// count would not, so it errs towards drawing. Default off. Overrides R2_ZCULL / R2_ZCULL_ACCURATE.
		if (penv_bool("R3_ZCULL_APPROX") == 1)
		{
			g_cfg.video.relaxed_zcull_sync.set(false);
			g_cfg.video.disable_zcull_queries.set(false);
			g_cfg.video.precise_zpass_count.set(false);
			std::printf("[r2rsx] ZCULL accuracy: approximate (R3_ZCULL_APPROX=1; any-hit reports, strict sync)\n");
		}
		if (const int b = penv_bool("R2_ASYNC_TEX"); b >= 0) g_cfg.video.vk.asynchronous_texture_streaming.set(b != 0);
		if (const int b = penv_bool("R2_HOST_LABELS"); b >= 0) g_cfg.video.host_label_synchronization.set(b != 0);

		// Quality (off/yml by default)
		if (const char* e = penv("R2_MSAA"); e && !(penv("RSX_FSR") && std::atoi(penv("RSX_FSR")) > 0))
			g_cfg.video.antialiasing_level.set(!_stricmp(e, "0") || !_stricmp(e, "off") || !_stricmp(e, "none") ? msaa_level::none : msaa_level::_auto);
		if (const int b = penv_bool("R2_HIGHP_Z"); b >= 0) g_cfg.video.force_high_precision_z_buffer.set(b != 0);
		if (const char* e = penv("R2_LOD_BIAS")) g_cfg.video.texture_lod_bias.set(std::clamp(std::atof(e), -32.0, 32.0));
		if (const int b = penv_bool("R2_GPU_TEX_SCALING"); b >= 0) g_cfg.video.use_gpu_texture_scaling.set(b != 0);

		apply_pacing();

		r2prof::enabled = penv_bool("R2_RSX_PROF") == 1 || s_prof_overlay;
		r2prof::census_on = r2prof::enabled && penv_bool("R2_RSX_CENSUS") == 1; // R2PORT round 9

		std::printf("[r2rsx] perf: cpu %s%s (%d threads, %.1f cores), shader compilers %d, MTRSX %d, async tex %d, "
			"WCB %d, ZCULL accurate %d relaxed %d off %d, host labels %d | MSAA %d, high-precision Z %d, LOD bias %.2f, "
			"GPU tex scaling %d | profiling %s\n",
			pn[cpu.profile], cpu.forced ? " (PORT_CPU_PROFILE)" : "", cpu.logical, cpu.eff_cores,
			r2port::g_pipe_compiler_threads ? r2port::g_pipe_compiler_threads : static_cast<int>(g_cfg.video.shader_compiler_threads_count.get()),
			g_cfg.video.multithreaded_rsx.get() ? 1 : 0, g_cfg.video.vk.asynchronous_texture_streaming.get() ? 1 : 0,
			g_cfg.video.write_color_buffers.get() ? 1 : 0, g_cfg.video.precise_zpass_count.get() ? 1 : 0,
			g_cfg.video.relaxed_zcull_sync.get() ? 1 : 0, g_cfg.video.disable_zcull_queries.get() ? 1 : 0,
			g_cfg.video.host_label_synchronization.get() ? 1 : 0,
			static_cast<int>(g_cfg.video.antialiasing_level.get()), g_cfg.video.force_high_precision_z_buffer.get() ? 1 : 0,
			static_cast<double>(g_cfg.video.texture_lod_bias.get()), g_cfg.video.use_gpu_texture_scaling.get() ? 1 : 0,
			r2prof::enabled ? "on (R2_RSX_PROF, every 5 s)" : "off");
	}

	// GPU offload switches (shared naming with R1/R3: RSX_GPU_*). Graphics work runs on the GPU; each feature that is
	// on prints one "[gpu-offload] <name> ON" line. Vertex fetch/decode is always GPU-side in this renderer (texel
	// buffers read by the vertex shader); tiled surface detile/encode is always GPU compute (cs_tile_memcpy).
	//  RSX_GPU_TEXDECODE (default 1): texture deswizzle + byteswap in compute shaders (cs_deswizzle_3d / cs_shuffle)
	//    for images >= 1 KB; bit-exact vs the CPU path (scratch\gpu_offload\vk_texdecode_test). 0 = CPU decode.
	//  RSX_GPU_BLIT (opt-in; alias R2_GPU_TEX_SCALING): blit-engine copies between main-memory images on the GPU
	//    instead of the CPU detile/scale path (nv3089). Filtering differs from the CPU path -> not bit-exact.
	//  RSX_GPU_TEXSTREAM (opt-in; alias R2_ASYNC_TEX): texture uploads on a separate Vulkan transfer/compute queue.
	//  The switches parse like every [native] switch (port_native_parse_bool). main.cpp's start-up [native] line
	//  cannot know the yml/alias result yet, so the effective state is printed here, in the same format.
	void apply_gpu_offload()
	{
		const int td = port_native_parse_bool(penv("RSX_GPU_TEXDECODE"), 1);
		r2port::g_gpu_texdecode = td;
		if (const int b = port_native_parse_bool(penv("RSX_GPU_BLIT"), -1); b >= 0) g_cfg.video.use_gpu_texture_scaling.set(b != 0);
		if (const int b = port_native_parse_bool(penv("RSX_GPU_TEXSTREAM"), -1); b >= 0) g_cfg.video.vk.asynchronous_texture_streaming.set(b != 0);
		const bool detile = g_cfg.video.handle_tiled_memory.get();
		const bool blit = g_cfg.video.use_gpu_texture_scaling.get();
		const bool stream = g_cfg.video.vk.asynchronous_texture_streaming.get();
		std::printf("[gpu-offload] vertex fetch ON (vertex shader)\n");
		if (detile) std::printf("[gpu-offload] surface detile ON (compute)\n");
		if (td) std::printf("[gpu-offload] texture decode ON (RSX_GPU_TEXDECODE)\n");
		if (blit) std::printf("[gpu-offload] blit ON (RSX_GPU_BLIT)\n");
		if (stream) std::printf("[gpu-offload] texture streaming ON (RSX_GPU_TEXSTREAM)\n");
		std::fflush(stdout);
		// Effective state (after the yml and the R2_* aliases), not just the RSX_GPU_* values.
		const port_native_feature k_gpu[] = {
			{ "gpu-texdecode", "RSX_GPU_TEXDECODE", 1 }, { "gpu-blit", "RSX_GPU_BLIT", 0 },
			{ "gpu-texstream", "RSX_GPU_TEXSTREAM", 0 },
			{ "rsx-fifo-inline", "RSX_FIFO_INLINE", 1 },
			{ "rsx-sampler-cache", "RSX_SAMPLER_CACHE", 1 }, { "rsx-vtx-reuse", "RSX_VTX_REUSE", 0 },
			{ "rsx-ibind-skip", "RSX_IBIND_SKIP", 1 }, { "rsx-vprun-fast", "RSX_VPRUN_FAST", 1 },
			{ "rsx-zcull-idle-flush", "RSX_ZCULL_IDLE_FLUSH", 1 },
		};
		const int k_gpu_on[] = { td, blit, stream, rsx::FIFO::inline_active() ? 1 : 0,
			vk::r2_sampler_cache_active() ? 1 : 0, vk::r2_vtx_reuse_active() ? 1 : 0,
			vk::r2_ibind_skip_active() ? 1 : 0, vk::r2_vprun_fast_active() ? 1 : 0,
			// effective: the idle flush only runs with relaxed ZCULL sync and queries on
			(rsx::r2_zcull_idle_flush_enabled() && g_cfg.video.relaxed_zcull_sync.get() && !g_cfg.video.disable_zcull_queries.get()) ? 1 : 0 };
		port_native_summary_state(k_gpu, k_gpu_on, (int)(sizeof k_gpu / sizeof k_gpu[0]));
	}

	// R2_RSX_PROF=1: every 5 s, one block of [r2rsx-prof] lines on stdout.
	struct thread_cpu_t { std::wstring name; u64 cpu100ns = 0; };

	// CPU time (100 ns units) of this process's named RSX threads, by thread id.
	// R2PORT round 7: the thread scan (system-wide toolhelp snapshot + open/describe every thread
	// of the process) cost the monitor ~0.25 s of CPU per 5 s sample in rp8 (thrcpu "r2rsx prof
	// monitor" 20-30% one window in five). The matching threads are cached with their handles and
	// the scan is redone only every 12th sample (1 min) or when a cached thread has exited.
	struct rsx_thread_ref { DWORD tid; HANDLE h; std::wstring name; };
	std::vector<rsx_thread_ref> s_rsx_threads;
	unsigned s_rsx_scan_age = ~0u;
	u64 s_mon_scan_ns = 0, s_mon_scans = 0, s_mon_last_work_ns = 0;

	void scan_rsx_threads()
	{
		const u64 t0 = r2prof::now_ns();
		for (auto& t : s_rsx_threads) CloseHandle(t.h);
		s_rsx_threads.clear();
		const DWORD pid = GetCurrentProcessId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 te{};
			te.dwSize = sizeof(te);
			for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
				if (!h) continue;
				PWSTR d = nullptr;
				if (SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0] &&
					(wcsstr(d, L"rsx::thread") || wcsstr(d, L"RSX ") || wcsstr(d, L"RSX.") || wcsstr(d, L"VBlank")))
				{
					s_rsx_threads.push_back({ te.th32ThreadID, h, d });
					h = nullptr;
				}
				if (d) LocalFree(d);
				if (h) CloseHandle(h);
			}
			CloseHandle(snap);
		}
		s_rsx_scan_age = 0;
		s_mon_scan_ns += r2prof::now_ns() - t0;
		s_mon_scans++;
	}

	std::map<DWORD, thread_cpu_t> sample_rsx_threads()
	{
		if (++s_rsx_scan_age >= 12 || s_rsx_threads.empty()) scan_rsx_threads();
		std::map<DWORD, thread_cpu_t> out;
		bool exited = false;
		for (const auto& t : s_rsx_threads)
		{
			FILETIME c, e, k, u;
			DWORD code = 0;
			if (!GetExitCodeThread(t.h, &code) || code != STILL_ACTIVE) { exited = true; continue; }
			if (GetThreadTimes(t.h, &c, &e, &k, &u))
			{
				const u64 kk = (u64{k.dwHighDateTime} << 32) | k.dwLowDateTime;
				const u64 uu = (u64{u.dwHighDateTime} << 32) | u.dwLowDateTime;
				out[t.tid] = { t.name, kk + uu };
			}
		}
		if (exited) s_rsx_scan_age = ~0u - 1; // rescan on the next sample
		return out;
	}

	struct prof_snap_t
	{
		u64 frames, draws, submits, setup, program, vertex, texture, exec, flip, fb_ns, draw_ns;
		u64 vreq, vmiss, plook, pelide, flushes, hard, flush_ns, zr, zr_ns, av, av_h, av_ns, av_fl;
		u64 x[r2prof::x_count];
	};

	prof_snap_t prof_snap()
	{
		auto& g = r2prof::g;
		auto l = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
		prof_snap_t s{ l(g.frames), l(g.draws), l(g.submits), l(g.setup_us), l(g.program_us), l(g.vertex_us), l(g.texture_us),
			l(g.exec_us), l(g.flip_us), l(g.fb_ns), l(g.draw_ns), l(g.vtx_cache_req), l(g.vtx_cache_miss), l(g.prog_lookups),
			l(g.prog_elided), l(g.flushes), l(g.hard_syncs), l(g.flush_ns), l(g.zcull_reads), l(g.zcull_read_ns),
			l(g.av_calls), l(g.av_handled), l(g.av_ns), l(g.av_flushes), {} };
		for (int i = 0; i < r2prof::x_count; ++i)
			s.x[i] = (i == r2prof::av_max_ns || i == r2prof::fbb_lk_max_ns) ? g.x[i].exchange(0, std::memory_order_relaxed) : l(g.x[i]);
		return s;
	}

	void prof_thread_main()
	{
		SetThreadDescription(GetCurrentThread(), L"r2rsx prof monitor");
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // R2PORT round 7: never preempt game threads
		const auto self_cpu_ns = [] {
			FILETIME c, e, k, u;
			if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return u64{0};
			return ((u64{k.dwHighDateTime} << 32 | k.dwLowDateTime) + (u64{u.dwHighDateTime} << 32 | u.dwLowDateTime)) * 100;
		};
		prof_snap_t p0 = prof_snap();
		auto t0 = std::chrono::steady_clock::now();
		auto th0 = sample_rsx_threads();
		u64 idle0 = s_render ? s_render->performance_counters.idle_time.load() : 0;

		u64 mon_cpu0 = self_cpu_ns(), mon_scan0 = s_mon_scan_ns, mon_scans0 = s_mon_scans;
		// R2PORT round 7: the block is formatted into one buffer and written once, so the CRT
		// stdout lock is held for a single write instead of ~16 printf calls.
		std::string blk;
		const auto bp = [&blk](const char* fmt, auto... args) {
			char b[2048];
			const int n = std::snprintf(b, sizeof(b), fmt, args...);
			if (n > 0) blk.append(b, std::min<size_t>(static_cast<size_t>(n), sizeof(b) - 1));
		};
		for (;;)
		{
			Sleep(5000);
			const u64 work_t0 = r2prof::now_ns();
			const prof_snap_t p1 = prof_snap();
			const auto t1 = std::chrono::steady_clock::now();
			auto th1 = sample_rsx_threads();
			const u64 idle1 = s_render ? s_render->performance_counters.idle_time.load() : 0;

			const double wall_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
			const double secs = wall_us / 1e6;
			const double idle_us = static_cast<double>(idle1 >= idle0 ? idle1 - idle0 : idle1);
			const double frames = static_cast<double>(p1.frames - p0.frames);
			const double draws = static_cast<double>(p1.draws - p0.draws);
			const double fr = std::max(frames, 1.0), dr = std::max(draws, 1.0);

			// rsx::thread CPU and the other RSX threads
			double rsx_cpu_us = 0;
			std::map<std::wstring, double> others;
			for (const auto& [tid, t] : th1)
			{
				const auto it = th0.find(tid);
				const double us = static_cast<double>(t.cpu100ns - (it != th0.end() ? it->second.cpu100ns : 0)) / 10.0;
				std::wstring key = t.name;
				if (key.rfind(L"RSX.W", 0) == 0) key = L"RSX.W*";
				if (t.name == L"rsx::thread") rsx_cpu_us += us;
				else others[key] += us;
			}

			const double busy_us = std::max(0.0, wall_us - idle_us);
			const double ph_setup = static_cast<double>(p1.setup - p0.setup), ph_prog = static_cast<double>(p1.program - p0.program);
			const double ph_vtx = static_cast<double>(p1.vertex - p0.vertex), ph_tex = static_cast<double>(p1.texture - p0.texture);
			const double ph_exec = static_cast<double>(p1.exec - p0.exec), ph_flip = static_cast<double>(p1.flip - p0.flip);
			const double ph_fb = static_cast<double>(p1.fb_ns - p0.fb_ns) / 1000.0;
			const double draw_us = static_cast<double>(p1.draw_ns - p0.draw_ns) / 1000.0;
			const double in_draw_other = std::max(0.0, draw_us - (ph_fb + ph_setup + ph_prog + ph_vtx + ph_tex + ph_exec));
			const double non_draw = std::max(0.0, busy_us - draw_us - ph_flip);

			bp("[r2rsx-prof] %.1fs: %.1f fps, %.0f draws/f | rsx::thread cpu %.0f%%, busy %.0f%% (wall - FIFO/limiter idle) | "
				"busy %.2f us/draw (target < 6), cpu %.2f us/draw\n",
				secs, frames / secs, draws / fr, rsx_cpu_us * 100.0 / wall_us, busy_us * 100.0 / wall_us,
				busy_us / dr, rsx_cpu_us / dr);
			bp("[r2rsx-prof]   us/draw: draw %.2f = fb-bind %.2f + setup %.2f + program %.2f + textures %.2f + vertex %.2f + exec %.2f + other %.2f"
				" | outside draws %.2f (FIFO methods, flushes, ZCULL) | present %.2f ms/f\n",
				draw_us / dr, ph_fb / dr, ph_setup / dr, ph_prog / dr, ph_tex / dr, ph_vtx / dr, ph_exec / dr, in_draw_other / dr,
				non_draw / dr, ph_flip / 1000.0 / fr);
			const double vreq = static_cast<double>(p1.vreq - p0.vreq), plook = static_cast<double>(p1.plook - p0.plook);
			bp("[r2rsx-prof]   per frame: submits %.1f, flushes %.1f (hard %.2f, %.2f ms), ZCULL GPU reads %.1f (%.2f ms) | "
				"vertex cache miss %.0f%%, program lookups elided %.0f%%\n",
				static_cast<double>(p1.submits - p0.submits) / fr, static_cast<double>(p1.flushes - p0.flushes) / fr,
				static_cast<double>(p1.hard - p0.hard) / fr, static_cast<double>(p1.flush_ns - p0.flush_ns) / 1e6 / fr,
				static_cast<double>(p1.zr - p0.zr) / fr, static_cast<double>(p1.zr_ns - p0.zr_ns) / 1e6 / fr,
				vreq > 0 ? static_cast<double>(p1.vmiss - p0.vmiss) * 100.0 / vreq : 0.0,
				plook > 0 ? static_cast<double>(p1.pelide - p0.pelide) * 100.0 / plook : 0.0);
			const double av = static_cast<double>(p1.av - p0.av);
			std::string ths;
			for (const auto& [n, us] : others)
			{
				char b[96];
				std::snprintf(b, sizeof(b), "%s%ls %.0f%%", ths.empty() ? "" : ", ", n.c_str(), us * 100.0 / wall_us);
				ths += b;
			}
			bp("[r2rsx-prof]   page faults to RSX: %.0f/s, %.2f us each, %.2f ms/s, handled %.0f/s, GPU readbacks %.1f/s | threads: %s\n",
				av / secs, av > 0 ? static_cast<double>(p1.av_ns - p0.av_ns) / 1000.0 / av : 0.0,
				static_cast<double>(p1.av_ns - p0.av_ns) / 1e6 / secs, static_cast<double>(p1.av_h - p0.av_h) / secs,
				static_cast<double>(p1.av_fl - p0.av_fl) / secs, ths.empty() ? "-" : ths.c_str());

			// Round-2 breakdowns
			const auto dx = [&](r2prof::xc i) { return static_cast<double>(p1.x[i] - p0.x[i]); };
			const double fbb = std::max(dx(r2prof::fbb_full), 1.0);
			bp("[r2rsx-prof]   fb-bind: full rebinds %.1f/f (same-layout %.1f/f), us per full rebind %.1f = aa/dlss %.1f + surface cache %.1f + "
				"WCB copies %.1f + locks %.1f + renderpass/fbo %.1f | WCB copies %.1f/f, skipped (rebound) %.1f/f, DMA flushes in bind %.1f/f\n",
				dx(r2prof::fbb_full) / fr, dx(r2prof::fbb_ignore) / fr, dx(r2prof::fbb_total_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_aa_ns) / 1000.0 / fbb, dx(r2prof::fbb_prep_ns) / 1000.0 / fbb, dx(r2prof::fbb_wcb_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_lock_ns) / 1000.0 / fbb, dx(r2prof::fbb_rp_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_wcb_copies) / fr, dx(r2prof::fbb_wcb_skipped) / fr, dx(r2prof::fbb_dma_flush) / fr);
			const double zr = std::max(static_cast<double>(p1.zr - p0.zr), 1.0);
			const double avfl = std::max(static_cast<double>(p1.av_fl - p0.av_fl), 1.0);
			bp("[r2rsx-prof]   flushes by cause /f: zcull hint %.1f, zcull hard %.1f, fault request %.1f, bind DMA %.1f | "
				"ZCULL: syncs %.1f/f, %.1f reads/sync, batched reads %.1f/f (%.0f slots each), GPU poll loops %.1f/read, fault flushes served in wait %.1f/s\n",
				dx(r2prof::fl_zcull_hint) / fr, dx(r2prof::fl_zcull_hard) / fr, dx(r2prof::fl_av_request) / fr, dx(r2prof::fl_bind_dma) / fr,
				dx(r2prof::zc_sync_calls) / fr, static_cast<double>(p1.zr - p0.zr) / std::max(dx(r2prof::zc_sync_calls), 1.0),
				dx(r2prof::zc_batch_calls) / fr, dx(r2prof::zc_batch_slots) / std::max(dx(r2prof::zc_batch_calls), 1.0),
				dx(r2prof::zc_poll_loops) / zr, dx(r2prof::zc_flush_serviced) / secs);
			bp("[r2rsx-prof]   readback faults: wait for rsx::thread %.0f us + copy/GPU wait %.0f us avg, worst %.0f us | "
				"vertex uploads uncacheable %.0f%% | program slow-path loads %.1f/f\n",
				dx(r2prof::av_wait_ns) / 1000.0 / avfl, dx(r2prof::av_flush_ns) / 1000.0 / avfl, static_cast<double>(p1.x[r2prof::av_max_ns]) / 1000.0,
				vreq > 0 ? dx(r2prof::vtx_uncacheable) * 100.0 / vreq : 0.0, std::max(0.0, plook / 2.0 - dx(r2prof::prog_memo_hits)) / fr);
			if (dx(r2prof::smp_memo_hits) + dx(r2prof::smp_memo_miss) + dx(r2prof::vtx_reuse_hits) + dx(r2prof::ibind_skipped) > 0)
			{
				bp("[r2rsx-prof]   RSX_SAMPLER_CACHE: memo hits %.1f/f, derivations %.1f/f | RSX_VTX_REUSE: reused uploads %.1f/f (%.1f KB/f) | "
					"RSX_IBIND_SKIP: index binds left out %.1f/f | RSX_ATEST_RESAMPLE: re-uploads avoided %.1f/f\n",
					dx(r2prof::smp_memo_hits) / fr, dx(r2prof::smp_memo_miss) / fr, dx(r2prof::vtx_reuse_hits) / fr, dx(r2prof::vtx_reuse_bytes) / 1024.0 / fr,
					dx(r2prof::ibind_skipped) / fr, dx(r2prof::smp_bias_only) / fr);
				bp("[r2rsx-prof]   resolve split us/draw: r2aa texture reads %.2f (jittered-content checks %.1f/f, RSX_AA_TEXREAD_MEMO hits %.1f/f, RSX_AA_LINK_MEMO hits %.1f/f), r3fx texture reads %.2f, get_current programs %.2f\n",
					dx(r2prof::aa_texr_ns) / 1000.0 / dr, dx(r2prof::aa_texr_jd) / fr, dx(r2prof::aa_texr_memo) / fr, dx(r2prof::aa_link_memo) / fr, dx(r2prof::fx_texr_ns) / 1000.0 / dr, dx(r2prof::prog_getcur_ns) / 1000.0 / dr);
				bp("[r2rsx-prof]   texture env split us/draw: expiry tests %.2f, upload_texture %.2f (%.1f/f, %.2f us each; search on memo miss %.2f), fs sampler %.2f, vs units %.2f\n",
					dx(r2prof::tex_expire_ns) / 1000.0 / dr, dx(r2prof::tex_upl_ns) / 1000.0 / dr, dx(r2prof::tex_upl_n) / fr,
					dx(r2prof::tex_upl_n) ? dx(r2prof::tex_upl_ns) / 1000.0 / dx(r2prof::tex_upl_n) : 0.0, dx(r2prof::tex_search_ns) / 1000.0 / dr,
					dx(r2prof::tex_smp_ns) / 1000.0 / dr, dx(r2prof::tex_vs_ns) / 1000.0 / dr);
			}

			// Round-3: outside draws, self times (nested scopes subtracted), ms per frame
			const double ms = 1e6 * fr;
			const double dlss = dx(r2prof::dlss_ns), dlss_draw = dx(r2prof::dlss_draw_ns);
			const double od_known = dx(r2prof::od_clear_ns) + dx(r2prof::od_fbb_ns) + dx(r2prof::od_zcull_ns) + dx(r2prof::od_flush_ns) +
				dx(r2prof::od_label_ns) + dx(r2prof::od_blit_ns) + dx(r2prof::od_flip_ns) + dx(r2prof::od_house_ns) +
				dx(r2prof::od_fl_drain_ns) + dx(r2prof::od_fl_reset_ns) + dx(r2prof::od_fl_end_ns) + dx(r2prof::od_fl_emu_ns); // R2PORT round 6: flip sub-buckets
			const double flip_all = dx(r2prof::od_flip_ns) + dx(r2prof::od_fl_drain_ns) + dx(r2prof::od_fl_reset_ns) + dx(r2prof::od_fl_end_ns) + dx(r2prof::od_fl_emu_ns);
			bp("[r2rsx-prof]   busy excluding DLSS/FSR hooks %.2f us/draw (hooks %.2f ms/f, %.2f of it inside draws) | outside draws ms/f: "
				"%.2f = clears %.2f (%.1f/f) + fb-bind %.2f (dlss/aa %.2f) + ZCULL wait %.2f + flushes %.2f + labels %.2f + blits %.2f + flip cmd %.2f + "
				"housekeeping %.2f + FIFO/methods %.2f\n",
				std::max(0.0, busy_us - dlss / 1000.0) / dr, dlss / ms, dlss_draw / ms,
				non_draw * 1000.0 / ms, dx(r2prof::od_clear_ns) / ms, dx(r2prof::od_clears) / fr,
				dx(r2prof::od_fbb_ns) / ms, std::max(0.0, dlss - dlss_draw) / ms, dx(r2prof::od_zcull_ns) / ms, dx(r2prof::od_flush_ns) / ms,
				dx(r2prof::od_label_ns) / ms, dx(r2prof::od_blit_ns) / ms, flip_all / ms, dx(r2prof::od_house_ns) / ms,
				std::max(0.0, non_draw * 1000.0 - od_known) / ms);
			bp("[r2rsx-prof]   ZCULL labels: deferred %.1f/f, released async %.1f/f, blocking drains %.1f/f (%.2f ms/f), fence sleeps %.1f/f | "
				"program: lookup %.2f us/draw, MRU hits %.1f/f, pipeline memo hits %.1f/f, re-sent FP kept %.1f/f, re-sent VP kept %.1f/f, dirty at draw /f: vp ucode %.1f, fp ucode/rehash %.1f, vp state-only %.1f, fp state-only %.1f "
				"(state-only keeps the hint unless R2_PROG_HINT=0)\n",
				dx(r2prof::zc_deferred) / fr, dx(r2prof::zc_released) / fr, dx(r2prof::zc_drain_block) / fr, dx(r2prof::zc_drain_ns) / ms,
				dx(r2prof::zc_fence_waits) / fr, dx(r2prof::prog_lookup_ns) / 1000.0 / dr, dx(r2prof::prog_mru_hits) / fr, dx(r2prof::prog_memo_hits) / fr, dx(r2prof::prog_fp_rebind) / fr, dx(r2prof::prog_vp_rebind) / fr,
				dx(r2prof::prog_vp_ucode) / fr, dx(r2prof::prog_fp_ucode) / fr, dx(r2prof::prog_vp_state) / fr, dx(r2prof::prog_fp_state) / fr);
			// Round-4: fb-bind lock phase, texture cache, VirtualProtect, flip, program split
			bp("[r2rsx-prof]   fb-bind locks us/rebind: discard %.1f + orphan %.1f + color %.1f + depth %.1f, worst %.0f us, slow (>20us) %.1f/f | "
				"keep (R2_FBB_KEEP) depth %.1f/f color %.1f/f\n",
				dx(r2prof::fbb_lk_discard_ns) / 1000.0 / fbb, dx(r2prof::fbb_lk_orphan_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_lk_color_ns) / 1000.0 / fbb, dx(r2prof::fbb_lk_depth_ns) / 1000.0 / fbb,
				static_cast<double>(p1.x[r2prof::fbb_lk_max_ns]) / 1000.0, dx(r2prof::fbb_lk_slow) / fr,
				dx(r2prof::fbb_keep_depth) / fr, dx(r2prof::fbb_keep_color) / fr);
			const double vpc = std::max(dx(r2prof::mm_vp_calls), 1.0);
			bp("[r2rsx-prof]   texture cache: mutex wait %.2f ms/f, invalidations %.1f/f (%.2f ms/f), sections unprotected %.1f/f, relocks %.1f/f, fresh locks %.1f/f | "
				"VirtualProtect: %.1f/f, %.2f ms/f (%.1f us each, %.1f pages each), in fb-bind %.1f/f (%.2f ms/f), skipped same-prot (R2_MM_SKIPSAME) %.1f/f | "
				"mm queue lock wait %.2f ms/f, Offloader mm flushes %.1f/f\n",
				dx(r2prof::tc_mutex_wait_ns) / ms, dx(r2prof::tc_inval_calls) / fr, dx(r2prof::tc_inval_ns) / ms, dx(r2prof::tc_unprotected) / fr,
				dx(r2prof::tc_relock) / fr, dx(r2prof::tc_fresh_lock) / fr,
				dx(r2prof::mm_vp_calls) / fr, dx(r2prof::mm_vp_ns) / ms, dx(r2prof::mm_vp_ns) / 1000.0 / vpc, dx(r2prof::mm_vp_pages) / vpc,
				dx(r2prof::mm_vp_fbb_calls) / fr, dx(r2prof::mm_vp_fbb_ns) / ms, dx(r2prof::mm_vp_skipped) / fr,
				dx(r2prof::mm_qlock_wait_ns) / ms, dx(r2prof::mm_offload_flushes) / fr);
			const double xf_ns = dx(r2prof::prog_xform_ns), fc_ns = dx(r2prof::prog_fconst_ns), env_ns = dx(r2prof::prog_env_ns);
			bp("[r2rsx-prof]   flip: frame limiter sleep %.2f ms/f (idle), present %.2f ms/f, ZCULL labels carried over flip %.1f/f (R2_ZCULL_FLIP_LAG) | "
				"program us/draw: analyse %.2f, load_program %.2f, env %.2f = xform consts %.2f (%.0f B/draw) + frag consts %.2f + rest %.2f\n",
				dx(r2prof::od_limit_ns) / ms, dx(r2prof::od_present_ns) / ms, dx(r2prof::zc_flip_carried) / fr,
				dx(r2prof::prog_analyse_ns) / 1000.0 / dr, dx(r2prof::prog_load_ns) / 1000.0 / dr, env_ns / 1000.0 / dr,
				xf_ns / 1000.0 / dr, dx(r2prof::prog_xform_bytes) / dr, fc_ns / 1000.0 / dr, std::max(0.0, env_ns - xf_ns - fc_ns) / 1000.0 / dr);

			// Round-5: texture-cache readbacks inside invalidations, program env "rest" split
			const double tfc = std::max(dx(r2prof::tc_flush_calls), 1.0), tfcc = std::max(dx(r2prof::tc_flush_commit_calls), 1.0);
			bp("[r2rsx-prof]   texture cache readbacks (flush_set): %.1f/f, %.2f ms/f (%.0f us each, %.1f sections each), of which render-target commits %.1f/f, "
				"%.2f ms/f (%.0f us each) | commit flushes skipped (R2_FBO_COMMIT_FLUSH=0 default) %.1f sections/f\n",
				dx(r2prof::tc_flush_calls) / fr, dx(r2prof::tc_flush_ns) / ms, dx(r2prof::tc_flush_ns) / 1000.0 / tfc, dx(r2prof::tc_flush_sections) / tfc,
				dx(r2prof::tc_flush_commit_calls) / fr, dx(r2prof::tc_flush_commit_ns) / ms, dx(r2prof::tc_flush_commit_ns) / 1000.0 / tfcc,
				dx(r2prof::tc_commit_skip) / fr);
			const double pe_known = dx(r2prof::pe_aa_ns) + dx(r2prof::pe_vtx_env_ns) + dx(r2prof::pe_inst_ns) + dx(r2prof::pe_frag_env_ns) +
				dx(r2prof::pe_tex_env_ns) + dx(r2prof::pe_bind_ns);
			bp("[r2rsx-prof]   program env rest us/draw: DLSS/FSR draw hook (r2aa_on_draw, incl. AA runs fired there) %.2f + vertex env %.2f + instancing %.2f + "
				"fragment env %.2f + texture env %.2f + bind_uniform %.2f + unaccounted %.2f\n",
				dx(r2prof::pe_aa_ns) / 1000.0 / dr, dx(r2prof::pe_vtx_env_ns) / 1000.0 / dr, dx(r2prof::pe_inst_ns) / 1000.0 / dr,
				dx(r2prof::pe_frag_env_ns) / 1000.0 / dr, dx(r2prof::pe_tex_env_ns) / 1000.0 / dr, dx(r2prof::pe_bind_ns) / 1000.0 / dr,
				std::max(0.0, env_ns - xf_ns - fc_ns - pe_known) / 1000.0 / dr);
			// Round-6: flip cmd split (self ms/f; limiter and present are listed on the flip: line), ZCULL reads by wait
			const double zrw = std::max(dx(r2prof::zc_read_wait), 1.0);
			bp("[r2rsx-prof]   flip cmd ms/f: %.2f = label drain/ZCULL poll %.2f + reset (ZCULL state) %.2f + frame end %.2f + emu flip %.2f + rest %.2f | "
				"ZCULL reads /f: no-wait %.1f, waited %.1f (%.2f ms/f, %.0f us each), by update %.1f, forced %.1f (R2_ZCULL_NOWAIT)\n",
				flip_all / ms, dx(r2prof::od_fl_drain_ns) / ms, dx(r2prof::od_fl_reset_ns) / ms, dx(r2prof::od_fl_end_ns) / ms,
				dx(r2prof::od_fl_emu_ns) / ms, dx(r2prof::od_flip_ns) / ms,
				dx(r2prof::zc_read_ready) / fr, dx(r2prof::zc_read_wait) / fr, dx(r2prof::zc_read_wait_ns) / ms,
				dx(r2prof::zc_read_wait_ns) / 1000.0 / zrw, dx(r2prof::zc_read_update) / fr, dx(r2prof::zc_read_forced) / fr);
			// Round-7: draw window split and lock-phase internals
			{
				const double drw = std::max(dr, 1.0);
				const double ph_sum_ns = (ph_setup + ph_prog + ph_vtx + ph_tex + ph_exec) * 1000.0;
				const double b_ns = dx(r2prof::dr_begin_ns), e_ns = dx(r2prof::dr_end_ns);
				const double win_ns = draw_us * 1000.0;
				bp("[r2rsx-prof]   draw window us/draw: begin %.2f (fb-bind %.2f + cond-render/other %.2f), end %.2f (phases %.2f + other %.2f), "
					"methods between begin/end %.2f, post-draw tail (outside window) %.2f | "
					"lock: inval superseded %.1f us/rebind, create+reprotect %.1f us/rebind, fresh reasons /f: new %.1f, unlocked %.1f, ctx change %.1f; "
					"superseded surfaces %.1f/f | invalidate ms/f: intersect %.2f, unprotect %.2f, fast discards %.1f/f\n",
					b_ns / 1000.0 / drw, ph_fb / drw, std::max(0.0, b_ns / 1000.0 - ph_fb) / drw,
					e_ns / 1000.0 / drw, ph_sum_ns / 1000.0 / drw, std::max(0.0, e_ns - ph_sum_ns) / 1000.0 / drw,
					std::max(0.0, win_ns - b_ns - e_ns) / 1000.0 / drw, dx(r2prof::dr_post_ns) / 1000.0 / drw,
					dx(r2prof::lk_inval_ns) / 1000.0 / fbb, dx(r2prof::lk_create_ns) / 1000.0 / fbb,
					dx(r2prof::lk_fresh_noexist) / fr, dx(r2prof::lk_fresh_unlocked) / fr, dx(r2prof::lk_fresh_ctx) / fr,
					dx(r2prof::lk_superseded) / fr, dx(r2prof::inval_isect_ns) / ms, dx(r2prof::inval_unprot_ns) / ms, dx(r2prof::inval_discards) / fr);
			}
			// Round-8: end-of-draw split, Vulkan recording share, uploads saved, cache misses, FIFO redundancy
			{
				const double us = 1000.0 * dr;
				const double vk_ns = dx(r2prof::vk_bind_ns) + dx(r2prof::vk_dyn_ns) + dx(r2prof::vk_draw_ns);
				const double meth = std::max(dx(r2prof::fifo_methods), 1.0);
				bp("[r2rsx-prof]   r8 us/draw: decode state %.2f, resolve programs %.2f (texture env load %.2f), write barriers %.2f, "
					"texture bind %.2f, on_write %.2f | Vulkan recording %.2f = pipeline+descriptors %.2f + dyn state/renderpass %.2f + draw %.2f | "
					"vertex: index %.2f (%.0f B/draw), layout %.2f, attribute data %.0f B/draw\n",
					dx(r2prof::dr_decode_ns) / us, dx(r2prof::dr_resolve_ns) / us, dx(r2prof::tex_load_ns) / us, dx(r2prof::dr_barrier_ns) / us,
					dx(r2prof::tex_bind_ns) / us, dx(r2prof::dr_onwrite_ns) / us, vk_ns / us, dx(r2prof::vk_bind_ns) / us,
					dx(r2prof::vk_dyn_ns) / us, dx(r2prof::vk_draw_ns) / us, dx(r2prof::vtx_idx_ns) / us, dx(r2prof::vtx_idx_bytes) / dr,
					dx(r2prof::vtx_layout_ns) / us, dx(r2prof::vtx_data_bytes) / dr);
				bp("[r2rsx-prof]   r8 per frame: pipeline slow lookups %.1f (RSX_PIPE_MEMO_MRU hits %.1f), in-memory misses %.2f, draws skipped (program not ready) %.2f | "
					"FIFO methods %.0f (same value %.0f%%), draws merged %.1f (R2_FIFO_MERGE) | xform memo hits %.1f (%.1f KB skipped) | "
					"index dedup hits %.1f (%.1f KB not re-copied, verify misses %.1f) | texture uploads %.1f (%.1f us each, async %.1f)\n",
					dx(r2prof::pipe_slow) / fr, dx(r2prof::pipe_memo2_hits) / fr, dx(r2prof::pipe_miss) / fr, dx(r2prof::pipe_not_ready) / fr,
					dx(r2prof::fifo_methods) / fr, dx(r2prof::fifo_same) * 100.0 / meth, dx(r2prof::fifo_merged) / fr,
					dx(r2prof::xc_memo_hits) / fr, dx(r2prof::xc_memo_bytes) / 1024.0 / fr,
					dx(r2prof::idx_dedup_hits) / fr, dx(r2prof::idx_dedup_bytes) / 1024.0 / fr, dx(r2prof::idx_dedup_stale) / fr,
					dx(r2prof::tex_up_count) / fr, dx(r2prof::tex_up_count) ? dx(r2prof::tex_up_ns) / 1000.0 / dx(r2prof::tex_up_count) : 0.0, dx(r2prof::tex_up_async) / fr);
				bp("[r2rsx-prof]   set0 (RSX_SET0_SHARE): allocations %.1f/f, shared-set reuses %.1f/f (with =2: would-be reuses, not taken), pool-epoch invalidations %.2f/f\n",
					dx(r2prof::set0_alloc) / fr, dx(r2prof::set0_adopt) / fr, dx(r2prof::set0_inval) / fr);
			}
			// RSX_FIFO_HOOKS_FAST: FIFO packets and the housekeeping split (each bucket includes nested od scopes)
			bp("[r2rsx-prof]   fifo: run_FIFO calls %.0f/f (%.1f methods each) | housekeeping runs %.1f/f (interrupt-forced %.1f/f, draw-light %.1f/f): "
				"context %.3f + local task %.3f + zcull %.3f + labels %.3f + host dma %.3f ms/f\n",
				dx(r2prof::fifo_packets) / fr, dx(r2prof::fifo_methods) / std::max(dx(r2prof::fifo_packets), 1.0),
				dx(r2prof::hk_runs) / fr, dx(r2prof::hk_irq) / fr, dx(r2prof::hk_light) / fr, dx(r2prof::hk_ctx_ns) / ms, dx(r2prof::hk_local_ns) / ms,
				dx(r2prof::hk_zcull_ns) / ms, dx(r2prof::hk_labels_ns) / ms, dx(r2prof::hk_dma_ns) / ms);
			// Round-9: usage census (R2_RSX_CENSUS=1): what to specialise for
			if (r2prof::census_on)
			{
				static std::vector<u32> s_prev(0x4000 * 2 + 32 + 8 + 256 + 5 + 2 + 3);
				std::vector<u32> cur(s_prev.size());
				auto& c = r2prof::census;
				usz k = 0;
				for (auto& v : c.meth) cur[k++] = v.load(std::memory_order_relaxed);
				for (auto& v : c.meth_same) cur[k++] = v.load(std::memory_order_relaxed);
				for (auto& v : c.color_fmt) cur[k++] = v.load(std::memory_order_relaxed);
				for (auto& v : c.depth_fmt) cur[k++] = v.load(std::memory_order_relaxed);
				for (auto& v : c.tex_fmt) cur[k++] = v.load(std::memory_order_relaxed);
				for (auto& v : c.mrt) cur[k++] = v.load(std::memory_order_relaxed);
				cur[k++] = c.cond_render.load(std::memory_order_relaxed);
				cur[k++] = c.draws.load(std::memory_order_relaxed);
				cur[k++] = c.delta_only.load(std::memory_order_relaxed);
				cur[k++] = c.delta_only_va.load(std::memory_order_relaxed);
				cur[k++] = c.vk_same.load(std::memory_order_relaxed);
				std::vector<double> d(cur.size());
				for (usz j = 0; j < cur.size(); ++j) d[j] = static_cast<double>(cur[j] - s_prev[j]);
				s_prev = std::move(cur);
				const double cdr = std::max(d[d.size() - 4], 1.0);

				std::vector<u32> order(0x4000);
				for (u32 j = 0; j < 0x4000; ++j) order[j] = j;
				std::partial_sort(order.begin(), order.begin() + 24, order.end(), [&](u32 a, u32 b) { return d[a] > d[b]; });
				std::string ms;
				for (u32 j = 0; j < 24 && d[order[j]] > 0; ++j)
				{
					char b[64];
					std::snprintf(b, sizeof(b), "%s%04x %.0f (%.0f%% same)", j ? ", " : "", order[j] * 4, d[order[j]] / fr, d[0x4000 + order[j]] * 100.0 / d[order[j]]);
					ms += b;
				}
				bp("[r2rsx-prof]   r9 census FIFO methods/f by register (byte offset): %s\n", ms.c_str());

				const auto list = [&](usz base, usz n, const char* fmt) {
					std::string s;
					for (usz j = 0; j < n; ++j)
					{
						if (d[base + j] <= 0) continue;
						char b[48];
						std::snprintf(b, sizeof(b), fmt, s.empty() ? "" : ", ", static_cast<unsigned>(j), d[base + j] * 100.0 / cdr);
						s += b;
					}
					return s.empty() ? std::string("-") : s;
				};
				const usz cb = 0x8000;
				bp("[r2rsx-prof]   r9 census per draw: colour fmt %s | depth fmt %s | MRT count %s | conditional-render draws %.1f%% | "
					"texture fmt (per 100 draws) %s\n",
					list(cb, 32, "%s%u %.0f%%").c_str(), list(cb + 32, 8, "%s%u %.0f%%").c_str(), list(cb + 40 + 256, 5, "%s%u %.0f%%").c_str(),
					d[cb + 40 + 256 + 5] * 100.0 / cdr, list(cb + 40, 256, "%s0x%02x %.0f").c_str());
				bp("[r2rsx-prof]   r9 census draw-call reduction: delta-only draws %.1f%% (only constants / vertex base / index address / "
					"invalidates / reports changed since the previous draw), %.1f%% also allowing vertex array offsets | "
					"Vulkan state equal to the previous draw %.1f%% (multi-draw-indirect upper bound)\n",
					d[d.size() - 3] * 100.0 / cdr, d[d.size() - 2] * 100.0 / cdr, d[d.size() - 1] * 100.0 / cdr);
				// RSX_FIFO_HOOKS_FAST census: method handler cost by register, methods that met each optional FIFO hook
				static std::vector<u64> s_prev_ns(0x4000);
				static u32 s_prev_hk[4];
				std::vector<double> dn(0x4000);
				double dn_all = 0;
				for (u32 j = 0; j < 0x4000; ++j)
				{
					const u64 v = c.meth_ns[j].load(std::memory_order_relaxed);
					dn[j] = static_cast<double>(v - s_prev_ns[j]);
					s_prev_ns[j] = v;
					dn_all += dn[j];
				}
				std::partial_sort(order.begin(), order.begin() + 16, order.end(), [&](u32 a, u32 b) { return dn[a] > dn[b]; });
				std::string hs;
				for (u32 j = 0; j < 16 && dn[order[j]] > 0; ++j)
				{
					char b[80];
					std::snprintf(b, sizeof(b), "%s%04x %.3f ms (%.0f ns each)", j ? ", " : "", order[j] * 4, dn[order[j]] / (1e6 * fr),
						d[order[j]] > 0 ? dn[order[j]] / d[order[j]] : 0.0);
					hs += b;
				}
				const u32 hk[4] = { c.hook_drawlog.load(std::memory_order_relaxed), c.hook_capture.load(std::memory_order_relaxed),
					c.hook_flat.load(std::memory_order_relaxed), c.hook_featcov.load(std::memory_order_relaxed) };
				double dh[4];
				for (int j = 0; j < 4; ++j) { dh[j] = static_cast<double>(hk[j] - s_prev_hk[j]); s_prev_hk[j] = hk[j]; }
				bp("[r2rsx-prof]   hooks census: method handlers %.2f ms/f (timed, incl. rdtsc), top by ms/f: %s | methods/f meeting FIFO hooks: "
					"draw log %.0f, frame capture %.0f, flattener %.0f, featcov on %.0f (RSX_FIFO_HOOKS_FAST skips all of them when none is active)\n",
					dn_all / (1e6 * fr), hs.empty() ? "-" : hs.c_str(), dh[0] / fr, dh[1] / fr, dh[2] / fr, dh[3] / fr);
			}
			// Round-7: the monitor's own cost (previous cycle's print included)
			const u64 mon_cpu1 = self_cpu_ns();
			bp("[r2rsx-prof]   monitor: cpu %.1f ms/5s (thread scans %llu, %.1f ms), last print %.1f ms wall\n",
				static_cast<double>(mon_cpu1 - mon_cpu0) / 1e6, static_cast<unsigned long long>(s_mon_scans - mon_scans0),
				static_cast<double>(s_mon_scan_ns - mon_scan0) / 1e6, static_cast<double>(s_mon_last_work_ns) / 1e6);
			mon_cpu0 = mon_cpu1; mon_scan0 = s_mon_scan_ns; mon_scans0 = s_mon_scans;
			std::fwrite(blk.data(), 1, blk.size(), stdout); // R2PORT round 7: one write per block
			std::fflush(stdout);
			blk.clear();
			s_mon_last_work_ns = r2prof::now_ns() - work_t0;

			p0 = p1; t0 = t1; th0 = std::move(th1); idle0 = idle1;
		}
	}
	// ---- end R2PORT perf ----

	void load_config()
	{
		// R2_RSX_CONFIG, else rpcs3_config.yml next to the exe or in its parent: an RPCS3 config.yml.
		std::vector<std::string> paths;
		if (const char* e = std::getenv("R2_RSX_CONFIG"); e && *e) paths.emplace_back(e);
		const std::string dir = exe_dir();
		paths.push_back(dir + "\\rpcs3_config.yml");
		paths.push_back(dir + "\\..\\rpcs3_config.yml");

		for (const auto& p : paths)
		{
			std::ifstream f(p, std::ios::binary);
			if (!f) continue;
			// Only the top-level Video: block. The other sections name enums whose
			// formatters live in parts of RPCS3 that are not vendored, and parsing
			// those never terminates (cfg::try_to_enum_value).
			std::string line, video;
			bool in_video = false;
			while (std::getline(f, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (!line.empty() && line[0] != ' ' && line[0] != '#')
					in_video = line.rfind("Video:", 0) == 0;
				if (in_video) video += line + '\n';
			}
			if (video.empty()) continue;
			if (g_cfg.from_string(video))
			{
				std::printf("[r2rsx] config: %s\n", p.c_str());
				break;
			}
			std::printf("[r2rsx] config: failed to parse %s\n", p.c_str());
		}

		g_cfg.video.renderer.set(video_renderer::vulkan);
		g_cfg.misc.use_native_interface.set(false);
		// R2PORT OSD (r2rsx_osd.cpp): the overlay manager may exist for the port's own layers; keep RPCS3's
		// own boot music and shader hints off
		g_cfg.misc.play_music_during_boot.set(false);
		g_cfg.misc.show_shader_compilation_hint.set(false);
		apply_shader_defaults();
		apply_launcher_overrides();
		apply_port_perf(); // R2PORT perf
		apply_gpu_offload(); // RSX_GPU_* (after the R2_* names, which they override)
	}

	void commit(u32 ea, u32 size)
	{
		for (u8* base : { vm_base, vm_sudo_base })
		{
			if (base) VirtualAlloc(base + ea, size, MEM_COMMIT, PAGE_READWRITE);
		}
	}

	// R2PORT [texcache]: an unclaimed fault on a page that is accessible again by now lost a race (another
	// thread's fault lifted the guard while this one was dispatched); the host handler just retries it.
	// Only the rest (still protected) are real unclaimed faults, logged by the host as "[vm] unclaimed".
	static bool av_page_open(uint32_t addr, int is_write)
	{
		if (!vm_base) return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(vm_base + addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
		return (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | (is_write ? 0 : (PAGE_READONLY | PAGE_EXECUTE_READ)))) != 0;
	}

	int access_violation_hook(uint32_t addr, int is_write)
	{
		// R3PORT: per-thread fault count and time (the PORT_HITCH_RSX_METH nv0039 split reads rsx::thread's)
		const u64 r3_tf0 = port_hitch_t0();
		struct r3_tf_scope { u64 t0; ~r3_tf_scope() { r2texdiag::tl_faults++; r2texdiag::tl_fault_ticks += port_hitch_t0() - t0; } } r3_tf{ r3_tf0 };
		if (!r2prof::enabled) [[likely]]
		{
			const int r0 = rsx::g_access_violation_handler && rsx::g_access_violation_handler(addr, is_write != 0);
			r2texdiag::on_fault(r0 != 0, !r0 && av_page_open(addr, is_write)); // R2PORT: [texcache]
			return r0;
		}

		// R2PORT perf: R2_RSX_PROF counts the faults and the time RPCS3 spends in them
		const u64 t0 = r2prof::now_ns();
		const int r = rsx::g_access_violation_handler && rsx::g_access_violation_handler(addr, is_write != 0);
		r2texdiag::on_fault(r != 0, !r && av_page_open(addr, is_write)); // R2PORT: [texcache]
		r2prof::add(r2prof::g.av_ns, r2prof::now_ns() - t0);
		r2prof::add(r2prof::g.av_calls, 1);
		if (r) r2prof::add(r2prof::g.av_handled, 1);
		return r;
	}
}

extern "C" void r2port_rsx_guest_call(u32 opd, u32 arg)
{
	{
		std::lock_guard lock(s_intr_mtx);
		s_intr_queue.emplace_back(opd, arg);
	}
	s_intr_cv.notify_one();
}

// R3PORT PC features: the F3 performance overlay (project/r3_pcfeat.cpp) turns the r2prof
// counters on while it is shown and samples them. R2_RSX_PROF keeps them on regardless.
extern "C" void r2rsx_prof_enable(int on)
{
	s_prof_overlay = on != 0;
	r2prof::enabled = s_prof_overlay || penv_bool("R2_RSX_PROF") == 1;
	r2prof::census_on = r2prof::enabled && penv_bool("R2_RSX_CENSUS") == 1; // R2PORT round 9
}

// Deltas since the previous call: RSX frames, draws per frame and average draw time (us,
// begin() .. end of draw on the RSX thread). Returns 0 when there was no frame in between.
extern "C" int r2rsx_prof_sample(double* frames, double* draws_per_frame, double* us_per_draw)
{
	static uint64_t lf = 0, ld = 0, ln = 0;
	const uint64_t f = r2prof::g.frames.load(std::memory_order_relaxed);
	const uint64_t d = r2prof::g.draws.load(std::memory_order_relaxed);
	const uint64_t n = r2prof::g.draw_ns.load(std::memory_order_relaxed);
	const uint64_t df = f - lf, dd = d - ld, dn = n - ln;
	lf = f; ld = d; ln = n;
	if (frames) *frames = static_cast<double>(df);
	if (draws_per_frame) *draws_per_frame = df ? static_cast<double>(dd) / df : 0.0;
	if (us_per_draw) *us_per_draw = dd ? static_cast<double>(dn) / dd / 1000.0 : 0.0;
	return df != 0;
}

// R3_ULTRAWIDE changed from the game's menus (project/r3_patches.cpp, r3_aspect_set_live): stretch the
// picture over the window, or pillarbox it again. RPCS3 reads this setting at every present.
extern "C" void r2rsx_set_stretch(int on)
{
	if (g_cfg.video.stretch_to_display_area.get() == (on != 0)) return;
	g_cfg.video.stretch_to_display_area.set(on != 0);
	std::printf("[r2rsx] R3_ULTRAWIDE: picture %s\n", on ? "stretched to the window" : "16:9 with side bars");
}

extern "C" int r2rsx_enabled(void)
{
	if (s_enabled < 0)
	{
		// RPCS3/Vulkan is the default renderer; R2_RSX=d3d12 the Direct3D 12 (experimental) draw engine (or 0 the old D3D12 backend),
		// R2_RSX=vulkan the native Vulkan renderer (ps3recomp's draw engine).
		const char* e = std::getenv("R2_RSX");
		s_enabled = !(e && (!_stricmp(e, "d3d12") || !_stricmp(e, "vulkan") || !_stricmp(e, "native") ||
		                    !std::strcmp(e, "0")));
	}
	return s_enabled;
}

extern "C" int rsx_null_backend_splash(int step, int steps, unsigned done, unsigned total);

extern "C" void r2rsx_set_window(void* hwnd, uint32_t, uint32_t)
{
	// Dark start-up screen until the renderer's on_init (shader preload) is done: nothing is presented before
	// that, and the unpainted window showed white (VKGSRender.cpp preload_shaders shows the progress on it).
	if (hwnd) rsx_null_backend_splash(-1, 0, 0, 0);
	{
		std::lock_guard lock(s_win_mtx);
		s_hwnd = static_cast<HWND>(hwnd);
	}
	s_win_cv.notify_all();
}

extern "C" int r2rsx_context_allocate(uint32_t dma_control_ea, uint32_t label_ea,
                                      uint32_t driver_info_ea, uint32_t device_ea,
                                      uint32_t local_size)
{
	if (!r2rsx_enabled()) return -1;
	if (s_up) return 0;

	{
		std::unique_lock lock(s_win_mtx);
		if (!s_win_cv.wait_for(lock, std::chrono::seconds(30), [] { return s_hwnd != nullptr; }))
		{
			std::printf("[r2rsx] no window after 30 s\n");
			return -1;
		}
	}

	const std::string root = exe_dir() + "\\..";
	std::error_code ec;
	std::filesystem::create_directories(root + "\\logs", ec);
	// Keep the previous run's RSX log (a fatal rsx::thread error is only in there)
	if (std::filesystem::file_size(root + "\\logs\\rpcs3_rsx.log", ec) > 0 && !ec)
	{
		std::filesystem::rename(root + "\\logs\\rpcs3_rsx.log", root + "\\logs\\rpcs3_rsx.prev.log", ec);
	}
	ec.clear();
	s_log = logs::make_file_listener(root + "\\logs\\rpcs3_rsx.log", 256ull << 20);
	{
		// RSX log verbosity: errors only unless R2_DIAG=1 (RPCS3's notice level) or R2_RSX_LOG=<level>.
		const char* d = std::getenv("R2_DIAG");
		logs::level lv = d && *d && *d != '0' ? logs::level::notice : logs::level::error;
		if (const char* e = std::getenv("R2_RSX_LOG"); e && *e)
		{
			if (!_stricmp(e, "fatal")) lv = logs::level::fatal;
			else if (!_stricmp(e, "error")) lv = logs::level::error;
			else if (!_stricmp(e, "warning")) lv = logs::level::warning;
			else if (!_stricmp(e, "notice")) lv = logs::level::notice;
			else if (!_stricmp(e, "trace")) lv = logs::level::trace;
		}
		logs::set_level(".*", lv);
	}

	// Shader cache next to the other one in shadercache\ (run.bat backs that folder up).
	r2port::g_rsx_cache_dir = root + "\\shadercache\\rpcs3\\";
	if (!std::filesystem::exists(r2port::g_rsx_cache_dir, ec) && std::filesystem::exists(root + "\\cache\\rpcs3\\shaders_cache", ec))
	{
		std::filesystem::create_directories(root + "\\shadercache", ec);
		std::filesystem::rename(root + "\\cache\\rpcs3", r2port::g_rsx_cache_dir.substr(0, r2port::g_rsx_cache_dir.size() - 1), ec);
	}
	std::filesystem::create_directories(r2port::g_rsx_cache_dir, ec);
	// PORT: pipelines the game links only in play (shipped without shader microcode; r2rsx_pipeline_seed.h)
	port_pipeline_seed::apply(root, r2port::g_rsx_cache_dir + "shaders_cache");

	load_config();

	g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return std::make_unique<r2_gs_frame>(); };
	// Headless: RPCS3 UI hooks must exist (empty std::function throws bad_function_call)
	g_emu_callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return nullptr; };
	g_emu_callbacks.get_localized_string = [](auto, const char* s) -> std::string { return s ? s : ""; };
	g_emu_callbacks.get_localized_u32string = [](auto, const char*) -> std::u32string { return {}; };
	g_emu_callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
	g_emu_callbacks.call_from_main_thread = [](std::function<void()> f, atomic_t<u32>* wake) { if (f) f(); if (wake) { *wake = 1; wake->notify_one(); } };

	// Local memory, the RsxReports block, driver info and the device page.
	commit(rsx::constants::local_mem_base, local_size);
	commit(label_ea & ~0xFFFu, 0x10000);
	commit(driver_info_ea & ~0xFFFu, 0x2000);
	commit(device_ea & ~0xFFFu, 0x1000);
	commit(dma_control_ea & ~0xFFFu, 0x1000);

	g_fxo->init();
	// PS3_DISPLAY_GAMMA=<g> (launcher "Brightness"): RPCS3's video-out gamma (out = in^g, the present pass
	// RPCS3 uses for cellVideoOutSetGamma); the native renderer reads it in cellAvconfExt_display_gamma.
	if (const char* e = penv("PS3_DISPLAY_GAMMA"))
	{
		const double g = std::atof(e);
		if (g >= 0.5 && g <= 2.0) g_fxo->get<rsx::avconf>().gamma = static_cast<f32>(g);
	}
	rsx::create_render_thread(nullptr);

	s_render = rsx::get_current_renderer();
	if (!s_render)
	{
		std::printf("[r2rsx] render thread was not created\n");
		return -1;
	}

	g_vm_access_violation_hook = access_violation_hook;

	auto* render = s_render;
	render->isHLE = true;
	render->local_mem_size = local_size;
	render->device_addr = device_ea;

	// sys_rsx_context_allocate
	auto& reports = *vm::_ptr<RsxReports>(label_ea);
	std::memset(&reports, 0, sizeof(RsxReports));
	for (auto& n : reports.notify) n.timestamp = -1;
	for (usz i = 0; i < std::size(reports.semaphore); i += 4)
	{
		reports.semaphore[i + 0] = 0x1337C0D3;
		reports.semaphore[i + 1] = 0x1337BABE;
		reports.semaphore[i + 2] = 0x1337BEEF;
		reports.semaphore[i + 3] = 0x1337F001;
	}
	for (auto& r : reports.report)
	{
		r.val = 0;
		r.timestamp = -1;
		r.pad = -1;
	}

	auto& driver = *vm::_ptr<RsxDriverInfo>(driver_info_ea);
	std::memset(&driver, 0, sizeof(RsxDriverInfo));
	driver.version_driver = 0x211;
	driver.version_gpu = 0x5c;
	driver.memory_size = local_size;
	driver.nvcore_frequency = 500000000;
	driver.memory_frequency = 650000000;
	driver.reportsNotifyOffset = 0x1000;
	driver.reportsOffset = 0;
	driver.reportsReportOffset = 0x1400;
	driver.hardware_channel = 1;

	auto& dma = *vm::_ptr<RsxDmaControl>(dma_control_ea);
	dma.get = 0;
	dma.put = 0;
	dma.ref = 0;

	rsx::context* ctx = render->get_context();
	ctx->fw.driver_info = driver_info_ea;
	ctx->fw.main_mem_size = 0x10000000;
	ctx->fw.label_addr = label_ea;
	ctx->display_buffers_count = 0;
	ctx->current_display_buffer = 0;

	render->init_context(ctx, dma_control_ea);

	// Vulkan device and backend resources
	while (!render->is_initialized)
	{
		Sleep(5);
	}

	std::thread(intr_thread_main).detach();
	// the 5 s [r2rsx-prof] log block only with R2_RSX_PROF=1: the F3 overlay (R2_OVERLAY=1 turns it on before
	// this point) needs the counters, not this thread, whose thread scans cost CPU on 4-thread PCs (R3 #33)
	if (penv_bool("R2_RSX_PROF") == 1) std::thread(prof_thread_main).detach(); // R2PORT perf
	rsx_host_sample::start_if_enabled(); // R3PORT perf r8: RSX_HOST_SAMPLE=1 (default off)
	r2texdiag::start_reporter(); // R2PORT: [texcache] 10 s line (RSX_TEXDIAG=0 off)

	render->start_device();
	s_up = true;

	std::printf("[r2rsx] RPCS3 RSX up (dma=0x%08X labels=0x%08X driver=0x%08X device=0x%08X local=0x%X)\n",
		dma_control_ea, label_ea, driver_info_ea, device_ea, local_size);
	cpu_replay_rsx_replay(dma_control_ea); // cpu-replay: one-time getenv; with PORT_RSX_REPLAY set it replays and exits
	return 0;
}

extern "C" void r2rsx_iomap(uint32_t ea, uint32_t io, uint32_t size)
{
	if (!s_up) return;
	auto& table = s_render->iomap_table;
	std::lock_guard lock(s_render->sys_rsx_mtx);

	ea >>= 20, io >>= 20, size >>= 20;
	for (u32 i = 0; i < size; i++)
	{
		const u32 prev_ea = table.ea[io + i];
		table.ea[io + i].release((ea + i) << 20);
		if (prev_ea + 1) table.io[prev_ea >> 20].release(-1);
		table.io[ea + i].release((io + i) << 20);
	}
}

extern "C" void r2rsx_iounmap(uint32_t io, uint32_t size)
{
	if (!s_up) return;
	auto& table = s_render->iomap_table;
	std::lock_guard lock(s_render->sys_rsx_mtx);

	for (const u32 end = (io >>= 20) + (size >>= 20); io < end;)
	{
		const u32 ea_entry = table.ea[io];
		table.ea[io++].release(-1);
		if (ea_entry + 1) table.io[ea_entry >> 20].release(-1);
	}
}

extern "C" void r2rsx_set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch,
                                         uint32_t width, uint32_t height)
{
	if (!s_up || id >= 8) return;
	rsx::context* ctx = s_render->get_context();
	auto& buf = ctx->display_buffers[id];
	buf.offset = offset;
	buf.pitch = pitch;
	buf.width = width;
	buf.height = height;
	if (ctx->display_buffers_count < id + 1) ctx->display_buffers_count = id + 1;
}

extern "C" void r2rsx_set_flip_mode(uint32_t mode)
{
	if (s_up) s_render->requested_vsync = (mode == 1);
}

extern "C" uint32_t r2rsx_get_flip_status(void)
{
	return s_up ? s_render->display_ctrl().get_flip_status() : 0;
}

extern "C" void r2rsx_set_flip_status(uint32_t status)
{
	if (s_up) s_render->display_ctrl().set_flip_status(status);
}

extern "C" uint64_t r2rsx_last_flip_time(void)
{
	return s_up ? s_render->display_ctrl().get_last_guest_flip_timestamp() : 0;
}

// Flip commands rsx::thread has executed so far (r3_hot occlusion fence); UINT64_MAX when the renderer is not up.
// Written by the RSX thread only; an aligned 64-bit load, read lag only delays the reader.
extern "C" uint64_t r2rsx_flips_executed(void)
{
	return s_up ? *reinterpret_cast<const volatile uint64_t*>(&s_render->m_r2_flip_seq) : UINT64_MAX;
}

extern "C" uint64_t r2rsx_vblank_count(void)
{
	return s_up ? s_render->display_ctrl().get_vblank_count() : 0;
}

// PORT: RSX_ZCULL_GUEST_WAIT. The game's blocking occlusion wait names the report EA it is blocked on, so rsx::thread
// submits that report's query at once (RSXZCULL.h guest_wait_state). Values are untouched; only the submit is earlier.
extern "C" void r2rsx_occ_guest_wait(uint32_t ea)
{
	if (!s_up || !ea || !rsx::reports::guest_wait_enabled()) return;
	auto& gw = rsx::reports::g_guest_wait;
	const u64 req = (static_cast<u64>(static_cast<u32>(get_system_time())) << 32) | ea;
	if (gw.req.exchange(req, std::memory_order_acq_rel)) gw.raced.fetch_add(1, std::memory_order_relaxed);
	gw.notices.fetch_add(1, std::memory_order_relaxed);
	// written between the guest's check and this store: drop the request (else the next frame's reuse of the slot matches)
	if (*static_cast<const volatile u64*>(vm::base(ea)))
	{
		u64 exp = req;
		gw.req.compare_exchange_strong(exp, 0);
	}
}

// Cumulative stats: notices, raced, claimed_before, claimed_during, written_unseen, submits,
// lat_before_us, lat_during_us, lat_unseen_us, lat_max_us (reset on read), claim_to_write_us. Returns 0 when off.
extern "C" int r2rsx_occ_guest_wait_stats(uint64_t out[11])
{
	if (!rsx::reports::guest_wait_enabled()) return 0;
	auto& gw = rsx::reports::g_guest_wait;
	out[0] = gw.notices.load(); out[1] = gw.raced.load(); out[2] = gw.claimed_before.load();
	out[3] = gw.claimed_during.load(); out[4] = gw.written_unseen.load(); out[5] = gw.submits.load();
	out[6] = gw.lat_before_us.load(); out[7] = gw.lat_during_us.load(); out[8] = gw.lat_unseen_us.load();
	out[9] = gw.lat_max_us.exchange(0); out[10] = gw.claim_to_write_us.load();
	return 1;
}

// PORT: RSX_OCC_SPEC_VISIBLE report fence. The guest answered an occlusion set without waiting for its last report `ea`;
// that report (and every report claimed before it) can still land after the game has zeroed and reused slots. Arm a
// fence slot that rsx::thread frees when it writes `ea` (RSXZCULL.cpp write()). Returns 1 when armed (or `ea` is already
// written), 0 when no slot is free or `ea` is already fenced (the caller must then wait as usual).
extern "C" int r2rsx_occ_spec_arm(uint32_t ea)
{
	if (!s_up || !ea) return 0;
	auto& gw = rsx::reports::g_guest_wait;
	for (auto& s : gw.spec_sink)
		if (s.load(std::memory_order_acquire) == ea) return 0;
	const u64 flips = *reinterpret_cast<const volatile uint64_t*>(&s_render->m_r2_flip_seq);
	for (int i = 0; i < rsx::reports::guest_wait_state::spec_slots; i++)
	{
		u32 e = 0;
		if (!gw.spec_sink[i].compare_exchange_strong(e, ea, std::memory_order_acq_rel)) continue;
		gw.spec_flip[i].store(flips, std::memory_order_relaxed);
		gw.spec_armed.fetch_add(1);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		// written between the guest's check and the arm: nothing left in flight for this report
		if (*static_cast<const volatile u64*>(vm::base(ea)))
		{
			u32 x = ea;
			if (gw.spec_sink[i].compare_exchange_strong(x, 0, std::memory_order_acq_rel))
				gw.spec_armed.fetch_sub(1, std::memory_order_acq_rel);
		}
		return 1;
	}
	return 0;
}

// Armed fence slots whose report is not written yet. A slot older than 120 executed flips is dropped (its report was
// discarded, e.g. by a ZCULL reset; a write that late cannot be pending). UINT32_MAX when the renderer is not up.
extern "C" uint32_t r2rsx_occ_spec_pending(void)
{
	if (!s_up) return UINT32_MAX;
	auto& gw = rsx::reports::g_guest_wait;
	if (gw.spec_armed.load(std::memory_order_acquire) <= 0) return 0;
	const u64 flips = *reinterpret_cast<const volatile uint64_t*>(&s_render->m_r2_flip_seq);
	u32 n = 0;
	for (int i = 0; i < rsx::reports::guest_wait_state::spec_slots; i++)
	{
		u32 ea = gw.spec_sink[i].load(std::memory_order_acquire);
		if (!ea) continue;
		if (flips - gw.spec_flip[i].load(std::memory_order_relaxed) > 120)
		{
			if (gw.spec_sink[i].compare_exchange_strong(ea, 0, std::memory_order_acq_rel))
				gw.spec_armed.fetch_sub(1, std::memory_order_acq_rel);
			continue;
		}
		n++;
	}
	return n;
}

// PORT: R3_OCC_NOWAIT catch-up fence (RSXZCULL.h guest_wait_state nw_*), ported from R2. The guest answered occlusion
// sets without consuming their reports; they can still land after the game zeroed and reused the slots. arm() asks
// rsx::thread to report FIFO-idle passes and returns the pass count now (UINT64_MAX when the renderer is not up).
// passed(seq0) = 1 once two idle passes followed (the second one saw every command the guest had put before arm()
// executed, so every report issued before it is claimed) and every claim up to that pass is written; 0 not yet; -1 not up.
extern "C" uint64_t r2rsx_occ_nw_arm(void)
{
	if (!s_up) return UINT64_MAX;
	auto& gw = rsx::reports::g_guest_wait;
	gw.nw_want.store(1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	return gw.nw_idle_seq.load(std::memory_order_acquire);
}

extern "C" int r2rsx_occ_nw_passed(uint64_t seq0)
{
	if (!s_up) return -1;
	auto& gw = rsx::reports::g_guest_wait;
	if (gw.nw_idle_seq.load(std::memory_order_acquire) < seq0 + 2) return 0;
	const u64 need = gw.nw_idle_claims.load(std::memory_order_relaxed);
	return gw.nw_done.load(std::memory_order_acquire) >= need ? 1 : 0;
}

extern "C" void r2rsx_occ_nw_disarm(void)
{
	if (s_up) rsx::reports::g_guest_wait.nw_want.store(0, std::memory_order_relaxed);
}

extern "C" uint32_t r2rsx_current_display_buffer(void)
{
	return s_up ? s_render->get_context()->current_display_buffer : 0;
}

extern "C" void r2rsx_set_flip_handler(uint32_t opd)   { if (s_up) s_render->flip_handler.set(opd); }
extern "C" void r2rsx_set_vblank_handler(uint32_t opd) { if (s_up) s_render->vblank_handler.set(opd); }
extern "C" void r2rsx_set_user_handler(uint32_t opd)   { if (s_up) s_render->user_handler.set(opd); }
extern "C" void r2rsx_set_queue_handler(uint32_t opd)  { if (s_up) s_render->queue_handler.set(opd); }

extern "C" void r2rsx_set_tile(uint32_t index, uint32_t location, uint32_t offset, uint32_t size,
                               uint32_t pitch, uint32_t comp, uint32_t base, uint32_t bank, int bound)
{
	if (!s_up || index >= std::size(s_render->tiles)) return;
	auto& tile = s_render->tiles[index];

	if (tile.bound && !bound)
	{
		s_render->notify_tile_unbound(index);
	}

	tile.location = location;
	tile.offset = offset;
	tile.size = size;
	tile.pitch = pitch;
	tile.comp = comp;
	tile.base = base;
	tile.bank = bank;
	tile.bound = bound != 0;
}

extern "C" void r2rsx_set_zcull(uint32_t index, uint32_t offset, uint32_t width, uint32_t height,
                                uint32_t cullStart, uint32_t zFormat, uint32_t aaFormat,
                                uint32_t zcullDir, uint32_t zcullFormat, uint32_t sFunc,
                                uint32_t sRef, uint32_t sMask, int bound)
{
	if (!s_up || index >= std::size(s_render->zculls)) return;
	auto& z = s_render->zculls[index];
	z.offset = offset;
	z.width = width;
	z.height = height;
	z.cullStart = cullStart;
	z.zFormat = zFormat;
	z.aaFormat = aaFormat;
	z.zcullDir = zcullDir;
	z.zcullFormat = zcullFormat;
	z.sFunc = sFunc;
	z.sRef = sRef;
	z.sMask = sMask;
	z.bound = bound != 0;
}

// R3PORT RSX_STALL_DIAG (ported from R2, report #153831: "GPU sit and spin timeout", GET fixed while PUT moved on).
// Called every ~250 ms from the port's watchdog thread. Returns 1 once per logged stall (may_report = 0: paused or out
// of reports) -- GET unchanged for stall_ms with PUT elsewhere -- with a one-line report in out: the puller's position
// and state (parked on a jump-to-self / in a semaphore acquire), the decoded command at it and the words around it,
// and the last semaphore and its value. Racy reads, never writes.
namespace
{
	void r2_stall_decode(u32 cmd, std::string& o)
	{
		if (cmd == 0) { o += "NOP"; return; }
		if ((cmd & 0xe0000003) == 0x20000000) { fmt::append(o, "JUMP(old) -> 0x%x", cmd & 0x1ffffffc); return; }
		if ((cmd & 3) == 1) { fmt::append(o, "JUMP -> 0x%x", cmd & ~3u); return; }
		if ((cmd & 3) == 2) { fmt::append(o, "CALL -> 0x%x", cmd & ~3u); return; }
		if ((cmd & 0xffff0003) == 0x00020000) { o += "RET"; return; }
		const u32 reg = cmd & 0xfffc;
		const u32 count = (cmd >> 18) & 0x7ff;
		std::string tmp;
		const auto [prefix, name] = rsx::get_method_name(reg >> 2, tmp);
		fmt::append(o, "%s%s 0x%04x x%u%s", prefix.empty() ? tmp : std::string(prefix), std::string(name), reg, count, (cmd & 0x40000000) ? " non-inc" : "");
	}

	bool r2_stall_read_io(u32 io, u32& v)
	{
		const u32 ea = s_render->iomap_table.get_addr(io);
		if (ea == umax) return false;
		v = vm::_ref<be_t<u32>>(ea);
		return true;
	}
}

extern "C" int r2rsx_stall_poll(unsigned stall_ms, int may_report, char* out, unsigned cap)
{
	static u32 s_get = umax;
	static u64 s_since = 0;
	static bool s_told = false;
	if (!s_up || !s_render->ctrl || !s_render->fifo_ctrl || !out || cap < 2) return 0;
	const u32 get = s_render->ctrl->get;
	const u32 put = s_render->ctrl->put;
	const u64 now = GetTickCount64();
	if (get != s_get || get == put)
	{
		if (s_told && get != s_get) std::fprintf(stderr, "[rsx-stall] GET moves again (0x%x -> 0x%x) after %.1f s\n", s_get, get, (now - s_since) / 1000.0);
		s_get = get; s_since = now; s_told = false;
		return 0;
	}
	if (s_told || now - s_since < stall_ms) return 0;
	if (!may_report)
	{
		// paused or out of reports: not logged, so no "moves again" either; a stall still there afterwards counts from now
		s_since = now;
		return 0;
	}
	s_told = true;

	auto* f = s_render->fifo_ctrl.get();
	const u32 pos = f->get_pos();
	std::string o;
	fmt::append(o, "[rsx-stall] GET 0x%x unchanged for %.1f s, PUT 0x%x, REF 0x%x | puller at 0x%x, last word 0x%08x (", get, (now - s_since) / 1000.0,
		put, static_cast<u32>(s_render->ctrl->ref), pos, f->last_cmd());
	r2_stall_decode(f->last_cmd(), o);
	o += ")";
	if (const u32 mw = f->r2_memwatch_addr())
	{
		u32 w = 0;
		const bool ok = r2_stall_read_io(mw, w);
		fmt::append(o, " | parked on a jump-to-self at 0x%x (word 0x%08x, now %s)", mw, f->r2_memwatch_cmp(),
			!ok ? "unmapped" : w == f->r2_memwatch_cmp() ? "unchanged: waits for the guest to patch it" : fmt::format("0x%08x", w).c_str());
	}
	if (const u64 since = port_rsx_stall::sem_since.load(std::memory_order_acquire))
	{
		const u32 a = port_rsx_stall::sem_addr.load(std::memory_order_relaxed);
		fmt::append(o, " | in a semaphore acquire at 0x%x for %llu ms: wants 0x%08x, holds 0x%08x", a,
			static_cast<unsigned long long>((get_system_time() - since) / 1000), port_rsx_stall::sem_arg.load(std::memory_order_relaxed),
			static_cast<u32>(vm::_ref<be_t<u32>>(a)));
	}
	else
	{
		o += " | not in a semaphore acquire";
	}
	if (const u32 la = s_render->last_sema_addr)
	{
		fmt::append(o, " | last acquire 0x%x at io 0x%x, holds 0x%08x", la, s_render->last_sema_cmd, static_cast<u32>(vm::_ref<be_t<u32>>(la)));
	}
	fmt::append(o, " | %llu acquire timeouts, cache 0x%x+%u, deferred labels %s", static_cast<unsigned long long>(port_rsx_stall::sem_tdr_n.load()),
		f->r2_cache_addr(), f->r2_cache_size(), s_render->r2_has_deferred_labels() ? "yes" : "none");
	// the words at GET and at the puller (they differ when GET is a published cache end)
	for (const u32 at : {get, pos})
	{
		fmt::append(o, " | words at %s 0x%x:", at == get ? "GET" : "puller", at);
		for (u32 i = 0; i < 8; i++)
		{
			u32 w = 0;
			if (!r2_stall_read_io(at + i * 4, w)) { o += " (unmapped)"; break; }
			fmt::append(o, " %08x", w);
		}
		u32 w0 = 0;
		if (r2_stall_read_io(at, w0)) { o += " = "; r2_stall_decode(w0, o); }
		if (pos == get) break;
	}
	std::snprintf(out, cap, "%s", o.c_str());
	return 1;
}

extern "C" void r2rsx_wait_get(uint32_t put, unsigned max_ms)
{
	if (!s_up || !s_render->ctrl) return;
	const u64 hitch_t0 = port_hitch_t0(); // R2PORT: [hitch] ring wrap: the guest thread waits for the RSX to drain
	const u64 deadline = GetTickCount64() + max_ms;
	while (s_render->ctrl->get != put && GetTickCount64() < deadline)
	{
		Sleep(0);
	}
	port_hitch_end_ppu_rsx(PH_RING, hitch_t0);
}

extern "C" void r2rsx_wait_get_outside(uint32_t lo, uint32_t hi, unsigned max_ms)
{
	if (!s_up || !s_render->ctrl) return;
	const u64 hitch_t0 = port_hitch_t0();
	const u64 deadline = GetTickCount64() + max_ms;
	for (;;)
	{
		const u32 get = s_render->ctrl->get;
		if (get < lo || get >= hi || GetTickCount64() >= deadline) break;
		Sleep(0);
	}
	port_hitch_end_ppu_rsx(PH_RING, hitch_t0);
}
