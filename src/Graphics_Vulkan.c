#define CC_SCRATCH_VBS_ARE_DYNAMIC
#include "Core.h"
#if CC_GFX_BACKEND == CC_GFX_BACKEND_VULKAN
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Window.h"
#include "Menus.h"
#include "Lighting.h"
#include "World.h"
#include "Camera.h"
#include "Block.h"
#include "EnvRenderer.h"
#include "Game.h"
#include "TexturePack.h"
#include "Bitmap.h"

/* Vulkan 1.1 backend (Windows only for now) */

/* Avoid pointless includes */
#define WIN32_LEAN_AND_MEAN
#define NOSERVICE
#define NOMCX
#define NOIME
#include <windows.h>

/* Vulkan types/functions are declared manually, so the Vulkan SDK isn't needed to compile */
#include "../misc/vulkan/VulkanCommon.h"
#include "../misc/vulkan/VulkanShaders.h"

/* Functions must be dynamically loaded from vulkan-1.dll */
/* e.g. static VkResult (VKAPI_PTR *vkCreateInstance)(const VkInstanceCreateInfo* pCreateInfo, ...); */
#define VK_FUNC(retType, name, args) static retType (VKAPI_PTR *name) args;
#include "../misc/vulkan/VulkanFuncs.h"

static const struct DynamicLibSym vk_funcs[] = {
	/* e.g. { "vkCreateInstance", (void**)&vkCreateInstance, true }, */
	#undef  VK_FUNC
	#define VK_FUNC(retType, name, args) { DYNAMICLIB_QUOTE(name), (void**)&name, true },
	#include "../misc/vulkan/VulkanFuncs.h"
};

/* Max number of frames the CPU can get ahead of the GPU */
#define FRAMES_IN_FLIGHT 2
#define MAX_SWAP_IMAGES  16
/* Buffers and textures are sub-allocated from large blocks of device memory, */
/*  since some drivers only support up to 4096 allocations in total */
#define MEM_BLOCK_SIZE   (64 * 1024 * 1024)
/* Staging data and dynamic vertices are written into a per frame ring buffer */
#define RING_CHUNK_SIZE  (4  * 1024 * 1024)
#define RING_MAX_KEEP    (32 * 1024 * 1024)
#define RING_MAX_CHUNKS  32
#define DESC_POOL_SETS   512
#define PIPELINE_KEYS    131072
#define KEY_DEPTH_PREPASS (1 << 15) /* See CalcPipelineKey */
#define KEY_AFTER_PREPASS (1 << 16)
#define WAIT_FOREVER     ((cc_uint64)-1)

#define GARBAGE_BUFFER  0
#define GARBAGE_TEXTURE 1
#define GARBAGE_SCRATCH 2
#define GARBAGE_RING    3

struct ShaderDesc { const unsigned int* data; int len; };

/* Free region within a block of device memory */
struct MemRange { struct MemRange* next; VkDeviceSize offset, size; };
struct MemBlock {
	struct MemBlock* next;
	struct MemRange* free;
	VkDeviceMemory memory;
	VkBuffer buffer;
	VkDeviceSize size;
	cc_uint32 type;
	VkDeviceAddress address;
};
struct MemPool  { struct MemBlock* blocks; VkFlags usage; };
struct MemAlloc { struct MemBlock* block; VkDeviceSize offset, size; struct ChunkRT* rt; };

struct RingChunk { VkBuffer buffer; VkDeviceMemory memory; cc_uint8* data; VkDeviceSize size, used; VkDeviceAddress address; };
struct Garbage   { int type; void* obj; };

/* Resources are only reused once the GPU has finished rendering the frame they were used in */
struct Frame {
	VkCommandPool pool;
	VkCommandBuffer uploadCmd, drawCmd;
	VkFence fence;
	VkSemaphore acquireSem;
	struct RingChunk chunks[RING_MAX_CHUNKS];
	int numChunks;
	struct Garbage* garbage;
	int numGarbage, maxGarbage;
	/* Top level acceleration structure of all the chunks, for ray traced shadows */
	VkAccelerationStructureKHR tlas;
	struct MemAlloc tlasAlloc, tlasScratch;
	cc_uint32 tlasCapacity;
	VkDescriptorSet rtSet;
};

/* Bottom level acceleration structure of the shadow casting faces of a chunk */
struct ChunkRT {
	VkAccelerationStructureKHR blas;
	struct MemAlloc blasAlloc;
	VkDeviceAddress address;
	struct MemAlloc* vb;
	/* Ranges of cutout vertices (e.g. leaves) come first, then ranges of opaque vertices */
	int* ranges;
	int numRanges, numCutout, numGeoms, listIndex;
	/* Light emitting blocks in the chunk (x, y, z, block | faces light gets out of << 16) */
	/*  (chunks with lights but no shadow casters, e.g. just a light sprite, only have the lights) */
	int* lights;
	int numLights;
	cc_bool built;
};

struct VulkanTexture {
	VkImage image;
	VkImageView view;
	struct MemAlloc alloc;
	VkDescriptorPool pool;
	VkDescriptorSet sets[2];
	cc_uint32 levels;
};

/* Dynamic VBs keep a CPU side copy of the vertices, since the game expects the */
/*  contents to persist across frames even when the VB isn't locked again */
struct DynamicVb {
	cc_uint8* data;
	cc_uint32 size, used;
	VkBuffer buffer;
	VkDeviceSize offset;
	cc_uint32 frameId;
};

/* NOTE: Layout must match the push constant blocks in misc/vulkan/VulkanShaderGen.c */
/*  (128 bytes is the maximum size that is guaranteed to be supported by all GPUs) */
static struct PushConstants {
	struct Matrix mvp;
	float texX, texY, rtTemporal, _pad1;
	float fogR, fogG, fogB, fogEnd;
	float fogDensity;
} pc;

/* Per frame constants for ray traced shadows (uniform buffer in descriptor set 1) */
/* NOTE: Layout must match the Params uniform block in misc/vulkan/VulkanShaderGen.c (std140 rules) */
struct RTParams {
	VkDeviceAddress table; float _pad0[2]; /* Table of cutout geometry vertices */
	float camX, camY, camZ, shadowDist;
	float sunX, sunY, sunZ, diffuseScale;
	float ratioR, ratioG, ratioB; int samples;
	float uX, uY, uZ, _pad1; /* Directions across the sun's disc */
	float vX, vY, vZ, _pad2;
	struct Matrix prevMVP;   /* Transform chunks were drawn with in the previous frame */
	float screenW, screenH; int frame, hasPrev;
	float maxHistory, aoStrength, aoRange, _pad3;
	float sunR, sunG, sunB, giStrength;
	float godStrength, godDensity, godG, godRange; /* God rays (sunlight scattered by the air) */
	float scatterR, scatterG, scatterB, caustics;   /* Colour of scattered light, and caustics under water */
	float skyDY; int skybox; float time, _pad4;      /* See EnvRenderer_RenderSky/RenderSkybox */
	/* Light emitting blocks (3 vec4s each: centre + intensity, colour + faces light gets out of, half size of */
	/*  the block's box + unused), and a grid of cells covering the world (offset + count, see RT_BuildLights) */
	VkDeviceAddress lights, lightGrid;
	float gridW, gridH, gridL, invCellSize; /* Number of cells along each axis */
	int lightsGen, numLights; float lightStrength, _pad5; /* (lightsGen changes whenever light indices do) */
};

static VkInstance instance;
static VkPhysicalDevice physDevice;
static VkDevice device;
static VkQueue queue;
static cc_uint32 queueFamily;
static VkSurfaceKHR surface;
static VkPhysicalDeviceProperties props;
static VkPhysicalDeviceMemoryProperties memProps;

static VkFormat colorFormat, depthFormat;
static VkColorSpaceKHR colorSpace;
/* resumePass keeps existing contents, for continuing a frame after taking a screenshot */
/* noHistoryPass is used instead of renderPass when ray traced shadows are supported but turned off */
static VkRenderPass renderPass, resumePass, noHistoryPass;

static VkSwapchainKHR swapchain;
static VkExtent2D swapExtent;
static VkFlags swapUsage;
static cc_uint32 swapCount, imageIndex;
static VkImage swapImages[MAX_SWAP_IMAGES];
static VkImageView swapViews[MAX_SWAP_IMAGES];
static VkFramebuffer framebuffers[MAX_SWAP_IMAGES][FRAMES_IN_FLIGHT];
static VkSemaphore renderSems[MAX_SWAP_IMAGES];
static VkImage depthImage;
static VkImageView depthView;
static struct MemAlloc depthAlloc;
/* Ray traced shadows/sky light (0) and bounced light (1) of each pixel, which are blended over */
/*  multiple frames to give smooth results (frame slot i writes to history images [x][i], and reads */
/*  the previous frame's history images). These are colour attachments 2 and 3 of the render pass */
/* Shadow history, bounced light/god ray history, and picked block light (ReSTIR) */
#define HISTORY_IMAGES 3
static VkImage historyImages[HISTORY_IMAGES][FRAMES_IN_FLIGHT];
/* Whether the history images were just created, and so still need to be transitioned to the right layout */
static cc_bool historyFresh;
static VkImageView historyViews[HISTORY_IMAGES][FRAMES_IN_FLIGHT];
static struct MemAlloc historyAllocs[HISTORY_IMAGES][FRAMES_IN_FLIGHT];
#define HISTORY_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
/* Picked block light needs 32 bit floats, since it includes the light's index */
#define HistoryFormat(i) ((i) == 2 ? VK_FORMAT_R32G32B32A32_SFLOAT : HISTORY_FORMAT)
static cc_bool swapchainDirty;

static struct Frame frames[FRAMES_IN_FLIGHT];
static struct Frame* frame;
static int frameIndex;
static cc_uint32 frameCounter;
static cc_bool frameOpen, imageAcquired, acquirePending, renderPassActive, uploadBegun, drawBegun;

static struct MemPool bufferPool, imagePool;

static VkDescriptorSetLayout setLayout;
static VkPipelineLayout pipelineLayout;
static VkSampler samplers[4];
static VkDescriptorPool* descPools;
static int numDescPools;
static VkShaderModule vsModules[2], fsModules[12];
static VkPipeline pipelines[PIPELINE_KEYS];

static GfxResourceID white_square;
static cc_bool inited;
static float clearColor[4];
static int colorMask = 0x0F;
static cc_bool cullFaces, mipmapsOn;
static VkViewport viewport;
static VkRect2D scissor;
static VkBuffer vbBuffer, ibBuffer;
static VkDeviceSize vbOffset, ibOffset;
static struct VulkanTexture* curTex;
static int boundPipeline;
static void FlushDeferredDraws(void);
static cc_bool dirtyViewport, dirtyScissor, dirtyConstants, dirtyTexture, dirtyVb, dirtyIb, dirtyCull;
static struct Matrix _view, _proj;

/* Face culling is toggled a lot by MapRenderer, so with VK_EXT_extended_dynamic_state it's */
/*  just a cheap state change (like Direct3D) instead of having to switch to another pipeline */
static cc_bool dynamicCull;
static void (VKAPI_PTR *vkCmdSetCullModeEXT)(VkCommandBuffer cmd, VkFlags cullMode);

/* Ray traced shadows (using VK_KHR_ray_query, so only on GPUs with ray tracing support) */
static cc_bool rtSupported, rtEnabled, rtActive, dirtyRtSet;
/* Whether chunks were drawn with ray traced shadows in the current/previous frame, and with what transform */
static cc_bool rtDrawn, rtPrevDrawn;
/* Set when an option changes, since the previous frames' shadows/lighting no longer apply */
static cc_bool rtResetHistory;
static struct Matrix rtFrameMVP, rtPrevMVP;
static int rtSunAngle, rtSunDir, rtShadowDist, rtSoftness, rtAO, rtGI, rtGodRays, rtBlockLights;
static VkDeviceSize rtScratchAlign;
static struct MemPool asPool, scratchPool;
static VkDescriptorSetLayout rtSetLayout;
static VkDescriptorPool rtDescPool;
/* Contains the struct RTParams for each frame */
/*  (256 is the largest minUniformBufferOffsetAlignment allowed by the Vulkan spec) */
static struct RingChunk rtParams;
#define RT_PARAMS_STRIDE 512
#define RT_FrameParams() ((struct RTParams*)(rtParams.data + frameIndex * RT_PARAMS_STRIDE))
static VkShaderModule vsRtModules[2], fsRtModules[6], fsSkyModules[6], fsEarlyModules[3];
/* Whether the sky is currently being drawn (see Gfx_SetSkyRendering) */
static cc_bool rtSkyDraw;
/* Chunks waiting for their BLAS to be built, and chunks which have a BLAS */
static struct ChunkRT** rtPending;
static struct ChunkRT** rtChunks;
static struct ChunkRT** rtLightOnly;
static int numRtLightOnly, maxRtLightOnly;
static int numRtPending, maxRtPending, numRtChunks, maxRtChunks;

static VkDeviceAddress (VKAPI_PTR *vkGetBufferDeviceAddressKHR)(VkDevice device, const VkBufferDeviceAddressInfo* pInfo);
static VkResult (VKAPI_PTR *vkCreateAccelerationStructureKHR)(VkDevice device, const VkAccelerationStructureCreateInfoKHR* pCreateInfo, const void* pAllocator, VkAccelerationStructureKHR* pStructure);
static void     (VKAPI_PTR *vkDestroyAccelerationStructureKHR)(VkDevice device, VkAccelerationStructureKHR structure, const void* pAllocator);
static void     (VKAPI_PTR *vkGetAccelerationStructureBuildSizesKHR)(VkDevice device, int buildType, const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo, const cc_uint32* pMaxPrimitiveCounts, VkAccelerationStructureBuildSizesInfoKHR* pSizeInfo);
static void     (VKAPI_PTR *vkCmdBuildAccelerationStructuresKHR)(VkCommandBuffer cmd, cc_uint32 infoCount, const VkAccelerationStructureBuildGeometryInfoKHR* pInfos, const VkAccelerationStructureBuildRangeInfoKHR* const* ppRangeInfos);
static VkDeviceAddress (VKAPI_PTR *vkGetAccelerationStructureDeviceAddressKHR)(VkDevice device, const VkAccelerationStructureDeviceAddressInfoKHR* pInfo);

static const struct DynamicLibSym rt_funcs[] = {
	{ "vkGetBufferDeviceAddressKHR",                (void**)&vkGetBufferDeviceAddressKHR,                true },
	{ "vkCreateAccelerationStructureKHR",           (void**)&vkCreateAccelerationStructureKHR,           true },
	{ "vkDestroyAccelerationStructureKHR",          (void**)&vkDestroyAccelerationStructureKHR,          true },
	{ "vkGetAccelerationStructureBuildSizesKHR",    (void**)&vkGetAccelerationStructureBuildSizesKHR,    true },
	{ "vkCmdBuildAccelerationStructuresKHR",        (void**)&vkCmdBuildAccelerationStructuresKHR,        true },
	{ "vkGetAccelerationStructureDeviceAddressKHR", (void**)&vkGetAccelerationStructureDeviceAddressKHR, true },
};

static void RecreateSwapchain(void);
static void Garbage_Collect(struct Frame* f);
static void RT_FreeChunk(struct ChunkRT* rt);
static void RT_FreeTLAS(struct Frame* f);
static void RT_Init(void);
static void RT_FreeAll(void);
static void RT_FreeLights(void);
static void RT_Unlink(struct ChunkRT* rt);


static void HandleError(VkResult res, const char* place) {
	if (res == VK_ERROR_DEVICE_LOST) {
		Gfx_LoseContext(" (Vulkan device lost)");
	} else if (res == VK_ERROR_SURFACE_LOST_KHR) {
		Gfx_LoseContext(" (Vulkan surface lost)");
	} else {
		Process_Abort2(res, place);
	}
}


/*########################################################################################################################*
*------------------------------------------------------Device memory------------------------------------------------------*
*#########################################################################################################################*/
/* Tries to find a memory type with required and preferred flags, then just required flags */
static int FindMemoryType(cc_uint32 typeBits, VkFlags required, VkFlags preferred) {
	VkFlags flags, wanted = required | preferred;
	cc_uint32 i;

	for (i = 0; i < memProps.memoryTypeCount; i++)
	{
		flags = memProps.memoryTypes[i].propertyFlags;
		if ((typeBits & (1u << i)) && (flags & wanted) == wanted) return (int)i;
	}
	for (i = 0; i < memProps.memoryTypeCount; i++)
	{
		flags = memProps.memoryTypes[i].propertyFlags;
		if ((typeBits & (1u << i)) && (flags & required) == required) return (int)i;
	}
	return -1;
}

/* Buffers whose address is used by shaders or acceleration structure builds */
/*  must have their memory allocated with the device address flag */
static VkResult AllocateMemory(const VkMemoryRequirements* reqs, int type, VkFlags usage, VkDeviceMemory* memory) {
	VkMemoryAllocateFlagsInfo flags = { 0 };
	VkMemoryAllocateInfo alloc      = { 0 };

	alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize  = reqs->size;
	alloc.memoryTypeIndex = type;

	if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
		flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		alloc.pNext = &flags;
	}
	return vkAllocateMemory(device, &alloc, NULL, memory);
}

static VkDeviceAddress GetBufferAddress(VkBuffer buffer, VkFlags usage) {
	VkBufferDeviceAddressInfo info = { 0 };
	if (!(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) return 0;

	info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	return vkGetBufferDeviceAddressKHR(device, &info);
}

#define MemAlloc_Address(alloc) ((alloc)->block->address + (alloc)->offset)


static struct MemBlock* MemBlock_Create(struct MemPool* pool, VkDeviceSize size, cc_uint32 typeBits) {
	VkBufferCreateInfo info = { 0 };
	VkMemoryRequirements reqs;
	VkDeviceMemory memory = 0;
	VkBuffer buffer = 0;
	struct MemBlock* block;
	int type;

	reqs.size           = size;
	reqs.memoryTypeBits = typeBits;

	if (pool->usage) {
		info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.size        = size;
		info.usage       = pool->usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device, &info, NULL, &buffer)) return NULL;
		vkGetBufferMemoryRequirements(device, buffer, &reqs);
	}

	type = FindMemoryType(reqs.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0 || AllocateMemory(&reqs, type, pool->usage, &memory)) {
		if (buffer) vkDestroyBuffer(device, buffer, NULL);
		return NULL;
	}
	if (buffer) vkBindBufferMemory(device, buffer, memory, 0);

	block = (struct MemBlock*)Mem_Alloc(1, sizeof(struct MemBlock), "Vulkan memory block");
	block->free = (struct MemRange*)Mem_Alloc(1, sizeof(struct MemRange), "Vulkan memory range");
	block->free->next   = NULL;
	block->free->offset = 0;
	block->free->size   = size;

	block->memory  = memory;
	block->buffer  = buffer;
	block->size    = size;
	block->type    = type;
	block->address = buffer ? GetBufferAddress(buffer, pool->usage) : 0;
	block->next    = pool->blocks;
	pool->blocks   = block;
	return block;
}

static void MemBlock_Destroy(struct MemBlock* block) {
	struct MemRange* range;
	struct MemRange* next;

	for (range = block->free; range; range = next)
	{
		next = range->next;
		Mem_Free(range);
	}
	if (block->buffer) vkDestroyBuffer(device, block->buffer, NULL);
	vkFreeMemory(device, block->memory, NULL);
	Mem_Free(block);
}

/* Simple first fit allocator */
static cc_bool MemBlock_Alloc(struct MemBlock* block, VkDeviceSize size, VkDeviceSize align, struct MemAlloc* alloc) {
	struct MemRange** link = &block->free;
	struct MemRange* range;
	struct MemRange* tail;
	VkDeviceSize start, end;

	for (range = *link; range; link = &range->next, range = *link)
	{
		start = (range->offset + align - 1) & ~(align - 1);
		end   = range->offset + range->size;
		if (start + size > end) continue;

		if (start + size < end && start == range->offset) {
			range->offset = start + size;
			range->size   = end - range->offset;
		} else if (start + size < end) {
			tail = (struct MemRange*)Mem_Alloc(1, sizeof(struct MemRange), "Vulkan memory range");
			tail->offset = start + size;
			tail->size   = end - tail->offset;
			tail->next   = range->next;
			range->next  = tail;
			range->size  = start - range->offset;
		} else if (start == range->offset) {
			*link = range->next;
			Mem_Free(range);
		} else {
			range->size = start - range->offset;
		}

		alloc->block  = block;
		alloc->offset = start;
		alloc->size   = size;
		return true;
	}
	return false;
}

static cc_bool MemPool_Alloc(struct MemPool* pool, const VkMemoryRequirements* reqs, struct MemAlloc* alloc) {
	VkDeviceSize align = reqs->alignment ? reqs->alignment : 1;
	struct MemBlock* block;

	for (block = pool->blocks; block; block = block->next)
	{
		if (!(reqs->memoryTypeBits & (1u << block->type))) continue;
		if (MemBlock_Alloc(block, reqs->size, align, alloc)) return true;
	}

	block = MemBlock_Create(pool, max(reqs->size, MEM_BLOCK_SIZE), reqs->memoryTypeBits);
	return block && MemBlock_Alloc(block, reqs->size, align, alloc);
}

/* Returns the range to the free list, merging it with any adjacent free ranges */
static void MemAlloc_Free(struct MemPool* pool, struct MemAlloc* alloc) {
	struct MemBlock* block = alloc->block;
	VkDeviceSize offset = alloc->offset, size = alloc->size;
	struct MemBlock** link;
	struct MemRange* prev = NULL;
	struct MemRange* next;
	struct MemRange* range;

	if (!block) return;
	alloc->block = NULL;

	for (next = block->free; next && next->offset < offset; next = next->next) prev = next;

	if (prev && prev->offset + prev->size == offset) {
		prev->size += size;
		if (next && next->offset == offset + size) {
			prev->size += next->size;
			prev->next  = next->next;
			Mem_Free(next);
		}
		range = prev;
	} else if (next && next->offset == offset + size) {
		next->offset = offset;
		next->size  += size;
		range = next;
	} else {
		range = (struct MemRange*)Mem_Alloc(1, sizeof(struct MemRange), "Vulkan memory range");
		range->offset = offset;
		range->size   = size;
		range->next   = next;
		if (prev) { prev->next = range; } else { block->free = range; }
	}

	/* Oversized blocks only ever hold one big resource, so release them once it is freed */
	if (block->size <= MEM_BLOCK_SIZE || range->offset || range->size != block->size) return;
	for (link = &pool->blocks; *link != block; link = &(*link)->next) { }
	*link = block->next;
	MemBlock_Destroy(block);
}

