// Copyright (c) 2015- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cstdio>
#include <vector>
#include <string>
#include <map>

#include "Common/System/Display.h"
#include "Common/Math/lin/matrix4x4.h"
#include "Common/Data/Convert/SmallDataConvert.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"

#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanImage.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"
#include "Common/Thread/Promise.h"

#include "Core/Config.h"

// We use a simple descriptor set for all rendering: 1 sampler, 1 texture, 1 UBO binding point.
// binding 0 - uniform data
// binding 1 - sampler
// binding 2 - sampler
//
// Vertex data lives in a separate namespace (location = 0, 1, etc)

#include "Common/GPU/Vulkan/VulkanLoader.h"

using namespace PPSSPP_VK;

namespace Draw {

// This can actually be replaced with a cast as the values are in the right order.
static const VkCompareOp compToVK[] = {
	VK_COMPARE_OP_NEVER,
	VK_COMPARE_OP_LESS,
	VK_COMPARE_OP_EQUAL,
	VK_COMPARE_OP_LESS_OR_EQUAL,
	VK_COMPARE_OP_GREATER,
	VK_COMPARE_OP_NOT_EQUAL,
	VK_COMPARE_OP_GREATER_OR_EQUAL,
	VK_COMPARE_OP_ALWAYS
};

// So can this.
static const VkBlendOp blendEqToVk[] = {
	VK_BLEND_OP_ADD,
	VK_BLEND_OP_SUBTRACT,
	VK_BLEND_OP_REVERSE_SUBTRACT,
	VK_BLEND_OP_MIN,
	VK_BLEND_OP_MAX,
};

static const VkBlendFactor blendFactorToVk[] = {
	VK_BLEND_FACTOR_ZERO,
	VK_BLEND_FACTOR_ONE,
	VK_BLEND_FACTOR_SRC_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
	VK_BLEND_FACTOR_DST_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
	VK_BLEND_FACTOR_SRC_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	VK_BLEND_FACTOR_DST_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
	VK_BLEND_FACTOR_CONSTANT_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
	VK_BLEND_FACTOR_CONSTANT_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
	VK_BLEND_FACTOR_SRC1_COLOR,
	VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
	VK_BLEND_FACTOR_SRC1_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
};

static const VkLogicOp logicOpToVK[] = {
	VK_LOGIC_OP_CLEAR,
	VK_LOGIC_OP_SET,
	VK_LOGIC_OP_COPY,
	VK_LOGIC_OP_COPY_INVERTED,
	VK_LOGIC_OP_NO_OP,
	VK_LOGIC_OP_INVERT,
	VK_LOGIC_OP_AND,
	VK_LOGIC_OP_NAND,
	VK_LOGIC_OP_OR,
	VK_LOGIC_OP_NOR,
	VK_LOGIC_OP_XOR,
	VK_LOGIC_OP_EQUIVALENT,
	VK_LOGIC_OP_AND_REVERSE,
	VK_LOGIC_OP_AND_INVERTED,
	VK_LOGIC_OP_OR_REVERSE,
	VK_LOGIC_OP_OR_INVERTED,
};

static const VkPrimitiveTopology primToVK[] = {
	VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
	VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
	// Tesselation shader primitive.
	VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
	// The rest are for geometry shaders only.
	VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
	VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
	VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
};


static const VkStencilOp stencilOpToVK[8] = {
	VK_STENCIL_OP_KEEP,
	VK_STENCIL_OP_ZERO,
	VK_STENCIL_OP_REPLACE,
	VK_STENCIL_OP_INCREMENT_AND_CLAMP,
	VK_STENCIL_OP_DECREMENT_AND_CLAMP,
	VK_STENCIL_OP_INVERT,
	VK_STENCIL_OP_INCREMENT_AND_WRAP,
	VK_STENCIL_OP_DECREMENT_AND_WRAP,
};

class VKBlendState : public BlendState {
public:
	VkPipelineColorBlendStateCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	std::vector<VkPipelineColorBlendAttachmentState> attachments;
};

class VKDepthStencilState : public DepthStencilState {
public:
	VkPipelineDepthStencilStateCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
};

class VKRasterState : public RasterState {
public:
	VKRasterState(VulkanContext *vulkan, const RasterStateDesc &desc) {
		cullFace = desc.cull;
		frontFace = desc.frontFace;
	}
	Facing frontFace;
	CullMode cullFace;

	void ToVulkan(VkPipelineRasterizationStateCreateInfo *info) const {
		memset(info, 0, sizeof(*info));
		info->sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		info->frontFace = frontFace == Facing::CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
		switch (cullFace) {
		case CullMode::BACK: info->cullMode = VK_CULL_MODE_BACK_BIT; break;
		case CullMode::FRONT: info->cullMode = VK_CULL_MODE_FRONT_BIT; break;
		case CullMode::FRONT_AND_BACK: info->cullMode = VK_CULL_MODE_FRONT_AND_BACK; break;
		case CullMode::NONE: info->cullMode = VK_CULL_MODE_NONE; break;
		}
		info->polygonMode = VK_POLYGON_MODE_FILL;
		info->lineWidth = 1.0f;
	}
};

VkShaderStageFlagBits StageToVulkan(ShaderStage stage) {
	switch (stage) {
	case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
	case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
	case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
	default:
	case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
	}
}

// Not registering this as a resource holder, instead the pipeline is registered. It will
// invoke Compile again to recreate the shader then link them together.
class VKShaderModule : public ShaderModule {
public:
	VKShaderModule(ShaderStage stage, const std::string &tag) : stage_(stage), tag_(tag) {
		vkstage_ = StageToVulkan(stage);
	}
	bool Compile(VulkanContext *vulkan, ShaderLanguage language, const uint8_t *data, size_t size);
	const std::string &GetSource() const { return source_; }
	~VKShaderModule() {
		if (module_) {
			DEBUG_LOG(G3D, "Queueing %s (shmodule %p) for release", tag_.c_str(), module_);
			vulkan_->Delete().QueueDeleteShaderModule(module_);
		}
	}
	VkShaderModule Get() const { return module_; }
	ShaderStage GetStage() const override {
		return stage_;
	}

private:
	VulkanContext *vulkan_;
	VkShaderModule module_ = VK_NULL_HANDLE;
	VkShaderStageFlagBits vkstage_;
	bool ok_ = false;
	ShaderStage stage_;
	std::string source_;  // So we can recompile in case of context loss.
	std::string tag_;
};

bool VKShaderModule::Compile(VulkanContext *vulkan, ShaderLanguage language, const uint8_t *data, size_t size) {
	vulkan_ = vulkan;
	// We'll need this to free it later.
	source_ = (const char *)data;
	std::vector<uint32_t> spirv;
	std::string errorMessage;
	if (!GLSLtoSPV(vkstage_, source_.c_str(), GLSLVariant::VULKAN, spirv, &errorMessage)) {
		WARN_LOG(G3D, "Shader compile to module failed (%s): %s", tag_.c_str(), errorMessage.c_str());
		return false;
	}

	// Just for kicks, sanity check the SPIR-V. The disasm isn't perfect
	// but gives you some idea of what's going on.
#if 0
	std::string disasm;
	if (DisassembleSPIRV(spirv, &disasm)) {
		OutputDebugStringA(disasm.c_str());
	}
#endif

	if (vulkan->CreateShaderModule(spirv, &module_, vkstage_ == VK_SHADER_STAGE_VERTEX_BIT ? "thin3d_vs" : "thin3d_fs")) {
		ok_ = true;
	} else {
		WARN_LOG(G3D, "vkCreateShaderModule failed (%s)", tag_.c_str());
		ok_ = false;
	}
	return ok_;
}

class VKInputLayout : public InputLayout {
public:
	std::vector<VkVertexInputBindingDescription> bindings;
	std::vector<VkVertexInputAttributeDescription> attributes;
	VkPipelineVertexInputStateCreateInfo visc;
};

class VKPipeline : public Pipeline {
public:
	VKPipeline(VulkanContext *vulkan, size_t size, PipelineFlags _flags, const char *tag) : vulkan_(vulkan), flags(_flags), tag_(tag) {
		uboSize_ = (int)size;
		ubo_ = new uint8_t[uboSize_];
	}
	~VKPipeline() {
		DEBUG_LOG(G3D, "Queueing %s (pipeline) for release", tag_.c_str());
		if (pipeline) {
			pipeline->QueueForDeletion(vulkan_);
		}
		for (auto dep : deps) {
			dep->Release();
		}
		delete[] ubo_;
	}

	void SetDynamicUniformData(const void *data, size_t size) {
		memcpy(ubo_, data, size);
	}

	// Returns the binding offset, and the VkBuffer to bind.
	size_t PushUBO(VulkanPushBuffer *buf, VulkanContext *vulkan, VkBuffer *vkbuf) {
		return buf->PushAligned(ubo_, uboSize_, vulkan->GetPhysicalDeviceProperties().properties.limits.minUniformBufferOffsetAlignment, vkbuf);
	}

