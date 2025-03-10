#pragma once

// VulkanRenderManager takes the role that a GL driver does of sequencing and optimizing render passes.
// Only draws and binds are handled here, resource creation and allocations are handled as normal -
// that's the nice thing with Vulkan.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <queue>

#include "Common/Thread/Promise.h"
#include "Common/System/Display.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/Data/Convert/SmallDataConvert.h"
#include "Common/Math/math_util.h"
#include "Common/GPU/DataFormat.h"
#include "Common/GPU/Vulkan/VulkanQueueRunner.h"

// Forward declaration
VK_DEFINE_HANDLE(VmaAllocation);

// Simple independent framebuffer image. Gets its own allocation, we don't have that many framebuffers so it's fine
// to let them have individual non-pooled allocations. Until it's not fine. We'll see.
struct VKRImage {
	// These four are "immutable".
	VkImage image;
	VkImageView imageView;
	VkImageView depthSampleView;
	VmaAllocation alloc;
	VkFormat format;

	// This one is used by QueueRunner's Perform functions to keep track. CANNOT be used anywhere else due to sync issues.
	VkImageLayout layout;

	// For debugging.
	std::string tag;
};
void CreateImage(VulkanContext *vulkan, VkCommandBuffer cmd, VKRImage &img, int width, int height, VkFormat format, VkImageLayout initialLayout, bool color, const char *tag);

class VKRFramebuffer {
public:
	VKRFramebuffer(VulkanContext *vk, VkCommandBuffer initCmd, VKRRenderPass *compatibleRenderPass, int _width, int _height, const char *tag);
	~VKRFramebuffer();

	VkFramebuffer Get(VKRRenderPass *compatibleRenderPass, RenderPassType rpType);

	int width = 0;
	int height = 0;
	VKRImage color{};
	VKRImage depth{};

	const char *Tag() const {
		return tag_.c_str();
	}

	// TODO: Hide.
	VulkanContext *vulkan_;
private:
	VkFramebuffer framebuf[RP_TYPE_COUNT]{};

	std::string tag_;
};

enum {
	MAX_TIMESTAMP_QUERIES = 128,
};

struct BoundingRect {
	int x1;
	int y1;
	int x2;
	int y2;

	BoundingRect() {
		Reset();
	}

	void Reset() {
		x1 = 65535;
		y1 = 65535;
		x2 = -65535;
		y2 = -65535;
	}

	bool Empty() const {
		return x2 < 0;
	}

	void SetRect(int x, int y, int width, int height) {
		x1 = x;
		y1 = y;
		x2 = width;
		y2 = height;
	}

	void Apply(const VkRect2D &rect) {
		if (rect.offset.x < x1) x1 = rect.offset.x;
		if (rect.offset.y < y1) y1 = rect.offset.y;
		int rect_x2 = rect.offset.x + rect.extent.width;
		int rect_y2 = rect.offset.y + rect.extent.height;
		if (rect_x2 > x2) x2 = rect_x2;
		if (rect_y2 > y2) y2 = rect_y2;
	}

	VkRect2D ToVkRect2D() const {
		VkRect2D rect;
		rect.offset.x = x1;
		rect.offset.y = y1;
		rect.extent.width = x2 - x1;
		rect.extent.height = y2 - y1;
		return rect;
	}
};