static void MemPool_Destroy(struct MemPool* pool) {
	struct MemBlock* block;
	struct MemBlock* next;

	for (block = pool->blocks; block; block = next)
	{
		next = block->next;
		MemBlock_Destroy(block);
	}
	pool->blocks = NULL;
}

/* Deleted resources are only freed after the GPU is done with them, */
/*  so when out of memory, try again after waiting for the GPU to finish */
static cc_bool AllocMemory(struct MemPool* pool, const VkMemoryRequirements* reqs, struct MemAlloc* alloc) {
	int i;
	if (MemPool_Alloc(pool, reqs, alloc)) return true;

	vkDeviceWaitIdle(device);
	for (i = 0; i < FRAMES_IN_FLIGHT; i++)
	{
		if (&frames[i] != frame) Garbage_Collect(&frames[i]);
	}
	return MemPool_Alloc(pool, reqs, alloc);
}


/*########################################################################################################################*
*----------------------------------------------------Frame ring buffers---------------------------------------------------*
*#########################################################################################################################*/
static cc_bool RingChunk_Create(struct RingChunk* chunk, VkDeviceSize size) {
	VkBufferCreateInfo info = { 0 };
	VkMemoryRequirements reqs;
	void* data;
	int type;

	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size        = size;
	info.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	/* Instances for the top level acceleration structure are also written into the ring buffer */
	if (rtSupported) info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
								| VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	if (vkCreateBuffer(device, &info, NULL, &chunk->buffer)) return false;

	vkGetBufferMemoryRequirements(device, chunk->buffer, &reqs);
	type = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);

	if (type < 0 || AllocateMemory(&reqs, type, info.usage, &chunk->memory)) {
		vkDestroyBuffer(device, chunk->buffer, NULL);
		return false;
	}
	vkBindBufferMemory(device, chunk->buffer, chunk->memory, 0);
	chunk->address = GetBufferAddress(chunk->buffer, info.usage);

	if (vkMapMemory(device, chunk->memory, 0, size, 0, &data)) {
		vkDestroyBuffer(device, chunk->buffer, NULL);
		vkFreeMemory(device, chunk->memory, NULL);
		return false;
	}
	chunk->data = (cc_uint8*)data;
	chunk->size = size;
	chunk->used = 0;
	return true;
}

static void RingChunk_Destroy(struct RingChunk* chunk) {
	vkDestroyBuffer(device, chunk->buffer, NULL);
	vkFreeMemory(device, chunk->memory, NULL);
}

/* Allocates temp memory that stays valid until the GPU has finished rendering this frame */
static void* Ring_Alloc(VkDeviceSize size, VkBuffer* buffer, VkDeviceSize* offset) {
	struct RingChunk* chunk = NULL;
	VkDeviceSize start = 0;

	if (frame->numChunks) {
		chunk = &frame->chunks[frame->numChunks - 1];
		start = (chunk->used + 15) & ~(VkDeviceSize)15;
		if (start + size > chunk->size) chunk = NULL;
	}

	if (!chunk) {
		if (frame->numChunks == RING_MAX_CHUNKS) return NULL;
		chunk = &frame->chunks[frame->numChunks];
		if (!RingChunk_Create(chunk, max(size, RING_CHUNK_SIZE))) return NULL;
		frame->numChunks++;
		start = 0;
	}

	chunk->used = start + size;
	*buffer     = chunk->buffer;
	*offset     = start;
	return chunk->data + start;
}

/* Same as Ring_Alloc, but outputs the device address of the allocated memory instead */
static void* Ring_AllocAddress(VkDeviceSize size, VkDeviceAddress* address) {
	VkBuffer buffer;
	VkDeviceSize offset;
	void* data = Ring_Alloc(size, &buffer, &offset);

	/* Ring_Alloc always allocates from the most recently created chunk */
	if (data) *address = frame->chunks[frame->numChunks - 1].address + offset;
	return data;
}

/* Replaces multiple chunks with one bigger chunk, to avoid allocating new chunks every frame */
static void Ring_Reset(struct Frame* f) {
	VkDeviceSize used = 0;
	int i;

	if (f->numChunks <= 1) {
		if (f->numChunks) f->chunks[0].used = 0;
		return;
	}

	for (i = 0; i < f->numChunks; i++)
	{
		used += f->chunks[i].used;
		RingChunk_Destroy(&f->chunks[i]);
	}
	f->numChunks = 0;

	if (used > RING_MAX_KEEP) return;
	if (RingChunk_Create(&f->chunks[0], max(used, RING_CHUNK_SIZE))) f->numChunks = 1;
}


/*########################################################################################################################*
*----------------------------------------------------Deferred deletion----------------------------------------------------*
*#########################################################################################################################*/
/* Resources might still be in use by the GPU when the game deletes them, so they are */
/*  only actually destroyed once the frame they were deleted in has finished rendering */
static void FreeTexture(struct VulkanTexture* tex) {
	vkFreeDescriptorSets(device, tex->pool, 2, tex->sets);
	vkDestroyImageView(device, tex->view, NULL);
	vkDestroyImage(device, tex->image, NULL);
	MemAlloc_Free(&imagePool, &tex->alloc);
	Mem_Free(tex);
}

static void Garbage_Add(int type, void* obj) {
	struct Frame* f = frame;

	if (f->numGarbage == f->maxGarbage) {
		f->maxGarbage = f->maxGarbage ? f->maxGarbage * 2 : 64;
		f->garbage    = f->garbage ?
			(struct Garbage*)Mem_Realloc(f->garbage, f->maxGarbage, sizeof(struct Garbage), "Vulkan garbage") :
			(struct Garbage*)Mem_Alloc(f->maxGarbage, sizeof(struct Garbage), "Vulkan garbage");
	}
	f->garbage[f->numGarbage].type = type;
	f->garbage[f->numGarbage].obj  = obj;
	f->numGarbage++;
}

static void Garbage_Collect(struct Frame* f) {
	struct MemAlloc* alloc;
	int i;

	for (i = 0; i < f->numGarbage; i++)
	{
		if (f->garbage[i].type == GARBAGE_TEXTURE) {
			FreeTexture((struct VulkanTexture*)f->garbage[i].obj);
		} else if (f->garbage[i].type == GARBAGE_RING) {
			RingChunk_Destroy((struct RingChunk*)f->garbage[i].obj);
			Mem_Free(f->garbage[i].obj);
		} else if (f->garbage[i].type == GARBAGE_SCRATCH) {
			alloc = (struct MemAlloc*)f->garbage[i].obj;
			MemAlloc_Free(&scratchPool, alloc);
			Mem_Free(alloc);
		} else {
			alloc = (struct MemAlloc*)f->garbage[i].obj;
			if (alloc->rt) RT_FreeChunk(alloc->rt);
			MemAlloc_Free(&bufferPool, alloc);
			Mem_Free(alloc);
		}
	}
	f->numGarbage = 0;
}


/*########################################################################################################################*
*-----------------------------------------------------Command buffers-----------------------------------------------------*
*#########################################################################################################################*/
static void BeginCmd(VkCommandBuffer cmd) {
	VkCommandBufferBeginInfo info = { 0 };
	info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &info);
}

/* Copies are recorded into a separate command buffer, since textures/buffers can be */
/*  updated at any time (even in the middle of a render pass). This command buffer */
/*  is submitted before the draw command buffer, so copies always happen before drawing */
static VkCommandBuffer UploadCmd(void) {
	if (uploadBegun) return frame->uploadCmd;
	uploadBegun = true;

	BeginCmd(frame->uploadCmd);
	vkCmdPipelineBarrier(frame->uploadCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
							0, 0, NULL, 0, NULL, 0, NULL);
	return frame->uploadCmd;
}

static void EndUploadCmd(void) {
	VkMemoryBarrier barrier = { 0 };
	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

	vkCmdPipelineBarrier(frame->uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
							VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
							0, 1, &barrier, 0, NULL, 0, NULL);
	vkEndCommandBuffer(frame->uploadCmd);
}

static void ImageBarrier(VkCommandBuffer cmd, VkImage image, cc_uint32 levels,
						VkImageLayout oldLayout, VkImageLayout newLayout,
						VkFlags srcStage, VkFlags srcAccess, VkFlags dstStage, VkFlags dstAccess) {
	VkImageMemoryBarrier barrier = { 0 };
	barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask       = srcAccess;
	barrier.dstAccessMask       = dstAccess;
	barrier.oldLayout           = oldLayout;
	barrier.newLayout           = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = levels;
	barrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier);
}


/*########################################################################################################################*
*------------------------------------------------------Initialisation-----------------------------------------------------*
*#########################################################################################################################*/
static void LoadVulkanLibrary(void) {
	static const cc_string path = String_FromConst("vulkan-1.dll");
	void* lib;

	if (DynamicLib_LoadAll(&path, vk_funcs, Array_Elems(vk_funcs), &lib)) return;
	Logger_FailToStart("Failed to load vulkan-1.dll. You may need to update your graphics drivers.\n\nNOTE: Vulkan requires a GPU with Vulkan 1.1 support\nYou may need to use the Direct3D9 version instead.\n");
}

static void CreateInstance(void) {
	static const char* const extensions[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
	VkApplicationInfo app     = { 0 };
	VkInstanceCreateInfo info = { 0 };
	VkResult res;

	app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "ClassiCube";
	app.pEngineName      = "ClassiCube";
	/* 1.1 is needed for negative viewport height (see Gfx_SetViewport) */
	app.apiVersion       = VK_API_VERSION_1_1;

	info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo        = &app;
	info.enabledExtensionCount   = Array_Elems(extensions);
	info.ppEnabledExtensionNames = extensions;

	res = vkCreateInstance(&info, NULL, &instance);
	if (res == VK_ERROR_INCOMPATIBLE_DRIVER)
		Logger_FailToStart("Your graphics drivers do not support Vulkan 1.1.\n\nYou may need to use the Direct3D9 version instead.\n");
	if (res) Process_Abort2(res, "Failed to create Vulkan instance");
}

/* TODO: Support other window backends besides Win32 */
static void CreateSurface(void) {
	VkWin32SurfaceCreateInfoKHR info = { 0 };
	VkResult res;

	info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	info.hinstance = GetModuleHandleW(NULL);
	info.hwnd      = Window_Main.Handle.ptr;

	res = vkCreateWin32SurfaceKHR(instance, &info, NULL, &surface);
	if (res) Process_Abort2(res, "Failed to create Vulkan surface");
}

static cc_bool FindQueueFamily(VkPhysicalDevice dev, cc_uint32* family) {
	VkQueueFamilyProperties families[16];
	cc_uint32 i, count = Array_Elems(families);
	VkBool32 present;

	vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families);
	for (i = 0; i < count; i++)
	{
		if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;

		present = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
		if (!present) continue;

		*family = i;
		return true;
	}
	return false;
}

/* Dedicated GPUs are preferred over integrated GPUs */
static int RateDevice(VkPhysicalDevice dev) {
	VkPhysicalDeviceProperties p;
	cc_uint32 family;

	vkGetPhysicalDeviceProperties(dev, &p);
	if (p.apiVersion < VK_API_VERSION_1_1) return 0;
	if (!FindQueueFamily(dev, &family))    return 0;

	if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   return 3;
	if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return 2;
	return 1;
}

static void PickPhysicalDevice(void) {
	VkPhysicalDevice devices[16];
	cc_uint32 i, count = Array_Elems(devices);
	int score, best = 0;

	vkEnumeratePhysicalDevices(instance, &count, devices);
	for (i = 0; i < count; i++)
	{
		score = RateDevice(devices[i]);
		if (score <= best) continue;

		best       = score;
		physDevice = devices[i];
	}

	if (!best) Logger_FailToStart("Failed to find a GPU that supports Vulkan 1.1.\n\nYou may need to use the Direct3D9 version instead.\n");
	FindQueueFamily(physDevice, &queueFamily);
	vkGetPhysicalDeviceProperties(physDevice, &props);
	vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
}

static cc_bool HasDeviceExtension(const char* name) {
	VkExtensionProperties* exts;
	cc_uint32 i, count = 0;
	cc_bool found = false;
	cc_string ext;

	vkEnumerateDeviceExtensionProperties(physDevice, NULL, &count, NULL);
	if (!count) return false;

	exts = (VkExtensionProperties*)Mem_Alloc(count, sizeof(VkExtensionProperties), "Vulkan extensions");
	vkEnumerateDeviceExtensionProperties(physDevice, NULL, &count, exts);

	for (i = 0; i < count && !found; i++)
	{
		ext   = String_FromRawArray(exts[i].extensionName);
		found = String_CaselessEqualsConst(&ext, name);
	}
	Mem_Free(exts);
	return found;
}

/* Ray queries need all of these extensions. Since the instance only uses Vulkan 1.1, */
/*  the extensions which were later promoted to Vulkan 1.2 have to be enabled too */
static const char* const rtExtensions[] = {
	"VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_deferred_host_operations",
	"VK_KHR_buffer_device_address",  "VK_EXT_descriptor_indexing",
	"VK_KHR_spirv_1_4",              "VK_KHR_shader_float_controls"
};

static cc_bool CheckRTSupport(void) {
	VkPhysicalDeviceFeatures2 features                  = { 0 };
	VkPhysicalDeviceBufferDeviceAddressFeatures address = { 0 };
	VkPhysicalDeviceAccelerationStructureFeaturesKHR as = { 0 };
	VkPhysicalDeviceRayQueryFeaturesKHR rayQuery        = { 0 };
	int i;

	for (i = 0; i < Array_Elems(rtExtensions); i++)
	{
		if (!HasDeviceExtension(rtExtensions[i])) return false;
	}

	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &address;
	address.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	address.pNext  = &as;
	as.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	as.pNext       = &rayQuery;
	rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

	vkGetPhysicalDeviceFeatures2(physDevice, &features);
	/* features[3] is independentBlend, see CreateDevice */
	return address.bufferDeviceAddress && as.accelerationStructure && rayQuery.rayQuery && features.features[3];
}

static void LoadRTFunctions(void) {
	VkPhysicalDeviceProperties2 properties2                   = { 0 };
	VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps = { 0 };
	int i;

	for (i = 0; i < Array_Elems(rt_funcs); i++)
	{
		*rt_funcs[i].symAddr = (void*)vkGetDeviceProcAddr(device, rt_funcs[i].name);
		if (!(*rt_funcs[i].symAddr)) rtSupported = false;
	}

	properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	properties2.pNext = &asProps;
	asProps.sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

	vkGetPhysicalDeviceProperties2(physDevice, &properties2);
	rtScratchAlign = max(asProps.minAccelerationStructureScratchOffsetAlignment, 256);
}

static void CreateDevice(void) {
	static const float priority = 1.0f;
	const char* extensions[2 + Array_Elems(rtExtensions)];
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicState = { 0 };
	VkPhysicalDeviceBufferDeviceAddressFeatures address = { 0 };
	VkPhysicalDeviceAccelerationStructureFeaturesKHR as = { 0 };
	VkPhysicalDeviceRayQueryFeaturesKHR rayQuery        = { 0 };
	VkDeviceQueueCreateInfo queueInfo = { 0 };
	VkDeviceCreateInfo info = { 0 };
	VkBool32 features[55]   = { 0 };
	cc_uint32 i, count = 0;
	VkResult res;

	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &priority;
	extensions[count++]        = "VK_KHR_swapchain";

	/* Ray tracing features are always enabled when supported, so that turning */
	/*  on ray traced shadows doesn't require recreating the Vulkan device */
	rtSupported = CheckRTSupport();
	if (rtSupported) {
		for (i = 0; i < Array_Elems(rtExtensions); i++) { extensions[count++] = rtExtensions[i]; }

		address.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		address.pNext  = &as;
		address.bufferDeviceAddress = true;
		as.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
		as.pNext       = &rayQuery;
		as.accelerationStructure = true;
		rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
		rayQuery.rayQuery = true;
		info.pNext     = &address;
		/* Only chunk pipelines write to the shadow history attachment, so other pipelines */
		/*  need a different colour write mask for it (i.e. features[3] = independentBlend) */
		features[3]    = true;
	}

	/* Implementations supporting this extension must also support the extendedDynamicState feature */
	dynamicCull = HasDeviceExtension("VK_EXT_extended_dynamic_state");
	if (dynamicCull) {
		extensions[count++] = "VK_EXT_extended_dynamic_state";
		dynamicState.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
		dynamicState.pNext  = (void*)info.pNext;
		dynamicState.extendedDynamicState = true;
		info.pNext = &dynamicState;
	}

	info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.queueCreateInfoCount    = 1;
	info.pQueueCreateInfos       = &queueInfo;
	info.enabledExtensionCount   = count;
	info.ppEnabledExtensionNames = extensions;
	info.pEnabledFeatures        = features;

	res = vkCreateDevice(physDevice, &info, NULL, &device);
	if (res) Process_Abort2(res, "Failed to create Vulkan device");
	vkGetDeviceQueue(device, queueFamily, 0, &queue);

	/* Extension functions aren't exported by vulkan-1.dll, so have to be loaded separately */
	if (rtSupported) LoadRTFunctions();
	if (!dynamicCull) return;
	vkCmdSetCullModeEXT = (void (VKAPI_PTR *)(VkCommandBuffer, VkFlags))vkGetDeviceProcAddr(device, "vkCmdSetCullModeEXT");
	dynamicCull         = vkCmdSetCullModeEXT != NULL;
}