	int GetUniformLoc(const char *name);
	int GetUBOSize() const {
		return uboSize_;
	}

	VKRGraphicsPipeline *pipeline = nullptr;
	VKRGraphicsPipelineDesc vkrDesc;
	PipelineFlags flags;

	std::vector<VKShaderModule *> deps;

	int stride[4]{};
	int dynamicUniformSize = 0;

	bool usesStencil = false;

private:
	VulkanContext *vulkan_;
	uint8_t *ubo_;
	int uboSize_;
	std::string tag_;
};

class VKTexture;
class VKBuffer;
class VKSamplerState;

enum {
	MAX_BOUND_TEXTURES = MAX_TEXTURE_SLOTS,
};

struct DescriptorSetKey {
	VkImageView imageViews_[MAX_BOUND_TEXTURES];
	VKSamplerState *samplers_[MAX_BOUND_TEXTURES];
	VkBuffer buffer_;

	bool operator < (const DescriptorSetKey &other) const {
		for (int i = 0; i < MAX_BOUND_TEXTURES; ++i) {
			if (imageViews_[i] < other.imageViews_[i]) return true; else if (imageViews_[i] > other.imageViews_[i]) return false;
			if (samplers_[i] < other.samplers_[i]) return true; else if (samplers_[i] > other.samplers_[i]) return false;
		}
		if (buffer_ < other.buffer_) return true; else if (buffer_ > other.buffer_) return false;
		return false;
	}
};

class VKTexture : public Texture {
public:
	VKTexture(VulkanContext *vulkan, VkCommandBuffer cmd, VulkanPushBuffer *pushBuffer, const TextureDesc &desc)
		: vulkan_(vulkan), mipLevels_(desc.mipLevels), format_(desc.format) {}
	bool Create(VkCommandBuffer cmd, VulkanPushBuffer *pushBuffer, const TextureDesc &desc);

	~VKTexture() {
		Destroy();
	}

	VkImageView GetImageView() {
		if (vkTex_) {
			vkTex_->Touch();
			return vkTex_->GetImageView();
		} else {
			// This would be bad.
			return VK_NULL_HANDLE;
		}
	}

private:
	void Destroy() {
		if (vkTex_) {
			vkTex_->Destroy();
			delete vkTex_;
			vkTex_ = nullptr;
		}
	}

	VulkanContext *vulkan_;
	VulkanTexture *vkTex_ = nullptr;

	int mipLevels_ = 0;

	DataFormat format_ = DataFormat::UNDEFINED;
};

class VKFramebuffer;

class VKContext : public DrawContext {
public:
	VKContext(VulkanContext *vulkan);
	virtual ~VKContext();

	const DeviceCaps &GetDeviceCaps() const override {
		return caps_;
	}
	std::vector<std::string> GetDeviceList() const override {
		std::vector<std::string> list;
		for (int i = 0; i < vulkan_->GetNumPhysicalDevices(); i++) {
			list.push_back(vulkan_->GetPhysicalDeviceProperties(i).properties.deviceName);
		}
		return list;
	}
	uint32_t GetSupportedShaderLanguages() const override {
		return (uint32_t)ShaderLanguage::GLSL_VULKAN;
	}
	uint32_t GetDataFormatSupport(DataFormat fmt) const override;

	DepthStencilState *CreateDepthStencilState(const DepthStencilStateDesc &desc) override;
	BlendState *CreateBlendState(const BlendStateDesc &desc) override;
	InputLayout *CreateInputLayout(const InputLayoutDesc &desc) override;
	SamplerState *CreateSamplerState(const SamplerStateDesc &desc) override;
	RasterState *CreateRasterState(const RasterStateDesc &desc) override;
	Pipeline *CreateGraphicsPipeline(const PipelineDesc &desc, const char *tag) override;
	ShaderModule *CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t dataSize, const char *tag) override;

	Texture *CreateTexture(const TextureDesc &desc) override;
	Buffer *CreateBuffer(size_t size, uint32_t usageFlags) override;
	Framebuffer *CreateFramebuffer(const FramebufferDesc &desc) override;

	void UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) override;

	void CopyFramebufferImage(Framebuffer *src, int level, int x, int y, int z, Framebuffer *dst, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth, int channelBits, const char *tag) override;
	bool BlitFramebuffer(Framebuffer *src, int srcX1, int srcY1, int srcX2, int srcY2, Framebuffer *dst, int dstX1, int dstY1, int dstX2, int dstY2, int channelBits, FBBlitFilter filter, const char *tag) override;
	bool CopyFramebufferToMemorySync(Framebuffer *src, int channelBits, int x, int y, int w, int h, Draw::DataFormat format, void *pixels, int pixelStride, const char *tag) override;
	DataFormat PreferredFramebufferReadbackFormat(Framebuffer *src) override;

	// These functions should be self explanatory.
	void BindFramebufferAsRenderTarget(Framebuffer *fbo, const RenderPassInfo &rp, const char *tag) override;
	Framebuffer *GetCurrentRenderTarget() override {
		return (Framebuffer *)curFramebuffer_.ptr;
	}
	void BindFramebufferAsTexture(Framebuffer *fbo, int binding, FBChannel channelBit, int attachment) override;
	void BindCurrentFramebufferForColorInput() override;

	void GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) override;

	void SetScissorRect(int left, int top, int width, int height) override;
	void SetViewports(int count, Viewport *viewports) override;
	void SetBlendFactor(float color[4]) override;
	void SetStencilParams(uint8_t refValue, uint8_t writeMask, uint8_t compareMask) override;

	void BindSamplerStates(int start, int count, SamplerState **state) override;
	void BindTextures(int start, int count, Texture **textures) override;
	void BindNativeTexture(int sampler, void *nativeTexture) override;

	void BindPipeline(Pipeline *pipeline) override {
		curPipeline_ = (VKPipeline *)pipeline;
	}

	// TODO: Make VKBuffers proper buffers, and do a proper binding model. This is just silly.
	void BindVertexBuffers(int start, int count, Buffer **buffers, const int *offsets) override {
		_assert_(start + count <= ARRAY_SIZE(curVBuffers_));
		for (int i = 0; i < count; i++) {
			curVBuffers_[i + start] = (VKBuffer *)buffers[i];
			curVBufferOffsets_[i + start] = offsets ? offsets[i] : 0;
		}
	}
	void BindIndexBuffer(Buffer *indexBuffer, int offset) override {
		curIBuffer_ = (VKBuffer *)indexBuffer;
		curIBufferOffset_ = offset;
	}

	void UpdateDynamicUniformBuffer(const void *ub, size_t size) override;

	// TODO: Add more sophisticated draws.
	void Draw(int vertexCount, int offset) override;
	void DrawIndexed(int vertexCount, int offset) override;
	void DrawUP(const void *vdata, int vertexCount) override;

	void BindCurrentPipeline();
	void ApplyDynamicState();

	void Clear(int mask, uint32_t colorval, float depthVal, int stencilVal) override;

	void BeginFrame() override;
	void EndFrame() override;
	void WipeQueue() override;

	void FlushState() override {}

	std::string GetInfoString(InfoField info) const override {
		// TODO: Make these actually query the right information
		switch (info) {
		case APINAME: return "Vulkan";
		case VENDORSTRING: return vulkan_->GetPhysicalDeviceProperties().properties.deviceName;
		case VENDOR: return VulkanVendorString(vulkan_->GetPhysicalDeviceProperties().properties.vendorID);
		case DRIVER: return FormatDriverVersion(vulkan_->GetPhysicalDeviceProperties().properties);
		case SHADELANGVERSION: return "N/A";;
		case APIVERSION: 
		{
			uint32_t ver = vulkan_->GetPhysicalDeviceProperties().properties.apiVersion;
			return StringFromFormat("%d.%d.%d", ver >> 22, (ver >> 12) & 0x3ff, ver & 0xfff);
		}
		default: return "?";
		}
	}

	VkDescriptorSet GetOrCreateDescriptorSet(VkBuffer buffer);

	std::vector<std::string> GetFeatureList() const override;
	std::vector<std::string> GetExtensionList() const override;

	uint64_t GetNativeObject(NativeObject obj, void *srcObject) override;

	void HandleEvent(Event ev, int width, int height, void *param1, void *param2) override;

	int GetCurrentStepId() const override {
		return renderManager_.GetCurrentStepId();
	}

	void InvalidateCachedState() override;

	void InvalidateFramebuffer(FBInvalidationStage stage, uint32_t channels) override;

