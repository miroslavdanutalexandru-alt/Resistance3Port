// Offline test harness for the port's CACAO ambient occlusion (VKR2Cacao.h): compiles the exact GLSL the renderer
// uses with glslang, runs prepare / generate / blur / apply on a dumped depth buffer through vulkan-1.dll and writes
// the AO (R8) plus GPU timings. Build: tools\ao_harness\build.bat. No game, no RPCS3 state.
//
//   ao_harness <depth.f32> <w> <h> <linear 0|1> <near> <far> <vfov_deg> <aspect> <quality 0|1|2|all> <out_prefix> [runs]
//
// depth.f32: w*h float32 depth-buffer values (0..1, as the shaders read them). Output: <out_prefix>_q<N>.r8 (w*h bytes).
#define VK_NO_PROTOTYPES
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include "VKR2Cacao.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define VKF(x) static PFN_##x x
VKF(vkGetInstanceProcAddr); VKF(vkCreateInstance); VKF(vkEnumeratePhysicalDevices); VKF(vkGetPhysicalDeviceProperties);
VKF(vkGetPhysicalDeviceQueueFamilyProperties); VKF(vkGetPhysicalDeviceMemoryProperties); VKF(vkGetPhysicalDeviceFormatProperties);
VKF(vkCreateDevice); VKF(vkGetDeviceProcAddr); VKF(vkGetDeviceQueue); VKF(vkCreateCommandPool); VKF(vkAllocateCommandBuffers);
VKF(vkBeginCommandBuffer); VKF(vkEndCommandBuffer); VKF(vkQueueSubmit); VKF(vkQueueWaitIdle); VKF(vkCreateBuffer);
VKF(vkGetBufferMemoryRequirements); VKF(vkAllocateMemory); VKF(vkBindBufferMemory); VKF(vkMapMemory); VKF(vkCreateImage);
VKF(vkGetImageMemoryRequirements); VKF(vkBindImageMemory); VKF(vkCreateImageView); VKF(vkCreateSampler); VKF(vkCreateShaderModule);
VKF(vkCreateDescriptorSetLayout); VKF(vkCreatePipelineLayout); VKF(vkCreateComputePipelines); VKF(vkCreateDescriptorPool);
VKF(vkAllocateDescriptorSets); VKF(vkUpdateDescriptorSets); VKF(vkCmdBindPipeline); VKF(vkCmdBindDescriptorSets); VKF(vkCmdPushConstants);
VKF(vkCmdDispatch); VKF(vkCmdPipelineBarrier); VKF(vkCmdCopyBufferToImage); VKF(vkCmdCopyImageToBuffer); VKF(vkCreateQueryPool);
VKF(vkCmdResetQueryPool); VKF(vkCmdWriteTimestamp); VKF(vkGetQueryPoolResults); VKF(vkResetCommandBuffer);

static void die(const char* m) { std::fprintf(stderr, "ao_harness: %s\n", m); std::exit(1); }
#define CK(x) do { if ((x) != VK_SUCCESS) die(#x); } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties memp;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags f)
{
	for (uint32_t i = 0; i < memp.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (memp.memoryTypes[i].propertyFlags & f) == f) return i;
	die("no memory type");
	return 0;
}

struct buf { VkBuffer b; VkDeviceMemory m; void* p; };
static buf make_buf(VkDeviceSize n)
{
	buf r{};
	VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = n;
	ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CK(vkCreateBuffer(dev, &ci, nullptr, &r.b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, r.b, &mr);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CK(vkAllocateMemory(dev, &ai, nullptr, &r.m));
	CK(vkBindBufferMemory(dev, r.b, r.m, 0));
	CK(vkMapMemory(dev, r.m, 0, n, 0, &r.p));
	return r;
}

struct img { VkImage i; VkDeviceMemory m; VkImageView v; uint32_t w, h; };
static img make_img(VkFormat fmt, uint32_t w, uint32_t h)
{
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(pd, fmt, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) std::fprintf(stderr, "warning: format %d has no storage support\n", fmt);
	img r{};
	r.w = w;
	r.h = h;
	VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ci.imageType = VK_IMAGE_TYPE_2D;
	ci.format = fmt;
	ci.extent = { w, h, 1 };
	ci.mipLevels = ci.arrayLayers = 1;
	ci.samples = VK_SAMPLE_COUNT_1_BIT;
	ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	CK(vkCreateImage(dev, &ci, nullptr, &r.i));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, r.i, &mr);
	VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = mr.size;
	ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	CK(vkAllocateMemory(dev, &ai, nullptr, &r.m));
	CK(vkBindImageMemory(dev, r.i, r.m, 0));
	VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = r.i;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = fmt;
	vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	CK(vkCreateImageView(dev, &vi, nullptr, &r.v));
	return r;
}