// All the data needed to create a graphics pipeline.
struct VKRGraphicsPipelineDesc {
	VkPipelineCache pipelineCache = VK_NULL_HANDLE;
	VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState blend0{};
	VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkDynamicState dynamicStates[6]{};
	VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };

	// Replaced the ShaderStageInfo with promises here so we can wait for compiles to finish.
	Promise<VkShaderModule> *vertexShader = nullptr;
	Promise<VkShaderModule> *fragmentShader = nullptr;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkVertexInputAttributeDescription attrs[8]{};
	VkVertexInputBindingDescription ibd{};
	VkPipelineVertexInputStateCreateInfo vis{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo views{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

	// Does not include the render pass type, it's passed in separately since the
	// desc is persistent.
	RPKey rpKey{};
};

// All the data needed to create a compute pipeline.
struct VKRComputePipelineDesc {
	VkPipelineCache pipelineCache;
	VkComputePipelineCreateInfo pipe{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
};

// Wrapped pipeline. Doesn't own desc.
struct VKRGraphicsPipeline {
	bool Create(VulkanContext *vulkan, VkRenderPass compatibleRenderPass, RenderPassType rpType);

	// This deletes the whole VKRGraphicsPipeline, you must remove your last pointer to it when doing this.
	void QueueForDeletion(VulkanContext *vulkan);

	u32 GetVariantsBitmask() const;

	VKRGraphicsPipelineDesc *desc = nullptr;  // not owned!
	Promise<VkPipeline> *pipeline[RP_TYPE_COUNT]{};
	std::string tag;
};

struct VKRComputePipeline {
	VKRComputePipeline() {
		pipeline = VK_NULL_HANDLE;
	}
	VKRComputePipelineDesc *desc = nullptr;
	Promise<VkPipeline> *pipeline;

	bool Create(VulkanContext *vulkan);
	bool Pending() const {
		return pipeline == VK_NULL_HANDLE && desc != nullptr;
	}
};

struct CompileQueueEntry {
	CompileQueueEntry(VKRGraphicsPipeline *p, VkRenderPass _compatibleRenderPass, RenderPassType _renderPassType)
		: type(Type::GRAPHICS), graphics(p), compatibleRenderPass(_compatibleRenderPass), renderPassType(_renderPassType) {}
	CompileQueueEntry(VKRComputePipeline *p) : type(Type::COMPUTE), compute(p), renderPassType(RP_TYPE_COLOR_DEPTH) {}
	enum class Type {
		GRAPHICS,
		COMPUTE,
	};
	Type type;
	VkRenderPass compatibleRenderPass;
	RenderPassType renderPassType;
	VKRGraphicsPipeline *graphics = nullptr;
	VKRComputePipeline *compute = nullptr;
};

class VulkanRenderManager {
public:
	VulkanRenderManager(VulkanContext *vulkan);
	~VulkanRenderManager();

	void ThreadFunc();
	void CompileThreadFunc();
	void DrainCompileQueue();

	// Makes sure that the GPU has caught up enough that we can start writing buffers of this frame again.
	void BeginFrame(bool enableProfiling, bool enableLogProfiler);
	// Can run on a different thread!
	void Finish();
	void Run(int frame);

	// Zaps queued up commands. Use if you know there's a risk you've queued up stuff that has already been deleted. Can happen during in-game shutdown.
	void Wipe();

	// This starts a new step containing a render pass.
	//
	// After a "CopyFramebuffer" or the other functions that start "steps", you need to call this beforce
	// making any new render state changes or draw calls.
	//
	// The following dynamic state needs to be reset by the caller after calling this (and will thus not safely carry over from
	// the previous one):
	//   * Viewport/Scissor
	//   * Stencil parameters
	//   * Blend color
	//
	// (Most other state is directly decided by your choice of pipeline and descriptor set, so not handled here).
	//
	// It can be useful to use GetCurrentStepId() to figure out when you need to send all this state again, if you're
	// not keeping track of your calls to this function on your own.
	void BindFramebufferAsRenderTarget(VKRFramebuffer *fb, VKRRenderPassLoadAction color, VKRRenderPassLoadAction depth, VKRRenderPassLoadAction stencil, uint32_t clearColor, float clearDepth, uint8_t clearStencil, const char *tag);

	// Returns an ImageView corresponding to a framebuffer. Is called BindFramebufferAsTexture to maintain a similar interface
	// as the other backends, even though there's no actual binding happening here.
	VkImageView BindFramebufferAsTexture(VKRFramebuffer *fb, int binding, VkImageAspectFlags aspectBits, int attachment);

	void BindCurrentFramebufferAsInputAttachment0(VkImageAspectFlags aspectBits);

	bool CopyFramebufferToMemorySync(VKRFramebuffer *src, VkImageAspectFlags aspectBits, int x, int y, int w, int h, Draw::DataFormat destFormat, uint8_t *pixels, int pixelStride, const char *tag);
	void CopyImageToMemorySync(VkImage image, int mipLevel, int x, int y, int w, int h, Draw::DataFormat destFormat, uint8_t *pixels, int pixelStride, const char *tag);

	void CopyFramebuffer(VKRFramebuffer *src, VkRect2D srcRect, VKRFramebuffer *dst, VkOffset2D dstPos, VkImageAspectFlags aspectMask, const char *tag);
	void BlitFramebuffer(VKRFramebuffer *src, VkRect2D srcRect, VKRFramebuffer *dst, VkRect2D dstRect, VkImageAspectFlags aspectMask, VkFilter filter, const char *tag);

	// Deferred creation, like in GL. Unlike GL though, the purpose is to allow background creation and avoiding
	// stalling the emulation thread as much as possible.
	// We delay creating pipelines until the end of the current render pass, so we can create the right type immediately.
	// Unless a variantBitmask is passed in, in which case we can just go ahead.
	// WARNING: desc must stick around during the lifetime of the pipeline! It's not enough to build it on the stack and drop it.
	VKRGraphicsPipeline *CreateGraphicsPipeline(VKRGraphicsPipelineDesc *desc, uint32_t variantBitmask, const char *tag);
	VKRComputePipeline *CreateComputePipeline(VKRComputePipelineDesc *desc);

	void NudgeCompilerThread() {
		compileMutex_.lock();
		compileCond_.notify_one();
		compileMutex_.unlock();
	}

	void BindPipeline(VKRGraphicsPipeline *pipeline, PipelineFlags flags, VkPipelineLayout pipelineLayout) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		_dbg_assert_(pipeline != nullptr);
		VkRenderData data{ VKRRenderCommand::BIND_GRAPHICS_PIPELINE };
		pipelinesToCheck_.push_back(pipeline);
		data.graphics_pipeline.pipeline = pipeline;
		data.graphics_pipeline.pipelineLayout = pipelineLayout;
		curPipelineFlags_ |= flags;
		curRenderStep_->commands.push_back(data);
	}

	void BindPipeline(VKRComputePipeline *pipeline, PipelineFlags flags, VkPipelineLayout pipelineLayout) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		_dbg_assert_(pipeline != nullptr);
		VkRenderData data{ VKRRenderCommand::BIND_COMPUTE_PIPELINE };
		data.compute_pipeline.pipeline = pipeline;
		data.compute_pipeline.pipelineLayout = pipelineLayout;
		curPipelineFlags_ |= flags;
		curRenderStep_->commands.push_back(data);
	}

	void SetViewport(const VkViewport &vp) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		_dbg_assert_((int)vp.width >= 0);
		_dbg_assert_((int)vp.height >= 0);
		VkRenderData data{ VKRRenderCommand::VIEWPORT };
		data.viewport.vp.x = vp.x;
		data.viewport.vp.y = vp.y;
		data.viewport.vp.width = vp.width;
		data.viewport.vp.height = vp.height;
		// We can't allow values outside this range unless we use VK_EXT_depth_range_unrestricted.
		// Sometimes state mapping produces 65536/65535 which is slightly outside.
		// TODO: This should be fixed at the source.
		data.viewport.vp.minDepth = clamp_value(vp.minDepth, 0.0f, 1.0f);
		data.viewport.vp.maxDepth = clamp_value(vp.maxDepth, 0.0f, 1.0f);
		curRenderStep_->commands.push_back(data);
		curStepHasViewport_ = true;
	}

	// It's OK to set scissor outside the valid range - the function will automatically clip.
	void SetScissor(int x, int y, int width, int height) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);

		if (x < 0) {
			width += x;  // since x is negative, this shrinks width.
			x = 0;
		}
		if (y < 0) {
			height += y;
			y = 0;
		}

		if (x + width > curWidth_) {
			width = curWidth_ - x;
		}
		if (y + height > curHeight_) {
			height = curHeight_ - y;
		}

		// Check validity.
		if (width < 0 || height < 0 || x >= curWidth_ || y >= curHeight_) {
			// TODO: If any of the dimensions are now zero or negative, we should flip a flag and not do draws, probably.
			// Instead, if we detect an invalid scissor rectangle, we just put a 1x1 rectangle in the upper left corner.
			x = 0;
			y = 0;
			width = 1;
			height = 1;
		}

		VkRect2D rc;
		rc.offset.x = x;
		rc.offset.y = y;
		rc.extent.width = width;
		rc.extent.height = height;

		curRenderArea_.Apply(rc);

		VkRenderData data{ VKRRenderCommand::SCISSOR };
		data.scissor.scissor = rc;
		curRenderStep_->commands.push_back(data);
		curStepHasScissor_ = true;
	}

	void SetStencilParams(uint8_t writeMask, uint8_t compareMask, uint8_t refValue) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		VkRenderData data{ VKRRenderCommand::STENCIL };
		data.stencil.stencilWriteMask = writeMask;
		data.stencil.stencilCompareMask = compareMask;
		data.stencil.stencilRef = refValue;
		curRenderStep_->commands.push_back(data);
	}

	void SetBlendFactor(uint32_t color) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		VkRenderData data{ VKRRenderCommand::BLEND };
		data.blendColor.color = color;
		curRenderStep_->commands.push_back(data);
	}

	void PushConstants(VkPipelineLayout pipelineLayout, VkShaderStageFlags stages, int offset, int size, void *constants) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		_dbg_assert_(size + offset < 40);
		VkRenderData data{ VKRRenderCommand::PUSH_CONSTANTS };
		data.push.stages = stages;
		data.push.offset = offset;
		data.push.size = size;
		memcpy(data.push.data, constants, size);
		curRenderStep_->commands.push_back(data);
	}

	void Clear(uint32_t clearColor, float clearZ, int clearStencil, int clearMask);

	// Cheaply set that we don't care about the contents of a surface at the start of the current render pass.
	// This set the corresponding load-op of the current render pass to DONT_CARE.
	// Useful when we don't know at bind-time whether we will overwrite the surface or not.
	void SetLoadDontCare(VkImageAspectFlags aspects) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		if (aspects & VK_IMAGE_ASPECT_COLOR_BIT)
			curRenderStep_->render.colorLoad = VKRRenderPassLoadAction::DONT_CARE;
		if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
			curRenderStep_->render.depthLoad = VKRRenderPassLoadAction::DONT_CARE;
		if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
			curRenderStep_->render.stencilLoad = VKRRenderPassLoadAction::DONT_CARE;
	}

	// Cheaply set that we don't care about the contents of a surface at the end of the current render pass.
	// This set the corresponding store-op of the current render pass to DONT_CARE.
	void SetStoreDontCare(VkImageAspectFlags aspects) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
		if (aspects & VK_IMAGE_ASPECT_COLOR_BIT)
			curRenderStep_->render.colorStore = VKRRenderPassStoreAction::DONT_CARE;
		if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
			curRenderStep_->render.depthStore = VKRRenderPassStoreAction::DONT_CARE;
		if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
			curRenderStep_->render.stencilStore = VKRRenderPassStoreAction::DONT_CARE;
	}

	void Draw(VkDescriptorSet descSet, int numUboOffsets, const uint32_t *uboOffsets, VkBuffer vbuffer, int voffset, int count, int offset = 0) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER && curStepHasViewport_ && curStepHasScissor_);
		VkRenderData data{ VKRRenderCommand::DRAW };
		data.draw.count = count;
		data.draw.offset = offset;
		data.draw.ds = descSet;
		data.draw.vbuffer = vbuffer;
		data.draw.voffset = voffset;
		data.draw.numUboOffsets = numUboOffsets;
		_dbg_assert_(numUboOffsets <= ARRAY_SIZE(data.draw.uboOffsets));
		for (int i = 0; i < numUboOffsets; i++)
			data.draw.uboOffsets[i] = uboOffsets[i];
		curRenderStep_->commands.push_back(data);
		curRenderStep_->render.numDraws++;
	}

	void DrawIndexed(VkDescriptorSet descSet, int numUboOffsets, const uint32_t *uboOffsets, VkBuffer vbuffer, int voffset, VkBuffer ibuffer, int ioffset, int count, int numInstances, VkIndexType indexType) {
		_dbg_assert_(curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER && curStepHasViewport_ && curStepHasScissor_);
		VkRenderData data{ VKRRenderCommand::DRAW_INDEXED };
		data.drawIndexed.count = count;
		data.drawIndexed.instances = numInstances;
		data.drawIndexed.ds = descSet;
		data.drawIndexed.vbuffer = vbuffer;
		data.drawIndexed.voffset = voffset;
		data.drawIndexed.ibuffer = ibuffer;
		data.drawIndexed.ioffset = ioffset;
		data.drawIndexed.numUboOffsets = numUboOffsets;
		_dbg_assert_(numUboOffsets <= ARRAY_SIZE(data.drawIndexed.uboOffsets));
		for (int i = 0; i < numUboOffsets; i++)
			data.drawIndexed.uboOffsets[i] = uboOffsets[i];
		data.drawIndexed.indexType = indexType;
		curRenderStep_->commands.push_back(data);
		curRenderStep_->render.numDraws++;
	}

	VkCommandBuffer GetInitCmd();

	// Gets a frame-unique ID of the current step being recorded. Can be used to figure out
	// when the current step has changed, which means the caller will need to re-record its state.
	int GetCurrentStepId() const {
		return renderStepOffset_ + (int)steps_.size();
	}

	bool CreateBackbuffers();
	void DestroyBackbuffers();

	bool HasBackbuffers() {
		return queueRunner_.HasBackbuffers();
	}

	void SetInflightFrames(int f) {
		newInflightFrames_ = f < 1 || f > VulkanContext::MAX_INFLIGHT_FRAMES ? VulkanContext::MAX_INFLIGHT_FRAMES : f;
	}

	VulkanContext *GetVulkanContext() {
		return vulkan_;
	}

	// Be careful with this. Only meant to be used for fetching render passes for shader cache initialization.
	VulkanQueueRunner *GetQueueRunner() {
		return &queueRunner_;
	}

	std::string GetGpuProfileString() const {
		return frameData_[vulkan_->GetCurFrame()].profile.profileSummary;
	}

	bool NeedsSwapchainRecreate() const {
		// Accepting a few of these makes shutdown simpler.
		return outOfDateFrames_ > VulkanContext::MAX_INFLIGHT_FRAMES;
	}

