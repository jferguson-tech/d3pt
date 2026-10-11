// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
//
// Vulkan ray tracing backend. The scene lives on the GPU as two acceleration
// structures, one for the map and one rebuilt every frame for what moves; a
// compute shader traces a ray per pixel through them (ray queries) into a
// picture that the last pass puts on screen under the overlay.
//
// The tracing follows the CPU backend: the same lights, found the same way,
// the same materials and the same paths, written again as shaders. What
// comes out is gathered over frames and filtered by further compute passes.

#include "../include/pt.h"
#include "../cpu/pt_world.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#define PT_SURFACE_EXTENSION VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#else
#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <unistd.h>
#define PT_SURFACE_EXTENSION VK_KHR_XLIB_SURFACE_EXTENSION_NAME
#endif
#include <vulkan/vulkan.h>
#ifndef _WIN32
// names Xlib takes for itself
#undef None
#undef Bool
#undef Status
#undef Always
#undef Success
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

const uint32_t blit_vert_spv[] =
#include "blit.vert.inc"
;
const uint32_t blit_frag_spv[] =
#include "blit.frag.inc"
;
const uint32_t trace_comp_spv[] =
#include "trace.comp.inc"
;
const uint32_t temporal_comp_spv[] =
#include "temporal.comp.inc"
;
const uint32_t change_comp_spv[] =
#include "change.comp.inc"
;
const uint32_t atrous_comp_spv[] =
#include "atrous.comp.inc"
;
const uint32_t compose_comp_spv[] =
#include "compose.comp.inc"
;
const uint32_t bloom_comp_spv[] =
#include "bloom.comp.inc"
;
const uint32_t resolve_comp_spv[] =
#include "resolve.comp.inc"
;
const uint32_t grade_comp_spv[] =
#include "grade.comp.inc"
;

const uint32_t VENDOR_NVIDIA = 0x10DE;

const char *const kDeviceExtensions[] = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_RAY_QUERY_EXTENSION_NAME,
};
const uint32_t kNumDeviceExtensions = sizeof(kDeviceExtensions) / sizeof(kDeviceExtensions[0]);

struct Push
{
	float	view[4];
	float	screen[2];
	float	has_view;		// there is a traced picture to show in the view
};

// a few numbers of its own for each compute pass
struct PassPush
{
	int32_t	a, b, c, d;
};

// simulated bodies of liquid the shaders are told of; those of a map beyond
// that stay the level sheets the map has for them
const int kMaxWaters = 24;

// What the shaders are told about the frame (std140); see scene.glsl, which
// says what is in each.
struct FrameBlock
{
	float	origin[4], forward[4], right[4], up[4];
	float	prev_origin[4], prev_forward[4], prev_right[4], prev_up[4];
	int32_t	sky_a[4], sky_b[4];
	float	sky_turn[4], sky_misc[4];
	int32_t	counts[4], bases[4], grid_dims[4];
	float	grid_origin[4];
	int32_t	table_at[4], table_at2[4], settings[4];
	float	settings_f[4];
	int32_t	settings2[4];
	float	medium[4];
	int32_t	output_i[4];
	float	output_f[4];
	int32_t	frame_has[4], size[4];
	float	water_rect[kMaxWaters][4], water_at[kMaxWaters][4], water_wave[kMaxWaters][4], water_lamp[kMaxWaters * 16][4];
	int32_t	out_size[4];
	float	open_origin[4], open_forward[4], open_right[4], open_up[4];	// motion blur: the eye as the shutter opened; open_origin[3]: there is blur
	int32_t	held[4];		// first triangle of the frame that the eye carries, how many; [2]: reflections are followed where they appear to be
	float	painted[4];		// [0]: what a metal painted dark reflects, see pt_view_t's metal_colour; [1]: bloom_max; [2]: fog_samples; [3]: fog_history
	float	liquid[4];		// [0]: wave_reach; [1]: react; [2]: water_shafts; [3]: water_wet
	int32_t	table_at3[4];	// in indices: what stands in for each of the map's lights when it is not kept; the cells' lists in order of light
};

// one triangle, one material and one light as the shaders read them (std430)
struct GpuTri
{
	float		uv[6];
	uint32_t	material;
	uint32_t	pad;
};

struct GpuMaterial
{
	int32_t		texture;
	uint32_t	flags;
	float		alpha;
	float		roughness;
	float		emission[4];
	float		emission_per_texel[4];
	int32_t		normal_texture, emission_map, anim_next, anim_length;
	float		absorb[4];
	float		wave_rect[4];
	float		scroll[2];
	int32_t		wave_map, caustic_map;
	uint32_t	bits;
	int32_t		foam_map;
	uint32_t	specular_map;	// slot + 1, 0 = none
	uint32_t	pad;
};

struct GpuLight
{
	float		origin[3];	// a triangle's first corner
	uint32_t	tri;		// a point: kPointLight plus its light style
	float		emission[3];
	float		pdf;
	float		dir[3];		// a triangle: from its first corner to its second
	float		cone_cos;
	float		edge[3];	// a triangle: from its first corner to its third
	float		radius;
	float		box_origin[3];	// a point: its lit volume, if all of box_size is above 0
	float		no_shadow;		// a point: above 0, nothing stops its light
	float		box_size[3];
	float		pad;
};
const uint32_t kPointLight = 0xffffff00u;

const uint32_t kBitEmissive = 1, kBitSampled = 2, kBitSwell = 4;	// GpuMaterial::bits
// Instances of the top level structure: the map, its glass, what moves, its
// glass, and what the eye carries. The last has a mask bit of its own so that
// a ray can be cast at it alone, or past it: see Nearest in scene.glsl.
const uint32_t kNumInstances = 5;
const uint32_t kMaskScene = 1, kMaskHeld = 2;
const int kNumStyles = 256;							// light styles, at the start of the tables
const uint32_t kNumBindings = 39;

// The pictures kept per pixel between the passes, in the order the shaders'
// bindings take them; see scene.glsl.
enum
{
	kSurface = 0,	// 2
	kSeen = 2,
	kAlbedo = 3,	// 2
	kNoisy = 5,		// 3
	kExtra = 8,
	kKept = 9,		// 6
	kFilter = 15,	// 6
	kPicture = 21,
	kHdr = 22,
	kBloom = 23,	// 2
	kSteady = 25,	// 2
	kGraded = 27,
	kMirror = 28,	// 2
	kOver = 30,
	kMoments = 31,	// 6
	kFlash = 37,	// 2
	kChange = 39,	// 4
	kExport = 43,	// 4
	kNumTargets = 47
};

const uint32_t kMaxTextures = 4096;

// The parts a view's time is counted in. The first is spent here, getting
// the view ready; the rest on the card, which notes the time between them.
const int kNumStages = 6;
// The card notes the time before each of its parts and after the last. A
// build made with PT_PERF looks closer, at each pass, and has two readings
// more past those for the frame's own commands: see pt_perf in the README.
#ifdef PT_PERF
const uint32_t kMaxStamps = 24;
const uint32_t kNumQueries = kMaxStamps + 2;
const int kMaxLaps = 16;
#else
const uint32_t kMaxStamps = kNumStages;
const uint32_t kNumQueries = kMaxStamps;
#endif
const char *const kStageNames[kNumStages] = {"scene", "build", "trace", "history", "filter", "out"};

struct Buffer
{
	VkBuffer		buffer = VK_NULL_HANDLE;
	VkDeviceMemory	memory = VK_NULL_HANDLE;
	VkDeviceSize	size = 0;
	void			*ptr = nullptr;			// where it is mapped, if it is
	VkDeviceAddress	address = 0;
};

struct Texture
{
	VkImage			image = VK_NULL_HANDLE;
	VkDeviceMemory	memory = VK_NULL_HANDLE;
	VkImageView		view = VK_NULL_HANDLE;
	int				width = 0, height = 0;
	float			average[3] = {1, 1, 1};	// colour, in linear light
};

struct Target
{
	VkImage			image = VK_NULL_HANDLE;
	VkDeviceMemory	memory = VK_NULL_HANDLE;
	VkImageView		view = VK_NULL_HANDLE;
};

// a texture whose new pixels wait to be sent to the card
struct Pending
{
	int				slot;
	VkDeviceSize	offset;
};

struct Accel
{
	VkAccelerationStructureKHR	as = VK_NULL_HANDLE;
	Buffer						storage;
	Buffer						scratch;
	VkDeviceAddress				scratch_address = 0;	// aligned as the build wants
	VkDeviceAddress				address = 0;
};

// The triangles of the map or of a frame, as the GPU holds them: the solid
// ones first, then the glass and liquid, then, in a frame, what the eye
// carries (PT_MAT_HELD), each with a structure of its own.
struct Geometry
{
	Buffer		corners;		// 9 floats a triangle
	Buffer		tris;			// GpuTri
	Buffer		materials;		// GpuMaterial
	Buffer		normals;		// 9 floats a triangle
	Buffer		tangents;		// 18 floats a triangle: at each corner the way u runs, then the way v does
	Buffer		prev;			// where the corners were last frame; the frame only
	Accel		solid, glass, held;
	uint32_t	num_solid = 0, num_glass = 0, num_held = 0;
	uint32_t	room_tris = 0, room_materials = 0;
};

struct RtxBackend
{
	pt_backend_t	base{};
	pt_log_fn		log = nullptr;
	int				width = 0, height = 0;

	VkInstance			instance = VK_NULL_HANDLE;
	VkSurfaceKHR		surface = VK_NULL_HANDLE;
	VkPhysicalDevice	gpu = VK_NULL_HANDLE;
	VkDevice			device = VK_NULL_HANDLE;
	uint32_t			queue_family = 0;
	VkQueue				queue = VK_NULL_HANDLE;

	VkCommandPool		cmdpool = VK_NULL_HANDLE;
	VkCommandBuffer		cmd = VK_NULL_HANDLE;
	VkCommandBuffer		cmd_once = VK_NULL_HANDLE;	// for work done at once, outside a frame
	VkCommandBuffer		cmd_trace = VK_NULL_HANDLE;	// the tracing of a view, sent off before the frame that shows it
	VkFence				fence = VK_NULL_HANDLE;
	VkFence				fence_trace = VK_NULL_HANDLE;	// the card has finished cmd_trace
	VkSemaphore			sem_acquire = VK_NULL_HANDLE;

	VkSwapchainKHR		swapchain = VK_NULL_HANDLE;

	// made with pt_create_t's shared: no window; one picture stands in for the
	// swapchain's, in memory that is exported
	bool				shared = false;
	VkImage				shared_image = VK_NULL_HANDLE;
	VkDeviceMemory		shared_memory = VK_NULL_HANDLE;
	VkDeviceSize		shared_size = 0;
	void				*shared_handle = nullptr;
	bool				any_card = true;	// false: only the card with this UUID
	uint8_t				card_uuid[VK_UUID_SIZE] = {};
	VkSurfaceFormatKHR	surface_format{};
	VkExtent2D			extent{};
	std::vector<VkImage>		swap_images;
	std::vector<VkImageView>	swap_views;
	// signalled when an image has been drawn; one each, because presenting
	// may hold on to it until that image comes round again
	std::vector<VkSemaphore>	swap_drawn;

	VkImage				ov_image = VK_NULL_HANDLE;
	VkDeviceMemory		ov_memory = VK_NULL_HANDLE;
	VkImageView			ov_view = VK_NULL_HANDLE;
	VkSampler			ov_sampler = VK_NULL_HANDLE;
	VkBuffer			staging = VK_NULL_HANDLE;
	VkDeviceMemory		staging_memory = VK_NULL_HANDLE;
	void				*staging_ptr = nullptr;
	bool				ov_initialized = false;

	VkDescriptorSetLayout	dsl = VK_NULL_HANDLE;
	VkDescriptorPool		dpool = VK_NULL_HANDLE;
	VkDescriptorSet			dset = VK_NULL_HANDLE;
	VkPipelineLayout		playout = VK_NULL_HANDLE;
	VkPipeline				pipeline = VK_NULL_HANDLE;

	bool		has_view = false;
	pt_view_t	view{};

	// extension entry points, which the loader does not export
	PFN_vkCreateAccelerationStructureKHR			CreateAccelerationStructure = nullptr;
	PFN_vkDestroyAccelerationStructureKHR			DestroyAccelerationStructure = nullptr;
	PFN_vkGetAccelerationStructureBuildSizesKHR		GetAccelerationStructureBuildSizes = nullptr;
	PFN_vkCmdBuildAccelerationStructuresKHR			CmdBuildAccelerationStructures = nullptr;
	PFN_vkGetAccelerationStructureDeviceAddressKHR	GetAccelerationStructureDeviceAddress = nullptr;
	VkDebugUtilsMessengerEXT	messenger = VK_NULL_HANDLE;
	uint32_t					scratch_align = 1;

	// the scene
	std::vector<Texture>	textures;			// kMaxTextures slots; the shaders see them all
	std::vector<int>		world_textures;		// slots the map's textures were given
	VkSampler				tex_sampler = VK_NULL_HANDLE, smooth_sampler = VK_NULL_HANDLE;
	Texture					blank;				// stands in every slot that holds nothing
	Geometry				world, frame;
	bool					world_loaded = false;
	bool					world_has_waves = false;
	int32_t					sky[6] = {-1, -1, -1, -1, -1, -1};
	Accel					tlas;
	Buffer					instances;
	uint32_t				num_instances = 0;

	// lights and the tables for finding them; see LoadWorldNow
	Buffer					frame_block, world_lights, frame_lights, tables, indices;
	// The sums the exposure is set from. The shader adds to them in the card's
	// own memory, where they run on and wrap round, and each view's tracing
	// ends by copying them to where they can be read here. (Added up straight
	// into memory of the host's, every sum was a trip to it and back: 4 ms of
	// a 47 ms frame on an RTX 4060 Laptop.)
	Buffer					meter, meter_read;
	uint32_t				meter_was[64] = {};		// what they stood at when last read
	float					metered = 0.0f;			// the light the exposure last went by
	int						num_world_lights = 0;
	std::vector<int>		light_given;		// for each light load_world was given, its place in world_lights or -1
	std::vector<float>		light_base;			// 3 per world light: its emission as given
	bool					lights_scaled = false;
	bool					has_grid = false;
	float					grid_origin[3] = {0, 0, 0}, grid_inv_cell = 0.0f;
	int						grid_dims[3] = {0, 0, 0};
	int						table_light_keep = 0, table_grid_pdf = 0, table_grid_keep = 0, table_sky_chance = 0, table_sky_cdf = 0;
	int						index_grid_light = 0, index_grid_count = 0, index_light_other = 0, index_grid_sorted = 0;
	int						sky_res = 0;
	float					sky_total = 0.0f, sky_scale = 1.0f;
	// simulated bodies of liquid: extent, height of the surface, material
	int						num_waters = 0;
	float					water_rect[kMaxWaters][4] = {}, water_at[kMaxWaters][4] = {}, water_wave[kMaxWaters][4] = {}, water_lamp[kMaxWaters * 16][4] = {};

	// textures the host has changed, until the next frame takes them
	Buffer					updates;
	VkDeviceSize			updates_used = 0;
	std::vector<Pending>	pending;

	// the passes and the pictures they hand on
	Target					targets[kNumTargets];
	VkSampler				trace_sampler = VK_NULL_HANDLE;
	int						trace_width = 0, trace_height = 0;	// what is traced
	int						out_width = 0, out_height = 0;		// the finished picture: the size of the view
	VkDescriptorSetLayout	trace_dsl = VK_NULL_HANDLE;
	VkDescriptorPool		trace_dpool = VK_NULL_HANDLE;
	VkDescriptorSet			trace_dset = VK_NULL_HANDLE;
	VkPipelineLayout		trace_playout = VK_NULL_HANDLE;
	VkPipeline				trace_pipeline = VK_NULL_HANDLE, temporal_pipeline = VK_NULL_HANDLE;
	VkPipeline				atrous_pipeline = VK_NULL_HANDLE, compose_pipeline = VK_NULL_HANDLE;
	VkPipeline				bloom_pipeline = VK_NULL_HANDLE, resolve_pipeline = VK_NULL_HANDLE;
	VkPipeline				grade_pipeline = VK_NULL_HANDLE, change_pipeline = VK_NULL_HANDLE;
	bool					bloom_on = false;
	float					camera[16] = {};		// the last view, to tell whether it has moved
	FrameBlock				block{};				// what the shaders were last told
	int						filter_passes = 0;
	bool					trace_ready = false;	// there is a picture of this frame's view to show
	bool					trace_pending = false;	// but it has yet to be traced
	bool					has_history = false;	// the last frame's pictures can be built on
	bool					react_on = false;		// this frame looks for where the light has changed
	int						still_frames = 0;		// frames in a row in which nothing changed
	int						parity = 0;				// which of each pair of pictures is this frame's
	uint32_t				frame_index = 0;
	uint32_t				prev_hash = 0;
	float					prev_time = 0.0f;
	float					auto_exposure = 1.0f, exposure_used = 0.0f;
	bool					have_exposure = false;

	// for reading a frame back
	Buffer					readback;
	std::vector<uint32_t>	last_overlay;
	std::vector<VkBufferImageCopy>	ov_regions;	// what of the overlay goes to the card this frame
	bool					ov_behind = true;	// a frame's changes did not reach the card
	int						view_rect[4] = {0, 0, 0, 0};	// x, y, width, height of this frame's view
	int						shown[4] = {0, 0, 0, 0};		// and of the one last presented
	bool					shown_traced = false;