static std::vector<uint32_t> compile(const std::string& src, const char* name)
{
	glslang_input_t in{};
	in.language = GLSLANG_SOURCE_GLSL;
	in.stage = GLSLANG_STAGE_COMPUTE;
	in.client = GLSLANG_CLIENT_VULKAN;
	in.client_version = GLSLANG_TARGET_VULKAN_1_1;
	in.target_language = GLSLANG_TARGET_SPV;
	in.target_language_version = GLSLANG_TARGET_SPV_1_3;
	in.code = src.c_str();
	in.default_version = 450;
	in.default_profile = GLSLANG_NO_PROFILE;
	in.messages = GLSLANG_MSG_DEFAULT_BIT;
	in.resource = glslang_default_resource();
	glslang_shader_t* sh = glslang_shader_create(&in);
	if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in))
	{
		std::fprintf(stderr, "%s: compile failed\n%s\n%s\n", name, glslang_shader_get_info_log(sh), glslang_shader_get_info_debug_log(sh));
		std::FILE* f = std::fopen("ao_harness_failed.comp", "wb");
		if (f) { std::fwrite(src.data(), 1, src.size(), f); std::fclose(f); }
		std::exit(1);
	}
	glslang_program_t* pr = glslang_program_create();
	glslang_program_add_shader(pr, sh);
	if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
	{
		std::fprintf(stderr, "%s: link failed\n%s\n", name, glslang_program_get_info_log(pr));
		std::exit(1);
	}
	glslang_program_SPIRV_generate(pr, GLSLANG_STAGE_COMPUTE);
	std::vector<uint32_t> spv(glslang_program_SPIRV_get_size(pr));
	glslang_program_SPIRV_get(pr, spv.data());
	glslang_program_delete(pr);
	glslang_shader_delete(sh);
	std::printf("compiled %-12s %6zu SPIR-V words\n", name, spv.size());
	return spv;
}

struct pipe { VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline p; };
// nsamp sampled-image bindings (0..nsamp-1) followed by nstore storage-image bindings
static pipe make_pipe(const std::vector<uint32_t>& spv, uint32_t nsamp, uint32_t nstore)
{
	pipe r{};
	VkDescriptorSetLayoutBinding b[8]{};
	for (uint32_t i = 0; i < nsamp + nstore; ++i)
	{
		b[i].binding = i;
		b[i].descriptorType = i < nsamp ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dci.bindingCount = nsamp + nstore;
	dci.pBindings = b;
	CK(vkCreateDescriptorSetLayout(dev, &dci, nullptr, &r.dsl));
	VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(r2cacao::push) };
	VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	lci.setLayoutCount = 1;
	lci.pSetLayouts = &r.dsl;
	lci.pushConstantRangeCount = 1;
	lci.pPushConstantRanges = &pc;
	CK(vkCreatePipelineLayout(dev, &lci, nullptr, &r.pl));
	VkShaderModuleCreateInfo sci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	sci.codeSize = spv.size() * 4;
	sci.pCode = spv.data();
	VkShaderModule sm;
	CK(vkCreateShaderModule(dev, &sci, nullptr, &sm));
	VkComputePipelineCreateInfo pci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	pci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main" };
	pci.layout = r.pl;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &r.p));
	return r;
}