private:
	VulkanTexture *GetNullTexture();
	VulkanContext *vulkan_ = nullptr;

	VulkanRenderManager renderManager_;

	VulkanTexture *nullTexture_ = nullptr;

	AutoRef<VKPipeline> curPipeline_;
	AutoRef<VKBuffer> curVBuffers_[4];
	int curVBufferOffsets_[4]{};
	AutoRef<VKBuffer> curIBuffer_;
	int curIBufferOffset_ = 0;

	VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
	VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
	AutoRef<VKFramebuffer> curFramebuffer_;

	VkDevice device_;
	VkQueue queue_;
	int queueFamilyIndex_;

	enum {
		MAX_FRAME_COMMAND_BUFFERS = 256,
	};
	AutoRef<VKTexture> boundTextures_[MAX_BOUND_TEXTURES];
	AutoRef<VKSamplerState> boundSamplers_[MAX_BOUND_TEXTURES];
	VkImageView boundImageView_[MAX_BOUND_TEXTURES]{};

	struct FrameData {
		FrameData() : descriptorPool("VKContext", false) {
			descriptorPool.Setup([this] { descSets_.clear(); });
		}

		VulkanPushBuffer *pushBuffer = nullptr;
		// Per-frame descriptor set cache. As it's per frame and reset every frame, we don't need to
		// worry about invalidating descriptors pointing to deleted textures.
		// However! ARM is not a fan of doing it this way.
		std::map<DescriptorSetKey, VkDescriptorSet> descSets_;
		VulkanDescSetPool descriptorPool;
	};

	FrameData frame_[VulkanContext::MAX_INFLIGHT_FRAMES];

	VulkanPushBuffer *push_ = nullptr;

	DeviceCaps caps_{};

	uint8_t stencilRef_ = 0;
	uint8_t stencilWriteMask_ = 0xFF;
	uint8_t stencilCompareMask_ = 0xFF;
};

static int GetBpp(VkFormat format) {
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
		return 32;
	case VK_FORMAT_R8_UNORM:
		return 8;
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R16_UNORM:
		return 16;
	case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
	case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
	case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
	case VK_FORMAT_R5G6B5_UNORM_PACK16:
	case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
	case VK_FORMAT_B5G6R5_UNORM_PACK16:
	case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
		return 16;
	case VK_FORMAT_D24_UNORM_S8_UINT:
		return 32;
	case VK_FORMAT_D16_UNORM:
		return 16;
	default:
		return 0;
	}
}