/* B8G8R8A8 is preferred since it matches BitmapCol, so screenshots don't need converting */
/* For depth, 32 bit float gives the best precision with a reversed depth buffer */
static void ChooseFormats(void) {
	static const VkFormat depthFormats[] = {
		VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM
	};
	VkSurfaceFormatKHR formats[64];
	VkFormatProperties fmtProps;
	cc_uint32 i, count = Array_Elems(formats);

	vkGetPhysicalDeviceSurfaceFormatsKHR(physDevice, surface, &count, formats);
	colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
	colorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

	if (count && formats[0].format != VK_FORMAT_UNDEFINED) {
		colorFormat = formats[0].format;
		colorSpace  = formats[0].colorSpace;
	}

	for (i = 0; i < count; i++)
	{
		if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
		if (formats[i].format != VK_FORMAT_B8G8R8A8_UNORM && formats[i].format != VK_FORMAT_R8G8B8A8_UNORM) continue;

		colorFormat = formats[i].format;
		colorSpace  = formats[i].colorSpace;
		if (colorFormat == VK_FORMAT_B8G8R8A8_UNORM) break;
	}

	for (i = 0; i < Array_Elems(depthFormats); i++)
	{
		vkGetPhysicalDeviceFormatProperties(physDevice, depthFormats[i], &fmtProps);
		if (!(fmtProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) continue;

		depthFormat = depthFormats[i];
		return;
	}
	Process_Abort("No supported Vulkan depth buffer format");
}

/* Normal render pass doesn't load existing contents, since Gfx_ClearBuffers is always called first */
/*  Resume render pass loads existing contents, for continuing a frame after taking a screenshot */
/* When ray traced shadows are supported, there's also a 2nd colour attachment for the shadow history */
/*  (which is cleared at the start of each frame, unless history is false) */
static VkRenderPass CreateRenderPass(cc_bool resume, cc_bool history) {
	VkAttachmentDescription attachments[5] = { { 0 } };
	VkSubpassDependency deps[2]       = { { 0 } };
	VkSubpassDescription subpass      = { 0 };
	VkRenderPassCreateInfo info       = { 0 };
	VkAttachmentReference colorRefs[4], depthRef;
	VkRenderPass pass;
	VkResult res;

	attachments[0].format         = colorFormat;
	attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp         = resume ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout  = resume ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	attachments[1].format         = depthFormat;
	attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp         = resume ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout  = resume ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	attachments[2].format         = HISTORY_FORMAT;
	attachments[2].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[2].loadOp         = resume ? VK_ATTACHMENT_LOAD_OP_LOAD : (history ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	attachments[2].storeOp        = (resume || history) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[2].initialLayout  = resume ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	/* The next frame reads it in fragment shaders */
	attachments[2].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	attachments[3] = attachments[2];
	attachments[4] = attachments[2];
	attachments[4].format = HistoryFormat(2);

	colorRefs[0].attachment = 0;
	colorRefs[0].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorRefs[1].attachment = 2;
	colorRefs[1].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorRefs[2].attachment = 3;
	colorRefs[2].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorRefs[3].attachment = 4;
	colorRefs[3].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depthRef.attachment     = 1;
	depthRef.layout         = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount    = rtSupported ? 4 : 1;
	subpass.pColorAttachments       = colorRefs;
	subpass.pDepthStencilAttachment = &depthRef;

	/* Depth buffer is shared between frames, so wait for previous frame to finish writing to it */
	/* Shadow history written by the previous frame is read by this frame's fragment shaders, and */
	/*  this frame overwrites the history image that the previous frame's fragment shaders read */
	deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass    = 0;
	deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
							VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
							VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
							VK_ACCESS_SHADER_READ_BIT;

	/* Allows copying the swapchain image afterwards for screenshots */
	deps[1].srcSubpass    = 0;
	deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = rtSupported ? 5 : 2;
	info.pAttachments    = attachments;
	info.subpassCount    = 1;
	info.pSubpasses      = &subpass;
	info.dependencyCount = 2;
	info.pDependencies   = deps;

	res = vkCreateRenderPass(device, &info, NULL, &pass);
	if (res) Process_Abort2(res, "Failed to create Vulkan render pass");
	return pass;
}

static void CreateFrames(void) {
	VkCommandPoolCreateInfo poolInfo       = { 0 };
	VkCommandBufferAllocateInfo allocInfo  = { 0 };
	VkFenceCreateInfo fenceInfo            = { 0 };
	VkSemaphoreCreateInfo semInfo          = { 0 };
	VkCommandBuffer cmds[2];
	struct Frame* f;
	VkResult res;
	int i;

	poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = queueFamily;

	allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 2;

	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	semInfo.sType   = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	for (i = 0; i < FRAMES_IN_FLIGHT; i++)
	{
		f   = &frames[i];
		res = vkCreateCommandPool(device, &poolInfo, NULL, &f->pool);
		if (res) Process_Abort2(res, "Failed to create Vulkan command pool");

		allocInfo.commandPool = f->pool;
		res = vkAllocateCommandBuffers(device, &allocInfo, cmds);
		if (res) Process_Abort2(res, "Failed to allocate Vulkan command buffers");
		f->uploadCmd = cmds[0];
		f->drawCmd   = cmds[1];

		res = vkCreateFence(device, &fenceInfo, NULL, &f->fence);
		if (res) Process_Abort2(res, "Failed to create Vulkan fence");
		res = vkCreateSemaphore(device, &semInfo, NULL, &f->acquireSem);
		if (res) Process_Abort2(res, "Failed to create Vulkan semaphore");
	}

	frameIndex = 0;
	frame      = &frames[0];
	/* Fences start signalled so NextFrame doesn't wait forever, except for the current frame */
	vkWaitForFences(device, 1, &frame->fence, true, WAIT_FOREVER);
	vkResetFences(device, 1, &frame->fence);
}

static void DestroyFrame(struct Frame* f) {
	int i;
	Garbage_Collect(f);
	RT_FreeTLAS(f);
	for (i = 0; i < f->numChunks; i++) RingChunk_Destroy(&f->chunks[i]);

	vkDestroyCommandPool(device, f->pool, NULL);
	vkDestroyFence(device, f->fence, NULL);
	vkDestroySemaphore(device, f->acquireSem, NULL);
	Mem_Free(f->garbage);
	Mem_Set(f, 0, sizeof(struct Frame));
}


/*########################################################################################################################*
*--------------------------------------------------------Swapchain--------------------------------------------------------*
*#########################################################################################################################*/
static void DestroySwapchainResources(void) {
	cc_uint32 i, j;
	for (i = 0; i < swapCount; i++)
	{
		for (j = 0; j < FRAMES_IN_FLIGHT; j++)
		{
			if (framebuffers[i][j]) vkDestroyFramebuffer(device, framebuffers[i][j], NULL);
			framebuffers[i][j] = 0;
		}
		vkDestroyImageView(device, swapViews[i], NULL);
		vkDestroySemaphore(device, renderSems[i], NULL);
	}
	swapCount = 0;

	for (i = 0; i < HISTORY_IMAGES; i++)
	{
		for (j = 0; j < FRAMES_IN_FLIGHT; j++)
		{
			if (historyViews[i][j])  vkDestroyImageView(device, historyViews[i][j], NULL);
			if (historyImages[i][j]) vkDestroyImage(device, historyImages[i][j], NULL);
			MemAlloc_Free(&imagePool, &historyAllocs[i][j]);
			historyViews[i][j]  = 0;
			historyImages[i][j] = 0;
		}
	}

	/* Depth buffer doesn't exist when the window was minimised */
	if (depthView)  vkDestroyImageView(device, depthView, NULL);
	if (depthImage) vkDestroyImage(device, depthImage, NULL);
	MemAlloc_Free(&imagePool, &depthAlloc);
	depthView  = 0;
	depthImage = 0;
}

/* Immediate is preferred over mailbox when not using vsync, since mailbox */
/*  is still capped to a multiple of the refresh rate by some drivers */
static VkPresentModeKHR ChoosePresentMode(void) {
	VkPresentModeKHR modes[16];
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	cc_uint32 i, count = Array_Elems(modes);
	if (gfx_vsync) return mode;

	vkGetPhysicalDeviceSurfacePresentModesKHR(physDevice, surface, &count, modes);
	for (i = 0; i < count; i++)
	{
		if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return modes[i];
		if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)   mode = modes[i];
	}
	return mode;
}

static void CreateDepthBuffer(void) {
	VkImageCreateInfo info         = { 0 };
	VkImageViewCreateInfo viewInfo = { 0 };
	VkMemoryRequirements reqs;
	VkResult res;

	info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType     = VK_IMAGE_TYPE_2D;
	info.format        = depthFormat;
	info.extent.width  = swapExtent.width;
	info.extent.height = swapExtent.height;
	info.extent.depth  = 1;
	info.mipLevels     = 1;
	info.arrayLayers   = 1;
	info.samples       = VK_SAMPLE_COUNT_1_BIT;
	info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	res = vkCreateImage(device, &info, NULL, &depthImage);
	if (res) Process_Abort2(res, "Failed to create Vulkan depth buffer");

	vkGetImageMemoryRequirements(device, depthImage, &reqs);
	if (!AllocMemory(&imagePool, &reqs, &depthAlloc)) Process_Abort("Out of video memory! (allocating depth buffer)");
	vkBindImageMemory(device, depthImage, depthAlloc.block->memory, depthAlloc.offset);

	viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image    = depthImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format   = depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	if (depthFormat == VK_FORMAT_D24_UNORM_S8_UINT) viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

	res = vkCreateImageView(device, &viewInfo, NULL, &depthView);
	if (res) Process_Abort2(res, "Failed to create Vulkan depth buffer view");
}

/* Each frame slot's ray tracing descriptor set reads the previous frame's history images */
static void CreateHistoryImages(void) {
	VkImageCreateInfo info         = { 0 };
	VkImageViewCreateInfo viewInfo = { 0 };
	VkWriteDescriptorSet write     = { 0 };
	VkDescriptorImageInfo imageInfo;
	VkMemoryRequirements reqs;
	VkResult res;
	int i, j;

	info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType     = VK_IMAGE_TYPE_2D;
	info.format        = HISTORY_FORMAT;
	info.extent.width  = swapExtent.width;
	info.extent.height = swapExtent.height;
	info.extent.depth  = 1;
	info.mipLevels     = 1;
	info.arrayLayers   = 1;
	info.samples       = VK_SAMPLE_COUNT_1_BIT;
	info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format   = HISTORY_FORMAT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	for (i = 0; i < HISTORY_IMAGES; i++)
	{
		info.format     = HistoryFormat(i);
		viewInfo.format = HistoryFormat(i);

		for (j = 0; j < FRAMES_IN_FLIGHT; j++)
		{
			res = vkCreateImage(device, &info, NULL, &historyImages[i][j]);
			if (res) Process_Abort2(res, "Failed to create Vulkan history image");

			vkGetImageMemoryRequirements(device, historyImages[i][j], &reqs);
			if (!AllocMemory(&imagePool, &reqs, &historyAllocs[i][j])) Process_Abort("Out of video memory! (allocating ray tracing history)");
			vkBindImageMemory(device, historyImages[i][j], historyAllocs[i][j].block->memory, historyAllocs[i][j].offset);

			viewInfo.image = historyImages[i][j];
			res = vkCreateImageView(device, &viewInfo, NULL, &historyViews[i][j]);
			if (res) Process_Abort2(res, "Failed to create Vulkan history view");

		}
	}
	/* Contents are undefined until a frame draws chunks, but the images still have to be in the right */
	/*  layout, since the descriptor sets referencing them might be bound before then (see BeginRenderPass) */
	historyFresh = true;

	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo      = &imageInfo;

	for (i = 0; i < HISTORY_IMAGES; i++)
	{
		for (j = 0; j < FRAMES_IN_FLIGHT; j++)
		{
			/* Only read using texelFetch, so the sampler doesn't matter */
			imageInfo.sampler     = samplers[0];
			imageInfo.imageView   = historyViews[i][(j + FRAMES_IN_FLIGHT - 1) % FRAMES_IN_FLIGHT];
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			write.dstSet          = frames[j].rtSet;
			write.dstBinding      = 2 + i;
			vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
		}
	}
	/* Previous frame's history is gone, including for the frame currently being recorded */
	/*  (whose parameters were already set in RT_BeginFrame, but haven't been used by the GPU yet) */
	rtPrevDrawn = false;
	for (j = 0; j < FRAMES_IN_FLIGHT; j++)
	{
		struct RTParams* p = (struct RTParams*)(rtParams.data + j * RT_PARAMS_STRIDE);
		p->hasPrev = false;
		p->screenW = (float)swapExtent.width;
		p->screenH = (float)swapExtent.height;
	}
}

static void CreateSwapchain(void) {
	VkSwapchainCreateInfoKHR info  = { 0 };
	VkImageViewCreateInfo viewInfo = { 0 };
	VkFramebufferCreateInfo fbInfo = { 0 };
	VkSemaphoreCreateInfo semInfo  = { 0 };
	VkSwapchainKHR old = swapchain;
	VkSurfaceCapabilitiesKHR caps;
	VkImageView attachments[5];
	cc_uint32 i, j, count;
	VkResult res;

	swapchainDirty = false;
	swapchain      = 0;

	res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDevice, surface, &caps);
	if (res) {
		if (old) vkDestroySwapchainKHR(device, old, NULL);
		HandleError(res, "Failed to query Vulkan surface");
		return;
	}

	swapExtent = caps.currentExtent;
	/* 0xFFFFFFFF means the swapchain size determines the surface size */
	if (swapExtent.width == 0xFFFFFFFFu) {
		swapExtent.width  = Game.Width;
		swapExtent.height = Game.Height;
		Math_Clamp(swapExtent.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
		Math_Clamp(swapExtent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
	}
	/* Swapchain can't be created while the window is minimised */
	if (!swapExtent.width || !swapExtent.height) {
		if (old) vkDestroySwapchainKHR(device, old, NULL);
		return;
	}

	count = caps.minImageCount + 1;
	if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
	swapUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface          = surface;
	info.minImageCount    = count;
	info.imageFormat      = colorFormat;
	info.imageColorSpace  = colorSpace;
	info.imageExtent      = swapExtent;
	info.imageArrayLayers = 1;
	info.imageUsage       = swapUsage;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform     = caps.currentTransform;
	info.compositeAlpha   = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ?
								VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	info.presentMode      = ChoosePresentMode();
	info.clipped          = true;
	info.oldSwapchain     = old;

	res = vkCreateSwapchainKHR(device, &info, NULL, &swapchain);
	/* NOTE: Some implicit layers (e.g. Bandicam) crash if a NULL swapchain is destroyed, */
	/*  even though destroying NULL handles is valid in Vulkan */
	if (old) vkDestroySwapchainKHR(device, old, NULL);
	if (res) { swapchain = 0; HandleError(res, "Failed to create Vulkan swapchain"); return; }

	count = MAX_SWAP_IMAGES;
	vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages);
	CreateDepthBuffer();
	if (rtSupported) CreateHistoryImages();

	viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format   = colorFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbInfo.renderPass      = renderPass;
	fbInfo.attachmentCount = rtSupported ? 5 : 2;
	fbInfo.pAttachments    = attachments;
	fbInfo.width           = swapExtent.width;
	fbInfo.height          = swapExtent.height;
	fbInfo.layers          = 1;
	semInfo.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	for (i = 0; i < count; i++)
	{
		viewInfo.image = swapImages[i];
		res = vkCreateImageView(device, &viewInfo, NULL, &swapViews[i]);
		if (res) Process_Abort2(res, "Failed to create Vulkan swapchain view");

		/* With ray traced shadows, each frame slot writes to a different history image */
		for (j = 0; j < (rtSupported ? FRAMES_IN_FLIGHT : 1); j++)
		{
			attachments[0] = swapViews[i];
			attachments[1] = depthView;
			attachments[2] = historyViews[0][j];
			attachments[3] = historyViews[1][j];
			attachments[4] = historyViews[2][j];
			res = vkCreateFramebuffer(device, &fbInfo, NULL, &framebuffers[i][j]);
			if (res) Process_Abort2(res, "Failed to create Vulkan framebuffer");
		}

		res = vkCreateSemaphore(device, &semInfo, NULL, &renderSems[i]);
		if (res) Process_Abort2(res, "Failed to create Vulkan semaphore");
	}
	swapCount = count;
}

static void RecreateSwapchain(void) {
	vkDeviceWaitIdle(device);
	DestroySwapchainResources();
	CreateSwapchain();
}


/*########################################################################################################################*
*--------------------------------------------------------Pipelines--------------------------------------------------------*
*#########################################################################################################################*/
/* SPIR-V is generated using misc/vulkan/VulkanShaderGen.c */
static const struct ShaderDesc vs_descs[] = {
	{ vs_colored,  sizeof(vs_colored)  },
	{ vs_textured, sizeof(vs_textured) },
};
static const struct ShaderDesc fs_descs[] = {
	{ fs_colored,               sizeof(fs_colored)               },
	{ fs_textured,              sizeof(fs_textured)              },
	{ fs_colored_test,          sizeof(fs_colored_test)          },
	{ fs_textured_test,         sizeof(fs_textured_test)         },
	{ fs_colored_linear,        sizeof(fs_colored_linear)        },
	{ fs_textured_linear,       sizeof(fs_textured_linear)       },
	{ fs_colored_test_linear,   sizeof(fs_colored_test_linear)   },
	{ fs_textured_test_linear,  sizeof(fs_textured_test_linear)  },
	{ fs_colored_density,       sizeof(fs_colored_density)       },
	{ fs_textured_density,      sizeof(fs_textured_density)      },
	{ fs_colored_test_density,  sizeof(fs_colored_test_density)  },
	{ fs_textured_test_density, sizeof(fs_textured_test_density) },
};
/* Indexed by alpha test + fog mode * 2 */
static const struct ShaderDesc fs_rt_descs[] = {
	{ fs_textured_rt,              sizeof(fs_textured_rt)              },
	{ fs_textured_test_rt,         sizeof(fs_textured_test_rt)         },
	{ fs_textured_linear_rt,       sizeof(fs_textured_linear_rt)       },
	{ fs_textured_test_linear_rt,  sizeof(fs_textured_test_linear_rt)  },
	{ fs_textured_density_rt,      sizeof(fs_textured_density_rt)      },
	{ fs_textured_test_density_rt, sizeof(fs_textured_test_density_rt) },
};
/* Indexed by fog mode (alpha tested, with early depth testing, see KEY_AFTER_PREPASS) */
static const struct ShaderDesc fs_early_descs[] = {
	{ fs_textured_test_early_rt,         sizeof(fs_textured_test_early_rt)         },
	{ fs_textured_test_linear_early_rt,  sizeof(fs_textured_test_linear_early_rt)  },
	{ fs_textured_test_density_early_rt, sizeof(fs_textured_test_density_early_rt) },
};
/* Indexed by vertex format + fog mode * 2 */
static const struct ShaderDesc fs_sky_descs[] = {
	{ fs_colored_sky_rt,          sizeof(fs_colored_sky_rt)          },
	{ fs_textured_sky_rt,         sizeof(fs_textured_sky_rt)         },
	{ fs_colored_sky_linear_rt,   sizeof(fs_colored_sky_linear_rt)   },
	{ fs_textured_sky_linear_rt,  sizeof(fs_textured_sky_linear_rt)  },
	{ fs_colored_sky_density_rt,  sizeof(fs_colored_sky_density_rt)  },
	{ fs_textured_sky_density_rt, sizeof(fs_textured_sky_density_rt) },
};
static const struct ShaderDesc vs_rt_descs[] = {
	{ vs_colored_rt,  sizeof(vs_colored_rt)  },
	{ vs_textured_rt, sizeof(vs_textured_rt) },
};

static VkShaderModule CreateShader(const struct ShaderDesc* desc) {
	VkShaderModuleCreateInfo info = { 0 };
	VkShaderModule module;
	VkResult res;

	info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = desc->len;
	info.pCode    = desc->data;

	res = vkCreateShaderModule(device, &info, NULL, &module);
	if (res) Process_Abort2(res, "Failed to create Vulkan shader");
	return module;
}

/* Set 1 contains the top level acceleration structure for ray traced shadows (binding 0), */
/*  struct RTParams (binding 1), and the previous frame's history images (bindings 2 and 3) */
static void CreateRTDescriptors(void) {
	VkDescriptorSetLayoutBinding bindings[5] = { { 0 } };
	VkDescriptorSetLayoutCreateInfo setInfo  = { 0 };
	VkDescriptorPoolCreateInfo poolInfo      = { 0 };
	VkDescriptorSetAllocateInfo allocInfo    = { 0 };
	VkDescriptorBufferInfo bufferInfo;
	VkWriteDescriptorSet write = { 0 };
	VkDescriptorPoolSize sizes[3];
	VkResult res;
	int i;

	bindings[0].binding         = 0;
	bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding         = 1;
	bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[2].binding         = 2;
	bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[3] = bindings[2];
	bindings[3].binding         = 3;
	bindings[4] = bindings[2];
	bindings[4].binding         = 4;

	setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = 5;
	setInfo.pBindings    = bindings;

	res = vkCreateDescriptorSetLayout(device, &setInfo, NULL, &rtSetLayout);
	if (res) Process_Abort2(res, "Failed to create Vulkan ray tracing descriptor set layout");

	sizes[0].type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	sizes[0].descriptorCount = FRAMES_IN_FLIGHT;
	sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[1].descriptorCount = FRAMES_IN_FLIGHT;
	sizes[2].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[2].descriptorCount = FRAMES_IN_FLIGHT * HISTORY_IMAGES;

	poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets       = FRAMES_IN_FLIGHT;
	poolInfo.poolSizeCount = 3;
	poolInfo.pPoolSizes    = sizes;

	res = vkCreateDescriptorPool(device, &poolInfo, NULL, &rtDescPool);
	if (res) Process_Abort2(res, "Failed to create Vulkan ray tracing descriptor pool");

	allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool     = rtDescPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts        = &rtSetLayout;

	if (!RingChunk_Create(&rtParams, RT_PARAMS_STRIDE * FRAMES_IN_FLIGHT))
		Process_Abort("Failed to create Vulkan ray tracing parameters buffer");

	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstBinding      = 1;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	write.pBufferInfo     = &bufferInfo;

	for (i = 0; i < FRAMES_IN_FLIGHT; i++)
	{
		res = vkAllocateDescriptorSets(device, &allocInfo, &frames[i].rtSet);
		if (res) Process_Abort2(res, "Failed to allocate Vulkan ray tracing descriptor set");
		Mem_Set(rtParams.data + i * RT_PARAMS_STRIDE, 0, RT_PARAMS_STRIDE);

		bufferInfo.buffer = rtParams.buffer;
		bufferInfo.offset = i * RT_PARAMS_STRIDE;
		bufferInfo.range  = sizeof(struct RTParams);
		write.dstSet      = frames[i].rtSet;
		vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
	}

	for (i = 0; i < Array_Elems(vsRtModules);  i++) vsRtModules[i]  = CreateShader(&vs_rt_descs[i]);
	for (i = 0; i < Array_Elems(fsRtModules);  i++) fsRtModules[i]  = CreateShader(&fs_rt_descs[i]);
	for (i = 0; i < Array_Elems(fsSkyModules); i++) fsSkyModules[i] = CreateShader(&fs_sky_descs[i]);
	for (i = 0; i < Array_Elems(fsEarlyModules); i++) fsEarlyModules[i] = CreateShader(&fs_early_descs[i]);
}

static void CreatePipelineObjects(void) {
	VkDescriptorSetLayoutBinding binding   = { 0 };
	VkDescriptorSetLayoutCreateInfo setInfo = { 0 };
	VkPipelineLayoutCreateInfo layoutInfo  = { 0 };
	VkSamplerCreateInfo samplerInfo        = { 0 };
	VkDescriptorSetLayout setLayouts[2];
	VkPushConstantRange range;
	VkResult res;
	int i;

	for (i = 0; i < Array_Elems(vsModules); i++) vsModules[i] = CreateShader(&vs_descs[i]);
	for (i = 0; i < Array_Elems(fsModules); i++) fsModules[i] = CreateShader(&fs_descs[i]);

	binding.binding         = 0;
	binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

	setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = 1;
	setInfo.pBindings    = &binding;

	res = vkCreateDescriptorSetLayout(device, &setInfo, NULL, &setLayout);
	if (res) Process_Abort2(res, "Failed to create Vulkan descriptor set layout");
	if (rtSupported) CreateRTDescriptors();

	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	range.offset     = 0;
	range.size       = sizeof(pc);

	setLayouts[0] = setLayout;
	setLayouts[1] = rtSetLayout;

	layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount         = rtSupported ? 2 : 1;
	layoutInfo.pSetLayouts            = setLayouts;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges    = &range;

	res = vkCreatePipelineLayout(device, &layoutInfo, NULL, &pipelineLayout);
	if (res) Process_Abort2(res, "Failed to create Vulkan pipeline layout");

	samplerInfo.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.addressModeU  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.compareOp     = VK_COMPARE_OP_NEVER;
	samplerInfo.maxLod        = VK_LOD_CLAMP_NONE;
	samplerInfo.borderColor   = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

	for (i = 0; i < Array_Elems(samplers); i++)
	{
		/* 0 = nearest, 1 = nearest + mipmaps, 2 = bilinear, 3 = bilinear + mipmaps */
		samplerInfo.magFilter  = (i & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		samplerInfo.minFilter  = (i & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		samplerInfo.mipmapMode = (i & 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

		res = vkCreateSampler(device, &samplerInfo, NULL, &samplers[i]);
		if (res) Process_Abort2(res, "Failed to create Vulkan sampler");
	}
}

static void DestroyPipelineObjects(void) {
	int i;
	for (i = 0; i < PIPELINE_KEYS; i++)
	{
		if (pipelines[i]) vkDestroyPipeline(device, pipelines[i], NULL);
		pipelines[i] = 0;
	}
	for (i = 0; i < Array_Elems(vsModules); i++) vkDestroyShaderModule(device, vsModules[i], NULL);
	for (i = 0; i < Array_Elems(fsModules); i++) vkDestroyShaderModule(device, fsModules[i], NULL);
	for (i = 0; i < Array_Elems(samplers);  i++) vkDestroySampler(device, samplers[i], NULL);
	for (i = 0; i < numDescPools; i++) vkDestroyDescriptorPool(device, descPools[i], NULL);

	Mem_Free(descPools);
	descPools    = NULL;
	numDescPools = 0;
	vkDestroyPipelineLayout(device, pipelineLayout, NULL);
	vkDestroyDescriptorSetLayout(device, setLayout, NULL);

	if (!rtSupported) return;
	for (i = 0; i < Array_Elems(vsRtModules);  i++) vkDestroyShaderModule(device, vsRtModules[i],  NULL);
	for (i = 0; i < Array_Elems(fsRtModules);  i++) vkDestroyShaderModule(device, fsRtModules[i],  NULL);
	for (i = 0; i < Array_Elems(fsEarlyModules); i++) vkDestroyShaderModule(device, fsEarlyModules[i], NULL);
	for (i = 0; i < Array_Elems(fsSkyModules); i++) vkDestroyShaderModule(device, fsSkyModules[i], NULL);
	vkDestroyDescriptorPool(device, rtDescPool, NULL);
	vkDestroyDescriptorSetLayout(device, rtSetLayout, NULL);
	RingChunk_Destroy(&rtParams);
}

/* Pipelines are created on demand for each combination of render state (see CalcPipelineKey) */
static VkPipeline CreatePipeline(int key) {
	static const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE_EXT };
	VkPipelineShaderStageCreateInfo stages[2]            = { { 0 } };
	VkVertexInputAttributeDescription attribs[3]         = { { 0 } };
	VkVertexInputBindingDescription binding              = { 0 };
	VkPipelineVertexInputStateCreateInfo vertexInput     = { 0 };
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { 0 };
	VkPipelineViewportStateCreateInfo viewportState      = { 0 };
	VkPipelineRasterizationStateCreateInfo raster        = { 0 };
	VkPipelineMultisampleStateCreateInfo multisample     = { 0 };
	VkPipelineDepthStencilStateCreateInfo depth          = { 0 };
	VkPipelineColorBlendAttachmentState blendAttachments[4] = { { 0 } };
	VkPipelineColorBlendStateCreateInfo blend            = { 0 };
	VkPipelineDynamicStateCreateInfo dynamic             = { 0 };
	VkGraphicsPipelineCreateInfo info                    = { 0 };
	VkPipeline pipeline;
	VkResult res;
	int fmt = key & 1, rt = (key >> 13) & 1, sky = (key >> 14) & 1;
	cc_bool prepass = (key & KEY_DEPTH_PREPASS) != 0, afterPrepass = (key & KEY_AFTER_PREPASS) != 0;

	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = rt ? vsRtModules[fmt] : vsModules[fmt];
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	/* Ray traced variants only exist for textured vertices (i.e. chunk meshes) */
	/*  (or for the sky, which can be coloured or textured) */
	if (sky) {
		stages[1].module = fsSkyModules[fmt + ((key >> 2) & 3) * 2];
	} else if (afterPrepass && (key & 0x02)) {
		/* Alpha tested, but with depth testing before the shader runs */
		stages[1].module = fsEarlyModules[(key >> 2) & 3];
	} else if (prepass) {
		/* Only depth is written, so just alpha testing is needed (but with the same vertex shader as */
		/*  the ray traced pass, so that both passes produce exactly the same depth) */
		stages[1].module = fsModules[key & 0x03];
	} else {
		stages[1].module = rt ? fsRtModules[((key >> 1) & 1) + ((key >> 2) & 3) * 2] : fsModules[key & 0x0F];
	}
	stages[1].pName  = "main";

	binding.stride    = strideSizes[fmt];
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	attribs[0].location = 0; attribs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attribs[0].offset = 0;
	attribs[1].location = 1; attribs[1].format = VK_FORMAT_R8G8B8A8_UNORM;   attribs[1].offset = 12;
	attribs[2].location = 2; attribs[2].format = VK_FORMAT_R32G32_SFLOAT;    attribs[2].offset = 16;

	vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount   = 1;
	vertexInput.pVertexBindingDescriptions      = &binding;
	vertexInput.vertexAttributeDescriptionCount = fmt ? 3 : 2;
	vertexInput.pVertexAttributeDescriptions    = attribs;

	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = (key & 0x1000) ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount  = 1;

	raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode    = (key & 0x0800) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth   = 1.0f;

	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth.depthTestEnable  = (key >> 9)  & 1;
	depth.depthWriteEnable = (key >> 10) & 1;
	/* Reversed depth buffer, same as Direct3D11 backend */
	depth.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;
	/* After the depth pre-pass, only the closest faces are drawn */
	if (afterPrepass) {
		depth.depthWriteEnable = 0;
		depth.depthCompareOp   = VK_COMPARE_OP_EQUAL;
	}

	blendAttachments[0].blendEnable         = (key >> 4) & 1;
	blendAttachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachments[0].colorBlendOp        = VK_BLEND_OP_ADD;
	blendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachments[0].alphaBlendOp        = VK_BLEND_OP_ADD;
	blendAttachments[0].colorWriteMask      = (key >> 5) & 0x0F;

	/* Only opaque chunk faces write to the history images (see PrepareDraw) */
	if (rt && !prepass && !blendAttachments[0].blendEnable && blendAttachments[0].colorWriteMask) {
		blendAttachments[1].colorWriteMask = 0x0F;
		blendAttachments[2].colorWriteMask = 0x0F;
		blendAttachments[3].colorWriteMask = 0x0F;
	}
	if (prepass) blendAttachments[0].colorWriteMask = 0;

	blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = rtSupported ? 4 : 1;
	blend.pAttachments    = blendAttachments;

	dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = dynamicCull ? 3 : 2;
	dynamic.pDynamicStates    = dynamics;

	info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount          = 2;
	info.pStages             = stages;
	info.pVertexInputState   = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState      = &viewportState;
	info.pRasterizationState = &raster;
	info.pMultisampleState   = &multisample;
	info.pDepthStencilState  = &depth;
	info.pColorBlendState    = &blend;
	info.pDynamicState       = &dynamic;
	info.layout              = pipelineLayout;
	info.renderPass          = renderPass;
	info.basePipelineIndex   = -1;

	res = vkCreateGraphicsPipelines(device, 0, 1, &info, NULL, &pipeline);
	if (res) Process_Abort2(res, "Failed to create Vulkan pipeline");
	return pipeline;
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_Create(void) {
	LoadVulkanLibrary();
	CreateInstance();
	CreateSurface();
	PickPhysicalDevice();
	CreateDevice();
	ChooseFormats();

	renderPass    = CreateRenderPass(false, true);
	resumePass    = CreateRenderPass(true,  true);
	noHistoryPass = CreateRenderPass(false, false);
	CreateFrames();
	CreatePipelineObjects();

	bufferPool.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	imagePool.usage  = 0;
	if (rtSupported) RT_Init();
	CreateSwapchain();

	Gfx.MaxTexWidth  = props.limits.maxImageDimension2D;
	Gfx.MaxTexHeight = props.limits.maxImageDimension2D;
	Gfx.Created         = true;
	Gfx.BackendType     = CC_GFX_BACKEND_VULKAN;
	customMipmapsLevels = true;
}

void Gfx_Free(void) {
	int i;
	Gfx_FreeState();
	if (!device) return;
	vkDeviceWaitIdle(device);

	for (i = 0; i < FRAMES_IN_FLIGHT; i++) DestroyFrame(&frames[i]);
	frame = NULL;
	RT_FreeAll();

	DestroySwapchainResources();
	if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
	swapchain = 0;

	DestroyPipelineObjects();
	vkDestroyRenderPass(device, renderPass, NULL);
	vkDestroyRenderPass(device, resumePass, NULL);
	vkDestroyRenderPass(device, noHistoryPass, NULL);
	MemPool_Destroy(&bufferPool);
	MemPool_Destroy(&imagePool);
	MemPool_Destroy(&asPool);
	MemPool_Destroy(&scratchPool);

	vkDestroyDevice(device, NULL);
	vkDestroySurfaceKHR(instance, surface, NULL);
	vkDestroyInstance(instance, NULL);
	device   = NULL;
	surface  = 0;
	instance = NULL;

	frameOpen        = false;
	imageAcquired    = false;
	acquirePending   = false;
	uploadBegun      = false;
	drawBegun        = false;
	swapchainDirty   = false;
	renderPassActive = false;
	vbBuffer = 0;
	ibBuffer = 0;
	curTex   = NULL;
}

cc_bool Gfx_TryRestoreContext(void) {
	Gfx_Free();
	Gfx_Create();
	return true;
}

static void Gfx_FreeState(void) {
	if (!inited) return;
	inited = false;

	FreeDefaultResources();
	Gfx_DeleteTexture(&white_square);
}

static void Gfx_RestoreState(void) {
	struct Bitmap bmp;
	BitmapCol pixels[1] = { BITMAPCOLOR_WHITE };
	if (inited) return;
	inited = true;

	InitDefaultResources();
	gfx_format = -1;

	/* 1x1 dummy white texture */
	Bitmap_Init(bmp, 1, 1, pixels);
	Gfx_RecreateTexture(&white_square, &bmp, 0, false);
	curTex = NULL;

	Gfx_SetViewport(0, 0, Game.Width, Game.Height);
	Gfx_SetScissor( 0, 0, Game.Width, Game.Height);
}


/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
static void CreateDescriptorPool(void) {
	VkDescriptorPoolCreateInfo info = { 0 };
	VkDescriptorPoolSize size;
	VkDescriptorPool pool;
	VkResult res;

	size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = DESC_POOL_SETS;

	info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	info.maxSets       = DESC_POOL_SETS;
	info.poolSizeCount = 1;
	info.pPoolSizes    = &size;

	res = vkCreateDescriptorPool(device, &info, NULL, &pool);
	if (res) Process_Abort2(res, "Failed to create Vulkan descriptor pool");

	descPools = descPools ?
		(VkDescriptorPool*)Mem_Realloc(descPools, numDescPools + 1, sizeof(VkDescriptorPool), "Vulkan descriptor pools") :
		(VkDescriptorPool*)Mem_Alloc(1, sizeof(VkDescriptorPool), "Vulkan descriptor pools");
	descPools[numDescPools++] = pool;
}

/* Each texture has one descriptor set for normal sampling, and one for mipmapped sampling */
static void AllocTextureSets(struct VulkanTexture* tex, int filter) {
	VkDescriptorSetAllocateInfo info = { 0 };
	VkWriteDescriptorSet writes[2]   = { { 0 } };
	VkDescriptorImageInfo images[2];
	VkDescriptorSetLayout layouts[2];
	VkResult res;
	int i;

	layouts[0] = setLayout;
	layouts[1] = setLayout;
	info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorSetCount = 2;
	info.pSetLayouts        = layouts;

	for (i = numDescPools - 1; i >= 0; i--)
	{
		info.descriptorPool = descPools[i];
		if (!vkAllocateDescriptorSets(device, &info, tex->sets)) break;
	}

	/* All existing pools are full */
	if (i < 0) {
		CreateDescriptorPool();
		i = numDescPools - 1;
		info.descriptorPool = descPools[i];

		res = vkAllocateDescriptorSets(device, &info, tex->sets);
		if (res) Process_Abort2(res, "Failed to allocate Vulkan descriptor sets");
	}
	tex->pool = descPools[i];

	for (i = 0; i < 2; i++)
	{
		images[i].sampler     = samplers[filter + i];
		images[i].imageView   = tex->view;
		images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet          = tex->sets[i];
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo      = &images[i];
	}
	vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
}

static void CopyTextureLevel(VkCommandBuffer cmd, struct VulkanTexture* tex, int level,
							int x, int y, int width, int height, BitmapCol* pixels, int rowWidth) {
	VkBufferImageCopy region = { 0 };
	VkDeviceSize offset;
	VkBuffer buffer;
	void* data = Ring_Alloc((VkDeviceSize)width * height * BITMAPCOLOR_SIZE, &buffer, &offset);
	if (!data) return;

	CopyPixels(data, width * BITMAPCOLOR_SIZE, pixels, rowWidth * BITMAPCOLOR_SIZE, width, height);
	region.bufferOffset = offset;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel   = level;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x      = x;
	region.imageOffset.y      = y;
	region.imageExtent.width  = width;
	region.imageExtent.height = height;
	region.imageExtent.depth  = 1;

	vkCmdCopyBufferToImage(cmd, buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static void UploadTexture(struct VulkanTexture* tex, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps, cc_bool created) {
	VkCommandBuffer cmd = UploadCmd();
	BitmapCol* prev = part->scan0;
	BitmapCol* cur;
	int lvl, lvls = 0;
	int width = part->width, height = part->height;

	ImageBarrier(cmd, tex->image, tex->levels,
		created ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		created ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	CopyTextureLevel(cmd, tex, 0, x, y, width, height, part->scan0, rowWidth);

	if (mipmaps) lvls = min(CalcMipmapsLevels(width, height), (int)tex->levels - 1);
	for (lvl = 1; lvl <= lvls; lvl++)
	{
		x /= 2; y /= 2;
		if (width  > 1) width  /= 2;
		if (height > 1) height /= 2;

		cur = (BitmapCol*)Mem_Alloc(width * height, BITMAPCOLOR_SIZE, "mipmaps");
		GenMipmaps(width, height, cur, prev, rowWidth);
		CopyTextureLevel(cmd, tex, lvl, x, y, width, height, cur, width);

		if (prev != part->scan0) Mem_Free(prev);
		prev     = cur;
		rowWidth = width;
	}
	if (prev != part->scan0) Mem_Free(prev);

	ImageBarrier(cmd, tex->image, tex->levels,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

GfxResourceID Gfx_AllocTexture(struct Bitmap* bmp, int rowWidth, cc_uint8 flags, cc_bool mipmaps) {
	VkImageCreateInfo info         = { 0 };
	VkImageViewCreateInfo viewInfo = { 0 };
	VkMemoryRequirements reqs;
	struct VulkanTexture* tex;
	VkResult res;

	tex = (struct VulkanTexture*)Mem_TryAllocCleared(1, sizeof(struct VulkanTexture));
	if (!tex) return NULL;
	tex->levels = mipmaps ? 1 + CalcMipmapsLevels(bmp->width, bmp->height) : 1;

	info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType     = VK_IMAGE_TYPE_2D;
	info.format        = VK_FORMAT_B8G8R8A8_UNORM;
	info.extent.width  = bmp->width;
	info.extent.height = bmp->height;
	info.extent.depth  = 1;
	info.mipLevels     = tex->levels;
	info.arrayLayers   = 1;
	info.samples       = VK_SAMPLE_COUNT_1_BIT;
	info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (vkCreateImage(device, &info, NULL, &tex->image)) { Mem_Free(tex); return NULL; }
	vkGetImageMemoryRequirements(device, tex->image, &reqs);

	while (!AllocMemory(&imagePool, &reqs, &tex->alloc))
	{
		if (Game_ReduceVRAM()) continue;
		vkDestroyImage(device, tex->image, NULL);
		Mem_Free(tex);
		return NULL;
	}
	vkBindImageMemory(device, tex->image, tex->alloc.block->memory, tex->alloc.offset);

	viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image    = tex->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format   = VK_FORMAT_B8G8R8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = tex->levels;
	viewInfo.subresourceRange.layerCount = 1;

	res = vkCreateImageView(device, &viewInfo, NULL, &tex->view);
	if (res) Process_Abort2(res, "Failed to create Vulkan texture view");

	AllocTextureSets(tex, (flags & TEXTURE_FLAG_BILINEAR) ? 2 : 0);
	UploadTexture(tex, 0, 0, bmp, rowWidth, mipmaps, true);
	return tex;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
	struct VulkanTexture* tex = (struct VulkanTexture*)texId;
	if (tex) UploadTexture(tex, x, y, part, rowWidth, mipmaps, false);
}

void Gfx_DeleteTexture(GfxResourceID* texId) {
	struct VulkanTexture* tex = (struct VulkanTexture*)(*texId);

	if (tex) {
		if (tex == curTex) { curTex = NULL; dirtyTexture = true; }
		Garbage_Add(GARBAGE_TEXTURE, tex);
	}
	*texId = NULL;
}

/* White texture is used when no texture is bound */
void Gfx_BindTexture(GfxResourceID texId) {
	struct VulkanTexture* tex = (struct VulkanTexture*)(texId ? texId : white_square);
	if (tex == curTex) return;

	curTex       = tex;
	dirtyTexture = true;
}

void Gfx_EnableMipmaps(void) {
	if (!Gfx.Mipmaps) return;
	mipmapsOn    = true;
	dirtyTexture = true;
}

void Gfx_DisableMipmaps(void) {
	if (!Gfx.Mipmaps) return;
	mipmapsOn    = false;
	dirtyTexture = true;
}


/*########################################################################################################################*
*------------------------------------------------------Index buffers------------------------------------------------------*
*#########################################################################################################################*/
/* Static vertex and index buffers are sub-allocated from big device local VkBuffers */
static struct MemAlloc* AllocBuffer(VkDeviceSize size) {
	struct MemAlloc* alloc = (struct MemAlloc*)Mem_TryAllocCleared(1, sizeof(struct MemAlloc));
	VkMemoryRequirements reqs;
	if (!alloc) return NULL;

	reqs.size           = max(size, 16);
	reqs.alignment      = 16;
	reqs.memoryTypeBits = ~0u;
	if (AllocMemory(&bufferPool, &reqs, alloc)) return alloc;

	Mem_Free(alloc);
	return NULL;
}

static void DeleteBuffer(GfxResourceID* id) {
	struct MemAlloc* alloc = (struct MemAlloc*)(*id);

	if (alloc) {
		if (alloc->rt) RT_Unlink(alloc->rt);
		Garbage_Add(GARBAGE_BUFFER, alloc);
	}
	*id = NULL;
}

static void CopyToBuffer(VkBuffer src, VkDeviceSize srcOffset, struct MemAlloc* dst, VkDeviceSize size) {
	VkBufferCopy region;
	if (!size) return;

	region.srcOffset = srcOffset;
	region.dstOffset = dst->offset;
	region.size      = size;
	vkCmdCopyBuffer(UploadCmd(), src, dst->block->buffer, 1, &region);
}

GfxResourceID Gfx_CreateIb2(int count, Gfx_FillIBFunc fillFunc, void* obj) {
	VkDeviceSize size = (VkDeviceSize)count * sizeof(cc_uint16);
	struct MemAlloc* ib = AllocBuffer(size);
	VkDeviceSize offset;
	VkBuffer src;
	void* data;

	if (!ib) Process_Abort("Failed to create index buffer");
	data = Ring_Alloc(size, &src, &offset);
	if (!data) Process_Abort("Failed to create index buffer");

	fillFunc((cc_uint16*)data, count, obj);
	CopyToBuffer(src, offset, ib, size);
	return ib;
}

void Gfx_BindIb(GfxResourceID ib) {
	struct MemAlloc* alloc = (struct MemAlloc*)ib;
	ibBuffer = alloc ? alloc->block->buffer : 0;
	ibOffset = alloc ? alloc->offset : 0;
	dirtyIb  = true;
}

void Gfx_DeleteIb(GfxResourceID* ib) { DeleteBuffer(ib); }


/*########################################################################################################################*
*------------------------------------------------------Vertex buffers-----------------------------------------------------*
*#########################################################################################################################*/
static GfxResourceID Gfx_AllocStaticVb(VertexFormat fmt, int count) {
	return AllocBuffer((VkDeviceSize)count * strideSizes[fmt]);
}

void Gfx_BindVb(GfxResourceID vb) {
	struct MemAlloc* alloc = (struct MemAlloc*)vb;
	vbBuffer = alloc ? alloc->block->buffer : 0;
	vbOffset = alloc ? alloc->offset : 0;
	dirtyVb  = true;
}

void Gfx_DeleteVb(GfxResourceID* vb) { DeleteBuffer(vb); }

static VkBuffer lockBuffer;
static VkDeviceSize lockOffset, lockSize;

/* Vertices are written to staging memory, then copied into the VB when unlocked */
void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	void* data;
	lockSize = (VkDeviceSize)count * strideSizes[fmt];
	data     = Ring_Alloc(lockSize, &lockBuffer, &lockOffset);

	if (!data) lockSize = 0;
	return data;
}

void Gfx_UnlockVb(GfxResourceID vb) {
	struct MemAlloc* alloc = (struct MemAlloc*)vb;
	CopyToBuffer(lockBuffer, lockOffset, alloc, min(lockSize, alloc->size));
}


/*########################################################################################################################*
*--------------------------------------------------Dynamic vertex buffers-------------------------------------------------*
*#########################################################################################################################*/
static GfxResourceID Gfx_AllocDynamicVb(VertexFormat fmt, int maxVertices) {
	struct DynamicVb* vb = (struct DynamicVb*)Mem_TryAllocCleared(1, sizeof(struct DynamicVb));
	if (!vb) return NULL;

	vb->size    = maxVertices * strideSizes[fmt];
	vb->frameId = frameCounter - 1;
	vb->data    = (cc_uint8*)Mem_TryAlloc(max(vb->size, 16), 1);
	if (vb->data) return vb;

	Mem_Free(vb);
	return NULL;
}

void Gfx_DeleteDynamicVb(GfxResourceID* vb) {
	struct DynamicVb* dyn = (struct DynamicVb*)(*vb);
	if (dyn) { Mem_Free(dyn->data); Mem_Free(dyn); }
	*vb = NULL;
}

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	struct DynamicVb* dyn = (struct DynamicVb*)vb;
	dyn->used = min((cc_uint32)(count * strideSizes[fmt]), dyn->size);
	return dyn->data;
}

static void UploadDynamicVb(struct DynamicVb* dyn) {
	void* dst = Ring_Alloc(max(dyn->used, 16), &dyn->buffer, &dyn->offset);
	if (!dst) { dyn->buffer = 0; return; }

	Mem_Copy(dst, dyn->data, dyn->used);
	dyn->frameId = frameCounter;
}

void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	UploadDynamicVb((struct DynamicVb*)vb);
	Gfx_BindDynamicVb(vb);
}

void Gfx_BindDynamicVb(GfxResourceID vb) {
	struct DynamicVb* dyn = (struct DynamicVb*)vb;
	if (!dyn) return;
	/* Data from a previous frame may have been overwritten in the ring buffer already */
	if (dyn->frameId != frameCounter) UploadDynamicVb(dyn);

	vbBuffer = dyn->buffer;
	vbOffset = dyn->offset;
	dirtyVb  = true;
}


/*########################################################################################################################*
*------------------------------------------------------Vertex drawing-----------------------------------------------------*
*#########################################################################################################################*/
/*########################################################################################################################*
*------------------------------------------------------Depth pre-pass-----------------------------------------------------*
*#########################################################################################################################*/
/* Ray traced chunk faces are expensive to draw, so they are first only drawn to the depth buffer, and then drawn */
/*  again with the ray traced shader afterwards, but only where they're the closest face (i.e. depth is equal). */
/*  Otherwise e.g. every layer of leaves in a tree would trace rays, even though only the closest one is seen */
/* The ray traced draws are recorded and then drawn just before the next draw that isn't a ray traced chunk face */
struct DeferredDraw {
	VkBuffer vb, ib;
	VkDeviceSize vbOffset, ibOffset;
	VkDescriptorSet texSet;
	int key, indices, startVertex;
	cc_bool cull;
	struct PushConstants pc;
};
static struct DeferredDraw* deferredDraws;
static int numDeferred, maxDeferred;
static struct DeferredDraw curDeferred;
static cc_bool deferringDraw;

static void AddDeferredDraw(int indices, int startVertex) {
	if (numDeferred == maxDeferred) {
		maxDeferred   = maxDeferred ? maxDeferred * 2 : 1024;
		deferredDraws = deferredDraws ?
			(struct DeferredDraw*)Mem_Realloc(deferredDraws, maxDeferred, sizeof(struct DeferredDraw), "Vulkan deferred draws") :
			(struct DeferredDraw*)Mem_Alloc(maxDeferred, sizeof(struct DeferredDraw), "Vulkan deferred draws");
	}
	curDeferred.indices     = indices;
	curDeferred.startVertex = startVertex;
	deferredDraws[numDeferred++] = curDeferred;
}

static void FlushDeferredDraws(void) {
	VkCommandBuffer cmd = frame->drawCmd;
	struct DeferredDraw* d;
	struct DeferredDraw* prev = NULL;
	int i;
	if (!numDeferred) return;

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &frame->rtSet, 0, NULL);
	for (i = 0; i < numDeferred; i++, prev = d)
	{
		d = &deferredDraws[i];
		if (!prev || d->key != prev->key) {
			if (!pipelines[d->key]) pipelines[d->key] = CreatePipeline(d->key);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[d->key]);
		}
		if (!prev || d->texSet != prev->texSet) {
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &d->texSet, 0, NULL);
		}
		if (!prev || d->vb != prev->vb || d->vbOffset != prev->vbOffset) {
			vkCmdBindVertexBuffers(cmd, 0, 1, &d->vb, &d->vbOffset);
		}
		if (!prev || d->ib != prev->ib || d->ibOffset != prev->ibOffset) {
			vkCmdBindIndexBuffer(cmd, d->ib, d->ibOffset, VK_INDEX_TYPE_UINT16);
		}
		if (!prev || !Mem_Equal(&d->pc, &prev->pc, sizeof(d->pc))) {
			vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
								0, sizeof(d->pc), &d->pc);
		}
		if (dynamicCull && (!prev || d->cull != prev->cull)) {
			vkCmdSetCullModeEXT(cmd, d->cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
		}
		vkCmdDrawIndexed(cmd, d->indices, 1, 0, d->startVertex, 0);
	}
	numDeferred = 0;

	/* The state above has replaced whatever was bound before */
	boundPipeline  = -1;
	dirtyConstants = true;
	dirtyTexture   = true;
	dirtyVb        = true;
	dirtyIb        = true;
	dirtyCull      = true;
}

/* Bound state is lost when starting a new command buffer */
static void InvalidateState(void) {
	numDeferred    = 0;
	boundPipeline  = -1;
	dirtyViewport  = true;
	dirtyScissor   = true;
	dirtyConstants = true;
	dirtyTexture   = true;
	dirtyVb        = true;
	dirtyIb        = true;
	dirtyCull      = true;
	dirtyRtSet     = true;
}

static void BeginRenderPass(VkRenderPass pass) {
	VkRenderPassBeginInfo info = { 0 };
	VkClearValue clears[5]     = { { { { 0 } } } };
	BeginCmd(frame->drawCmd);
	drawBegun = true;
	InvalidateState();

	/* Shadow history is cleared to no history (i.e. a depth that never matches) */
	clears[2].color.float32[0] =  1.0f;
	clears[2].color.float32[1] = -1.0f;

	info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass        = pass;
	info.framebuffer       = framebuffers[imageIndex][rtSupported ? frameIndex : 0];
	info.renderArea.extent = swapExtent;
	info.clearValueCount   = rtSupported ? 5 : 0;
	info.pClearValues      = clears;

	/* Swapchain recreation happens before this (see AcquireImage), so the images can't be destroyed */
	/*  between recording this and the command buffer being submitted */
	if (historyFresh) {
		int i, j;
		for (i = 0; i < HISTORY_IMAGES; i++)
		{
			for (j = 0; j < FRAMES_IN_FLIGHT; j++)
			{
				ImageBarrier(frame->drawCmd, historyImages[i][j], 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
			}
		}
		historyFresh = false;
	}

	vkCmdBeginRenderPass(frame->drawCmd, &info, VK_SUBPASS_CONTENTS_INLINE);
	renderPassActive = true;
}

/* Swapchain image is only acquired once something actually needs to be drawn */
static cc_bool AcquireImage(void) {
	VkResult res;
	if (swapchainDirty) RecreateSwapchain();
	if (!swapchain) return false;

	res = vkAcquireNextImageKHR(device, swapchain, WAIT_FOREVER, frame->acquireSem, 0, &imageIndex);
	if (res == VK_ERROR_OUT_OF_DATE_KHR) {
		RecreateSwapchain();
		if (!swapchain) return false;
		res = vkAcquireNextImageKHR(device, swapchain, WAIT_FOREVER, frame->acquireSem, 0, &imageIndex);
	}

	if (res == VK_SUBOPTIMAL_KHR) { swapchainDirty = true; res = VK_SUCCESS; }
	if (res) { HandleError(res, "Failed to acquire Vulkan swapchain image"); return false; }

	imageAcquired  = true;
	acquirePending = true;
	return true;
}

static cc_bool EnsureRenderPass(void) {
	if (renderPassActive) return true;
	if (Gfx.LostContext)  return false;
	if (!imageAcquired && !AcquireImage()) return false;

	BeginRenderPass(rtSupported && !rtActive ? noHistoryPass : renderPass);
	return true;
}

/* Bit 0 = format, 1 = alpha test, 2-3 = fog, 4 = blending, 5-8 = colour mask, */
/*  9 = depth test, 10 = depth write, 11 = culling (only without dynamic cull mode), */
/*  12 = lines, 13 = ray traced shadows, 14 = ray traced sky, */
/*  15 = depth pre-pass of ray traced chunk faces, 16 = ray traced chunk faces after depth pre-pass */
static int CalcPipelineKey(cc_bool lines, cc_bool rt, cc_bool sky) {
	int fog = 0, cull = dynamicCull ? 0 : cullFaces;
	if (gfx_fogEnabled && gfx_fogMode >= 0) fog = gfx_fogMode == FOG_LINEAR ? 1 : 2;

	return gfx_format | (gfx_alphaTest << 1) | (fog << 2) | (gfx_alphaBlend << 4) | (colorMask << 5) |
		(gfx_depthTest << 9) | (gfx_depthWrite << 10) | (cull << 11) | (lines << 12) | (rt << 13) | (sky << 14);
}

static cc_bool PrepareDraw(cc_bool lines, cc_bool terrain) {
	struct VulkanTexture* tex = curTex ? curTex : (struct VulkanTexture*)white_square;
	cc_bool textured = gfx_format == VERTEX_FORMAT_TEXTURED;
	cc_bool rt, sky;
	float temporal;
	VkCommandBuffer cmd;
	int key;

	if (gfx_format < 0 || !vbBuffer)  return false;
	if (!lines && !ibBuffer)         return false;
	if (textured && !tex)            return false;
	if (!EnsureRenderPass())         return false;
	cmd = frame->drawCmd;

	/* No point tracing rays when nothing is written to the colour buffer (e.g. depth only pass) */
	/*  The sky only has god rays, so it's only ray traced when they're on */
	sky = rtSkyDraw && rtActive && frame->tlas && colorMask && rtGodRays && !lines;
	rt  = (terrain && textured && rtActive && frame->tlas && colorMask) || sky;
	key = CalcPipelineKey(lines, rt, sky);

	/* Opaque ray traced chunk faces are drawn to the depth buffer now, and with the ray traced shader later */
	deferringDraw = rt && !sky && !gfx_alphaBlend && gfx_depthTest && gfx_depthWrite;
	if (!deferringDraw) FlushDeferredDraws();
	if (deferringDraw) {
		/* Still alpha tested, since two faces can be in exactly the same place (e.g. between two leaf blocks), */
		/*  and the closest depth there might have come from the other face (the alpha tested shaders used */
		/*  after the pre-pass do depth testing before running, so hidden faces still never run them) */
		curDeferred.key = (key & ~(1 << 10)) | KEY_AFTER_PREPASS;
		key |= KEY_DEPTH_PREPASS;
	}

	/* Translucent faces (e.g. water) don't use the shadow history, since otherwise they would */
	/*  replace the history of the faces behind them every frame */
	temporal = (rt && !gfx_alphaBlend) ? 1.0f : 0.0f;
	if (temporal != pc.rtTemporal) { pc.rtTemporal = temporal; dirtyConstants = true; }
	if (rt) rtDrawn = true;
	/* The sky is drawn with a different view matrix (e.g. skybox is centred on the camera), so the */
	/*  camera's actual view is used instead (in case nothing but the sky is drawn this frame) */
	if (rt && !sky) rtFrameMVP = pc.mvp;
	if (rt &&  sky) Matrix_Mul(&rtFrameMVP, &Gfx.View, &_proj);
	if (key != boundPipeline) {
		if (!pipelines[key]) pipelines[key] = CreatePipeline(key);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[key]);
		boundPipeline = key;
	}

	if (rt && dirtyRtSet) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
								1, 1, &frame->rtSet, 0, NULL);
		dirtyRtSet = false;
	}

	if (dirtyCull && dynamicCull) {
		vkCmdSetCullModeEXT(cmd, cullFaces ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
		dirtyCull = false;
	}
	if (dirtyViewport) {
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		dirtyViewport = false;
	}
	if (dirtyScissor) {
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		dirtyScissor = false;
	}
	if (dirtyConstants) {
		vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
							0, sizeof(pc), &pc);
		dirtyConstants = false;
	}
	if (dirtyTexture && textured) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
								0, 1, &tex->sets[mipmapsOn], 0, NULL);
		dirtyTexture = false;
	} else if (rt && !textured) {
		/* Ray traced shaders always use the texture (for alpha testing), even for the coloured sky */
		/*  (which uses the terrain atlas, so that e.g. leaves are alpha tested by god rays from the sky) */
		tex = (struct VulkanTexture*)(Atlas1D.TexIds[0] ? Atlas1D.TexIds[0] : white_square);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
								0, 1, &tex->sets[mipmapsOn], 0, NULL);
		dirtyTexture = true;
	}
	if (dirtyVb) {
		vkCmdBindVertexBuffers(cmd, 0, 1, &vbBuffer, &vbOffset);
		dirtyVb = false;
	}
	if (dirtyIb && !lines) {
		vkCmdBindIndexBuffer(cmd, ibBuffer, ibOffset, VK_INDEX_TYPE_UINT16);
		dirtyIb = false;
	}

	if (deferringDraw) {
		curDeferred.vb       = vbBuffer;
		curDeferred.vbOffset = vbOffset;
		curDeferred.ib       = ibBuffer;
		curDeferred.ibOffset = ibOffset;
		curDeferred.texSet   = tex->sets[mipmapsOn];
		curDeferred.cull     = cullFaces;
		curDeferred.pc       = pc;
	}
	return true;
}

