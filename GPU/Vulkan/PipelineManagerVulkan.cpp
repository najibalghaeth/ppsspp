#include <cstring>

#include "profiler/profiler.h"

#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/Vulkan/VulkanContext.h"
#include "GPU/Vulkan/VulkanUtil.h"
#include "GPU/Vulkan/PipelineManagerVulkan.h"
#include "GPU/Vulkan/ShaderManagerVulkan.h"

PipelineManagerVulkan::PipelineManagerVulkan(VulkanContext *vulkan) : vulkan_(vulkan), pipelines_(256) {
	pipelineCache_ = vulkan->CreatePipelineCache();
}

PipelineManagerVulkan::~PipelineManagerVulkan() {
	Clear();
	if (pipelineCache_ != VK_NULL_HANDLE)
		vulkan_->Delete().QueueDeletePipelineCache(pipelineCache_);
}

void PipelineManagerVulkan::Clear() {
	// This should kill off all the shaders at once.
	// This could also be an opportunity to store the whole cache to disk. Will need to also
	// store the keys.

	pipelines_.Iterate([&](const VulkanPipelineKey &key, VulkanPipeline *value) {
		vulkan_->Delete().QueueDeletePipeline(value->pipeline);
		delete value;
	});

	pipelines_.Clear();
}

void PipelineManagerVulkan::DeviceLost() {
	Clear();
	if (pipelineCache_ != VK_NULL_HANDLE)
		vulkan_->Delete().QueueDeletePipelineCache(pipelineCache_);
}

void PipelineManagerVulkan::DeviceRestore(VulkanContext *vulkan) {
	vulkan_ = vulkan;
	pipelineCache_ = vulkan->CreatePipelineCache();
}

struct DeclTypeInfo {
	VkFormat type;
	const char *name;
};

