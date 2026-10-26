#include "vk_device.h"

#include "gt/shaders.h"

#include <chrono>

namespace gt::rhi::vk {
namespace {

VkShaderStageFlags toVkStages(ShaderStage s) {
    VkShaderStageFlags f = 0;
    if (any(s & ShaderStage::Vertex)) f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (any(s & ShaderStage::Hull)) f |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (any(s & ShaderStage::Domain)) f |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (any(s & ShaderStage::Geometry)) f |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (any(s & ShaderStage::Fragment)) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (any(s & ShaderStage::Compute)) f |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (any(s & ShaderStage::Task)) f |= VK_SHADER_STAGE_TASK_BIT_EXT;
    if (any(s & ShaderStage::Mesh)) f |= VK_SHADER_STAGE_MESH_BIT_EXT;
    return f;
}

VkBlendFactor toVk(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InvDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InvDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp toVk(BlendOp op) {
    switch (op) {
    case BlendOp::Add: return VK_BLEND_OP_ADD;
    case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min: return VK_BLEND_OP_MIN;
    case BlendOp::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkPrimitiveTopology toVk(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PrimitiveTopology::PatchList: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

VkShaderModule VulkanDevice::createShaderModule(const ShaderRef& ref) {
    std::span<const std::uint8_t> code = ref.bytecode;
    if (code.empty()) {
        auto blob = shaders::find(ref.module, ref.entry, shaders::Format::Spirv);
        if (!blob) throw Error(std::format("SPIR-V for shader {}:{} is not embedded in this build", ref.module, ref.entry));
        code = blob.bytes();
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(m_device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

void VulkanPipeline::createLayout(const LayoutDesc& layout) {
    m_layoutDesc = layout;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const Binding& b : layout.bindings) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = bindingIndex(b.type, b.slot);
        lb.descriptorType = descriptorType(b.type);
        lb.descriptorCount = 1;
        lb.stageFlags = m_bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE
                            ? static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_COMPUTE_BIT)
                            : toVkStages(b.stages) & ~static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_COMPUTE_BIT);
        if (lb.stageFlags == 0) lb.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        bindings.push_back(lb);
    }
    VkDescriptorSetLayoutCreateInfo sci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (m_dev.pushDescriptors()) sci.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sci.bindingCount = static_cast<std::uint32_t>(bindings.size());
    sci.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(m_dev.vkDevice(), &sci, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout");

    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, layout.pushConstantBytes};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &m_setLayout;
    lci.pushConstantRangeCount = layout.pushConstantBytes ? 1 : 0;
    lci.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(m_dev.vkDevice(), &lci, nullptr, &m_layout), "vkCreatePipelineLayout");
}

VulkanPipeline::VulkanPipeline(VulkanDevice& dev, const GraphicsPipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = false;
    m_bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    createLayout(d.layout);

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkShaderModule> modules;
    auto addStage = [&](const ShaderRef& ref, VkShaderStageFlagBits stage) {
        if (!ref.valid()) return;
        VkShaderModule m = dev.createShaderModule(ref);
        modules.push_back(m);
        VkPipelineShaderStageCreateInfo s{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        s.stage = stage;
        s.module = m;
        s.pName = "main";
        stages.push_back(s);
    };
    addStage(d.vertex, VK_SHADER_STAGE_VERTEX_BIT);
    addStage(d.hull, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
    addStage(d.domain, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
    addStage(d.geometry, VK_SHADER_STAGE_GEOMETRY_BIT);
    addStage(d.fragment, VK_SHADER_STAGE_FRAGMENT_BIT);

    std::vector<VkVertexInputBindingDescription> vbs;
    for (const auto& b : d.vertexBindings)
        vbs.push_back({b.binding, b.stride, b.perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    std::vector<VkVertexInputAttributeDescription> vas;
    for (const auto& a : d.attributes) vas.push_back({a.location, a.binding, toVk(a.format), a.offset});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(vbs.size());
    vi.pVertexBindingDescriptions = vbs.data();
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vas.size());
    vi.pVertexAttributeDescriptions = vas.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = toVk(d.topology);
    VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    ts.patchControlPoints = d.patchControlPoints;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = d.raster.wireframe && dev.features().wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = d.raster.cull == CullMode::None    ? VK_CULL_MODE_NONE
                  : d.raster.cull == CullMode::Front ? VK_CULL_MODE_FRONT_BIT
                                                     : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = d.raster.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthClampEnable = d.raster.depthClamp && dev.enabledFeatures().core.features.depthClamp;
    rs.depthBiasEnable = d.raster.depthBias != 0.0f || d.raster.depthBiasSlope != 0.0f;
    rs.depthBiasConstantFactor = d.raster.depthBias;
    rs.depthBiasSlopeFactor = d.raster.depthBiasSlope;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = toVkSamples(d.sampleCount);

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (d.depthFormat != Format::Unknown) {
        ds.depthTestEnable = d.depth.test;
        ds.depthWriteEnable = d.depth.write;
        ds.depthCompareOp = toVk(d.depth.compare);
    }

    std::vector<VkPipelineColorBlendAttachmentState> blends;
    for (std::size_t i = 0; i < d.colorFormats.size(); ++i) {
        BlendAttachment b = i < d.blend.size() ? d.blend[i] : BlendAttachment{};
        VkPipelineColorBlendAttachmentState s{};
        s.blendEnable = b.enable;
        s.srcColorBlendFactor = toVk(b.srcColor);
        s.dstColorBlendFactor = toVk(b.dstColor);
        s.colorBlendOp = toVk(b.colorOp);
        s.srcAlphaBlendFactor = toVk(b.srcAlpha);
        s.dstAlphaBlendFactor = toVk(b.dstAlpha);
        s.alphaBlendOp = toVk(b.alphaOp);
        s.colorWriteMask = b.writeMask;
        blends.push_back(s);
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<std::uint32_t>(blends.size());
    cb.pAttachments = blends.data();

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = static_cast<std::uint32_t>(stages.size());
    ci.pStages = stages.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pTessellationState = d.topology == PrimitiveTopology::PatchList ? &ts : nullptr;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = m_layout;
    ci.renderPass = dev.compatibleRenderPass(d.colorFormats, d.depthFormat, d.sampleCount);
    VkResult r = vkCreateGraphicsPipelines(dev.vkDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
    for (VkShaderModule m : modules) vkDestroyShaderModule(dev.vkDevice(), m, nullptr);
    if (r != VK_SUCCESS) throwResult(r, std::format("vkCreateGraphicsPipelines({})", d.name).c_str());
    dev.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(m_pipeline), d.name);
    m_creationMs = msSince(t0);
}

VulkanPipeline::VulkanPipeline(VulkanDevice& dev, const ComputePipelineDesc& d) : m_dev(dev), m_name(d.name) {
    const auto t0 = std::chrono::steady_clock::now();
    m_isCompute = true;
    m_bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    createLayout(d.layout);
    VkShaderModule m = dev.createShaderModule(d.compute);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = m_layout;
    VkResult r = vkCreateComputePipelines(dev.vkDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
    vkDestroyShaderModule(dev.vkDevice(), m, nullptr);
    if (r != VK_SUCCESS) throwResult(r, std::format("vkCreateComputePipelines({})", d.name).c_str());
    dev.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(m_pipeline), d.name);
    m_creationMs = msSince(t0);
}

VulkanPipeline::~VulkanPipeline() {
    VkDevice d = m_dev.vkDevice();
    VkPipeline p = m_pipeline;
    VkPipelineLayout l = m_layout;
    VkDescriptorSetLayout s = m_setLayout;
    m_dev.deferDestroy([d, p, l, s] {
        if (p) vkDestroyPipeline(d, p, nullptr);
        if (l) vkDestroyPipelineLayout(d, l, nullptr);
        if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
    });
}

} // namespace gt::rhi::vk