void Gfx_SetVertexFormat(VertexFormat fmt) {
	if (fmt == gfx_format) return;
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];
}

void Gfx_DrawVb_Lines(int verticesCount) {
	if (!PrepareDraw(true, false)) return;
	vkCmdDraw(frame->drawCmd, verticesCount, 1, 0, 0);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	if (!PrepareDraw(false, false)) return;
	vkCmdDrawIndexed(frame->drawCmd, ICOUNT(verticesCount), 1, 0, 0, 0);
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex, DrawHints hints) {
	if (!PrepareDraw(false, false)) return;
	vkCmdDrawIndexed(frame->drawCmd, ICOUNT(verticesCount), 1, 0, startVertex, 0);
}

/* Only used by MapRenderer, so this is how chunks are drawn with ray traced shadows */
void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex, DrawHints hints) {
	if (!PrepareDraw(false, true)) return;
	vkCmdDrawIndexed(frame->drawCmd, ICOUNT(verticesCount), 1, 0, startVertex, 0);
	if (deferringDraw) AddDeferredDraw(ICOUNT(verticesCount), startVertex);
}


/*########################################################################################################################*
*-----------------------------------------------------Vertex transform----------------------------------------------------*
*#########################################################################################################################*/
/* Source https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxmatrixorthooffcenterrh */
/*   The simplified calculation below uses: L = 0, R = width, T = 0, B = height */
/* NOTE: This calculation is shared with Direct3D 9/11 backends */
void Gfx_CalcOrthoMatrix(struct Matrix* matrix, float width, float height, float zNear, float zFar) {
	*matrix = Matrix_Identity;

	matrix->row1.x =  2.0f / width;
	matrix->row2.y = -2.0f / height;
	matrix->row3.z =  1.0f / (zNear - zFar);

	matrix->row4.x = -1.0f;
	matrix->row4.y =  1.0f;
	matrix->row4.z = zNear / (zNear - zFar);
}

