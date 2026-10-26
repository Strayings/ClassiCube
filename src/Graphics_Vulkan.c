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
#define PIPELINE_KEYS    16384
#define WAIT_FOREVER     ((cc_uint64)-1)

#define GARBAGE_BUFFER  0
#define GARBAGE_TEXTURE 1
#define GARBAGE_SCRATCH 2

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
	int numRanges, numCutout, numCutoutGeoms, listIndex;
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
	float texX, texY, camX, camY;
	float fogR, fogG, fogB, fogEnd;
	float fogDensity, shadowRatio, camZ, _pad1;
	float sunX, sunY, sunZ, shadowDist;
} pc;

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
static VkRenderPass renderPass, resumePass;

static VkSwapchainKHR swapchain;
static VkExtent2D swapExtent;
static VkFlags swapUsage;
static cc_uint32 swapCount, imageIndex;
static VkImage swapImages[MAX_SWAP_IMAGES];
static VkImageView swapViews[MAX_SWAP_IMAGES];
static VkFramebuffer framebuffers[MAX_SWAP_IMAGES];
static VkSemaphore renderSems[MAX_SWAP_IMAGES];
static VkImage depthImage;
static VkImageView depthView;
static struct MemAlloc depthAlloc;
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
static cc_bool dirtyViewport, dirtyScissor, dirtyConstants, dirtyTexture, dirtyVb, dirtyIb, dirtyCull;
static struct Matrix _view, _proj;

/* Face culling is toggled a lot by MapRenderer, so with VK_EXT_extended_dynamic_state it's */
/*  just a cheap state change (like Direct3D) instead of having to switch to another pipeline */
static cc_bool dynamicCull;
static void (VKAPI_PTR *vkCmdSetCullModeEXT)(VkCommandBuffer cmd, VkFlags cullMode);

