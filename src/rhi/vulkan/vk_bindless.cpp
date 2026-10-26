// Texture tables (descriptor indexing): one partially bound, variable-count array of
// sampled images per table, bound as descriptor set 1 next to the per-draw set 0.

#include "vk_device.h"

namespace gt::rhi::vk {

VkDescriptorSetLayout VulkanDevice::textureTableLayout() {
    if (m_tableLayout) return m_tableLayout;
    if (!m_features.maxBindlessTextures) throw Error("texture tables (descriptor indexing) are not supported by this GPU");
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b.descriptorCount = m_features.maxBindlessTextures;
    b.stageFlags = VK_SHADER_STAGE_ALL;
    VkDescriptorBindingFlags flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    if (m_tableUpdateAfterBind) flags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fi.bindingCount = 1;
    fi.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.pNext = &fi;
    if (m_tableUpdateAfterBind) ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci.bindingCount = 1;
    ci.pBindings = &b;
    check(vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &m_tableLayout), "vkCreateDescriptorSetLayout(texture table)");
    return m_tableLayout;
}

class VulkanTextureTable final : public TextureTable {
public:
    VulkanTextureTable(VulkanDevice& dev, std::uint32_t capacity, bool updateAfterBind) : m_dev(dev) {
        m_capacity = capacity;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        if (updateAfterBind) pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        check(vkCreateDescriptorPool(dev.vkDevice(), &pci, nullptr, &m_pool), "vkCreateDescriptorPool(texture table)");
        VkDescriptorSetVariableDescriptorCountAllocateInfo vi{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vi.descriptorSetCount = 1;
        vi.pDescriptorCounts = &capacity;
        VkDescriptorSetLayout layout = dev.textureTableLayout();
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.pNext = &vi;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        check(vkAllocateDescriptorSets(dev.vkDevice(), &ai, &m_set), "vkAllocateDescriptorSets(texture table)");
    }
    ~VulkanTextureTable() override {
        VkDevice d = m_dev.vkDevice();
        VkDescriptorPool p = m_pool;
        m_dev.deferDestroy([d, p] { vkDestroyDescriptorPool(d, p, nullptr); });
    }

    void set(std::uint32_t index, Texture* texture) override {
        if (index >= m_capacity) throw Error("TextureTable::set: index out of range");
        auto* t = static_cast<VulkanTexture*>(texture);
        const VkDescriptorImageInfo info{VK_NULL_HANDLE, t->view({}), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_set;
        w.dstBinding = 0;
        w.dstArrayElement = index;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(m_dev.vkDevice(), 1, &w, 0, nullptr);
    }

    VkDescriptorSet handle() const { return m_set; }

private:
    VulkanDevice& m_dev;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
};

std::unique_ptr<TextureTable> VulkanDevice::createTextureTable(std::uint32_t capacity) {
    if (capacity == 0 || capacity > m_features.maxBindlessTextures)
        throw Error(std::format("texture table of {} entries: this GPU allows up to {}", capacity,
                                m_features.maxBindlessTextures));
    return std::make_unique<VulkanTextureTable>(*this, capacity, m_tableUpdateAfterBind);
}

void VulkanCommandList::setTextureTable(TextureTable* table) {
    m_tableSet = static_cast<VulkanTextureTable*>(table)->handle();
    m_tableDirty = true;
}

} // namespace gt::rhi::vk