static float Cotangent(float x) { return Math_CosF(x) / Math_SinF(x); }
void Gfx_CalcPerspectiveMatrix(struct Matrix* matrix, float fov, float aspect, float zFar) {
	/* Deliberately swap zNear/zFar in projection matrix calculation to produce */
	/*  a projection matrix that results in a reversed depth buffer */
	/* https://developer.nvidia.com/content/depth-precision-visualized */
	float zNear_ = zFar;
	float zFar_  = Reversed_CalcZNear(fov, 24);
	float c = Cotangent(0.5f * fov);
	*matrix = Matrix_Identity;

	matrix->row1.x =  c / aspect;
	matrix->row2.y =  c;
	matrix->row3.z = zFar_ / (zNear_ - zFar_);
	matrix->row3.w = -1.0f;
	matrix->row4.z = (zNear_ * zFar_) / (zNear_ - zFar_);
	matrix->row4.w =  0.0f;
}

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (type == MATRIX_VIEW) _view = *matrix;
	if (type == MATRIX_PROJ) _proj = *matrix;

	Matrix_Mul(&pc.mvp, &_view, &_proj);
	dirtyConstants = true;
}

void Gfx_LoadMVP(const struct Matrix* view, const struct Matrix* proj, struct Matrix* mvp) {
	Gfx_LoadMatrix(MATRIX_VIEW, view);
	Gfx_LoadMatrix(MATRIX_PROJ, proj);
	Matrix_Mul(mvp, view, proj);
}

void Gfx_EnableTextureOffset(float x, float y) {
	pc.texX = x;
	pc.texY = y;
	dirtyConstants = true;
}

void Gfx_DisableTextureOffset(void) {
	pc.texX = 0;
	pc.texY = 0;
	dirtyConstants = true;
}

/* Negative height flips the viewport vertically, so the same projection matrices as */
/*  Direct3D can be used (since Vulkan clip space Y points down instead of up) */
void Gfx_SetViewport(int x, int y, int w, int h) {
	FlushDeferredDraws();
	viewport.x        = (float)x;
	viewport.y        = (float)(y + h);
	viewport.width    = (float)w;
	viewport.height   = -(float)h;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	dirtyViewport     = true;
}

void Gfx_SetScissor(int x, int y, int w, int h) {
	FlushDeferredDraws();
	scissor.offset.x      = max(x, 0);
	scissor.offset.y      = max(y, 0);
	scissor.extent.width  = max(w, 0);
	scissor.extent.height = max(h, 0);
	dirtyScissor          = true;
}


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
/* State is applied when drawing, by selecting the matching pipeline */
static void SetAlphaTest(cc_bool enabled)  { }
static void SetAlphaBlend(cc_bool enabled) { }
static void SetDepthTest(cc_bool enabled)  { }
static void SetDepthWrite(cc_bool enabled) { }
void Gfx_SetAlphaArgBlend(cc_bool enabled) { }

static void SetColorWrite(cc_bool r, cc_bool g, cc_bool b, cc_bool a) {
	colorMask = r | (g << 1) | (b << 2) | (a << 3);
}

void Gfx_DepthOnlyRendering(cc_bool depthOnly) {
	DefaultDepthOnlyRendering(depthOnly);
}

void Gfx_SetFaceCulling(cc_bool enabled) {
	if (cullFaces == enabled) return;
	cullFaces = enabled;
	dirtyCull = true;
}


/*########################################################################################################################*
*-----------------------------------------------------------Fog-----------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFog(cc_bool enabled) { gfx_fogEnabled = enabled; }

static void SetFogColor(PackedCol color) {
	pc.fogR = PackedCol_R(color) / 255.0f;
	pc.fogG = PackedCol_G(color) / 255.0f;
	pc.fogB = PackedCol_B(color) / 255.0f;
	dirtyConstants = true;
}

static void SetFogDensity(float value) {
	/* avoid doing - in fragment shader for density fog */
	pc.fogDensity  = -value;
	dirtyConstants = true;
}

static void SetFogEnd(float value) {
	/* avoid doing division in fragment shader for linear fog */
	pc.fogEnd      = 1.0f / value;
	dirtyConstants = true;
}

static void SetFogMode(FogFunc func) { }


/*########################################################################################################################*
*-----------------------------------------------------Buffer clearing-----------------------------------------------------*
*#########################################################################################################################*/
void Gfx_ClearColor(PackedCol color) {
	clearColor[0] = PackedCol_R(color) / 255.0f;
	clearColor[1] = PackedCol_G(color) / 255.0f;
	clearColor[2] = PackedCol_B(color) / 255.0f;
	clearColor[3] = PackedCol_A(color) / 255.0f;
}

void Gfx_ClearBuffers(GfxBuffers buffers) {
	VkClearAttachment attachments[2];
	VkClearRect rect = { { { 0 } } };
	cc_uint32 count = 0;

	if (!EnsureRenderPass()) return;
	FlushDeferredDraws();

	if (buffers & GFX_BUFFER_COLOR) {
		attachments[count].aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
		attachments[count].colorAttachment = 0;
		Mem_Copy(attachments[count].clearValue.color.float32, clearColor, sizeof(clearColor));
		count++;
	}
	if (buffers & GFX_BUFFER_DEPTH) {
		attachments[count].aspectMask      = VK_IMAGE_ASPECT_DEPTH_BIT;
		attachments[count].colorAttachment = 0;
		/* 0 since using a reversed depth buffer */
		attachments[count].clearValue.depthStencil.depth   = 0.0f;
		attachments[count].clearValue.depthStencil.stencil = 0;
		count++;
	}
	if (!count) return;

	rect.rect.extent = swapExtent;
	rect.layerCount  = 1;
	vkCmdClearAttachments(frame->drawCmd, count, attachments, 1, &rect);
}


/*########################################################################################################################*
*-------------------------------------------------------Ray tracing-------------------------------------------------------*
*#########################################################################################################################*/
#define RT_OPT_ENABLED     "gfx-raytracing"
#define RT_OPT_SUN_ANGLE   "gfx-rt-sunangle"
#define RT_OPT_SUN_DIR     "gfx-rt-sundir"
#define RT_OPT_SHADOW_DIST "gfx-rt-shadowdist"
#define RT_OPT_SOFTNESS    "gfx-rt-softness"
#define RT_OPT_AO          "gfx-rt-ao"
#define RT_OPT_GI          "gfx-rt-gi"
#define RT_OPT_GOD_RAYS    "gfx-rt-godrays"
#define RT_OPT_BLOCK_LIGHTS "gfx-rt-blocklights"
/* How much sunlight the air scatters per block, how much it scatters forwards (Henyey-Greenstein g), */
/*  and how far along the view ray it's calculated for */
#define RT_GOD_DENSITY 0.02f
#define RT_GOD_G       0.4f
#define RT_GOD_RANGE   96.0f
/* How far away blocks can be and still block sky light */
#define RT_AO_RANGE 16.0f
/* Each geometry can only use up to 16384 quads, since that's all the default index buffer has */
#define RT_MAX_GEOMETRY_VERTICES 65536
/* Extra space in the top level acceleration structure for chunks built during the frame */
#define RT_TLAS_SLACK 512
#define RT_GeometryCount(vertices) (((vertices) + RT_MAX_GEOMETRY_VERTICES - 1) / RT_MAX_GEOMETRY_VERTICES)