private:
	void EndCurRenderStep();

	void BeginSubmitFrame(int frame);
	void EndSubmitFrame(int frame);
	void Submit(int frame, bool triggerFence);
	void SubmitInitCommands(int frame);

	// Bad for performance but sometimes necessary for synchronous CPU readbacks (screenshots and whatnot).
	void FlushSync();
	void EndSyncFrame(int frame);

	void StopThread();

	// Permanent objects
	VkSemaphore acquireSemaphore_;
	VkSemaphore renderingCompleteSemaphore_;

	FrameData frameData_[VulkanContext::MAX_INFLIGHT_FRAMES];
	int newInflightFrames_ = -1;
	int inflightFramesAtStart_ = 0;

	int outOfDateFrames_ = 0;

	// Submission time state

	// Note: These are raw backbuffer-sized. Rotated.
	int curWidthRaw_ = -1;
	int curHeightRaw_ = -1;

	// Pre-rotation (as you'd expect).
	int curWidth_ = -1;
	int curHeight_ = -1;

	bool insideFrame_ = false;
	// This is the offset within this frame, in case of a mid-frame sync.
	int renderStepOffset_ = 0;
	VKRStep *curRenderStep_ = nullptr;
	bool curStepHasViewport_ = false;
	bool curStepHasScissor_ = false;
	PipelineFlags curPipelineFlags_{};
	BoundingRect curRenderArea_;

	std::vector<VKRStep *> steps_;

	// Execution time state
	bool run_ = true;
	VulkanContext *vulkan_;
	std::thread thread_;
	std::mutex mutex_;
	int threadInitFrame_ = 0;
	VulkanQueueRunner queueRunner_;

	// Shader compilation thread to compile while emulating the rest of the frame.
	// Only one right now but we could use more.
	std::thread compileThread_;
	// Sync
	std::condition_variable compileCond_;
	std::mutex compileMutex_;
	std::vector<CompileQueueEntry> compileQueue_;

	// pipelines to check and possibly create at the end of the current render pass.
	std::vector<VKRGraphicsPipeline *> pipelinesToCheck_;

	bool useThread_ = true;
};