static VkFormat DataFormatToVulkan(DataFormat format) {
	switch (format) {
	case DataFormat::D16: return VK_FORMAT_D16_UNORM;
	case DataFormat::D32F: return VK_FORMAT_D32_SFLOAT;
	case DataFormat::D32F_S8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
	case DataFormat::S8: return VK_FORMAT_S8_UINT;

	case DataFormat::R16_UNORM: return VK_FORMAT_R16_UNORM;

	case DataFormat::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
	case DataFormat::R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
	case DataFormat::R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
	case DataFormat::R8_UNORM: return VK_FORMAT_R8_UNORM;
	case DataFormat::R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
	case DataFormat::R8G8B8_UNORM: return VK_FORMAT_R8G8B8_UNORM;
	case DataFormat::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
	case DataFormat::R4G4_UNORM_PACK8: return VK_FORMAT_R4G4_UNORM_PACK8;

	// Note: A4R4G4B4_UNORM_PACK16 is not supported.
	case DataFormat::R4G4B4A4_UNORM_PACK16: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
	case DataFormat::B4G4R4A4_UNORM_PACK16: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
	case DataFormat::R5G5B5A1_UNORM_PACK16: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
	case DataFormat::B5G5R5A1_UNORM_PACK16: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
	case DataFormat::R5G6B5_UNORM_PACK16: return VK_FORMAT_R5G6B5_UNORM_PACK16;
	case DataFormat::B5G6R5_UNORM_PACK16: return VK_FORMAT_B5G6R5_UNORM_PACK16;
	case DataFormat::A1R5G5B5_UNORM_PACK16: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;

	case DataFormat::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
	case DataFormat::R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
	case DataFormat::R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
	case DataFormat::R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;

	case DataFormat::BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case DataFormat::BC2_UNORM_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
	case DataFormat::BC3_UNORM_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
	case DataFormat::BC4_UNORM_BLOCK: return VK_FORMAT_BC4_UNORM_BLOCK;
	case DataFormat::BC4_SNORM_BLOCK: return VK_FORMAT_BC4_SNORM_BLOCK;
	case DataFormat::BC5_UNORM_BLOCK: return VK_FORMAT_BC5_UNORM_BLOCK;
	case DataFormat::BC5_SNORM_BLOCK: return VK_FORMAT_BC5_SNORM_BLOCK;
	case DataFormat::BC6H_SFLOAT_BLOCK: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
	case DataFormat::BC6H_UFLOAT_BLOCK: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
	case DataFormat::BC7_UNORM_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
	case DataFormat::BC7_SRGB_BLOCK:  return VK_FORMAT_BC7_SRGB_BLOCK;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

static inline VkSamplerAddressMode AddressModeToVulkan(Draw::TextureAddressMode mode) {
	switch (mode) {
	case TextureAddressMode::CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	case TextureAddressMode::CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	case TextureAddressMode::REPEAT_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	default:
	case TextureAddressMode::REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}
}

VulkanTexture *VKContext::GetNullTexture() {
	if (!nullTexture_) {
		VkCommandBuffer cmdInit = renderManager_.GetInitCmd();
		nullTexture_ = new VulkanTexture(vulkan_);
		nullTexture_->SetTag("Null");
		int w = 8;
		int h = 8;
		nullTexture_->CreateDirect(cmdInit, w, h, 1, 1, VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		uint32_t bindOffset;
		VkBuffer bindBuf;
		uint32_t *data = (uint32_t *)push_->Push(w * h * 4, &bindOffset, &bindBuf);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				// data[y*w + x] = ((x ^ y) & 1) ? 0xFF808080 : 0xFF000000;   // gray/black checkerboard
				data[y*w + x] = 0;  // black
			}
		}
		nullTexture_->UploadMip(cmdInit, 0, w, h, 0, bindBuf, bindOffset, w);
		nullTexture_->EndCreate(cmdInit, false, VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else {
		nullTexture_->Touch();
	}
	return nullTexture_;
}

class VKSamplerState : public SamplerState {
public:
	VKSamplerState(VulkanContext *vulkan, const SamplerStateDesc &desc) : vulkan_(vulkan) {
		VkSamplerCreateInfo s = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		s.addressModeU = AddressModeToVulkan(desc.wrapU);
		s.addressModeV = AddressModeToVulkan(desc.wrapV);
		s.addressModeW = AddressModeToVulkan(desc.wrapW);
		s.anisotropyEnable = desc.maxAniso > 1.0f;
		s.magFilter = desc.magFilter == TextureFilter::LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		s.minFilter = desc.minFilter == TextureFilter::LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		s.mipmapMode = desc.mipFilter == TextureFilter::LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		s.maxLod = VK_LOD_CLAMP_NONE;
		VkResult res = vkCreateSampler(vulkan_->GetDevice(), &s, nullptr, &sampler_);
		_assert_(VK_SUCCESS == res);
	}
	~VKSamplerState() {
		vulkan_->Delete().QueueDeleteSampler(sampler_);
	}

	VkSampler GetSampler() { return sampler_; }

private:
	VulkanContext *vulkan_;
	VkSampler sampler_;
};

SamplerState *VKContext::CreateSamplerState(const SamplerStateDesc &desc) {
	return new VKSamplerState(vulkan_, desc);
}

RasterState *VKContext::CreateRasterState(const RasterStateDesc &desc) {
	return new VKRasterState(vulkan_, desc);
}

void VKContext::BindSamplerStates(int start, int count, SamplerState **state) {
	_assert_(start + count <= MAX_BOUND_TEXTURES);
	for (int i = start; i < start + count; i++) {
		boundSamplers_[i] = (VKSamplerState *)state[i - start];
	}
}

enum class TextureState {
	UNINITIALIZED,
	STAGED,
	INITIALIZED,
	PENDING_DESTRUCTION,
};

bool VKTexture::Create(VkCommandBuffer cmd, VulkanPushBuffer *push, const TextureDesc &desc) {
	// Zero-sized textures not allowed.
	_assert_(desc.width * desc.height * desc.depth > 0);  // remember to set depth to 1!
	if (desc.width * desc.height * desc.depth <= 0) {
		ERROR_LOG(G3D,  "Bad texture dimensions %dx%dx%d", desc.width, desc.height, desc.depth);
		return false;
	}
	_assert_(push);
	format_ = desc.format;
	mipLevels_ = desc.mipLevels;
	width_ = desc.width;
	height_ = desc.height;
	depth_ = desc.depth;
	vkTex_ = new VulkanTexture(vulkan_);
	if (desc.tag) {
		vkTex_->SetTag(desc.tag);
	}
	VkFormat vulkanFormat = DataFormatToVulkan(format_);
	int bpp = GetBpp(vulkanFormat);
	int bytesPerPixel = bpp / 8;
	int usageBits = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (mipLevels_ > (int)desc.initData.size()) {
		// Gonna have to generate some, which requires TRANSFER_SRC
		usageBits |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	if (!vkTex_->CreateDirect(cmd, width_, height_, 1, mipLevels_, vulkanFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, usageBits)) {
		ERROR_LOG(G3D,  "Failed to create VulkanTexture: %dx%dx%d fmt %d, %d levels", width_, height_, depth_, (int)vulkanFormat, mipLevels_);
		return false;
	}
	VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	if (desc.initData.size()) {
		int w = width_;
		int h = height_;
		int d = depth_;
		int i;
		for (i = 0; i < (int)desc.initData.size(); i++) {
			uint32_t offset;
			VkBuffer buf;
			size_t size = w * h * d * bytesPerPixel;
			if (desc.initDataCallback) {
				uint8_t *dest = (uint8_t *)push->PushAligned(size, &offset, &buf, 16);
				if (!desc.initDataCallback(dest, desc.initData[i], w, h, d, w * bytesPerPixel, h * w * bytesPerPixel)) {
					memcpy(dest, desc.initData[i], size);
				}
			} else {
				offset = push->PushAligned((const void *)desc.initData[i], size, 16, &buf);
			}
			vkTex_->UploadMip(cmd, i, w, h, 0, buf, offset, w);
			w = (w + 1) / 2;
			h = (h + 1) / 2;
			d = (d + 1) / 2;
		}
		// Generate the rest of the mips automatically.
		if (i < mipLevels_) {
			vkTex_->GenerateMips(cmd, i, false);
			layout = VK_IMAGE_LAYOUT_GENERAL;
		}
	}
	vkTex_->EndCreate(cmd, false, VK_PIPELINE_STAGE_TRANSFER_BIT, layout);
	return true;
}

VKContext::VKContext(VulkanContext *vulkan)
	: vulkan_(vulkan), renderManager_(vulkan) {
	shaderLanguageDesc_.Init(GLSL_VULKAN);

	VkFormat depthStencilFormat = vulkan->GetDeviceInfo().preferredDepthStencilFormat;

	caps_.anisoSupported = vulkan->GetDeviceFeatures().enabled.samplerAnisotropy != 0;
	caps_.geometryShaderSupported = vulkan->GetDeviceFeatures().enabled.geometryShader != 0;
	caps_.tesselationShaderSupported = vulkan->GetDeviceFeatures().enabled.tessellationShader != 0;
	caps_.multiViewport = vulkan->GetDeviceFeatures().enabled.multiViewport != 0;
	caps_.dualSourceBlend = vulkan->GetDeviceFeatures().enabled.dualSrcBlend != 0;
	caps_.depthClampSupported = vulkan->GetDeviceFeatures().enabled.depthClamp != 0;
	caps_.clipDistanceSupported = vulkan->GetDeviceFeatures().enabled.shaderClipDistance != 0;
	caps_.cullDistanceSupported = vulkan->GetDeviceFeatures().enabled.shaderCullDistance != 0;
	caps_.framebufferBlitSupported = true;
	caps_.framebufferCopySupported = true;
	caps_.framebufferDepthBlitSupported = vulkan->GetDeviceInfo().canBlitToPreferredDepthStencilFormat;
	caps_.framebufferStencilBlitSupported = caps_.framebufferDepthBlitSupported;
	caps_.framebufferDepthCopySupported = true;   // Will pretty much always be the case.
	caps_.framebufferSeparateDepthCopySupported = true;   // Will pretty much always be the case.
	caps_.preferredDepthBufferFormat = DataFormat::D24_S8;  // TODO: Ask vulkan.
	caps_.texture3DSupported = true;
	caps_.textureDepthSupported = true;
	caps_.fragmentShaderInt32Supported = true;
	caps_.textureNPOTFullySupported = true;
	caps_.fragmentShaderDepthWriteSupported = true;
	caps_.logicOpSupported = vulkan->GetDeviceFeatures().enabled.logicOp != 0;

	auto deviceProps = vulkan->GetPhysicalDeviceProperties(vulkan_->GetCurrentPhysicalDeviceIndex()).properties;
	switch (deviceProps.vendorID) {
	case VULKAN_VENDOR_AMD: caps_.vendor = GPUVendor::VENDOR_AMD; break;
	case VULKAN_VENDOR_ARM: caps_.vendor = GPUVendor::VENDOR_ARM; break;
	case VULKAN_VENDOR_IMGTEC: caps_.vendor = GPUVendor::VENDOR_IMGTEC; break;
	case VULKAN_VENDOR_NVIDIA: caps_.vendor = GPUVendor::VENDOR_NVIDIA; break;
	case VULKAN_VENDOR_QUALCOMM: caps_.vendor = GPUVendor::VENDOR_QUALCOMM; break;
	case VULKAN_VENDOR_INTEL: caps_.vendor = GPUVendor::VENDOR_INTEL; break;
	default: caps_.vendor = GPUVendor::VENDOR_UNKNOWN; break;
	}

	if (caps_.vendor == GPUVendor::VENDOR_QUALCOMM) {
		// Adreno 5xx devices, all known driver versions, fail to discard stencil when depth write is off.
		// See: https://github.com/hrydgard/ppsspp/pull/11684
		if (deviceProps.deviceID >= 0x05000000 && deviceProps.deviceID < 0x06000000) {
			if (deviceProps.driverVersion < 0x80180000) {
				bugs_.Infest(Bugs::NO_DEPTH_CANNOT_DISCARD_STENCIL);
			}
		}
		// Color write mask not masking write in certain scenarios with a depth test, see #10421.
		// Known still present on driver 0x80180000 and Adreno 5xx (possibly more.)
		bugs_.Infest(Bugs::COLORWRITEMASK_BROKEN_WITH_DEPTHTEST);

		// Trying to follow all the rules in https://registry.khronos.org/vulkan/specs/1.3/html/vkspec.html#synchronization-pipeline-barriers-subpass-self-dependencies
		// and https://registry.khronos.org/vulkan/specs/1.3/html/vkspec.html#renderpass-feedbackloop, but still it doesn't
		// quite work - artifacts on triangle boundaries on Adreno.
		bugs_.Infest(Bugs::SUBPASS_FEEDBACK_BROKEN);
	} else if (caps_.vendor == GPUVendor::VENDOR_AMD) {
		// See issue #10074, and also #10065 (AMD) and #10109 for the choice of the driver version to check for.
		if (deviceProps.driverVersion < 0x00407000) {
			bugs_.Infest(Bugs::DUAL_SOURCE_BLENDING_BROKEN);
		}
	} else if (caps_.vendor == GPUVendor::VENDOR_INTEL) {
		// Workaround for Intel driver bug. TODO: Re-enable after some driver version
		bugs_.Infest(Bugs::DUAL_SOURCE_BLENDING_BROKEN);
	} else if (caps_.vendor == GPUVendor::VENDOR_ARM) {
		int majorVersion = VK_API_VERSION_MAJOR(deviceProps.driverVersion);

		// These GPUs (up to some certain hardware version?) have a bug where draws where gl_Position.w == .z
		// corrupt the depth buffer. This is easily worked around by simply scaling Z down a tiny bit when this case
		// is detected. See: https://github.com/hrydgard/ppsspp/issues/11937
		bugs_.Infest(Bugs::EQUAL_WZ_CORRUPTS_DEPTH);

		// Nearly identical to the the Adreno bug, see #13833 (Midnight Club map broken) and other issues.
		// Reported fixed in major version 40 - let's add a check once confirmed.
		bugs_.Infest(Bugs::NO_DEPTH_CANNOT_DISCARD_STENCIL);

		// This started in driver 31 or 32, fixed in 40 - let's add a check once confirmed.
		if (majorVersion >= 32) {
			bugs_.Infest(Bugs::MALI_CONSTANT_LOAD_BUG);  // See issue #15661
		}
	}

	// Limited, through input attachments and self-dependencies.
	// We turn it off here already if buggy.
	caps_.framebufferFetchSupported = !bugs_.Has(Bugs::SUBPASS_FEEDBACK_BROKEN);

	caps_.deviceID = deviceProps.deviceID;
	device_ = vulkan->GetDevice();

	queue_ = vulkan->GetGraphicsQueue();
	queueFamilyIndex_ = vulkan->GetGraphicsQueueFamilyIndex();

	VkCommandPoolCreateInfo p{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	p.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	p.queueFamilyIndex = vulkan->GetGraphicsQueueFamilyIndex();

	std::vector<VkDescriptorPoolSize> dpTypes;
	dpTypes.resize(2);
	dpTypes[0].descriptorCount = 200;
	dpTypes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	dpTypes[1].descriptorCount = 200 * MAX_BOUND_TEXTURES;
	dpTypes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	VkDescriptorPoolCreateInfo dp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	// Don't want to mess around with individually freeing these, let's go dynamic each frame.
	dp.flags = 0;
	// 200 textures per frame was not enough for the UI.
	dp.maxSets = 4096;

	for (int i = 0; i < VulkanContext::MAX_INFLIGHT_FRAMES; i++) {
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		frame_[i].pushBuffer = new VulkanPushBuffer(vulkan_, "pushBuffer", 1024 * 1024, usage, PushBufferType::CPU_TO_GPU);
		frame_[i].descriptorPool.Create(vulkan_, dp, dpTypes);
	}

	// binding 0 - uniform data
	// binding 1 - combined sampler/image 0
	// binding 2 - combined sampler/image 1
	VkDescriptorSetLayoutBinding bindings[MAX_BOUND_TEXTURES + 1];
	bindings[0].descriptorCount = 1;
	bindings[0].pImmutableSamplers = nullptr;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[0].binding = 0;
	for (int i = 0; i < MAX_BOUND_TEXTURES; ++i) {
		bindings[i + 1].descriptorCount = 1;
		bindings[i + 1].pImmutableSamplers = nullptr;
		bindings[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i + 1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		bindings[i + 1].binding = i + 1;
	}

	VkDescriptorSetLayoutCreateInfo dsl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dsl.bindingCount = ARRAY_SIZE(bindings);
	dsl.pBindings = bindings;
	VkResult res = vkCreateDescriptorSetLayout(device_, &dsl, nullptr, &descriptorSetLayout_);
	_assert_(VK_SUCCESS == res);

	vulkan_->SetDebugName(descriptorSetLayout_, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "thin3d_d_layout");

	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.pPushConstantRanges = nullptr;
	pl.pushConstantRangeCount = 0;
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &descriptorSetLayout_;
	res = vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_);
	_assert_(VK_SUCCESS == res);

	vulkan_->SetDebugName(pipelineLayout_, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "thin3d_p_layout");

	VkPipelineCacheCreateInfo pc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	res = vkCreatePipelineCache(vulkan_->GetDevice(), &pc, nullptr, &pipelineCache_);
	_assert_(VK_SUCCESS == res);
}

VKContext::~VKContext() {
	delete nullTexture_;
	// This also destroys all descriptor sets.
	for (int i = 0; i < VulkanContext::MAX_INFLIGHT_FRAMES; i++) {
		frame_[i].descriptorPool.Destroy();
		frame_[i].pushBuffer->Destroy(vulkan_);
		delete frame_[i].pushBuffer;
	}
	vulkan_->Delete().QueueDeleteDescriptorSetLayout(descriptorSetLayout_);
	vulkan_->Delete().QueueDeletePipelineLayout(pipelineLayout_);
	vulkan_->Delete().QueueDeletePipelineCache(pipelineCache_);
}

void VKContext::BeginFrame() {
	// TODO: Bad dependency on g_Config here!
	renderManager_.BeginFrame(g_Config.bShowGpuProfile, g_Config.bGpuLogProfiler);

	FrameData &frame = frame_[vulkan_->GetCurFrame()];
	push_ = frame.pushBuffer;

	// OK, we now know that nothing is reading from this frame's data pushbuffer,
	push_->Reset();
	push_->Begin(vulkan_);

	frame.descriptorPool.Reset();
}

void VKContext::EndFrame() {
	// Stop collecting data in the frame's data pushbuffer.
	push_->End();

	renderManager_.Finish();

	push_ = nullptr;

	// Unbind stuff, to avoid accidentally relying on it across frames (and provide some protection against forgotten unbinds of deleted things).
	InvalidateCachedState();
}

void VKContext::InvalidateCachedState() {
	curPipeline_ = nullptr;

	for (auto &view : boundImageView_) {
		view = VK_NULL_HANDLE;
	}
	for (auto &sampler : boundSamplers_) {
		sampler = nullptr;
	}
	for (auto &texture : boundTextures_) {
		texture = nullptr;
	}
}

void VKContext::WipeQueue() {
	renderManager_.Wipe();
}

VkDescriptorSet VKContext::GetOrCreateDescriptorSet(VkBuffer buf) {
	DescriptorSetKey key;

	FrameData *frame = &frame_[vulkan_->GetCurFrame()];

	for (int i = 0; i < MAX_BOUND_TEXTURES; ++i) {
		key.imageViews_[i] = boundTextures_[i] ? boundTextures_[i]->GetImageView() : boundImageView_[i];
		key.samplers_[i] = boundSamplers_[i];
	}
	key.buffer_ = buf;

	auto iter = frame->descSets_.find(key);
	if (iter != frame->descSets_.end()) {
		return iter->second;
	}

	VkDescriptorSet descSet = frame->descriptorPool.Allocate(1, &descriptorSetLayout_);
	if (descSet == VK_NULL_HANDLE) {
		ERROR_LOG(G3D, "GetOrCreateDescriptorSet failed");
		return VK_NULL_HANDLE;
	}

	VkDescriptorBufferInfo bufferDesc;
	bufferDesc.buffer = buf;
	bufferDesc.offset = 0;
	bufferDesc.range = curPipeline_->GetUBOSize();

	VkDescriptorImageInfo imageDesc[MAX_BOUND_TEXTURES]{};
	VkWriteDescriptorSet writes[1 + MAX_BOUND_TEXTURES]{};

	// If handles are NULL for whatever buggy reason, it's best to leave the descriptors
	// unwritten instead of trying to write a zero, which is not legal.

	int numWrites = 0;
	if (buf) {
		writes[numWrites].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[numWrites].dstSet = descSet;
		writes[numWrites].dstArrayElement = 0;
		writes[numWrites].dstBinding = 0;
		writes[numWrites].pBufferInfo = &bufferDesc;
		writes[numWrites].pImageInfo = nullptr;
		writes[numWrites].pTexelBufferView = nullptr;
		writes[numWrites].descriptorCount = 1;
		writes[numWrites].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		numWrites++;
	}

	for (int i = 0; i < MAX_BOUND_TEXTURES; ++i) {
		if (key.imageViews_[i] && key.samplers_[i] && key.samplers_[i]->GetSampler()) {
			imageDesc[i].imageView = key.imageViews_[i];
			imageDesc[i].sampler = key.samplers_[i]->GetSampler();
			imageDesc[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			writes[numWrites].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[numWrites].dstSet = descSet;
			writes[numWrites].dstArrayElement = 0;
			writes[numWrites].dstBinding = i + 1;
			writes[numWrites].pBufferInfo = nullptr;
			writes[numWrites].pImageInfo = &imageDesc[i];
			writes[numWrites].pTexelBufferView = nullptr;
			writes[numWrites].descriptorCount = 1;
			writes[numWrites].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			numWrites++;
		}
	}

	vkUpdateDescriptorSets(device_, numWrites, writes, 0, nullptr);

	frame->descSets_[key] = descSet;
	return descSet;
}

Pipeline *VKContext::CreateGraphicsPipeline(const PipelineDesc &desc, const char *tag) {
	VKInputLayout *input = (VKInputLayout *)desc.inputLayout;
	VKBlendState *blend = (VKBlendState *)desc.blend;
	VKDepthStencilState *depth = (VKDepthStencilState *)desc.depthStencil;
	VKRasterState *raster = (VKRasterState *)desc.raster;

	PipelineFlags pipelineFlags = (PipelineFlags)0;
	if (depth->info.depthTestEnable || depth->info.stencilTestEnable) {
		pipelineFlags |= PipelineFlags::USES_DEPTH_STENCIL;
	}

	VKPipeline *pipeline = new VKPipeline(vulkan_, desc.uniformDesc ? desc.uniformDesc->uniformBufferSize : 16 * sizeof(float), pipelineFlags, tag);

	VKRGraphicsPipelineDesc &gDesc = pipeline->vkrDesc;

	std::vector<VkPipelineShaderStageCreateInfo> stages;
	stages.resize(desc.shaders.size());

	for (auto &iter : desc.shaders) {
		VKShaderModule *vkshader = (VKShaderModule *)iter;
		vkshader->AddRef();
		pipeline->deps.push_back(vkshader);
		if (vkshader->GetStage() == ShaderStage::Vertex) {
			gDesc.vertexShader = Promise<VkShaderModule>::AlreadyDone(vkshader->Get());
		} else if (vkshader->GetStage() == ShaderStage::Fragment) {
			gDesc.fragmentShader = Promise<VkShaderModule>::AlreadyDone(vkshader->Get());
		} else {
			ERROR_LOG(G3D, "Bad stage");
			return nullptr;
		}
	}

	if (input) {
		for (int i = 0; i < (int)input->bindings.size(); i++) {
			pipeline->stride[i] = input->bindings[i].stride;
		}
	} else {
		pipeline->stride[0] = 0;
	}
	_dbg_assert_(input->bindings.size() == 1);
	_dbg_assert_((int)input->attributes.size() == (int)input->visc.vertexAttributeDescriptionCount);

	gDesc.ibd = input->bindings[0];
	for (size_t i = 0; i < input->attributes.size(); i++) {
		gDesc.attrs[i] = input->attributes[i];
	}
	gDesc.vis.vertexAttributeDescriptionCount = input->visc.vertexAttributeDescriptionCount;
	gDesc.vis.vertexBindingDescriptionCount = input->visc.vertexBindingDescriptionCount;
	gDesc.vis.pVertexBindingDescriptions = &gDesc.ibd;
	gDesc.vis.pVertexAttributeDescriptions = gDesc.attrs;

	gDesc.blend0 = blend->attachments[0];
	gDesc.cbs = blend->info;
	gDesc.cbs.pAttachments = &gDesc.blend0;

	gDesc.dss = depth->info;

	raster->ToVulkan(&gDesc.rs);

	// Copy bindings from input layout.
	gDesc.inputAssembly.topology = primToVK[(int)desc.prim];

	// We treat the three stencil states as a unit in other places, so let's do that here too.
	const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
	gDesc.ds.dynamicStateCount = depth->info.stencilTestEnable ? ARRAY_SIZE(dynamics) : 2;
	for (size_t i = 0; i < gDesc.ds.dynamicStateCount; i++) {
		gDesc.dynamicStates[i] = dynamics[i];
	}
	gDesc.ds.pDynamicStates = gDesc.dynamicStates;

	gDesc.ms.pSampleMask = nullptr;
	gDesc.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	gDesc.views.viewportCount = 1;
	gDesc.views.scissorCount = 1;
	gDesc.views.pViewports = nullptr;  // dynamic
	gDesc.views.pScissors = nullptr;  // dynamic

	gDesc.pipelineLayout = pipelineLayout_;

	VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster->ToVulkan(&gDesc.rs);

	pipeline->pipeline = renderManager_.CreateGraphicsPipeline(&gDesc, 1 << RP_TYPE_BACKBUFFER, tag ? tag : "thin3d");

	if (desc.uniformDesc) {
		pipeline->dynamicUniformSize = (int)desc.uniformDesc->uniformBufferSize;
	}
	if (depth->info.stencilTestEnable) {
		pipeline->usesStencil = true;
	}
	return pipeline;
}

void VKContext::SetScissorRect(int left, int top, int width, int height) {
	renderManager_.SetScissor(left, top, width, height);
}

void VKContext::SetViewports(int count, Viewport *viewports) {
	if (count > 0) {
		// Ignore viewports more than the first.
		VkViewport viewport;
		viewport.x = viewports[0].TopLeftX;
		viewport.y = viewports[0].TopLeftY;
		viewport.width = viewports[0].Width;
		viewport.height = viewports[0].Height;
		viewport.minDepth = viewports[0].MinDepth;
		viewport.maxDepth = viewports[0].MaxDepth;
		renderManager_.SetViewport(viewport);
	}
}

void VKContext::SetBlendFactor(float color[4]) {
	uint32_t col = Float4ToUint8x4(color);
	renderManager_.SetBlendFactor(col);
}

void VKContext::SetStencilParams(uint8_t refValue, uint8_t writeMask, uint8_t compareMask) {
	if (curPipeline_->usesStencil)
		renderManager_.SetStencilParams(writeMask, compareMask, refValue);
	stencilRef_ = refValue;
	stencilWriteMask_ = writeMask;
	stencilCompareMask_ = compareMask;
}

InputLayout *VKContext::CreateInputLayout(const InputLayoutDesc &desc) {
	VKInputLayout *vl = new VKInputLayout();
	vl->visc = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vl->visc.flags = 0;
	vl->visc.vertexAttributeDescriptionCount = (uint32_t)desc.attributes.size();
	vl->visc.vertexBindingDescriptionCount = (uint32_t)desc.bindings.size();
	vl->bindings.resize(vl->visc.vertexBindingDescriptionCount);
	vl->attributes.resize(vl->visc.vertexAttributeDescriptionCount);
	vl->visc.pVertexBindingDescriptions = vl->bindings.data();
	vl->visc.pVertexAttributeDescriptions = vl->attributes.data();
	for (size_t i = 0; i < desc.attributes.size(); i++) {
		vl->attributes[i].binding = (uint32_t)desc.attributes[i].binding;
		vl->attributes[i].format = DataFormatToVulkan(desc.attributes[i].format);
		vl->attributes[i].location = desc.attributes[i].location;
		vl->attributes[i].offset = desc.attributes[i].offset;
	}
	for (size_t i = 0; i < desc.bindings.size(); i++) {
		vl->bindings[i].inputRate = desc.bindings[i].instanceRate ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
		vl->bindings[i].binding = (uint32_t)i;
		vl->bindings[i].stride = desc.bindings[i].stride;
	}
	return vl;
}

Texture *VKContext::CreateTexture(const TextureDesc &desc) {
	VkCommandBuffer initCmd = renderManager_.GetInitCmd();
	if (!push_ || !initCmd) {
		// Too early! Fail.
		ERROR_LOG(G3D,  "Can't create textures before the first frame has started.");
		return nullptr;
	}
	VKTexture *tex = new VKTexture(vulkan_, initCmd, push_, desc);
	if (tex->Create(initCmd, push_, desc)) {
		return tex;
	} else {
		ERROR_LOG(G3D,  "Failed to create texture");
		delete tex;
		return nullptr;
	}
}

static inline void CopySide(VkStencilOpState &dest, const StencilSetup &src) {
	dest.compareOp = compToVK[(int)src.compareOp];
	dest.failOp = stencilOpToVK[(int)src.failOp];
	dest.passOp = stencilOpToVK[(int)src.passOp];
	dest.depthFailOp = stencilOpToVK[(int)src.depthFailOp];
}

DepthStencilState *VKContext::CreateDepthStencilState(const DepthStencilStateDesc &desc) {
	VKDepthStencilState *ds = new VKDepthStencilState();
	ds->info.depthCompareOp = compToVK[(int)desc.depthCompare];
	ds->info.depthTestEnable = desc.depthTestEnabled;
	ds->info.depthWriteEnable = desc.depthWriteEnabled;
	ds->info.stencilTestEnable = desc.stencilEnabled;
	ds->info.depthBoundsTestEnable = false;
	if (ds->info.stencilTestEnable) {
		CopySide(ds->info.front, desc.stencil);
		CopySide(ds->info.back, desc.stencil);
	}
	return ds;
}

BlendState *VKContext::CreateBlendState(const BlendStateDesc &desc) {
	VKBlendState *bs = new VKBlendState();
	bs->info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	bs->info.attachmentCount = 1;
	bs->info.logicOp = logicOpToVK[(int)desc.logicOp];
	bs->info.logicOpEnable = desc.logicEnabled;
	bs->attachments.resize(1);
	bs->attachments[0].blendEnable = desc.enabled;
	bs->attachments[0].colorBlendOp = blendEqToVk[(int)desc.eqCol];
	bs->attachments[0].alphaBlendOp = blendEqToVk[(int)desc.eqAlpha];
	bs->attachments[0].colorWriteMask = desc.colorMask;
	bs->attachments[0].dstAlphaBlendFactor = blendFactorToVk[(int)desc.dstAlpha];
	bs->attachments[0].dstColorBlendFactor = blendFactorToVk[(int)desc.dstCol];
	bs->attachments[0].srcAlphaBlendFactor = blendFactorToVk[(int)desc.srcAlpha];
	bs->attachments[0].srcColorBlendFactor = blendFactorToVk[(int)desc.srcCol];
	bs->info.pAttachments = bs->attachments.data();
	return bs;
}

// Very simplistic buffer that will simply copy its contents into our "pushbuffer" when it's time to draw,
// to avoid synchronization issues.
class VKBuffer : public Buffer {
public:
	VKBuffer(size_t size, uint32_t flags) : dataSize_(size) {
		data_ = new uint8_t[size];
	}
	~VKBuffer() override {
		delete[] data_;
	}

	size_t GetSize() const { return dataSize_; }
	const uint8_t *GetData() const { return data_; }

	uint8_t *data_;
	size_t dataSize_;
};

Buffer *VKContext::CreateBuffer(size_t size, uint32_t usageFlags) {
	return new VKBuffer(size, usageFlags);
}

void VKContext::UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) {
	VKBuffer *buf = (VKBuffer *)buffer;
	memcpy(buf->data_ + offset, data, size);
}

void VKContext::BindTextures(int start, int count, Texture **textures) {
	_assert_(start + count <= MAX_BOUND_TEXTURES);
	for (int i = start; i < start + count; i++) {
		boundTextures_[i] = static_cast<VKTexture *>(textures[i - start]);
		boundImageView_[i] = boundTextures_[i] ? boundTextures_[i]->GetImageView() : GetNullTexture()->GetImageView();
	}
}

void VKContext::BindNativeTexture(int sampler, void *nativeTexture) {
	boundTextures_[sampler] = nullptr;
	boundImageView_[sampler] = (VkImageView)nativeTexture;
}

ShaderModule *VKContext::CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t size, const char *tag) {
	VKShaderModule *shader = new VKShaderModule(stage, tag);
	if (shader->Compile(vulkan_, language, data, size)) {
		return shader;
	} else {
		ERROR_LOG(G3D, "Failed to compile shader %s:\n%s", tag, (const char *)LineNumberString((const char *)data).c_str());
		shader->Release();
		return nullptr;
	}
}

int VKPipeline::GetUniformLoc(const char *name) {
	int loc = -1;

	// HACK! As we only use one uniform we hardcode it.
	if (!strcmp(name, "WorldViewProj")) {
		return 0;
	}

	return loc;
}

void VKContext::UpdateDynamicUniformBuffer(const void *ub, size_t size) {
	curPipeline_->SetDynamicUniformData(ub, size);
}

void VKContext::ApplyDynamicState() {
	// TODO: blend constants, stencil, viewports should be here, after bindpipeline..
	if (curPipeline_->usesStencil) {
		renderManager_.SetStencilParams(stencilWriteMask_, stencilCompareMask_, stencilRef_);
	}
}

void VKContext::Draw(int vertexCount, int offset) {
	VKBuffer *vbuf = curVBuffers_[0];

	VkBuffer vulkanVbuf;
	VkBuffer vulkanUBObuf;
	uint32_t ubo_offset = (uint32_t)curPipeline_->PushUBO(push_, vulkan_, &vulkanUBObuf);
	size_t vbBindOffset = push_->Push(vbuf->GetData(), vbuf->GetSize(), &vulkanVbuf);

	VkDescriptorSet descSet = GetOrCreateDescriptorSet(vulkanUBObuf);
	if (descSet == VK_NULL_HANDLE) {
		ERROR_LOG(G3D, "GetOrCreateDescriptorSet failed, skipping %s", __FUNCTION__);
		return;
	}

	BindCurrentPipeline();
	ApplyDynamicState();
	renderManager_.Draw(descSet, 1, &ubo_offset, vulkanVbuf, (int)vbBindOffset + curVBufferOffsets_[0], vertexCount, offset);
}

void VKContext::DrawIndexed(int vertexCount, int offset) {
	VKBuffer *ibuf = curIBuffer_;
	VKBuffer *vbuf = curVBuffers_[0];

	VkBuffer vulkanVbuf, vulkanIbuf, vulkanUBObuf;
	uint32_t ubo_offset = (uint32_t)curPipeline_->PushUBO(push_, vulkan_, &vulkanUBObuf);
	size_t vbBindOffset = push_->Push(vbuf->GetData(), vbuf->GetSize(), &vulkanVbuf);
	size_t ibBindOffset = push_->Push(ibuf->GetData(), ibuf->GetSize(), &vulkanIbuf);

	VkDescriptorSet descSet = GetOrCreateDescriptorSet(vulkanUBObuf);
	if (descSet == VK_NULL_HANDLE) {
		ERROR_LOG(G3D, "GetOrCreateDescriptorSet failed, skipping %s", __FUNCTION__);
		return;
	}

	BindCurrentPipeline();
	ApplyDynamicState();
	renderManager_.DrawIndexed(descSet, 1, &ubo_offset, vulkanVbuf, (int)vbBindOffset + curVBufferOffsets_[0], vulkanIbuf, (int)ibBindOffset + offset * sizeof(uint32_t), vertexCount, 1, VK_INDEX_TYPE_UINT16);
}

void VKContext::DrawUP(const void *vdata, int vertexCount) {
	VkBuffer vulkanVbuf, vulkanUBObuf;
	size_t vbBindOffset = push_->Push(vdata, vertexCount * curPipeline_->stride[0], &vulkanVbuf);
	uint32_t ubo_offset = (uint32_t)curPipeline_->PushUBO(push_, vulkan_, &vulkanUBObuf);

	VkDescriptorSet descSet = GetOrCreateDescriptorSet(vulkanUBObuf);
	if (descSet == VK_NULL_HANDLE) {
		ERROR_LOG(G3D, "GetOrCreateDescriptorSet failed, skipping %s", __FUNCTION__);
		return;
	}

	BindCurrentPipeline();
	ApplyDynamicState();
	renderManager_.Draw(descSet, 1, &ubo_offset, vulkanVbuf, (int)vbBindOffset + curVBufferOffsets_[0], vertexCount);
}

void VKContext::BindCurrentPipeline() {
	renderManager_.BindPipeline(curPipeline_->pipeline, curPipeline_->flags, pipelineLayout_);
}

void VKContext::Clear(int clearMask, uint32_t colorval, float depthVal, int stencilVal) {
	int mask = 0;
	if (clearMask & FBChannel::FB_COLOR_BIT)
		mask |= VK_IMAGE_ASPECT_COLOR_BIT;
	if (clearMask & FBChannel::FB_DEPTH_BIT)
		mask |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (clearMask & FBChannel::FB_STENCIL_BIT)
		mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
	renderManager_.Clear(colorval, depthVal, stencilVal, mask);
}

DrawContext *T3DCreateVulkanContext(VulkanContext *vulkan) {
	return new VKContext(vulkan);
}

void AddFeature(std::vector<std::string> &features, const char *name, VkBool32 available, VkBool32 enabled) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s: Available: %d Enabled: %d", name, (int)available, (int)enabled);
	features.push_back(buf);
}

std::vector<std::string> VKContext::GetFeatureList() const {
	const VkPhysicalDeviceFeatures &available = vulkan_->GetDeviceFeatures().available;
	const VkPhysicalDeviceFeatures &enabled = vulkan_->GetDeviceFeatures().enabled;

	std::vector<std::string> features;
	AddFeature(features, "dualSrcBlend", available.dualSrcBlend, enabled.dualSrcBlend);
	AddFeature(features, "logicOp", available.logicOp, enabled.logicOp);
	AddFeature(features, "geometryShader", available.geometryShader, enabled.geometryShader);
	AddFeature(features, "depthBounds", available.depthBounds, enabled.depthBounds);
	AddFeature(features, "depthClamp", available.depthClamp, enabled.depthClamp);
	AddFeature(features, "fillModeNonSolid", available.fillModeNonSolid, enabled.fillModeNonSolid);
	AddFeature(features, "pipelineStatisticsQuery", available.pipelineStatisticsQuery, enabled.pipelineStatisticsQuery);
	AddFeature(features, "samplerAnisotropy", available.samplerAnisotropy, enabled.samplerAnisotropy);
	AddFeature(features, "textureCompressionBC", available.textureCompressionBC, enabled.textureCompressionBC);
	AddFeature(features, "textureCompressionETC2", available.textureCompressionETC2, enabled.textureCompressionETC2);
	AddFeature(features, "textureCompressionASTC_LDR", available.textureCompressionASTC_LDR, enabled.textureCompressionASTC_LDR);
	AddFeature(features, "shaderClipDistance", available.shaderClipDistance, enabled.shaderClipDistance);
	AddFeature(features, "shaderCullDistance", available.shaderCullDistance, enabled.shaderCullDistance);
	AddFeature(features, "occlusionQueryPrecise", available.occlusionQueryPrecise, enabled.occlusionQueryPrecise);
	AddFeature(features, "multiDrawIndirect", available.multiDrawIndirect, enabled.multiDrawIndirect);

	features.push_back(std::string("Preferred depth buffer format: ") + VulkanFormatToString(vulkan_->GetDeviceInfo().preferredDepthStencilFormat));

	return features;
}

std::vector<std::string> VKContext::GetExtensionList() const {
	std::vector<std::string> extensions;
	for (auto &iter : vulkan_->GetDeviceExtensionsAvailable()) {
		extensions.push_back(iter.extensionName);
	}
	return extensions;
}

uint32_t VKContext::GetDataFormatSupport(DataFormat fmt) const {
	VkFormat vulkan_format = DataFormatToVulkan(fmt);
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(vulkan_->GetCurrentPhysicalDevice(), vulkan_format, &properties);
	uint32_t flags = 0;
	if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
		flags |= FMT_RENDERTARGET;
	}
	if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
		flags |= FMT_DEPTHSTENCIL;
	}
	if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
		flags |= FMT_TEXTURE;
	}
	if (properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) {
		flags |= FMT_INPUTLAYOUT;
	}
	if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
		flags |= FMT_BLIT;
	}
	if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
		flags |= FMT_STORAGE_IMAGE;
	}
	return flags;
}