static const char* const rtSunAngle_Names[]   = { "Low", "Medium", "High", "Overhead" };
static const char* const rtSunDir_Names[]     = { "North", "Northeast", "East", "Southeast", "South", "Southwest", "West", "Northwest" };
static const char* const rtShadowDist_Names[] = { "64", "128", "256", "Max" };
static const char* const rtSoftness_Names[]   = { "Off", "Low", "Medium", "High" };
static const char* const rtAO_Names[]         = { "Off", "Low", "Medium", "High" };
static const char* const rtGI_Names[]         = { "Off", "On" };
static const char* const rtGodRays_Names[]    = { "Off", "Low", "Medium", "High" };
static const float rtGodRays_Strength[]       = { 0.0f, 0.06f, 0.12f, 0.2f };
static const char* const rtBlockLights_Names[] = { "Off", "Low", "Medium", "High" };
static const float rtBlockLights_Strength[]    = { 0.0f, 3.0f, 6.0f, 12.0f };
static const float rtSunAngle_Degrees[]  = { 20.0f, 40.0f, 60.0f, 90.0f };
static const float rtShadowDist_Blocks[] = { 64.0f, 128.0f, 256.0f, 4096.0f };
/* Angular radius of the sun's disc (the real sun is about 0.27 degrees), and how many points on the disc */
/*  each pixel cycles through over multiple frames (one ray per frame) */
static const float rtSoftness_Degrees[] = { 0.0f, 1.0f, 2.0f, 4.0f };
static const int   rtSoftness_Samples[] = { 1, 32, 64, 64 };
/* How many frames of shadows and sky light are blended together */
#define RT_MAX_HISTORY 32.0f
/* How much ambient light is reduced in places that can't see the sky (never all of it, so caves aren't pitch black) */
static const float rtAO_Strength[] = { 0.0f, 0.4f, 0.6f, 0.8f };
/* Horizontal direction towards the sun (north is -Z, diagonals are normalised) */
static const float rtSunDir_X[] = {  0.0f,  0.70710678f, 1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f };
static const float rtSunDir_Z[] = { -1.0f, -0.70710678f, 0.0f, 0.70710678f, 1.0f,  0.70710678f,  0.0f, -0.70710678f };

static void RT_Init(void) {
	VkFlags address = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	/* Chunk vertices and the default index buffer are used directly when building acceleration structures */
	bufferPool.usage |= address | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	asPool.usage      = address | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
	scratchPool.usage = address | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

	rtEnabled    = Options_GetBool(RT_OPT_ENABLED, false);
	rtSunAngle   = Options_GetEnum(RT_OPT_SUN_ANGLE,   1, rtSunAngle_Names,   Array_Elems(rtSunAngle_Names));
	rtSunDir     = Options_GetEnum(RT_OPT_SUN_DIR,     2, rtSunDir_Names,     Array_Elems(rtSunDir_Names));
	rtShadowDist = Options_GetEnum(RT_OPT_SHADOW_DIST, 2, rtShadowDist_Names, Array_Elems(rtShadowDist_Names));
	rtSoftness   = Options_GetEnum(RT_OPT_SOFTNESS,    2, rtSoftness_Names,   Array_Elems(rtSoftness_Names));
	rtAO         = Options_GetEnum(RT_OPT_AO,          2, rtAO_Names,         Array_Elems(rtAO_Names));
	rtGI         = Options_GetEnum(RT_OPT_GI,          1, rtGI_Names,         Array_Elems(rtGI_Names));
	rtGodRays    = Options_GetEnum(RT_OPT_GOD_RAYS,    2, rtGodRays_Names,    Array_Elems(rtGodRays_Names));
	rtBlockLights = Options_GetEnum(RT_OPT_BLOCK_LIGHTS, 2, rtBlockLights_Names, Array_Elems(rtBlockLights_Names));
}

static void RT_ListAdd(struct ChunkRT*** list, int* count, int* max, struct ChunkRT* rt) {
	if (*count == *max) {
		*max  = *max ? *max * 2 : 256;
		*list = *list ?
			(struct ChunkRT**)Mem_Realloc(*list, *max, sizeof(struct ChunkRT*), "Vulkan RT chunks") :
			(struct ChunkRT**)Mem_Alloc(*max, sizeof(struct ChunkRT*), "Vulkan RT chunks");
	}
	rt->listIndex = *count;
	(*list)[(*count)++] = rt;
}

static void RT_ListRemove(struct ChunkRT** list, int* count, struct ChunkRT* rt) {
	int i = rt->listIndex;
	list[i] = list[--(*count)];
	list[i]->listIndex = i;
	rt->listIndex = -1;
}

/* Called when a chunk's VB is deleted. The BLAS is destroyed later in Garbage_Collect, */
/*  since the GPU might still be using it in a frame that hasn't finished rendering yet */
static void RT_LightsChanged(void);
static void RT_Unlink(struct ChunkRT* rt) {
	if (rt->listIndex < 0) return;
	if (rt->numLights) RT_LightsChanged();

	if (!rt->numRanges) {
		RT_ListRemove(rtLightOnly, &numRtLightOnly, rt);
	} else if (rt->built) {
		RT_ListRemove(rtChunks,  &numRtChunks,  rt);
	} else {
		RT_ListRemove(rtPending, &numRtPending, rt);
	}
}

static void RT_FreeChunk(struct ChunkRT* rt) {
	if (rt->blas) vkDestroyAccelerationStructureKHR(device, rt->blas, NULL);
	MemAlloc_Free(&asPool, &rt->blasAlloc);
	rt->vb->rt = NULL;

	Mem_Free(rt->ranges);
	Mem_Free(rt->lights);
	Mem_Free(rt);
}

static void RT_FreeAll(void) {
	while (numRtChunks)  { RT_FreeChunk(rtChunks[--numRtChunks]);   }
	while (numRtPending) { RT_FreeChunk(rtPending[--numRtPending]); }
	while (numRtLightOnly) { RT_FreeChunk(rtLightOnly[--numRtLightOnly]); }

	Mem_Free(rtChunks);
	Mem_Free(rtPending);
	Mem_Free(rtLightOnly);
	rtChunks    = NULL; maxRtChunks    = 0;
	rtPending   = NULL; maxRtPending   = 0;
	rtLightOnly = NULL; maxRtLightOnly = 0;
	RT_FreeLights();
}

void Gfx_SetSkyRendering(cc_bool enabled) { rtSkyDraw = enabled; }

void Gfx_SetShadowCasters(GfxResourceID vb, const int* ranges, int numOpaque, int numCutout,
						const int* lights, int numLights) {
	struct MemAlloc* alloc = (struct MemAlloc*)vb;
	int i, numRanges = numOpaque + numCutout;
	struct ChunkRT* rt;
	if (!rtActive || !alloc || (!numRanges && !numLights)) return;

	rt = (struct ChunkRT*)Mem_TryAllocCleared(1, sizeof(struct ChunkRT));
	if (!rt) return;
	rt->vb = alloc;

	if (numLights) {
		rt->lights = (int*)Mem_TryAlloc(numLights * 4, sizeof(int));
		if (rt->lights) {
			Mem_Copy(rt->lights, lights, numLights * 4 * sizeof(int));
			rt->numLights = numLights;
			RT_LightsChanged();
		}
	}

	/* Chunk only has lights (e.g. just a light sprite), so it has no BLAS */
	if (!numRanges) {
		if (!rt->lights) { Mem_Free(rt); return; }
		alloc->rt = rt;
		RT_ListAdd(&rtLightOnly, &numRtLightOnly, &maxRtLightOnly, rt);
		return;
	}

	rt->ranges = (int*)Mem_TryAlloc(numRanges * 2, sizeof(int));
	if (!rt->ranges) { Mem_Free(rt->lights); Mem_Free(rt); return; }

	/* Cutout ranges go first, so their geometries are the first geometries in the BLAS */
	Mem_Copy(rt->ranges,                 ranges + numOpaque * 2, numCutout * 2 * sizeof(int));
	Mem_Copy(rt->ranges + numCutout * 2, ranges,                 numOpaque * 2 * sizeof(int));
	for (i = 0; i < numRanges; i++)
	{
		rt->numGeoms += RT_GeometryCount(rt->ranges[i * 2 + 1]);
	}

	rt->numRanges = numRanges;
	rt->numCutout = numCutout;
	alloc->rt     = rt;
	RT_ListAdd(&rtPending, &numRtPending, &maxRtPending, rt);
}


static cc_bool RT_Alloc(struct MemPool* pool, struct MemAlloc* alloc, VkDeviceSize size, VkDeviceSize align) {
	VkMemoryRequirements reqs;
	reqs.size           = size;
	reqs.alignment      = align;
	reqs.memoryTypeBits = ~0u;
	return AllocMemory(pool, &reqs, alloc);
}

/* Scratch memory is over-allocated, so that its device address can always be aligned */
static cc_bool RT_AllocScratch(struct MemAlloc* alloc, VkDeviceSize size) {
	return RT_Alloc(&scratchPool, alloc, size + rtScratchAlign, rtScratchAlign);
}

static VkDeviceAddress RT_ScratchAddress(struct MemAlloc* alloc) {
	return (MemAlloc_Address(alloc) + rtScratchAlign - 1) & ~(rtScratchAlign - 1);
}

static VkAccelerationStructureKHR RT_CreateAS(struct MemAlloc* alloc, VkDeviceSize size, int type) {
	VkAccelerationStructureCreateInfoKHR info = { 0 };
	VkAccelerationStructureKHR as = 0;

	info.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	info.buffer = alloc->block->buffer;
	info.offset = alloc->offset;
	info.size   = size;
	info.type   = type;

	if (vkCreateAccelerationStructureKHR(device, &info, NULL, &as)) return 0;
	return as;
}

static void RT_FreeTLAS(struct Frame* f) {
	if (!f->tlas) return;
	vkDestroyAccelerationStructureKHR(device, f->tlas, NULL);
	MemAlloc_Free(&asPool,      &f->tlasAlloc);
	MemAlloc_Free(&scratchPool, &f->tlasScratch);

	f->tlas         = 0;
	f->tlasCapacity = 0;
}

static void RT_InitTLASGeometry(VkAccelerationStructureGeometryKHR* geometry, VkAccelerationStructureBuildGeometryInfoKHR* build) {
	geometry->sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry->geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

	build->sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	build->type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	build->flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	build->mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build->geometryCount = 1;
	build->pGeometries   = geometry;
}

/* Must be called before anything is drawn, since the descriptor set can't */
/*  be changed after it has been bound in a command buffer that is recording */
static void RT_EnsureTLAS(struct Frame* f) {
	VkAccelerationStructureGeometryKHR geometry       = { 0 };
	VkAccelerationStructureBuildGeometryInfoKHR build = { 0 };
	VkAccelerationStructureBuildSizesInfoKHR sizes    = { 0 };
	VkWriteDescriptorSetAccelerationStructureKHR tlasWrite = { 0 };
	VkWriteDescriptorSet write = { 0 };
	cc_uint32 capacity = numRtChunks + RT_TLAS_SLACK;

	if (f->tlas && f->tlasCapacity >= capacity) return;
	RT_FreeTLAS(f);
	capacity = max(capacity * 2, 4096);

	RT_InitTLASGeometry(&geometry, &build);
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &capacity, &sizes);

	if (!RT_Alloc(&asPool, &f->tlasAlloc, sizes.accelerationStructureSize, 256)) return;
	if (!RT_AllocScratch(&f->tlasScratch, sizes.buildScratchSize)) {
		MemAlloc_Free(&asPool, &f->tlasAlloc); return;
	}

	f->tlas = RT_CreateAS(&f->tlasAlloc, sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
	if (!f->tlas) {
		MemAlloc_Free(&asPool,      &f->tlasAlloc);
		MemAlloc_Free(&scratchPool, &f->tlasScratch);
		return;
	}
	f->tlasCapacity = capacity;

	tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	tlasWrite.accelerationStructureCount = 1;
	tlasWrite.pAccelerationStructures    = &f->tlas;

	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.pNext           = &tlasWrite;
	write.dstSet          = f->rtSet;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
}

static int RT_CountGeometries(struct ChunkRT* rt) {
	int i, count = 0;
	for (i = 0; i < rt->numRanges; i++)
	{
		count += RT_GeometryCount(rt->ranges[i * 2 + 1]);
	}
	return count;
}

/* Chunk vertices are quads, so they're drawn as triangles using the default index buffer */
static int RT_FillGeometries(struct ChunkRT* rt, VkAccelerationStructureGeometryKHR* geoms,
							VkAccelerationStructureBuildRangeInfoKHR* ranges, cc_uint32* primCounts) {
	struct MemAlloc* vb = rt->vb;
	struct MemAlloc* ib = (struct MemAlloc*)Gfx.DefaultIb;
	VkAccelerationStructureGeometryTrianglesDataKHR* tris;
	int i, first, count, vertices, n = 0;

	for (i = 0; i < rt->numRanges; i++)
	{
		first = rt->ranges[i * 2];
		count = rt->ranges[i * 2 + 1];

		for (; count > 0; n++, first += vertices, count -= vertices)
		{
			vertices = min(count, RT_MAX_GEOMETRY_VERTICES);
			Mem_Set(&geoms[n], 0, sizeof(geoms[n]));

			geoms[n].sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
			geoms[n].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			/* Cutout geometry isn't opaque, so the shader can check the texture alpha when it's hit */
			geoms[n].flags        = i < rt->numCutout ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

			tris = &geoms[n].geometry.triangles;
			tris->sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
			tris->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
			tris->vertexData.deviceAddress = MemAlloc_Address(vb);
			tris->vertexStride = SIZEOF_VERTEX_TEXTURED;
			tris->maxVertex    = (cc_uint32)(vb->size / SIZEOF_VERTEX_TEXTURED) - 1;
			tris->indexType    = VK_INDEX_TYPE_UINT16;
			tris->indexData.deviceAddress  = MemAlloc_Address(ib);

			ranges[n].primitiveCount  = vertices / 4 * 2;
			ranges[n].primitiveOffset = 0;
			ranges[n].firstVertex     = first;
			ranges[n].transformOffset = 0;
			primCounts[n] = ranges[n].primitiveCount;
		}
	}
	return n;
}

static cc_bool RT_PrepareChunk(struct ChunkRT* rt, VkAccelerationStructureBuildGeometryInfoKHR* build, cc_uint32* primCounts) {
	VkAccelerationStructureBuildSizesInfoKHR sizes = { 0 };
	VkAccelerationStructureDeviceAddressInfoKHR addressInfo = { 0 };
	struct MemAlloc* scratch;

	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, build, primCounts, &sizes);

	scratch = (struct MemAlloc*)Mem_TryAllocCleared(1, sizeof(struct MemAlloc));
	if (!scratch) return false;

	if (!RT_AllocScratch(scratch, sizes.buildScratchSize)) {
		Mem_Free(scratch); return false;
	}
	/* Scratch memory is only needed until the build has finished on the GPU */
	Garbage_Add(GARBAGE_SCRATCH, scratch);

	if (!RT_Alloc(&asPool, &rt->blasAlloc, sizes.accelerationStructureSize, 256)) return false;
	rt->blas = RT_CreateAS(&rt->blasAlloc, sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
	if (!rt->blas) return false;

	addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addressInfo.accelerationStructure = rt->blas;
	rt->address = vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);

	build->dstAccelerationStructure = rt->blas;
	build->scratchData.deviceAddress = RT_ScratchAddress(scratch);
	return true;
}

static void RT_BuildChunks(VkCommandBuffer cmd) {
	VkAccelerationStructureBuildGeometryInfoKHR* builds;
	const VkAccelerationStructureBuildRangeInfoKHR** buildRanges;
	VkAccelerationStructureGeometryKHR* geoms;
	VkAccelerationStructureBuildRangeInfoKHR* ranges;
	cc_uint32* primCounts;
	struct ChunkRT* rt;
	int i, n, numGeoms = 0, numBuilds = 0, maxGeoms = 0;

	if (!numRtPending) return;
	for (i = 0; i < numRtPending; i++) { maxGeoms += RT_CountGeometries(rtPending[i]); }

	builds      = (VkAccelerationStructureBuildGeometryInfoKHR*)Mem_Alloc(numRtPending, sizeof(*builds), "Vulkan BLAS builds");
	buildRanges = (const VkAccelerationStructureBuildRangeInfoKHR**)Mem_Alloc(numRtPending, sizeof(*buildRanges), "Vulkan BLAS builds");
	geoms       = (VkAccelerationStructureGeometryKHR*)Mem_Alloc(maxGeoms, sizeof(*geoms), "Vulkan BLAS geometry");
	ranges      = (VkAccelerationStructureBuildRangeInfoKHR*)Mem_Alloc(maxGeoms, sizeof(*ranges), "Vulkan BLAS geometry");
	primCounts  = (cc_uint32*)Mem_Alloc(maxGeoms, sizeof(*primCounts), "Vulkan BLAS geometry");

	for (i = 0; i < numRtPending; i++)
	{
		rt = rtPending[i];
		rt->listIndex = -1;
		n  = RT_FillGeometries(rt, &geoms[numGeoms], &ranges[numGeoms], &primCounts[numGeoms]);

		Mem_Set(&builds[numBuilds], 0, sizeof(builds[numBuilds]));
		builds[numBuilds].sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		builds[numBuilds].type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		builds[numBuilds].flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		builds[numBuilds].mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		builds[numBuilds].geometryCount = n;
		builds[numBuilds].pGeometries   = &geoms[numGeoms];

		/* If out of memory, the chunk just won't cast shadows */
		if (!RT_PrepareChunk(rt, &builds[numBuilds], &primCounts[numGeoms])) continue;

		buildRanges[numBuilds] = &ranges[numGeoms];
		numGeoms  += n;
		numBuilds++;

		rt->built = true;
		RT_ListAdd(&rtChunks, &numRtChunks, &maxRtChunks, rt);
	}

	if (numBuilds) vkCmdBuildAccelerationStructuresKHR(cmd, numBuilds, builds, buildRanges);
	numRtPending = 0;

	Mem_Free(builds);
	Mem_Free(buildRanges);
	Mem_Free(geoms);
	Mem_Free(ranges);
	Mem_Free(primCounts);
}

/* Each geometry's entry in the table is the address of the geometry's first vertex */
/*  (used for alpha testing cutout geometry, and for finding what bounced light hit) */
static int RT_FillTable(struct ChunkRT* rt, VkDeviceAddress* table) {
	VkDeviceAddress vb = MemAlloc_Address(rt->vb);
	int i, first, count, vertices, n = 0;

	for (i = 0; i < rt->numRanges; i++)
	{
		first = rt->ranges[i * 2];
		count = rt->ranges[i * 2 + 1];

		for (; count > 0; first += vertices, count -= vertices)
		{
			vertices   = min(count, RT_MAX_GEOMETRY_VERTICES);
			table[n++] = vb + (VkDeviceAddress)first * SIZEOF_VERTEX_TEXTURED;
		}
	}
	return n;
}

/* Light colour of a block is the average colour of its texture (brightened so the brightest channel is full) */
static PackedCol rtLightCols[BLOCK_COUNT];
static cc_bool rtLightColsValid[BLOCK_COUNT];
static BitmapCol* rtLightColsAtlas;
static int rtLightColsTileSize;

static void RT_AddTileColor(TextureLoc loc, int* sums) {
	int size = Atlas2D.TileSize, x, y;
	int x0 = Atlas2D_TileX(loc) * size, y0 = Atlas2D_TileY(loc) * size;
	BitmapCol* row;
	BitmapCol c;
	if (x0 + size > Atlas2D.Bmp.width || y0 + size > Atlas2D.Bmp.height) return;

	for (y = y0; y < y0 + size; y++)
	{
		row = Bitmap_GetRow(&Atlas2D.Bmp, y);
		for (x = x0; x < x0 + size; x++)
		{
			c = row[x];
			if (BitmapCol_A(c) < 128) continue;
			sums[0] += BitmapCol_R(c); sums[1] += BitmapCol_G(c); sums[2] += BitmapCol_B(c);
		}
	}
}

static PackedCol RT_LightColor(BlockID block) {
	int sums[3] = { 0, 0, 0 }, maxC;
	/* Texture pack changed, so the colours need to be calculated again */
	if (Atlas2D.Bmp.scan0 != rtLightColsAtlas || Atlas2D.TileSize != rtLightColsTileSize) {
		Mem_Set(rtLightColsValid, 0, sizeof(rtLightColsValid));
		rtLightColsAtlas    = Atlas2D.Bmp.scan0;
		rtLightColsTileSize = Atlas2D.TileSize;
	}
	if (rtLightColsValid[block]) return rtLightCols[block];

	if (Atlas2D.Bmp.scan0) {
		RT_AddTileColor(Block_Tex(block, FACE_YMAX), sums);
		RT_AddTileColor(Block_Tex(block, FACE_XMAX), sums);
	}
	maxC = max(sums[0], max(sums[1], sums[2]));
	rtLightCols[block] = maxC ? PackedCol_Make((int)(sums[0] * 255.0f / maxC), (int)(sums[1] * 255.0f / maxC),
												(int)(sums[2] * 255.0f / maxC), 255) : PACKEDCOL_WHITE;
	rtLightColsValid[block] = true;
	return rtLightCols[block];
}

/* Light emitting blocks are kept in a grid of cells covering the whole world. Each cell has a list of the lights */
/*  that reach it, with the most important lights for the cell first (see the shader for how it's used). Like the */
/*  sections Radiance uses for block lights, the grid is in world space, so it only has to be rebuilt when lights */
/*  change, rather than every frame */
#define RT_LIGHT_CELL      8    /* Size of each cell in blocks */
#define RT_LIGHT_REACH     16   /* How far light from a block reaches (NOTE: must match FLOAT_INV_REACH2) */
#define RT_LIGHTS_PER_CELL 16   /* NOTE: Must match LIGHTS_PER_CELL in misc/vulkan/VulkanShaderGen.c */
#define RT_MAX_LIGHTS      65536 /* (when there are more, the lights nearest to the camera are used) NOTE: the shader packs light indices into 16 bits */
#define RT_MAX_LIGHT_ENTRIES (16 * 1024 * 1024)
/* After lights change, the grid is rebuilt after waiting at least this many seconds (and 10 times as long as */
/*  the last rebuild took), so that e.g. loading lots of chunks or flowing lava doesn't rebuild it every frame */
#define RT_LIGHTS_DELAY    0.25
/* When there are more than RT_MAX_LIGHTS lights, the grid is rebuilt when the camera moves this far */
#define RT_LIGHTS_MOVE     32