static const DeclTypeInfo VComp[] = {
	{ VK_FORMAT_UNDEFINED, "NULL" }, // DEC_NONE,
	{ VK_FORMAT_R32_SFLOAT, "R32_SFLOAT " },  // DEC_FLOAT_1,
	{ VK_FORMAT_R32G32_SFLOAT, "R32G32_SFLOAT " },  // DEC_FLOAT_2,
	{ VK_FORMAT_R32G32B32_SFLOAT, "R32G32B32_SFLOAT " },  // DEC_FLOAT_3,
	{ VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT " },  // DEC_FLOAT_4,

	{ VK_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM" }, // DEC_S8_3,
	{ VK_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM	" },	// DEC_S16_3,

	{ VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM	" },	// DEC_U8_1,
	{ VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM	" },	// DEC_U8_2,
	{ VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM	" },	// DEC_U8_3,
	{ VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM	" },	// DEC_U8_4,
	{ VK_FORMAT_R16G16_UNORM, "R16G16_UNORM" },	// 	DEC_U16_1,
	{ VK_FORMAT_R16G16_UNORM, "R16G16_UNORM" },	// 	DEC_U16_2,
	{ VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM " }, // DEC_U16_3,
	{ VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM " }, // DEC_U16_4,
};

static void VertexAttribSetup(VkVertexInputAttributeDescription *attr, int fmt, int offset, PspAttributeLocation location) {
	attr->location = (uint32_t)location;
	attr->binding = 0;
	attr->format = VComp[fmt].type;
	attr->offset = offset;
}

// Returns the number of attributes that were set.
// We could cache these AttributeDescription arrays (with pspFmt as the key), but hardly worth bothering
// as we will only call this code when we need to create a new VkPipeline.
static int SetupVertexAttribs(VkVertexInputAttributeDescription attrs[], const DecVtxFormat &decFmt) {
	int count = 0;
	if (decFmt.w0fmt != 0) {
		VertexAttribSetup(&attrs[count++], decFmt.w0fmt, decFmt.w0off, PspAttributeLocation::W1);
	}
	if (decFmt.w1fmt != 0) {
		VertexAttribSetup(&attrs[count++], decFmt.w1fmt, decFmt.w1off, PspAttributeLocation::W2);
	}
	if (decFmt.uvfmt != 0) {
		VertexAttribSetup(&attrs[count++], decFmt.uvfmt, decFmt.uvoff, PspAttributeLocation::TEXCOORD);
	}
	if (decFmt.c0fmt != 0) {
		VertexAttribSetup(&attrs[count++], decFmt.c0fmt, decFmt.c0off, PspAttributeLocation::COLOR0);
	}
	if (decFmt.c1fmt != 0) {
		VertexAttribSetup(&attrs[count++], decFmt.c1fmt, decFmt.c1off, PspAttributeLocation::COLOR1);
	}
	if (decFmt.nrmfmt != 0) {
		VertexAttribSetup(&attrs[count++], decFmt.nrmfmt, decFmt.nrmoff, PspAttributeLocation::NORMAL);
	}
	// Position is always there.
	VertexAttribSetup(&attrs[count++], decFmt.posfmt, decFmt.posoff, PspAttributeLocation::POSITION);
	return count;
}

static int SetupVertexAttribsPretransformed(VkVertexInputAttributeDescription attrs[], const DecVtxFormat &decFmt) {
	int count = 0;
	VertexAttribSetup(&attrs[count++], DEC_FLOAT_4, 0, PspAttributeLocation::POSITION);
	VertexAttribSetup(&attrs[count++], DEC_FLOAT_3, 16, PspAttributeLocation::TEXCOORD);
	VertexAttribSetup(&attrs[count++], DEC_U8_4, 28, PspAttributeLocation::COLOR0);
	VertexAttribSetup(&attrs[count++], DEC_U8_4, 32, PspAttributeLocation::COLOR1);
	return count;
}

static bool UsesBlendConstant(int factor) {
	return factor == VK_BLEND_FACTOR_CONSTANT_ALPHA || factor == VK_BLEND_FACTOR_CONSTANT_COLOR;
}

static VulkanPipeline *CreateVulkanPipeline(VkDevice device, VkPipelineCache pipelineCache, 
		VkPipelineLayout layout, VkRenderPass renderPass, const VulkanPipelineRasterStateKey &key,
		const DecVtxFormat *decFmt, VulkanVertexShader *vs, VulkanFragmentShader *fs, bool useHwTransform, float lineWidth) {
	bool useBlendConstant = false;

	VkPipelineColorBlendAttachmentState blend0 = {};
	blend0.blendEnable = key.blendEnable;
	if (key.blendEnable) {
		blend0.colorBlendOp = (VkBlendOp)key.blendOpColor;
		blend0.alphaBlendOp = (VkBlendOp)key.blendOpAlpha;
		blend0.srcColorBlendFactor = (VkBlendFactor)key.srcColor;
		blend0.srcAlphaBlendFactor = (VkBlendFactor)key.srcAlpha;
		blend0.dstColorBlendFactor = (VkBlendFactor)key.destColor;
		blend0.dstAlphaBlendFactor = (VkBlendFactor)key.destAlpha;
	}
	blend0.colorWriteMask = key.colorWriteMask;

	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cbs.flags = 0;
	cbs.pAttachments = &blend0;
	cbs.attachmentCount = 1;
	cbs.logicOpEnable = key.logicOpEnable;
	if (key.logicOpEnable)
		cbs.logicOp = (VkLogicOp)key.logicOp;
	else
		cbs.logicOp = VK_LOGIC_OP_COPY;

	VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	dss.depthBoundsTestEnable = false;
	dss.stencilTestEnable = key.stencilTestEnable;
	if (key.stencilTestEnable) {
		dss.front.compareOp = (VkCompareOp)key.stencilCompareOp;
		dss.front.passOp = (VkStencilOp)key.stencilPassOp;
		dss.front.failOp = (VkStencilOp)key.stencilFailOp;
		dss.front.depthFailOp = (VkStencilOp)key.stencilDepthFailOp;
		// Back stencil is always the same as front on PSP.
		memcpy(&dss.back, &dss.front, sizeof(dss.front));
	}
	dss.depthTestEnable = key.depthTestEnable;
	if (key.depthTestEnable) {
		dss.depthCompareOp = (VkCompareOp)key.depthCompareOp;
		dss.depthWriteEnable = key.depthWriteEnable;
	}

	VkDynamicState dynamicStates[8];
	int numDyn = 0;
	if (key.blendEnable &&
		  (UsesBlendConstant(key.srcAlpha) || UsesBlendConstant(key.srcColor) || UsesBlendConstant(key.destAlpha) || UsesBlendConstant(key.destColor))) {
		dynamicStates[numDyn++] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
		useBlendConstant = true;
	}
	dynamicStates[numDyn++] = VK_DYNAMIC_STATE_SCISSOR;
	dynamicStates[numDyn++] = VK_DYNAMIC_STATE_VIEWPORT;
	if (key.stencilTestEnable) {
		dynamicStates[numDyn++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
		dynamicStates[numDyn++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
		dynamicStates[numDyn++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
	}
	
	VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.flags = 0;
	ds.pDynamicStates = dynamicStates;
	ds.dynamicStateCount = numDyn;
	
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.flags = 0;
	rs.depthBiasEnable = false;
	rs.cullMode = key.cullMode;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = lineWidth;
	rs.rasterizerDiscardEnable = false;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.depthClampEnable = false;

	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.pSampleMask = nullptr;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineShaderStageCreateInfo ss[2];
	ss[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	ss[0].pNext = nullptr;
	ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	ss[0].pSpecializationInfo = nullptr;
	ss[0].module = vs->GetModule();
	ss[0].pName = "main";
	ss[0].flags = 0;
	ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	ss[1].pNext = nullptr;
	ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	ss[1].pSpecializationInfo = nullptr;
	ss[1].module = fs->GetModule();
	ss[1].pName = "main";
	ss[1].flags = 0;

	if (!ss[0].module || !ss[1].module) {
		ERROR_LOG(G3D, "Failed creating graphics pipeline - bad shaders");
		return nullptr;
	}

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.flags = 0;
	inputAssembly.topology = (VkPrimitiveTopology)key.topology;
	inputAssembly.primitiveRestartEnable = false;

	int vertexStride = 0;

	int offset = 0;
	VkVertexInputAttributeDescription attrs[8];
	int attributeCount;
	if (useHwTransform) {
		attributeCount = SetupVertexAttribs(attrs, *decFmt);
		vertexStride = decFmt->stride;
	} else {
		attributeCount = SetupVertexAttribsPretransformed(attrs, *decFmt);
		vertexStride = 36;
	}

	VkVertexInputBindingDescription ibd;
	ibd.binding = 0;
	ibd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	ibd.stride = vertexStride;

	VkPipelineVertexInputStateCreateInfo vis = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vis.flags = 0;
	vis.vertexBindingDescriptionCount = 1;
	vis.pVertexBindingDescriptions = &ibd;
	vis.vertexAttributeDescriptionCount = attributeCount;
	vis.pVertexAttributeDescriptions = attrs;

	VkPipelineViewportStateCreateInfo views = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	views.flags = 0;
	views.viewportCount = 1;
	views.scissorCount = 1;
	views.pViewports = nullptr;  // dynamic
	views.pScissors = nullptr;  // dynamic

	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.flags = 0;
	pipe.stageCount = 2;
	pipe.pStages = ss;
	pipe.basePipelineIndex = 0;

	pipe.pColorBlendState = &cbs;
	pipe.pDepthStencilState = &dss;
	pipe.pRasterizationState = &rs;

	// We will use dynamic viewport state.
	pipe.pVertexInputState = &vis;
	pipe.pViewportState = &views;
	pipe.pTessellationState = nullptr;
	pipe.pDynamicState = &ds;
	pipe.pInputAssemblyState = &inputAssembly;
	pipe.pMultisampleState = &ms;
	pipe.layout = layout;
	pipe.basePipelineHandle = VK_NULL_HANDLE;
	pipe.basePipelineIndex = 0;
	pipe.renderPass = renderPass;
	pipe.subpass = 0;

	VkPipeline pipeline;
	VkResult result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipe, nullptr, &pipeline);
	if (result != VK_SUCCESS) {
		_assert_msg_(G3D, false, "Failed creating graphics pipeline! result='%s'", VulkanResultToString(result));
		ERROR_LOG(G3D, "Failed creating graphics pipeline! result='%s'", VulkanResultToString(result));
		return nullptr;
	}

	VulkanPipeline *vulkanPipeline = new VulkanPipeline();
	vulkanPipeline->pipeline = pipeline;
	vulkanPipeline->uniformBlocks = UB_VS_FS_BASE;
	vulkanPipeline->useBlendConstant = useBlendConstant;
	vulkanPipeline->usesLines = key.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || key.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	if (useHwTransform) {
		if (vs->HasLights()) {
			vulkanPipeline->uniformBlocks |= UB_VS_LIGHTS;
		}
		if (vs->HasBones()) {
			vulkanPipeline->uniformBlocks |= UB_VS_BONES;
		}
	}
	return vulkanPipeline;
}

VulkanPipeline *PipelineManagerVulkan::GetOrCreatePipeline(VkPipelineLayout layout, VkRenderPass renderPass, const VulkanPipelineRasterStateKey &rasterKey, const DecVtxFormat *decFmt, VulkanVertexShader *vs, VulkanFragmentShader *fs, bool useHwTransform) {
	VulkanPipelineKey key{};
	_assert_msg_(G3D, renderPass, "Can't create a pipeline with a null renderpass");

	key.raster = rasterKey;
	key.renderPass = renderPass;
	key.useHWTransform = useHwTransform;
	key.vShader = vs->GetModule();
	key.fShader = fs->GetModule();
	key.vtxDecId = useHwTransform ? decFmt->id : 0;

	auto iter = pipelines_.Get(key);
	if (iter)
		return iter;

	PROFILE_THIS_SCOPE("pipelinebuild");

	VulkanPipeline *pipeline = CreateVulkanPipeline(
		vulkan_->GetDevice(), pipelineCache_, layout, renderPass, 
		rasterKey, decFmt, vs, fs, useHwTransform, lineWidth_);
	// Even if the result is nullptr, insert it so we don't try to create it repeatedly.
	pipelines_.Insert(key, pipeline);
	return pipeline;
}

std::vector<std::string> PipelineManagerVulkan::DebugGetObjectIDs(DebugShaderType type) {
	std::vector<std::string> ids;
	switch (type) {
	case SHADER_TYPE_PIPELINE:
	{
		pipelines_.Iterate([&](const VulkanPipelineKey &key, VulkanPipeline *value) {
			std::string id;
			key.ToString(&id);
			ids.push_back(id);
		});
	}
	break;
	default:
		break;
	}
	return ids;
}

static const char *const topologies[8] = {
	"POINTLIST",
	"LINELIST",
	"LINESTRIP",
	"TRILIST",
	"TRISTRIP",
	"TRIFAN",
};

static const char *const blendOps[8] = {
	"ADD",
	"SUB",
	"REVSUB",
	"MIN",
	"MAX",
};

static const char *const compareOps[8] = {
	"NEVER",
	"<",
	"==",
	"<=",
	">",
	">=",
	"!=",
	"ALWAYS",
};

static const char *const stencilOps[8] = {
	"KEEP",
	"ZERO",
	"REPLACE",
	"INC_CLAMP",
	"DEC_CLAMP",
	"INVERT",
	"INC_WRAP",
	"DEC_WRAP",
};

static const char *const blendFactors[19] = {
	"ZERO",
	"ONE",
	"SRC_COLOR",
	"ONE_MINUS_SRC_COLOR",
	"DST_COLOR",
	"ONE_MINUS_DST_COLOR",
	"SRC_ALPHA",
	"ONE_MINUS_SRC_ALPHA",
	"DST_ALPHA",
	"ONE_MINUS_DST_ALPHA",
	"CONSTANT_COLOR",
	"ONE_MINUS_CONSTANT_COLOR",
	"CONSTANT_ALPHA",
	"ONE_MINUS_CONSTANT_ALPHA",
	"SRC_ALPHA_SATURATE",
	"SRC1_COLOR",
	"ONE_MINUS_SRC1_COLOR",
	"SRC1_ALPHA",
	"ONE_MINUS_SRC1_ALPHA",
};

std::string PipelineManagerVulkan::DebugGetObjectString(std::string id, DebugShaderType type, DebugShaderStringType stringType) {
	if (type != SHADER_TYPE_PIPELINE)
		return "N/A";

	VulkanPipelineKey pipelineKey;
	pipelineKey.FromString(id);

	VulkanPipeline *iter = pipelines_.Get(pipelineKey);
	if (!iter) {
		return "";
	}

	switch (stringType) {
	case SHADER_STRING_SHORT_DESC:
	{
		std::stringstream str;
		str << topologies[pipelineKey.raster.topology] << " ";
		if (pipelineKey.raster.blendEnable) {
			str << "Blend(";
			str << "C:" << blendOps[pipelineKey.raster.blendOpColor] << "/"
				<< blendFactors[pipelineKey.raster.srcColor] << ":" << blendFactors[pipelineKey.raster.destColor] << " ";
			if (pipelineKey.raster.blendOpAlpha != VK_BLEND_OP_ADD ||
				pipelineKey.raster.srcAlpha != VK_BLEND_FACTOR_ONE ||
				pipelineKey.raster.destAlpha != VK_BLEND_FACTOR_ZERO) {
				str << "A:" << blendOps[pipelineKey.raster.blendOpAlpha] << "/"
					<< blendFactors[pipelineKey.raster.srcColor] << ":" << blendFactors[pipelineKey.raster.destColor] << " ";
			}
			str << ") ";
		}
		if (pipelineKey.raster.colorWriteMask != 0xF) {
			str << "Mask(";
			for (int i = 0; i < 4; i++) {
				if (pipelineKey.raster.colorWriteMask & (1 << i)) {
					str << "RGBA"[i];
				} else {
					str << "_";
				}
			}
			str << ") ";
		}
		if (pipelineKey.raster.depthTestEnable) {
			str << "Depth(";
			if (pipelineKey.raster.depthWriteEnable)
				str << "W, ";
			if (pipelineKey.raster.depthCompareOp)
				str << compareOps[pipelineKey.raster.depthCompareOp & 7];
			str << ") ";
		}
		if (pipelineKey.raster.stencilTestEnable) {
			str << "Stencil(";
			str << compareOps[pipelineKey.raster.stencilCompareOp & 7] << " ";
			str << stencilOps[pipelineKey.raster.stencilPassOp & 7] << "/";
			str << stencilOps[pipelineKey.raster.stencilFailOp & 7] << "/";
			str << stencilOps[pipelineKey.raster.stencilDepthFailOp& 7];
			str << ") ";
		}
		if (pipelineKey.raster.logicOpEnable) {
			str << "Logic(";
			str << ") ";
		}
		if (pipelineKey.useHWTransform) {
			str << "HWX ";
		}
		if (pipelineKey.vtxDecId) {
			str << "V(";
			str << StringFromFormat("%08x", pipelineKey.vtxDecId);  // TODO: Format nicer.
			str << ") ";
		} else {
			str << "SWX ";
		}
		return StringFromFormat("%p: %s", iter, str.str().c_str());
	}

	case SHADER_STRING_SOURCE_CODE:
	{
		return "N/A";
	}
	default:
		return "N/A";
	}
}

void PipelineManagerVulkan::SetLineWidth(float lineWidth) {
	if (lineWidth_ == lineWidth)
		return;
	lineWidth_ = lineWidth;

	// Wipe all line-drawing pipelines.
	pipelines_.Iterate([&](const VulkanPipelineKey &key, VulkanPipeline *value) {
		if (value->usesLines) {
			vulkan_->Delete().QueueDeletePipeline(value->pipeline);
			delete value;
			pipelines_.Remove(key);
		}
	});
}
