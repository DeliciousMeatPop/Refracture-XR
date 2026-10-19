// ASTC and ETC2/EAC texture emulation for host GPUs without them (desktop NVIDIA/AMD).
//
// Quest games ship ASTC textures (UE4 games cooked for Android_ETC2: ETC2). When the
// device cannot sample them, Unity decodes every texture on the CPU, which under the
// JIT takes minutes, and UE4 ends up with placeholder textures. Instead we report the
// formats as supported, back such images with RGBA8 (EAC: R16/R16G16) and decode the
// blocks on the host when the app copies them in (vkCmdCopyBufferToImage):
// the source is read from the mapped staging buffer at record time and the copy
// is redirected to a host staging buffer holding the decoded texels. That
// buffer lives until its command buffer is reset or freed (by then the spec
// guarantees it is no longer pending).
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <astcenc.h>
#include <smmintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <execution>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "etc2_decode.h"
#include "thunks.h"
#include "vulkan_hle.h"

namespace rn {

namespace {

template <typename T>
T Fn(const char* name) {
    return reinterpret_cast<T>(VulkanHostProc(name));
}

// --- formats ------------------------------------------------------------------------

enum Family { kAstc, kEtc2, kFamilies };

struct CompFormat {
    Family family;
    uint32_t bw, bh;
    bool srgb;
    EtcCodec etc;  // ETC2/EAC only
    bool sign;     // EAC SNORM
    bool transcode;  // ETC2 colour transcoded to BC1/BC3 instead of decoded
    VkFormat substitute;
};

// The host samples BC1/BC3 (desktop GPUs do): ETC2 colour textures are transcoded to them, so they stay as
// small as on the device (decoded to RGBA8 they take 4-8x the memory; RE4's levels then exceed 10 GB of VRAM).
std::atomic<bool> g_bc_transcode{false};

bool CompInfo(VkFormat f, CompFormat* out) {
    static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    CompFormat c{};
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        int i = f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        c = {kAstc, dims[i / 2][0], dims[i / 2][1], (i & 1) != 0};
        c.substitute = c.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    } else if (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) {
        static const EtcCodec codecs[5] = {EtcCodec::Rgb8, EtcCodec::Rgb8A1, EtcCodec::Rgba8, EtcCodec::R11,
                                           EtcCodec::Rg11};
        int i = f - VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        c = {kEtc2, 4, 4};
        c.etc = codecs[i / 2];
        if (i < 6) {
            c.srgb = (i & 1) != 0;
            c.transcode = g_bc_transcode;
            if (!c.transcode)
                c.substitute = c.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            else if (c.etc == EtcCodec::Rgb8)
                c.substitute = c.srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
            else
                c.substitute = c.srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        } else {
            c.sign = (i & 1) != 0;
            c.substitute = c.etc == EtcCodec::R11 ? (c.sign ? VK_FORMAT_R16_SNORM : VK_FORMAT_R16_UNORM)
                                                  : (c.sign ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM);
        }
    } else {
        return false;
    }
    if (out)
        *out = c;
    return true;
}

VkFormat Substitute(VkFormat f) {
    CompFormat c;
    return CompInfo(f, &c) ? c.substitute : f;
}

constexpr VkFormatFeatureFlags kEmulatedFeatures =
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
constexpr VkImageUsageFlags kEmulatedUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// Which compressed families a host GPU cannot sample (desktop NVIDIA/AMD: ASTC and ETC2 both).
struct Missing {
    bool family[kFamilies] = {};
    bool any() const { return family[kAstc] || family[kEtc2]; }
};

std::mutex g_mu;
std::atomic<uint64_t> g_decoded_texels{0};
std::atomic<uint64_t> g_decode_us{0};  // time spent in copy hooks (on the threads recording the copies)
std::atomic<uint64_t> g_core_us{0};    // of which decoding/transcoding itself
std::atomic<uint64_t> g_alloc_us{0};   // of which allocating staging memory

struct AddElapsed {  // adds the scope's duration to a counter
    std::atomic<uint64_t>& counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~AddElapsed() {
        counter += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    }
};
std::map<VkPhysicalDevice, Missing> g_missing;
VkPhysicalDevice g_last_pd = VK_NULL_HANDLE;
std::unordered_map<VkDevice, VkPhysicalDevice> g_device_pd;

Missing MissingFamilies(VkPhysicalDevice pd) {
    {
        std::lock_guard lock(g_mu);
        auto it = g_missing.find(pd);
        if (it != g_missing.end())
            return it->second;
    }
    auto props = Fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    auto lacks = [&](VkFormat f) {
        VkFormatProperties p{};
        props(pd, f, &p);
        return !(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    };
    Missing m;
    m.family[kAstc] = lacks(VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    m.family[kEtc2] = lacks(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
    const bool bc = !lacks(VK_FORMAT_BC1_RGB_UNORM_BLOCK) && !lacks(VK_FORMAT_BC3_UNORM_BLOCK);
    std::lock_guard lock(g_mu);
    if (!g_missing.count(pd)) {
        if (m.family[kAstc])
            RN_INFO("Vulkan: host GPU has no ASTC; emulating it (RGBA8 images, host-side decode)");
        if (m.family[kEtc2])
            RN_INFO("Vulkan: host GPU has no ETC2/EAC; emulating it (%s, EAC as R16; host-side)",
                    bc ? "colour transcoded to BC1/BC3" : "colour decoded to RGBA8");
        if (m.family[kEtc2])
            g_bc_transcode = bc;
    }
    g_missing[pd] = m;
    g_last_pd = pd;
    return m;
}

bool NeedsEmulation(VkPhysicalDevice pd) {
    return MissingFamilies(pd).any();
}

// f is a compressed format the host cannot sample, so it is emulated.
bool Emulated(VkPhysicalDevice pd, VkFormat f, CompFormat* out = nullptr) {
    const Missing m = MissingFamilies(pd);  // first: it decides g_bc_transcode, which CompInfo reads
    CompFormat c;
    if (!CompInfo(f, &c) || !m.family[c.family])
        return false;
    if (out)
        *out = c;
    return true;
}

VkPhysicalDevice DevicePd(VkDevice d) {
    std::lock_guard lock(g_mu);
    auto it = g_device_pd.find(d);
    return it != g_device_pd.end() ? it->second : g_last_pd;
}

bool DeviceNeedsEmulation(VkDevice d) {
    VkPhysicalDevice pd = DevicePd(d);
    return pd && NeedsEmulation(pd);
}

// --- tracked objects ------------------------------------------------------------------

struct EmuImage {
    VkDevice device;
    CompFormat format;
};
struct MemoryInfo {
    VkDevice device;
    uint8_t* base = nullptr;  // host pointer to offset 0 while mapped
};
struct BufferBinding {
    VkDeviceMemory memory;
    VkDeviceSize offset;
};
struct Chunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t* ptr;
    VkDeviceSize size, used;
};
struct CmdInfo {
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<Chunk> chunks;
};

std::unordered_map<VkImage, EmuImage> g_images;
std::unordered_map<VkDeviceMemory, MemoryInfo> g_memory;
std::unordered_map<VkBuffer, BufferBinding> g_buffers;
std::unordered_map<VkCommandBuffer, CmdInfo> g_cmds;

// Framebuffers larger than their attachments (UE4's Quest renderer draws its 1280x720 window pass into a 1x1
// dummy back buffer) are invalid and lose the device on desktop GPUs: they are clamped to the attachments.
std::unordered_map<VkImage, VkExtent2D> g_image_extent;
std::unordered_map<VkImageView, VkExtent2D> g_view_extent;
std::unordered_map<VkFramebuffer, VkExtent2D> g_fb_clamped;

// Staging chunks of finished command buffers, kept for reuse: allocating and mapping host memory for every
// upload took RE4 as long as decoding (seconds over a level load, stalls while streaming). Caller holds g_mu.
struct FreeChunk {
    VkDevice device;
    Chunk chunk;
};
std::vector<FreeChunk> g_free_chunks;
VkDeviceSize g_free_bytes = 0;
constexpr VkDeviceSize kMaxFreeBytes = 512ull << 20;

void DestroyChunk(VkDevice device, const Chunk& c) {
    Fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, c.buffer, nullptr);
    Fn<PFN_vkFreeMemory>("vkFreeMemory")(device, c.memory, nullptr);  // implicitly unmaps
}

void FreeChunks(VkDevice device, std::vector<Chunk>& chunks) {
    for (auto& c : chunks) {
        if (g_free_bytes + c.size <= kMaxFreeBytes) {
            c.used = 0;
            g_free_chunks.push_back({device, c});
            g_free_bytes += c.size;
        } else {
            DestroyChunk(device, c);
        }
    }
    chunks.clear();
}

// A free chunk of this device with room for size bytes, the smallest that fits. Caller holds g_mu.
bool ReuseChunk(VkDevice device, VkDeviceSize size, Chunk* out) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < g_free_chunks.size(); ++i)
        if (g_free_chunks[i].device == device && g_free_chunks[i].chunk.size >= size &&
            (best == SIZE_MAX || g_free_chunks[i].chunk.size < g_free_chunks[best].chunk.size))
            best = i;
    if (best == SIZE_MAX)
        return false;
    *out = g_free_chunks[best].chunk;
    g_free_bytes -= out->size;
    g_free_chunks.erase(g_free_chunks.begin() + static_cast<ptrdiff_t>(best));
    return true;
}