static struct RingChunk* rtLightData; /* Lights, then the grid */
static VkDeviceSize rtLightGridOffset;
static int rtNumLights, rtGridCells[3]; /* Cells along each axis of the grid in rtLightData */
static int rtLightsGen;                 /* Number of times rtLightData was rebuilt */
static int rtLightCells[3];             /* Cells along each axis of the grid being built */
static cc_bool rtLightsDirty = true, rtLightsCapped;
static double rtLightsChangedTime, rtLightsDelay = RT_LIGHTS_DELAY;
static int rtLightsCam[3];
static void* rtLightsAtlas;

/* Temp data while building the grid */
static int*   rtLightsRaw;
static float* rtLightPower;
static int*   rtCellStart;
static int*   rtCellCount;
static int*   rtEntries;
static float* rtEntryWeights;
static int maxLightsRaw, maxLightPower, maxCellStart, maxCellCount, maxEntries, maxEntryWeights;

static void RT_LightsChanged(void) {
	if (!rtLightsDirty) rtLightsChangedTime = Game.Time;
	rtLightsDirty = true;
}

static void RT_FreeLights(void) {
	if (rtLightData) { RingChunk_Destroy(rtLightData); Mem_Free(rtLightData); }
	rtLightData = NULL;
	rtNumLights = 0;

	Mem_Free(rtLightsRaw);    rtLightsRaw    = NULL; maxLightsRaw    = 0;
	Mem_Free(rtLightPower);   rtLightPower   = NULL; maxLightPower   = 0;
	Mem_Free(rtCellStart);    rtCellStart    = NULL; maxCellStart    = 0;
	Mem_Free(rtCellCount);    rtCellCount    = NULL; maxCellCount    = 0;
	Mem_Free(rtEntries);      rtEntries      = NULL; maxEntries      = 0;
	Mem_Free(rtEntryWeights); rtEntryWeights = NULL; maxEntryWeights = 0;
	rtLightsDirty = true;
}

/* Makes sure the temp array has room for count elements (keeps its contents) */
static cc_bool RT_EnsureTemp(void** data, int* max, int count, int elemSize) {
	void* mem;
	if (count <= *max) return true;

	/* (Windows doesn't support reallocating NULL) */
	mem = *data ? Mem_TryRealloc(*data, count, elemSize) : Mem_TryAlloc(count, elemSize);
	if (!mem) return false;
	*data = mem; *max = count;
	return true;
}

/* Finds the cells that light from the light at the given position reaches (false if none) */
static cc_bool RT_LightCells(const float* pos, int* lo, int* hi) {
	int i, block;
	for (i = 0; i < 3; i++)
	{
		block = (int)Math_Floor(pos[i]);
		lo[i] = block - RT_LIGHT_REACH;
		hi[i] = block + RT_LIGHT_REACH + 1;
		if (hi[i] < 0 || lo[i] >= rtLightCells[i] * RT_LIGHT_CELL) return false;

		lo[i] = lo[i] < 0 ? 0 : lo[i] / RT_LIGHT_CELL;
		hi[i] = min(hi[i] / RT_LIGHT_CELL, rtLightCells[i] - 1);
	}
	return true;
}

#define RT_CellIndex(x, y, z) (((y) * rtLightCells[2] + (z)) * rtLightCells[0] + (x))

/* Distance along an axis from p to the range min to min + size */
static float RT_AxisDistance(float p, int min, int size) {
	if (p < min)        return min - p;
	if (p > min + size) return p - (min + size);
	return 0.0f;
}

/* How much light a light gives to (the closest point of) a cell, ignoring shadows (the same as the shader: */
/*  brightness * window / (1 + distance^2)). Lights closer to the centre of the cell are slightly more */
/*  important, so that the most important lights of a cell are the closest ones when e.g. lots are inside it */
static float RT_LightImportance(int index, const float* light, int x, int y, int z) {
	float dx = RT_AxisDistance(light[0], x * RT_LIGHT_CELL, RT_LIGHT_CELL);
	float dy = RT_AxisDistance(light[1], y * RT_LIGHT_CELL, RT_LIGHT_CELL);
	float dz = RT_AxisDistance(light[2], z * RT_LIGHT_CELL, RT_LIGHT_CELL);
	float d2 = dx * dx + dy * dy + dz * dz;
	float q  = d2 / (RT_LIGHT_REACH * RT_LIGHT_REACH);
	float window = 1.0f - q * q, cx, cy, cz;

	if (window <= 0.0f) return 0.0f;
	cx = light[0] - (x + 0.5f) * RT_LIGHT_CELL;
	cy = light[1] - (y + 0.5f) * RT_LIGHT_CELL;
	cz = light[2] - (z + 0.5f) * RT_LIGHT_CELL;
	return rtLightPower[index] * window * window / (1.0f + d2) / (1.0f + 0.01f * (cx * cx + cy * cy + cz * cz));
}

/* Moves the most important lights of a cell to the start of its list (most important first), keeping the */
/*  order of the others. Weights must be more than 0 */
static void RT_SortCell(int* entries, float* weights, int count) {
	int top[RT_LIGHTS_PER_CELL], topE[RT_LIGHTS_PER_CELL];
	float topW[RT_LIGHTS_PER_CELL];
	int i, j, numTop = 0;

	/* Find the most important lights in one pass (top is kept sorted, most important first) */
	for (i = 0; i < count; i++)
	{
		if (numTop == RT_LIGHTS_PER_CELL && weights[i] <= weights[top[RT_LIGHTS_PER_CELL - 1]]) continue;

		j = numTop < RT_LIGHTS_PER_CELL ? numTop++ : RT_LIGHTS_PER_CELL - 1;
		for (; j > 0 && weights[top[j - 1]] < weights[i]; j--) { top[j] = top[j - 1]; }
		top[j] = i;
	}

	/* Take them out of the list, then move the others to the end of the list (from the back, so nothing */
	/*  is overwritten before it's moved), then put them at the start */
	for (i = 0; i < numTop; i++)
	{
		topE[i] = entries[top[i]];
		topW[i] = weights[top[i]];
		weights[top[i]] = -1.0f;
	}
	for (i = count - 1, j = count - 1; i >= 0; i--)
	{
		if (weights[i] < 0.0f) continue;
		entries[j] = entries[i]; weights[j] = weights[i]; j--;
	}
	for (i = 0; i < numTop; i++)
	{
		entries[i] = topE[i]; weights[i] = topW[i];
	}
}

/* Copies (up to max of) every light in every chunk, whether or not its BLAS has been built yet. */
/*  Returns how many lights there are in total (which may be more than max) */
static int RT_GatherLights(int* lights, int max) {
	struct ChunkRT** lists[3];
	int counts[3], i, j, k, n = 0;
	struct ChunkRT* rt;

	lists[0] = rtChunks;    counts[0] = numRtChunks;
	lists[1] = rtPending;   counts[1] = numRtPending;
	lists[2] = rtLightOnly; counts[2] = numRtLightOnly;

	for (k = 0; k < 3; k++)
	{
		for (i = 0; i < counts[k]; i++)
		{
			rt = lists[k][i];
			for (j = 0; j < rt->numLights; j++, n++)
			{
				if (lights && n < max) Mem_Copy(&lights[n * 4], &rt->lights[j * 4], 4 * sizeof(int));
			}
		}
	}
	return n;
}

static int RT_CameraDistance(const int* light, const int* cam) {
	return max(Math_AbsI(light[0] - cam[0]), max(Math_AbsI(light[1] - cam[1]), Math_AbsI(light[2] - cam[2])));
}

/* Keeps only the RT_MAX_LIGHTS lights nearest to the camera (in the same order), returning how many are kept */
static int RT_KeepNearestLights(int* lights, int count, const int* cam) {
	int maxDist = max(World.Width, max(World.Height, World.Length)) * 2;
	int* buckets = (int*)Mem_TryAllocCleared(maxDist + 1, sizeof(int));
	int i, d, cut, kept = 0, extra, n = 0;
	if (!buckets) return RT_MAX_LIGHTS;

	for (i = 0; i < count; i++)
	{
		buckets[min(RT_CameraDistance(&lights[i * 4], cam), maxDist)]++;
	}
	/* Lights closer than cut are all kept, then as many as fit at distance cut */
	for (cut = 0; cut < maxDist && kept + buckets[cut] <= RT_MAX_LIGHTS; cut++) { kept += buckets[cut]; }
	extra = RT_MAX_LIGHTS - kept;
	Mem_Free(buckets);

	for (i = 0; i < count; i++)
	{
		d = min(RT_CameraDistance(&lights[i * 4], cam), maxDist);
		if (d > cut || (d == cut && extra-- <= 0)) continue;
		if (n != i) Mem_Copy(&lights[n * 4], &lights[i * 4], 4 * sizeof(int));
		n++;
	}
	return n;
}

static void RT_CameraBlock(int* cam) {
	cam[0] = (int)Math_Floor(Camera.CurrentPos.x);
	cam[1] = (int)Math_Floor(Camera.CurrentPos.y);
	cam[2] = (int)Math_Floor(Camera.CurrentPos.z);
}

/* Rebuilds the light data and grid from every light in the world */
/*  (if anything fails, RT_LightsChanged is called so that it's tried again later) */
static void RT_RebuildLights(void) {
	struct RingChunk* data;
	int lo[3], hi[3], x, y, z, i, n, cell, total, numLights, numCells, numEntries;
	int* light;
	float* lights;
	int* grid;
	BlockID block;
	PackedCol col;
	Vec3 boxMin, boxMax;
	float intensity, imp, sum;
	VkDeviceSize lightsSize, size;

	rtLightsDirty   = false;
	rtLightsAtlas   = Atlas2D.Bmp.scan0;
	rtLightCells[0] = (World.Width  + RT_LIGHT_CELL - 1) / RT_LIGHT_CELL;
	rtLightCells[1] = (World.Height + RT_LIGHT_CELL - 1) / RT_LIGHT_CELL;
	rtLightCells[2] = (World.Length + RT_LIGHT_CELL - 1) / RT_LIGHT_CELL;
	numCells  = rtLightCells[0] * rtLightCells[1] * rtLightCells[2];
	total     = RT_GatherLights(NULL, 0);

	if (!total || !numCells) {
		if (rtLightData) { Garbage_Add(GARBAGE_RING, rtLightData); rtLightData = NULL; }
		rtNumLights = 0;
		return;
	}

	if (!RT_EnsureTemp((void**)&rtLightsRaw,  &maxLightsRaw,  total * 4, sizeof(int))   ||
		!RT_EnsureTemp((void**)&rtLightPower, &maxLightPower, total,     sizeof(float)) ||
		!RT_EnsureTemp((void**)&rtCellStart,  &maxCellStart,  numCells,  sizeof(int))   ||
		!RT_EnsureTemp((void**)&rtCellCount,  &maxCellCount,  numCells,  sizeof(int))) { RT_LightsChanged(); return; }
	RT_GatherLights(rtLightsRaw, total);

	/* When there are too many lights, the nearest to the camera are used (and rebuilt when it moves far enough) */
	RT_CameraBlock(rtLightsCam);
	rtLightsCapped = total > RT_MAX_LIGHTS;
	numLights      = rtLightsCapped ? RT_KeepNearestLights(rtLightsRaw, total, rtLightsCam) : total;

	lightsSize = (VkDeviceSize)numLights * 12 * sizeof(float);
	lights     = (float*)Mem_TryAlloc(numLights * 12, sizeof(float));
	if (!lights) { RT_LightsChanged(); return; }

	for (i = 0; i < numLights; i++)
	{
		light     = &rtLightsRaw[i * 4];
		block     = (BlockID)(light[3] & 0xFFFF);
		col       = RT_LightColor(block);
		intensity = max(Blocks.Brightness[block] & 0x0F, Blocks.Brightness[block] >> 4) / 15.0f;

		/* The box of the block as it's drawn (e.g. lava is drawn a bit lower than a full block) */
		boxMin = Blocks.RenderMinBB[block]; boxMax = Blocks.RenderMaxBB[block];

		lights[i * 12 + 0]  = light[0] + (boxMin.x + boxMax.x) * 0.5f;
		lights[i * 12 + 1]  = light[1] + (boxMin.y + boxMax.y) * 0.5f;
		lights[i * 12 + 2]  = light[2] + (boxMin.z + boxMax.z) * 0.5f;
		lights[i * 12 + 3]  = intensity;
		lights[i * 12 + 4]  = PackedCol_R(col) / 255.0f;
		lights[i * 12 + 5]  = PackedCol_G(col) / 255.0f;
		lights[i * 12 + 6]  = PackedCol_B(col) / 255.0f;
		lights[i * 12 + 7]  = (float)(light[3] >> 16); /* Faces light gets out of (see Builder_LightFaces) */
		lights[i * 12 + 8]  = (boxMax.x - boxMin.x) * 0.5f;
		lights[i * 12 + 9]  = (boxMax.y - boxMin.y) * 0.5f;
		lights[i * 12 + 10] = (boxMax.z - boxMin.z) * 0.5f;
		lights[i * 12 + 11] = 0.0f;

		/* How bright the light is overall (same luminance weights as the shader) */
		rtLightPower[i] = intensity * (0.299f * lights[i * 12 + 4] + 0.587f * lights[i * 12 + 5] + 0.114f * lights[i * 12 + 6]);
	}

	/* Count how many lights reach each cell (lights that can't light any of a cell aren't in its list) */
	Mem_Set(rtCellCount, 0, numCells * sizeof(int));
	numEntries = 0;
	for (i = 0; i < numLights; i++)
	{
		if (!RT_LightCells(&lights[i * 12], lo, hi)) continue;

		for (y = lo[1]; y <= hi[1]; y++)
			for (z = lo[2]; z <= hi[2]; z++)
				for (x = lo[0]; x <= hi[0]; x++)
				{
					if (RT_LightImportance(i, &lights[i * 12], x, y, z) <= 0.0f) continue;
					rtCellCount[RT_CellIndex(x, y, z)]++; numEntries++;
				}
	}
	numEntries = min(numEntries, RT_MAX_LIGHT_ENTRIES);

	if (!RT_EnsureTemp((void**)&rtEntries,      &maxEntries,      numEntries, sizeof(int)) ||
		!RT_EnsureTemp((void**)&rtEntryWeights, &maxEntryWeights, numEntries, sizeof(float))) {
		Mem_Free(lights); RT_LightsChanged(); return;
	}

	/* Each cell's list starts after the previous cell's list */
	n = 0;
	for (cell = 0; cell < numCells; cell++)
	{
		rtCellStart[cell] = n;
		n += rtCellCount[cell];
		rtCellCount[cell] = 0;
	}

	/* Add each light to every cell it reaches, along with how important it is to that cell */
	for (i = 0; i < numLights; i++)
	{
		if (!RT_LightCells(&lights[i * 12], lo, hi)) continue;

		for (y = lo[1]; y <= hi[1]; y++)
			for (z = lo[2]; z <= hi[2]; z++)
				for (x = lo[0]; x <= hi[0]; x++)
				{
					imp = RT_LightImportance(i, &lights[i * 12], x, y, z);
					if (imp <= 0.0f) continue;

					cell = RT_CellIndex(x, y, z);
					n    = rtCellStart[cell] + rtCellCount[cell];
					if (n >= numEntries) continue;

					rtEntries[n]      = i;
					rtEntryWeights[n] = imp;
					rtCellCount[cell]++;
				}
	}

	/* The light data, then the grid: (offset, count) for each cell, then each cell's list of */
	/*  (light index, running total of importance), the total only being for lights after the first 16 */
	size = lightsSize + ((VkDeviceSize)numCells * 2 + (VkDeviceSize)numEntries * 2) * sizeof(int);
	data = (struct RingChunk*)Mem_TryAllocCleared(1, sizeof(struct RingChunk));
	if (!data || !RingChunk_Create(data, size)) { Mem_Free(data); Mem_Free(lights); RT_LightsChanged(); return; }

	Mem_Copy(data->data, lights, (cc_uint32)lightsSize);
	Mem_Free(lights);
	grid = (int*)(data->data + lightsSize);

	for (cell = 0; cell < numCells; cell++)
	{
		n = rtCellStart[cell];
		if (rtCellCount[cell] > 1) RT_SortCell(&rtEntries[n], &rtEntryWeights[n], rtCellCount[cell]);
		grid[cell * 2]     = numCells * 2 + n * 2;
		grid[cell * 2 + 1] = rtCellCount[cell];

		sum = 0.0f;
		for (i = 0; i < rtCellCount[cell]; i++)
		{
			if (i >= RT_LIGHTS_PER_CELL) sum += rtEntryWeights[n + i];
			grid[numCells * 2 + (n + i) * 2]     = rtEntries[n + i];
			((float*)grid)[numCells * 2 + (n + i) * 2 + 1] = sum;
		}
	}

	/* The old data might still be used by frames the GPU hasn't finished yet */
	if (rtLightData) Garbage_Add(GARBAGE_RING, rtLightData);
	rtLightData       = data;
	rtLightGridOffset = lightsSize;
	rtNumLights       = numLights;
	rtLightsGen++;
	Mem_Copy(rtGridCells, rtLightCells, sizeof(rtGridCells));
}

/* Whether the world is a different size to when the grid was built (e.g. a new map was loaded) */
static cc_bool RT_WorldResized(void) {
	return rtGridCells[0] != (World.Width  + RT_LIGHT_CELL - 1) / RT_LIGHT_CELL
		|| rtGridCells[1] != (World.Height + RT_LIGHT_CELL - 1) / RT_LIGHT_CELL
		|| rtGridCells[2] != (World.Length + RT_LIGHT_CELL - 1) / RT_LIGHT_CELL;
}

/* Sets this frame's light data, first rebuilding it if lights changed */
static void RT_BuildLights(struct RTParams* p) {
	cc_uint64 beg, end;
	int cam[3];
	p->numLights = 0;
	if (!rtBlockLights) return;

	/* Light colours come from the textures */
	if (Atlas2D.Bmp.scan0 != rtLightsAtlas) RT_LightsChanged();
	if (rtLightsCapped) {
		RT_CameraBlock(cam);
		if (RT_CameraDistance(cam, rtLightsCam) > RT_LIGHTS_MOVE) RT_LightsChanged();
	}

	/* (the old lights are wrong for a new map, so the grid is rebuilt straight away then) */
	if (rtLightsDirty && (!rtLightData || RT_WorldResized() || Game.Time - rtLightsChangedTime >= rtLightsDelay)) {
		beg = Stopwatch_Measure();
		RT_RebuildLights();
		end = Stopwatch_Measure();
		rtLightsDelay = max(RT_LIGHTS_DELAY, 10.0 * Stopwatch_ElapsedMicroseconds(beg, end) / 1e6);
	}
	if (!rtLightData || !rtNumLights) return;

	p->lights      = rtLightData->address;
	p->lightGrid   = rtLightData->address + rtLightGridOffset;
	p->gridW       = (float)rtGridCells[0];
	p->gridH       = (float)rtGridCells[1];
	p->gridL       = (float)rtGridCells[2];
	p->invCellSize = 1.0f / RT_LIGHT_CELL;
	p->numLights   = rtNumLights;
	/* The shader packs this with light indices (which are below 65536) into a float, so it must be below 256 */
	p->lightsGen   = rtLightsGen & 0xFF;
}

static void RT_BuildTLAS(VkCommandBuffer cmd) {
	VkAccelerationStructureGeometryKHR geometry       = { 0 };
	VkAccelerationStructureBuildGeometryInfoKHR build = { 0 };
	VkAccelerationStructureBuildRangeInfoKHR range    = { 0 };
	const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
	VkAccelerationStructureInstanceKHR* instances;
	VkDeviceAddress* table;
	VkDeviceAddress address, tableAddress = 0;
	struct ChunkRT* rt;
	cc_uint32 i, flags, base = 0, numGeoms = 0, count = min((cc_uint32)numRtChunks, frame->tlasCapacity);
	cc_bool inTable, overflow = false;

	for (i = 0; i < count; i++) { numGeoms += rtChunks[i]->numGeoms; }
	table = (VkDeviceAddress*)Ring_AllocAddress(max(numGeoms, 1) * sizeof(VkDeviceAddress), &tableAddress);

	instances = (VkAccelerationStructureInstanceKHR*)Ring_AllocAddress(
					max(count, 1) * sizeof(VkAccelerationStructureInstanceKHR), &address);
	/* Still have to build an empty TLAS, since draws using it have already been recorded */
	if (!instances) { count = 0; address = 0; }

	/* Chunk vertices are already in world space, so every instance just uses an identity transform */
	for (i = 0; i < count; i++)
	{
		rt = rtChunks[i];
		Mem_Set(&instances[i], 0, sizeof(instances[i]));
		instances[i].matrix[0][0] = 1.0f;
		instances[i].matrix[1][1] = 1.0f;
		instances[i].matrix[2][2] = 1.0f;
		flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;

		/* The custom index is where the instance's geometries start in the table */
		/*  (if there's no room in the table, cutout geometries are just treated as opaque) */
		inTable = table && base < (1u << 24);
		if (inTable) {
			instances[i].customIndexAndMask = base | (0xFFu << 24);
			base += RT_FillTable(rt, table + base);
		} else {
			instances[i].customIndexAndMask = 0xFFu << 24;
			overflow = true;
		}
		if (!rt->numCutout || !inTable) flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
		instances[i].sbtOffsetAndFlags = flags << 24;
		instances[i].accelerationStructureReference = rt->address;
	}
	RT_FrameParams()->table = table ? tableAddress : 0;
	/* Bounced light needs the table to find what was hit */
	if (!table || overflow) RT_FrameParams()->giStrength = 0.0f;
	RT_BuildLights(RT_FrameParams());

	RT_InitTLASGeometry(&geometry, &build);
	geometry.geometry.instances.data.deviceAddress = address;
	build.dstAccelerationStructure = frame->tlas;
	build.scratchData.deviceAddress = RT_ScratchAddress(&frame->tlasScratch);
	range.primitiveCount = count;

	vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &pRange);
}