// A VKFramebuffer is a VkFramebuffer (note caps difference) plus all the textures it owns.
// It also has a reference to the command buffer that it was last rendered to with.
// If it needs to be transitioned, and the frame number matches, use it, otherwise
// use this frame's init command buffer.
class VKFramebuffer : public Framebuffer {
public:
	VKFramebuffer(VKRFramebuffer *fb) : buf_(fb) {
		_assert_msg_(fb, "Null fb in VKFramebuffer constructor");
		width_ = fb->width;
		height_ = fb->height;
	}
	~VKFramebuffer() {
		_assert_msg_(buf_, "Null buf_ in VKFramebuffer - double delete?");
		buf_->vulkan_->Delete().QueueCallback([](void *fb) {
			VKRFramebuffer *vfb = static_cast<VKRFramebuffer *>(fb);
			delete vfb;
		}, buf_);
		buf_ = nullptr;
	}
	VKRFramebuffer *GetFB() const { return buf_; }
private:
	VKRFramebuffer *buf_;
};

Framebuffer *VKContext::CreateFramebuffer(const FramebufferDesc &desc) {
	VkCommandBuffer cmd = renderManager_.GetInitCmd();
	// TODO: We always create with depth here, even when it's not needed (such as color temp FBOs).
	// Should optimize those away.
	VKRFramebuffer *vkrfb = new VKRFramebuffer(vulkan_, cmd, renderManager_.GetQueueRunner()->GetCompatibleRenderPass(), desc.width, desc.height, desc.tag);
	return new VKFramebuffer(vkrfb);
}

