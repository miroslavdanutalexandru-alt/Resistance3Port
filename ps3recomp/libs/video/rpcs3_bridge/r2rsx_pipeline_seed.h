#pragma once
// PORT: pipeline seed (tools\pipeline_seed.bin, written by scripts\export_pipeline_seed.ps1).
//
// RPCS3's shader cache has two parts: raw\<hash>.vp / .fp (the RSX shader microcode, game data) and
// pipelines\vulkan\<ver>\<vp>+<fp>+<state>+<state>.bin (which two shaders + the render state, no microcode).
// shaderscan builds raw\ (and its own pipeline guesses) from the player's game files; the pipelines the game only
// creates in play were cache misses the first time (async mode skips those draws). The seed ships just those pipeline
// records, so the start-up preload builds them before play. A record whose raw shaders are not on this PC (no
// shaderscan run yet) is skipped by the preload. Each start copies the records that are missing into the cache.
// PORT_PIPELINE_SEED=0 turns it off.
//
// File: "PORTPSD1", u32 record size, u32 count, char subdir[32] (e.g. "pipelines/vulkan/v1.95"),
// then count x { char name[80]; u8 data[record size]; } (names and subdir zero-padded, little-endian).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace port_pipeline_seed
{
	inline bool valid_name(const char* s)
	{
		// <hex>+<hex>+<hex>+<hex>.bin, each 1-16 upper-case hex digits
		int groups = 0, digits = 0;
		for (;; ++s)
		{
			const char c = *s;
			if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
			{
				if (++digits > 16) return false;
				continue;
			}
			if (!digits) return false;
			++groups;
			digits = 0;
			if (c == '+' && groups < 4) continue;
			return groups == 4 && std::strcmp(s, ".bin") == 0;
		}
	}

	inline bool valid_subdir(const char* s)
	{
		// pipelines/<lower-case class>/v<digits and dots>
		if (std::strncmp(s, "pipelines/", 10) != 0) return false;
		s += 10;
		const char* cls = s;
		while (*s >= 'a' && *s <= 'z') ++s;
		if (s == cls || *s++ != '/' || *s++ != 'v') return false;
		const char* ver = s;
		while ((*s >= '0' && *s <= '9') || *s == '.') ++s;
		return s != ver && *s == 0;
	}

	struct result
	{
		uint32_t total = 0, imported = 0, failed = 0;
		std::string error; // set when the file is there but unusable
	};

	// Copies the seed's missing records into <cache_dir>\<subdir>\. cache_dir = ...\shaders_cache
	inline result import(const std::filesystem::path& seed_file, const std::filesystem::path& cache_dir)
	{
		result r;
		FILE* f = nullptr;
		if (_wfopen_s(&f, seed_file.c_str(), L"rb") || !f) return r;
		struct header
		{
			char magic[8];
			uint32_t record_size, count;
			char subdir[32];
		} h{};
		if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.magic, "PORTPSD1", 8) != 0 || !h.record_size ||
			h.record_size > 4096 || h.count > 1000000 || h.subdir[31] || !valid_subdir(h.subdir))
		{
			std::fclose(f);
			r.error = "not a pipeline seed file";
			return r;
		}
		r.total = h.count;
		const std::filesystem::path dir = cache_dir / std::filesystem::path(h.subdir).make_preferred();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		std::vector<unsigned char> data(h.record_size);
		for (uint32_t i = 0; i < h.count; ++i)
		{
			char name[81]{};
			if (std::fread(name, 80, 1, f) != 1 || std::fread(data.data(), data.size(), 1, f) != 1)
			{
				r.error = "file is truncated";
				break;
			}
			if (!valid_name(name))
			{
				++r.failed;
				continue;
			}
			const std::filesystem::path out = dir / name;
			if (std::filesystem::exists(out, ec)) continue;
			FILE* o = nullptr;
			if (_wfopen_s(&o, out.c_str(), L"wb") || !o)
			{
				++r.failed;
				continue;
			}
			const bool ok = std::fwrite(data.data(), data.size(), 1, o) == 1;
			if (std::fclose(o) != 0 || !ok)
			{
				std::filesystem::remove(out, ec); // a short file would be deleted by the preload anyway
				++r.failed;
				continue;
			}
			++r.imported;
		}
		std::fclose(f);
		return r;
	}

	// root = install folder. Logs only when something was copied or went wrong.
	inline void apply(const std::string& root, const std::string& cache_dir)
	{
		const char* e = std::getenv("PORT_PIPELINE_SEED");
		if (e && (*e == '0' || !_stricmp(e, "off") || !_stricmp(e, "false") || !_stricmp(e, "no"))) return;
		const std::filesystem::path seed = std::filesystem::path(root) / "tools" / "pipeline_seed.bin";
		const result r = import(seed, cache_dir);
		if (!r.error.empty())
			std::printf("[r2rsx] pipeline seed: %s: %s (%u of %u records copied)\n", seed.string().c_str(), r.error.c_str(), r.imported, r.total);
		else if (r.imported || r.failed)
			std::printf("[r2rsx] pipeline seed: %u of %u pipelines added to the shader cache (%u failed; tools\\pipeline_seed.bin, PORT_PIPELINE_SEED=0 turns it off)\n",
				r.imported, r.total, r.failed);
	}
}
