// Unit test for the pipeline-stutter helpers (third_party/rpcs3/rpcs3/Emu/RSX/port_r2pipe.h):
// [pipe] stall window + histogram, report timer, GPL part-cache byte keys, first-use preload order and boot split,
// the two-level compiler queue, and the R2_FOSSILIZE mode parser.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_r2pipe.h"

#include <cstdio>
#include <unordered_map>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static void test_buckets()
{
	CHECK(r2pipe::stall_bucket(0.0) == -1);
	CHECK(r2pipe::stall_bucket(1.999) == -1);
	CHECK(r2pipe::stall_bucket(2.0) == 0);
	CHECK(r2pipe::stall_bucket(3.9) == 0);
	CHECK(r2pipe::stall_bucket(4.0) == 1);
	CHECK(r2pipe::stall_bucket(8.0) == 2);
	CHECK(r2pipe::stall_bucket(16.0) == 3);
	CHECK(r2pipe::stall_bucket(32.9) == 3);
	CHECK(r2pipe::stall_bucket(33.0) == 4);
	CHECK(r2pipe::stall_bucket(500.0) == 4);
}

static void test_window()
{
	r2pipe::stall_window w;
	w.add_stall(0.5);
	w.add_stall(3.0);
	w.end_frame();              // frame 1: 3.5 ms
	w.end_frame();              // frame 2: none
	w.add_stall(40.0);
	w.add_stall(-1.0);          // clock glitch clamps to 0
	w.end_frame();              // frame 3: 40 ms
	CHECK(w.frames == 3);
	CHECK(w.stalls == 4);
	CHECK(w.total_ms == 43.5);
	CHECK(w.worst_frame_ms == 40.0);
	CHECK(w.worst_stall_ms == 40.0);
	CHECK(w.frames_with_stall == 2);
	CHECK(w.hist[0] == 1 && w.hist[1] == 0 && w.hist[2] == 0 && w.hist[3] == 0 && w.hist[4] == 1);
	CHECK(w.per_frame_ms() == 14.5);

	w.add_stall(5.0);           // unfinished frame carries into the next window
	w.reset();
	CHECK(w.frames == 0 && w.stalls == 0 && w.total_ms == 0.0 && w.hist[1] == 0);
	w.end_frame();
	CHECK(w.worst_frame_ms == 5.0 && w.frames_with_stall == 1);
	CHECK(r2pipe::stall_window{}.per_frame_ms() == 0.0);
}