// Caller holds g_mu.
void ReleaseCmd(VkCommandBuffer cb, bool forget) {
    auto it = g_cmds.find(cb);
    if (it == g_cmds.end())
        return;
    FreeChunks(it->second.device, it->second.chunks);
    if (forget)
        g_cmds.erase(it);
}

// Host-visible staging space owned by a command buffer. Caller holds g_mu.
uint8_t* StagingAlloc(VkCommandBuffer cb, VkDevice device, VkDeviceSize size, VkBuffer* buffer, VkDeviceSize* offset) {
    CmdInfo& ci = g_cmds[cb];
    ci.device = device;
    if (!ci.chunks.empty()) {
        Chunk& c = ci.chunks.back();
        VkDeviceSize at = (c.used + 15) & ~VkDeviceSize(15);
        if (at + size <= c.size) {
            c.used = at + size;
            *buffer = c.buffer;
            *offset = at;
            return c.ptr + at;
        }
    }
    AddElapsed alloc_time{g_alloc_us};
    if (Chunk reused; ReuseChunk(device, size, &reused)) {
        reused.used = size;
        ci.chunks.push_back(reused);
        *buffer = reused.buffer;
        *offset = 0;
        return reused.ptr;
    }
    auto pd = g_device_pd.count(device) ? g_device_pd[device] : g_last_pd;
    VkDeviceSize chunk_size = std::max<VkDeviceSize>(size, 16u << 20);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = chunk_size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Chunk c{};
    if (Fn<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bi, nullptr, &c.buffer) != VK_SUCCESS)
        return nullptr;
    VkMemoryRequirements req;
    Fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, c.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp;
    Fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(pd, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    void* p = nullptr;
    if (type == UINT32_MAX || Fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &ai, nullptr, &c.memory) != VK_SUCCESS) {
        Fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, c.buffer, nullptr);
        Log("ASTC emulation: cannot allocate %llu bytes of staging memory", static_cast<unsigned long long>(req.size));
        return nullptr;
    }
    Fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, c.buffer, c.memory, 0);
    Fn<PFN_vkMapMemory>("vkMapMemory")(device, c.memory, 0, VK_WHOLE_SIZE, 0, &p);
    c.ptr = static_cast<uint8_t*>(p);
    c.size = chunk_size;
    c.used = size;
    ci.chunks.push_back(c);
    *buffer = c.buffer;
    *offset = 0;
    return c.ptr;
}