static VkDescriptorPool dpool;
static VkSampler smp;
static VkDescriptorSet make_set(const pipe& p, std::initializer_list<const img*> sampled, std::initializer_list<const img*> storage)
{
	VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	ai.descriptorPool = dpool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &p.dsl;
	VkDescriptorSet s;
	CK(vkAllocateDescriptorSets(dev, &ai, &s));
	VkDescriptorImageInfo ii[8];
	VkWriteDescriptorSet w[8]{};
	uint32_t n = 0;
	for (const img* i : sampled)
	{
		ii[n] = { smp, i->v, VK_IMAGE_LAYOUT_GENERAL };
		w[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s, n, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[n] };
		++n;
	}
	for (const img* i : storage)
	{
		ii[n] = { VK_NULL_HANDLE, i->v, VK_IMAGE_LAYOUT_GENERAL };
		w[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s, n, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[n] };
		++n;
	}
	vkUpdateDescriptorSets(dev, n, w, 0, nullptr);
	return s;
}

static void barrier(VkCommandBuffer cb)
{
	VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

int main(int argc, char** argv)
{
	if (argc < 11) die("usage: ao_harness <depth.f32> <w> <h> <linear 0|1> <near> <far> <vfov_deg> <aspect> <quality 0|1|2|all> <out_prefix> [runs]");
	const uint32_t W = std::atoi(argv[2]), H = std::atoi(argv[3]);
	r2cacao::params prm;
	prm.linear_depth = std::atoi(argv[4]) != 0;
	prm.n = static_cast<float>(std::atof(argv[5]));
	prm.f = static_cast<float>(std::atof(argv[6]));
	prm.fov_y = static_cast<float>(std::atof(argv[7]) * 3.14159265358979 / 180.0);
	prm.aspect = static_cast<float>(std::atof(argv[8]));
	const int qa = std::strcmp(argv[9], "all") ? std::atoi(argv[9]) : 0, qb = std::strcmp(argv[9], "all") ? qa : 2;
	const std::string out = argv[10];
	const int runs = argc > 11 ? std::max(1, std::atoi(argv[11])) : 20;

	std::vector<float> depth(static_cast<size_t>(W) * H);
	{
		std::FILE* f = std::fopen(argv[1], "rb");
		if (!f || std::fread(depth.data(), 4, depth.size(), f) != depth.size()) die("cannot read the depth file");
		std::fclose(f);
	}

	HMODULE lib = LoadLibraryA("vulkan-1.dll");
	if (!lib) die("no vulkan-1.dll");
	vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
	vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "ao_harness";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	VkInstance inst;
	CK(vkCreateInstance(&ici, nullptr, &inst));
#define LI(x) x = reinterpret_cast<PFN_##x>(vkGetInstanceProcAddr(inst, #x)); if (!x) die(#x)
	LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties); LI(vkGetPhysicalDeviceQueueFamilyProperties);
	LI(vkGetPhysicalDeviceMemoryProperties); LI(vkGetPhysicalDeviceFormatProperties); LI(vkCreateDevice); LI(vkGetDeviceProcAddr);
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(inst, &n, nullptr);
	std::vector<VkPhysicalDevice> pds(n);
	vkEnumeratePhysicalDevices(inst, &n, pds.data());
	if (!n) die("no Vulkan device");
	pd = pds[0];
	VkPhysicalDeviceProperties props;
	for (auto p : pds)
	{
		vkGetPhysicalDeviceProperties(p, &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = p; break; }
	}
	vkGetPhysicalDeviceProperties(pd, &props);
	std::printf("device: %s\n", props.deviceName);
	vkGetPhysicalDeviceMemoryProperties(pd, &memp);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
	std::vector<VkQueueFamilyProperties> qf(n);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf.data());
	uint32_t qfi = 0;
	while (qfi < n && !(qf[qfi].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++qfi;
	const float prio = 1.f;
	VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkPhysicalDeviceFeatures feats{};
	feats.shaderStorageImageExtendedFormats = VK_TRUE;
	VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &feats;
	CK(vkCreateDevice(pd, &dci, nullptr, &dev));
#define LD(x) x = reinterpret_cast<PFN_##x>(vkGetDeviceProcAddr(dev, #x)); if (!x) die(#x)
	LD(vkGetDeviceQueue); LD(vkCreateCommandPool); LD(vkAllocateCommandBuffers); LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer);
	LD(vkQueueSubmit); LD(vkQueueWaitIdle); LD(vkCreateBuffer); LD(vkGetBufferMemoryRequirements); LD(vkAllocateMemory);
	LD(vkBindBufferMemory); LD(vkMapMemory); LD(vkCreateImage); LD(vkGetImageMemoryRequirements); LD(vkBindImageMemory);
	LD(vkCreateImageView); LD(vkCreateSampler); LD(vkCreateShaderModule); LD(vkCreateDescriptorSetLayout); LD(vkCreatePipelineLayout);
	LD(vkCreateComputePipelines); LD(vkCreateDescriptorPool); LD(vkAllocateDescriptorSets); LD(vkUpdateDescriptorSets);
	LD(vkCmdBindPipeline); LD(vkCmdBindDescriptorSets); LD(vkCmdPushConstants); LD(vkCmdDispatch); LD(vkCmdPipelineBarrier);
	LD(vkCmdCopyBufferToImage); LD(vkCmdCopyImageToBuffer); LD(vkCreateQueryPool); LD(vkCmdResetQueryPool); LD(vkCmdWriteTimestamp);
	LD(vkGetQueryPoolResults); LD(vkResetCommandBuffer);
	VkQueue q;
	vkGetDeviceQueue(dev, qfi, 0, &q);

	glslang_initialize_process();
	const auto sp_prep = compile(r2cacao::prepare_src(), "prepare");
	const auto sp_blur = compile(r2cacao::blur_src(), "blur");
	const auto sp_apply = compile(r2cacao::apply_src(), "apply");
	std::vector<uint32_t> sp_gen[3];
	for (int qq = 0; qq < 3; ++qq) sp_gen[qq] = compile(r2cacao::generate_src(qq), qq == 0 ? "generate Q0" : qq == 1 ? "generate Q1" : "generate Q2");

	const pipe p_prep = make_pipe(sp_prep, 1, 2), p_blur = make_pipe(sp_blur, 1, 1), p_apply = make_pipe(sp_apply, 1, 1);
	pipe p_gen[3];
	for (int qq = 0; qq < 3; ++qq) p_gen[qq] = make_pipe(sp_gen[qq], 2, 1);

	const uint32_t LW = r2cacao::layer_w(W), LH = r2cacao::layer_w(H);
	img i_depth = make_img(VK_FORMAT_R32_SFLOAT, W, H);
	img i_z = make_img(VK_FORMAT_R32_SFLOAT, LW * 2, LH * 2);
	img i_n = make_img(VK_FORMAT_R8G8B8A8_UNORM, LW * 2, LH * 2);
	img i_a = make_img(VK_FORMAT_R8G8_UNORM, LW * 2, LH * 2);
	img i_b = make_img(VK_FORMAT_R8G8_UNORM, LW * 2, LH * 2);
	img i_o = make_img(VK_FORMAT_R8_UNORM, W, H);

	VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	CK(vkCreateSampler(dev, &sci, nullptr, &smp));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 } };
	VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = 32;
	dpci.poolSizeCount = 2;
	dpci.pPoolSizes = ps;
	CK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));
	const VkDescriptorSet s_prep = make_set(p_prep, { &i_depth }, { &i_z, &i_n });
	VkDescriptorSet s_gen[3];
	for (int qq = 0; qq < 3; ++qq) s_gen[qq] = make_set(p_gen[qq], { &i_z, &i_n }, { &i_a });
	const VkDescriptorSet s_blur_ab = make_set(p_blur, { &i_a }, { &i_b }), s_blur_ba = make_set(p_blur, { &i_b }, { &i_a });
	const VkDescriptorSet s_apply_a = make_set(p_apply, { &i_a }, { &i_o }), s_apply_b = make_set(p_apply, { &i_b }, { &i_o });

	VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = qfi;
	VkCommandPool cp;
	CK(vkCreateCommandPool(dev, &cpci, nullptr, &cp));
	VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cbai.commandPool = cp;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cb;
	CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
	const auto submit = [&]
	{
		CK(vkEndCommandBuffer(cb));
		VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
		CK(vkQueueWaitIdle(q));
	};
	const auto begin = [&]
	{
		vkResetCommandBuffer(cb, 0);
		VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		CK(vkBeginCommandBuffer(cb, &bi));
	};

	// upload + layouts (everything stays GENERAL)
	buf up = make_buf(depth.size() * 4), down = make_buf(static_cast<VkDeviceSize>(W) * H + 4096);
	std::memcpy(up.p, depth.data(), depth.size() * 4);
	begin();
	{
		VkImageMemoryBarrier ib[6]{};
		const img* all[6] = { &i_depth, &i_z, &i_n, &i_a, &i_b, &i_o };
		for (int i = 0; i < 6; ++i)
		{
			ib[i] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			ib[i].image = all[i]->i;
			ib[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			ib[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		}
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 6, ib);
		VkBufferImageCopy bc{};
		bc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		bc.imageExtent = { W, H, 1 };
		vkCmdCopyBufferToImage(cb, up.b, i_depth.i, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
		barrier(cb);
	}
	submit();

	VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	qpci.queryCount = 8;
	VkQueryPool qp;
	CK(vkCreateQueryPool(dev, &qpci, nullptr, &qp));

	for (int qq = qa; qq <= qb; ++qq)
	{
		prm.quality = qq;
		r2cacao::push k;
		r2cacao::make_push(k, prm, W, H, W, H);
		const uint32_t gx = (LW * 2 + 7) / 8, gy = (LH * 2 + 7) / 8;
		const uint32_t nblur = r2cacao::blur_passes(qq);
		double t_sum[4] = {}, t_tot = 0;
		for (int run = 0; run <= runs; ++run)
		{
			begin();
			vkCmdResetQueryPool(cb, qp, 0, 8);
			const auto stage = [&](const pipe& p, VkDescriptorSet s, uint32_t x, uint32_t y)
			{
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.p);
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &s, 0, nullptr);
				vkCmdPushConstants(cb, p.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof k, &k);
				vkCmdDispatch(cb, x, y, 1);
				barrier(cb);
			};
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
			stage(p_prep, s_prep, gx, gy);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, 1);
			stage(p_gen[qq], s_gen[qq], gx, gy);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, 2);
			for (uint32_t b = 0; b < nblur; ++b) stage(p_blur, (b & 1) ? s_blur_ba : s_blur_ab, gx, gy);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, 3);
			stage(p_apply, (nblur & 1) ? s_apply_b : s_apply_a, (W + 7) / 8, (H + 7) / 8);
			vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qp, 4);
			if (run == runs)
			{
				VkBufferImageCopy bc{};
				bc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				bc.imageExtent = { W, H, 1 };
				vkCmdCopyImageToBuffer(cb, i_o.i, VK_IMAGE_LAYOUT_GENERAL, down.b, 1, &bc);
			}
			submit();
			uint64_t ts[5];
			CK(vkGetQueryPoolResults(dev, qp, 0, 5, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
			if (run == 0) continue; // warm-up
			const double tick = props.limits.timestampPeriod * 1e-6;
			for (int i = 0; i < 4; ++i) t_sum[i] += (ts[i + 1] - ts[i]) * tick;
			t_tot += (ts[4] - ts[0]) * tick;
		}
		std::printf("quality %s (%d taps x2, %u blur): prepare %.3f ms, generate %.3f ms, blur %.3f ms, apply %.3f ms, total %.3f ms at %ux%u (%d runs)\n",
			r2cacao::quality_name(qq), qq == 0 ? 3 : qq == 1 ? 5 : 12, nblur, t_sum[0] / runs, t_sum[1] / runs, t_sum[2] / runs, t_sum[3] / runs,
			t_tot / runs, W, H, runs);
		// R8 rows are tightly packed in the buffer (bufferRowLength 0)
		const std::string fn = out + "_q" + std::to_string(qq) + ".r8";
		std::FILE* f = std::fopen(fn.c_str(), "wb");
		if (!f) die("cannot write the output");
		std::fwrite(down.p, 1, static_cast<size_t>(W) * H, f);
		std::fclose(f);
		const unsigned char* px = static_cast<const unsigned char*>(down.p);
		double mean = 0;
		for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) mean += px[i];
		std::printf("  wrote %s (mean AO %.3f)\n", fn.c_str(), mean / (255.0 * W * H));
	}
	glslang_finalize_process();
	return 0;
}