	// where the time goes
	VkQueryPool	stamps = VK_NULL_HANDLE;	// null if the card keeps no time
	double		stamp_ms = 0.0;				// milliseconds to one tick of its clock
	uint64_t	stamp_mask = ~0ull;			// the bits of a reading that count
	bool		stamps_asked = false;		// a view's times were asked for and are not yet read
	float		scene_ms = 0.0f;			// getting that view ready here
	float		stage_ms[kNumStages] = {};	// the latest known
	bool		stages_known = false;
	bool		stages_new = false;			// and nobody has asked since
	int			stamp_stage[kMaxStamps] = {};	// the stage the time after each reading belongs to
	uint32_t	stamps_written = 0;
#ifdef PT_PERF
	const char	*stamp_name[kMaxStamps] = {};	// and the name of the pass that follows it
	pt_stage_t	card[kMaxStamps] = {};		// the card's passes on the latest view known
	int			num_card = 0;
	bool		show_free = false;			// the two readings round the frame's commands may be written
	bool		show_asked = false;
	float		show_ms = 0.0f;
	pt_stage_t	laps[kMaxLaps] = {};		// what was done here since perf was last asked
	int			num_laps = 0;
	bool		laps_taken = true;
	std::chrono::steady_clock::time_point lap_at;
#endif

	char		device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "";
	char		stats[320] = "";
	int			world_smooth = 0;			// the map came with 1: normals, 2: tangents
};

// The work done here, timed part by part: LapStart, then Lap after each part
// with a name for it. Nothing at all unless the build is made with PT_PERF.
#ifdef PT_PERF
void LapStart(RtxBackend *s)
{
	if (s->laps_taken)
	{
		s->num_laps = 0;
		s->laps_taken = false;
	}
	s->lap_at = std::chrono::steady_clock::now();
}

void Lap(RtxBackend *s, const char *name, int where = 0)
{
	const auto now = std::chrono::steady_clock::now();
	const float ms = std::chrono::duration<float, std::milli>(now - s->lap_at).count();
	s->lap_at = now;
	for (int i = 0; i < s->num_laps; i++)
	{
		if (!strcmp(s->laps[i].name, name))
		{
			s->laps[i].ms += ms;
			return;
		}
	}
	if (s->num_laps < kMaxLaps)
		s->laps[s->num_laps++] = {name, ms, where};
}
#else
inline void LapStart(RtxBackend *) {}
inline void Lap(RtxBackend *, const char *, int = 0) {}
#endif

RtxBackend *Self(pt_backend_t *b) { return reinterpret_cast<RtxBackend *>(b); }

void Logf(const RtxBackend *s, const char *fmt, ...)
{
	if (!s->log)
		return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	s->log(buf);
}

struct Fail
{
	std::string	msg;
};

[[noreturn]] void Throw(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	throw Fail{buf};
}

void Check(VkResult r, const char *what)
{
	if (r != VK_SUCCESS)
		Throw("%s failed (VkResult %d)", what, (int)r);
}

// ---------------------------------------------------------------- device

bool HasExtensions(VkPhysicalDevice gpu, std::string &missing)
{
	uint32_t n = 0;
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> exts(n);
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, exts.data());

	for (uint32_t i = 0; i < kNumDeviceExtensions; i++)
	{
		bool found = false;
		for (const VkExtensionProperties &e : exts)
			if (!strcmp(e.extensionName, kDeviceExtensions[i]))
				found = true;
		if (!found)
		{
			missing = kDeviceExtensions[i];
			return false;
		}
	}
	return true;
}

bool FindQueueFamily(const RtxBackend *s, VkPhysicalDevice gpu, uint32_t &family)
{
	uint32_t n = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
	std::vector<VkQueueFamilyProperties> props(n);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, props.data());

	for (uint32_t i = 0; i < n; i++)
	{
		VkBool32 present = VK_TRUE;		// nothing is presented without a surface
		if (s->surface)
			vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, s->surface, &present);
		if (present && (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		{
			family = i;
			return true;
		}
	}
	return false;
}

struct RtFeatures
{
	VkPhysicalDeviceFeatures2							f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceVulkan12Features					v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceVulkan13Features					v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceAccelerationStructureFeaturesKHR	as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
	VkPhysicalDeviceRayQueryFeaturesKHR					rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};

	RtFeatures()
	{
		f2.pNext = &v12;
		v12.pNext = &v13;
		v13.pNext = &as;
		as.pNext = &rq;
	}
	RtFeatures(const RtFeatures &) = delete;
	RtFeatures &operator=(const RtFeatures &) = delete;
};

void PickDevice(RtxBackend *s)
{
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(s->instance, &n, nullptr);
	std::vector<VkPhysicalDevice> gpus(n);
	vkEnumeratePhysicalDevices(s->instance, &n, gpus.data());
	if (!n)
		Throw("no Vulkan devices found");

	std::string reasons;
	for (VkPhysicalDevice gpu : gpus)
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(gpu, &props);

		std::string why;
		std::string missing;
		RtFeatures feat;
		uint32_t family = 0;

		VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		props2.pNext = &id;
		if (!s->any_card)
			vkGetPhysicalDeviceProperties2(gpu, &props2);

		if (!s->any_card && memcmp(id.deviceUUID, s->card_uuid, VK_UUID_SIZE))
			why = "not the card asked for";
		else if (props.vendorID != VENDOR_NVIDIA)
			why = "not an Nvidia GPU";
		else if (props.apiVersion < VK_API_VERSION_1_3)
			why = "driver does not support Vulkan 1.3";
		else if (!HasExtensions(gpu, missing))
			why = "missing " + missing;
		else if (!FindQueueFamily(s, gpu, family))
			why = "cannot present to this window";
		else
		{
			vkGetPhysicalDeviceFeatures2(gpu, &feat.f2);
			if (!feat.as.accelerationStructure || !feat.rq.rayQuery ||
				!feat.f2.features.shaderStorageImageArrayDynamicIndexing ||
				!feat.f2.features.shaderSampledImageArrayDynamicIndexing ||
				!feat.v12.bufferDeviceAddress || !feat.v12.runtimeDescriptorArray ||
				!feat.v12.shaderSampledImageArrayNonUniformIndexing ||
				!feat.v13.dynamicRendering || !feat.v13.synchronization2)
				why = "ray tracing features not available";
		}

		if (why.empty())
		{
			s->gpu = gpu;
			s->queue_family = family;
			snprintf(s->device_name, sizeof(s->device_name), "%s", props.deviceName);
			Logf(s, "RTX path tracer: using %s\n", props.deviceName);
			return;
		}
		if (!reasons.empty())
			reasons += "; ";
		reasons += std::string(props.deviceName) + ": " + why;
	}
	Throw("no usable ray tracing GPU (%s)", reasons.c_str());
}