// --- decoding ------------------------------------------------------------------------

unsigned DecodeThreads() {
    static const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    return n;
}

// Decodes one image, splitting large ones across DecodeThreads() threads (astcenc hands out
// blocks to every thread that calls in with its own index).
astcenc_error Decompress(astcenc_context* ctx, const uint8_t* data, size_t len, astcenc_image* im,
                         const astcenc_swizzle* swz) {
    const unsigned threads = size_t(im->dim_x) * im->dim_y >= 256 * 256 ? DecodeThreads() : 1;
    std::vector<astcenc_error> errors(threads, ASTCENC_SUCCESS);
    std::vector<std::thread> workers;
    for (unsigned i = 1; i < threads; ++i)
        workers.emplace_back([&, i] { errors[i] = astcenc_decompress_image(ctx, data, len, im, swz, i); });
    errors[0] = astcenc_decompress_image(ctx, data, len, im, swz, 0);
    for (auto& w : workers)
        w.join();
    astcenc_decompress_reset(ctx);
    for (astcenc_error e : errors)
        if (e != ASTCENC_SUCCESS)
            return e;
    return ASTCENC_SUCCESS;
}

// Decodes one ETC2/EAC image, splitting large ones into bands of block rows across DecodeThreads() threads.
// Decodes/transcodes one ETC2/EAC image in bands of block rows on the Windows thread pool (MSVC's parallel
// algorithms), so streamed textures stall the recording thread as briefly as possible.
void DecodeEtc(const CompFormat& f, const uint8_t* src, uint32_t row_blocks, uint32_t w, uint32_t h, uint8_t* dst) {
    const uint32_t block_rows = (h + 3) / 4;
    constexpr uint32_t kBandRows = 8;
    const uint32_t bands = size_t(w) * h >= 64 * 64 ? (block_rows + kBandRows - 1) / kBandRows : 1;
    const size_t block_bytes = EtcBlockBytes(f.etc);
    // Output bytes per row of blocks.
    const size_t out_row = f.transcode ? size_t((w + 3) / 4) * BcBlockBytes(f.etc) : size_t(w) * 4 * EtcTexelBytes(f.etc);
    auto band = [&](uint32_t i) {
        const uint32_t first = bands == 1 ? 0 : i * kBandRows, last = bands == 1 ? block_rows : std::min(block_rows, first + kBandRows);
        const uint32_t y0 = first * 4, rows = std::min(h, last * 4) - y0;
        const uint8_t* in = src + size_t(first) * row_blocks * block_bytes;
        if (f.transcode)
            TranscodeEtcToBc(f.etc, in, row_blocks, w, rows, dst + size_t(first) * out_row);
        else
            DecodeEtcImage(f.etc, f.sign, in, row_blocks, w, rows, dst + size_t(first) * out_row);
    };
    if (bands == 1) {
        band(0);
        return;
    }
    std::vector<uint32_t> ids(bands);
    for (uint32_t i = 0; i < bands; ++i)
        ids[i] = i;
    std::for_each(std::execution::par, ids.begin(), ids.end(), band);
}

astcenc_context* Context(const CompFormat& f) {
    static std::map<uint32_t, astcenc_context*> contexts;  // under g_mu
    uint32_t key = f.bw << 16 | f.bh << 8 | (f.srgb ? 1 : 0);
    auto it = contexts.find(key);
    if (it != contexts.end())
        return it->second;
    astcenc_config cfg;
    astcenc_context* ctx = nullptr;
    astcenc_error e = astcenc_config_init(f.srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, f.bw, f.bh, 1,
                                          ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg);
    if (e == ASTCENC_SUCCESS)
        e = astcenc_context_alloc(&cfg, DecodeThreads(), &ctx);
    if (e != ASTCENC_SUCCESS) {
        Log("ASTC emulation: context %ux%u: %s", f.bw, f.bh, astcenc_get_error_string(e));
        ctx = nullptr;
    }
    contexts[key] = ctx;
    return ctx;
}

// Source bytes for a buffer range, mapping the memory temporarily if the app has not.
struct SourceView {
    const uint8_t* ptr = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory temp = VK_NULL_HANDLE;
    ~SourceView() {
        if (temp)
            Fn<PFN_vkUnmapMemory>("vkUnmapMemory")(device, temp);
    }
};