static void RT_Barrier(VkCommandBuffer cmd, VkFlags srcStage, VkFlags srcAccess, VkFlags dstStage, VkFlags dstAccess) {
	VkMemoryBarrier barrier = { 0 };
	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* Acceleration structures are built at the end of the upload command buffer, */
/*  which is executed before the frame's draw command buffer */
static void RT_RecordBuilds(void) {
	VkCommandBuffer cmd;
	if (!rtActive || !frame->tlas) return;
	cmd = UploadCmd();

	/* Chunk vertices might have only just been copied into the VBs */
	/*  (fragment shaders also read the UVs of cutout geometry vertices) */
	RT_Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	RT_BuildChunks(cmd);

	RT_Barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
				VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
	RT_BuildTLAS(cmd);

	RT_Barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

/* Blocks in shadow are darkened by the same amount as with classic lighting */
static float RT_ShadowRatio(int shadow, int sun) { return sun ? min((float)shadow / sun, 1.0f) : 1.0f; }

/* Returns the liquid the camera is inside (same check as for underwater fog), or BLOCK_AIR */
static BlockID RT_CameraLiquid(void) {
	Vec3 pos = Camera.CurrentPos;
	BlockID block;
	IVec3 coords;

	if (!World.Loaded) return BLOCK_AIR;
	IVec3_Floor(&coords, &pos);
	block = World_SafeGetBlock(coords.x, coords.y, coords.z);
	if (!Blocks.IsLiquid[block] || !Blocks.FogDensity[block]) return BLOCK_AIR;

	if (pos.x < coords.x + Blocks.MinBB[block].x || pos.x > coords.x + Blocks.MaxBB[block].x) return BLOCK_AIR;
	if (pos.y < coords.y + Blocks.MinBB[block].y || pos.y > coords.y + Blocks.MaxBB[block].y) return BLOCK_AIR;
	if (pos.z < coords.z + Blocks.MinBB[block].z || pos.z > coords.z + Blocks.MaxBB[block].z) return BLOCK_AIR;
	return block;
}

/* Under water, the water scatters much more light than air (mostly forwards), tinted by the */
/*  water's fog colour, and the shafts are made wavy by moving caustics */
static void RT_UpdateWater(struct RTParams* p) {
	BlockID liquid = RT_CameraLiquid();
	PackedCol fog;
	float r, g, b, maxC;
	if (liquid == BLOCK_AIR || Blocks.Draw[liquid] != DRAW_TRANSLUCENT) return;

	fog  = Blocks.FogCol[liquid];
	r    = (float)PackedCol_R(fog); g = (float)PackedCol_G(fog); b = (float)PackedCol_B(fog);
	maxC = max(r, max(g, b));
	if (maxC < 1.0f) maxC = 1.0f;

	p->godDensity   = Blocks.FogDensity[liquid] * 0.6f;
	Math_Clamp(p->godDensity, 0.02f, 0.15f);
	p->godG         = 0.7f;
	p->godRange     = 48.0f;
	p->godStrength *= 2.5f;
	p->caustics     = 1.0f;
	p->scatterR     = p->sunR * (0.3f + 0.7f * r / maxC);
	p->scatterG     = p->sunG * (0.3f + 0.7f * g / maxC);
	p->scatterB     = p->sunB * (0.3f + 0.7f * b / maxC);
}

/* Called at the start of the frame (the GPU has already finished with this frame's parameters) */
#define RT_TIME_WRAP (200.0 * 3.14159265358979)
static void RT_UpdateConstants(void) {
	struct RTParams* p = RT_FrameParams();
	float angle  = rtSunAngle_Degrees[rtSunAngle] * MATH_DEG2RAD;
	float horiz  = Math_CosF(angle);
	float radius = rtSoftness_Degrees[rtSoftness] * MATH_DEG2RAD;
	float spread = Math_SinF(radius) / Math_CosF(radius);
	Vec3 sun, up, u, v;

	sun.x = horiz * rtSunDir_X[rtSunDir];
	sun.y = Math_SinF(angle);
	sun.z = horiz * rtSunDir_Z[rtSunDir];
	p->sunX = sun.x; p->sunY = sun.y; p->sunZ = sun.z;
	/* So that the face pointing the most towards the sun receives full sunlight */
	p->diffuseScale = 1.0f / max(Math_AbsF(sun.x), max(Math_AbsF(sun.y), Math_AbsF(sun.z)));

	/* Two directions across the sun's disc, at right angles to each other and the sun direction */
	up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
	if (Math_AbsF(sun.y) > 0.99f) { up.x = 1.0f; up.y = 0.0f; }

	u.x = sun.y * up.z - sun.z * up.y;
	u.y = sun.z * up.x - sun.x * up.z;
	u.z = sun.x * up.y - sun.y * up.x;
	Vec3_Normalise(&u);
	v.x = sun.y * u.z - sun.z * u.y;
	v.y = sun.z * u.x - sun.x * u.z;
	v.z = sun.x * u.y - sun.y * u.x;

	p->uX = u.x * spread; p->uY = u.y * spread; p->uZ = u.z * spread;
	p->vX = v.x * spread; p->vY = v.y * spread; p->vZ = v.z * spread;
	p->samples = rtSoftness_Samples[rtSoftness];

	p->shadowDist = rtShadowDist_Blocks[rtShadowDist];
	p->camX = Camera.CurrentPos.x;
	p->camY = Camera.CurrentPos.y;
	p->camZ = Camera.CurrentPos.z;
	p->ratioR = RT_ShadowRatio(PackedCol_R(Env.ShadowCol), PackedCol_R(Env.SunCol));
	p->ratioG = RT_ShadowRatio(PackedCol_G(Env.ShadowCol), PackedCol_G(Env.SunCol));
	p->ratioB = RT_ShadowRatio(PackedCol_B(Env.ShadowCol), PackedCol_B(Env.SunCol));

	/* For finding where each pixel was in the previous frame's shadow history */
	p->prevMVP    = rtPrevMVP;
	p->hasPrev    = rtPrevDrawn && !rtResetHistory;
	rtResetHistory = false;
	p->screenW    = (float)swapExtent.width;
	p->screenH    = (float)swapExtent.height;
	/* Each pixel steps through the points on the sun's disc/hemisphere 9 at a time, rather than */
	/*  in order from the centre outwards (which shimmers). 9 doesn't divide 32 or 64, so all points */
	/*  are still used, and 2^24 is a multiple of both so there's no jump when the counter wraps */
	p->frame      = (int)((frameCounter * 9u) & 0xFFFFFF);
	p->maxHistory = RT_MAX_HISTORY;
	p->aoStrength = rtAO_Strength[rtAO];
	p->aoRange    = RT_AO_RANGE;

	/* Bounced light is relative to sunlight, since vertex colours include the sun colour */
	p->sunR = PackedCol_R(Env.SunCol) / 255.0f;
	p->sunG = PackedCol_G(Env.SunCol) / 255.0f;
	p->sunB = PackedCol_B(Env.SunCol) / 255.0f;
	p->giStrength = rtGI ? 1.0f : 0.0f;

	p->lightStrength = rtBlockLights_Strength[rtBlockLights];
	p->godStrength = rtGodRays_Strength[rtGodRays];
	p->godDensity  = RT_GOD_DENSITY;
	p->godG        = RT_GOD_G;
	p->godRange    = RT_GOD_RANGE;
	p->scatterR    = p->sunR;
	p->scatterG    = p->sunG;
	p->scatterB    = p->sunB;
	p->caustics    = 0.0f;
	RT_UpdateWater(p);

	/* Sky is moved up to stay above the camera (see EnvRenderer_RenderSky) */
	p->skyDY  = max(Camera.CurrentPos.y + 8.0f, (float)World.Height + 8.0f) - ((float)World.Height + 8.0f);
	p->skybox = EnvRenderer_ShouldRenderSkybox();
	/* Wrapped at a multiple of 20 pi, so the caustics (whose speeds are multiples of 0.1) don't jump */
	p->time   = (float)(Game.Time - RT_TIME_WRAP * (int)(Game.Time / RT_TIME_WRAP));
}

/* The shadow history written this frame is only usable next frame if chunks were drawn this frame */
static void RT_EndFrame(void) {
	rtPrevDrawn = rtDrawn;
	rtPrevMVP   = rtFrameMVP;
	rtDrawn     = false;
}

static void RT_BeginFrame(void) {
	cc_bool active = rtSupported && rtEnabled;

	if (active != rtActive) {
		rtActive = active;
		/* Chunks get rebuilt, which is when their BLASes get created */
		Lighting_SetGfxSunShadows(active);
	}

	if (!rtActive) { if (frame->tlas) RT_FreeTLAS(frame); return; }
	RT_UpdateConstants();
	RT_EnsureTLAS(frame);
}


static cc_bool RT_GetEnabled(void) { return rtEnabled; }
static void    RT_SetEnabled(cc_bool v) {
	rtEnabled = v;
	Options_SetBool(RT_OPT_ENABLED, v);
}

static void RT_SaveEnum(const char* key, const char* value) {
	cc_string str = String_FromReadonly(value);
	Options_Set(key, &str);
}

static int  RT_GetSunAngle(void) { return rtSunAngle; }
static void RT_SetSunAngle(int v) {
	rtResetHistory = true;
	rtSunAngle = v;
	RT_SaveEnum(RT_OPT_SUN_ANGLE, rtSunAngle_Names[v]);
}

static int  RT_GetSunDir(void) { return rtSunDir; }
static void RT_SetSunDir(int v) {
	rtResetHistory = true;
	rtSunDir = v;
	RT_SaveEnum(RT_OPT_SUN_DIR, rtSunDir_Names[v]);
}

static int  RT_GetShadowDist(void) { return rtShadowDist; }
static void RT_SetShadowDist(int v) {
	rtResetHistory = true;
	rtShadowDist = v;
	RT_SaveEnum(RT_OPT_SHADOW_DIST, rtShadowDist_Names[v]);
}

static int  RT_GetSoftness(void) { return rtSoftness; }
static void RT_SetSoftness(int v) {
	rtResetHistory = true;
	rtSoftness = v;
	RT_SaveEnum(RT_OPT_SOFTNESS, rtSoftness_Names[v]);
}

static int  RT_GetAO(void) { return rtAO; }
static void RT_SetAO(int v) {
	rtResetHistory = true;
	rtAO = v;
	RT_SaveEnum(RT_OPT_AO, rtAO_Names[v]);
}

static int  RT_GetBlockLights(void) { return rtBlockLights; }
static void RT_SetBlockLights(int v) {
	rtResetHistory = true;
	rtBlockLights = v;
	RT_SaveEnum(RT_OPT_BLOCK_LIGHTS, rtBlockLights_Names[v]);
}

static int  RT_GetGodRays(void) { return rtGodRays; }
static void RT_SetGodRays(int v) {
	rtResetHistory = true;
	rtGodRays = v;
	RT_SaveEnum(RT_OPT_GOD_RAYS, rtGodRays_Names[v]);
}

static int  RT_GetGI(void) { return rtGI; }
static void RT_SetGI(int v) {
	rtResetHistory = true;
	rtGI = v;
	RT_SaveEnum(RT_OPT_GI, rtGI_Names[v]);
}

static void RTXOptionsScreen_InitWidgets(struct MenuOptionsScreen* s) {
	MenuOptionsScreen_AddBool(s, "RT shadows",
		RT_GetEnabled,    RT_SetEnabled,
		"&eShadows from the sun are calculated using ray tracing.\n" \
		"&eReplaces the Lighting mode and Smooth lighting settings.");
	MenuOptionsScreen_AddEnum(s, "Sun angle", rtSunAngle_Names, Array_Elems(rtSunAngle_Names),
		RT_GetSunAngle,   RT_SetSunAngle,
		"&eHow high up in the sky the sun is.\n" \
		"&eLower angles cast longer shadows.");
	MenuOptionsScreen_AddEnum(s, "Sun direction", rtSunDir_Names, Array_Elems(rtSunDir_Names),
		RT_GetSunDir,     RT_SetSunDir,
		"&eWhich direction the sun shines from.");
	MenuOptionsScreen_AddEnum(s, "Shadow distance", rtShadowDist_Names, Array_Elems(rtShadowDist_Names),
		RT_GetShadowDist, RT_SetShadowDist,
		"&eHow far away blocks can still cast shadows from.\n" \
		"&eShorter distances render faster.");
	MenuOptionsScreen_AddEnum(s, "Shadow softness", rtSoftness_Names, Array_Elems(rtSoftness_Names),
		RT_GetSoftness,   RT_SetSoftness,
		"&eHow blurry the edges of shadows are.\n" \
		"&eSofter shadows use more rays, so render slower.");
	MenuOptionsScreen_AddEnum(s, "Ambient occlusion", rtAO_Names, Array_Elems(rtAO_Names),
		RT_GetAO,         RT_SetAO,
		"&eDarkens corners, caves and areas under trees,\n" \
		"&ebased on how much of the sky they can see.");
	MenuOptionsScreen_AddEnum(s, "Global illumination", rtGI_Names, Array_Elems(rtGI_Names),
		RT_GetGI,         RT_SetGI,
		"&eLight bounces off blocks onto nearby blocks,\n" \
		"&ee.g. sunlit grass lights up walls next to it.");
	MenuOptionsScreen_AddEnum(s, "God rays", rtGodRays_Names, Array_Elems(rtGodRays_Names),
		RT_GetGodRays,    RT_SetGodRays,
		"&eShafts of sunlight in the air, e.g. through trees,\n" \
		"&ebrightest when looking towards the sun.");
	MenuOptionsScreen_AddEnum(s, "Block lights", rtBlockLights_Names, Array_Elems(rtBlockLights_Names),
		RT_GetBlockLights, RT_SetBlockLights,
		"&eBlocks that give off light (e.g. lava) light up\n" \
		"&enearby blocks, in the colour of their texture.");
}


/*########################################################################################################################*
*-----------------------------------------------------Frame management----------------------------------------------------*
*#########################################################################################################################*/
static VkResult SubmitFrame(cc_bool present) {
	static const VkFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit = { 0 };
	VkCommandBuffer cmds[2];
	cc_uint32 count = 0;

	RT_RecordBuilds();
	if (uploadBegun) { EndUploadCmd(); cmds[count++] = frame->uploadCmd; }
	if (drawBegun)   { vkEndCommandBuffer(frame->drawCmd); cmds[count++] = frame->drawCmd; }

	submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = count;
	submit.pCommandBuffers    = cmds;

	if (acquirePending) {
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores    = &frame->acquireSem;
		submit.pWaitDstStageMask  = &waitStage;
	}
	if (present) {
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores    = &renderSems[imageIndex];
	}

	uploadBegun    = false;
	drawBegun      = false;
	acquirePending = false;
	return vkQueueSubmit(queue, 1, &submit, frame->fence);
}

static void PresentFrame(void) {
	VkPresentInfoKHR info = { 0 };
	VkResult res;

	info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.waitSemaphoreCount = 1;
	info.pWaitSemaphores    = &renderSems[imageIndex];
	info.swapchainCount     = 1;
	info.pSwapchains        = &swapchain;
	info.pImageIndices      = &imageIndex;

	res = vkQueuePresentKHR(queue, &info);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		swapchainDirty = true;
	} else if (res) {
		HandleError(res, "Failed to present Vulkan swapchain image");
	}
}

/* Waits until the GPU has finished with the next frame's resources, then resets them */
static void NextFrame(void) {
	frameIndex = (frameIndex + 1) % FRAMES_IN_FLIGHT;
	frame      = &frames[frameIndex];
	frameCounter++;

	vkWaitForFences(device, 1, &frame->fence, true, WAIT_FOREVER);
	vkResetFences(device, 1, &frame->fence);
	vkResetCommandPool(device, frame->pool, 0);
	Garbage_Collect(frame);
	Ring_Reset(frame);
}

/* Game doesn't call Gfx_EndFrame when the window is inactive (see Game_RenderFrame) */
void Gfx_BeginFrame(void) {
	if (frameOpen) Gfx_EndFrame();
	frameOpen = true;
	RT_BeginFrame();
}

void Gfx_EndFrame(void) {
	cc_bool present = imageAcquired;
	VkResult res;

	if (renderPassActive) {
		FlushDeferredDraws();
		vkCmdEndRenderPass(frame->drawCmd);
		renderPassActive = false;
	}

	res = SubmitFrame(present);
	RT_EndFrame();
	if (res) {
		HandleError(res, "Failed to submit Vulkan commands");
	} else if (present) {
		PresentFrame();
	}

	imageAcquired = false;
	frameOpen     = false;
	NextFrame();

	/* run at reduced FPS when minimised */
	if (!swapchain) { TickReducedPerformance(); return; }
	EndReducedPerformance();
}

void Gfx_SetVSync(cc_bool vsync) {
	if (gfx_vsync == vsync) return;
	gfx_vsync      = vsync;
	swapchainDirty = swapchain != 0;
}

/* Swapchain is recreated when the next swapchain image is acquired */
void Gfx_OnWindowResize(int width, int height) {
	swapchainDirty = true;
	Gfx_SetViewport(0, 0, width, height);
	Gfx_SetScissor( 0, 0, width, height);
}


/*########################################################################################################################*
*-----------------------------------------------------------Misc----------------------------------------------------------*
*#########################################################################################################################*/
static BitmapCol* Vulkan_GetRow(struct Bitmap* bmp, int y, void* ctx) {
	return (BitmapCol*)((cc_uint8*)ctx + (cc_uint32)y * bmp->width * BITMAPCOLOR_SIZE);
}

/* Screenshots are taken in the middle of a frame, so rendering has to be paused to copy */
/*  the swapchain image, then resumed afterwards with the resume render pass */
cc_result Gfx_TakeScreenshot(struct Stream* output) {
	VkBufferCreateInfo info    = { 0 };
	VkMemoryAllocateInfo alloc = { 0 };
	VkBufferImageCopy region   = { 0 };
	VkMemoryBarrier barrier    = { 0 };
	VkMemoryRequirements reqs;
	VkDeviceMemory memory = 0;
	VkBuffer buffer = 0;
	VkCommandBuffer cmd;
	VkImage image;
	struct Bitmap bmp;
	cc_uint8* data;
	cc_uint8 tmp;
	cc_uint32 i, pixels;
	void* mapped;
	cc_result res;
	int type;

	if (!(swapUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !EnsureRenderPass()) return ERR_NOT_SUPPORTED;
	pixels = swapExtent.width * swapExtent.height;

	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size        = (VkDeviceSize)pixels * 4;
	info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &info, NULL, &buffer)) return ERR_OUT_OF_MEMORY;

	vkGetBufferMemoryRequirements(device, buffer, &reqs);
	type = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
							VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize  = reqs.size;
	alloc.memoryTypeIndex = type;

	if (type < 0 || vkAllocateMemory(device, &alloc, NULL, &memory)) {
		vkDestroyBuffer(device, buffer, NULL);
		return ERR_OUT_OF_MEMORY;
	}
	vkBindBufferMemory(device, buffer, memory, 0);

	cmd   = frame->drawCmd;
	image = swapImages[imageIndex];
	FlushDeferredDraws();
	vkCmdEndRenderPass(cmd);
	renderPassActive = false;

	ImageBarrier(cmd, image, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);

	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width  = swapExtent.width;
	region.imageExtent.height = swapExtent.height;
	region.imageExtent.depth  = 1;
	vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

	ImageBarrier(cmd, image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);

	res = SubmitFrame(false);
	if (!res) res = vkWaitForFences(device, 1, &frame->fence, true, WAIT_FOREVER);
	vkResetFences(device, 1, &frame->fence);
	vkResetCommandPool(device, frame->pool, 0);
	BeginRenderPass(resumePass);

	if (!res) res = vkMapMemory(device, memory, 0, info.size, 0, &mapped);
	if (!res) {
		data = (cc_uint8*)mapped;
		/* Swapchain format might be RGBA instead of BGRA */
		if (colorFormat == VK_FORMAT_R8G8B8A8_UNORM) {
			for (i = 0; i < pixels; i++)
			{
				tmp             = data[i * 4 + 0];
				data[i * 4 + 0] = data[i * 4 + 2];
				data[i * 4 + 2] = tmp;
			}
		}

		Bitmap_Init(bmp, swapExtent.width, swapExtent.height, NULL);
		res = Png_Encode(&bmp, output, Vulkan_GetRow, false, data);
	}

	vkDestroyBuffer(device, buffer, NULL);
	vkFreeMemory(device, memory, NULL);
	return res;
}

cc_bool Gfx_WarnIfNecessary(void) { return false; }
cc_bool Gfx_GetUIOptions(struct MenuOptionsScreen* s) {
	if (rtSupported) {
		MenuOptionsScreen_AddSubmenu(s, "RTX options...", RTXOptionsScreen_InitWidgets,
			"&eOptions for ray tracing effects.\n" \
			"&eRequires a GPU with ray tracing support.");
	}
	return false;
}

void Gfx_GetApiInfo(cc_string* info) {
	int pointerSize = sizeof(void*) * 8;
	int major = VK_VERSION_MAJOR(props.apiVersion);
	int minor = VK_VERSION_MINOR(props.apiVersion);
	int patch = VK_VERSION_PATCH(props.apiVersion);
	VkDeviceSize vram = 0;
	float vram_;
	cc_uint32 i;

	String_Format1(info, "-- Using Vulkan (%i bit) --\n", &pointerSize);
	String_Format1(info, "Adapter: %c\n", props.deviceName);
	String_Format3(info, "Vulkan version: %i.%i.%i\n", &major, &minor, &patch);

	for (i = 0; i < memProps.memoryHeapCount; i++)
	{
		if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += memProps.memoryHeaps[i].size;
	}
	vram_ = vram / (1024.0f * 1024.0f);
	String_Format1(info, "Graphics memory: %f2 MB VRAM\n", &vram_);
	PrintMaxTextureInfo(info);

	String_Format1(info, "Ray tracing: %c\n", rtActive ? "active" : (rtSupported ? "supported" : "unsupported"));
	if (rtActive) String_Format1(info, "Ray traced chunks: %i\n", &numRtChunks);
}
#endif
