/*------------------------------------------------------------------------------------
	NVulkanDrv: Vulkan render device (see NVulkanDrvPrivate.h).
------------------------------------------------------------------------------------*/
#include "NVulkanDrvPrivate.h"
typedef uint32_t UINT;   // not defined by the UE1 headers
#if !defined(__FreeBSD__)
#include <dlfcn.h>
#endif

IMPLEMENT_PACKAGE(NVulkanDrv);
IMPLEMENT_CLASS(UNVulkanRenderDevice);

#ifdef UT99_PS5_PAD
// PS5 statistics overlay counters (ps5_keyboard.cpp shows them).
extern "C" { extern volatile unsigned ut_ps5_stat_draw_calls, ut_ps5_stat_render_passes, ut_ps5_stat_barriers, ut_ps5_stat_graphics_pipelines; }
#define UT_STAT(c) ( (c)++ )
#else
#define UT_STAT(c) ((void)0)
#endif
#include "Shaders/ut.vert.inc"
#include "Shaders/ut.frag.inc"

#define MASKED_TEXTURE_TAG (1ULL << 60ULL)   // see NOpenGLESDrv: palettized textures need a masked copy
#define ALPHA_MASK 0xff000000
#define LIGHTMAP_OVERBRIGHT_VALUE 1.4f
#define MAX_VERTS 32768
#define VTX_RING_BYTES (48u << 20)
#define IDX_RING_BYTES (12u << 20)
#define DESC_SETS 8192

static const DWORD AttribSizes[AT_Count] = { 3, 2, 2, 2, 2, 4, 4 };
static const DWORD AttribFlags[AT_Count] = { 0, SF_Texture0, SF_Texture1, SF_Texture2, SF_Texture3, SF_VtxColor, SF_VtxFog };

/*------------------------------------------------------------------------------------
	Vulkan function table. The PS5 title links RADV directly (no loader); elsewhere libvulkan is loaded.
------------------------------------------------------------------------------------*/
#if defined(__FreeBSD__)
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr( VkInstance, const char* );
#endif

#define VK_INSTANCE_FUNCS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE_FUNCS(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCreateFence) X(vkWaitForFences) \
	X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
	X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
	X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateRenderPass) X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) X(vkDestroyRenderPass) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
	X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
	X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
	X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdPushConstants) \
	X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdClearAttachments) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
	X(vkCmdCopyImageToBuffer)

#define DECLARE_FN(n) PFN_##n n;
struct FVk
{
	VK_INSTANCE_FUNCS(DECLARE_FN)
	VK_DEVICE_FUNCS(DECLARE_FN)

	VkInstance Instance; VkPhysicalDevice Phys; VkDevice Dev; VkQueue Queue; UINT Family;
	VkPhysicalDeviceMemoryProperties Mem;
	VkCommandPool Pool; VkCommandBuffer FrameCmd, UploadCmd; VkFence Fence;

	// Render targets.
	INT W, H;
	VkImage ColorImg, DepthImg; VkDeviceMemory ColorMem, DepthMem; VkImageView ColorView, DepthView;
	VkRenderPass Pass; VkFramebuffer Fb;
	VkBuffer ReadBuf; VkDeviceMemory ReadMem; BYTE* ReadPtr;

	// Rings.
	VkBuffer VtxBuf, IdxBuf; VkDeviceMemory VtxMem, IdxMem; BYTE *VtxPtr, *IdxPtr; VkDeviceSize VtxOff, IdxOff;

	// Staging for texture uploads.
	VkBuffer StageBuf; VkDeviceMemory StageMem; BYTE* StagePtr; VkDeviceSize StageSize;

	// Pipelines and descriptors.
	VkShaderModule VS, FS; VkDescriptorSetLayout SetLayout; VkPipelineLayout PipeLayout;
	std::map<DWORD, VkPipeline> Pipelines;
	VkDescriptorPool DescPool;
	std::map<std::array<uint32_t,4>, VkDescriptorSet> DescCache;
	INT DescUsed;
	VkSampler Samplers[4];   // bit0 = nearest, bit1 = clamp
	VkImage DummyImg; VkDeviceMemory DummyMem; VkImageView DummyView;

	// Device-local sub-allocation for textures.
	struct FBlock { VkDeviceMemory Mem; VkDeviceSize Size, Used; UINT Type; };
	std::vector<FBlock> Blocks;

	// Frame state.
	UBOOL FrameOpen, PassOpen;
	VkPipeline BoundPipe; VkDescriptorSet BoundSet;
	FLOAT ClearColor[4];
	UBOOL ClearColorPending;
};
#undef DECLARE_FN

static const char* VkErr( VkResult R ) { static char B[32]; sprintf( B, "VkResult %d", (int)R ); return B; }
#define VKCHECK(call) do { VkResult r_ = (call); if( r_ != VK_SUCCESS ) { appErrorf( "NVulkanDrv: %s failed (%s)", #call, VkErr(r_) ); } } while(0)

static UBOOL FindMemType( const FVk* V, UINT Bits, VkMemoryPropertyFlags Want, UINT& Out )
{
	for( UINT i = 0; i < V->Mem.memoryTypeCount; ++i )
		if( ( Bits & ( 1u << i ) ) && ( V->Mem.memoryTypes[i].propertyFlags & Want ) == Want ) { Out = i; return 1; }
	return 0;
}

static void CreateBuf( FVk* V, VkDeviceSize Size, VkBufferUsageFlags Usage, VkMemoryPropertyFlags Props, VkBuffer& Buf, VkDeviceMemory& Mem, BYTE** Mapped )
{
	VkBufferCreateInfo BI = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	BI.size = Size; BI.usage = Usage; BI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VKCHECK( V->vkCreateBuffer( V->Dev, &BI, NULL, &Buf ) );
	VkMemoryRequirements MR; V->vkGetBufferMemoryRequirements( V->Dev, Buf, &MR );
	UINT Type = 0;
	if( !FindMemType( V, MR.memoryTypeBits, Props, Type ) )
		appErrorf( "NVulkanDrv: no suitable memory type for a buffer" );
	VkMemoryAllocateInfo AI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	AI.allocationSize = MR.size; AI.memoryTypeIndex = Type;
	VKCHECK( V->vkAllocateMemory( V->Dev, &AI, NULL, &Mem ) );
	VKCHECK( V->vkBindBufferMemory( V->Dev, Buf, Mem, 0 ) );
	if( Mapped ) { void* P = NULL; VKCHECK( V->vkMapMemory( V->Dev, Mem, 0, VK_WHOLE_SIZE, 0, &P ) ); *Mapped = (BYTE*)P; }
}

// Sub-allocate device-local memory for an image (blocks are freed all at once on Flush).
static void BindImageMemory( FVk* V, VkImage Img )
{
	VkMemoryRequirements MR; V->vkGetImageMemoryRequirements( V->Dev, Img, &MR );
	UINT Type = 0;
	if( !FindMemType( V, MR.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Type ) && !FindMemType( V, MR.memoryTypeBits, 0, Type ) )
		appErrorf( "NVulkanDrv: no memory type for an image" );
	for( size_t i = 0; i < V->Blocks.size(); ++i )
	{
		FVk::FBlock& B = V->Blocks[i];
		if( B.Type != Type ) continue;
		VkDeviceSize Off = ( B.Used + MR.alignment - 1 ) & ~( MR.alignment - 1 );
		if( Off + MR.size <= B.Size ) { VKCHECK( V->vkBindImageMemory( V->Dev, Img, B.Mem, Off ) ); B.Used = Off + MR.size; return; }
	}
	FVk::FBlock NB; NB.Type = Type; NB.Size = MR.size > ( 64u << 20 ) ? MR.size : ( 64u << 20 ); NB.Used = 0;
	VkMemoryAllocateInfo AI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	AI.allocationSize = NB.Size; AI.memoryTypeIndex = Type;
	VKCHECK( V->vkAllocateMemory( V->Dev, &AI, NULL, &NB.Mem ) );
	VKCHECK( V->vkBindImageMemory( V->Dev, Img, NB.Mem, 0 ) );
	NB.Used = MR.size;
	V->Blocks.push_back( NB );
}

static void ImageBarrier( FVk* V, VkCommandBuffer Cmd, VkImage Img, VkImageAspectFlags Aspect, VkImageLayout From, VkImageLayout To,
	VkAccessFlags Src, VkAccessFlags Dst, VkPipelineStageFlags SrcStage, VkPipelineStageFlags DstStage, UINT Levels = 1 )
{
	VkImageMemoryBarrier B = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	B.srcAccessMask = Src; B.dstAccessMask = Dst; B.oldLayout = From; B.newLayout = To;
	B.srcQueueFamilyIndex = B.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	B.image = Img; B.subresourceRange = { Aspect, 0, Levels, 0, 1 };
	UT_STAT( ut_ps5_stat_barriers );
	V->vkCmdPipelineBarrier( Cmd, SrcStage, DstStage, 0, 0, NULL, 0, NULL, 1, &B );
}