bool MapSource(VkBuffer buffer, SourceView* v) {
    auto b = g_buffers.find(buffer);
    if (b == g_buffers.end())
        return false;
    auto m = g_memory.find(b->second.memory);
    if (m == g_memory.end())
        return false;
    if (!m->second.base) {
        void* p = nullptr;
        if (Fn<PFN_vkMapMemory>("vkMapMemory")(m->second.device, b->second.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
            return false;
        v->device = m->second.device;
        v->temp = b->second.memory;
        v->ptr = static_cast<const uint8_t*>(p) + b->second.offset;
        return true;
    }
    v->ptr = m->second.base + b->second.offset;
    return true;
}

// Copies from memory that may be uncached/write-combined (games put upload buffers there; ordinary loads from it
// are slow, and the decoders read every block) with streaming loads into ordinary memory.
void StreamCopy(uint8_t* dst, const uint8_t* src, size_t n) {
    size_t head = std::min(n, size_t((16 - (reinterpret_cast<uintptr_t>(src) & 15)) & 15));
    memcpy(dst, src, head);
    dst += head, src += head, n -= head;
    const size_t vectors = n / 64;
    for (size_t i = 0; i < vectors; ++i, src += 64, dst += 64) {
        auto* p = reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src));
        const __m128i a = _mm_stream_load_si128(p), b = _mm_stream_load_si128(p + 1);
        const __m128i c = _mm_stream_load_si128(p + 2), d = _mm_stream_load_si128(p + 3);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst) + 1, b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst) + 2, c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst) + 3, d);
    }
    memcpy(dst, src, n - vectors * 64);
}

// Decodes one copy region into staging; returns the rewritten region. Caller holds g_mu.
bool DecodeRegion(VkCommandBuffer cb, const EmuImage& img, const uint8_t* src, const VkBufferImageCopy& r,
                  VkBuffer* out_buffer, VkBufferImageCopy* out) {
    const CompFormat& f = img.format;
    const bool etc = f.family == kEtc2;
    const size_t block_bytes = etc ? EtcBlockBytes(f.etc) : 16, texel_bytes = etc ? EtcTexelBytes(f.etc) : 4;
    uint32_t w = r.imageExtent.width, h = r.imageExtent.height, depth = std::max(1u, r.imageExtent.depth);
    uint32_t layers = std::max(1u, r.imageSubresource.layerCount);
    uint32_t row_texels = r.bufferRowLength ? r.bufferRowLength : w;
    uint32_t img_rows = r.bufferImageHeight ? r.bufferImageHeight : h;
    uint32_t bx = (w + f.bw - 1) / f.bw, by = (h + f.bh - 1) / f.bh;
    uint32_t row_blocks = (row_texels + f.bw - 1) / f.bw;
    size_t slice_bytes = size_t(row_blocks) * ((img_rows + f.bh - 1) / f.bh) * block_bytes;
    size_t out_slice = f.transcode ? size_t(bx) * by * BcBlockBytes(f.etc) : size_t(w) * h * texel_bytes;
    VkDeviceSize out_offset;
    uint8_t* dst = StagingAlloc(cb, img.device, out_slice * depth * layers, out_buffer, &out_offset);
    if (!dst)
        return false;
    AddElapsed core_time{g_core_us};
    static thread_local std::vector<uint8_t> source;
    // Up to the last block the region reads (the final row and slice may end before a full stride).
    source.resize((size_t(depth) * layers - 1) * slice_bytes + ((size_t(by) - 1) * row_blocks + bx) * block_bytes);
    StreamCopy(source.data(), src + r.bufferOffset, source.size());
    const uint8_t* region = source.data();
    if (etc) {
        for (uint32_t s = 0; s < depth * layers; ++s)
            DecodeEtc(f, region + s * slice_bytes, row_blocks, w, h, dst + s * out_slice);
        *out = r;
        out->bufferOffset = out_offset;
        out->bufferRowLength = 0;
        out->bufferImageHeight = 0;
        return true;
    }
    astcenc_context* ctx = Context(f);
    if (!ctx)
        return false;
    std::vector<uint8_t> packed(size_t(bx) * by * 16);
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    for (uint32_t s = 0; s < depth * layers; ++s) {
        const uint8_t* slice = region + s * slice_bytes;
        for (uint32_t y = 0; y < by; ++y)
            memcpy(&packed[size_t(y) * bx * 16], slice + size_t(y) * row_blocks * 16, size_t(bx) * 16);
        void* plane = dst + s * out_slice;
        astcenc_image im{w, h, 1, ASTCENC_TYPE_U8, &plane};
        astcenc_error e = Decompress(ctx, packed.data(), packed.size(), &im, &swz);
        if (e != ASTCENC_SUCCESS) {
            static int warned = 0;
            if (warned++ < 5)
                Log("ASTC emulation: decode %ux%u (%ux%u blocks): %s", w, h, f.bw, f.bh, astcenc_get_error_string(e));
            memset(plane, 0xff, out_slice);
        }
    }
    *out = r;
    out->bufferOffset = out_offset;
    out->bufferRowLength = 0;
    out->bufferImageHeight = 0;
    return true;
}

// --- intercepted commands -------------------------------------------------------------

void ReportEmulatedFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f) {
    const Missing m = MissingFamilies(pd);
    if (m.family[kAstc])
        f->textureCompressionASTC_LDR = VK_TRUE;
    if (m.family[kEtc2])
        f->textureCompressionETC2 = VK_TRUE;
}

void VKAPI_CALL E_vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f) {
    Fn<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(pd, f);
    ReportEmulatedFeatures(pd, f);
}

void VKAPI_CALL E_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f) {
    Fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(pd, f);
    ReportEmulatedFeatures(pd, &f->features);
}