void VKContext::CopyFramebufferImage(Framebuffer *srcfb, int level, int x, int y, int z, Framebuffer *dstfb, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth, int channelBits, const char *tag) {
	VKFramebuffer *src = (VKFramebuffer *)srcfb;
	VKFramebuffer *dst = (VKFramebuffer *)dstfb;

	int aspectMask = 0;
	if (channelBits & FBChannel::FB_COLOR_BIT) aspectMask |= VK_IMAGE_ASPECT_COLOR_BIT;
	if (channelBits & FBChannel::FB_DEPTH_BIT) aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (channelBits & FBChannel::FB_STENCIL_BIT) aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

	renderManager_.CopyFramebuffer(src->GetFB(), VkRect2D{ {x, y}, {(uint32_t)width, (uint32_t)height } }, dst->GetFB(), VkOffset2D{ dstX, dstY }, aspectMask, tag);
}

bool VKContext::BlitFramebuffer(Framebuffer *srcfb, int srcX1, int srcY1, int srcX2, int srcY2, Framebuffer *dstfb, int dstX1, int dstY1, int dstX2, int dstY2, int channelBits, FBBlitFilter filter, const char *tag) {
	VKFramebuffer *src = (VKFramebuffer *)srcfb;
	VKFramebuffer *dst = (VKFramebuffer *)dstfb;

	int aspectMask = 0;
	if (channelBits & FBChannel::FB_COLOR_BIT) aspectMask |= VK_IMAGE_ASPECT_COLOR_BIT;
	if (channelBits & FBChannel::FB_DEPTH_BIT) aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (channelBits & FBChannel::FB_STENCIL_BIT) aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

	renderManager_.BlitFramebuffer(src->GetFB(), VkRect2D{ {srcX1, srcY1}, {(uint32_t)(srcX2 - srcX1), (uint32_t)(srcY2 - srcY1) } }, dst->GetFB(), VkRect2D{ {dstX1, dstY1}, {(uint32_t)(dstX2 - dstX1), (uint32_t)(dstY2 - dstY1) } }, aspectMask, filter == FB_BLIT_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, tag);
	return true;
}