static void SubmitWait( FVk* V, VkCommandBuffer Cmd )
{
	VkSubmitInfo SI = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	SI.commandBufferCount = 1; SI.pCommandBuffers = &Cmd;
	VKCHECK( V->vkQueueSubmit( V->Queue, 1, &SI, V->Fence ) );
	VKCHECK( V->vkWaitForFences( V->Dev, 1, &V->Fence, VK_TRUE, ~0ull ) );
	VKCHECK( V->vkResetFences( V->Dev, 1, &V->Fence ) );
}

/*------------------------------------------------------------------------------------
	Instance and device.
------------------------------------------------------------------------------------*/
static UBOOL CreateVulkan( FVk* V )
{
	PFN_vkGetInstanceProcAddr GetProc = NULL;
#if defined(__FreeBSD__)
	GetProc = (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr;
#else
	void* Lib = dlopen( "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL );
	if( !Lib ) { debugf( NAME_Warning, "NVulkanDrv: libvulkan.so.1 not found" ); return 0; }
	GetProc = (PFN_vkGetInstanceProcAddr)dlsym( Lib, "vkGetInstanceProcAddr" );
#endif
	if( !GetProc ) return 0;
	PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)GetProc( VK_NULL_HANDLE, "vkCreateInstance" );
	if( !CreateInstance ) return 0;

	VkApplicationInfo App = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	App.pApplicationName = "UT99"; App.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo IC = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	IC.pApplicationInfo = &App;
	if( CreateInstance( &IC, NULL, &V->Instance ) != VK_SUCCESS ) return 0;
#define LOAD_I(n) V->n = (PFN_##n)GetProc( V->Instance, #n );
	VK_INSTANCE_FUNCS(LOAD_I)
#undef LOAD_I

	UINT Count = 0;
	V->vkEnumeratePhysicalDevices( V->Instance, &Count, NULL );
	if( !Count ) return 0;
	std::vector<VkPhysicalDevice> Devs( Count );
	V->vkEnumeratePhysicalDevices( V->Instance, &Count, Devs.data() );
	V->Phys = Devs[0];
	for( UINT i = 0; i < Count; ++i )
	{
		VkPhysicalDeviceProperties P; V->vkGetPhysicalDeviceProperties( Devs[i], &P );
		if( P.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) { V->Phys = Devs[i]; break; }
	}
	VkPhysicalDeviceProperties P; V->vkGetPhysicalDeviceProperties( V->Phys, &P );
	debugf( NAME_Log, "NVulkanDrv: device %s (Vulkan %u.%u)", ANSI_TO_TCHAR(P.deviceName), VK_VERSION_MAJOR(P.apiVersion), VK_VERSION_MINOR(P.apiVersion) );
	V->vkGetPhysicalDeviceMemoryProperties( V->Phys, &V->Mem );

	UINT QF = 0; V->vkGetPhysicalDeviceQueueFamilyProperties( V->Phys, &QF, NULL );
	std::vector<VkQueueFamilyProperties> QP( QF );
	V->vkGetPhysicalDeviceQueueFamilyProperties( V->Phys, &QF, QP.data() );
	V->Family = ~0u;
	for( UINT i = 0; i < QF; ++i ) if( QP[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) { V->Family = i; break; }
	if( V->Family == ~0u ) return 0;

	float Prio = 1.f;
	VkDeviceQueueCreateInfo QC = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	QC.queueFamilyIndex = V->Family; QC.queueCount = 1; QC.pQueuePriorities = &Prio;
	VkDeviceCreateInfo DC = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	DC.queueCreateInfoCount = 1; DC.pQueueCreateInfos = &QC;
	if( V->vkCreateDevice( V->Phys, &DC, NULL, &V->Dev ) != VK_SUCCESS ) return 0;
#define LOAD_D(n) V->n = (PFN_##n)V->vkGetDeviceProcAddr( V->Dev, #n );
	VK_DEVICE_FUNCS(LOAD_D)
#undef LOAD_D
	V->vkGetDeviceQueue( V->Dev, V->Family, 0, &V->Queue );
	return 1;
}

static void DestroyTargets( FVk* V )
{
	if( !V->Fb ) return;
	V->vkDeviceWaitIdle( V->Dev );
	V->vkDestroyFramebuffer( V->Dev, V->Fb, NULL ); V->Fb = VK_NULL_HANDLE;
	V->vkDestroyImageView( V->Dev, V->ColorView, NULL ); V->vkDestroyImageView( V->Dev, V->DepthView, NULL );
	V->vkDestroyImage( V->Dev, V->ColorImg, NULL ); V->vkDestroyImage( V->Dev, V->DepthImg, NULL );
	V->vkFreeMemory( V->Dev, V->ColorMem, NULL ); V->vkFreeMemory( V->Dev, V->DepthMem, NULL );
	V->vkDestroyBuffer( V->Dev, V->ReadBuf, NULL ); V->vkFreeMemory( V->Dev, V->ReadMem, NULL );
}

static void MakeImage( FVk* V, VkFormat Fmt, INT W, INT H, VkImageUsageFlags Usage, VkImage& Img, VkDeviceMemory& Mem )
{
	VkImageCreateInfo IC = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	IC.imageType = VK_IMAGE_TYPE_2D; IC.format = Fmt; IC.extent = { (uint32_t)W, (uint32_t)H, 1 };
	IC.mipLevels = 1; IC.arrayLayers = 1; IC.samples = VK_SAMPLE_COUNT_1_BIT; IC.tiling = VK_IMAGE_TILING_OPTIMAL;
	IC.usage = Usage; IC.sharingMode = VK_SHARING_MODE_EXCLUSIVE; IC.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKCHECK( V->vkCreateImage( V->Dev, &IC, NULL, &Img ) );
	VkMemoryRequirements MR; V->vkGetImageMemoryRequirements( V->Dev, Img, &MR );
	UINT Type = 0;
	if( !FindMemType( V, MR.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Type ) && !FindMemType( V, MR.memoryTypeBits, 0, Type ) )
		appErrorf( "NVulkanDrv: no memory type for a target" );
	VkMemoryAllocateInfo AI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	AI.allocationSize = MR.size; AI.memoryTypeIndex = Type;
	VKCHECK( V->vkAllocateMemory( V->Dev, &AI, NULL, &Mem ) );
	VKCHECK( V->vkBindImageMemory( V->Dev, Img, Mem, 0 ) );
}

static VkImageView MakeView( FVk* V, VkImage Img, VkFormat Fmt, VkImageAspectFlags Aspect, UINT Levels )
{
	VkImageViewCreateInfo VC = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VC.image = Img; VC.viewType = VK_IMAGE_VIEW_TYPE_2D; VC.format = Fmt; VC.subresourceRange = { Aspect, 0, Levels, 0, 1 };
	VkImageView View; VKCHECK( V->vkCreateImageView( V->Dev, &VC, NULL, &View ) );
	return View;
}

static void CreateTargets( FVk* V, INT W, INT H )
{
	DestroyTargets( V );
	V->W = W; V->H = H;
	MakeImage( V, VK_FORMAT_B8G8R8A8_UNORM, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, V->ColorImg, V->ColorMem );
	MakeImage( V, VK_FORMAT_D32_SFLOAT, W, H, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, V->DepthImg, V->DepthMem );
	V->ColorView = MakeView( V, V->ColorImg, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1 );
	V->DepthView = MakeView( V, V->DepthImg, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1 );
	VkImageView Att[2] = { V->ColorView, V->DepthView };
	VkFramebufferCreateInfo FC = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	FC.renderPass = V->Pass; FC.attachmentCount = 2; FC.pAttachments = Att; FC.width = W; FC.height = H; FC.layers = 1;
	VKCHECK( V->vkCreateFramebuffer( V->Dev, &FC, NULL, &V->Fb ) );
	CreateBuf( V, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, V->ReadBuf, V->ReadMem, &V->ReadPtr );

	// Put both attachments into their working layouts once.
	VkCommandBuffer Cmd = V->UploadCmd;
	VkCommandBufferBeginInfo BI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	V->vkResetCommandBuffer( Cmd, 0 ); V->vkBeginCommandBuffer( Cmd, &BI );
	ImageBarrier( V, Cmd, V->ColorImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );
	ImageBarrier( V, Cmd, V->DepthImg, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT );
	V->vkEndCommandBuffer( Cmd );
	SubmitWait( V, Cmd );
}

static void CreateStaticObjects( FVk* V )
{
	VkCommandPoolCreateInfo PC = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	PC.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; PC.queueFamilyIndex = V->Family;
	VKCHECK( V->vkCreateCommandPool( V->Dev, &PC, NULL, &V->Pool ) );
	VkCommandBufferAllocateInfo CA = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	CA.commandPool = V->Pool; CA.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; CA.commandBufferCount = 1;
	VKCHECK( V->vkAllocateCommandBuffers( V->Dev, &CA, &V->FrameCmd ) );
	VKCHECK( V->vkAllocateCommandBuffers( V->Dev, &CA, &V->UploadCmd ) );
	VkFenceCreateInfo FI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VKCHECK( V->vkCreateFence( V->Dev, &FI, NULL, &V->Fence ) );

	// Render pass: colour + depth, both kept (LOAD/STORE) so a frame can be split and the screen need not be cleared.
	VkAttachmentDescription At[2] = {};
	At[0].format = VK_FORMAT_B8G8R8A8_UNORM; At[0].samples = VK_SAMPLE_COUNT_1_BIT;
	At[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; At[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	At[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; At[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	At[0].initialLayout = At[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	At[1] = At[0]; At[1].format = VK_FORMAT_D32_SFLOAT;
	At[1].initialLayout = At[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkAttachmentReference CR = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, DR = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription SP = {}; SP.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	SP.colorAttachmentCount = 1; SP.pColorAttachments = &CR; SP.pDepthStencilAttachment = &DR;
	VkSubpassDependency Dep = {};
	Dep.srcSubpass = VK_SUBPASS_EXTERNAL; Dep.dstSubpass = 0;
	Dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	Dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	Dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	Dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	VkRenderPassCreateInfo RP = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	RP.attachmentCount = 2; RP.pAttachments = At; RP.subpassCount = 1; RP.pSubpasses = &SP; RP.dependencyCount = 1; RP.pDependencies = &Dep;
	VKCHECK( V->vkCreateRenderPass( V->Dev, &RP, NULL, &V->Pass ) );

	// Shaders and layouts.
	VkShaderModuleCreateInfo SM = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	SM.codeSize = sizeof(ut_vert_spv); SM.pCode = ut_vert_spv; VKCHECK( V->vkCreateShaderModule( V->Dev, &SM, NULL, &V->VS ) );
	SM.codeSize = sizeof(ut_frag_spv); SM.pCode = ut_frag_spv; VKCHECK( V->vkCreateShaderModule( V->Dev, &SM, NULL, &V->FS ) );
	VkDescriptorSetLayoutBinding LB[4] = {};
	for( INT i = 0; i < 4; ++i ) { LB[i].binding = i; LB[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; LB[i].descriptorCount = 1; LB[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
	VkDescriptorSetLayoutCreateInfo DL = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	DL.bindingCount = 4; DL.pBindings = LB;
	VKCHECK( V->vkCreateDescriptorSetLayout( V->Dev, &DL, NULL, &V->SetLayout ) );
	VkPushConstantRange PCR = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 68 };
	VkPipelineLayoutCreateInfo PL = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	PL.setLayoutCount = 1; PL.pSetLayouts = &V->SetLayout; PL.pushConstantRangeCount = 1; PL.pPushConstantRanges = &PCR;
	VKCHECK( V->vkCreatePipelineLayout( V->Dev, &PL, NULL, &V->PipeLayout ) );

	VkDescriptorPoolSize PS = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DESC_SETS * 4 };
	VkDescriptorPoolCreateInfo DP = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	DP.maxSets = DESC_SETS; DP.poolSizeCount = 1; DP.pPoolSizes = &PS;
	VKCHECK( V->vkCreateDescriptorPool( V->Dev, &DP, NULL, &V->DescPool ) );

	// Samplers: bit0 nearest, bit1 clamp.
	for( INT i = 0; i < 4; ++i )
	{
		VkSamplerCreateInfo SC = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		SC.magFilter = SC.minFilter = ( i & 1 ) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		SC.mipmapMode = ( i & 1 ) ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
		SC.addressModeU = SC.addressModeV = SC.addressModeW = ( i & 2 ) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
		SC.maxLod = 16.f;
		VKCHECK( V->vkCreateSampler( V->Dev, &SC, NULL, &V->Samplers[i] ) );
	}

	// Rings and staging.
	const VkMemoryPropertyFlags Host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	CreateBuf( V, VTX_RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, Host, V->VtxBuf, V->VtxMem, &V->VtxPtr );
	CreateBuf( V, IDX_RING_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, Host, V->IdxBuf, V->IdxMem, &V->IdxPtr );
	V->StageSize = 8u << 20;
	CreateBuf( V, V->StageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Host, V->StageBuf, V->StageMem, &V->StagePtr );

	// 1x1 white dummy texture for unused units.
	MakeImage( V, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, V->DummyImg, V->DummyMem );
	V->DummyView = MakeView( V, V->DummyImg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1 );
	*(DWORD*)V->StagePtr = 0xffffffff;
	VkCommandBufferBeginInfo BI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	V->vkBeginCommandBuffer( V->UploadCmd, &BI );
	ImageBarrier( V, V->UploadCmd, V->DummyImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
	VkBufferImageCopy BC = {}; BC.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; BC.imageExtent = { 1, 1, 1 };
	V->vkCmdCopyBufferToImage( V->UploadCmd, V->StageBuf, V->DummyImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &BC );
	ImageBarrier( V, V->UploadCmd, V->DummyImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );
	V->vkEndCommandBuffer( V->UploadCmd );
	SubmitWait( V, V->UploadCmd );
}

/*------------------------------------------------------------------------------------
	Frame handling.
------------------------------------------------------------------------------------*/
static void BeginPass( FVk* V )
{
	VkRenderPassBeginInfo RB = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	RB.renderPass = V->Pass; RB.framebuffer = V->Fb; RB.renderArea = { { 0, 0 }, { (uint32_t)V->W, (uint32_t)V->H } };
	UT_STAT( ut_ps5_stat_render_passes );
	V->vkCmdBeginRenderPass( V->FrameCmd, &RB, VK_SUBPASS_CONTENTS_INLINE );
	VkRect2D Sc = { { 0, 0 }, { (uint32_t)V->W, (uint32_t)V->H } };
	V->vkCmdSetScissor( V->FrameCmd, 0, 1, &Sc );
	V->PassOpen = 1; V->BoundPipe = VK_NULL_HANDLE; V->BoundSet = VK_NULL_HANDLE;
}

static void BeginFrameCmd( FVk* V )
{
	VkCommandBufferBeginInfo BI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	V->vkResetCommandBuffer( V->FrameCmd, 0 );
	VKCHECK( V->vkBeginCommandBuffer( V->FrameCmd, &BI ) );
	V->FrameOpen = 1; V->VtxOff = V->IdxOff = 0;
	BeginPass( V );
}

/*------------------------------------------------------------------------------------
	Class.
------------------------------------------------------------------------------------*/
void UNVulkanRenderDevice::InternalClassInitializer( UClass* Class )
{
	guardSlow(UNVulkanRenderDevice::InternalClassInitializer);
	new(Class, "NoFiltering",    RF_Public)UBoolProperty( CPP_PROPERTY(NoFiltering),    "Options", CPF_Config );
	new(Class, "Overbright",     RF_Public)UBoolProperty( CPP_PROPERTY(Overbright),     "Options", CPF_Config );
	new(Class, "DetailTextures", RF_Public)UBoolProperty( CPP_PROPERTY(DetailTextures), "Options", CPF_Config );
	unguardSlow;
}

void UNVulkanRenderDevice::StaticConstructor()
{
	guard(UNVulkanRenderDevice::StaticConstructor);
	InternalClassInitializer( StaticClass() );
	unguard;
}

UNVulkanRenderDevice::UNVulkanRenderDevice()
{
	VolumetricLighting = true; ShinySurfaces = true; Coronas = true; HighDetailActors = true;
	DetailTextures = true; Overbright = true; NoFiltering = false;
	CurrentBrightness = -1.f; Vk = NULL;
}

UBOOL UNVulkanRenderDevice::Init( UViewport* InViewport, INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen )
{
	guard(UNVulkanRenderDevice::Init)
	debugf( NAME_Log, TEXT("NVulkanDrv::Init: %dx%d"), NewX, NewY );

	Vk = new FVk();   // value-initialised: zeroes the plain members, constructs the containers
	if( !CreateVulkan( Vk ) )
	{
		debugf( NAME_Warning, "NVulkanDrv: no Vulkan device" );
		return 0;
	}
	CreateStaticObjects( Vk );
	CreateTargets( Vk, NewX, NewY );

	SupportsFogMaps = true;
	SupportsDistanceFog = true;

	VtxDataSize = 18 * MAX_VERTS;
	VtxData = (FLOAT*)appMalloc( VtxDataSize * sizeof(FLOAT), "VkVtxDataBuf" );
	VtxDataEnd = VtxData + VtxDataSize; VtxDataPtr = VtxData;
	IdxDataSize = MAX_VERTS;
	IdxData = (_WORD*)appMalloc( IdxDataSize * sizeof(_WORD), "VkIdxDataBuf" );
	IdxDataEnd = IdxData + IdxDataSize; IdxDataPtr = IdxData; IdxCount = 0;

	CurrentPolyFlags = PF_Occlude; CurrentShaderFlags = 0; BoundShaderFlags = 0; ShaderBound = 0;
	BlendMode = BM_Opaque; DepthWrite = 1; DepthTest = 1; ColorWrite = 1;
	VpX = 0; VpY = 0; VpW = NewX; VpH = NewY; VpDirty = 1;
	PushDirty = 1; AppliedBrightness = 0.5f; CurrentBrightness = -1.f;
	memset( AppliedMVP, 0, sizeof(AppliedMVP) ); memset( MtxMVP, 0, sizeof(MtxMVP) );
	appMemzero( TexInfo, sizeof(TexInfo) );
	appMemzero( &CurrentSceneNode, sizeof(CurrentSceneNode) );
	CurrentSceneNode.X = -1;
	Viewport = InViewport;

	debugf( NAME_Log, TEXT("NVulkanDrv::Init complete") );
	return 1;
	unguard;
}

UBOOL UNVulkanRenderDevice::SetRes( INT NewX, INT NewY, INT NewColorBytes, UBOOL Fullscreen )
{
	guard(UNVulkanRenderDevice::SetRes);
	debugf( NAME_Log, TEXT("NVulkanDrv::SetRes: %dx%d"), NewX, NewY );
	if( Vk && ( Vk->W != NewX || Vk->H != NewY ) )
	{
		FlushTriangles();
		Vk->FrameOpen = 0; Vk->PassOpen = 0;
		CreateTargets( Vk, NewX, NewY );
		VpX = 0; VpY = 0; VpW = NewX; VpH = NewY; VpDirty = 1;
		CurrentSceneNode.X = -1;
	}
	return 1;
	unguard;
}

void UNVulkanRenderDevice::Exit()
{
	guard(UNVulkanRenderDevice::Exit);
	debugf( NAME_Log, "Shutting down the Vulkan renderer" );
	if( Vk )
	{
		Flush( 0 );
		Vk->vkDeviceWaitIdle( Vk->Dev );
		// Objects are left to the driver on exit; the process is ending.
	}
	unguard;
}

void UNVulkanRenderDevice::Flush( UBOOL AllowPrecache )
{
	guard(UNVulkanRenderDevice::Flush);
	if( Textures.size() && Vk )
	{
		debugf( NAME_Log, "Flushing %d textures", (INT)Textures.size() );
		FlushTriangles();
		for( INT i = 0; i < MaxTexUnits; ++i ) TexInfo[i].CurrentCacheID = 0;
		// Anything recorded so far may reference these textures: finish it first.
		if( Vk->FrameOpen )
		{
			Vk->vkCmdEndRenderPass( Vk->FrameCmd ); Vk->PassOpen = 0;
			VKCHECK( Vk->vkEndCommandBuffer( Vk->FrameCmd ) );
			SubmitWait( Vk, Vk->FrameCmd );
			Vk->FrameOpen = 0;
			BeginFrameCmd( Vk );
		}
		Vk->vkDeviceWaitIdle( Vk->Dev );
		for( size_t i = 0; i < Textures.size(); ++i )
		{
			Vk->vkDestroyImageView( Vk->Dev, Textures[i].View, NULL );
			Vk->vkDestroyImage( Vk->Dev, Textures[i].Image, NULL );
		}
		Textures.clear();
		BindMap.Empty();
		for( size_t i = 0; i < Vk->Blocks.size(); ++i ) Vk->vkFreeMemory( Vk->Dev, Vk->Blocks[i].Mem, NULL );
		Vk->Blocks.clear();
		Vk->DescCache.clear();
		Vk->vkResetDescriptorPool( Vk->Dev, Vk->DescPool, 0 );
		Vk->BoundSet = VK_NULL_HANDLE;
	}
	unguard;
}

UBOOL UNVulkanRenderDevice::Exec( const TCHAR* Cmd, FOutputDevice& Ar )
{
	guard(UNVulkanRenderDevice::Exec);
	if( URenderDevice::Exec( Cmd, Ar ) ) return 1;
	return 0;
	unguard;
}

void UNVulkanRenderDevice::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize )
{
	guard(UNVulkanRenderDevice::Lock);

	if( Viewport && ( Viewport->SizeX != Vk->W || Viewport->SizeY != Vk->H ) && Viewport->SizeX > 0 && Viewport->SizeY > 0 )
		SetRes( Viewport->SizeX, Viewport->SizeY, 4, 0 );

	if( !Vk->FrameOpen )
		BeginFrameCmd( Vk );

	FLOAT TargetBrightness = CurrentBrightness;
	if( Viewport && Viewport->GetOuterUClient() )
		TargetBrightness = Clamp<FLOAT>( Viewport->GetOuterUClient()->Brightness, 0.0f, 1.0f );
	else if( CurrentBrightness < 0.f )
		TargetBrightness = 0.5f;
	if( CurrentBrightness != TargetBrightness )
	{
		FlushTriangles();
		CurrentBrightness = TargetBrightness;
	}

	SetBlend( PF_Occlude );

	// Clear depth (always) and colour (when asked).
	VkClearAttachment CA[2]; INT N = 0;
	CA[N].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; CA[N].clearValue.depthStencil = { 1.f, 0 }; ++N;
	if( RenderLockFlags & LOCKR_ClearScreen )
	{
		CA[N].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; CA[N].colorAttachment = 0;
		CA[N].clearValue.color.float32[0] = ScreenClear.X; CA[N].clearValue.color.float32[1] = ScreenClear.Y;
		CA[N].clearValue.color.float32[2] = ScreenClear.Z; CA[N].clearValue.color.float32[3] = ScreenClear.W; ++N;
	}
	VkClearRect CR = { { { 0, 0 }, { (uint32_t)Vk->W, (uint32_t)Vk->H } }, 0, 1 };
	Vk->vkCmdClearAttachments( Vk->FrameCmd, N, CA, 1, &CR );

	if( FlashScale != FPlane(0.5f, 0.5f, 0.5f, 0.0f) || FlashFog != FPlane(0.0f, 0.0f, 0.0f, 0.0f) )
		ColorMod = FPlane( FlashFog.X, FlashFog.Y, FlashFog.Z, 1.f - Min( FlashScale.X * 2.f, 1.f ) );
	else
		ColorMod = FPlane( 0.f, 0.f, 0.f, 0.f );

	unguard;
}

void UNVulkanRenderDevice::Unlock( UBOOL Blit )
{
	guard(UNVulkanRenderDevice::Unlock);

	static INT UnlockCount = 0;
	++UnlockCount;

	FlushTriangles();
	if( !Vk->FrameOpen ) return;

	// Finish the frame: copy the colour image out and wait.
	Vk->vkCmdEndRenderPass( Vk->FrameCmd ); Vk->PassOpen = 0;
	ImageBarrier( Vk, Vk->FrameCmd, Vk->ColorImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
	VkBufferImageCopy BC = {}; BC.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; BC.imageExtent = { (uint32_t)Vk->W, (uint32_t)Vk->H, 1 };
	Vk->vkCmdCopyImageToBuffer( Vk->FrameCmd, Vk->ColorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Vk->ReadBuf, 1, &BC );
	ImageBarrier( Vk, Vk->FrameCmd, Vk->ColorImg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );
	VKCHECK( Vk->vkEndCommandBuffer( Vk->FrameCmd ) );
	SubmitWait( Vk, Vk->FrameCmd );
	Vk->FrameOpen = 0;

	// Hand the pixels to the viewport (BGRA, same as SDL's ARGB8888 texture).
	if( Viewport && Viewport->ScreenPointer && Viewport->ColorBytes == 4 )
	{
		const INT RowBytes = Vk->W * 4;
		const INT Pitch = Viewport->Stride * 4;
		for( INT y = 0; y < Vk->H && y < Viewport->SizeY; ++y )
			memcpy( Viewport->ScreenPointer + y * Pitch, Vk->ReadPtr + (size_t)y * RowBytes, RowBytes );
	}

	static const char* DumpFrame = getenv( "UT99_DUMP_FRAME" );
	static const char* DumpPath = getenv( "UT99_DUMP_PATH" );
	if( DumpFrame && DumpPath && UnlockCount == atoi( DumpFrame ) )
	{
		if( FILE* F = fopen( DumpPath, "wb" ) )
		{
			fprintf( F, "P6\n%d %d\n255\n", Vk->W, Vk->H );
			for( INT y = 0; y < Vk->H; ++y )
				for( INT x = 0; x < Vk->W; ++x )
				{
					const BYTE* P = Vk->ReadPtr + ( (size_t)y * Vk->W + x ) * 4;
					BYTE RGB[3] = { P[2], P[1], P[0] };
					fwrite( RGB, 1, 3, F );
				}
			fclose( F );
			debugf( NAME_Log, TEXT("NVulkanDrv: dumped frame %d"), UnlockCount );
		}
	}

	unguard;
}

/*------------------------------------------------------------------------------------
	Drawing (same semantics as NOpenGLESDrv).
------------------------------------------------------------------------------------*/
void UNVulkanRenderDevice::EnsureSpace( INT NumPts )
{
	if( IdxDataPtr + 3 * NumPts + 3 > IdxDataEnd || VtxDataPtr + 18 * NumPts > VtxDataEnd || (INT)IdxCount + NumPts >= 65530 )
		FlushTriangles();
}

void UNVulkanRenderDevice::DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet )
{
	guard(UNVulkanRenderDevice::DrawComplexSurface);

	check(Surface.Texture);

	SetSceneNode( Frame );
	SetBlend( Surface.PolyFlags );
	SetTexture( 0, *Surface.Texture, ( Surface.PolyFlags & PF_Masked ), 0.f );
	if( Surface.LightMap ) { SetTexture( 1, *Surface.LightMap, 0, -0.5f ); CurrentShaderFlags |= SF_Lightmap; }
	if( Surface.FogMap )   { SetTexture( 2, *Surface.FogMap, 0, -0.5f ); CurrentShaderFlags |= SF_Fogmap; }
	if( Surface.DetailTexture && DetailTextures ) { SetTexture( 3, *Surface.DetailTexture, 0, 0.f ); CurrentShaderFlags |= SF_Detail; }
	SetShader( CurrentShaderFlags );

	FLOAT UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
	FLOAT VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;
	for( FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next )
	{
		EnsureSpace( Poly->NumPts );
		BeginPoly();
		for( INT i = 0; i < Poly->NumPts; i++ )
		{
			FLOAT U = Facet.MapCoords.XAxis | Poly->Pts[i]->Point;
			FLOAT V = Facet.MapCoords.YAxis | Poly->Pts[i]->Point;
			AttribFloat3( &Poly->Pts[i]->Point.X );
			AttribFloat2( (U-UDot-TexInfo[0].UPan)*TexInfo[0].UMult, (V-VDot-TexInfo[0].VPan)*TexInfo[0].VMult );
			if( Surface.LightMap )
				AttribFloat2( (U-UDot-TexInfo[1].UPan)*TexInfo[1].UMult, (V-VDot-TexInfo[1].VPan)*TexInfo[1].VMult );
			if( Surface.FogMap )
				AttribFloat2( (U-UDot-TexInfo[2].UPan)*TexInfo[2].UMult, (V-VDot-TexInfo[2].VPan)*TexInfo[2].VMult );
			if( Surface.DetailTexture && DetailTextures )
				AttribFloat2( (U-UDot-TexInfo[3].UPan)*TexInfo[3].UMult, (V-VDot-TexInfo[3].VPan)*TexInfo[3].VMult );
			PolyVertex();
		}
		EndPoly();
	}

	CurrentShaderFlags &= ~( SF_Lightmap|SF_Fogmap|SF_Detail );
	ResetTexture( 1 ); ResetTexture( 2 ); ResetTexture( 3 );

	unguard;
}

void UNVulkanRenderDevice::DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer )
{
	guard(UNVulkanRenderDevice::DrawGouraudPolygon);

	const UBOOL IsFog = ( ( PolyFlags & ( PF_RenderFog|PF_Translucent|PF_Modulated ) ) == PF_RenderFog );
	const UBOOL IsModulated = ( PolyFlags & PF_Modulated );
	if( !IsModulated ) CurrentShaderFlags |= SF_VtxColor;
	if( IsFog ) CurrentShaderFlags |= SF_VtxFog;

	SetSceneNode( Frame );
	SetBlend( PolyFlags );
	SetTexture( 0, Texture, ( PolyFlags & PF_Masked ), 0 );
	SetShader( CurrentShaderFlags );

	EnsureSpace( NumPts );
	BeginPoly();
	for( INT i=0; i<NumPts; i++ )
	{
		FTransTexture* P = Pts[i];
		AttribFloat3( &P->Point.X );
		AttribFloat2( P->U*TexInfo[0].UMult, P->V*TexInfo[0].VMult );
		if( !IsModulated ) AttribFloat4( P->Light.X, P->Light.Y, P->Light.Z, 1.f );
		if( IsFog ) AttribFloat4( &P->Fog.X );
		PolyVertex();
	}
	EndPoly();

	CurrentShaderFlags &= ~( SF_VtxColor|SF_VtxFog|SF_ModulatedDecalMask );

	unguard;
}

void UNVulkanRenderDevice::DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags )
{
	guard(UNVulkanRenderDevice::DrawTile);

	FPlane VtxColor;
	if( !( PolyFlags & PF_Modulated ) ) { VtxColor.X = Light.X; VtxColor.Y = Light.Y; VtxColor.Z = Light.Z; VtxColor.W = 1.f; }
	else { VtxColor.X = VtxColor.Y = VtxColor.Z = VtxColor.W = 1.f; }

	CurrentShaderFlags |= SF_VtxColor;

	SetSceneNode( Frame );
	SetBlend( PolyFlags );
	SetTexture( 0, Texture, ( PolyFlags & PF_Masked ), 0.f );
	SetShader( CurrentShaderFlags );

	EnsureSpace( 4 );
	BeginPoly();
		AttribFloat3( RFX2 * Z * (X - Frame->FX2), RFY2 * Z * (Y - Frame->FY2), Z );
		AttribFloat2( U * TexInfo[0].UMult, V * TexInfo[0].VMult );
		AttribFloat4( &VtxColor.X ); PolyVertex();
		AttribFloat3( RFX2 * Z * (X + XL - Frame->FX2), RFY2 * Z * (Y - Frame->FY2), Z );
		AttribFloat2( (U + UL) * TexInfo[0].UMult, V * TexInfo[0].VMult );
		AttribFloat4( &VtxColor.X ); PolyVertex();
		AttribFloat3( RFX2 * Z * (X + XL - Frame->FX2), RFY2 * Z * (Y + YL - Frame->FY2), Z );
		AttribFloat2( (U + UL) * TexInfo[0].UMult, (V + VL) *TexInfo[0].VMult );
		AttribFloat4( &VtxColor.X ); PolyVertex();
		AttribFloat3( RFX2 * Z * (X - Frame->FX2), RFY2 * Z * (Y + YL - Frame->FY2), Z );
		AttribFloat2( U * TexInfo[0].UMult, (V + VL) * TexInfo[0].VMult );
		AttribFloat4( &VtxColor.X ); PolyVertex();
	EndPoly();

	CurrentShaderFlags &= ~SF_VtxColor;

	unguard;
}

void UNVulkanRenderDevice::Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 ) {}
void UNVulkanRenderDevice::Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, FLOAT Z ) {}
void UNVulkanRenderDevice::PushHit( const BYTE* Data, INT Count ) {}
void UNVulkanRenderDevice::PopHit( INT Count, UBOOL bForce ) {}
void UNVulkanRenderDevice::GetStats( TCHAR* Result ) { if( Result ) *Result = '\0'; }

void UNVulkanRenderDevice::EndFlash()
{
	guard(UNVulkanRenderDevice::EndFlash);

	if( ColorMod == FPlane( 0.f, 0.f, 0.f, 0.f ) )
		return;

	FlushTriangles();
	CurrentShaderFlags = SF_VtxColor;
	ResetTexture( 0 ); ResetTexture( 1 ); ResetTexture( 2 ); ResetTexture( 3 );
	SetBlend( PF_Highlighted );
	SetShader( CurrentShaderFlags );

	const FLOAT Z = 1.f;
	const FLOAT FX2 = RProjZ;
	const FLOAT FY2 = RProjZ * Aspect;

	FlushTriangles();
	DepthTest = 0;

	EnsureSpace( 4 );
	BeginPoly();
		AttribFloat3( FX2 * -Z, FY2 * -Z, Z ); AttribFloat4( &ColorMod.X ); PolyVertex();
		AttribFloat3( FX2 * +Z, FY2 * -Z, Z ); AttribFloat4( &ColorMod.X ); PolyVertex();
		AttribFloat3( FX2 * +Z, FY2 * +Z, Z ); AttribFloat4( &ColorMod.X ); PolyVertex();
		AttribFloat3( FX2 * -Z, FY2 * +Z, Z ); AttribFloat4( &ColorMod.X ); PolyVertex();
	EndPoly();

	FlushTriangles();
	DepthTest = 1;

	CurrentShaderFlags &= ~SF_VtxColor;

	unguard;
}

void UNVulkanRenderDevice::ReadPixels( FColor* Pixels )
{
	guard(UNVulkanRenderDevice::ReadPixels);
	// The last finished frame is still in the readback buffer (BGRA, top-down).
	if( Vk && Vk->ReadPtr )
		memcpy( Pixels, Vk->ReadPtr, (size_t)Vk->W * Vk->H * 4 );
	unguard;
}

void UNVulkanRenderDevice::ClearZ( FSceneNode* Frame )
{
	guard(UNVulkanRenderDevice::ClearZ);
	FlushTriangles();
	SetBlend( PF_Occlude );
	if( Vk->FrameOpen )
	{
		VkClearAttachment CA = {}; CA.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; CA.clearValue.depthStencil = { 1.f, 0 };
		VkClearRect CR = { { { 0, 0 }, { (uint32_t)Vk->W, (uint32_t)Vk->H } }, 0, 1 };
		Vk->vkCmdClearAttachments( Vk->FrameCmd, 1, &CA, 1, &CR );
	}
	unguard;
}

/*------------------------------------------------------------------------------------
	State.
------------------------------------------------------------------------------------*/
void UNVulkanRenderDevice::SetShader( DWORD ShaderFlags )
{
	guard(UNVulkanRenderDevice::SetShader);

	if( !ShaderBound || BoundShaderFlags != ShaderFlags )
	{
		FlushTriangles();
		BoundShaderFlags = ShaderFlags;
		ShaderBound = 1;
	}
	// Uniform changes (matrix / brightness) take effect after what is queued has been drawn.
	if( memcmp( AppliedMVP, MtxMVP, sizeof(MtxMVP) ) != 0 || AppliedBrightness != CurrentBrightness )
	{
		FlushTriangles();
		memcpy( AppliedMVP, MtxMVP, sizeof(MtxMVP) );
		AppliedBrightness = CurrentBrightness;
		PushDirty = 1;
	}

	unguard;
}

static void MatMul( const FLOAT* A, const FLOAT* B, FLOAT* Out )   // column major: Out = A * B
{
	for( INT c = 0; c < 4; ++c )
		for( INT r = 0; r < 4; ++r )
		{
			FLOAT S = 0;
			for( INT k = 0; k < 4; ++k ) S += A[k*4 + r] * B[c*4 + k];
			Out[c*4 + r] = S;
		}
}

void UNVulkanRenderDevice::SetSceneNode( FSceneNode* Frame )
{
	guard(UNVulkanRenderDevice::SetSceneNode);

	check(Viewport);

	if( !Frame )
	{
		CurrentSceneNode.X = -1; CurrentSceneNode.FX = -1.f; CurrentSceneNode.SizeX = -1;
		return;
	}

	if( Frame->X != CurrentSceneNode.X || Frame->Y != CurrentSceneNode.Y ||
			Frame->XB != CurrentSceneNode.XB || Frame->YB != CurrentSceneNode.YB ||
			Viewport->SizeX != CurrentSceneNode.SizeX || Viewport->SizeY != CurrentSceneNode.SizeY )
	{
		FlushTriangles();
		// GL's bottom-left window rectangle (XB, SizeY-Y-YB, X, Y) is, from the top, a rectangle at y = YB.
		VpX = Frame->XB; VpY = Frame->YB; VpW = Frame->X; VpH = Frame->Y;
		VpDirty = 1;
		CurrentSceneNode.X = Frame->X; CurrentSceneNode.Y = Frame->Y;
		CurrentSceneNode.XB = Frame->XB; CurrentSceneNode.YB = Frame->YB;
		CurrentSceneNode.SizeX = Viewport->SizeX; CurrentSceneNode.SizeY = Viewport->SizeY;
	}

	FLOAT EffectiveFovAngle = Viewport->Actor->FovAngle;
	FLOAT EffectiveRProjZ = appTan( EffectiveFovAngle * PI / 360.0 );
	if( Frame->FX > 0.0f && Frame->Proj.Z > 0.0f )
	{
		EffectiveRProjZ = (0.5f * Frame->FX) / Frame->Proj.Z;
		EffectiveFovAngle = (FLOAT)( appAtan( EffectiveRProjZ ) * 360.0 / PI );
	}

	if( Frame->FX != CurrentSceneNode.FX || Frame->FY != CurrentSceneNode.FY || EffectiveFovAngle != CurrentSceneNode.FovAngle )
	{
		RProjZ = EffectiveRProjZ;
		Aspect = Frame->FY / Frame->FX;
		RFX2 = 2.0f * RProjZ / Frame->FX;
		RFY2 = 2.0f * RProjZ * Aspect / Frame->FY;

		// Frustum with Vulkan's 0..1 depth: l=-r, b=-t, near 1, far 32768. Column major.
		const FLOAT n = 1.f, f = 32768.f, r = RProjZ, t = Aspect * RProjZ;
		FLOAT P[16] = { 0 };
		P[0] = n / r;  P[5] = n / t;
		P[10] = -f / ( f - n ); P[11] = -1.f;
		P[14] = -f * n / ( f - n );
		// UT's world -> view axes (as NOpenGLESDrv's MtxModelView).
		const FLOAT MV[16] = { 1,0,0,0,  0,-1,0,0,  0,0,-1,0,  0,0,0,1 };
		MatMul( P, MV, MtxMVP );
		CurrentSceneNode.FX = Frame->FX; CurrentSceneNode.FY = Frame->FY; CurrentSceneNode.FovAngle = EffectiveFovAngle;
	}

	unguard;
}

void UNVulkanRenderDevice::SetBlend( DWORD PolyFlags )
{
	guard(UNVulkanRenderDevice::SetBlend);

	if( !(PolyFlags & (PF_Translucent|PF_Modulated)) )
		PolyFlags |= PF_Occlude;
	else if( PolyFlags & PF_Translucent )
		PolyFlags &= ~PF_Masked;

	DWORD Xor = CurrentPolyFlags ^ PolyFlags;
	if( Xor & (PF_Translucent|PF_Modulated|PF_Invisible|PF_Occlude|PF_Masked|PF_Highlighted) )
	{
		FlushTriangles();
		if( Xor & (PF_Translucent|PF_Modulated|PF_Highlighted) )
		{
			if( PolyFlags & PF_Translucent )      BlendMode = BM_Translucent;
			else if( PolyFlags & PF_Modulated )   BlendMode = BM_Modulated;
			else if( PolyFlags & PF_Highlighted ) BlendMode = BM_Highlighted;
			else                                  BlendMode = BM_Opaque;
		}
		if( Xor & PF_Invisible )
			ColorWrite = !( PolyFlags & PF_Invisible );
		if( Xor & PF_Occlude )
			DepthWrite = ( PolyFlags & PF_Occlude ) != 0;
		if( Xor & PF_Masked )
		{
			if( PolyFlags & PF_Masked ) CurrentShaderFlags |= SF_AlphaTest;
			else                        CurrentShaderFlags &= ~SF_AlphaTest;
		}
	}
	CurrentPolyFlags = PolyFlags;

	unguard;
}

void UNVulkanRenderDevice::ResetTexture( INT TMU )
{
	guard(UNVulkanRenderDevice::ResetTexture);
	CurrentShaderFlags &= ~(1 << TMU);
	if( TexInfo[TMU].CurrentCacheID != 0 )
	{
		FlushTriangles();
		TexInfo[TMU].CurrentCacheID = 0;
	}
	unguard;
}

void UNVulkanRenderDevice::SetTexture( INT TMU, FTextureInfo& Info, DWORD PolyFlags, FLOAT PanBias )
{
	guard(UNVulkanRenderDevice::SetTexture);

	CurrentShaderFlags |= 1 << TMU;

	FTexInfo& Tex = TexInfo[TMU];
	Tex.UPan = Info.Pan.X + PanBias*Info.UScale;
	Tex.VPan = Info.Pan.Y + PanBias*Info.VScale;
	Tex.UMult = 1.f / (Info.UScale * static_cast<FLOAT>(Info.USize));
	Tex.VMult = 1.f / (Info.VScale * static_cast<FLOAT>(Info.VSize));

	QWORD NewCacheID = Info.CacheID;
	if( ( PolyFlags & PF_Masked ) && Info.Palette )
		NewCacheID |= MASKED_TEXTURE_TAG;
	UBOOL RealtimeChanged = Info.bRealtimeChanged;
	if( NewCacheID == Tex.CurrentCacheID && !RealtimeChanged )
		return;

	FlushTriangles();

	Tex.CurrentCacheID = NewCacheID;
	INT* Found = BindMap.Find( NewCacheID );
	if( !Found || RealtimeChanged )
	{
		Info.bRealtimeChanged = 0;
		INT Index = UploadTexture( Info, ( PolyFlags & PF_Masked ), Found ? *Found : -1 );
		if( Index >= 0 )
			BindMap.Set( NewCacheID, Index );
		else
			Tex.CurrentCacheID = 0;
	}

	unguard;
}

// Uploads all mips (new image, or a refresh of an existing one) and returns its index in Textures.
INT UNVulkanRenderDevice::UploadTexture( FTextureInfo& Info, UBOOL Masked, INT ExistingIndex )
{
	guard(UNVulkanRenderDevice::UploadTexture);

	if( !Info.Mips[0] ) { debugf( NAME_Warning, "Encountered texture with invalid mips!" ); return ExistingIndex; }

	// Count usable mips and the staging space they need.
	INT Levels = 0; size_t Total = 0;
	for( INT i = 0; i < Info.NumMips && i < 12; ++i )
	{
		FMipmapBase* Mip = Info.Mips[i];
		if( !Mip || !Mip->DataPtr ) break;
		++Levels; Total += (size_t)Mip->USize * Mip->VSize * 4;
	}
	if( Levels == 0 ) return ExistingIndex;

	if( Total > Vk->StageSize )
	{
		Vk->vkDeviceWaitIdle( Vk->Dev );
		Vk->vkDestroyBuffer( Vk->Dev, Vk->StageBuf, NULL ); Vk->vkFreeMemory( Vk->Dev, Vk->StageMem, NULL );
		Vk->StageSize = Total * 2;
		CreateBuf( Vk, Vk->StageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, Vk->StageBuf, Vk->StageMem, &Vk->StagePtr );
	}

	const UBOOL Pal = Info.Palette != NULL;
	const VkFormat Fmt = Pal ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;

	INT Index = ExistingIndex;
	if( Index >= 0 && ( Textures[Index].Levels != Levels || Textures[Index].Width != Info.Mips[0]->USize || Textures[Index].Height != Info.Mips[0]->VSize || Textures[Index].Format != Fmt ) )
		Index = -1;   // shape changed: make a new image (the old one is released on the next Flush)

	std::vector<VkBufferImageCopy> Copies;
	size_t Off = 0;
	for( INT m = 0; m < Levels; ++m )
	{
		FMipmapBase* Mip = Info.Mips[m];
		const DWORD Count = Mip->USize * Mip->VSize;
		DWORD* Dst = (DWORD*)( Vk->StagePtr + Off );
		const BYTE* Src = (const BYTE*)Mip->DataPtr;
		if( Pal )
		{
			const DWORD* PalData = (const DWORD*)Info.Palette;
			if( Masked ) for( DWORD i = 0; i < Count; ++i, ++Src ) *Dst++ = *Src ? ( PalData[*Src] | ALPHA_MASK ) : 0;
			else         for( DWORD i = 0; i < Count; ++i ) *Dst++ = ( PalData[*Src++] | ALPHA_MASK );
		}
		else
			memcpy( Dst, Src, (size_t)Count * 4 );
		VkBufferImageCopy BC = {};
		BC.bufferOffset = Off; BC.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, 0, 1 };
		BC.imageExtent = { (uint32_t)Mip->USize, (uint32_t)Mip->VSize, 1 };
		Copies.push_back( BC );
		Off += (size_t)Count * 4;
	}

	UBOOL IsNew = Index < 0;
	if( IsNew )
	{
		FVkTexture T = {};
		T.Levels = Levels; T.Width = Info.Mips[0]->USize; T.Height = Info.Mips[0]->VSize; T.Format = Fmt;
		VkImageCreateInfo IC = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		IC.imageType = VK_IMAGE_TYPE_2D; IC.format = Fmt; IC.extent = { (uint32_t)T.Width, (uint32_t)T.Height, 1 };
		IC.mipLevels = Levels; IC.arrayLayers = 1; IC.samples = VK_SAMPLE_COUNT_1_BIT; IC.tiling = VK_IMAGE_TILING_OPTIMAL;
		IC.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; IC.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		IC.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKCHECK( Vk->vkCreateImage( Vk->Dev, &IC, NULL, &T.Image ) );
		BindImageMemory( Vk, T.Image );
		T.View = MakeView( Vk, T.Image, Fmt, VK_IMAGE_ASPECT_COLOR_BIT, Levels );
		const UBOOL Nearest = ( ( CurrentPolyFlags & PF_NoSmooth ) || ( NoFiltering && Pal ) );
		T.Sampler = ( Nearest ? 1 : 0 ) | ( Pal ? 0 : 2 );
		Textures.push_back( T );
		Index = (INT)Textures.size() - 1;
	}

	FVkTexture& T = Textures[Index];
	VkCommandBufferBeginInfo BI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Vk->vkResetCommandBuffer( Vk->UploadCmd, 0 );
	Vk->vkBeginCommandBuffer( Vk->UploadCmd, &BI );
	ImageBarrier( Vk, Vk->UploadCmd, T.Image, VK_IMAGE_ASPECT_COLOR_BIT,
		IsNew ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		IsNew ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		IsNew ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, Levels );
	Vk->vkCmdCopyBufferToImage( Vk->UploadCmd, Vk->StageBuf, T.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)Copies.size(), Copies.data() );
	ImageBarrier( Vk, Vk->UploadCmd, T.Image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, Levels );
	VKCHECK( Vk->vkEndCommandBuffer( Vk->UploadCmd ) );
	SubmitWait( Vk, Vk->UploadCmd );

	return Index;
	unguard;
}

/*------------------------------------------------------------------------------------
	Batch submission.
------------------------------------------------------------------------------------*/
static VkPipeline GetPipeline( FVk* V, DWORD Flags, INT Blend, UBOOL DepthWrite, UBOOL DepthTest, UBOOL ColorWrite, UBOOL Overbright )
{
	const DWORD Key = Flags | ( Blend << 11 ) | ( DepthWrite << 13 ) | ( DepthTest << 14 ) | ( ColorWrite << 15 ) | ( Overbright << 16 );
	std::map<DWORD, VkPipeline>::iterator It = V->Pipelines.find( Key );
	if( It != V->Pipelines.end() ) return It->second;

	// Vertex layout follows the flags, as the CPU side packs it.
	VkVertexInputAttributeDescription Att[AT_Count]; INT NA = 0; DWORD Offset = 0;
	static const VkFormat Fmts[5] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
	for( INT i = 0; i < AT_Count; ++i )
		if( i == 0 || ( Flags & AttribFlags[i] ) )
		{
			Att[NA].location = i; Att[NA].binding = 0; Att[NA].format = Fmts[AttribSizes[i]]; Att[NA].offset = Offset * 4;
			Offset += AttribSizes[i]; ++NA;
		}
	// The shader declares every input: point the ones this layout does not carry at the start of the vertex.
	for( INT i = 0; i < AT_Count; ++i )
		if( !( i == 0 || ( Flags & AttribFlags[i] ) ) )
		{
			Att[NA].location = i; Att[NA].binding = 0; Att[NA].offset = 0;
			Att[NA].format = ( AttribSizes[i] == 2 ) ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT; ++NA;
		}
	VkVertexInputBindingDescription Bind = { 0, Offset * 4, VK_VERTEX_INPUT_RATE_VERTEX };
	VkPipelineVertexInputStateCreateInfo VI = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VI.vertexBindingDescriptionCount = 1; VI.pVertexBindingDescriptions = &Bind; VI.vertexAttributeDescriptionCount = NA; VI.pVertexAttributeDescriptions = Att;
	VkPipelineInputAssemblyStateCreateInfo IA = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO }; IA.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo VS = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO }; VS.viewportCount = 1; VS.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo RS = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	RS.polygonMode = VK_POLYGON_MODE_FILL; RS.cullMode = VK_CULL_MODE_NONE; RS.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; RS.lineWidth = 1.f;
	VkPipelineMultisampleStateCreateInfo MS = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO }; MS.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo DS = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	DS.depthTestEnable = DepthTest; DS.depthWriteEnable = DepthWrite; DS.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendAttachmentState CB = {};
	CB.colorWriteMask = ColorWrite ? 0xF : 0;
	switch( Blend )
	{
		case BM_Translucent: CB.blendEnable = VK_TRUE; CB.srcColorBlendFactor = CB.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; CB.dstColorBlendFactor = CB.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; break;
		case BM_Modulated:   CB.blendEnable = VK_TRUE; CB.srcColorBlendFactor = CB.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_COLOR; CB.dstColorBlendFactor = CB.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_COLOR; break;
		case BM_Highlighted: CB.blendEnable = VK_TRUE; CB.srcColorBlendFactor = CB.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; CB.dstColorBlendFactor = CB.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
		default: break;
	}
	CB.colorBlendOp = CB.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo CBS = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO }; CBS.attachmentCount = 1; CBS.pAttachments = &CB;
	VkDynamicState Dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo DY = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO }; DY.dynamicStateCount = 2; DY.pDynamicStates = Dyn;

	struct { uint32_t Flags; float Overbright; } SpecData = { Flags, Overbright ? LIGHTMAP_OVERBRIGHT_VALUE : 1.f };
	VkSpecializationMapEntry Map[2] = { { 0, 0, 4 }, { 1, 4, 4 } };
	VkSpecializationInfo SI = { 2, Map, sizeof(SpecData), &SpecData };
	VkPipelineShaderStageCreateInfo Stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	Stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; Stages[0].module = V->VS; Stages[0].pName = "main"; Stages[0].pSpecializationInfo = &SI;
	Stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; Stages[1].module = V->FS; Stages[1].pName = "main"; Stages[1].pSpecializationInfo = &SI;

	VkGraphicsPipelineCreateInfo GP = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	GP.stageCount = 2; GP.pStages = Stages; GP.pVertexInputState = &VI; GP.pInputAssemblyState = &IA; GP.pViewportState = &VS;
	GP.pRasterizationState = &RS; GP.pMultisampleState = &MS; GP.pDepthStencilState = &DS; GP.pColorBlendState = &CBS; GP.pDynamicState = &DY;
	GP.layout = V->PipeLayout; GP.renderPass = V->Pass; GP.subpass = 0;
	VkPipeline Pipe = VK_NULL_HANDLE;
	UT_STAT( ut_ps5_stat_graphics_pipelines );
	VKCHECK( V->vkCreateGraphicsPipelines( V->Dev, VK_NULL_HANDLE, 1, &GP, NULL, &Pipe ) );
	V->Pipelines[Key] = Pipe;
	return Pipe;
}

