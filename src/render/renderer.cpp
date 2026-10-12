// Vulkan 1.3 renderer (docs/adr/0005). volk loads the API, vk-bootstrap creates the instance,
// device and swapchain, VMA manages memory. Uploads are recorded into the next frame's command
// buffer; buffers retired by a frame are freed once that frame's fence has signalled.

#include <bridger/render/renderer.hpp>

#include "external_image.hpp"

#include <volk.h>

#include <VkBootstrap.h>
#include <vma/vk_mem_alloc.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace bridger::embedded {
extern const unsigned char shader_chunk_vert[];
extern const std::size_t shader_chunk_vert_size;
extern const unsigned char shader_chunk_frag[];
extern const std::size_t shader_chunk_frag_size;
extern const unsigned char shader_line_vert[];
extern const std::size_t shader_line_vert_size;
extern const unsigned char shader_line_frag[];
extern const std::size_t shader_line_frag_size;
} // namespace bridger::embedded

namespace bridger::render {
namespace {

constexpr u32 kFramesInFlight = 2;
constexpr u32 kMaxQuadsPerDraw = 1u << 18;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kSharedFormat = VK_FORMAT_B8G8R8A8_UNORM; // DXGI_FORMAT_B8G8R8A8_UNORM
constexpr u32 kMipLevels = 5; // 16, 8, 4, 2, 1

Status vk_check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return {};
    return make_error(ErrorCode::IoError, std::string("Vulkan: ") + what + " failed (" + std::to_string(static_cast<int>(r)) + ")");
}

template <typename T>
Status vkb_check(const vkb::Result<T>& r, const char* what) {
    if (r) return {};
    return make_error(ErrorCode::IoError, std::string("Vulkan: ") + what + ": " + r.error().message());
}

struct PushConstants {
    glm::mat4 view_projection;
    glm::vec4 origin;
    glm::vec4 fog;
    glm::vec4 sky;
    glm::vec4 camera;
};
static_assert(sizeof(PushConstants) == 128);

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    u32 width = 0, height = 0;
};

struct GpuSection {
    glm::ivec3 origin{0};
    Buffer vertices;
    u32 opaque_quads = 0;      // solid then cutout
    u32 translucent_quads = 0; // after them
};

struct PendingCopy {
    Buffer staging;
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<Buffer> garbage;
    Buffer lines; // host-visible outline vertices
};

u64 section_key(glm::ivec3 o) {
    return (static_cast<u64>(static_cast<u32>(o.x >> 4) & 0x3FFFFF) << 42) | (static_cast<u64>(static_cast<u32>(o.y >> 4) & 0xFFFFF) << 22) |
           (static_cast<u64>(static_cast<u32>(o.z >> 4) & 0x3FFFFF));
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access, u32 mips = 1, u32 layers = 1) {
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
    b.subresourceRange = {aspect, 0, mips, 0, layers};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// Frustum planes (camera-relative) from a view-projection matrix with depth 0..1.
std::array<glm::vec4, 6> frustum_planes(const glm::mat4& m) {
    const glm::mat4 t = glm::transpose(m);
    std::array<glm::vec4, 6> p{t[3] + t[0], t[3] - t[0], t[3] + t[1], t[3] - t[1], t[2], t[3] - t[2]};
    for (glm::vec4& v : p) v /= glm::length(glm::vec3(v));
    return p;
}

bool box_visible(const std::array<glm::vec4, 6>& planes, const glm::vec3& lo, const glm::vec3& hi) {
    for (const glm::vec4& p : planes) {
        const glm::vec3 v(p.x >= 0 ? hi.x : lo.x, p.y >= 0 ? hi.y : lo.y, p.z >= 0 ? hi.z : lo.z);
        if (glm::dot(glm::vec3(p), v) + p.w < 0) return false;
    }
    return true;
}

} // namespace

struct Renderer::Impl {
    RendererConfig config;
    vkb::Instance instance;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vkb::PhysicalDevice physical;
    vkb::Device device;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator vma = nullptr;
    bool offscreen = true;
    bool shared = false; // color = imported D3D11 texture
    VkDeviceMemory shared_memory = VK_NULL_HANDLE;

    vkb::Swapchain swapchain;
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    std::vector<VkSemaphore> render_done; // per swapchain image
    VkFormat color_format = kOffscreenFormat;
    Image color; // offscreen target
    Image depth;
    Buffer readback;
    u32 width = 0, height = 0;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    Image textures;
    u32 texture_layers = 0;

    VkPipelineLayout chunk_layout = VK_NULL_HANDLE, line_layout = VK_NULL_HANDLE;
    VkPipeline opaque_pipeline = VK_NULL_HANDLE, translucent_pipeline = VK_NULL_HANDLE, line_pipeline = VK_NULL_HANDLE;
    Buffer quad_indices;