void MaskFormatProperties(VkFormatProperties* p) {
    p->linearTilingFeatures = 0;
    p->optimalTilingFeatures &= kEmulatedFeatures;
    p->bufferFeatures = 0;
}

void VKAPI_CALL E_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties* p) {
    bool emu = Emulated(pd, f);
    Fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(pd, emu ? Substitute(f) : f, p);
    if (emu)
        MaskFormatProperties(p);
}

void VKAPI_CALL E_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2* p) {
    bool emu = Emulated(pd, f);
    Fn<PFN_vkGetPhysicalDeviceFormatProperties2>("vkGetPhysicalDeviceFormatProperties2")(pd, emu ? Substitute(f) : f, p);
    if (!emu)
        return;
    MaskFormatProperties(&p->formatProperties);
    for (auto* n = static_cast<VkBaseOutStructure*>(p->pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            auto* p3 = reinterpret_cast<VkFormatProperties3*>(n);
            p3->linearTilingFeatures = 0;
            p3->optimalTilingFeatures &= kEmulatedFeatures;
            p3->bufferFeatures = 0;
        }
}

VkResult VKAPI_CALL E_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat f, VkImageType type,
                                                               VkImageTiling tiling, VkImageUsageFlags usage,
                                                               VkImageCreateFlags flags, VkImageFormatProperties* p) {
    auto host = Fn<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
    if (!Emulated(pd, f))
        return host(pd, f, type, tiling, usage, flags, p);
    if ((usage & ~kEmulatedUsage) || tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return host(pd, Substitute(f), type, tiling, usage, flags & ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, p);
}

VkResult VKAPI_CALL E_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                const VkPhysicalDeviceImageFormatInfo2* info,
                                                                VkImageFormatProperties2* p) {
    auto host = Fn<PFN_vkGetPhysicalDeviceImageFormatProperties2>("vkGetPhysicalDeviceImageFormatProperties2");
    if (!Emulated(pd, info->format))
        return host(pd, info, p);
    if ((info->usage & ~kEmulatedUsage) || info->tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkPhysicalDeviceImageFormatInfo2 i = *info;
    i.format = Substitute(info->format);
    i.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    return host(pd, &i, p);
}

VkResult VKAPI_CALL E_vkCreateImage(VkDevice device, const VkImageCreateInfo* ci, const VkAllocationCallbacks*,
                                    VkImage* out) {
    auto create = Fn<PFN_vkCreateImage>("vkCreateImage");
    CompFormat comp;
    VkPhysicalDevice pd = DevicePd(device);
    if (!pd || !Emulated(pd, ci->format, &comp)) {
        VkResult r = create(device, ci, nullptr, out);
        if (r == VK_SUCCESS) {
            std::lock_guard lock(g_mu);
            g_image_extent[*out] = {ci->extent.width, ci->extent.height};
        }
        return r;
    }
    VkImageCreateInfo c = *ci;
    c.format = Substitute(ci->format);
    c.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    // Rewrite a view format list in place for the call.
    VkImageFormatListCreateInfo* list = nullptr;
    const VkFormat* saved_formats = nullptr;
    std::vector<VkFormat> formats;
    for (auto* n = static_cast<const VkBaseInStructure*>(c.pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            list = reinterpret_cast<VkImageFormatListCreateInfo*>(const_cast<VkBaseInStructure*>(n));
    if (list && list->viewFormatCount) {
        formats.assign(list->pViewFormats, list->pViewFormats + list->viewFormatCount);
        for (auto& f : formats)
            f = Substitute(f);
        saved_formats = list->pViewFormats;
        list->pViewFormats = formats.data();
    }
    VkResult r = create(device, &c, nullptr, out);
    if (list && saved_formats)
        list->pViewFormats = saved_formats;
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_images[*out] = {device, comp};
    }
    return r;
}

void VKAPI_CALL E_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_images.erase(image);
        g_image_extent.erase(image);
    }
    Fn<PFN_vkDestroyImage>("vkDestroyImage")(device, image, nullptr);
}

VkResult VKAPI_CALL E_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* ci,
                                        const VkAllocationCallbacks*, VkImageView* out) {
    auto create = Fn<PFN_vkCreateImageView>("vkCreateImageView");
    bool emu;
    VkExtent2D extent{};
    {
        std::lock_guard lock(g_mu);
        emu = g_images.count(ci->image) != 0;
        auto it = g_image_extent.find(ci->image);
        if (it != g_image_extent.end()) {
            const uint32_t mip = ci->subresourceRange.baseMipLevel;
            extent = {std::max(1u, it->second.width >> mip), std::max(1u, it->second.height >> mip)};
        }
    }
    VkImageViewCreateInfo c = *ci;
    if (emu)
        c.format = Substitute(ci->format);
    VkResult r = create(device, &c, nullptr, out);
    if (r == VK_SUCCESS && extent.width) {
        std::lock_guard lock(g_mu);
        g_view_extent[*out] = extent;
    }
    return r;
}

void VKAPI_CALL E_vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_view_extent.erase(view);
    }
    Fn<PFN_vkDestroyImageView>("vkDestroyImageView")(device, view, nullptr);
}