bool VKContext::CopyFramebufferToMemorySync(Framebuffer *srcfb, int channelBits, int x, int y, int w, int h, Draw::DataFormat format, void *pixels, int pixelStride, const char *tag) {
	VKFramebuffer *src = (VKFramebuffer *)srcfb;

	int aspectMask = 0;
	if (channelBits & FBChannel::FB_COLOR_BIT) aspectMask |= VK_IMAGE_ASPECT_COLOR_BIT;
	if (channelBits & FBChannel::FB_DEPTH_BIT) aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (channelBits & FBChannel::FB_STENCIL_BIT) aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

	return renderManager_.CopyFramebufferToMemorySync(src ? src->GetFB() : nullptr, aspectMask, x, y, w, h, format, (uint8_t *)pixels, pixelStride, tag);
}

DataFormat VKContext::PreferredFramebufferReadbackFormat(Framebuffer *src) {
	if (src) {
		return DrawContext::PreferredFramebufferReadbackFormat(src);
	}

	if (vulkan_->GetSwapchainFormat() == VK_FORMAT_B8G8R8A8_UNORM) {
		return Draw::DataFormat::B8G8R8A8_UNORM;
	}
	return DrawContext::PreferredFramebufferReadbackFormat(src);
}

void VKContext::BindFramebufferAsRenderTarget(Framebuffer *fbo, const RenderPassInfo &rp, const char *tag) {
	VKFramebuffer *fb = (VKFramebuffer *)fbo;
	VKRRenderPassLoadAction color = (VKRRenderPassLoadAction)rp.color;
	VKRRenderPassLoadAction depth = (VKRRenderPassLoadAction)rp.depth;
	VKRRenderPassLoadAction stencil = (VKRRenderPassLoadAction)rp.stencil;

	renderManager_.BindFramebufferAsRenderTarget(fb ? fb->GetFB() : nullptr, color, depth, stencil, rp.clearColor, rp.clearDepth, rp.clearStencil, tag);
	curFramebuffer_ = fb;
}