    std::unordered_map<u64, GpuSection> sections;
    std::vector<PendingCopy> pending;
    std::vector<OutlineBox> outlines;
    std::array<Frame, kFramesInFlight> frames{};
    u32 frame = 0;
    bool readback_ready = false;

    ~Impl() { destroy(); }

    VkDevice dev() const { return device.device; }

    Result<Buffer> make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<VkDeviceSize>(size, 4);
        bi.usage = usage;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        if (host) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        Buffer b;
        VmaAllocationInfo info{};
        BRIDGER_TRY_STATUS(vk_check(vmaCreateBuffer(vma, &bi, &ai, &b.buffer, &b.alloc, &info), "vmaCreateBuffer"));
        b.size = bi.size;
        b.mapped = info.pMappedData;
        return b;
    }

    void free_buffer(Buffer& b) {
        if (b.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vma, b.buffer, b.alloc);
        b = {};
    }

    Result<Image> make_image(u32 w, u32 h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, u32 mips = 1,
                             u32 layers = 1) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {w, h, 1};
        ii.mipLevels = mips;
        ii.arrayLayers = layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        Image img;
        BRIDGER_TRY_STATUS(vk_check(vmaCreateImage(vma, &ii, &ai, &img.image, &img.alloc, nullptr), "vmaCreateImage"));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, mips, 0, layers};
        BRIDGER_TRY_STATUS(vk_check(vkCreateImageView(dev(), &vi, nullptr, &img.view), "vkCreateImageView"));
        img.width = w;
        img.height = h;
        return img;
    }

    void free_image(Image& img) {
        if (img.view != VK_NULL_HANDLE) vkDestroyImageView(dev(), img.view, nullptr);
        if (img.image != VK_NULL_HANDLE) vmaDestroyImage(vma, img.image, img.alloc);
        img = {};
    }

    // Runs `record` in a one-time command buffer and waits for it.
    Status immediate(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = device.get_queue_index(vkb::QueueType::graphics).value();
        VkCommandPool pool = VK_NULL_HANDLE;
        BRIDGER_TRY_STATUS(vk_check(vkCreateCommandPool(dev(), &pi, nullptr, &pool), "vkCreateCommandPool"));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(dev(), &ai, &cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        Status st = vk_check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
        vkQueueWaitIdle(queue);
        vkDestroyCommandPool(dev(), pool, nullptr);
        return st;
    }

    Result<VkShaderModule> shader(const unsigned char* code, std::size_t size) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        std::vector<u32> words((size + 3) / 4);
        std::memcpy(words.data(), code, size);
        ci.codeSize = size;
        ci.pCode = words.data();
        VkShaderModule m = VK_NULL_HANDLE;
        BRIDGER_TRY_STATUS(vk_check(vkCreateShaderModule(dev(), &ci, nullptr, &m), "vkCreateShaderModule"));
        return m;
    }

    Result<VkPipeline> make_pipeline(VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, bool lines, bool blend,
                                     bool depth_write) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

        VkVertexInputBindingDescription binding{0, lines ? 28u : static_cast<u32>(sizeof(Vertex)), VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attrs;
        if (lines) {
            attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}};
        } else {
            attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                     {1, 0, VK_FORMAT_R16G16_UNORM, 12},
                     {2, 0, VK_FORMAT_R32_UINT, 16},
                     {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 20}};
        }
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = static_cast<u32>(attrs.size());
        vin.pVertexAttributeDescriptions = attrs.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // Quads are counter-clockwise around their outward normal. The projection's Y flip and
        // Vulkan's downward framebuffer Y cancel out, so they stay counter-clockwise on screen.
        rs.cullMode = lines || blend ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState cb{};
        cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (blend) {
            cb.blendEnable = VK_TRUE;
            cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cb;
        const std::array<VkDynamicState, 2> dyn{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = static_cast<u32>(dyn.size());
        dy.pDynamicStates = dyn.data();
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &color_format;
        ri.depthAttachmentFormat = kDepthFormat;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &ri;
        pi.stageCount = 2;
        pi.pStages = stages.data();
        pi.pVertexInputState = &vin;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cbs;
        pi.pDynamicState = &dy;
        pi.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        BRIDGER_TRY_STATUS(vk_check(vkCreateGraphicsPipelines(dev(), VK_NULL_HANDLE, 1, &pi, nullptr, &p), "vkCreateGraphicsPipelines"));
        return p;
    }

    Status create_targets(u32 w, u32 h) {
        width = std::max(1u, w);
        height = std::max(1u, h);
        if (shared) {
            color_format = kSharedFormat; // the color image comes from set_shared_target()
        } else if (offscreen) {
            BRIDGER_TRY(color, make_image(width, height, kOffscreenFormat,
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT));
            BRIDGER_TRY(readback, make_buffer(static_cast<VkDeviceSize>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
        } else {
            vkb::SwapchainBuilder sb(device, surface);
            sb.set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                .add_fallback_format({VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                .set_desired_present_mode(config.vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR)
                .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                .set_desired_extent(width, height)
                .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            if (swapchain.swapchain != VK_NULL_HANDLE) sb.set_old_swapchain(swapchain);
            auto sc = sb.build();
            BRIDGER_TRY_STATUS(vkb_check(sc, "swapchain"));
            destroy_swapchain();
            swapchain = sc.value();
            color_format = swapchain.image_format;
            swap_images = swapchain.get_images().value();
            swap_views = swapchain.get_image_views().value();
            width = swapchain.extent.width;
            height = swapchain.extent.height;
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            render_done.resize(swap_images.size());
            for (VkSemaphore& s : render_done) vkCreateSemaphore(dev(), &si, nullptr, &s);
        }
        BRIDGER_TRY(depth, make_image(width, height, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
        return {};
    }

    void destroy_swapchain() {
        for (VkSemaphore s : render_done) vkDestroySemaphore(dev(), s, nullptr);
        render_done.clear();
        if (swapchain.swapchain != VK_NULL_HANDLE) {
            swapchain.destroy_image_views(swap_views);
            vkb::destroy_swapchain(swapchain);
            swapchain = {};
        }
        swap_views.clear();
        swap_images.clear();
    }

    void free_shared_color() {
        if (color.view != VK_NULL_HANDLE) vkDestroyImageView(dev(), color.view, nullptr);
        if (color.image != VK_NULL_HANDLE) vkDestroyImage(dev(), color.image, nullptr);
        if (shared_memory != VK_NULL_HANDLE) vkFreeMemory(dev(), shared_memory, nullptr);
        shared_memory = VK_NULL_HANDLE;
        color = {};
    }

    Status set_shared_target(void* handle, u32 w, u32 h) {
        if (!shared) return make_error(ErrorCode::InvalidArgument, "set_shared_target needs a renderer created with shared_target");
        vkDeviceWaitIdle(dev());
        free_shared_color();
        free_image(depth);
        width = std::max(1u, w);
        height = std::max(1u, h);
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = detail::kSharedTextureHandleType;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.pNext = &ext;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = kSharedFormat;
        ii.extent = {width, height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        BRIDGER_TRY_STATUS(vk_check(vkCreateImage(dev(), &ii, nullptr, &color.image), "vkCreateImage (shared)"));
        auto memory = detail::import_shared_texture(dev(), physical.physical_device, color.image, handle);
        if (!memory) {
            free_shared_color();
            return tl::unexpected(memory.error());
        }
        shared_memory = *memory;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = color.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kSharedFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        BRIDGER_TRY_STATUS(vk_check(vkCreateImageView(dev(), &vi, nullptr, &color.view), "vkCreateImageView (shared)"));
        color.width = width;
        color.height = height;
        BRIDGER_TRY(depth, make_image(width, height, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));
        return {};
    }

    std::optional<u64> adapter_luid() const {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &id;
        vkGetPhysicalDeviceProperties2(physical.physical_device, &p2);
        if (id.deviceLUIDValid != VK_TRUE) return std::nullopt;
        u64 luid = 0;
        std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        return luid;
    }

    void destroy_targets() {
        if (shared) free_shared_color();
        else free_image(color);
        free_image(depth);
        free_buffer(readback);
    }

    void destroy() {
        if (device.device == VK_NULL_HANDLE) {
            if (instance.instance != VK_NULL_HANDLE) {
                if (surface != VK_NULL_HANDLE) vkb::destroy_surface(instance, surface);
                vkb::destroy_instance(instance);
            }
            return;
        }
        vkDeviceWaitIdle(dev());
        for (auto& [k, s] : sections) free_buffer(s.vertices);
        sections.clear();
        for (PendingCopy& p : pending) free_buffer(p.staging);
        pending.clear();
        for (Frame& f : frames) {
            for (Buffer& b : f.garbage) free_buffer(b);
            free_buffer(f.lines);
            if (f.fence) vkDestroyFence(dev(), f.fence, nullptr);
            if (f.image_available) vkDestroySemaphore(dev(), f.image_available, nullptr);
            if (f.pool) vkDestroyCommandPool(dev(), f.pool, nullptr);
        }
        free_buffer(quad_indices);
        free_image(textures);
        destroy_targets();
        destroy_swapchain();
        for (VkPipeline p : {opaque_pipeline, translucent_pipeline, line_pipeline})
            if (p) vkDestroyPipeline(dev(), p, nullptr);
        for (VkPipelineLayout l : {chunk_layout, line_layout})
            if (l) vkDestroyPipelineLayout(dev(), l, nullptr);
        if (sampler) vkDestroySampler(dev(), sampler, nullptr);
        if (descriptor_pool) vkDestroyDescriptorPool(dev(), descriptor_pool, nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(dev(), set_layout, nullptr);
        if (vma) vmaDestroyAllocator(vma);
        vkb::destroy_device(device);
        if (surface != VK_NULL_HANDLE) vkb::destroy_surface(instance, surface);
        vkb::destroy_instance(instance);
        device = {};
        instance = {};
    }

    Status init(const RendererConfig& cfg) {
        config = cfg;
        shared = cfg.shared_target;
        offscreen = !cfg.make_surface && !shared;
        BRIDGER_TRY_STATUS(vk_check(volkInitialize(), "loading the Vulkan driver (vulkan-1.dll)"));

        vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
        ib.set_app_name("Bridger").set_engine_name("Bridger").require_api_version(1, 3, 0);
        if (!cfg.make_surface) ib.set_headless(true);
        for (const std::string& e : cfg.instance_extensions) ib.enable_extension(e.c_str());
        if (cfg.validation) ib.request_validation_layers(true).use_default_debug_messenger();
        auto inst = ib.build();
        BRIDGER_TRY_STATUS(vkb_check(inst, "instance (Vulkan 1.3 is required)"));
        instance = inst.value();
        volkLoadInstance(instance.instance);

        if (cfg.make_surface) surface = reinterpret_cast<VkSurfaceKHR>(cfg.make_surface(instance.instance));

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        vkb::PhysicalDeviceSelector sel = !cfg.make_surface ? vkb::PhysicalDeviceSelector(instance) : vkb::PhysicalDeviceSelector(instance, surface);
        sel.set_minimum_version(1, 3).set_required_features_13(f13).prefer_gpu_device_type(vkb::PreferredDeviceType::discrete);
        if (!cfg.make_surface) sel.defer_surface_initialization();
        if (shared) sel.add_required_extension(detail::kExternalMemoryExtension);
        auto phys = sel.select();
        BRIDGER_TRY_STATUS(vkb_check(phys, "selecting a GPU"));
        physical = phys.value();
        auto devr = vkb::DeviceBuilder(physical).build();
        BRIDGER_TRY_STATUS(vkb_check(devr, "device"));
        device = devr.value();
        volkLoadDevice(device.device);
        auto q = device.get_queue(vkb::QueueType::graphics);
        BRIDGER_TRY_STATUS(vkb_check(q, "graphics queue"));
        queue = q.value();

        VmaAllocatorCreateInfo ai{};
        ai.physicalDevice = physical.physical_device;
        ai.device = device.device;
        ai.instance = instance.instance;
        ai.vulkanApiVersion = VK_API_VERSION_1_3;
        VmaVulkanFunctions funcs{};
        BRIDGER_TRY_STATUS(vk_check(vmaImportVulkanFunctionsFromVolk(&ai, &funcs), "vmaImportVulkanFunctionsFromVolk"));
        ai.pVulkanFunctions = &funcs;
        BRIDGER_TRY_STATUS(vk_check(vmaCreateAllocator(&ai, &vma), "vmaCreateAllocator"));

        BRIDGER_TRY_STATUS(create_targets(cfg.width, cfg.height));

        // Frames.
        for (Frame& f : frames) {
            VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pi.queueFamilyIndex = device.get_queue_index(vkb::QueueType::graphics).value();
            BRIDGER_TRY_STATUS(vk_check(vkCreateCommandPool(dev(), &pi, nullptr, &f.pool), "vkCreateCommandPool"));
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = f.pool;
            cai.commandBufferCount = 1;
            vkAllocateCommandBuffers(dev(), &cai, &f.cmd);
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(dev(), &si, nullptr, &f.image_available);
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(dev(), &fi, nullptr, &f.fence);
        }

        // Descriptors: one combined image sampler (the block texture array).
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        BRIDGER_TRY_STATUS(vk_check(vkCreateDescriptorSetLayout(dev(), &li, nullptr, &set_layout), "descriptor set layout"));
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        BRIDGER_TRY_STATUS(vk_check(vkCreateDescriptorPool(dev(), &pci, nullptr, &descriptor_pool), "descriptor pool"));
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = descriptor_pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &set_layout;
        BRIDGER_TRY_STATUS(vk_check(vkAllocateDescriptorSets(dev(), &dai, &descriptor_set), "descriptor set"));
        VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        smp.magFilter = VK_FILTER_NEAREST;
        smp.minFilter = VK_FILTER_NEAREST;
        smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        smp.maxLod = static_cast<float>(kMipLevels);
        BRIDGER_TRY_STATUS(vk_check(vkCreateSampler(dev(), &smp, nullptr, &sampler), "sampler"));

        // Pipelines.
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        BRIDGER_TRY_STATUS(vk_check(vkCreatePipelineLayout(dev(), &pli, nullptr, &chunk_layout), "pipeline layout"));
        VkPushConstantRange lpr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
        VkPipelineLayoutCreateInfo lli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lli.pushConstantRangeCount = 1;
        lli.pPushConstantRanges = &lpr;
        BRIDGER_TRY_STATUS(vk_check(vkCreatePipelineLayout(dev(), &lli, nullptr, &line_layout), "pipeline layout"));
        BRIDGER_TRY(VkShaderModule cv, shader(embedded::shader_chunk_vert, embedded::shader_chunk_vert_size));
        BRIDGER_TRY(VkShaderModule cf, shader(embedded::shader_chunk_frag, embedded::shader_chunk_frag_size));
        BRIDGER_TRY(VkShaderModule lv, shader(embedded::shader_line_vert, embedded::shader_line_vert_size));
        BRIDGER_TRY(VkShaderModule lf, shader(embedded::shader_line_frag, embedded::shader_line_frag_size));
        auto op = make_pipeline(chunk_layout, cv, cf, false, false, true);
        auto tp = make_pipeline(chunk_layout, cv, cf, false, true, false);
        auto lp = make_pipeline(line_layout, lv, lf, true, false, true);
        for (VkShaderModule m : {cv, cf, lv, lf}) vkDestroyShaderModule(dev(), m, nullptr);
        BRIDGER_TRY(opaque_pipeline, std::move(op));
        BRIDGER_TRY(translucent_pipeline, std::move(tp));
        BRIDGER_TRY(line_pipeline, std::move(lp));

        // Shared index buffer: quads as two triangles (0 1 2, 0 2 3).
        std::vector<u32> idx(static_cast<usize>(kMaxQuadsPerDraw) * 6);
        for (u32 quad = 0; quad < kMaxQuadsPerDraw; ++quad) {
            const u32 v = quad * 4;
            const usize i = static_cast<usize>(quad) * 6;
            idx[i] = v;
            idx[i + 1] = v + 1;
            idx[i + 2] = v + 2;
            idx[i + 3] = v;
            idx[i + 4] = v + 2;
            idx[i + 5] = v + 3;
        }
        BRIDGER_TRY(quad_indices, make_buffer(idx.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false));
        BRIDGER_TRY(Buffer staging, make_buffer(idx.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
        std::memcpy(staging.mapped, idx.data(), idx.size() * 4);
        const Status st = immediate([&](VkCommandBuffer cmd) {
            VkBufferCopy c{0, 0, idx.size() * 4};
            vkCmdCopyBuffer(cmd, staging.buffer, quad_indices.buffer, 1, &c);
        });
        free_buffer(staging);
        return st;
    }

    Status upload_textures(std::span<const u32> rgba, u32 layers) {
        if (layers == 0 || rgba.size() < static_cast<usize>(layers) * 256) return make_error(ErrorCode::InvalidArgument, "texture data too short");
        vkDeviceWaitIdle(dev());
        free_image(textures);
        BRIDGER_TRY(textures, make_image(16, 16, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                         VK_IMAGE_ASPECT_COLOR_BIT, kMipLevels, layers));
        texture_layers = layers;
        // All mip levels, CPU box filter (alpha-weighted colour so cutout edges do not darken).
        std::vector<u8> data;
        std::vector<VkBufferImageCopy> copies;
        std::vector<u32> level(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(layers) * 256);
        u32 size = 16;
        for (u32 mip = 0; mip < kMipLevels; ++mip) {
            VkBufferImageCopy c{};
            c.bufferOffset = data.size();
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers};
            c.imageExtent = {size, size, 1};
            copies.push_back(c);
            const auto* bytes = reinterpret_cast<const u8*>(level.data());
            data.insert(data.end(), bytes, bytes + level.size() * 4);
            if (size == 1) break;
            const u32 half = size / 2;
            std::vector<u32> next(static_cast<usize>(layers) * half * half);
            for (u32 l = 0; l < layers; ++l)
                for (u32 y = 0; y < half; ++y)
                    for (u32 x = 0; x < half; ++x) {
                        u32 sum[4] = {0, 0, 0, 0}, wsum = 0;
                        for (u32 dy = 0; dy < 2; ++dy)
                            for (u32 dx = 0; dx < 2; ++dx) {
                                const u32 p = level[static_cast<usize>(l) * size * size + (y * 2 + dy) * size + x * 2 + dx];
                                const u32 a = p >> 24;
                                for (int c2 = 0; c2 < 3; ++c2) sum[c2] += ((p >> (c2 * 8)) & 0xFF) * a;
                                sum[3] += a;
                                wsum += a;
                            }
                        u32 out = 0;
                        for (int c2 = 0; c2 < 3; ++c2) out |= (wsum ? sum[c2] / wsum : 0) << (c2 * 8);
                        out |= (sum[3] / 4) << 24;
                        next[static_cast<usize>(l) * half * half + y * half + x] = out;
                    }
            level = std::move(next);
            size = half;
        }
        BRIDGER_TRY(Buffer staging, make_buffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
        std::memcpy(staging.mapped, data.data(), data.size());
        const Status st = immediate([&](VkCommandBuffer cmd) {
            image_barrier(cmd, textures.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kMipLevels, layers);
            vkCmdCopyBufferToImage(cmd, staging.buffer, textures.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(copies.size()),
                                   copies.data());
            image_barrier(cmd, textures.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kMipLevels, layers);
        });
        free_buffer(staging);
        BRIDGER_TRY_STATUS(st);
        VkDescriptorImageInfo ii{sampler, textures.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descriptor_set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev(), 1, &w, 0, nullptr);
        return {};
    }

    Status upload_section(glm::ivec3 origin, const SectionMesh& mesh) {
        remove_section(origin);
        const usize quads = (mesh.solid.size() + mesh.cutout.size() + mesh.translucent.size()) / 4;
        if (quads == 0) return {};
        if ((mesh.solid.size() + mesh.cutout.size()) / 4 > kMaxQuadsPerDraw || mesh.translucent.size() / 4 > kMaxQuadsPerDraw)
            return make_error(ErrorCode::LimitExceeded, "section mesh too large");
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(quads) * 4 * sizeof(Vertex);
        GpuSection s;
        s.origin = origin;
        s.opaque_quads = static_cast<u32>((mesh.solid.size() + mesh.cutout.size()) / 4);
        s.translucent_quads = static_cast<u32>(mesh.translucent.size() / 4);
        BRIDGER_TRY(s.vertices, make_buffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false));
        BRIDGER_TRY(Buffer staging, make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
        auto* dst = static_cast<u8*>(staging.mapped);
        for (const auto* part : {&mesh.solid, &mesh.cutout, &mesh.translucent}) {
            std::memcpy(dst, part->data(), part->size() * sizeof(Vertex));
            dst += part->size() * sizeof(Vertex);
        }
        pending.push_back({staging, s.vertices.buffer, bytes});
        sections.emplace(section_key(origin), s);
        return {};
    }

    void remove_section(glm::ivec3 origin) {
        const auto it = sections.find(section_key(origin));
        if (it == sections.end()) return;
        frames[frame].garbage.push_back(it->second.vertices);
        sections.erase(it);
    }

    Result<FrameStats> render(const Camera& camera, const FrameSettings& fs) {
        const auto t0 = std::chrono::steady_clock::now();
        Frame& f = frames[frame];
        vkWaitForFences(dev(), 1, &f.fence, VK_TRUE, UINT64_MAX);
        for (Buffer& b : f.garbage) free_buffer(b);
        f.garbage.clear();

        if (shared && color.image == VK_NULL_HANDLE) return FrameStats{};
        const bool swap = !offscreen && !shared;
        u32 image_index = 0;
        if (swap) {
            const VkResult r = vkAcquireNextImageKHR(dev(), swapchain.swapchain, UINT64_MAX, f.image_available, VK_NULL_HANDLE, &image_index);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) {
                BRIDGER_TRY_STATUS(recreate(width, height));
                return FrameStats{};
            }
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return make_error(ErrorCode::IoError, "Vulkan: acquiring a swapchain image failed");
        }
        vkResetFences(dev(), 1, &f.fence);
        vkResetCommandBuffer(f.cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cmd, &bi);

        // Pending uploads, then make them visible to vertex input.
        if (!pending.empty()) {
            for (PendingCopy& p : pending) {
                VkBufferCopy c{0, 0, p.size};
                vkCmdCopyBuffer(f.cmd, p.staging.buffer, p.dst, 1, &c);
                f.garbage.push_back(p.staging);
            }
            pending.clear();
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
            mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1;
            d.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(f.cmd, &d);
        }

        const VkImage target = swap ? swap_images[image_index] : color.image;
        const VkImageView target_view = swap ? swap_views[image_index] : color.view;
        image_barrier(f.cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        image_barrier(f.cmd, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = target_view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{fs.sky_color.r, fs.sky_color.g, fs.sky_color.b, 1.0f}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {width, height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(f.cmd, &ri);
        const VkViewport vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        const VkRect2D sc{{0, 0}, {width, height}};
        vkCmdSetViewport(f.cmd, 0, 1, &vp);
        vkCmdSetScissor(f.cmd, 0, 1, &sc);

        // Camera (relative to its own position).
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        glm::mat4 view, proj;
        if (fs.map_mode) {
            const float e = fs.map_half_extent;
            view = glm::lookAtRH(glm::vec3(0, 512, 0), glm::vec3(0, 0, 0), glm::vec3(0, 0, -1));
            proj = glm::orthoRH_ZO(-e * aspect, e * aspect, -e, e, 1.0f, 1024.0f);
            proj[1][1] *= -1.0f;
        } else {
            view = camera.view(camera.position);
            proj = camera.projection(aspect);
        }
        const glm::mat4 vp_matrix = proj * view;
        const auto planes = frustum_planes(vp_matrix);

        FrameStats stats;
        struct Visible {
            const GpuSection* s;
            glm::vec3 rel;
            float dist;
        };
        std::vector<Visible> visible;
        visible.reserve(sections.size());
        for (const auto& [key, s] : sections) {
            const glm::vec3 rel = glm::vec3(glm::dvec3(s.origin) - camera.position);
            if (fs.slice_y && static_cast<float>(s.origin.y) >= *fs.slice_y) {
                ++stats.sections_culled;
                continue;
            }
            if (!box_visible(planes, rel, rel + glm::vec3(16.0f))) {
                ++stats.sections_culled;
                continue;
            }
            const glm::vec3 c = rel + glm::vec3(8.0f);
            visible.push_back({&s, rel, glm::dot(c, c)});
        }

        PushConstants pc{};
        pc.view_projection = vp_matrix;
        pc.fog = {fs.fog_start, fs.fog_end, fs.slice_y.value_or(0.0f), fs.slice_y ? 1.0f : 0.0f};
        pc.camera = {static_cast<float>(camera.position.x), static_cast<float>(camera.position.y), static_cast<float>(camera.position.z),
                     fs.map_mode ? 1.0f : 0.0f};
        if (texture_layers > 0 && !visible.empty()) {
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, chunk_layout, 0, 1, &descriptor_set, 0, nullptr);
            vkCmdBindIndexBuffer(f.cmd, quad_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            // Opaque (solid + cutout), front to back for early depth rejection.
            std::sort(visible.begin(), visible.end(), [](const Visible& a, const Visible& b) { return a.dist < b.dist; });
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_pipeline);
            pc.sky = {fs.sky_color, 0.0f};
            for (const Visible& v : visible) {
                if (v.s->opaque_quads == 0) continue;
                pc.origin = {v.rel, static_cast<float>(v.s->origin.y)};
                vkCmdPushConstants(f.cmd, chunk_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                const VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(f.cmd, 0, 1, &v.s->vertices.buffer, &off);
                vkCmdDrawIndexed(f.cmd, v.s->opaque_quads * 6, 1, 0, 0, 0);
                stats.quads_drawn += v.s->opaque_quads;
            }
            // Translucent, back to front.
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, translucent_pipeline);
            pc.sky = {fs.sky_color, 1.0f};
            for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
                if (it->s->translucent_quads == 0) continue;
                pc.origin = {it->rel, static_cast<float>(it->s->origin.y)};
                vkCmdPushConstants(f.cmd, chunk_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                const VkDeviceSize off = static_cast<VkDeviceSize>(it->s->opaque_quads) * 4 * sizeof(Vertex);
                vkCmdBindVertexBuffers(f.cmd, 0, 1, &it->s->vertices.buffer, &off);
                vkCmdDrawIndexed(f.cmd, it->s->translucent_quads * 6, 1, 0, 0, 0);
                stats.quads_drawn += it->s->translucent_quads;
            }
        }
        stats.sections_drawn = static_cast<u32>(visible.size());

        // Outline boxes.
        if (!outlines.empty()) {
            struct LineVertex {
                glm::vec3 p;
                glm::vec4 c;
            };
            std::vector<LineVertex> lines;
            for (const OutlineBox& b : outlines) {
                const glm::vec3 lo = glm::vec3(b.min - camera.position), hi = glm::vec3(b.max - camera.position);
                const std::array<glm::vec3, 8> c{{{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
                                                  {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}}};
                static constexpr std::array<std::pair<int, int>, 12> kEdges{{{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4},
                                                                              {0, 4}, {1, 5}, {2, 6}, {3, 7}}};
                for (const auto& [a, z] : kEdges) {
                    lines.push_back({c[static_cast<usize>(a)], b.color});
                    lines.push_back({c[static_cast<usize>(z)], b.color});
                }
            }
            const VkDeviceSize bytes = lines.size() * sizeof(LineVertex);
            if (f.lines.size < bytes) {
                free_buffer(f.lines);
                auto lb = make_buffer(bytes * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
                if (lb) f.lines = *lb;
            }
            if (f.lines.mapped != nullptr) {
                std::memcpy(f.lines.mapped, lines.data(), bytes);
                vmaFlushAllocation(vma, f.lines.alloc, 0, bytes);
                vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline);
                vkCmdPushConstants(f.cmd, line_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &vp_matrix);
                const VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(f.cmd, 0, 1, &f.lines.buffer, &off);
                vkCmdDraw(f.cmd, static_cast<u32>(lines.size()), 1, 0, 0);
            }
        }
        vkCmdEndRendering(f.cmd);

        if (offscreen) {
            image_barrier(f.cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(f.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &c);
        } else if (shared) {
            // Hand the image to Direct3D (queue family release to "external").
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = device.get_queue_index(vkb::QueueType::graphics).value();
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            b.image = target;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.imageMemoryBarrierCount = 1;
            d.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(f.cmd, &d);
        } else {
            image_barrier(f.cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        }
        vkEndCommandBuffer(f.cmd);

        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = f.cmd;
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = f.image_available;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = swap ? render_done[image_index] : VK_NULL_HANDLE;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        if (swap) {
            si.waitSemaphoreInfoCount = 1;
            si.pWaitSemaphoreInfos = &wait;
            si.signalSemaphoreInfoCount = 1;
            si.pSignalSemaphoreInfos = &signal;
        }
        BRIDGER_TRY_STATUS(vk_check(vkQueueSubmit2(queue, 1, &si, f.fence), "vkQueueSubmit2"));
        if (shared) {
            vkWaitForFences(dev(), 1, &f.fence, VK_TRUE, UINT64_MAX); // Direct3D reads the image next
        } else if (swap) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &render_done[image_index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain.swapchain;
            pi.pImageIndices = &image_index;
            const VkResult r = vkQueuePresentKHR(queue, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) BRIDGER_TRY_STATUS(recreate(width, height));
        } else {
            readback_ready = true;
            last_frame = frame;
        }
        frame = (frame + 1) % kFramesInFlight;
        stats.cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return stats;
    }

    u32 last_frame = 0;

    Status recreate(u32 w, u32 h) {
        if (shared) return make_error(ErrorCode::InvalidArgument, "the shared target is resized with set_shared_target");
        vkDeviceWaitIdle(dev());
        free_image(depth);
        if (offscreen) {
            free_image(color);
            free_buffer(readback);
            readback_ready = false;
        }
        return create_targets(w, h);
    }

    Result<std::vector<u8>> read_pixels() {
        if (!offscreen) return make_error(ErrorCode::InvalidArgument, "read_pixels needs the offscreen target");
        if (!readback_ready) return make_error(ErrorCode::InvalidArgument, "no frame rendered yet");
        vkWaitForFences(dev(), 1, &frames[last_frame].fence, VK_TRUE, UINT64_MAX);
        vmaInvalidateAllocation(vma, readback.alloc, 0, VK_WHOLE_SIZE);
        const auto* p = static_cast<const u8*>(readback.mapped);
        return std::vector<u8>(p, p + static_cast<usize>(width) * height * 4);
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
Renderer::~Renderer() = default;

Result<Renderer> Renderer::create(const RendererConfig& config) {
    Renderer r;
    BRIDGER_TRY_STATUS(r.impl_->init(config));
    return r;
}

Status Renderer::upload_textures(std::span<const u32> rgba, u32 layers) { return impl_->upload_textures(rgba, layers); }
Status Renderer::upload_section(glm::ivec3 origin, const SectionMesh& mesh) { return impl_->upload_section(origin, mesh); }
void Renderer::remove_section(glm::ivec3 origin) { impl_->remove_section(origin); }
void Renderer::clear_sections() {
    for (auto& [k, s] : impl_->sections) impl_->frames[impl_->frame].garbage.push_back(s.vertices);
    impl_->sections.clear();
}
usize Renderer::section_count() const noexcept { return impl_->sections.size(); }
u64 Renderer::gpu_bytes() const noexcept {
    u64 n = 0;
    for (const auto& [k, s] : impl_->sections) n += s.vertices.size;
    return n;
}
void Renderer::set_outlines(std::vector<OutlineBox> boxes) { impl_->outlines = std::move(boxes); }
Status Renderer::resize(u32 w, u32 h) { return impl_->recreate(w, h); }
Status Renderer::set_shared_target(void* handle, u32 w, u32 h) { return impl_->set_shared_target(handle, w, h); }
std::optional<u64> Renderer::adapter_luid() const { return impl_->adapter_luid(); }
Result<FrameStats> Renderer::render(const Camera& camera, const FrameSettings& settings) { return impl_->render(camera, settings); }
Result<std::vector<u8>> Renderer::read_pixels() { return impl_->read_pixels(); }
std::string Renderer::device_name() const { return impl_->physical.properties.deviceName; }
u32 Renderer::width() const noexcept { return impl_->width; }
u32 Renderer::height() const noexcept { return impl_->height; }

} // namespace bridger::render