VkResult VKAPI_CALL E_vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* ci,
                                          const VkAllocationCallbacks*, VkFramebuffer* out) {
    VkFramebufferCreateInfo c = *ci;
    if (!(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
        std::lock_guard lock(g_mu);
        for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
            auto it = g_view_extent.find(ci->pAttachments[i]);
            if (it == g_view_extent.end())
                continue;
            c.width = std::min(c.width, it->second.width);
            c.height = std::min(c.height, it->second.height);
        }
    }
    VkResult r = Fn<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(device, &c, nullptr, out);
    if (r == VK_SUCCESS && (c.width != ci->width || c.height != ci->height)) {
        static std::atomic<int> logged{0};
        if (logged++ < 5)
            RN_INFO("Vulkan: framebuffer %ux%u larger than its attachments, clamped to %ux%u", ci->width, ci->height,
                    c.width, c.height);
        std::lock_guard lock(g_mu);
        g_fb_clamped[*out] = {c.width, c.height};
    }
    return r;
}

void VKAPI_CALL E_vkDestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_fb_clamped.erase(fb);
    }
    Fn<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(device, fb, nullptr);
}

// The render area must stay inside a clamped framebuffer.
bool ClampRenderArea(const VkRenderPassBeginInfo* bi, VkRenderPassBeginInfo* out) {
    std::lock_guard lock(g_mu);
    auto it = g_fb_clamped.find(bi->framebuffer);
    if (it == g_fb_clamped.end())
        return false;
    *out = *bi;
    VkRect2D& a = out->renderArea;
    a.offset.x = std::min<int32_t>(std::max(a.offset.x, 0), static_cast<int32_t>(it->second.width) - 1);
    a.offset.y = std::min<int32_t>(std::max(a.offset.y, 0), static_cast<int32_t>(it->second.height) - 1);
    a.extent.width = std::min(a.extent.width, it->second.width - static_cast<uint32_t>(a.offset.x));
    a.extent.height = std::min(a.extent.height, it->second.height - static_cast<uint32_t>(a.offset.y));
    return true;
}

void VKAPI_CALL E_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents contents) {
    CheckpointBeginRenderPass(cb, bi);
    VkRenderPassBeginInfo c;
    Fn<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(cb, ClampRenderArea(bi, &c) ? &c : bi, contents);
}

void VKAPI_CALL E_vkCmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi,
                                        const VkSubpassBeginInfo* si) {
    CheckpointBeginRenderPass(cb, bi);
    VkRenderPassBeginInfo c;
    Fn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2")(cb, ClampRenderArea(bi, &c) ? &c : bi, si);
}

void VKAPI_CALL E_vkCmdBeginRenderPass2KHR(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi,
                                           const VkSubpassBeginInfo* si) {
    CheckpointBeginRenderPass(cb, bi);
    VkRenderPassBeginInfo c;
    Fn<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2KHR")(cb, ClampRenderArea(bi, &c) ? &c : bi, si);
}

VkResult VKAPI_CALL E_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                  VkMemoryMapFlags flags, void** data) {
    VkResult r = Fn<PFN_vkMapMemory>("vkMapMemory")(device, memory, offset, size, flags, data);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[memory] = {device, static_cast<uint8_t*>(*data) - offset};
    }
    return r;
}

VkResult VKAPI_CALL E_vkMapMemory2(VkDevice device, const VkMemoryMapInfo* info, void** data) {
    VkResult r = Fn<PFN_vkMapMemory2>("vkMapMemory2")(device, info, data);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[info->memory] = {device, static_cast<uint8_t*>(*data) - info->offset};
    }
    return r;
}

VkResult VKAPI_CALL E_vkMapMemory2KHR(VkDevice device, const VkMemoryMapInfo* info, void** data) {
    VkResult r = Fn<PFN_vkMapMemory2>("vkMapMemory2KHR")(device, info, data);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[info->memory] = {device, static_cast<uint8_t*>(*data) - info->offset};
    }
    return r;
}

void VKAPI_CALL E_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    {
        std::lock_guard lock(g_mu);
        g_memory[memory] = {device, nullptr};
    }
    Fn<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory);
}

VkResult VKAPI_CALL E_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks*,
                                       VkDeviceMemory* out) {
    VkResult r = Fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device, ai, nullptr, out);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[*out] = {device, nullptr};
    }
    return r;
}

void VKAPI_CALL E_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_memory.erase(memory);
    }
    Fn<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
}

VkResult VKAPI_CALL E_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                         VkDeviceSize offset) {
    VkResult r = Fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buffer, memory, offset);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_buffers[buffer] = {memory, offset};
    }
    return r;
}

VkResult BindBufferMemory2(const char* name, VkDevice device, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    VkResult r = Fn<PFN_vkBindBufferMemory2>(name)(device, n, infos);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        for (uint32_t i = 0; i < n; ++i)
            g_buffers[infos[i].buffer] = {infos[i].memory, infos[i].memoryOffset};
    }
    return r;
}
VkResult VKAPI_CALL E_vkBindBufferMemory2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    return BindBufferMemory2("vkBindBufferMemory2", d, n, infos);
}
VkResult VKAPI_CALL E_vkBindBufferMemory2KHR(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    return BindBufferMemory2("vkBindBufferMemory2KHR", d, n, infos);
}

void VKAPI_CALL E_vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_buffers.erase(buffer);
    }
    Fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, buffer, nullptr);
}

VkResult VKAPI_CALL E_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* ai,
                                               VkCommandBuffer* out) {
    VkResult r = Fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, ai, out);
    if (r == VK_SUCCESS && DeviceNeedsEmulation(device)) {
        std::lock_guard lock(g_mu);
        for (uint32_t i = 0; i < ai->commandBufferCount; ++i)
            g_cmds[out[i]] = {device, ai->commandPool, {}};
    }
    return r;
}