static void test_timer()
{
	r2pipe::report_timer t;
	CHECK(!t.due(1000));                          // arms
	CHECK(!t.due(1000 + 4'999'999'999ull));
	CHECK(t.due(1000 + 5'000'000'000ull));
	CHECK(!t.due(1000 + 9'000'000'000ull));
	CHECK(t.due(1000 + 10'000'000'001ull));
}

static void test_keys()
{
	r2pipe::byte_key a, b;
	a.add(uint32_t{1}).add(uint64_t{0x1234});
	b.add(uint32_t{1}).add(uint64_t{0x1234});
	CHECK(a == b && a.hash() == b.hash());
	b.add(uint8_t{0});
	CHECK(!(a == b));

	// length-prefixed variable parts: {AB}{C} differs from {A}{BC}
	r2pipe::byte_key c, d;
	c.add_bytes("AB", 2).add_bytes("C", 1);
	d.add_bytes("A", 1).add_bytes("BC", 2);
	CHECK(!(c == d));
	CHECK(c.hash() != d.hash());

	// map lookups compare full bytes, not only the hash
	std::unordered_map<r2pipe::byte_key, int, r2pipe::byte_key_hash> m;
	m[a] = 7;
	r2pipe::byte_key a2;
	a2.add(uint32_t{1}).add(uint64_t{0x1234});
	CHECK(m.count(a2) == 1 && m[a2] == 7);
	CHECK(m.count(c) == 0);

	// FNV-1a reference values
	CHECK(r2pipe::fnv1a64("", 0) == 0xcbf29ce484222325ull);
	CHECK(r2pipe::fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
}

static void test_preload_order()
{
	const std::vector<int64_t> mt = { 50, 10, 30, 10, 20 };
	const auto o = r2pipe::first_use_order(mt);
	CHECK(o.size() == 5);
	CHECK(o[0] == 1 && o[1] == 3 && o[2] == 4 && o[3] == 2 && o[4] == 0); // stable on ties
	CHECK(r2pipe::first_use_order({}).empty());

	CHECK(r2pipe::boot_count(2035, -1) == 2035);
	CHECK(r2pipe::boot_count(2035, 0) == 0);
	CHECK(r2pipe::boot_count(2035, 384) == 384);
	CHECK(r2pipe::boot_count(100, 384) == 100);
}

static void test_queue()
{
	r2pipe::two_level_queue<int> q;
	int v = 0;
	CHECK(!q.pop(v) && q.empty());
	q.push(1, true);
	q.push(2, false);
	q.push(3, true);
	q.push(4, false);
	CHECK(q.size() == 4);
	CHECK(q.pop(v) && v == 2);
	CHECK(q.pop(v) && v == 4);
	q.push(5, false);
	CHECK(q.pop(v) && v == 5);
	CHECK(q.pop(v) && v == 1);
	CHECK(q.pop(v) && v == 3);
	CHECK(!q.pop(v));
	q.push(9, true);
	q.clear();
	CHECK(q.empty());
}

static void test_foz_mode()
{
	using m = r2pipe::foz_mode;
	CHECK(r2pipe::parse_foz_mode(nullptr) == m::auto_replay);
	CHECK(r2pipe::parse_foz_mode("") == m::auto_replay);
	CHECK(r2pipe::parse_foz_mode("auto") == m::auto_replay);
	CHECK(r2pipe::parse_foz_mode("0") == m::off);
	CHECK(r2pipe::parse_foz_mode("OFF") == m::off);
	CHECK(r2pipe::parse_foz_mode("record") == m::record);
	CHECK(r2pipe::parse_foz_mode("Record") == m::record);
	CHECK(r2pipe::parse_foz_mode("replay") == m::replay);
	CHECK(r2pipe::parse_foz_mode("1") == m::replay);
}

static void test_env()
{
	_putenv("R2PIPE_TEST_INT=");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == 42);
	_putenv("R2PIPE_TEST_INT=-1");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == -1);
	_putenv("R2PIPE_TEST_INT=300");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == 300);
	_putenv("R2PIPE_TEST_INT=abc");
	CHECK(r2pipe::env_int("R2PIPE_TEST_INT", 42) == 42);
}

// Issue #10: MSAA filter, AMD thread default, breadcrumb round trip, crash report key
static void test_preload_safety()
{
	CHECK(r2pipe::preload_keep_samples(1, false, false));
	CHECK(!r2pipe::preload_keep_samples(2, false, false));
	CHECK(!r2pipe::preload_keep_samples(4, false, false));
	CHECK(r2pipe::preload_keep_samples(2, true, false));
	CHECK(r2pipe::preload_keep_samples(4, false, true));

	// depth format in the render-pass key (bits 8-15): D24S8 = 129, D32S8 = 130
	const uint64_t rp_d24 = (129ull << 8) | (1ull << 16) | 37, rp_d32 = (130ull << 8) | (1ull << 16) | 37;
	uint64_t k = rp_d24;
	CHECK(r2pipe::preload_fix_depth(k, true, true) == 0 && k == rp_d24);
	CHECK(r2pipe::preload_fix_depth(k, false, true) == 1 && k == rp_d32);
	k = rp_d24;
	CHECK(r2pipe::preload_fix_depth(k, false, false) == -1);
	k = rp_d32;
	CHECK(r2pipe::preload_fix_depth(k, true, true) == 0 && k == rp_d32);
	CHECK(r2pipe::preload_fix_depth(k, true, false) == -1);
	k = (124ull << 8) | 5;
	CHECK(r2pipe::preload_fix_depth(k, false, false) == 0);
	k = 37;
	CHECK(r2pipe::preload_fix_depth(k, false, false) == 0);
	k = 1ull << 63;
	CHECK(r2pipe::preload_fix_depth(k, false, false) == 0);
	k = 200ull << 8;
	CHECK(r2pipe::preload_fix_depth(k, true, true) == -1);

	_putenv("PORT_PRELOAD_THREADS=");
	_putenv("R2_PRELOAD_THREADS=");
	CHECK(r2pipe::preload_bg_threads(16, 0x1002) == 1);
	CHECK(r2pipe::preload_bg_threads(16, 0x10DE) == 4);
	CHECK(r2pipe::preload_bg_threads(8, 0x10DE) == 2);
	CHECK(r2pipe::preload_bg_threads(2, 0x8086) == 1);
	_putenv("R2_PRELOAD_THREADS=3");
	CHECK(r2pipe::preload_bg_threads(16, 0x1002) == 3);
	_putenv("PORT_PRELOAD_THREADS=6");
	CHECK(r2pipe::preload_bg_threads(16, 0x1002) == 6);
	_putenv("PORT_PRELOAD_THREADS=99");
	CHECK(r2pipe::preload_bg_threads(16, 0x10DE) == 16);
	_putenv("PORT_PRELOAD_THREADS=");
	_putenv("R2_PRELOAD_THREADS=");

	_putenv("PORT_PRELOAD_BOOT_THREADS=");
	_putenv("R2_PRELOAD_BOOT_THREADS=");
	_putenv("PORT_PRELOAD_BOOT_AMD=");
	CHECK(r2pipe::preload_boot_workers(16, 0x10DE, false) == 15);
	CHECK(r2pipe::preload_boot_workers(16, 0x1002, false) == 2);
	CHECK(r2pipe::preload_boot_workers(16, 0x10DE, true) == 2);
	CHECK(r2pipe::preload_boot_workers(8, 0x10DE, false) == 2);
	CHECK(r2pipe::preload_boot_workers(1, 0x1002, false) == 1);
	CHECK(r2pipe::preload_boot_workers(0, 0x10DE, false) == 1);
	_putenv("PORT_PRELOAD_BOOT_AMD=0");
	CHECK(r2pipe::preload_boot_workers(16, 0x1002, false) == 15);
	_putenv("R2_PRELOAD_BOOT_THREADS=6");
	CHECK(r2pipe::preload_boot_workers(16, 0x1002, false) == 6);
	_putenv("PORT_PRELOAD_BOOT_THREADS=99");
	CHECK(r2pipe::preload_boot_workers(16, 0x1002, false) == 64);
	_putenv("PORT_PRELOAD_BOOT_THREADS=");
	_putenv("R2_PRELOAD_BOOT_THREADS=");
	_putenv("PORT_PRELOAD_BOOT_AMD=");

	_putenv("PORT_PRELOAD_FAULT_RELINK=");
	CHECK(r2pipe::preload_fault_relink(true));
	CHECK(!r2pipe::preload_fault_relink(false));
	_putenv("PORT_PRELOAD_FAULT_RELINK=0");
	CHECK(!r2pipe::preload_fault_relink(true));
	_putenv("PORT_PRELOAD_FAULT_RELINK=");

	CHECK(r2pipe::inflight_is_stage("rt:gfx_s2"));
	CHECK(!r2pipe::inflight_is_stage("AAAA+BBBB.bin"));
	CHECK(!r2pipe::inflight_is_stage("rt"));

	std::string file(r2pipe::inflight_slots * r2pipe::inflight_slot_bytes, 'x');
	for (size_t i = 0; i < r2pipe::inflight_slots; ++i)
		r2pipe::inflight_format(&file[i * r2pipe::inflight_slot_bytes], 0, nullptr);
	CHECK(r2pipe::inflight_parse(file).empty());
	r2pipe::inflight_format(&file[0], 1234, "AAAA+BBBB+CCCC+DDDD.bin");
	r2pipe::inflight_format(&file[3 * r2pipe::inflight_slot_bytes], 77, "EEEE.bin");
	r2pipe::inflight_format(&file[0], 1234, "A.bin"); // shorter key over a longer one
	const auto parsed = r2pipe::inflight_parse(file);
	CHECK(parsed.size() == 2);
	CHECK(parsed.size() == 2 && parsed[0].tid == 1234 && parsed[0].key == "A.bin");
	CHECK(parsed.size() == 2 && parsed[1].tid == 77 && parsed[1].key == "EEEE.bin");
	r2pipe::inflight_format(&file[0], 0, nullptr);
	CHECK(r2pipe::inflight_parse(file).size() == 1);
	const std::string longkey(300, 'k');
	r2pipe::inflight_format(&file[0], 5, longkey.c_str()); // truncated, never runs into the next slot
	CHECK(file[r2pipe::inflight_slot_bytes - 1] == '\n');
	CHECK(r2pipe::inflight_parse(file).size() == 2);

	CHECK(r2pipe::crash_preload_key("x\npreload faulting: K1+K2.bin\nthread 5\n") == "K1+K2.bin");
	CHECK(r2pipe::crash_preload_key("preload faulting: K3.bin\r\n") == "K3.bin");
	CHECK(r2pipe::crash_preload_key("no preload here\n").empty());
	CHECK(r2pipe::crash_preload_key("note: preload faulting: X\n").empty());
}

static void test_pcache_file()
{
	uint8_t uuid[16];
	for (int i = 0; i < 16; ++i) uuid[i] = static_cast<uint8_t>(0xA0 + i);
	std::vector<uint8_t> blob(4096);
	for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i * 7);
	const uint32_t hdr[4] = { 32, 1, 0x10DE, 0x2684 };
	std::memcpy(blob.data(), hdr, sizeof(hdr));
	std::memcpy(blob.data() + 16, uuid, 16);
	const std::string st = "vendor=10de device=2684 driver=1";
	size_t n = 99;

	const auto file = r2pipe::pcache_wrap(blob.data(), blob.size());
	CHECK(file.size() == blob.size() + r2pipe::pcache_trailer_bytes);
	CHECK(r2pipe::pcache_check(file.data(), file.size(), n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::use);
	CHECK(n == blob.size() && std::memcmp(file.data(), blob.data(), n) == 0);
	CHECK(r2pipe::pcache_check(file.data(), file.size(), n, 0x10DE, 0x2684, uuid, "", st) == r2pipe::pcache_verdict::use);
	// legacy file (no trailer): header check only
	CHECK(r2pipe::pcache_check(blob.data(), blob.size(), n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::use_legacy);
	CHECK(n == blob.size());

	auto bad = file;
	bad[1000] ^= 0x01; // body bit flip
	CHECK(r2pipe::pcache_check(bad.data(), bad.size(), n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::corrupt);
	CHECK(n == 0);
	bad = file;
	bad[bad.size() - 1] ^= 0x80; // checksum flip
	CHECK(r2pipe::pcache_check(bad.data(), bad.size(), n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::corrupt);
	bad = file;
	bad.erase(bad.begin() + 100); // short write in the middle: size field no longer matches
	CHECK(r2pipe::pcache_check(bad.data(), bad.size(), n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::corrupt);
	// truncated (trailer cut off): legacy path, header still parses -> only the driver's own check; a header-only
	// truncation is refused
	CHECK(r2pipe::pcache_check(file.data(), 31, n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::other_gpu);
	CHECK(r2pipe::pcache_check(file.data(), 0, n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::empty);
	CHECK(r2pipe::pcache_check(nullptr, 0, n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::empty);
	CHECK(r2pipe::pcache_check(file.data(), r2pipe::pcache_max_bytes, n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::too_large);

	// mismatches
	CHECK(r2pipe::pcache_check(file.data(), file.size(), n, 0x1002, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::other_gpu);
	CHECK(r2pipe::pcache_check(file.data(), file.size(), n, 0x10DE, 0x2204, uuid, st, st) == r2pipe::pcache_verdict::other_gpu);
	uint8_t uuid2[16];
	std::memcpy(uuid2, uuid, 16);
	uuid2[15] ^= 1;
	CHECK(r2pipe::pcache_check(file.data(), file.size(), n, 0x10DE, 0x2684, uuid2, st, st) == r2pipe::pcache_verdict::other_gpu);
	CHECK(r2pipe::pcache_check(file.data(), file.size(), n, 0x10DE, 0x2684, uuid, st, "vendor=10de device=2684 driver=2") ==
		r2pipe::pcache_verdict::gpu_changed);
	CHECK(n == 0);
	auto v2 = blob;
	v2[4] = 2; // header version
	const auto f2 = r2pipe::pcache_wrap(v2.data(), v2.size());
	CHECK(r2pipe::pcache_check(f2.data(), f2.size(), n, 0x10DE, 0x2684, uuid, st, st) == r2pipe::pcache_verdict::other_gpu);
	CHECK(std::strcmp(r2pipe::pcache_verdict_text(r2pipe::pcache_verdict::corrupt), "corrupt (checksum)") == 0);

	CHECK(r2pipe::preload_boot_default(true) == -1);
	CHECK(r2pipe::preload_boot_default(false) == 0);
}

// Issue #10: preload off = no preload at all unless R2_PRELOAD_BOOT is set; AMD serialisation; Fossilize entry names
static void test_preload_off_and_serial()
{
	CHECK(r2pipe::preload_enabled(true, false));
	CHECK(!r2pipe::preload_enabled(false, false));
	CHECK(r2pipe::preload_enabled(false, true));
	CHECK(r2pipe::boot_count(5000, r2pipe::preload_boot_default(false)) == 0);

	_putenv("PORT_PIPE_SERIAL=");
	CHECK(r2pipe::pipe_serialize(0x1002));
	CHECK(!r2pipe::pipe_serialize(0x10DE));
	CHECK(!r2pipe::pipe_serialize(0x8086));
	_putenv("PORT_PIPE_SERIAL=0");
	CHECK(!r2pipe::pipe_serialize(0x1002));
	_putenv("PORT_PIPE_SERIAL=1");
	CHECK(r2pipe::pipe_serialize(0x10DE));
	_putenv("PORT_PIPE_SERIAL=");

	CHECK(r2pipe::foz_entry_name(0x0123456789ABCDEFull) == "foz_0123456789abcdef");
	CHECK(r2pipe::foz_entry_name(1) == "foz_0000000000000001");
	// A replay entry round-trips through the breadcrumb and the crash report like a cache file name
	std::string slot(r2pipe::inflight_slot_bytes, 0);
	r2pipe::inflight_format(&slot[0], 9, r2pipe::foz_entry_name(42).c_str());
	const auto parsed = r2pipe::inflight_parse(slot);
	CHECK(parsed.size() == 1 && parsed[0].key == "foz_000000000000002a");
	CHECK(r2pipe::crash_preload_key("preload faulting: " + r2pipe::foz_entry_name(42) + "\n") == "foz_000000000000002a");
}

int main()
{
	test_buckets();
	test_window();
	test_timer();
	test_keys();
	test_preload_order();
	test_queue();
	test_foz_mode();
	test_env();
	test_preload_safety();
	test_pcache_file();
	test_preload_off_and_serial();
	std::printf("%s\n", g_fail ? "test_r2pipe: FAIL" : "test_r2pipe: PASS");
	return g_fail;
}