/* Ray traced shadows (using VK_KHR_ray_query, so only on GPUs with ray tracing support) */
static cc_bool rtSupported, rtEnabled, rtActive, dirtyRtSet;
static int rtSunAngle, rtSunDir, rtShadowDist;
static VkDeviceSize rtScratchAlign;
static struct MemPool asPool, scratchPool;
static VkDescriptorSetLayout rtSetLayout;
static VkDescriptorPool rtDescPool;
/* Contains the address of each frame's table of cutout geometry vertices */
/*  (256 is the largest minUniformBufferOffsetAlignment allowed by the Vulkan spec) */
static struct RingChunk rtParams;
#define RT_PARAMS_STRIDE 256
static VkShaderModule vsRtModule, fsRtModules[6];
/* Chunks waiting for their BLAS to be built, and chunks which have a BLAS */
static struct ChunkRT** rtPending;
static struct ChunkRT** rtChunks;
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
	return address.bufferDeviceAddress && as.accelerationStructure && rayQuery.rayQuery;
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
static VkRenderPass CreateRenderPass(cc_bool resume) {
	VkAttachmentDescription attachments[2] = { { 0 } };
	VkSubpassDependency deps[2]       = { { 0 } };
	VkSubpassDescription subpass      = { 0 };
	VkRenderPassCreateInfo info       = { 0 };
	VkAttachmentReference colorRef, depthRef;
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

	colorRef.attachment = 0;
	colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depthRef.attachment = 1;
	depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount    = 1;
	subpass.pColorAttachments       = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	/* Depth buffer is shared between frames, so wait for previous frame to finish writing to it */
	deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass    = 0;
	deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
							VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	/* Allows copying the swapchain image afterwards for screenshots */
	deps[1].srcSubpass    = 0;
	deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 2;
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
	cc_uint32 i;
	for (i = 0; i < swapCount; i++)
	{
		vkDestroyFramebuffer(device, framebuffers[i], NULL);
		vkDestroyImageView(device, swapViews[i], NULL);
		vkDestroySemaphore(device, renderSems[i], NULL);
	}
	swapCount = 0;

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

static void CreateSwapchain(void) {
	VkSwapchainCreateInfoKHR info  = { 0 };
	VkImageViewCreateInfo viewInfo = { 0 };
	VkFramebufferCreateInfo fbInfo = { 0 };
	VkSemaphoreCreateInfo semInfo  = { 0 };
	VkSwapchainKHR old = swapchain;
	VkSurfaceCapabilitiesKHR caps;
	VkImageView attachments[2];
	cc_uint32 i, count;
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

	viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format   = colorFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbInfo.renderPass      = renderPass;
	fbInfo.attachmentCount = 2;
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

		attachments[0] = swapViews[i];
		attachments[1] = depthView;
		res = vkCreateFramebuffer(device, &fbInfo, NULL, &framebuffers[i]);
		if (res) Process_Abort2(res, "Failed to create Vulkan framebuffer");

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
static const struct ShaderDesc vs_rt_desc = { vs_textured_rt, sizeof(vs_textured_rt) };

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
/*  and the address of the table of cutout geometry vertices (binding 1) */
static void CreateRTDescriptors(void) {
	VkDescriptorSetLayoutBinding bindings[2] = { { 0 } };
	VkDescriptorSetLayoutCreateInfo setInfo  = { 0 };
	VkDescriptorPoolCreateInfo poolInfo      = { 0 };
	VkDescriptorSetAllocateInfo allocInfo    = { 0 };
	VkDescriptorBufferInfo bufferInfo;
	VkWriteDescriptorSet write = { 0 };
	VkDescriptorPoolSize sizes[2];
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

	setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = 2;
	setInfo.pBindings    = bindings;

	res = vkCreateDescriptorSetLayout(device, &setInfo, NULL, &rtSetLayout);
	if (res) Process_Abort2(res, "Failed to create Vulkan ray tracing descriptor set layout");

	sizes[0].type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	sizes[0].descriptorCount = FRAMES_IN_FLIGHT;
	sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[1].descriptorCount = FRAMES_IN_FLIGHT;

	poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets       = FRAMES_IN_FLIGHT;
	poolInfo.poolSizeCount = 2;
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
		bufferInfo.range  = sizeof(VkDeviceAddress);
		write.dstSet      = frames[i].rtSet;
		vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
	}

	vsRtModule = CreateShader(&vs_rt_desc);
	for (i = 0; i < Array_Elems(fsRtModules); i++) fsRtModules[i] = CreateShader(&fs_rt_descs[i]);
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
	vkDestroyShaderModule(device, vsRtModule, NULL);
	for (i = 0; i < Array_Elems(fsRtModules); i++) vkDestroyShaderModule(device, fsRtModules[i], NULL);
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
	VkPipelineColorBlendAttachmentState blendAttachment  = { 0 };
	VkPipelineColorBlendStateCreateInfo blend            = { 0 };
	VkPipelineDynamicStateCreateInfo dynamic             = { 0 };
	VkGraphicsPipelineCreateInfo info                    = { 0 };
	VkPipeline pipeline;
	VkResult res;
	int fmt = key & 1, rt = (key >> 13) & 1;

	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = rt ? vsRtModule : vsModules[fmt];
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	/* Ray traced variants only exist for textured vertices (i.e. chunk meshes) */
	stages[1].module = rt ? fsRtModules[((key >> 1) & 1) + ((key >> 2) & 3) * 2] : fsModules[key & 0x0F];
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

	blendAttachment.blendEnable         = (key >> 4) & 1;
	blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
	blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
	blendAttachment.colorWriteMask      = (key >> 5) & 0x0F;

	blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments    = &blendAttachment;

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

	renderPass = CreateRenderPass(false);
	resumePass = CreateRenderPass(true);
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
/* Bound state is lost when starting a new command buffer */
static void InvalidateState(void) {
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
	BeginCmd(frame->drawCmd);
	drawBegun = true;
	InvalidateState();

	info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	info.renderPass        = pass;
	info.framebuffer       = framebuffers[imageIndex];
	info.renderArea.extent = swapExtent;

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

	BeginRenderPass(renderPass);
	return true;
}

/* Bit 0 = format, 1 = alpha test, 2-3 = fog, 4 = blending, 5-8 = colour mask, */
/*  9 = depth test, 10 = depth write, 11 = culling (only without dynamic cull mode), */
/*  12 = lines, 13 = ray traced shadows */
static int CalcPipelineKey(cc_bool lines, cc_bool rt) {
	int fog = 0, cull = dynamicCull ? 0 : cullFaces;
	if (gfx_fogEnabled && gfx_fogMode >= 0) fog = gfx_fogMode == FOG_LINEAR ? 1 : 2;

	return gfx_format | (gfx_alphaTest << 1) | (fog << 2) | (gfx_alphaBlend << 4) | (colorMask << 5) |
		(gfx_depthTest << 9) | (gfx_depthWrite << 10) | (cull << 11) | (lines << 12) | (rt << 13);
}

static cc_bool PrepareDraw(cc_bool lines, cc_bool terrain) {
	struct VulkanTexture* tex = curTex ? curTex : (struct VulkanTexture*)white_square;
	cc_bool textured = gfx_format == VERTEX_FORMAT_TEXTURED;
	cc_bool rt;
	VkCommandBuffer cmd;
	int key;

	if (gfx_format < 0 || !vbBuffer)  return false;
	if (!lines && !ibBuffer)         return false;
	if (textured && !tex)            return false;
	if (!EnsureRenderPass())         return false;
	cmd = frame->drawCmd;

	/* No point tracing rays when nothing is written to the colour buffer (e.g. depth only pass) */
	rt  = terrain && textured && rtActive && frame->tlas && colorMask;
	key = CalcPipelineKey(lines, rt);
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
	}
	if (dirtyVb) {
		vkCmdBindVertexBuffers(cmd, 0, 1, &vbBuffer, &vbOffset);
		dirtyVb = false;
	}
	if (dirtyIb && !lines) {
		vkCmdBindIndexBuffer(cmd, ibBuffer, ibOffset, VK_INDEX_TYPE_UINT16);
		dirtyIb = false;
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
	viewport.x        = (float)x;
	viewport.y        = (float)(y + h);
	viewport.width    = (float)w;
	viewport.height   = -(float)h;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	dirtyViewport     = true;
}

void Gfx_SetScissor(int x, int y, int w, int h) {
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
/* Each geometry can only use up to 16384 quads, since that's all the default index buffer has */
#define RT_MAX_GEOMETRY_VERTICES 65536
/* Extra space in the top level acceleration structure for chunks built during the frame */
#define RT_TLAS_SLACK 512
#define RT_GeometryCount(vertices) (((vertices) + RT_MAX_GEOMETRY_VERTICES - 1) / RT_MAX_GEOMETRY_VERTICES)

static const char* const rtSunAngle_Names[]   = { "Low", "Medium", "High", "Overhead" };
static const char* const rtSunDir_Names[]     = { "North", "Northeast", "East", "Southeast", "South", "Southwest", "West", "Northwest" };
static const char* const rtShadowDist_Names[] = { "64", "128", "256", "Max" };
static const float rtSunAngle_Degrees[]  = { 20.0f, 40.0f, 60.0f, 90.0f };
static const float rtShadowDist_Blocks[] = { 64.0f, 128.0f, 256.0f, 4096.0f };
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
static void RT_Unlink(struct ChunkRT* rt) {
	if (rt->listIndex < 0) return;

	if (rt->built) {
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
	Mem_Free(rt);
}

static void RT_FreeAll(void) {
	while (numRtChunks)  { RT_FreeChunk(rtChunks[--numRtChunks]);   }
	while (numRtPending) { RT_FreeChunk(rtPending[--numRtPending]); }

	Mem_Free(rtChunks);
	Mem_Free(rtPending);
	rtChunks  = NULL; maxRtChunks  = 0;
	rtPending = NULL; maxRtPending = 0;
}

void Gfx_SetShadowCasters(GfxResourceID vb, const int* ranges, int numOpaque, int numCutout) {
	struct MemAlloc* alloc = (struct MemAlloc*)vb;
	int i, numRanges = numOpaque + numCutout;
	struct ChunkRT* rt;
	if (!rtActive || !alloc || !numRanges) return;

	rt = (struct ChunkRT*)Mem_TryAllocCleared(1, sizeof(struct ChunkRT));
	if (!rt) return;
	rt->ranges = (int*)Mem_TryAlloc(numRanges * 2, sizeof(int));
	if (!rt->ranges) { Mem_Free(rt); return; }

	/* Cutout ranges go first, so their geometries are the first geometries in the BLAS */
	Mem_Copy(rt->ranges,                 ranges + numOpaque * 2, numCutout * 2 * sizeof(int));
	Mem_Copy(rt->ranges + numCutout * 2, ranges,                 numOpaque * 2 * sizeof(int));
	for (i = 0; i < numCutout; i++)
	{
		rt->numCutoutGeoms += RT_GeometryCount(rt->ranges[i * 2 + 1]);
	}

	rt->numRanges = numRanges;
	rt->numCutout = numCutout;
	rt->vb        = alloc;
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
	build->flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
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

/* Each cutout geometry's entry in the table is the address of the geometry's first vertex */
static int RT_FillTable(struct ChunkRT* rt, VkDeviceAddress* table) {
	VkDeviceAddress vb = MemAlloc_Address(rt->vb);
	int i, first, count, vertices, n = 0;

	for (i = 0; i < rt->numCutout; i++)
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

	for (i = 0; i < count; i++) { numGeoms += rtChunks[i]->numCutoutGeoms; }
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

		/* The custom index is where the instance's cutout geometries start in the table */
		/*  (if there's no room in the table, cutout geometries are just treated as opaque) */
		if (table && rt->numCutoutGeoms && base < (1u << 24)) {
			instances[i].customIndexAndMask = base | (0xFFu << 24);
			base += RT_FillTable(rt, table + base);
		} else {
			instances[i].customIndexAndMask = 0xFFu << 24;
			flags |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
		}
		instances[i].sbtOffsetAndFlags = flags << 24;
		instances[i].accelerationStructureReference = rt->address;
	}
	*(VkDeviceAddress*)(rtParams.data + frameIndex * RT_PARAMS_STRIDE) = table ? tableAddress : 0;

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

static void RT_UpdateConstants(void) {
	float angle = rtSunAngle_Degrees[rtSunAngle] * MATH_DEG2RAD;
	float horiz = Math_CosF(angle);
	int sun     = PackedCol_R(Env.SunCol)    + PackedCol_G(Env.SunCol)    + PackedCol_B(Env.SunCol);
	int shadow  = PackedCol_R(Env.ShadowCol) + PackedCol_G(Env.ShadowCol) + PackedCol_B(Env.ShadowCol);

	pc.sunX = horiz * rtSunDir_X[rtSunDir];
	pc.sunY = Math_SinF(angle);
	pc.sunZ = horiz * rtSunDir_Z[rtSunDir];
	pc.shadowDist  = rtShadowDist_Blocks[rtShadowDist];
	pc.camX = Camera.CurrentPos.x;
	pc.camY = Camera.CurrentPos.y;
	pc.camZ = Camera.CurrentPos.z;
	/* Blocks in shadow are darkened by the same amount as with classic lighting */
	pc.shadowRatio = sun ? min((float)shadow / sun, 1.0f) : 1.0f;
	dirtyConstants = true;
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
	rtSunAngle = v;
	RT_SaveEnum(RT_OPT_SUN_ANGLE, rtSunAngle_Names[v]);
}

static int  RT_GetSunDir(void) { return rtSunDir; }
static void RT_SetSunDir(int v) {
	rtSunDir = v;
	RT_SaveEnum(RT_OPT_SUN_DIR, rtSunDir_Names[v]);
}

static int  RT_GetShadowDist(void) { return rtShadowDist; }
static void RT_SetShadowDist(int v) {
	rtShadowDist = v;
	RT_SaveEnum(RT_OPT_SHADOW_DIST, rtShadowDist_Names[v]);
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
		vkCmdEndRenderPass(frame->drawCmd);
		renderPassActive = false;
	}

	res = SubmitFrame(present);
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