void VKContext::BindFramebufferAsTexture(Framebuffer *fbo, int binding, FBChannel channelBit, int attachment) {
	VKFramebuffer *fb = (VKFramebuffer *)fbo;
	_assert_(binding < MAX_BOUND_TEXTURES);

	// TODO: There are cases where this is okay, actually. But requires layout transitions and stuff -
	// we're not ready for this.
	_assert_(fb != curFramebuffer_);

	int aspect = 0;
	switch (channelBit) {
	case FBChannel::FB_COLOR_BIT:
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		break;
	case FBChannel::FB_DEPTH_BIT:
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		break;
	default:
		_assert_(false);
		break;
	}
	boundTextures_[binding] = nullptr;
	boundImageView_[binding] = renderManager_.BindFramebufferAsTexture(fb->GetFB(), binding, aspect, attachment);
}

void VKContext::BindCurrentFramebufferForColorInput() {
	renderManager_.BindCurrentFramebufferAsInputAttachment0(VK_IMAGE_ASPECT_COLOR_BIT);
}

void VKContext::GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) {
	VKFramebuffer *fb = (VKFramebuffer *)fbo;
	if (fb) {
		*w = fb->GetFB()->width;
		*h = fb->GetFB()->height;
	} else {
		*w = vulkan_->GetBackbufferWidth();
		*h = vulkan_->GetBackbufferHeight();
	}
}

void VKContext::HandleEvent(Event ev, int width, int height, void *param1, void *param2) {
	switch (ev) {
	case Event::LOST_BACKBUFFER:
		renderManager_.DestroyBackbuffers();
		break;
	case Event::GOT_BACKBUFFER:
		renderManager_.CreateBackbuffers();
		break;
	default:
		_assert_(false);
		break;
	}
}

void VKContext::InvalidateFramebuffer(FBInvalidationStage stage, uint32_t channels) {
	VkImageAspectFlags flags = 0;
	if (channels & FBChannel::FB_COLOR_BIT)
		flags |= VK_IMAGE_ASPECT_COLOR_BIT;
	if (channels & FBChannel::FB_DEPTH_BIT)
		flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if (channels & FBChannel::FB_STENCIL_BIT)
		flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if (stage == FB_INVALIDATION_LOAD) {
		renderManager_.SetLoadDontCare(flags);
	} else if (stage == FB_INVALIDATION_STORE) {
		renderManager_.SetStoreDontCare(flags);
	}
}

uint64_t VKContext::GetNativeObject(NativeObject obj, void *srcObject) {
	switch (obj) {
	case NativeObject::CONTEXT:
		return (uint64_t)vulkan_;
	case NativeObject::INIT_COMMANDBUFFER:
		return (uint64_t)renderManager_.GetInitCmd();
	case NativeObject::BOUND_TEXTURE0_IMAGEVIEW:
		return (uint64_t)boundImageView_[0];
	case NativeObject::BOUND_TEXTURE1_IMAGEVIEW:
		return (uint64_t)boundImageView_[1];
	case NativeObject::RENDER_MANAGER:
		return (uint64_t)(uintptr_t)&renderManager_;
	case NativeObject::NULL_IMAGEVIEW:
		return (uint64_t)GetNullTexture()->GetImageView();
	case NativeObject::TEXTURE_VIEW:
		return (uint64_t)(((VKTexture *)srcObject)->GetImageView());
	case NativeObject::BOUND_FRAMEBUFFER_COLOR_IMAGEVIEW:
		return (uint64_t)curFramebuffer_->GetFB()->color.imageView;
	default:
		Crash();
		return 0;
	}
}

}  // namespace Draw