void UNVulkanRenderDevice::FlushTriangles()
{
	if( IdxCount && Vk && Vk->FrameOpen )
	{
		const size_t VtxBytes = (BYTE*)VtxDataPtr - (BYTE*)VtxData;
		const size_t IdxBytes = (BYTE*)IdxDataPtr - (BYTE*)IdxData;
		const DWORD Flags = BoundShaderFlags;

		// Rings full: finish what is recorded, wait, and start over (the pass is reopened with LOAD).
		if( Vk->VtxOff + VtxBytes > VTX_RING_BYTES || Vk->IdxOff + IdxBytes > IDX_RING_BYTES )
		{
			Vk->vkCmdEndRenderPass( Vk->FrameCmd );
			VKCHECK( Vk->vkEndCommandBuffer( Vk->FrameCmd ) );
			SubmitWait( Vk, Vk->FrameCmd );
			VkCommandBufferBeginInfo BI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			Vk->vkResetCommandBuffer( Vk->FrameCmd, 0 ); Vk->vkBeginCommandBuffer( Vk->FrameCmd, &BI );
			Vk->VtxOff = Vk->IdxOff = 0;
			BeginPass( Vk ); VpDirty = 1; PushDirty = 1;
		}

		// Textures per unit (only the ones the shader samples).
		std::array<uint32_t,4> Key = { 0, 0, 0, 0 };
		for( INT i = 0; i < MaxTexUnits; ++i )
			if( ( Flags & ( 1u << i ) ) && TexInfo[i].CurrentCacheID )
			{
				INT* Idx = BindMap.Find( TexInfo[i].CurrentCacheID );
				if( Idx ) Key[i] = (uint32_t)( *Idx + 1 );
			}
		std::map<std::array<uint32_t,4>, VkDescriptorSet>::iterator DIt = Vk->DescCache.find( Key );
		VkDescriptorSet Set;
		if( DIt != Vk->DescCache.end() ) Set = DIt->second;
		else
		{
			if( Vk->DescUsed >= DESC_SETS )
			{
				// Out of sets: submit, wait, recycle the pool.
				Vk->vkCmdEndRenderPass( Vk->FrameCmd );
				VKCHECK( Vk->vkEndCommandBuffer( Vk->FrameCmd ) );
				SubmitWait( Vk, Vk->FrameCmd );
				VkCommandBufferBeginInfo BI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				Vk->vkResetCommandBuffer( Vk->FrameCmd, 0 ); Vk->vkBeginCommandBuffer( Vk->FrameCmd, &BI );
				Vk->VtxOff = Vk->IdxOff = 0;
				Vk->DescCache.clear(); Vk->vkResetDescriptorPool( Vk->Dev, Vk->DescPool, 0 ); Vk->DescUsed = 0;
				BeginPass( Vk ); VpDirty = 1; PushDirty = 1;
			}
			VkDescriptorSetAllocateInfo AI = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			AI.descriptorPool = Vk->DescPool; AI.descriptorSetCount = 1; AI.pSetLayouts = &Vk->SetLayout;
			VKCHECK( Vk->vkAllocateDescriptorSets( Vk->Dev, &AI, &Set ) );
			++Vk->DescUsed;
			VkDescriptorImageInfo II[4]; VkWriteDescriptorSet W[4];
			for( INT i = 0; i < 4; ++i )
			{
				if( Key[i] ) { FVkTexture& T = Textures[Key[i] - 1]; II[i] = { Vk->Samplers[T.Sampler], T.View, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; }
				else         { II[i] = { Vk->Samplers[0], Vk->DummyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; }
				W[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				W[i].dstSet = Set; W[i].dstBinding = i; W[i].descriptorCount = 1; W[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; W[i].pImageInfo = &II[i];
			}
			Vk->vkUpdateDescriptorSets( Vk->Dev, 4, W, 0, NULL );
			Vk->DescCache[Key] = Set;
		}

		if( Vk->BoundPipe == VK_NULL_HANDLE ) { VpDirty = 1; PushDirty = 1; }   // new command buffer / render pass
		VkPipeline Pipe = GetPipeline( Vk, Flags, BlendMode, DepthWrite, DepthTest, ColorWrite, Overbright );
		VkCommandBuffer Cmd = Vk->FrameCmd;
		if( Pipe != Vk->BoundPipe ) { Vk->vkCmdBindPipeline( Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipe ); Vk->BoundPipe = Pipe; }
		if( Set != Vk->BoundSet ) { Vk->vkCmdBindDescriptorSets( Cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Vk->PipeLayout, 0, 1, &Set, 0, NULL ); Vk->BoundSet = Set; }
		if( PushDirty )
		{
			BYTE PC[68]; memcpy( PC, AppliedMVP, 64 ); memcpy( PC + 64, &AppliedBrightness, 4 );
			Vk->vkCmdPushConstants( Cmd, Vk->PipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 68, PC );
			PushDirty = 0;
		}
		if( VpDirty )
		{
			// Negative height: Vulkan's y-down NDC becomes GL's y-up.
			VkViewport VP = { (float)VpX, (float)( VpY + VpH ), (float)VpW, -(float)VpH, 0.f, 1.f };
			Vk->vkCmdSetViewport( Cmd, 0, 1, &VP );
			VpDirty = 0;
		}

		memcpy( Vk->VtxPtr + Vk->VtxOff, VtxData, VtxBytes );
		memcpy( Vk->IdxPtr + Vk->IdxOff, IdxData, IdxBytes );
		VkDeviceSize VOff = Vk->VtxOff;
		Vk->vkCmdBindVertexBuffers( Cmd, 0, 1, &Vk->VtxBuf, &VOff );
		Vk->vkCmdBindIndexBuffer( Cmd, Vk->IdxBuf, Vk->IdxOff, VK_INDEX_TYPE_UINT16 );
		UT_STAT( ut_ps5_stat_draw_calls );
		Vk->vkCmdDrawIndexed( Cmd, (uint32_t)( IdxBytes / 2 ), 1, 0, 0, 0 );
		Vk->VtxOff += ( VtxBytes + 15 ) & ~15ull;
		Vk->IdxOff += ( IdxBytes + 15 ) & ~15ull;
	}
	IdxCount = 0;
	VtxDataPtr = VtxData;
	IdxDataPtr = IdxData;
}