void VKAPI_CALL E_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
    {
        std::lock_guard lock(g_mu);
        for (uint32_t i = 0; i < n; ++i)
            ReleaseCmd(cbs[i], true);
    }
    Fn<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device, pool, n, cbs);
}

VkResult VKAPI_CALL E_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    {
        std::lock_guard lock(g_mu);
        ReleaseCmd(cb, false);
    }
    return Fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, bi);
}

VkResult VKAPI_CALL E_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    {
        std::lock_guard lock(g_mu);
        ReleaseCmd(cb, false);
    }
    return Fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(cb, flags);
}

void ReleasePool(VkCommandPool pool, bool forget) {
    std::lock_guard lock(g_mu);
    for (auto it = g_cmds.begin(); it != g_cmds.end();) {
        if (it->second.pool == pool) {
            FreeChunks(it->second.device, it->second.chunks);
            if (forget) {
                it = g_cmds.erase(it);
                continue;
            }
        }
        ++it;
    }
}

VkResult VKAPI_CALL E_vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    ReleasePool(pool, false);
    return Fn<PFN_vkResetCommandPool>("vkResetCommandPool")(device, pool, flags);
}

void VKAPI_CALL E_vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks*) {
    ReleasePool(pool, true);
    Fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
}


void NoteDecoded(const std::vector<VkBufferImageCopy>& rs) {
    uint64_t n = 0;
    for (auto& r : rs)
        n += uint64_t(r.imageExtent.width) * r.imageExtent.height * std::max(1u, r.imageExtent.depth) *
             std::max(1u, r.imageSubresource.layerCount);
    uint64_t before = g_decoded_texels.fetch_add(n);
    if (before == 0 || before >> 26 != (before + n) >> 26)
        RN_INFO("Texture emulation: %llu Mtexels decoded so far (%llu ms; decode %llu ms, staging allocation %llu ms)",
                static_cast<unsigned long long>((before + n) >> 20), static_cast<unsigned long long>(g_decode_us.load() / 1000),
                static_cast<unsigned long long>(g_core_us.load() / 1000), static_cast<unsigned long long>(g_alloc_us.load() / 1000));
}

void VKAPI_CALL E_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
                                         uint32_t n, const VkBufferImageCopy* regions) {
    auto host = Fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    CheckpointUpload(cb, reinterpret_cast<uint64_t>(dst), n);
    std::vector<VkBufferImageCopy> out;
    VkBuffer staging = VK_NULL_HANDLE;
    {
        std::lock_guard lock(g_mu);
        auto img = g_images.find(dst);
        if (img == g_images.end()) {
            host(cb, src, dst, layout, n, regions);
            return;
        }
        SourceView view;
        if (!MapSource(src, &view)) {
            Log("ASTC emulation: copy source buffer is not host-visible; texture left undefined");
            return;
        }
        // Each region may land in a different chunk; issue one copy per chunk run.
        const auto start = std::chrono::steady_clock::now();
        struct AddTime {
            std::chrono::steady_clock::time_point start;
            ~AddTime() {
                g_decode_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - start).count();
            }
        } add_time{start};
        for (uint32_t i = 0; i < n; ++i) {
            VkBuffer b;
            VkBufferImageCopy r;
            if (!DecodeRegion(cb, img->second, view.ptr, regions[i], &b, &r))
                continue;
            if (b != staging && !out.empty()) {
                host(cb, staging, dst, layout, static_cast<uint32_t>(out.size()), out.data());
                NoteDecoded(out);
                out.clear();
            }
            staging = b;
            out.push_back(r);
        }
    }
    if (!out.empty()) {
        host(cb, staging, dst, layout, static_cast<uint32_t>(out.size()), out.data());
        NoteDecoded(out);
    }
}

void CopyBufferToImage2(const char* name, VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    bool emu;
    {
        std::lock_guard lock(g_mu);
        emu = g_images.count(info->dstImage) != 0;
    }
    if (!emu) {
        Fn<PFN_vkCmdCopyBufferToImage2>(name)(cb, info);
        return;
    }
    std::vector<VkBufferImageCopy> rs(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const VkBufferImageCopy2& r = info->pRegions[i];
        rs[i] = {r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource, r.imageOffset,
                 r.imageExtent};
    }
    E_vkCmdCopyBufferToImage(cb, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount, rs.data());
}
void VKAPI_CALL E_vkCmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    CopyBufferToImage2("vkCmdCopyBufferToImage2", cb, info);
}
void VKAPI_CALL E_vkCmdCopyBufferToImage2KHR(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    CopyBufferToImage2("vkCmdCopyBufferToImage2KHR", cb, info);
}

}  // namespace

void NoteVulkanDevice(void* device, void* pd) {
    std::lock_guard lock(g_mu);
    g_device_pd[static_cast<VkDevice>(device)] = static_cast<VkPhysicalDevice>(pd);
}