void CreateDevice(RtxBackend *s)
{
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = s->queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;

	// only what the tracer will need; everything here was verified in PickDevice
	RtFeatures feat;
	feat.v12.bufferDeviceAddress = VK_TRUE;
	feat.v13.dynamicRendering = VK_TRUE;
	feat.v13.synchronization2 = VK_TRUE;
	feat.as.accelerationStructure = VK_TRUE;
	feat.rq.rayQuery = VK_TRUE;
	// the shaders pick among their pictures and textures by number
	feat.f2.features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
	feat.f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
	feat.v12.runtimeDescriptorArray = VK_TRUE;
	feat.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &feat.f2;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	std::vector<const char *> extensions(kDeviceExtensions, kDeviceExtensions + kNumDeviceExtensions);
#ifdef _WIN32
	if (s->shared)
		extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
#else
	if (s->shared)
		extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
#endif
	dci.enabledExtensionCount = (uint32_t)extensions.size();
	dci.ppEnabledExtensionNames = extensions.data();
	Check(vkCreateDevice(s->gpu, &dci, nullptr, &s->device), "vkCreateDevice");
	vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = s->queue_family;
	Check(vkCreateCommandPool(s->device, &pci, nullptr, &s->cmdpool), "vkCreateCommandPool");

	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = s->cmdpool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	Check(vkAllocateCommandBuffers(s->device, &cai, &s->cmd), "vkAllocateCommandBuffers");
	Check(vkAllocateCommandBuffers(s->device, &cai, &s->cmd_once), "vkAllocateCommandBuffers");
	Check(vkAllocateCommandBuffers(s->device, &cai, &s->cmd_trace), "vkAllocateCommandBuffers");

#define LOAD(name) \
	s->name = (PFN_vk##name##KHR)vkGetDeviceProcAddr(s->device, "vk" #name "KHR"); \
	if (!s->name) Throw("the driver has no vk" #name "KHR")
	LOAD(CreateAccelerationStructure);
	LOAD(DestroyAccelerationStructure);
	LOAD(GetAccelerationStructureBuildSizes);
	LOAD(CmdBuildAccelerationStructures);
	LOAD(GetAccelerationStructureDeviceAddress);
#undef LOAD

	VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
	VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext = &asp;
	vkGetPhysicalDeviceProperties2(s->gpu, &p2);
	s->scratch_align = std::max(1u, asp.minAccelerationStructureScratchOffsetAlignment);

	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	Check(vkCreateFence(s->device, &fci, nullptr, &s->fence), "vkCreateFence");
	Check(vkCreateFence(s->device, &fci, nullptr, &s->fence_trace), "vkCreateFence");

	VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	Check(vkCreateSemaphore(s->device, &sci, nullptr, &s->sem_acquire), "vkCreateSemaphore");
}

// ------------------------------------------------------------- swapchain

void DestroySwapchain(RtxBackend *s)
{
	for (VkImageView v : s->swap_views)
		vkDestroyImageView(s->device, v, nullptr);
	s->swap_views.clear();
	for (VkSemaphore sem : s->swap_drawn)
		vkDestroySemaphore(s->device, sem, nullptr);
	s->swap_drawn.clear();
	s->swap_images.clear();
	if (s->swapchain)
	{
		vkDestroySwapchainKHR(s->device, s->swapchain, nullptr);
		s->swapchain = VK_NULL_HANDLE;
	}
}

// leaves swapchain null while the window has no area (minimized)
void CreateSwapchain(RtxBackend *s)
{
	VkSurfaceCapabilitiesKHR caps;
	Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->gpu, s->surface, &caps), "surface capabilities");

	VkExtent2D extent = caps.currentExtent;
	if (extent.width == 0xFFFFFFFF)
	{
		extent.width = (uint32_t)s->width;
		extent.height = (uint32_t)s->height;
	}
	if (!extent.width || !extent.height)
		return;

	uint32_t n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(s->gpu, s->surface, &n, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(n);
	vkGetPhysicalDeviceSurfaceFormatsKHR(s->gpu, s->surface, &n, formats.data());
	if (!n)
		Throw("surface has no formats");

	// the engine's colours are already display referred, so no sRGB encode
	s->surface_format = formats[0];
	for (const VkSurfaceFormatKHR &f : formats)
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			s->surface_format = f;

	vkGetPhysicalDeviceSurfacePresentModesKHR(s->gpu, s->surface, &n, nullptr);
	std::vector<VkPresentModeKHR> modes(n);
	vkGetPhysicalDeviceSurfacePresentModesKHR(s->gpu, s->surface, &n, modes.data());

	// the engine paces its own frames, so prefer not to block on vsync
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	for (VkPresentModeKHR m : modes)
		if (m == VK_PRESENT_MODE_IMMEDIATE_KHR)
			mode = m;
	for (VkPresentModeKHR m : modes)
		if (m == VK_PRESENT_MODE_MAILBOX_KHR)
			mode = m;

	uint32_t count = caps.minImageCount + 1;
	if (caps.maxImageCount && count > caps.maxImageCount)
		count = caps.maxImageCount;

	VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sci.surface = s->surface;
	sci.minImageCount = count;
	sci.imageFormat = s->surface_format.format;
	sci.imageColorSpace = s->surface_format.colorSpace;
	sci.imageExtent = extent;
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.preTransform = caps.currentTransform;
	sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.presentMode = mode;
	sci.clipped = VK_TRUE;
	Check(vkCreateSwapchainKHR(s->device, &sci, nullptr, &s->swapchain), "vkCreateSwapchainKHR");
	s->extent = extent;

	vkGetSwapchainImagesKHR(s->device, s->swapchain, &n, nullptr);
	s->swap_images.resize(n);
	vkGetSwapchainImagesKHR(s->device, s->swapchain, &n, s->swap_images.data());

	for (VkImage img : s->swap_images)
	{
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = img;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = s->surface_format.format;
		vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view;
		Check(vkCreateImageView(s->device, &vci, nullptr, &view), "vkCreateImageView");
		s->swap_views.push_back(view);

		VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkSemaphore sem;
		Check(vkCreateSemaphore(s->device, &semci, nullptr, &sem), "vkCreateSemaphore");
		s->swap_drawn.push_back(sem);
	}
}

uint32_t FindMemoryType(const RtxBackend *s, uint32_t bits, VkMemoryPropertyFlags want);

// In place of a swapchain: one picture the size of the window, in memory
// another graphics API can open. It is drawn to as a swapchain's picture is.
void CreateShared(RtxBackend *s)
{
#ifdef _WIN32
	const VkExternalMemoryHandleTypeFlagBits kSharedHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
	const VkExternalMemoryHandleTypeFlagBits kSharedHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
	s->surface_format.format = VK_FORMAT_R8G8B8A8_UNORM;
	s->extent = {(uint32_t)s->width, (uint32_t)s->height};

	VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = kSharedHandleType;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &emi;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = s->surface_format.format;
	ici.extent = {s->extent.width, s->extent.height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	Check(vkCreateImage(s->device, &ici, nullptr, &s->shared_image), "vkCreateImage (shared)");

	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(s->device, s->shared_image, &mr);
	VkExportMemoryAllocateInfo ema{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	ema.handleTypes = kSharedHandleType;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &ema;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = FindMemoryType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	Check(vkAllocateMemory(s->device, &mai, nullptr, &s->shared_memory), "vkAllocateMemory (shared)");
	Check(vkBindImageMemory(s->device, s->shared_image, s->shared_memory, 0), "vkBindImageMemory (shared)");
	s->shared_size = mr.size;

#ifdef _WIN32
	VkMemoryGetWin32HandleInfoKHR ghi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
	ghi.memory = s->shared_memory;
	ghi.handleType = kSharedHandleType;
	HANDLE handle = nullptr;
	// an extension's function: the loader's library has no entry for it
	const auto get_handle = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(s->device, "vkGetMemoryWin32HandleKHR");
	if (!get_handle)
		Throw("vkGetMemoryWin32HandleKHR is missing");
	Check(get_handle(s->device, &ghi, &handle), "vkGetMemoryWin32HandleKHR");
	s->shared_handle = handle;
#else
	VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = s->shared_memory;
	gfi.handleType = kSharedHandleType;
	int fd = -1;
	// an extension's function: the loader's library has no entry for it
	const auto get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(s->device, "vkGetMemoryFdKHR");
	if (!get_fd)
		Throw("vkGetMemoryFdKHR is missing");
	Check(get_fd(s->device, &gfi, &fd), "vkGetMemoryFdKHR");
	// kept as descriptor + 1, so that descriptor 0 is not taken for none
	s->shared_handle = (void *)(intptr_t)(fd + 1);
#endif

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = s->shared_image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = s->surface_format.format;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView view;
	Check(vkCreateImageView(s->device, &vci, nullptr, &view), "vkCreateImageView (shared)");
	s->swap_images.push_back(s->shared_image);
	s->swap_views.push_back(view);
}

void RecreateSwapchain(RtxBackend *s)
{
	vkDeviceWaitIdle(s->device);
	DestroySwapchain(s);
	CreateSwapchain(s);
}

// --------------------------------------------------------------- overlay

uint32_t FindMemoryType(const RtxBackend *s, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(s->gpu, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	Throw("no suitable memory type");
}

void CreateOverlay(RtxBackend *s)
{
	const VkDeviceSize bytes = (VkDeviceSize)s->width * s->height * 4;

	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {(uint32_t)s->width, (uint32_t)s->height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	Check(vkCreateImage(s->device, &ici, nullptr, &s->ov_image), "vkCreateImage");

	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(s->device, s->ov_image, &mr);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = FindMemoryType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	Check(vkAllocateMemory(s->device, &mai, nullptr, &s->ov_memory), "vkAllocateMemory");
	Check(vkBindImageMemory(s->device, s->ov_image, s->ov_memory, 0), "vkBindImageMemory");

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = s->ov_image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	Check(vkCreateImageView(s->device, &vci, nullptr, &s->ov_view), "vkCreateImageView");

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = VK_FILTER_NEAREST;
	sci.minFilter = VK_FILTER_NEAREST;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	Check(vkCreateSampler(s->device, &sci, nullptr, &s->ov_sampler), "vkCreateSampler");

	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = bytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	Check(vkCreateBuffer(s->device, &bci, nullptr, &s->staging), "vkCreateBuffer");

	vkGetBufferMemoryRequirements(s->device, s->staging, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = FindMemoryType(s, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	Check(vkAllocateMemory(s->device, &mai, nullptr, &s->staging_memory), "vkAllocateMemory");
	Check(vkBindBufferMemory(s->device, s->staging, s->staging_memory, 0), "vkBindBufferMemory");
	Check(vkMapMemory(s->device, s->staging_memory, 0, bytes, 0, &s->staging_ptr), "vkMapMemory");
}

// -------------------------------------------------------------- pipeline

void ShowTracePicture(RtxBackend *s);

VkShaderModule CreateShader(RtxBackend *s, const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	ci.codeSize = bytes;
	ci.pCode = code;
	VkShaderModule m;
	Check(vkCreateShaderModule(s->device, &ci, nullptr, &m), "vkCreateShaderModule");
	return m;
}

void CreatePipeline(RtxBackend *s, VkFormat color_format)
{
	// 0: the overlay, 1: the traced picture
	VkDescriptorSetLayoutBinding binding[2]{};
	for (uint32_t i = 0; i < 2; i++)
	{
		binding[i].binding = i;
		binding[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding[i].descriptorCount = 1;
		binding[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dlci.bindingCount = 2;
	dlci.pBindings = binding;
	Check(vkCreateDescriptorSetLayout(s->device, &dlci, nullptr, &s->dsl), "vkCreateDescriptorSetLayout");

	VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &psize;
	Check(vkCreateDescriptorPool(s->device, &dpci, nullptr, &s->dpool), "vkCreateDescriptorPool");

	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = s->dpool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &s->dsl;
	Check(vkAllocateDescriptorSets(s->device, &dsai, &s->dset), "vkAllocateDescriptorSets");

	VkDescriptorImageInfo dii{s->ov_sampler, s->ov_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wds.dstSet = s->dset;
	wds.dstBinding = 0;
	wds.descriptorCount = 1;
	wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wds.pImageInfo = &dii;
	vkUpdateDescriptorSets(s->device, 1, &wds, 0, nullptr);
	ShowTracePicture(s);

	VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &s->dsl;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	Check(vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout), "vkCreatePipelineLayout");

	VkShaderModule vs = CreateShader(s, blit_vert_spv, sizeof(blit_vert_spv));
	VkShaderModule fs = VK_NULL_HANDLE;
	VkResult result;
	try
	{
		fs = CreateShader(s, blit_frag_spv, sizeof(blit_frag_spv));

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fs;
		stages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState att{};
		att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &att;
		const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		ds.dynamicStateCount = 2;
		ds.pDynamicStates = dyn;

		VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rci.colorAttachmentCount = 1;
		rci.pColorAttachmentFormats = &color_format;

		VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpci.pNext = &rci;
		gpci.stageCount = 2;
		gpci.pStages = stages;
		gpci.pVertexInputState = &vi;
		gpci.pInputAssemblyState = &ia;
		gpci.pViewportState = &vp;
		gpci.pRasterizationState = &rs;
		gpci.pMultisampleState = &ms;
		gpci.pColorBlendState = &cb;
		gpci.pDynamicState = &ds;
		gpci.layout = s->playout;
		result = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &s->pipeline);
	}
	catch (...)
	{
		vkDestroyShaderModule(s->device, vs, nullptr);
		throw;
	}
	vkDestroyShaderModule(s->device, vs, nullptr);
	vkDestroyShaderModule(s->device, fs, nullptr);
	Check(result, "vkCreateGraphicsPipelines");
}

// ----------------------------------------------------------------- frame

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
	VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
	VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = src_stage;
	b.srcAccessMask = src_access;
	b.dstStageMask = dst_stage;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.imageMemoryBarrierCount = 1;
	di.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &di);
}

// ------------------------------------------------------------ validation

bool HasValidationLayer()
{
	uint32_t n = 0;
	vkEnumerateInstanceLayerProperties(&n, nullptr);
	std::vector<VkLayerProperties> layers(n);
	vkEnumerateInstanceLayerProperties(&n, layers.data());
	for (const VkLayerProperties &l : layers)
		if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"))
			return true;
	return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
	const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
	Logf(static_cast<const RtxBackend *>(user), "Vulkan: %s\n", data->pMessage);
	return VK_FALSE;
}

void CreateMessenger(RtxBackend *s)
{
	auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(s->instance, "vkCreateDebugUtilsMessengerEXT");
	if (!create)
		return;
	VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
	ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
	ci.pfnUserCallback = OnMessage;
	ci.pUserData = s;
	create(s->instance, &ci, nullptr, &s->messenger);
	Logf(s, "RTX path tracer: Vulkan validation is on\n");
}

// ---------------------------------------------------------------- memory

// commands run at once, outside a frame; nothing is left pending afterwards
VkCommandBuffer BeginOnce(RtxBackend *s)
{
	vkQueueWaitIdle(s->queue);
	vkResetCommandBuffer(s->cmd_once, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Check(vkBeginCommandBuffer(s->cmd_once, &bi), "vkBeginCommandBuffer");
	return s->cmd_once;
}

void EndOnce(RtxBackend *s)
{
	Check(vkEndCommandBuffer(s->cmd_once), "vkEndCommandBuffer");
	VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cbi.commandBuffer = s->cmd_once;
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &cbi;
	Check(vkQueueSubmit2(s->queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");
	Check(vkQueueWaitIdle(s->queue), "vkQueueWaitIdle");
}

bool TryMemoryType(const RtxBackend *s, uint32_t bits, VkMemoryPropertyFlags want, uint32_t &type)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(s->gpu, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
	{
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
		{
			type = i;
			return true;
		}
	}
	return false;
}

void FreeBuffer(RtxBackend *s, Buffer &b)
{
	if (b.buffer)
		vkDestroyBuffer(s->device, b.buffer, nullptr);
	if (b.memory)
		vkFreeMemory(s->device, b.memory, nullptr);		// unmaps it too
	b = Buffer();
}

// A buffer the shaders and the acceleration structure builds can address.
// `mapped` ones are written from here, and sit in the card's own memory
// where it can be reached from here at all. That memory is very slow to
// read back from, so what is `read_here` is kept in ordinary memory.
Buffer MakeBuffer(RtxBackend *s, VkDeviceSize size, VkBufferUsageFlags usage, bool mapped, bool read_here = false)
{
	Buffer b;
	try
	{
		b.size = std::max<VkDeviceSize>(size, 64);
		VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bci.size = b.size;
		bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		Check(vkCreateBuffer(s->device, &bci, nullptr, &b.buffer), "vkCreateBuffer");

		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(s->device, b.buffer, &mr);
		const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		uint32_t type = 0;
		if (read_here)
		{
			if (!TryMemoryType(s, mr.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, type)
				&& !TryMemoryType(s, mr.memoryTypeBits, host, type))
				Throw("no memory the host can read");
		}
		else if (mapped)
		{
			if (!TryMemoryType(s, mr.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)
				&& !TryMemoryType(s, mr.memoryTypeBits, host, type))
				Throw("no memory the host can write");
		}
		else
			type = FindMemoryType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
		flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.pNext = &flags;
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = type;
		Check(vkAllocateMemory(s->device, &mai, nullptr, &b.memory), "vkAllocateMemory");
		Check(vkBindBufferMemory(s->device, b.buffer, b.memory, 0), "vkBindBufferMemory");
		if (mapped)
			Check(vkMapMemory(s->device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.ptr), "vkMapMemory");

		VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
		ai.buffer = b.buffer;
		b.address = vkGetBufferDeviceAddress(s->device, &ai);
	}
	catch (...)
	{
		FreeBuffer(s, b);
		throw;
	}
	return b;
}

// -------------------------------------------------------------- textures

void FreeTexture(RtxBackend *s, Texture &t)
{
	if (t.view)
		vkDestroyImageView(s->device, t.view, nullptr);
	if (t.image)
		vkDestroyImage(s->device, t.image, nullptr);
	if (t.memory)
		vkFreeMemory(s->device, t.memory, nullptr);
	t = Texture();
}

// width * height pixels of R,G,B,A bytes, onto the card
Texture MakeTexture(RtxBackend *s, int width, int height, const uint32_t *pixels)
{
	Texture t;
	Buffer staging;
	try
	{
		t.width = width;
		t.height = height;
		const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;

		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		// as stored: the shaders undo the display encoding where it is colour
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = {(uint32_t)width, (uint32_t)height, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		Check(vkCreateImage(s->device, &ici, nullptr, &t.image), "vkCreateImage");

		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(s->device, t.image, &mr);
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = FindMemoryType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		Check(vkAllocateMemory(s->device, &mai, nullptr, &t.memory), "vkAllocateMemory");
		Check(vkBindImageMemory(s->device, t.image, t.memory, 0), "vkBindImageMemory");

		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = t.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = VK_FORMAT_R8G8B8A8_UNORM;
		vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		Check(vkCreateImageView(s->device, &vci, nullptr, &t.view), "vkCreateImageView");

		staging = MakeBuffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		memcpy(staging.ptr, pixels, (size_t)bytes);

		double sum[3] = {0, 0, 0};
		for (int i = 0; i < width * height; i++)
			for (int c = 0; c < 3; c++)
				sum[c] += pt::g_to_linear[(pixels[i] >> (c * 8)) & 255];
		for (int c = 0; c < 3; c++)
			t.average[c] = std::max((float)(sum[c] / ((double)width * height)), 1e-4f);

		VkCommandBuffer cmd = BeginOnce(s);
		Barrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
		vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		Barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		EndOnce(s);
		FreeBuffer(s, staging);
	}
	catch (...)
	{
		FreeBuffer(s, staging);
		FreeTexture(s, t);
		throw;
	}
	return t;
}

// what the shaders find in a slot: its texture, or the blank one
void ShowTexture(RtxBackend *s, int slot)
{
	const Texture &t = s->textures[slot].view ? s->textures[slot] : s->blank;
	VkDescriptorImageInfo dii{VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = s->trace_dset;
	w.dstBinding = 8;
	w.dstArrayElement = (uint32_t)slot;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	w.pImageInfo = &dii;
	vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
}

// Puts a texture in a free slot. Returns the slot, or -1 if there is none
// left or nothing to put there.
int AddTexture(RtxBackend *s, const pt_texture_t *in)
{
	if (!in || !in->pixels || in->width <= 0 || in->height <= 0)
		return -1;
	for (uint32_t slot = 0; slot < kMaxTextures; slot++)
	{
		if (s->textures[slot].view)
			continue;
		s->textures[slot] = MakeTexture(s, in->width, in->height, in->pixels);	// leaves the queue idle
		ShowTexture(s, (int)slot);
		return (int)slot;
	}
	return -1;
}

void RemoveTexture(RtxBackend *s, int slot)
{
	if (slot < 0 || slot >= (int)kMaxTextures || !s->textures[slot].view)
		return;
	vkQueueWaitIdle(s->queue);
	FreeTexture(s, s->textures[slot]);
	ShowTexture(s, slot);
}

// ------------------------------------------------ acceleration structures

void FreeAccel(RtxBackend *s, Accel &a)
{
	if (a.as)
		s->DestroyAccelerationStructure(s->device, a.as, nullptr);
	FreeBuffer(s, a.storage);
	FreeBuffer(s, a.scratch);
	a = Accel();
}

// triangles given as three corners each, one after another
VkAccelerationStructureGeometryKHR TriangleGeometry(VkDeviceAddress corners, uint32_t num_tris)
{
	VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	g.geometry.triangles.vertexData.deviceAddress = corners;
	g.geometry.triangles.vertexStride = 12;
	g.geometry.triangles.maxVertex = num_tris ? num_tris * 3 - 1 : 0;
	g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
	return g;
}

VkAccelerationStructureGeometryKHR InstanceGeometry(VkDeviceAddress instances)
{
	VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	g.geometry.instances.data.deviceAddress = instances;
	return g;
}

// room for a structure over up to max_count of what the geometry describes,
// and for the working space a build of it needs
void MakeAccel(RtxBackend *s, Accel &a, VkAccelerationStructureTypeKHR type,
	const VkAccelerationStructureGeometryKHR &geometry, uint32_t max_count, VkBuildAccelerationStructureFlagsKHR flags)
{
	FreeAccel(s, a);

	VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	info.type = type;
	info.flags = flags;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
	s->GetAccelerationStructureBuildSizes(s->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &max_count, &sizes);

	a.storage = MakeBuffer(s, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
	VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
	ci.buffer = a.storage.buffer;
	ci.size = sizes.accelerationStructureSize;
	ci.type = type;
	Check(s->CreateAccelerationStructure(s->device, &ci, nullptr, &a.as), "vkCreateAccelerationStructureKHR");

	a.scratch = MakeBuffer(s, sizes.buildScratchSize + s->scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
	a.scratch_address = (a.scratch.address + s->scratch_align - 1) & ~(VkDeviceAddress)(s->scratch_align - 1);

	VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
	ai.accelerationStructure = a.as;
	a.address = s->GetAccelerationStructureDeviceAddress(s->device, &ai);
}

void BuildAccel(RtxBackend *s, VkCommandBuffer cmd, const Accel &a, VkAccelerationStructureTypeKHR type,
	const VkAccelerationStructureGeometryKHR &geometry, uint32_t count, VkBuildAccelerationStructureFlagsKHR flags)
{
	VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	info.type = type;
	info.flags = flags;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.dstAccelerationStructure = a.as;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	info.scratchData.deviceAddress = a.scratch_address;

	VkAccelerationStructureBuildRangeInfoKHR range{};
	range.primitiveCount = count;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	s->CmdBuildAccelerationStructures(cmd, 1, &info, &ranges);
}

// what was just built may be read by what comes next
void AfterBuild(VkCommandBuffer cmd, VkPipelineStageFlags2 next_stage)
{
	VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	b.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	b.dstStageMask = next_stage;
	b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &di);
}

// -------------------------------------------------------------- geometry

const VkBuildAccelerationStructureFlagsKHR kWorldBuild = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
const VkBuildAccelerationStructureFlagsKHR kFrameBuild = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;

void FreeGeometry(RtxBackend *s, Geometry &g)
{
	FreeBuffer(s, g.corners);
	FreeBuffer(s, g.tris);
	FreeBuffer(s, g.materials);
	FreeBuffer(s, g.normals);
	FreeBuffer(s, g.tangents);
	FreeBuffer(s, g.prev);
	FreeAccel(s, g.solid);
	FreeAccel(s, g.glass);
	FreeAccel(s, g.held);
	g = Geometry();
}

// Makes sure there is room for so many triangles and materials. Returns
// true if any buffer is a new one, which the shaders then have to be shown.
bool Reserve(RtxBackend *s, Geometry &g, uint32_t tris, uint32_t materials, VkBuildAccelerationStructureFlagsKHR build, bool grows)
{
	bool changed = false;

	if (!g.corners.buffer || tris > g.room_tris)
	{
		vkQueueWaitIdle(s->queue);
		FreeBuffer(s, g.corners);
		FreeBuffer(s, g.tris);
		FreeBuffer(s, g.normals);
		FreeBuffer(s, g.tangents);
		FreeBuffer(s, g.prev);
		// what changes every frame is given room to spare, so that it is not
		// made again for every triangle more
		g.room_tris = grows ? std::max(tris + tris / 2, 4096u) : std::max(tris, 1u);
		const VkDeviceSize floats = (VkDeviceSize)g.room_tris * 36;
		g.corners = MakeBuffer(s, floats,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
		g.tris = MakeBuffer(s, (VkDeviceSize)g.room_tris * sizeof(GpuTri), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		g.normals = MakeBuffer(s, floats, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		g.tangents = MakeBuffer(s, floats * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		// only what moves has a place it was before
		g.prev = MakeBuffer(s, grows ? floats : 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		// either part may turn out to be all of it
		MakeAccel(s, g.solid, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			TriangleGeometry(g.corners.address, g.room_tris), g.room_tris, build);
		MakeAccel(s, g.glass, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			TriangleGeometry(g.corners.address, g.room_tris), g.room_tris, build);
		if (grows)		// only a frame has anything carried by the eye
			MakeAccel(s, g.held, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
				TriangleGeometry(g.corners.address, g.room_tris), g.room_tris, build);
		changed = true;
	}
	if (!g.materials.buffer || materials > g.room_materials)
	{
		vkQueueWaitIdle(s->queue);
		FreeBuffer(s, g.materials);
		g.room_materials = grows ? std::max(materials * 2, 256u) : std::max(materials, 1u);
		g.materials = MakeBuffer(s, (VkDeviceSize)g.room_materials * sizeof(GpuMaterial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		changed = true;
	}
	return changed;
}

// a buffer that is at least so big; true if it is a new one
bool Room(RtxBackend *s, Buffer &b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
	if (b.buffer && b.size >= bytes)
		return false;
	vkQueueWaitIdle(s->queue);
	FreeBuffer(s, b);
	b = MakeBuffer(s, bytes + bytes / 2, usage, true);
	return true;
}

// Glass, water and the like are met from one side only: seen from behind
// they are not there. They are kept apart so that the card can do that.
bool OneSided(const pt_material_t &m)
{
	return m.alpha < 1.0f && !(m.flags & (PT_MAT_BLACK | PT_MAT_EMIT_TEXTURE));
}

float Clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// A material as the shaders read it. slot() turns the numbers the host
// gave its textures into slots; handles are slots already.
template <class F>
void SetMaterial(const RtxBackend *s, GpuMaterial &dest, const pt_material_t &in, F slot)
{
	const auto used = [&](int t) { return (t >= 0 && t < (int)kMaxTextures && s->textures[t].view) ? t : -1; };

	// put together here: where it is going is slow to read back from
	GpuMaterial out;
	memset(&out, 0, sizeof(out));
	out.texture = used(slot(in.texture));
	out.normal_texture = used(slot(in.normal_texture));
	out.emission_map = used(slot(in.emission_texture - 1));
	out.specular_map = (uint32_t)(used(slot(in.specular_texture - 1)) + 1);
	out.wave_map = used(in.wave_map - 1);			// these three are handles
	out.caustic_map = used(in.caustic_map - 1);
	out.foam_map = used(in.foam_map - 1);
	out.flags = in.flags;
	out.alpha = in.alpha;
	out.roughness = Clamp01(in.roughness);
	out.anim_next = -1;
	out.anim_length = 1;

	const bool emissive = std::max(in.emission[0], std::max(in.emission[1], in.emission[2])) > 0.0f && !(in.flags & PT_MAT_SKY);
	for (int i = 0; i < 3; i++)
	{
		out.emission[i] = in.emission[i];
		// the texture's own colour is taken out, so that a light gives off
		// what the map says it does whatever its picture
		out.emission_per_texel[i] = in.emission[i];
		// where a map says which part of the surface the light comes from, the
		// light is shared out over that part and not over the whole picture
		const int32_t shared = (out.emission_map >= 0 && (in.flags & PT_MAT_EMIT_MAPPED)) ? out.emission_map : out.texture;
		if (shared >= 0 && !(in.flags & PT_MAT_EMIT_TEXTURE))
			out.emission_per_texel[i] = in.emission[i] / s->textures[shared].average[i];
		out.absorb[i] = in.absorb[i];
	}
	out.emission[3] = in.emission_seen;
	out.emission_per_texel[3] = Clamp01(in.metallic);
	for (int i = 0; i < 4; i++)
		out.wave_rect[i] = in.wave_rect[i];
	out.scroll[0] = in.scroll[0];
	out.scroll[1] = in.scroll[1];
	if (emissive)
		out.bits |= kBitEmissive;
	// glowing detail is too dim and too patchy to be worth sampling as a light
	if (emissive && !(in.flags & PT_MAT_EMIT_BRIGHT) && (out.emission_map < 0 || (in.flags & PT_MAT_EMIT_MAPPED)))
		out.bits |= kBitSampled;
	dest = out;
}

// ------------------------------------------------------------ descriptors

void WriteBuffer(RtxBackend *s, uint32_t binding, const Buffer &b, VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
{
	if (!b.buffer)
		return;
	VkDescriptorBufferInfo info{b.buffer, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = s->trace_dset;
	w.dstBinding = binding;
	w.descriptorCount = 1;
	w.descriptorType = type;
	w.pBufferInfo = &info;
	vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
}

// shows the shaders every buffer of the map and of the frame
void ShowBuffers(RtxBackend *s)
{
	WriteBuffer(s, 1, s->frame_block, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	WriteBuffer(s, 2, s->world.corners);
	WriteBuffer(s, 3, s->world.tris);
	WriteBuffer(s, 4, s->world.materials);
	WriteBuffer(s, 5, s->frame.corners);
	WriteBuffer(s, 6, s->frame.tris);
	WriteBuffer(s, 7, s->frame.materials);
	WriteBuffer(s, 11, s->world_lights);
	WriteBuffer(s, 12, s->frame_lights);
	WriteBuffer(s, 13, s->tables);
	WriteBuffer(s, 14, s->indices);
	WriteBuffer(s, 15, s->frame.normals);
	WriteBuffer(s, 16, s->frame.prev);
	WriteBuffer(s, 25, s->meter);
	WriteBuffer(s, 36, s->world.normals);
	WriteBuffer(s, 37, s->world.tangents);
	WriteBuffer(s, 38, s->frame.tangents);
}

// ------------------------------------------------- the pictures in between

// the last pass shows the finished picture in the view
void ShowTracePicture(RtxBackend *s)
{
	const Target &picture = s->targets[kPicture];
	if (!s->dset || !picture.view)
		return;
	VkDescriptorImageInfo dii{s->trace_sampler, picture.view, VK_IMAGE_LAYOUT_GENERAL};
	VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = s->dset;
	w.dstBinding = 1;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &dii;
	vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
}

void FreeTargets(RtxBackend *s)
{
	for (Target &t : s->targets)
	{
		if (t.view) vkDestroyImageView(s->device, t.view, nullptr);
		if (t.image) vkDestroyImage(s->device, t.image, nullptr);
		if (t.memory) vkFreeMemory(s->device, t.memory, nullptr);
		t = Target();
	}
}

// Everything kept per pixel: most of it at the size the picture is traced
// at, the finished picture and its history at the size of the view. They
// stay in the one layout all passes can use.
void MakeTargets(RtxBackend *s, int width, int height, int out_width, int out_height)
{
	vkQueueWaitIdle(s->queue);
	FreeTargets(s);
	s->trace_width = width;
	s->trace_height = height;
	s->out_width = out_width;
	s->out_height = out_height;
	s->has_history = false;

	for (int i = 0; i < kNumTargets; i++)
	{
		Target &t = s->targets[i];
		const bool positions = i == kSeen || i == kMirror || i == kMirror + 1 || i == kOver;
		// The light gathered over frames is a running average, each frame
		// moving it by a small part of the difference. At half precision
		// the smallest of those steps round away and the others do not, and
		// light is lost: so these are kept at full precision.
		const bool gathered = i >= kKept && i < kKept + 6;
		const bool moments = i >= kMoments && i < kMoments + 6;
		const bool flash = i >= kFlash && i < kFlash + 2;
		// one value for each block of 8 by 8 pixels
		const bool blocks = i >= kChange && i < kChange + 4;
		// the export planes carry indices, which must come back whole
		const bool exported = i >= kExport && i < kExport + 4;
		const VkFormat format = (positions || gathered || blocks || exported) ? VK_FORMAT_R32G32B32A32_SFLOAT
			: moments ? VK_FORMAT_R32G32_SFLOAT : flash ? VK_FORMAT_R32_SFLOAT
			: (i == kPicture ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT);

		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = format;
		const bool full = i == kPicture || i == kSteady || i == kSteady + 1;
		ici.extent = {(uint32_t)(full ? out_width : width), (uint32_t)(full ? out_height : height), 1};
		if (blocks)
			ici.extent = {((uint32_t)width + 7) / 8, ((uint32_t)height + 7) / 8, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		Check(vkCreateImage(s->device, &ici, nullptr, &t.image), "vkCreateImage");

		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(s->device, t.image, &mr);
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = FindMemoryType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		Check(vkAllocateMemory(s->device, &mai, nullptr, &t.memory), "vkAllocateMemory");
		Check(vkBindImageMemory(s->device, t.image, t.memory, 0), "vkBindImageMemory");

		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = t.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = format;
		vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		Check(vkCreateImageView(s->device, &vci, nullptr, &t.view), "vkCreateImageView");
	}

	VkCommandBuffer cmd = BeginOnce(s);
	for (const Target &t : s->targets)
		Barrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	EndOnce(s);

	// which pictures go in which binding, in the order of the Target list
	const struct { uint32_t binding, first, count; } groups[] = {
		{17, kSurface, 2}, {18, kSeen, 1}, {19, kAlbedo, 2}, {20, kNoisy, 3},
		{21, kExtra, 1}, {22, kKept, 6}, {23, kFilter, 6}, {24, kPicture, 1},
		{26, kHdr, 1}, {27, kBloom, 2}, {28, kSteady, 2}, {29, kGraded, 1},
		{30, kMirror, 2}, {31, kOver, 1}, {32, kMoments, 6}, {33, kFlash, 2},
		{34, kChange, 4}, {35, kExport, 4},
	};
	VkDescriptorImageInfo info[kNumTargets];
	for (const auto &g : groups)
	{
		for (uint32_t i = 0; i < g.count; i++)
			info[i] = {VK_NULL_HANDLE, s->targets[g.first + i].view, VK_IMAGE_LAYOUT_GENERAL};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = s->trace_dset;
		w.dstBinding = g.binding;
		w.descriptorCount = g.count;
		w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w.pImageInfo = info;
		vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
	}
	ShowTracePicture(s);
}

// ------------------------------------------------------------------ scene

VkPipeline MakeComputePipeline(RtxBackend *s, const uint32_t *code, size_t bytes)
{
	VkShaderModule module = CreateShader(s, code, bytes);
	VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpci.stage.module = module;
	cpci.stage.pName = "main";
	cpci.layout = s->trace_playout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result = vkCreateComputePipelines(s->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
	vkDestroyShaderModule(s->device, module, nullptr);
	Check(result, "vkCreateComputePipelines");
	return pipeline;
}

// everything the tracer needs that does not depend on the map
void CreateScene(RtxBackend *s)
{
	pt::InitColourTables();

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = VK_FILTER_NEAREST;
	sci.minFilter = VK_FILTER_NEAREST;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	Check(vkCreateSampler(s->device, &sci, nullptr, &s->tex_sampler), "vkCreateSampler");

	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	Check(vkCreateSampler(s->device, &sci, nullptr, &s->smooth_sampler), "vkCreateSampler");

	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	Check(vkCreateSampler(s->device, &sci, nullptr, &s->trace_sampler), "vkCreateSampler");

	// see scene.glsl for what each binding is
	VkDescriptorSetLayoutBinding bind[kNumBindings]{};
	for (uint32_t i = 0; i < kNumBindings; i++)
	{
		bind[i].binding = i;
		bind[i].descriptorCount = 1;
		bind[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		bind[i].descriptorType = i >= 17 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	}
	bind[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bind[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bind[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	bind[8].descriptorCount = kMaxTextures;
	bind[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	bind[10].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	bind[17].descriptorCount = 2;
	bind[19].descriptorCount = 2;
	bind[20].descriptorCount = 3;
	bind[22].descriptorCount = 6;
	bind[23].descriptorCount = 6;
	bind[25].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bind[27].descriptorCount = 2;
	bind[28].descriptorCount = 2;
	bind[30].descriptorCount = 2;
	bind[32].descriptorCount = 6;
	bind[33].descriptorCount = 2;
	bind[34].descriptorCount = 4;
	bind[35].descriptorCount = 4;
	bind[36].descriptorType = bind[37].descriptorType = bind[38].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

	VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dlci.bindingCount = kNumBindings;
	dlci.pBindings = bind;
	Check(vkCreateDescriptorSetLayout(s->device, &dlci, nullptr, &s->trace_dsl), "vkCreateDescriptorSetLayout");

	const VkDescriptorPoolSize sizes[] = {
		{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 24},
		{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures},
		{VK_DESCRIPTOR_TYPE_SAMPLER, 2},
		{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kNumTargets},
	};
	VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpci.maxSets = 1;
	dpci.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
	dpci.pPoolSizes = sizes;
	Check(vkCreateDescriptorPool(s->device, &dpci, nullptr, &s->trace_dpool), "vkCreateDescriptorPool");

	VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dsai.descriptorPool = s->trace_dpool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &s->trace_dsl;
	Check(vkAllocateDescriptorSets(s->device, &dsai, &s->trace_dset), "vkAllocateDescriptorSets");

	// each pass may be told a few numbers of its own
	VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PassPush)};
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &s->trace_dsl;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	Check(vkCreatePipelineLayout(s->device, &plci, nullptr, &s->trace_playout), "vkCreatePipelineLayout");

	s->trace_pipeline = MakeComputePipeline(s, trace_comp_spv, sizeof(trace_comp_spv));
	s->temporal_pipeline = MakeComputePipeline(s, temporal_comp_spv, sizeof(temporal_comp_spv));
	s->change_pipeline = MakeComputePipeline(s, change_comp_spv, sizeof(change_comp_spv));
	s->atrous_pipeline = MakeComputePipeline(s, atrous_comp_spv, sizeof(atrous_comp_spv));
	s->compose_pipeline = MakeComputePipeline(s, compose_comp_spv, sizeof(compose_comp_spv));
	s->bloom_pipeline = MakeComputePipeline(s, bloom_comp_spv, sizeof(bloom_comp_spv));
	s->resolve_pipeline = MakeComputePipeline(s, resolve_comp_spv, sizeof(resolve_comp_spv));
	s->grade_pipeline = MakeComputePipeline(s, grade_comp_spv, sizeof(grade_comp_spv));

	// every texture slot shows something from the start
	const uint32_t white = 0xffffffffu;
	s->blank = MakeTexture(s, 1, 1, &white);
	{
		std::vector<VkDescriptorImageInfo> all(kMaxTextures,
			VkDescriptorImageInfo{VK_NULL_HANDLE, s->blank.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = s->trace_dset;
		w.dstBinding = 8;
		w.descriptorCount = kMaxTextures;
		w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		w.pImageInfo = all.data();
		vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);

		const VkDescriptorImageInfo samplers[2] = {
			{s->tex_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
			{s->smooth_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
		};
		for (uint32_t i = 0; i < 2; i++)
		{
			VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			ws.dstSet = s->trace_dset;
			ws.dstBinding = 9 + i;
			ws.descriptorCount = 1;
			ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
			ws.pImageInfo = &samplers[i];
			vkUpdateDescriptorSets(s->device, 1, &ws, 0, nullptr);
		}
	}

	// the instances: the map, its glass, what moves, its glass, what the eye carries
	s->instances = MakeBuffer(s, kNumInstances * sizeof(VkAccelerationStructureInstanceKHR),
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
	MakeAccel(s, s->tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
		InstanceGeometry(s->instances.address), kNumInstances, kFrameBuild);
	{
		VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
		as.accelerationStructureCount = 1;
		as.pAccelerationStructures = &s->tlas.as;
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.pNext = &as;
		w.dstSet = s->trace_dset;
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
	}

	// something in every binding before there is a map
	s->frame_block = MakeBuffer(s, sizeof(FrameBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
	s->world_lights = MakeBuffer(s, sizeof(GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	s->frame_lights = MakeBuffer(s, 64 * sizeof(GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	s->tables = MakeBuffer(s, kNumStyles * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	s->indices = MakeBuffer(s, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	s->meter = MakeBuffer(s, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
		| VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	s->meter_read = MakeBuffer(s, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
	memset(s->meter_read.ptr, 0, 256);
	memset(s->meter_was, 0, sizeof(s->meter_was));
	{
		VkCommandBuffer cmd = BeginOnce(s);
		vkCmdFillBuffer(cmd, s->meter.buffer, 0, VK_WHOLE_SIZE, 0);
		EndOnce(s);
	}
	Reserve(s, s->world, 1, 1, kWorldBuild, false);
	Reserve(s, s->frame, 1, 1, kFrameBuild, true);
	ShowBuffers(s);
	MakeTargets(s, s->width, s->height, s->width, s->height);

	// The card times the parts of the tracing itself, if its clock can be
	// read from the queue the work is done on.
	{
		uint32_t n = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(s->gpu, &n, nullptr);
		std::vector<VkQueueFamilyProperties> families(n);
		vkGetPhysicalDeviceQueueFamilyProperties(s->gpu, &n, families.data());
		const uint32_t bits = s->queue_family < n ? families[s->queue_family].timestampValidBits : 0;
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(s->gpu, &props);

		if (bits && props.limits.timestampPeriod > 0.0f)
		{
			VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
			qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
			qpci.queryCount = kNumQueries;
			Check(vkCreateQueryPool(s->device, &qpci, nullptr, &s->stamps), "vkCreateQueryPool");
			s->stamp_ms = (double)props.limits.timestampPeriod * 1.0e-6;	// it is given in nanoseconds
			s->stamp_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;

			// nothing may be read from a query that was never reset
			VkCommandBuffer cmd = BeginOnce(s);
			vkCmdResetQueryPool(cmd, s->stamps, 0, kNumQueries);
			EndOnce(s);
		}
	}
}

void DestroyScene(RtxBackend *s)
{
	if (s->stamps)
		vkDestroyQueryPool(s->device, s->stamps, nullptr);
	FreeGeometry(s, s->world);
	FreeGeometry(s, s->frame);
	FreeAccel(s, s->tlas);
	FreeBuffer(s, s->instances);
	FreeBuffer(s, s->frame_block);
	FreeBuffer(s, s->world_lights);
	FreeBuffer(s, s->frame_lights);
	FreeBuffer(s, s->tables);
	FreeBuffer(s, s->indices);
	FreeBuffer(s, s->meter);
	FreeBuffer(s, s->meter_read);
	FreeBuffer(s, s->updates);
	FreeBuffer(s, s->readback);
	for (Texture &t : s->textures)
		FreeTexture(s, t);
	FreeTexture(s, s->blank);
	FreeTargets(s);
	for (VkPipeline p : {s->trace_pipeline, s->temporal_pipeline, s->atrous_pipeline, s->compose_pipeline,
		s->bloom_pipeline, s->resolve_pipeline, s->grade_pipeline, s->change_pipeline})
		if (p)
			vkDestroyPipeline(s->device, p, nullptr);
	if (s->trace_playout) vkDestroyPipelineLayout(s->device, s->trace_playout, nullptr);
	if (s->trace_dpool) vkDestroyDescriptorPool(s->device, s->trace_dpool, nullptr);
	if (s->trace_dsl) vkDestroyDescriptorSetLayout(s->device, s->trace_dsl, nullptr);
	if (s->trace_sampler) vkDestroySampler(s->device, s->trace_sampler, nullptr);
	if (s->smooth_sampler) vkDestroySampler(s->device, s->smooth_sampler, nullptr);
	if (s->tex_sampler) vkDestroySampler(s->device, s->tex_sampler, nullptr);
}

// For picking one of count things by chance, each as often as its share
// says, with no search however many there are (Walker's alias method, built
// Vose's way): a place among them is drawn evenly, and the thing there is
// kept with the chance keep has for it, or else the one that other names
// is taken in its stead. Each thing's whole chance, as itself and as the
// stand-in of others, comes to its share.
void PickingTable(const float *share, size_t count, float *keep, uint32_t *other)
{
	// each one's share of a place, in which 1 is all of it
	double sum = 0.0;
	for (size_t i = 0; i < count; i++)
		sum += share[i] > 0.0f ? share[i] : 0.0;
	std::vector<double> has(count);
	std::vector<uint32_t> under, over;
	for (size_t i = 0; i < count; i++)
	{
		has[i] = sum > 0.0 ? (share[i] > 0.0f ? share[i] : 0.0) * (double)count / sum : 1.0;
		keep[i] = 1.0f;
		other[i] = (uint32_t)i;
		(has[i] < 1.0 ? under : over).push_back((uint32_t)i);
	}
	// one with less than a place to itself has the rest of its place filled
	// by one with more, which is left with that much less
	while (!under.empty() && !over.empty())
	{
		const uint32_t less = under.back(), more = over.back();
		under.pop_back();
		keep[less] = (float)has[less];
		other[less] = more;
		has[more] -= 1.0 - has[less];
		if (has[more] < 1.0)
		{
			over.pop_back();
			under.push_back(more);
		}
	}
	// (what is left on either side has a place to itself, give or take the rounding)
}

void LoadWorldNow(RtxBackend *s, const pt_world_t *in)
{
	vkQueueWaitIdle(s->queue);

	// the map before this one
	s->world_loaded = false;
	s->has_history = false;
	for (int slot : s->world_textures)
		if (slot >= 0)
		{
			FreeTexture(s, s->textures[slot]);
			ShowTexture(s, slot);
		}
	s->world_textures.clear();
	for (int32_t &face : s->sky)
		face = -1;

	if (!in || in->num_triangles <= 0)
		return;

	s->world_textures.assign(in->num_textures, -1);
	for (int i = 0; i < in->num_textures; i++)
		s->world_textures[i] = AddTexture(s, &in->textures[i]);
	const auto slot = [&](int texture)
	{
		return (texture >= 0 && texture < in->num_textures) ? s->world_textures[texture] : -1;
	};

	const uint32_t num_tris = (uint32_t)in->num_triangles;
	Reserve(s, s->world, num_tris, (uint32_t)std::max(in->num_materials, 1), kWorldBuild, false);

	// solid triangles first, then the glass; lights are told where theirs went
	const auto material_of = [&](uint32_t t)
	{
		return (in->tri_materials && (int)in->tri_materials[t] < in->num_materials) ? in->tri_materials[t] : 0u;
	};
	std::vector<uint32_t> place(num_tris);
	uint32_t num_solid = 0;
	for (uint32_t t = 0; t < num_tris; t++)
		if (in->num_materials <= 0 || !OneSided(in->materials[material_of(t)]))
			place[t] = num_solid++;
	uint32_t next = num_solid;
	for (uint32_t t = 0; t < num_tris; t++)
		if (in->num_materials > 0 && OneSided(in->materials[material_of(t)]))
			place[t] = next++;

	float *corners = static_cast<float *>(s->world.corners.ptr);
	float *normals = static_cast<float *>(s->world.normals.ptr);
	float *tangents = static_cast<float *>(s->world.tangents.ptr);
	s->world_smooth = (in->normals ? 1 : 0) | (in->tangents ? 2 : 0);
	GpuTri *tris = static_cast<GpuTri *>(s->world.tris.ptr);
	for (uint32_t t = 0; t < num_tris; t++)
	{
		GpuTri &out = tris[place[t]];
		out.pad = 0;
		out.material = material_of(t);
		for (int k = 0; k < 3; k++)
		{
			const uint32_t v = in->indices[t * 3 + k];
			memcpy(&corners[place[t] * 9 + k * 3], &in->positions[v * 3], 12);
			if (in->normals)
				memcpy(&normals[place[t] * 9 + k * 3], &in->normals[v * 3], 12);
			if (in->tangents)
				memcpy(&tangents[place[t] * 18 + k * 6], &in->tangents[v * 6], 24);
			out.uv[k * 2] = in->uvs ? in->uvs[v * 2] : 0.0f;
			out.uv[k * 2 + 1] = in->uvs ? in->uvs[v * 2 + 1] : 0.0f;
		}
	}

	GpuMaterial *materials = static_cast<GpuMaterial *>(s->world.materials.ptr);
	if (in->num_materials <= 0)
	{
		const pt_material_t plain{};
		SetMaterial(s, materials[0], plain, [](int) { return -1; });
		materials[0].alpha = 1.0f;
		materials[0].roughness = 1.0f;
	}
	s->world_has_waves = false;
	for (int i = 0; i < in->num_materials; i++)
	{
		SetMaterial(s, materials[i], in->materials[i], slot);
		const int then = in->materials[i].anim_next;
		if (then >= 0 && then < in->num_materials && then != i)
			materials[i].anim_next = then;
		if (in->materials[i].flags & PT_MAT_WAVES)
			s->world_has_waves = true;
	}
	for (int i = 0; i < in->num_materials; i++)
	{
		// animations loop; count the steps until this one comes round again
		int length = 1;
		for (int n = materials[i].anim_next; n >= 0 && n != i && length < 64; n = materials[n].anim_next)
			length++;
		materials[i].anim_length = length;
	}

	// The lights, where to look for them from each part of the map, and the
	// sky as a light: worked out by the same code the CPU backend uses.
	const std::unique_ptr<pt::World> w = pt::BuildWorld(in);
	const pt::LightGrid &grid = w->grid;
	const size_t num_lights = w->lights.size();
	const size_t cells = grid.count.size();
	const size_t sky_cells = w->sky_cdf.size();

	Room(s, s->world_lights, std::max<size_t>(num_lights, 1) * sizeof(GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	GpuLight *lights = static_cast<GpuLight *>(s->world_lights.ptr);
	for (size_t i = 0; i < num_lights; i++)
	{
		const pt::Light &l = w->lights[i];
		GpuLight &out = lights[i];
		memset(&out, 0, sizeof(out));
		out.pdf = l.pdf;
		out.cone_cos = l.cone_cos;
		for (int a = 0; a < 3; a++)
			out.emission[a] = l.emission[a];
		if (l.tri == ~0u)
		{
			out.tri = kPointLight | (uint32_t)((l.style > 0 && l.style < 256) ? l.style : 0);
			for (int a = 0; a < 3; a++)
			{
				out.origin[a] = l.origin[a];
				out.dir[a] = l.dir[a];
			}
			out.edge[0] = l.reach;		// a point has no edge: its place holds how far it reaches
			out.edge[1] = l.core;		// and the least distance it is taken to be at
			for (int a = 0; a < 3; a++)
			{
				out.box_origin[a] = l.box_origin[a];
				out.box_size[a] = l.box_size[a];
			}
			out.no_shadow = l.no_shadow ? 1.0f : 0.0f;
		}
		else
		{
			// A point is drawn on it for every light weighed, which is most
			// of what weighing costs: its corners are here with the rest of
			// it, not to be looked up among the map's.
			out.tri = place[l.tri];
			const float *corner[3];
			for (int k = 0; k < 3; k++)
				corner[k] = &in->positions[in->indices[l.tri * 3 + k] * 3];
			for (int a = 0; a < 3; a++)
			{
				out.origin[a] = corner[0][a];
				out.dir[a] = corner[1][a] - corner[0][a];
				out.edge[a] = corner[2][a] - corner[0][a];
			}
		}
	}

	// tables: the light styles, then whatever the frame block says is where
	s->table_light_keep = kNumStyles;
	s->table_grid_pdf = s->table_light_keep + (int)num_lights;
	s->table_grid_keep = s->table_grid_pdf + (int)grid.pdf.size();
	s->table_sky_chance = s->table_grid_keep + (int)grid.cdf.size();
	s->table_sky_cdf = s->table_sky_chance + (int)w->sky_chance.size();
	const size_t num_floats = (size_t)s->table_sky_cdf + sky_cells;
	Room(s, s->tables, num_floats * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	float *tables = static_cast<float *>(s->tables.ptr);
	for (int i = 0; i < kNumStyles; i++)
		tables[i] = 1.0f;
	const auto put = [&](int at, const std::vector<float> &v)
	{
		if (!v.empty())
			memcpy(&tables[at], v.data(), v.size() * sizeof(float));
	};
	put(s->table_grid_pdf, grid.pdf);
	put(s->table_sky_chance, w->sky_chance);
	put(s->table_sky_cdf, w->sky_cdf);

	// The shaders weigh many lights at every point they light, and each is
	// first picked by chance: from the whole map by its light, or from the
	// list of the cell the point is in. The CPU backend finds the one a
	// number drawn stands for by halving a table of the chances added up,
	// a dozen looks for the map and five for a cell. Pixels are traced on
	// the card in step with their neighbours, each waiting out every look
	// any of them has to take, so the shaders pick from tables that take one
	// look whatever the count: see PickingTable. The chances are the same.
	const int per_cell = pt::LightGrid::kPerCell;
	static_assert(per_cell <= 32, "a place in a cell's list has five bits");
	s->index_grid_light = 0;
	s->index_grid_count = (int)grid.light.size();
	s->index_light_other = s->index_grid_count + (int)cells;
	s->index_grid_sorted = s->index_light_other + (int)num_lights;
	Room(s, s->indices, std::max<size_t>(grid.light.size() * 2 + cells + num_lights, 1) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	uint32_t *index = static_cast<uint32_t *>(s->indices.ptr);
	{
		std::vector<float> share(num_lights), keep(std::max<size_t>(num_lights, per_cell));
		std::vector<uint32_t> other(std::max<size_t>(num_lights, per_cell));
		for (size_t i = 0; i < num_lights; i++)
			share[i] = w->lights[i].pdf;
		PickingTable(share.data(), num_lights, keep.data(), other.data());
		if (num_lights)
		{
			memcpy(&tables[s->table_light_keep], keep.data(), num_lights * sizeof(float));
			memcpy(&index[s->index_light_other], other.data(), num_lights * sizeof(uint32_t));
		}

		// A cell's list: each light with, in its low five bits, the place in
		// the list of the one that stands in for it. And the list again in
		// order of light, each with its own place in its low bits: a light
		// picked map wide may be in the cell's list as well, what chance it
		// had of being picked from there has to be known, and going down the
		// list for it took a look at every entry.
		for (size_t c = 0; c < cells; c++)
		{
			const size_t first = c * per_cell;
			const int count = grid.count[c];
			uint32_t entry[pt::LightGrid::kPerCell] = {}, sorted[pt::LightGrid::kPerCell];
			PickingTable(&grid.pdf[first], count, keep.data(), other.data());
			for (int j = 0; j < per_cell; j++)
			{
				if (j >= count)
					keep[j] = 1.0f;
				else
					entry[j] = (grid.light[first + j] << 5) | other[j];
				sorted[j] = j < count ? (grid.light[first + j] << 5) | (uint32_t)j : ~0u;
			}
			std::sort(sorted, sorted + count);
			memcpy(&tables[s->table_grid_keep + first], keep.data(), per_cell * sizeof(float));
			memcpy(&index[s->index_grid_light + first], entry, sizeof(entry));
			memcpy(&index[s->index_grid_sorted + first], sorted, sizeof(sorted));
			index[s->index_grid_count + c] = (uint32_t)count;
		}
	}

	s->num_world_lights = (int)num_lights;
	s->light_given.assign(in->num_lights > 0 ? in->num_lights : 0, -1);
	s->light_base.resize(num_lights * 3);
	for (size_t i = 0; i < num_lights; i++)
	{
		const pt::Light &l = w->lights[i];
		for (int a = 0; a < 3; a++)
			s->light_base[i * 3 + a] = l.emission[a];
		if (l.tri == ~0u && l.given >= 0 && l.given < (int)s->light_given.size())
			s->light_given[l.given] = (int)i;
	}
	s->lights_scaled = false;
	s->has_grid = cells > 0 && w->sky_chance.size() == cells;
	for (int a = 0; a < 3; a++)
	{
		s->grid_origin[a] = grid.origin[a];
		s->grid_dims[a] = grid.dims[a];
	}
	s->grid_inv_cell = grid.inv_cell;
	s->num_waters = 0;
	for (const pt::World::Water &body : w->waters)
	{
		if (s->num_waters == kMaxWaters)
			break;
		float *rect = s->water_rect[s->num_waters], *at = s->water_at[s->num_waters];
		rect[0] = body.min_x;
		rect[1] = body.min_y;
		rect[2] = body.max_x;
		rect[3] = body.max_y;
		at[0] = body.z;
		at[1] = body.top ? (float)(body.mat - w->materials.data()) : -1.0f;
		// as SetMaterial has them
		const auto used = [&](int t) { return (t >= 0 && t < (int)kMaxTextures && s->textures[t].view) ? t : -1; };
		at[2] = (float)used(body.mat->wave_map - 1);
		at[3] = (float)used(body.mat->caustic_map - 1);
		for (int i = 0; i < 4; i++)
			s->water_wave[s->num_waters][i] = body.mat->wave_rect[i];
		static_assert(pt::World::Water::kLamps == 4, "the shaders take four by four");
		for (int k = 0; k < 16; k++)
			for (int i = 0; i < 3; i++)
				s->water_lamp[s->num_waters * 16 + k][i] = body.lamps[k][i];
		// whatever shows this body's waves is met where they stand: the
		// shaders look for that surface in the bodies listed here
		for (int i = 0; i < in->num_materials; i++)
			if (in->materials[i].wave_map == body.mat->wave_map && at[2] >= 0.0f)
				materials[i].bits |= kBitSwell;
		s->num_waters++;
	}
	s->sky_res = sky_cells ? w->sky_res : 0;
	s->sky_total = w->sky_total;
	s->sky_scale = w->sky_scale;
	ShowBuffers(s);

	s->world.num_solid = num_solid;
	s->world.num_glass = num_tris - num_solid;
	VkCommandBuffer cmd = BeginOnce(s);
	if (s->world.num_solid)
		BuildAccel(s, cmd, s->world.solid, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			TriangleGeometry(s->world.corners.address, s->world.num_solid), s->world.num_solid, kWorldBuild);
	if (s->world.num_glass)
		BuildAccel(s, cmd, s->world.glass, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			TriangleGeometry(s->world.corners.address + (VkDeviceAddress)s->world.num_solid * 36, s->world.num_glass),
			s->world.num_glass, kWorldBuild);
	EndOnce(s);

	for (int f = 0; f < 6; f++)
		s->sky[f] = slot(in->sky_textures[f]);
	s->world_loaded = true;
	Logf(s, "RTX path tracer: %d triangles (%u of them glass or liquid), %d materials, %d textures, %d lights on the card\n",
		in->num_triangles, s->world.num_glass, in->num_materials, in->num_textures, (int)num_lights);
}

// Only ever compared with the hash of the frame before, to tell whether
// anything has changed. Every triangle that moves goes through here every
// frame, so it takes eight bytes at a step and four steps side by side: a
// byte at a time this took longer than copying the triangles to the card.
uint32_t HashBytes(const void *data, size_t bytes, uint32_t h)
{
	const uint8_t *p = static_cast<const uint8_t *>(data);
	const uint64_t k = 0x9E3779B97F4A7C15ull;
	uint64_t lane[4] = {h ^ k, k * 3, k * 5, (uint64_t)bytes * k};
	size_t i = 0;
	for (; i + 32 <= bytes; i += 32)
	{
		uint64_t w[4];
		memcpy(w, p + i, 32);
		for (int a = 0; a < 4; a++)
		{
			lane[a] = (lane[a] ^ w[a]) * k;
			lane[a] ^= lane[a] >> 29;
		}
	}
	for (; i < bytes; i++)
	{
		lane[0] = (lane[0] ^ p[i]) * k;
		lane[0] ^= lane[0] >> 29;
	}
	uint64_t all = lane[0];
	for (int a = 1; a < 4; a++)
	{
		all = (all ^ lane[a]) * k;
		all ^= all >> 29;
	}
	return (uint32_t)(all ^ (all >> 32));
}

// The eye adapts. What it goes by is a reading of how bright what is seen
// is, counted on the last frame's picture (see compose.comp):
//
// - the light that falls there, with half a say for how dark the surface is
//   painted: going by the picture alone, a room of dark metal in good light
//   was taken for a dim one and shown as bright as a white room;
// - the middle of the count, without its darkest and its brightest parts: a
//   black doorway, a lamp or a screen does not move it;
// - a little of the way only: a place a sixteenth as bright as most is shown
//   a little over twice as bright, not sixteen times. A dark place is to stay
//   dark, as the game drew it, which did not adapt at all;
// - within two stops either way;
// - quickly into the light and slowly into the dark, as an eye does.
//
// The numbers are from ten places in seven of the game's maps, set so that
// on the whole the picture is as bright as the game's own renderer draws
// them, and comes as near it in each as one rule can: within a stop.
const int kMeterBins = 64;				// a quarter of a stop each, from 2^-14
const float kLightTypical = 0.043f;		// the reading in a place lit as most are,
const float kExposureTypical = 0.40f;	// and the exposure it is shown at
const float kMeterFrom = 0.30f, kMeterTo = 0.90f;	// the part of the count that is gone by
const float kCorrection = 0.3f;			// how much of the difference is made up, in stops
const float kExposureMin = 0.25f * kExposureTypical, kExposureMax = 4.0f * kExposureTypical;
const float kRateDown = 3.0f, kRateUp = 0.4f;		// per second: into the light, into the dark

void AdaptExposure(RtxBackend *s, const pt_view_t *view)
{
	// what the views traced since the last look added to the counts
	const uint32_t *meter = static_cast<const uint32_t *>(s->meter_read.ptr);
	uint32_t bins[kMeterBins];
	uint32_t total = 0;
	for (int i = 0; i < kMeterBins; i++)
	{
		bins[i] = meter[i] - s->meter_was[i];
		s->meter_was[i] = meter[i];
		total += bins[i];
	}
	if (total < 64)
		return;		// nearly all sky, or nothing drawn

	// the mean, in stops, of the readings between the two marks
	const double from = (double)total * kMeterFrom, to = (double)total * kMeterTo;
	double before = 0.0, sum = 0.0;
	for (int i = 0; i < kMeterBins; i++)
	{
		const double after = before + bins[i];
		const double part = std::min(after, to) - std::max(before, from);
		if (part > 0.0)
			sum += part * ((i + 0.5) * 0.25 - 14.0);
		before = after;
	}
	const float typical = std::exp2((float)(sum / (to - from)));
	s->metered = typical;

	const float want = std::min(kExposureMax, std::max(kExposureMin, kExposureTypical * std::pow(kLightTypical / typical, kCorrection)));
	const float dt = view->time - s->prev_time;
	if (!s->have_exposure || dt < 0.0f || dt > 1.0f)
		s->auto_exposure = want;
	else
		s->auto_exposure += (want - s->auto_exposure) * (1.0f - std::exp(-dt * (want < s->auto_exposure ? kRateDown : kRateUp)));
	s->have_exposure = true;
}

void RecordUpdates(RtxBackend *s, VkCommandBuffer cmd);
void RecordTrace(RtxBackend *s, VkCommandBuffer cmd);

// Traces a view that is waiting to be, at once instead of as part of the
// frame that shows it: when its picture is wanted before then, or another
// view is about to take its place (the passes of a screenshot).
void TraceNow(RtxBackend *s)
{
	if (!s->trace_pending)
		return;
	VkCommandBuffer cmd = BeginOnce(s);
	if (!s->pending.empty())
		RecordUpdates(s, cmd);
	RecordTrace(s, cmd);
	EndOnce(s);
	s->trace_pending = false;
}

// Sends the tracing of the view just described to the card at once, without
// waiting for the frame that will show it. The card can then be tracing
// while the game draws its status bar and menus and the frame's own commands
// are put together; left until then, it sat idle for as long as that took,
// every frame. The frame's commands follow on the same queue, so they find
// the picture finished.
void SubmitTrace(RtxBackend *s)
{
	if (!s->trace_pending)
		return;
	LapStart(s);
	vkWaitForFences(s->device, 1, &s->fence_trace, VK_TRUE, UINT64_MAX);
	Lap(s, "wait card", 2);
	vkResetCommandBuffer(s->cmd_trace, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Check(vkBeginCommandBuffer(s->cmd_trace, &bi), "vkBeginCommandBuffer");
	if (!s->pending.empty())
		RecordUpdates(s, s->cmd_trace);
	RecordTrace(s, s->cmd_trace);
	Check(vkEndCommandBuffer(s->cmd_trace), "vkEndCommandBuffer");
	Lap(s, "record");

	VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cbi.commandBuffer = s->cmd_trace;
	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &cbi;
	vkResetFences(s->device, 1, &s->fence_trace);
	s->trace_pending = false;
	Check(vkQueueSubmit2(s->queue, 1, &si, s->fence_trace), "vkQueueSubmit2");
	Lap(s, "submit");
}

// the card's own times for the view it traced last, once it has finished it
void ReadStamps(RtxBackend *s)
{
	if (!s->stamps)
		return;
#ifdef PT_PERF
	if (s->show_asked)
	{	// the frame's own commands, which may not be done yet: then next time
		uint64_t round[2];
		if (vkGetQueryPoolResults(s->device, s->stamps, kMaxStamps, 2, sizeof(round), round, sizeof(round[0]),
			VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
		{
			s->show_ms = (float)((double)((round[1] - round[0]) & s->stamp_mask) * s->stamp_ms);
			s->show_asked = false;
		}
	}
#endif
	if (!s->stamps_asked)
		return;
	uint64_t at[kMaxStamps];
	const uint32_t count = s->stamps_written;
	if (count < 2 || vkGetQueryPoolResults(s->device, s->stamps, 0, count, sizeof(at[0]) * count, at, sizeof(at[0]),
		VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
		return;		// not all there yet
	s->stamps_asked = false;

	s->stage_ms[0] = s->scene_ms;
	for (int i = 1; i < kNumStages; i++)
		s->stage_ms[i] = 0.0f;
#ifdef PT_PERF
	s->num_card = 0;
#endif
	for (uint32_t i = 0; i + 1 < count; i++)
	{
		const float ms = (float)((double)((at[i + 1] - at[i]) & s->stamp_mask) * s->stamp_ms);
		s->stage_ms[s->stamp_stage[i]] += ms;
#ifdef PT_PERF
		s->card[s->num_card++] = {s->stamp_name[i], ms, 1};
#endif
	}
	s->stages_known = true;
	s->stages_new = true;
}

// what moves this frame, and the view: all the tracer needs to be told
void RenderViewNow(RtxBackend *s, const pt_view_t *view)
{
	LapStart(s);
	TraceNow(s);
	s->view = *view;
	s->has_view = true;
	s->trace_ready = false;
	s->view_rect[0] = view->x;
	s->view_rect[1] = view->y;
	s->view_rect[2] = view->width;
	s->view_rect[3] = view->height;
	s->stats[0] = 0;
	if (!s->world_loaded || view->width <= 0 || view->height <= 0)
		return;

	// the card may still be tracing the last view from these buffers
	vkWaitForFences(s->device, 1, &s->fence_trace, VK_TRUE, UINT64_MAX);
	ReadStamps(s);		// and once it is done, how long that frame took it is known
	Lap(s, "wait card", 2);
	const auto began = std::chrono::steady_clock::now();

	const float scale = view->scale < 0.05f ? 0.05f : (view->scale > 1.0f ? 1.0f : view->scale);
	const int rw = std::max(1, (int)(view->width * scale + 0.5f));
	const int rh = std::max(1, (int)(view->height * scale + 0.5f));
	if (rw != s->trace_width || rh != s->trace_height || view->width != s->out_width || view->height != s->out_height)
		MakeTargets(s, rw, rh, view->width, view->height);
	if (view->restart)
		s->has_history = false;

	// ---- what moves
	const pt_scene_t *scene = view->scene;
	const uint32_t n = (scene && scene->positions && scene->num_triangles > 0) ? (uint32_t)scene->num_triangles : 0;
	const uint32_t num_materials = (scene && scene->num_materials > 0) ? (uint32_t)scene->num_materials : 0;
	const uint32_t num_lights = (scene && scene->lights && scene->num_lights > 0) ? (uint32_t)scene->num_lights : 0;
	bool changed = Reserve(s, s->frame, n, std::max(num_materials, 1u), kFrameBuild, true);
	changed |= Room(s, s->frame_lights, std::max(num_lights, 1u) * sizeof(GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	if (changed)
		ShowBuffers(s);

	GpuMaterial *materials = static_cast<GpuMaterial *>(s->frame.materials.ptr);
	{
		const pt_material_t plain{};
		SetMaterial(s, materials[0], plain, [](int) { return -1; });
		materials[0].alpha = 1.0f;
		materials[0].roughness = 1.0f;
	}
	for (uint32_t i = 0; i < num_materials; i++)
		SetMaterial(s, materials[i], scene->materials[i], [](int handle) { return handle; });

	Lap(s, "materials");

	// solid triangles first, then the glass, then what the eye carries
	uint32_t hash = 2166136261u;
	uint32_t num_solid = 0, num_held = 0;
	if (n)
	{
		const auto material_of = [&](uint32_t t)
		{
			return (scene->tri_materials && scene->tri_materials[t] < num_materials) ? scene->tri_materials[t] : 0u;
		};
		float *corners = static_cast<float *>(s->frame.corners.ptr);
		float *normals = static_cast<float *>(s->frame.normals.ptr);
		float *prev = static_cast<float *>(s->frame.prev.ptr);
		float *frame_tangents = static_cast<float *>(s->frame.tangents.ptr);
		GpuTri *tris = static_cast<GpuTri *>(s->frame.tris.ptr);

		// (held glass goes with the glass: it is not kept apart)
		const auto held_of = [&](uint32_t m)
		{
			return num_materials && (scene->materials[m].flags & PT_MAT_HELD)
				&& !OneSided(scene->materials[m]);
		};
		for (uint32_t t = 0; t < n; t++)
		{
			const uint32_t m = material_of(t);
			if (held_of(m))
				num_held++;
			else if (!num_materials || !OneSided(scene->materials[m]))
				num_solid++;
		}
		uint32_t at_solid = 0, at_glass = num_solid, at_held = n - num_held;
		// (a run of triangles of one material at a time: they stay
		// together, and a model's are thousands to a material)
		for (uint32_t t = 0; t < n; )
		{
			const uint32_t m = material_of(t);
			uint32_t count = 1;
			while (t + count < n && material_of(t + count) == m)
				count++;
			uint32_t &at = held_of(m) ? at_held : (!num_materials || !OneSided(scene->materials[m])) ? at_solid : at_glass;
			const uint32_t to = at;
			at += count;
			memcpy(&corners[to * 9], &scene->positions[t * 9], (size_t)count * 36);
			if (scene->normals)
				memcpy(&normals[to * 9], &scene->normals[t * 9], (size_t)count * 36);
			if (scene->tangents)
				memcpy(&frame_tangents[to * 18], &scene->tangents[t * 18], (size_t)count * 72);
			if (scene->prev_positions)
				memcpy(&prev[to * 9], &scene->prev_positions[t * 9], (size_t)count * 36);
			for (uint32_t i = 0; i < count; i++)
			{
				GpuTri out;
				if (scene->uvs)
					memcpy(out.uv, &scene->uvs[(t + i) * 6], 24);
				else
					memset(out.uv, 0, 24);
				out.material = m;
				out.pad = 0;
				tris[to + i] = out;
			}
			t += count;
		}
		hash = HashBytes(scene->positions, (size_t)n * 36, hash);
	}
	s->frame.num_solid = num_solid;
	s->frame.num_glass = n - num_solid - num_held;
	s->frame.num_held = num_held;

	GpuLight *lights = static_cast<GpuLight *>(s->frame_lights.ptr);
	for (uint32_t i = 0; i < num_lights; i++)
	{
		memset(&lights[i], 0, sizeof(GpuLight));
		lights[i].tri = kPointLight;
		for (int a = 0; a < 3; a++)
		{
			lights[i].origin[a] = scene->lights[i].origin[a];
			lights[i].emission[a] = scene->lights[i].intensity[a];
		}
		lights[i].radius = std::max(scene->lights[i].radius, 0.0f);
		if (lights[i].radius <= 0.0f)
		{
			for (int a = 0; a < 3; a++)
				lights[i].dir[a] = scene->lights[i].direction[a];
			lights[i].cone_cos = scene->lights[i].cone_cos;
			lights[i].edge[0] = std::max(scene->lights[i].reach, 0.0f);
			lights[i].edge[1] = std::max(scene->lights[i].core, 0.0f);
			const float *size = scene->lights[i].box_size;
			if (size[0] > 0.0f && size[1] > 0.0f && size[2] > 0.0f)
				for (int a = 0; a < 3; a++)
				{
					lights[i].box_origin[a] = scene->lights[i].box_origin[a];
					lights[i].box_size[a] = size[a];
				}
			lights[i].no_shadow = scene->lights[i].no_shadow ? 1.0f : 0.0f;
		}
	}
	if (num_lights)
		hash = HashBytes(scene->lights, num_lights * sizeof(pt_point_light_t), hash);

	Lap(s, "triangles");

	// ---- the instances: the map, its glass, what moves, its glass, what the eye carries
	VkAccelerationStructureInstanceKHR *inst = static_cast<VkAccelerationStructureInstanceKHR *>(s->instances.ptr);
	const struct { const Accel *blas; uint32_t count; } parts[kNumInstances] = {
		{&s->world.solid, s->world.num_solid}, {&s->world.glass, s->world.num_glass},
		{&s->frame.solid, s->frame.num_solid}, {&s->frame.glass, s->frame.num_glass},
		{&s->frame.held, s->frame.num_held},
	};
	s->num_instances = 0;
	for (uint32_t i = 0; i < kNumInstances; i++)
	{
		if (!parts[i].count)
			continue;
		VkAccelerationStructureInstanceKHR &out = inst[s->num_instances++];
		memset(&out, 0, sizeof(out));
		out.transform.matrix[0][0] = out.transform.matrix[1][1] = out.transform.matrix[2][2] = 1.0f;
		out.instanceCustomIndex = i;		// the shaders tell them apart by this
		out.mask = i == 4 ? kMaskHeld : kMaskScene;
		// rays are told to pass through the backs of triangles; only glass
		// heeds that. Its front is the side its corners run counter clockwise
		// seen from, which is how the card takes them unless told otherwise.
		out.flags = (i & 1) ? 0 : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		out.accelerationStructureReference = parts[i].blas->address;
	}

	// ---- the view
	AdaptExposure(s, view);

	// filled in here and copied to the card at the end
	FrameBlock &f = s->block;
	const FrameBlock was = f;
	memset(&f, 0, sizeof(f));
	for (int i = 0; i < 3; i++)
	{
		f.origin[i] = view->origin[i];
		f.forward[i] = view->forward[i];
		f.right[i] = view->right[i];
		f.up[i] = view->up[i];
		f.sky_turn[i] = 0.0f;
		f.grid_origin[i] = s->grid_origin[i];
		f.grid_dims[i] = s->grid_dims[i];
		f.medium[i] = view->medium_absorb[i];
	}
	f.origin[3] = std::tan(view->fov_x * 0.5f * 3.14159265f / 180.0f);
	f.forward[3] = std::tan(view->fov_y * 0.5f * 3.14159265f / 180.0f);
	// motion blur: the eye as the shutter opened, and what it carries
	if (view->blur)
	{
		for (int i = 0; i < 3; i++)
		{
			f.open_origin[i] = view->open_origin[i];
			f.open_forward[i] = view->open_forward[i];
			f.open_right[i] = view->open_right[i];
			f.open_up[i] = view->open_up[i];
		}
		f.open_origin[3] = 1.0f;
	}
	f.held[0] = (int32_t)(num_solid + s->frame.num_glass);
	f.held[1] = (int32_t)num_held;
	f.held[2] = view->reflection_history ? 1 : 0;
	f.held[3] = s->world_smooth;
	const bool same_camera = s->has_history && !memcmp(f.origin, s->camera, sizeof(s->camera));
	// What is done about noise: 2, all there is; 1, nothing, but frames add up
	// while the eye is at rest; 0, nothing. Without the first there is nothing
	// of an earlier view in the picture, ever. A debug view is shown filtered.
	const int filtering = view->debug ? 2 : std::min(std::max(view->filter, 0), 2);
	const bool use_history = filtering == 2 || (filtering == 1 && same_camera);
	memcpy(s->camera, f.origin, sizeof(s->camera));			// origin, forward, right, up
	memcpy(f.prev_origin, was.origin, sizeof(s->camera));

	// each frame looks through a slightly different point of every pixel, so
	// that over time edges are seen from all across it
	// Only where the last pass puts the picture back together from those
	// points, which is the filtered picture: a raw one would show each
	// frame where it was traced, and shake by a part of a pixel.
	if (view->antialias && !view->debug && filtering == 2)
	{
		const auto halton = [](uint32_t index, uint32_t base)
		{
			float fr = 1.0f, r = 0.0f;
			for (; index; index /= base)
			{
				fr /= base;
				r += fr * (index % base);
			}
			return r;
		};
		// the more the picture is enlarged, the more places within a traced
		// pixel have to be visited before every full size pixel has had one
		const uint32_t places = (uint32_t)std::min(64.0f, std::max(16.0f, 8.0f / (scale * scale)));
		f.right[3] = halton((s->frame_index + 1) % places + 1, 2) - 0.5f;
		f.up[3] = halton((s->frame_index + 1) % places + 1, 3) - 0.5f;
	}

	for (int i = 0; i < 4; i++)
	{
		f.sky_a[i] = s->sky[i];
		f.sky_b[i] = i < 2 ? s->sky[4 + i] : -1;
	}
	{
		// the sky box turns about an axis
		float axis[3] = {view->sky_axis[0], view->sky_axis[1], view->sky_axis[2]};
		const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
		float angle = 0.0f;
		if (len > 0.0f && view->sky_angle != 0.0f)
		{
			for (float &a : axis)
				a /= len;
			angle = view->sky_angle * 3.14159265f / 180.0f;
		}
		else
		{
			axis[0] = axis[1] = 0.0f;
			axis[2] = 1.0f;
		}
		f.sky_turn[0] = axis[0];
		f.sky_turn[1] = axis[1];
		f.sky_turn[2] = axis[2];
		f.sky_turn[3] = std::sin(angle);
		f.sky_misc[0] = std::cos(angle);
		hash = HashBytes(&angle, sizeof(angle), hash);
	}
	f.sky_misc[1] = s->sky_scale;
	f.sky_misc[2] = s->sky_total;
	f.sky_misc[3] = view->time;

	s->frame_index++;
	f.counts[0] = s->num_world_lights;
	f.counts[1] = (int32_t)num_lights;
	f.counts[2] = (int32_t)s->frame_index;
	f.counts[3] = view->anim_frame;
	f.bases[0] = view->view_mode;
	f.bases[2] = filtering;
	f.bases[1] = (int32_t)s->world.num_solid;
	f.bases[3] = (int32_t)s->frame.num_solid;
	f.grid_dims[3] = s->has_grid ? 1 : 0;
	f.grid_origin[3] = s->grid_inv_cell;
	f.table_at[0] = s->table_light_keep;
	f.table_at[1] = s->table_grid_pdf;
	f.table_at[2] = s->table_grid_keep;
	f.table_at[3] = s->table_sky_chance;
	f.table_at2[0] = s->table_sky_cdf;
	f.table_at2[1] = s->sky_res;
	f.table_at2[2] = s->index_grid_light;
	f.table_at2[3] = s->index_grid_count;
	f.table_at3[0] = s->index_light_other;
	f.table_at3[1] = s->index_grid_sorted;

	const int bounces = std::max(0, view->bounces);
	const int paths = std::min(std::max(1, view->samples), 64);
	f.settings[0] = bounces;
	f.settings[1] = view->light_samples > 0 ? std::min(view->light_samples, 64) : 8;
	f.settings[2] = view->reflections;
	f.settings[3] = std::max(1, view->reflection_bounces > 0 ? view->reflection_bounces : bounces);
	f.settings_f[0] = view->firefly_clamp > 0.0f ? view->firefly_clamp : 40.0f;
	f.settings_f[1] = std::max(0.0f, view->reflection_rate);
	f.settings_f[2] = std::max(0.0f, view->wave_strength);
	f.settings_f[3] = view->fog ? std::min(std::max(view->fog_density, 0.0f), 0.05f) : 0.0f;
	f.settings2[0] = (view->refraction != 0 ? 1 : 0) | (view->shadow_faces != 0 ? 2 : 0);
	f.settings2[1] = view->texture_filter != 0;
	f.settings2[2] = paths;
	f.settings2[3] = view->debug;
	f.painted[0] = std::max(0.0f, view->metal_colour);
	f.painted[1] = std::max(0.0f, view->bloom_max);
	f.painted[2] = (float)std::min(std::max(view->fog_samples, 1), 16);
	{
		const float history = (float)std::min(std::max(view->history, 1), 512);
		f.painted[3] = view->fog_history >= 1 ? std::min((float)view->fog_history, history) : history;
	}

	s->exposure_used = view->debug ? 1.0f : view->exposure * (view->auto_exposure ? s->auto_exposure : 1.0f);
	f.medium[3] = s->exposure_used;
	s->filter_passes = filtering == 2 ? std::min(std::max(view->denoise, 0), 4) : 0;
	f.output_i[0] = view->tonemap;
	f.output_i[1] = s->filter_passes;
	f.output_i[2] = std::min(std::max(view->history, 1), 512);
	f.output_i[3] = (s->has_history && use_history) ? 1 : 0;
	f.output_f[0] = view->saturation;
	f.output_f[1] = view->contrast;
	f.output_f[2] = view->bloom;

	// the light styles, which flicker
	float styles[kNumStyles];
	for (int i = 0; i < kNumStyles; i++)
		styles[i] = (view->light_styles && i < view->num_light_styles) ? view->light_styles[i] : 1.0f;
	memcpy(s->tables.ptr, styles, sizeof(styles));
	hash = HashBytes(styles, sizeof(styles), hash);

	// the map's lights, as bright as the frame says each is
	if (view->light_scales || s->lights_scaled)
	{
		GpuLight *of_map = static_cast<GpuLight *>(s->world_lights.ptr);
		const int given = view->light_scales ? std::min(view->num_light_scales, (int)s->light_given.size()) : 0;
		for (int i = 0; i < (int)s->light_given.size(); i++)
		{
			const int at = s->light_given[i];
			if (at < 0)
				continue;
			for (int a = 0; a < 3; a++)
				of_map[at].emission[a] = s->light_base[at * 3 + a] * (i < given ? std::max(view->light_scales[i * 3 + a], 0.0f) : 1.0f);
		}
		if (given)
			hash = HashBytes(view->light_scales, (size_t)given * 3 * sizeof(float), hash);
		s->lights_scaled = view->light_scales != nullptr;
	}

	// everything outside the camera that changes the picture
	hash = HashBytes(&view->anim_frame, sizeof(view->anim_frame), hash);
	hash = HashBytes(f.settings, sizeof(f.settings) + sizeof(f.settings_f) + sizeof(f.settings2), hash);
	hash = HashBytes(&view->exposure, sizeof(view->exposure), hash);
	hash = HashBytes(f.painted, sizeof(f.painted), hash);
	if (s->world_has_waves || s->pending.size())
		hash = HashBytes(&view->time, sizeof(view->time), hash);
	// with nothing changing the average may run on and converge
	// (or, adding frames up at rest, whenever the eye has not moved: what
	// does move in the view starts afresh by itself, see temporal.comp)
	const bool still = same_camera && (filtering == 1 || hash == s->prev_hash);
	s->prev_hash = hash;
	f.output_f[3] = still ? 1.0f : 0.0f;

	s->parity ^= 1;
	f.frame_has[0] = ((scene && scene->normals) ? 1 : 0) | ((scene && scene->tangents) ? 2 : 0);
	f.frame_has[1] = (scene && scene->prev_positions) ? 1 : 0;
	f.frame_has[2] = s->parity;
	f.frame_has[3] = (view->antialias && !view->debug && filtering == 2) ? 1 : 0;
	s->bloom_on = view->bloom > 0.0f && !view->debug;
	f.size[0] = rw;
	f.size[1] = rh;
	f.out_size[0] = s->out_width;
	f.out_size[1] = s->out_height;
	f.size[2] = s->num_waters;
	f.size[3] = view->planes ? 1 : 0;
	memcpy(f.water_rect, s->water_rect, sizeof(f.water_rect));
	memcpy(f.water_at, s->water_at, sizeof(f.water_at));
	memcpy(f.water_wave, s->water_wave, sizeof(f.water_wave));
	memcpy(f.water_lamp, s->water_lamp, sizeof(f.water_lamp));
	f.liquid[0] = std::min(std::max(view->wave_reach, 0.0f), 8.0f);
	f.liquid[2] = std::min(std::max(view->water_shafts, 0.0f), 8.0f);
	f.liquid[3] = std::min(std::max(view->water_wet, 0.0f), 1.0f);
	// Looked for while things change and for a few frames after: the frame
	// a light goes out is the last that differs from the one before, and
	// what little of it is missed then would fade very slowly from an
	// average that runs on.
	s->still_frames = still ? std::min(s->still_frames + 1, 1000) : 0;
	s->react_on = f.output_i[3] && view->react > 0.0f && s->still_frames < 8;
	f.liquid[1] = s->react_on ? std::min(view->react, 1.0f) : 0.0f;

	memcpy(s->frame_block.ptr, &f, sizeof(f));
	s->prev_time = view->time;
	s->trace_ready = true;
	s->trace_pending = true;

	// the times shown are the last known: this view's are not, until the
	// card has traced it
	char times[128] = "";
	if (s->stages_known)
		snprintf(times, sizeof(times), "|scene %.1f build %.1f trace %.1f history %.1f filter %.1f out %.1f",
			s->stage_ms[0], s->stage_ms[1], s->stage_ms[2], s->stage_ms[3], s->stage_ms[4], s->stage_ms[5]);
	snprintf(s->stats, sizeof(s->stats), "%dx%d to %dx%d %dspp %db%s|%u + %u triangles, %d + %u lights|exp %.2f light %.4f%s",
		rw, rh, s->out_width, s->out_height, paths, bounces, times,
		s->world.num_solid + s->world.num_glass, n, s->num_world_lights, num_lights,
		s->exposure_used, s->metered, still ? " still" : "");
	s->scene_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count();
	Lap(s, "view");
}

// what one step has written, the next may read
void Between(VkCommandBuffer cmd, VkPipelineStageFlags2 stage, VkAccessFlags2 access,
	VkPipelineStageFlags2 next_stage, VkAccessFlags2 next_access)
{
	VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	b.srcStageMask = stage;
	b.srcAccessMask = access;
	b.dstStageMask = next_stage;
	b.dstAccessMask = next_access;
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.memoryBarrierCount = 1;
	di.pMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cmd, &di);
}

// what one pass has written, the next may read
void BetweenPasses(VkCommandBuffer cmd, VkPipelineStageFlags2 next_stage, VkAccessFlags2 next_access)
{
	Between(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, next_stage, next_access);
}

// pictures the host has changed since the last frame (the wave maps of
// simulated water): bring the card's copies up to date
void RecordUpdates(RtxBackend *s, VkCommandBuffer cmd)
{
	for (const Pending &p : s->pending)
	{
		const Texture &t = s->textures[p.slot];
		if (!t.image)
			continue;
		Barrier(cmd, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkBufferImageCopy region{};
		region.bufferOffset = p.offset;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {(uint32_t)t.width, (uint32_t)t.height, 1};
		vkCmdCopyBufferToImage(cmd, s->updates.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		Barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	s->pending.clear();
	s->updates_used = 0;
}

// traces the view and makes the picture of it; part of the frame's commands
void RecordTrace(RtxBackend *s, VkCommandBuffer cmd)
{
	// The card notes the time before each part of the work, and after the
	// last: stage is the one of kStageNames what follows belongs to. A fine
	// reading splits a stage, and is only taken in a build with PT_PERF.
	uint32_t stamp = 0;
	const auto mark = [&](int stage, const char *name, bool fine = false)
	{
#ifndef PT_PERF
		if (fine)
			return;
		(void)name;
#endif
		if (!s->stamps || stamp >= kMaxStamps)
			return;
		s->stamp_stage[stamp] = stage;
#ifdef PT_PERF
		s->stamp_name[stamp] = name;
#endif
		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->stamps, stamp++);
	};
	if (s->stamps)
		vkCmdResetQueryPool(cmd, s->stamps, 0, kNumQueries);
#ifdef PT_PERF
	s->show_free = true;
	s->show_asked = false;
#endif
	mark(1, "build");

	// the acceleration structures of what moves, then the one over everything
	const struct { const Accel *blas; VkDeviceAddress corners; uint32_t count; } parts[3] = {
		{&s->frame.solid, s->frame.corners.address, s->frame.num_solid},
		{&s->frame.glass, s->frame.corners.address + (VkDeviceAddress)s->frame.num_solid * 36, s->frame.num_glass},
		{&s->frame.held, s->frame.corners.address + (VkDeviceAddress)(s->frame.num_solid + s->frame.num_glass) * 36,
			s->frame.num_held},
	};
	bool built = false;
	for (const auto &p : parts)
	{
		if (!p.count)
			continue;
		BuildAccel(s, cmd, *p.blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			TriangleGeometry(p.corners, p.count), p.count, kFrameBuild);
		built = true;
	}
	if (built)
		AfterBuild(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
	BuildAccel(s, cmd, s->tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
		InstanceGeometry(s->instances.address), s->num_instances, kFrameBuild);
	AfterBuild(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

	const uint32_t gx = ((uint32_t)s->trace_width + 7) / 8, gy = ((uint32_t)s->trace_height + 7) / 8;
	const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->trace_playout, 0, 1, &s->trace_dset, 0, nullptr);
	uint32_t groups_x = gx, groups_y = gy;
	const auto run = [&](VkPipeline pipeline, int a, int b, int c)
	{
		const PassPush push{a, b, c, 0};
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdPushConstants(cmd, s->trace_playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(cmd, groups_x, groups_y, 1);
	};

	// a ray and its paths for every pixel
	mark(2, "trace");
	run(s->trace_pipeline, 0, 0, 0);
	BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);

	// gathered with what earlier frames saw, less where the light is found
	// to have changed since: looked for block by block, see change.comp
	mark(3, s->react_on ? "change" : "temporal");
	if (s->react_on)
	{
		run(s->change_pipeline, 0, 0, 0);
		BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
		groups_x = (gx + 7) / 8;
		groups_y = (gy + 7) / 8;
		run(s->change_pipeline, 1, 0, 0);
		BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
		groups_x = gx;
		groups_y = gy;
		mark(3, "temporal", true);
	}
	run(s->temporal_pipeline, 0, 0, 0);
	BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);

	// filtered, each pass reading what the one before wrote
	static const char *const kPassNames[] = {"filter 1", "filter 2", "filter 3", "filter 4", "filter 5", "filter 6",
		"filter 7", "filter 8"};
	// (the first from the filter's second set, where the pass before left
	// what was gathered and the noise in it: see temporal.comp)
	mark(4, kPassNames[0]);
	int source = 0;
	for (int i = 0; i < s->filter_passes; i++)
	{
		if (i)
			mark(4, kPassNames[std::min(i, 7)], true);
		run(s->atrous_pipeline, i ? source : 2, i & 1, 1 << i);
		BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
		source = 1 + (i & 1);
	}

	// put together
	mark(5, "compose");
	run(s->compose_pipeline, source, 0, 0);
	BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);

	// the exposure's sums, which that added to, to where they can be read here
	{
		const VkAccessFlags2 copied = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
		BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, copied);
		const VkBufferCopy sums{0, 0, 64 * sizeof(uint32_t)};
		vkCmdCopyBuffer(cmd, s->meter.buffer, s->meter_read.buffer, 1, &sums);
		Between(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, copied, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
	}

	// the glow of what is too bright for the screen: taken out at half size,
	// then blurred across and down three times, which comes close to a gaussian
	if (s->bloom_on)
	{
		mark(5, "bloom", true);
		run(s->bloom_pipeline, 0, 0, 0);
		BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
		for (int i = 0; i < 3; i++)
		{
			run(s->bloom_pipeline, 1, 0, 0);
			BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
			run(s->bloom_pipeline, 2, 0, 0);
			BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);
		}
	}

	// graded for the screen
	mark(5, "grade", true);
	run(s->grade_pipeline, s->bloom_on ? 1 : 0, 0, 0);
	BetweenPasses(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, rw);

	// and built up to the size of the view, edges smoothed over frames
	groups_x = ((uint32_t)s->out_width + 7) / 8;
	groups_y = ((uint32_t)s->out_height + 7) / 8;
	mark(5, "resolve", true);
	run(s->resolve_pipeline, 0, 0, 0);
	BetweenPasses(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	mark(0, nullptr);
	s->stamps_written = stamp;
	s->stamps_asked = s->stamps != VK_NULL_HANDLE;

	s->has_history = true;
}

// -------------------------------------------------- the backend's entries
//
// These are called from C and must not let an exception out.

void RenderView(pt_backend_t *b, const pt_view_t *view)
{
	RtxBackend *s = Self(b);
	try
	{
		RenderViewNow(s, view);
		SubmitTrace(s);
	}
	catch (const Fail &f)
	{
		Logf(s, "RTX path tracer: %s\n", f.msg.c_str());
		s->trace_ready = false;
		s->trace_pending = false;
	}
}

void LoadWorld(pt_backend_t *b, const pt_world_t *world)
{
	RtxBackend *s = Self(b);
	try
	{
		LoadWorldNow(s, world);
	}
	catch (const Fail &f)
	{
		Logf(s, "RTX path tracer: could not load the map: %s\n", f.msg.c_str());
		s->world_loaded = false;
	}
}

const char *Stats(pt_backend_t *b)
{
	return Self(b)->stats;
}

int Stages(pt_backend_t *b, pt_stage_t *stages, int max)
{
	RtxBackend *s = Self(b);
	if (!s->stages_new)
		return 0;
	s->stages_new = false;

	const int count = std::min(max, kNumStages);
	for (int i = 0; i < count; i++)
	{
		stages[i].name = kStageNames[i];
		stages[i].ms = s->stage_ms[i];
	}
	return count;
}

#ifdef PT_PERF
int Perf(pt_backend_t *b, pt_stage_t *stages, int max)
{
	RtxBackend *s = Self(b);
	int n = 0;
	for (int i = 0; i < s->num_laps && n < max; i++)
		stages[n++] = s->laps[i];
	s->laps_taken = true;
	for (int i = 0; i < s->num_card && n < max; i++)
		stages[n++] = s->card[i];
	if (s->show_ms > 0.0f && n < max)
		stages[n++] = {"show", s->show_ms, 1};
	return n;
}
#endif

int TextureCreate(pt_backend_t *b, const pt_texture_t *texture)
{
	RtxBackend *s = Self(b);
	try
	{
		return AddTexture(s, texture);
	}
	catch (const Fail &f)
	{
		Logf(s, "RTX path tracer: %s\n", f.msg.c_str());
		return -1;
	}
}

void TextureDestroy(pt_backend_t *b, int handle)
{
	RemoveTexture(Self(b), handle);
}

// New pixels for a texture. They are kept until the next frame is drawn and
// go to the card as part of it.
void TextureUpdate(pt_backend_t *b, int handle, const uint32_t *pixels)
{
	RtxBackend *s = Self(b);
	if (handle < 0 || handle >= (int)kMaxTextures || !s->textures[handle].image || !pixels)
		return;
	try
	{
		const Texture &t = s->textures[handle];
		const VkDeviceSize bytes = (VkDeviceSize)t.width * t.height * 4;
		// the card may still be copying from here for the last frame
		LapStart(s);
		if (s->pending.empty())
		{
			const VkFence both[2] = {s->fence, s->fence_trace};
			vkWaitForFences(s->device, 2, both, VK_TRUE, UINT64_MAX);
		}
		Lap(s, "wait card", 2);
		if (!s->updates.buffer || s->updates_used + bytes > s->updates.size)
		{
			if (!s->pending.empty())
				return;		// no room left this frame; the next will do
			vkQueueWaitIdle(s->queue);
			FreeBuffer(s, s->updates);
			s->updates = MakeBuffer(s, std::max<VkDeviceSize>(bytes * 8, 1 << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		}
		memcpy(static_cast<uint8_t *>(s->updates.ptr) + s->updates_used, pixels, (size_t)bytes);
		s->pending.push_back(Pending{handle, s->updates_used});
		s->updates_used += bytes;
		Lap(s, "texture copy");
	}
	catch (const Fail &f)
	{
		Logf(s, "RTX path tracer: %s\n", f.msg.c_str());
	}
}

// The picture last presented, put together again here: the traced picture
// stretched over the view as the last pass stretches it, and the overlay.
// ---- the planes of the last view, for export: see pt.h's PT_PLANE_

int PlaneSize(pt_backend_t *b, int *width, int *height)
{
	RtxBackend *s = Self(b);
	if (!s->targets[kSeen].image)
		return 0;
	*width = s->trace_width;
	*height = s->trace_height;
	return 1;
}

// a half precision float, as the card stores most pictures, as a float
float HalfToFloat(uint16_t h)
{
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
	uint32_t bits;
	if (exponent == 0)
	{
		if (mantissa == 0)
			bits = sign;
		else
		{
			// below the smallest normal number: shift up until it is one
			exponent = 1;
			while ((mantissa & 0x400u) == 0)
			{
				mantissa <<= 1;
				exponent--;
			}
			mantissa &= 0x3ffu;
			bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
		}
	}
	else if (exponent == 31)
		bits = sign | 0x7f800000u | (mantissa << 13);
	else
		bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

int ReadPlane(pt_backend_t *b, int plane, float *out)
{
	RtxBackend *s = Self(b);
	if (!s->targets[kSeen].image || !out)
		return 0;
	const int now = s->parity;

	// which picture holds it, from which channel, how many, and whether the
	// picture is at half precision
	int target, first, count;
	bool half = false;
	switch (plane)
	{
	case PT_PLANE_POSITION:			target = kSeen; first = 0; count = 3; break;
	case PT_PLANE_NORMAL:			target = kSurface + now; first = 0; count = 3; half = true; break;
	case PT_PLANE_DEPTH:			target = kSurface + now; first = 3; count = 1; half = true; break;
	case PT_PLANE_ALBEDO:			target = kAlbedo; first = 0; count = 3; half = true; break;
	case PT_PLANE_SPECULAR:			target = kAlbedo + 1; first = 0; count = 3; half = true; break;
	case PT_PLANE_ROUGHNESS:		target = kAlbedo + 1; first = 3; count = 1; half = true; break;
	case PT_PLANE_EMISSION:			target = kExport; first = 0; count = 3; break;
	case PT_PLANE_METALLIC:			target = kExport; first = 3; count = 1; break;
	case PT_PLANE_MATERIAL:			target = kExport + 1; first = 0; count = 1; break;
	case PT_PLANE_TRIANGLE:			target = kExport + 1; first = 1; count = 2; break;
	case PT_PLANE_DIRECT_DIFFUSE:	target = kExport + 2; first = 0; count = 3; break;
	case PT_PLANE_DIRECT_SPECULAR:	target = kExport + 3; first = 0; count = 3; break;
	case PT_PLANE_RAY_DIFFUSE:		target = kNoisy; first = 0; count = 3; half = true; break;
	case PT_PLANE_RAY_SPECULAR:		target = kNoisy + 1; first = 0; count = 3; half = true; break;
	case PT_PLANE_LIGHT_DIFFUSE:	target = kKept + now * 3; first = 0; count = 3; break;
	case PT_PLANE_LIGHT_SPECULAR:	target = kKept + now * 3 + 1; first = 0; count = 3; break;
	case PT_PLANE_LIGHT_LAYERS:		target = kKept + now * 3 + 2; first = 0; count = 3; break;
	case PT_PLANE_LIGHT_EXTRA:		target = kExtra; first = 0; count = 3; half = true; break;
	case PT_PLANE_PICTURE:			target = kHdr; first = 0; count = 3; half = true; break;
	default:
		return 0;
	}

	try
	{
		TraceNow(s);
		const int w = s->trace_width, h = s->trace_height;
		const VkDeviceSize bytes = (VkDeviceSize)w * h * (half ? 8 : 16);
		vkQueueWaitIdle(s->queue);
		if (!s->readback.buffer || s->readback.size < bytes)
		{
			FreeBuffer(s, s->readback);
			s->readback = MakeBuffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
		}
		VkCommandBuffer cmd = BeginOnce(s);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
		vkCmdCopyImageToBuffer(cmd, s->targets[target].image, VK_IMAGE_LAYOUT_GENERAL, s->readback.buffer, 1, &region);
		EndOnce(s);

		const size_t pixels = (size_t)w * h;
		if (half)
		{
			const uint16_t *in = static_cast<const uint16_t *>(s->readback.ptr);
			for (size_t i = 0; i < pixels; i++)
				for (int c = 0; c < count; c++)
					out[i * count + c] = HalfToFloat(in[i * 4 + first + c]);
		}
		else
		{
			const float *in = static_cast<const float *>(s->readback.ptr);
			for (size_t i = 0; i < pixels; i++)
				for (int c = 0; c < count; c++)
					out[i * count + c] = in[i * 4 + first + c];
		}
	}
	catch (const Fail &f)
	{
		Logf(s, "RTX path tracer: %s\n", f.msg.c_str());
		return 0;
	}
	return count;
}

int ReadPixels(pt_backend_t *b, uint32_t *pixels, int with_overlay)
{
	RtxBackend *s = Self(b);
	const Target &picture = s->targets[kPicture];
	if (!picture.image)
		return 0;
	try
	{
		TraceNow(s);
		const int tw = s->out_width, th = s->out_height;		// the finished picture
		const VkDeviceSize bytes = (VkDeviceSize)tw * th * 4;
		vkQueueWaitIdle(s->queue);
		if (!s->readback.buffer || s->readback.size < bytes)
		{
			FreeBuffer(s, s->readback);
			s->readback = MakeBuffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
		}
		VkCommandBuffer cmd = BeginOnce(s);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {(uint32_t)tw, (uint32_t)th, 1};
		vkCmdCopyImageToBuffer(cmd, picture.image, VK_IMAGE_LAYOUT_GENERAL, s->readback.buffer, 1, &region);
		EndOnce(s);

		const uint32_t *traced = static_cast<const uint32_t *>(s->readback.ptr);
		const uint32_t *overlay = s->last_overlay.data();
		if (s->last_overlay.size() != (size_t)s->width * s->height)
			with_overlay = 0;
		// a view made since the last frame was shown can be read already
		const int *rect = s->has_view ? s->view_rect : s->shown;
		const bool traced_now = s->has_view ? s->trace_ready : s->shown_traced;
		for (int y = 0; y < s->height; y++)
		{
			for (int x = 0; x < s->width; x++)
			{
				uint32_t c = 0xff000000u;
				if (traced_now && x >= rect[0] && y >= rect[1] && x < rect[0] + rect[2] && y < rect[1] + rect[3])
				{
					// between the four nearest, as the card's sampler does it
					const float fx = (x + 0.5f - rect[0]) * tw / rect[2] - 0.5f;
					const float fy = (y + 0.5f - rect[1]) * th / rect[3] - 0.5f;
					const int x0 = std::min(std::max((int)std::floor(fx), 0), tw - 1), x1 = std::min(x0 + 1, tw - 1);
					const int y0 = std::min(std::max((int)std::floor(fy), 0), th - 1), y1 = std::min(y0 + 1, th - 1);
					const float ax = std::min(std::max(fx - x0, 0.0f), 1.0f), ay = std::min(std::max(fy - y0, 0.0f), 1.0f);
					const uint32_t p[4] = {traced[y0 * tw + x0], traced[y0 * tw + x1], traced[y1 * tw + x0], traced[y1 * tw + x1]};
					c = 0xff000000u;
					for (int ch = 0; ch < 3; ch++)
					{
						const float top = ((p[0] >> (ch * 8)) & 255) * (1.0f - ax) + ((p[1] >> (ch * 8)) & 255) * ax;
						const float bottom = ((p[2] >> (ch * 8)) & 255) * (1.0f - ax) + ((p[3] >> (ch * 8)) & 255) * ax;
						c |= (uint32_t)(top * (1.0f - ay) + bottom * ay + 0.5f) << (ch * 8);
					}
				}
				if (with_overlay)
				{
					// premultiplied: its colour, plus what it leaves of the view
					const uint32_t o = overlay[(size_t)y * s->width + x];
					const uint32_t keep = 255 - (o >> 24);
					uint32_t out = 0xff000000u;
					for (int ch = 0; ch < 3; ch++)
					{
						const uint32_t v = ((o >> (ch * 8)) & 255) + (((c >> (ch * 8)) & 255) * keep + 127) / 255;
						out |= std::min(v, 255u) << (ch * 8);
					}
					c = out;
				}
				pixels[(size_t)y * s->width + x] = c;
			}
		}
	}
	catch (const Fail &f)
	{
		Logf(s, "RTX path tracer: %s\n", f.msg.c_str());
		return 0;
	}
	return 1;
}

void Record(RtxBackend *s, uint32_t image_index)
{
	VkCommandBuffer cmd = s->cmd;
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	// upload what has changed of the overlay
	if (!s->ov_regions.empty())
	{
	Barrier(cmd, s->ov_image,
		s->ov_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		s->ov_initialized ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
		s->ov_initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
		VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	vkCmdCopyBufferToImage(cmd, s->staging, s->ov_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		(uint32_t)s->ov_regions.size(), s->ov_regions.data());
	Barrier(cmd, s->ov_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	s->ov_initialized = true;
	}

	if (!s->pending.empty())
		RecordUpdates(s, cmd);
	if (s->trace_pending)
	{
		RecordTrace(s, cmd);
		s->trace_pending = false;
	}
#ifdef PT_PERF
	// how long the card takes to put the picture on the screen: once for
	// each view traced, whose commands made the two readings ready
	const bool show_timed = s->stamps && s->show_free;
	if (show_timed)
		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->stamps, kMaxStamps);
#endif

	Barrier(cmd, s->swap_images[image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

	VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	color.imageView = s->swap_views[image_index];
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;	// the triangle covers everything
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, s->extent};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &color;
	vkCmdBeginRendering(cmd, &ri);

	const VkViewport viewport{0.0f, 0.0f, (float)s->extent.width, (float)s->extent.height, 0.0f, 1.0f};
	const VkRect2D scissor{{0, 0}, s->extent};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	Push push{};
	push.view[0] = (float)s->view.x;
	push.view[1] = (float)s->view.y;
	push.view[2] = (float)s->view.width;
	push.view[3] = (float)s->view.height;
	push.screen[0] = (float)s->width;
	push.screen[1] = (float)s->height;
	push.has_view = s->trace_ready ? 1.0f : 0.0f;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->dset, 0, nullptr);
	vkCmdPushConstants(cmd, s->playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);

	Barrier(cmd, s->swap_images[image_index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		s->shared ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
#ifdef PT_PERF
	if (show_timed)
	{
		vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->stamps, kMaxStamps + 1);
		s->show_free = false;
		s->show_asked = true;
	}
#endif

	vkEndCommandBuffer(cmd);
}

void PresentFrame(RtxBackend *s, const uint32_t *overlay, const pt_rect_t *changed, int num_changed)
{
	// a frame that is not shown, for whatever reason, leaves the card's
	// overlay behind: the next one that is takes all of it
	const bool behind = s->ov_behind;
	s->ov_behind = true;

	if (!s->swapchain && !s->shared)
	{
		CreateSwapchain(s);		// window may have been minimized
		if (!s->swapchain)
			return;
	}

	LapStart(s);
	vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, UINT64_MAX);
	Lap(s, "wait card", 2);

	uint32_t index = 0;
	VkResult r = VK_SUCCESS;
	if (!s->shared)
		r = vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX, s->sem_acquire, VK_NULL_HANDLE, &index);
	Lap(s, "acquire", 2);
	if (r == VK_ERROR_OUT_OF_DATE_KHR)
	{
		RecreateSwapchain(s);
		return;
	}
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		Check(r, "vkAcquireNextImageKHR");

	// The card keeps the overlay from frame to frame, and so does the copy
	// here that a frame is read back with: only what has changed is fetched.
	// Most frames that is a status bar and a few lines of text, where all of
	// it is tens of megabytes, copied while the card waits.
	const size_t count = (size_t)s->width * s->height;
	s->ov_regions.clear();
	if (behind || num_changed < 0 || !changed || !s->ov_initialized || s->last_overlay.size() != count)
	{
		memcpy(s->staging_ptr, overlay, count * 4);
		s->last_overlay.assign(overlay, overlay + count);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {(uint32_t)s->width, (uint32_t)s->height, 1};
		s->ov_regions.push_back(region);
	}
	else
	{
		for (int i = 0; i < num_changed; i++)
		{
			const int x0 = std::max(changed[i].x, 0), y0 = std::max(changed[i].y, 0);
			const int x1 = std::min(changed[i].x + changed[i].width, s->width);
			const int y1 = std::min(changed[i].y + changed[i].height, s->height);
			if (x0 >= x1 || y0 >= y1)
				continue;
			for (int y = y0; y < y1; y++)
			{
				const size_t at = (size_t)y * s->width + x0;
				memcpy(static_cast<uint32_t *>(s->staging_ptr) + at, overlay + at, (size_t)(x1 - x0) * 4);
				memcpy(s->last_overlay.data() + at, overlay + at, (size_t)(x1 - x0) * 4);
			}
			// the buffer is laid out as the picture is
			VkBufferImageCopy region{};
			region.bufferOffset = ((VkDeviceSize)y0 * s->width + x0) * 4;
			region.bufferRowLength = (uint32_t)s->width;
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {x0, y0, 0};
			region.imageExtent = {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1};
			s->ov_regions.push_back(region);
		}
	}
	Lap(s, "overlay");
	vkResetFences(s->device, 1, &s->fence);
	vkResetCommandBuffer(s->cmd, 0);
	Record(s, index);
	Lap(s, "record");

	VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	wait.semaphore = s->sem_acquire;
	wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	if (!s->shared)
		signal.semaphore = s->swap_drawn[index];
	VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	cbi.commandBuffer = s->cmd;

	VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.waitSemaphoreInfoCount = 1;
	si.pWaitSemaphoreInfos = &wait;
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &cbi;
	si.signalSemaphoreInfoCount = 1;
	si.pSignalSemaphoreInfos = &signal;
	if (s->shared)
	{
		// nothing was acquired and nothing is presented: whoever opened the
		// picture reads it once this returns, so wait until it is drawn
		si.waitSemaphoreInfoCount = 0;
		si.signalSemaphoreInfoCount = 0;
		Check(vkQueueSubmit2(s->queue, 1, &si, s->fence), "vkQueueSubmit2");
		Lap(s, "submit");
		vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, UINT64_MAX);
		Lap(s, "wait card", 2);
		s->ov_behind = false;
		return;
	}
	Check(vkQueueSubmit2(s->queue, 1, &si, s->fence), "vkQueueSubmit2");
	Lap(s, "submit");

	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &s->swap_drawn[index];
	pi.swapchainCount = 1;
	pi.pSwapchains = &s->swapchain;
	pi.pImageIndices = &index;
	r = vkQueuePresentKHR(s->queue, &pi);
	Lap(s, "present", 2);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
		RecreateSwapchain(s);
	else
		Check(r, "vkQueuePresentKHR");
	s->ov_behind = false;		// the commands with the overlay's changes were sent
}

void Present(pt_backend_t *b, const uint32_t *overlay, const pt_rect_t *changed, int num_changed)
{
	RtxBackend *s = Self(b);
	try
	{
		PresentFrame(s, overlay, changed, num_changed);
	}
	catch (const Fail &f)
	{
		// nothing useful to do mid-frame; say so and keep the game alive
		Logf(s, "RTX path tracer: %s\n", f.msg.c_str());
	}
	memcpy(s->shown, s->view_rect, sizeof(s->shown));
	s->shown_traced = s->has_view && s->trace_ready;
	s->has_view = false;
	s->trace_ready = false;
	s->trace_pending = false;
}

void Destroy(pt_backend_t *b)
{
	RtxBackend *s = Self(b);
	if (s->device)
	{
		vkDeviceWaitIdle(s->device);
		DestroyScene(s);
		if (s->pipeline) vkDestroyPipeline(s->device, s->pipeline, nullptr);
		if (s->playout) vkDestroyPipelineLayout(s->device, s->playout, nullptr);
		if (s->dpool) vkDestroyDescriptorPool(s->device, s->dpool, nullptr);
		if (s->dsl) vkDestroyDescriptorSetLayout(s->device, s->dsl, nullptr);
		if (s->staging) vkDestroyBuffer(s->device, s->staging, nullptr);
		if (s->staging_memory) vkFreeMemory(s->device, s->staging_memory, nullptr);
		if (s->ov_sampler) vkDestroySampler(s->device, s->ov_sampler, nullptr);
		if (s->ov_view) vkDestroyImageView(s->device, s->ov_view, nullptr);
		if (s->ov_image) vkDestroyImage(s->device, s->ov_image, nullptr);
		if (s->ov_memory) vkFreeMemory(s->device, s->ov_memory, nullptr);
		DestroySwapchain(s);
		if (s->shared_image) vkDestroyImage(s->device, s->shared_image, nullptr);
		if (s->shared_memory) vkFreeMemory(s->device, s->shared_memory, nullptr);
#ifdef _WIN32
		if (s->shared_handle) CloseHandle((HANDLE)s->shared_handle);
#else
		if (s->shared_handle) close((int)(intptr_t)s->shared_handle - 1);
#endif
		if (s->sem_acquire) vkDestroySemaphore(s->device, s->sem_acquire, nullptr);
		if (s->fence) vkDestroyFence(s->device, s->fence, nullptr);
		if (s->fence_trace) vkDestroyFence(s->device, s->fence_trace, nullptr);
		if (s->cmdpool) vkDestroyCommandPool(s->device, s->cmdpool, nullptr);
		vkDestroyDevice(s->device, nullptr);
	}
	if (s->surface) vkDestroySurfaceKHR(s->instance, s->surface, nullptr);
	if (s->messenger)
	{
		auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(s->instance, "vkDestroyDebugUtilsMessengerEXT");
		if (destroy)
			destroy(s->instance, s->messenger, nullptr);
	}
	if (s->instance) vkDestroyInstance(s->instance, nullptr);
	delete s;
}

} // namespace

int SharedImage(pt_backend_t *b, pt_shared_image_t *out)
{
	RtxBackend *s = Self(b);

	if (!s->shared || !s->shared_handle)
		return 0;
#ifdef _WIN32
	out->handle = s->shared_handle;
#else
	out->handle = (void *)((intptr_t)s->shared_handle - 1);
#endif
	out->size = s->shared_size;
	out->width = (int)s->extent.width;
	out->height = (int)s->extent.height;
	return 1;
}

extern "C" pt_backend_t *pt_rtx_create(const pt_create_t *ci, char *err, int errlen)
{
	RtxBackend *s = new RtxBackend;
	s->base.name = "RTX path tracer";
	s->base.device = s->device_name;
	s->base.destroy = Destroy;
	s->base.load_world = LoadWorld;
	s->base.texture_create = TextureCreate;
	s->base.texture_destroy = TextureDestroy;
	s->base.texture_update = TextureUpdate;
	s->base.render_view = RenderView;
	s->base.present = Present;
	s->base.shared_image = SharedImage;
	s->shared = ci->shared != 0;
	if (ci->device_uuid)
	{
		s->any_card = false;
		memcpy(s->card_uuid, ci->device_uuid, VK_UUID_SIZE);
	}
	s->base.stats = Stats;
	s->base.stages = Stages;
#ifdef PT_PERF
	s->base.perf = Perf;
#else
	s->base.perf = nullptr;
#endif
	s->base.read_pixels = ReadPixels;
	s->base.plane_size = PlaneSize;
	s->base.read_plane = ReadPlane;
	s->textures.resize(kMaxTextures);
	s->log = ci->log;
	s->width = ci->width;
	s->height = ci->height;

	try
	{
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "q2pt";
		app.apiVersion = VK_API_VERSION_1_3;

		// PT_VK_VALIDATE in the environment turns on Vulkan's own checking,
		// if the SDK's layer is installed; what it finds goes to the log
		const bool validate = getenv("PT_VK_VALIDATE") != nullptr && HasValidationLayer();
		const char *const iexts[] = {VK_KHR_SURFACE_EXTENSION_NAME, PT_SURFACE_EXTENSION,
			VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
		const char *const layers[] = {"VK_LAYER_KHRONOS_validation"};
		VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &app;
		ici.enabledExtensionCount = validate ? 3 : 2;
		ici.ppEnabledExtensionNames = iexts;
		ici.enabledLayerCount = validate ? 1 : 0;
		ici.ppEnabledLayerNames = layers;
		Check(vkCreateInstance(&ici, nullptr, &s->instance), "vkCreateInstance");
		if (validate)
			CreateMessenger(s);

#ifdef _WIN32
		if (!s->shared)
		{
			VkWin32SurfaceCreateInfoKHR wci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
			wci.hinstance = (HINSTANCE)ci->hinstance;
			wci.hwnd = (HWND)ci->hwnd;
			Check(vkCreateWin32SurfaceKHR(s->instance, &wci, nullptr, &s->surface), "vkCreateWin32SurfaceKHR");
		}
#else
		if (!s->shared)
		{
			VkXlibSurfaceCreateInfoKHR xci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
			xci.dpy = (Display *)ci->hinstance;
			xci.window = (Window)(uintptr_t)ci->hwnd;
			Check(vkCreateXlibSurfaceKHR(s->instance, &xci, nullptr, &s->surface), "vkCreateXlibSurfaceKHR");
		}
#endif

		PickDevice(s);
		CreateDevice(s);
		if (s->shared)
			CreateShared(s);
		else
		{
			CreateSwapchain(s);
			if (!s->swapchain)
				Throw("window has no drawable area");
		}
		CreateOverlay(s);
		CreateScene(s);
		CreatePipeline(s, s->surface_format.format);
	}
	catch (const Fail &f)
	{
		snprintf(err, errlen, "%s", f.msg.c_str());
		Destroy(&s->base);
		return nullptr;
	}
	return &s->base;
}