// Emulated features must not reach the host's vkCreateDevice. Clears them in a
// copy of pEnabledFeatures and, for a VkPhysicalDeviceFeatures2 chain entry, in
// place (restored by the returned object's destructor).
DeviceFeatureFix::DeviceFeatureFix(void* create_info, void* physical_device) {
    auto* ci = static_cast<VkDeviceCreateInfo*>(create_info);
    const Missing m = MissingFamilies(static_cast<VkPhysicalDevice>(physical_device));
    auto emulated = [&](const VkPhysicalDeviceFeatures& f) {
        return (m.family[kAstc] && f.textureCompressionASTC_LDR) || (m.family[kEtc2] && f.textureCompressionETC2);
    };
    auto clear = [&](VkPhysicalDeviceFeatures& f) {
        if (m.family[kAstc])
            f.textureCompressionASTC_LDR = VK_FALSE;
        if (m.family[kEtc2])
            f.textureCompressionETC2 = VK_FALSE;
    };
    if (ci->pEnabledFeatures && emulated(*ci->pEnabledFeatures)) {
        auto* copy = new VkPhysicalDeviceFeatures(*ci->pEnabledFeatures);
        clear(*copy);
        owned_ = copy;
        ci->pEnabledFeatures = copy;
    }
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            auto* f2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(const_cast<VkBaseInStructure*>(n));
            if (emulated(f2->features)) {
                saved_ = new VkPhysicalDeviceFeatures(f2->features);
                patched_ = &f2->features;
                clear(f2->features);
            }
        }
}

DeviceFeatureFix::~DeviceFeatureFix() {
    if (patched_)
        *static_cast<VkPhysicalDeviceFeatures*>(patched_) = *static_cast<VkPhysicalDeviceFeatures*>(saved_);
    delete static_cast<VkPhysicalDeviceFeatures*>(saved_);
    delete static_cast<VkPhysicalDeviceFeatures*>(owned_);
}

void AddTextureEmulationThunks(std::map<std::string, ThunkFn>& m) {
    m["vkGetPhysicalDeviceFeatures"] = &Wrap<&E_vkGetPhysicalDeviceFeatures>;
    m["vkGetPhysicalDeviceFeatures2"] = &Wrap<&E_vkGetPhysicalDeviceFeatures2>;
    m["vkGetPhysicalDeviceFeatures2KHR"] = &Wrap<&E_vkGetPhysicalDeviceFeatures2>;
    m["vkGetPhysicalDeviceFormatProperties"] = &Wrap<&E_vkGetPhysicalDeviceFormatProperties>;
    m["vkGetPhysicalDeviceFormatProperties2"] = &Wrap<&E_vkGetPhysicalDeviceFormatProperties2>;
    m["vkGetPhysicalDeviceFormatProperties2KHR"] = &Wrap<&E_vkGetPhysicalDeviceFormatProperties2>;
    m["vkGetPhysicalDeviceImageFormatProperties"] = &Wrap<&E_vkGetPhysicalDeviceImageFormatProperties>;
    m["vkGetPhysicalDeviceImageFormatProperties2"] = &Wrap<&E_vkGetPhysicalDeviceImageFormatProperties2>;
    m["vkGetPhysicalDeviceImageFormatProperties2KHR"] = &Wrap<&E_vkGetPhysicalDeviceImageFormatProperties2>;
    m["vkCreateImage"] = &Wrap<&E_vkCreateImage>;
    m["vkDestroyImage"] = &Wrap<&E_vkDestroyImage>;
    m["vkCreateImageView"] = &Wrap<&E_vkCreateImageView>;
    m["vkDestroyImageView"] = &Wrap<&E_vkDestroyImageView>;
    m["vkCreateFramebuffer"] = &Wrap<&E_vkCreateFramebuffer>;
    m["vkDestroyFramebuffer"] = &Wrap<&E_vkDestroyFramebuffer>;
    m["vkCmdBeginRenderPass"] = &Wrap<&E_vkCmdBeginRenderPass>;
    m["vkCmdBeginRenderPass2"] = &Wrap<&E_vkCmdBeginRenderPass2>;
    m["vkCmdBeginRenderPass2KHR"] = &Wrap<&E_vkCmdBeginRenderPass2KHR>;
    m["vkAllocateMemory"] = &Wrap<&E_vkAllocateMemory>;
    m["vkFreeMemory"] = &Wrap<&E_vkFreeMemory>;
    m["vkMapMemory"] = &Wrap<&E_vkMapMemory>;
    m["vkMapMemory2"] = &Wrap<&E_vkMapMemory2>;
    m["vkMapMemory2KHR"] = &Wrap<&E_vkMapMemory2KHR>;
    m["vkUnmapMemory"] = &Wrap<&E_vkUnmapMemory>;
    m["vkBindBufferMemory"] = &Wrap<&E_vkBindBufferMemory>;
    m["vkBindBufferMemory2"] = &Wrap<&E_vkBindBufferMemory2>;
    m["vkBindBufferMemory2KHR"] = &Wrap<&E_vkBindBufferMemory2KHR>;
    m["vkDestroyBuffer"] = &Wrap<&E_vkDestroyBuffer>;
    m["vkAllocateCommandBuffers"] = &Wrap<&E_vkAllocateCommandBuffers>;
    m["vkFreeCommandBuffers"] = &Wrap<&E_vkFreeCommandBuffers>;
    m["vkBeginCommandBuffer"] = &Wrap<&E_vkBeginCommandBuffer>;
    m["vkResetCommandBuffer"] = &Wrap<&E_vkResetCommandBuffer>;
    m["vkResetCommandPool"] = &Wrap<&E_vkResetCommandPool>;
    m["vkDestroyCommandPool"] = &Wrap<&E_vkDestroyCommandPool>;
    m["vkCmdCopyBufferToImage"] = &Wrap<&E_vkCmdCopyBufferToImage>;
    m["vkCmdCopyBufferToImage2"] = &Wrap<&E_vkCmdCopyBufferToImage2>;
    m["vkCmdCopyBufferToImage2KHR"] = &Wrap<&E_vkCmdCopyBufferToImage2KHR>;
}

}  // namespace rn
