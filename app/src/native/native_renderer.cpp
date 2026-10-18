// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native renderer for the Switch port. See native_renderer.h for the design.
//
// Parts of this file are ports of ReXGlue's Vulkan backend (src/graphics/vulkan,
// itself from Xenia: Copyright Ben Vanik and the Xenia contributors, BSD-3-Clause,
// see LICENSES/): the system constants (VulkanCommandProcessor::
// UpdateSystemConstantValues), the dynamic state (UpdateDynamicState), the
// shader modifications and pipeline state (VulkanPipelineCache), and the resolve
// rectangle (draw_util::GetResolveInfo). They are what the translated shaders
// expect, so they are kept as close to the original as the native design allows;
// everything about EDRAM, fragment shader interlock and tessellation is gone.

#include "native_renderer.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <map>
#include <tuple>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <fmt/format.h>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/string/buffer.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

#if defined(__SWITCH__)
extern "C" {
#include <switch/kernel/svc.h>
}
#endif

#include "native_geometry_shaders.h"
#include "native_hash.h"
#include "native_textures.h"

// project8-sw's SDK can compile texture signs into each pipeline (gpu_specialize_texture_signs, off by default); our
// SDK's translator can't, so the signs are always selected at run time.
REXCVAR_DECLARE(std::string, bo1_debug_capture_dir);
REXCVAR_DECLARE(std::string, log_network);
REXCVAR_DEFINE_STRING(bo1_debug_dump_host, "", "BO1",
                      "Debug: send trace image dumps to this ip:port over TCP (tools/dump_receiver.py) instead of "
                      "writing them to bo1_debug_capture_dir; for the Switch");
REXCVAR_DEFINE_UINT32(bo1_debug_trace_frame, 0, "BO1",
                      "Debug: log every render pass, resolve and skipped draw of this frame number (0 = off)");
REXCVAR_DEFINE_UINT32(bo1_debug_trace_after_s, 0, "BO1",
                      "Debug: like bo1_debug_trace_frame, for the first frame this many seconds after start (0 = off)");
REXCVAR_DEFINE_UINT32(bo1_debug_dump_swaps, 0, "BO1",
                      "Debug: with bo1_debug_trace_after_s, also dump the presented image of this many consecutive "
                      "frames from the traced one (flicker hunting)");
REXCVAR_DEFINE_UINT32(bo1_native_resolve_exp_bias, 2, "BO1",
                      "Native renderer: apply the exponent bias of color resolves (0 = ignore it; 1 = 2^bias; 2 = "
                      "2^(bias+5) for 16_16 and 16_16_16_16 sources, as the SDK's resolve does)");
REXCVAR_DEFINE_BOOL(bo1_debug_mip_watch, false, "BO1",
                    "Debug: watch the mip data of large DXT textures (entropy at first sight and later changes)");
REXCVAR_DEFINE_BOOL(bo1_native_skip_aliased_depth, true, "BO1",
                    "Native renderer: skip depth-only draws into a depth target that lies inside a larger one with "
                    "the same pitch, drawn this frame and not resolved yet. Each EDRAM base is its own image here, so "
                    "those draws (the far half of BO1's 1024x2048 sun shadow map) never reached what the game resolves");
// bo1-nx: the "min" shadow preset (bo1_shadows). Shadow map draws are depth-only draws into a depth target without
// MSAA (the scene's depth uses the game's 2x MSAA); skipping them leaves each map as its clear left it, so nothing is
// in shadow, while the game keeps its shadow system on (with it off, models flicker between lit and dark).
// bo1-nx: BO1 draws its opaque scene twice: a depth-only prepass into the scene's depth buffer (~660 draws a frame in
// Kino der Toten), then the colour pass, which tests depth (greater-or-equal: BO1's depth is reversed) and writes it
// itself. Skipping the prepass leaves the colour pass with the same result and saves the renderer those draws; the GPU
// loses the early rejection of hidden pixels the prepass gave. Prepass draws: depth-only, depth written, into a
// multisampled target (shadow maps have no MSAA). Mass Effect's port skips its prepass the same way. Off by default:
// on the console (2026-10-16) skipping it cut the renderer's draws 987 -> 559 per frame but raised GPU time 36 -> 42 ms
// per frame, and the GPU is the limit (the ring thread waited on fences a third of the time).
REXCVAR_DEFINE_BOOL(bo1_native_skip_prepass, false, "BO1",
                    "Native renderer: skip the game's depth-only prepass (the colour pass sets depth itself)");
// bo1-nx: ZCULL (the GPU's coarse hidden-surface rejection). NVK only gives a depth image a ZCULL plane when nothing but
// depth rendering writes it, so no VK_IMAGE_USAGE_TRANSFER_DST_BIT ("[nvk] ZCULL: NO plane ... incompatible usages"
// in the log). Our depth targets used it twice, both at creation: the initial clear (now a clearing render pass) and the
// copy of the old content when a target grows (avoided by creating depth targets at the full height their EDRAM base
// allows; a target that still grows keeps TRANSFER_DST and goes without ZCULL).
REXCVAR_DEFINE_BOOL(bo1_native_zcull, false, "BO1",
                    "Native renderer: create depth targets so NVK can use ZCULL on them (full EDRAM height, cleared "
                    "by a render pass instead of a transfer)");
REXCVAR_DEFINE_BOOL(bo1_native_skip_shadow_maps, false, "BO1",
                    "Native renderer: skip the draws into shadow maps (set by bo1_shadows=min)");
REXCVAR_DEFINE_BOOL(bo1_native_memexport_copy, true, "BO1",
                    "Native renderer: do BO1's GPU memory copy (a memexport vertex shader) on the CPU; its texture "
                    "streamer copies the low mips into place with it");
REXCVAR_DEFINE_BOOL(bo1_native_ranged_vertices, true, "BO1",
                    "Native renderer: copy only the vertices a draw reads (its index range) when the vertex "
                    "shader fetches by vertex index, instead of whole vertex buffers");
REXCVAR_DEFINE_BOOL(bo1_native_hash_counters, false, "BO1",
                    "Diagnostics: time vertex/index data hashing and vertex stream copies");
REXCVAR_DEFINE_BOOL(bo1_native_depth24, false, "BO1",
                    "Store depth in 32 bits per pixel (24-bit depth with stencil, or float "
                    "without) instead of 32-bit float with stencil, which NVIDIA GPUs keep "
                    "in 64");
REXCVAR_DEFINE_BOOL(bo1_native_gpu_profile, false, "BO1",
                    "Log GPU time by render pass and resolve, measured on one frame in 16");
REXCVAR_DEFINE_BOOL(bo1_native_record_thread, false, "BO1",
                    "Make the Vulkan calls of draws on a second thread, so the command ring "
                    "thread prepares the next draw meanwhile");
REXCVAR_DEFINE_BOOL(bo1_native_msaa, true, "BO1",
                    "Render with the multisampling the game asks for; off draws every target "
                    "with one sample, which costs the GPU much less memory bandwidth");
REXCVAR_DEFINE_BOOL(bo1_native_async_pipelines, true, "BO1",
                    "Native renderer: compile new pipelines on a worker thread and skip the "
                    "draws that need them until they are ready, instead of stalling the frame");

namespace bo1::native {
namespace shaders {
#include "shaders/vulkan_spirv/apply_gamma_table_cs.h"
#include "shaders/depth_resolve_cs.h"
#include "shaders/depth_to_float_cs.h"
#include "shaders/copy_color_cs.h"
}  // namespace shaders

namespace {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;
using rex::graphics::vulkan::VulkanShader;
using rex::ui::vulkan::VulkanDevice;

// bo1-nx: walks a vertex shader in program order and tells whether every vertex fetch reads the unmodified vertex
// index (r0.x, which the translator loads with the guest index of the vertex). Then a draw only reads the vertices
// between its lowest and highest index, and only that part of each vertex buffer has to be copied. CoD keeps large
// shared vertex buffers (up to 16 MB) that it partly rewrites every frame; copying them whole was ~330 MB per frame
// in the front end. Anything the walk can't follow (r0 written or a jump, loop or call before a full fetch, an index
// from another register, relative addressing) makes the shader "unsafe", and its buffers are copied whole as before.
class VertexIndexAnalyzer : public rex::graphics::ShaderTranslator {
 public:
  bool safe() const { return safe_ && fetches_ > 0; }

 protected:
  void Reset() override {
    ShaderTranslator::Reset();
    safe_ = true;
    r0_written_ = false;
    flow_ = false;
    fetches_ = 0;
  }
  void ProcessLoopStartInstruction(const rex::graphics::ParsedLoopStartInstruction&) override { flow_ = true; }
  void ProcessLoopEndInstruction(const rex::graphics::ParsedLoopEndInstruction&) override { flow_ = true; }
  void ProcessCallInstruction(const rex::graphics::ParsedCallInstruction&) override { flow_ = true; }
  void ProcessReturnInstruction(const rex::graphics::ParsedReturnInstruction&) override { flow_ = true; }
  void ProcessJumpInstruction(const rex::graphics::ParsedJumpInstruction&) override { flow_ = true; }
  void ProcessVertexFetchInstruction(const rex::graphics::ParsedVertexFetchInstruction& instr) override {
    if (!instr.is_mini_fetch) {
      ++fetches_;
      const auto& index = instr.operands[0];
      if (r0_written_ || flow_ || instr.operand_count < 1 ||
          index.storage_source != rex::graphics::InstructionStorageSource::kRegister ||
          index.storage_index != 0 ||
          index.storage_addressing_mode != rex::graphics::InstructionStorageAddressingMode::kAbsolute ||
          index.is_negated || index.is_absolute_value ||
          index.GetComponent(0) != rex::graphics::SwizzleSource::kX) {
        safe_ = false;
      }
    }
    NoteResult(instr.result);
  }
  void ProcessTextureFetchInstruction(const rex::graphics::ParsedTextureFetchInstruction& instr) override {
    NoteResult(instr.result);
  }
  void ProcessAluInstruction(const rex::graphics::ParsedAluInstruction& instr,
                             uint8_t /*memexport_eM_potentially_written_before*/) override {
    NoteResult(instr.vector_and_constant_result);
    NoteResult(instr.scalar_result);
  }

 private:
  void NoteResult(const rex::graphics::InstructionResult& result) {
    if (result.storage_target == rex::graphics::InstructionStorageTarget::kRegister &&
        result.original_write_mask &&
        (result.storage_index == 0 ||
         result.storage_addressing_mode != rex::graphics::InstructionStorageAddressingMode::kAbsolute)) {
      r0_written_ = true;
    }
  }
  bool safe_ = true;
  bool r0_written_ = false;
  bool flow_ = false;
  uint32_t fetches_ = 0;
};

// What the vertex copy needs to know about a vertex shader (bo1-nx).
struct VertexShaderRanges {
  bool index_safe = false;
  struct Binding {
    uint32_t fetch_constant;
    uint32_t stride_bytes;
    // Bytes past the start of a vertex that its attributes read (largest offset plus 16, the largest format).
    uint32_t vertex_extent;
    bool usable;  // no negative attribute offsets
  };
  std::vector<Binding> bindings;
};

// Four: with three, a single frame of heavy GPU work stalled the ring thread
// at once (30-49 ms waits for a free slot in frames that then dropped).
constexpr uint32_t kSubmissionSlots = 4;
// Vertex, index, constant and texture upload data of the submissions in flight.
constexpr VkDeviceSize kStreamSize = 160u << 20;
// Room at the end of the stream for the fixed ranges of constant bindings.
constexpr VkDeviceSize kStreamGuard = 16u << 10;
// Past the end of the stream, never allocated from but inside the range the
// shaders' vertex fetches may address: a fetch past its copied data (a guest
// index out of range) reads garbage there instead of faulting the GPU's MMU,
// which loses the device for good.
constexpr VkDeviceSize kStreamSlack = 16u << 20;
// Sizes of the constant buffer bindings. The float ones are bound with their
// maximum size and filled up to the shader's count.
constexpr VkDeviceSize kFloatConstantsBytes = 256 * 4 * sizeof(float);
constexpr VkDeviceSize kBoolLoopConstantsBytes = sizeof(uint32_t) * (8 + 32);
constexpr VkDeviceSize kFetchConstantsBytes = sizeof(uint32_t) * 6 * 32;

VkFormat ColorFormatToVk(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case F::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case F::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

bool IsColorFormat64bpp(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  return format == F::k_16_16_16_16 || format == F::k_16_16_16_16_FLOAT ||
         format == F::k_32_32_FLOAT;
}

// Depth buffers are D32_SFLOAT_S8_UINT unless bo1_native_depth24 stores the
// game's unorm 24-bit ones as D24_UNORM_S8_UINT and its float 24-bit ones as
// D32_SFLOAT (see DepthFormatFor). A resolve turns depth into the R32_SFLOAT
// the translated shaders sample k_24_8 as, and into the guest's memory words
// (see Resolve).
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

VkBlendFactor BlendFactorToVk(xenos::BlendFactor factor) {
  switch (uint32_t(factor)) {
    case 0:
      return VK_BLEND_FACTOR_ZERO;
    case 1:
      return VK_BLEND_FACTOR_ONE;
    case 4:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case 5:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8:
      return VK_BLEND_FACTOR_DST_COLOR;
    case 9:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case 11:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12:
      return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14:
      return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16:
      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:
      return VK_BLEND_FACTOR_ZERO;
  }
}

VkBlendOp BlendOpToVk(xenos::BlendOp op) {
  switch (uint32_t(op)) {
    case 1:
      return VK_BLEND_OP_SUBTRACT;
    case 2:
      return VK_BLEND_OP_MIN;
    case 3:
      return VK_BLEND_OP_MAX;
    case 4:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:
      return VK_BLEND_OP_ADD;
  }
}

struct RenderTargetKey {
  uint32_t base_tiles;
  uint32_t pitch;
  uint32_t format;
  uint32_t msaa;
  uint32_t is_depth;
  bool operator==(const RenderTargetKey& o) const { return !std::memcmp(this, &o, sizeof(o)); }
};
struct RenderTargetKeyHasher {
  size_t operator()(const RenderTargetKey& k) const { return size_t(XXH3_64bits(&k, sizeof(k))); }
};

struct RenderTarget {
  RenderTargetKey key;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  // Depth only, for the compute pass that resolves depth.
  VkImageView sample_view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t width = 0;
  uint32_t height = 0;
  // bo1-nx: the tallest height draws and resolves asked for. With bo1_native_zcull a depth image is taller than that
  // (its full EDRAM range); DepthContainer must use what is really drawn.
  uint32_t used_height = 0;
  uint64_t last_used_frame = 0;
  uint64_t last_drawn_frame = 0;  // bo1-nx: last frame a pass rendered into it
  bool drawn_since_resolve = false;  // bo1-nx: a draw went into it after its last resolve
};

// The attachments of a rendering scope: color 0-3 and depth.
struct PassTargets {
  RenderTarget* color[4] = {};
  RenderTarget* depth = nullptr;
  bool operator==(const PassTargets& o) const {
    return !std::memcmp(this, &o, sizeof(o));
  }
  bool Empty() const { return !color[0] && !color[1] && !color[2] && !color[3] && !depth; }
};

// Everything a graphics pipeline depends on, hashed as plain memory.
struct PipelineKey {
  uint64_t vs_hash;
  uint64_t vs_modification;
  uint64_t ps_hash;
  uint64_t ps_modification;
  uint32_t color_formats[4];
  uint32_t depth_format;
  uint32_t samples;
  uint32_t topology;
  uint32_t geometry_shader;
  uint32_t primitive_restart;
  uint32_t cull_mode;
  uint32_t front_face_clockwise;
  uint32_t polygon_line;
  uint32_t depth_clamp;
  uint32_t depth_test;
  uint32_t depth_write;
  uint32_t depth_compare;
  uint32_t stencil_test;
  uint32_t stencil_front[4];
  uint32_t stencil_back[4];
  uint32_t blend[4];
  uint32_t write_masks;
  uint32_t texture_signs[8];
  uint64_t layout;
};

struct PipelineLayout {
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_pixel = VK_NULL_HANDLE;
};

struct VertexRangeKey {
  uint32_t address;
  uint32_t size;
  uint64_t hash;
  bool operator==(const VertexRangeKey& o) const {
    return address == o.address && size == o.size && hash == o.hash;
  }
};
struct VertexRangeKeyHasher {
  size_t operator()(const VertexRangeKey& k) const { return size_t(k.hash ^ k.address); }
};

}  // namespace

struct Renderer::Impl {
  Renderer* owner = nullptr;
  rex::ui::vulkan::VulkanProvider* provider = nullptr;
  const VulkanDevice* device = nullptr;
  VkDevice vk = VK_NULL_HANDLE;
  rex::memory::Memory* memory = nullptr;
  RegisterFile* regs = nullptr;

  // Shaders.
  std::unique_ptr<SpirvShaderTranslator> translator;
  rex::string::StringBuffer disasm;
  std::unordered_map<uint64_t, std::unique_ptr<VulkanShader>> shaders;
  struct KnownShader {
    VulkanShader* shader;
    uint64_t sample;  // of all of the code
    uint64_t verified_frame;
    uint64_t head;  // of the first 16 words
  };
  std::unordered_map<uint64_t, KnownShader> shaders_by_location;
  VulkanShader* active_vs = nullptr;
  VulkanShader* active_ps = nullptr;
  bool specialize_signs = false;

  // Layouts.
  VkDescriptorSetLayout layout_empty = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_shared = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_constants = VK_NULL_HANDLE;
  uint32_t shared_binding_count = 1;
  std::unordered_map<uint64_t, VkDescriptorSetLayout> texture_set_layouts;
  std::unordered_map<uint64_t, PipelineLayout> pipeline_layouts;

  // The stream: one host-visible buffer the shaders read vertices from (as
  // the translator's "shared memory"), with the indices, the constants and the
  // texture staging data in it too.
  VkBuffer stream_buffer = VK_NULL_HANDLE;
  VkDeviceMemory stream_memory = VK_NULL_HANDLE;
  uint8_t* stream_mapping = nullptr;
  uint64_t stream_head = 0;  // total bytes ever allocated
  VkDescriptorPool static_pool = VK_NULL_HANDLE;
  VkDescriptorSet set_shared = VK_NULL_HANDLE;
  VkDescriptorSet set_constants = VK_NULL_HANDLE;
  VkDeviceSize ubo_alignment = 256;

  // Submissions.
  struct Submission {
    VkCommandPool pool = VK_NULL_HANDLE;
    // The main command buffer's own: the setup one is recorded on the ring
    // thread while the recording thread records this one.
    VkCommandPool main_pool = VK_NULL_HANDLE;
    VkCommandBuffer setup = VK_NULL_HANDLE;
    VkCommandBuffer main = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    uint64_t number = 0;
    uint64_t frame = 0;
    // Oldest stream position the submission reads.
    uint64_t stream_oldest = 0;
    std::vector<VkDescriptorPool> descriptor_pools;
    size_t descriptor_pool_used = 0;
    bool timed = false;
  };
  VkQueryPool timestamps = VK_NULL_HANDLE;
  float timestamp_period = 1.0f;
  Submission submissions[kSubmissionSlots];
  uint64_t submission_number = 0;  // the one being recorded, if recording
  bool recording = false;
  uint64_t frame = 1;
  uint64_t completed_frame = 0;
  // Whether render targets get the game's sample count (bo1_native_msaa).
  // The targets are sized in guest pixels whatever the sample count, so one
  // sample changes only the images, the pipelines and the resolves.
  bool multisample = true;
  // D24_UNORM_S8_UINT for the game's kD24S8 buffers (bo1_native_depth24).
  bool depth24 = false;
  // The float24 ones (here only shadow maps) become D32_SFLOAT without
  // stencil, 32 instead of 64 bits per pixel; a stencil test on them passes.
  VkFormat DepthFormatFor(uint32_t guest_format) const {
    if (!depth24) return kDepthFormat;
    return guest_format == uint32_t(xenos::DepthRenderTargetFormat::kD24S8)
               ? VK_FORMAT_D24_UNORM_S8_UINT
               : VK_FORMAT_D32_SFLOAT;
  }
  static bool HasStencil(VkFormat format) { return format != VK_FORMAT_D32_SFLOAT; }
  VkDescriptorSetLayout depth_convert_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout depth_convert_layout = VK_NULL_HANDLE;
  VkPipeline depth_convert_pipeline = VK_NULL_HANDLE;
  VkDescriptorPool depth_convert_pool = VK_NULL_HANDLE;
  VkDescriptorSet depth_convert_set = VK_NULL_HANDLE;
  // Depth straight from the depth buffer into the R32_SFLOAT image, in one
  // compute pass (depth_to_float.cs.glsl).
  VkDescriptorSetLayout depth_copy_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout depth_copy_layout = VK_NULL_HANDLE;
  VkPipeline depth_copy_pipeline = VK_NULL_HANDLE;
  // Color resolves by compute (copy_color.cs.glsl), same set layout.
  VkPipelineLayout copy_color_layout = VK_NULL_HANDLE;
  VkPipeline copy_color_pipeline = VK_NULL_HANDLE;
  bool ComputeColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest, int32_t sx,
                        int32_t sy, int32_t dx, int32_t dy, uint32_t width, uint32_t height);
  bool CreateDepthConversion();

  // GPU time by render pass and resolve (bo1_native_gpu_profile): on one
  // frame in 16, a timestamp is written where each pass or resolve begins,
  // and the time to the next one is charged to it.
  static constexpr uint32_t kProfileQueries = 512;
  VkQueryPool profile_pool = VK_NULL_HANDLE;
  bool profile_frame = false;
  // bo1-nx debug (bo1_debug_trace_frame): this frame's passes, resolves and skips go to the log.
  bool trace_frame = false;
  uint32_t dump_swaps_left = 0;
  uint32_t trace_pass_draws = 0;
  uint32_t trace_skip_logged = 0;
  uint64_t clear_redirects = 0;
  uint32_t trace_draws_detailed = 0;
  // Texture fetches looked up in the traced frame, by format and layout: count and one example.
  std::map<std::string, std::pair<uint32_t, std::string>> trace_texture_formats;
  struct TraceTextureDump {
    VkImage image;
    uint32_t width, height;
    VkFormat format;
    std::string name;
  };
  std::vector<TraceTextureDump> trace_texture_dumps;
  std::unordered_set<uint32_t> trace_texture_dumped;
  std::unordered_map<uint32_t, uint32_t> trace_texture_quota;
  bool trace_mips_dumped = false;
  // bo1-nx debug (bo1_debug_mip_watch): mip data of large DXT textures. Real DXT blocks measure ~6 bits of entropy
  // per byte; the noise in BO1's mips measured ~7.1, like still-compressed data.
  struct MipWatch {
    uint32_t mip_address = 0;
    uint32_t bytes = 0;
    uint64_t first_frame = 0;
    float first_entropy = 0;
    uint64_t hash = 0;
    uint32_t changes = 0;
    float last_entropy = 0;
  };
  std::unordered_map<uint32_t, MipWatch> mip_watch;  // by base address
  static float Entropy(const uint8_t* p, size_t n) {
    uint32_t counts[256] = {};
    for (size_t i = 0; i < n; ++i) ++counts[p[i]];
    double e = 0;
    for (uint32_t c : counts) {
      if (c) {
        const double q = double(c) / double(n);
        e -= q * std::log2(q);
      }
    }
    return float(e);
  }
  void WatchMips(const xenos::xe_gpu_texture_fetch_t& f) {
    const uint32_t format = uint32_t(f.format);
    if ((format != 18 && format != 19 && format != 20 && format != 49) || !f.mip_address ||
        f.size_2d.width + 1 < 256 || f.dimension != xenos::DataDimension::k2DOrStacked) {
      return;
    }
    const uint32_t base = f.base_address << 12;
    auto [it, inserted] = mip_watch.try_emplace(base);
    MipWatch& w = it->second;
    if (inserted) {
      // Level 1 alone: a quarter of level 0's blocks.
      const uint32_t blocks = ((f.size_2d.width + 1) / 8) * ((f.size_2d.height + 1) / 8);
      w.bytes = std::min<uint32_t>(blocks * (format == 18 ? 8 : 16), 0x10000);
      w.mip_address = f.mip_address << 12;
      w.first_frame = frame;
      const uint8_t* p = memory->TranslatePhysical<const uint8_t*>(w.mip_address);
      w.first_entropy = w.last_entropy = Entropy(p, w.bytes);
      w.hash = XXH3_64bits(p, w.bytes);
      return;
    }
    if (frame % 120 != (base >> 12) % 120) return;
    const uint8_t* p = memory->TranslatePhysical<const uint8_t*>(w.mip_address);
    const uint64_t hash = XXH3_64bits(p, w.bytes);
    if (hash != w.hash) {
      const float e = Entropy(p, w.bytes);
      if (w.changes < 3) {
        REXLOG_INFO("[mipwatch] {:08X} mips {:08X} changed at frame {} (first seen {}): entropy {:.2f} -> {:.2f}",
                    base, w.mip_address, frame, w.first_frame, w.last_entropy, e);
      }
      w.hash = hash;
      w.last_entropy = e;
      ++w.changes;
    }
  }
  void ReportMipWatch() {
    uint32_t total = 0, noisy_first = 0, noisy_now = 0, changed = 0;
    for (const auto& [base, w] : mip_watch) {
      ++total;
      noisy_first += w.first_entropy > 6.8f;
      noisy_now += w.last_entropy > 6.8f;
      changed += w.changes != 0;
    }
    REXLOG_INFO("[mipwatch] {} textures: {} looked compressed at first sight, {} now; {} changed after first sight",
                total, noisy_first, noisy_now, changed);
  }
  uint32_t trace_raw_mip_address = 0, trace_raw_base_address = 0;
  uint64_t last_frame_draw_count = 0;
  std::unordered_set<uint64_t> trace_disassembled;
  // bo1-nx: the exponent bias of the last color resolve to each destination (by 4 KB page address). The resolve
  // copies without scaling; the bias is added to the exp_adjust of the texture fetch constants that sample the
  // destination, which the translated shaders apply to the fetched value (exact apart from the hardware's clamp).
  // BO1's exposure chain resolves its 16_16_16_16 targets with bias 11 and samples them back with -11: ignoring
  // the bias read the values 2048 times too small, zero after a few passes, and the whole frame came out black.
  std::unordered_map<uint32_t, int32_t> resolve_exp_bias;
  void NoteResolveExpBias(uint32_t dest_base, xenos::ColorRenderTargetFormat source_format) {
    const uint32_t mode = REXCVAR_GET(bo1_native_resolve_exp_bias);
    int32_t bias = regs->Get<reg::RB_COPY_DEST_INFO>().copy_dest_exp_bias;
    if (mode == 2 && (source_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                      source_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)) {
      bias += 5;
    }
    const uint32_t page = dest_base & 0x1FFFF000;
    if (!mode || !bias) {
      resolve_exp_bias.erase(page);
    } else {
      resolve_exp_bias[page] = bias;
    }
  }
  uint32_t trace_pass_skips[32] = {};
  // Depth state of the pass's draws: "c" color written or "d" depth only, z test on/off, z write, compare function.
  std::map<std::string, uint32_t> trace_pass_depth;
  void TraceEndPass() {
    if (!trace_frame) return;
    std::string skips;
    for (uint32_t i = 0; i < 32; ++i) {
      if (trace_pass_skips[i]) skips += fmt::format(" {}:{}", i, trace_pass_skips[i]);
    }
    std::string depth;
    for (const auto& [key, n] : trace_pass_depth) depth += fmt::format(" {}x{}", key, n);
    REXLOG_INFO("[trace]   {} draws, skipped{}; depth state{}", trace_pass_draws, skips.empty() ? " none" : skips,
                depth.empty() ? " -" : depth);
    trace_pass_depth.clear();
    trace_pass_draws = 0;
    std::memset(trace_pass_skips, 0, sizeof(trace_pass_skips));
  }
  // The destination of the last resolve of the traced frame, saved by DebugDumpResolve.
  ResolveDestination* trace_last_dest = nullptr;
  uint32_t trace_dumps = 0;
  // Copies a single-sampled image back and writes <bo1_debug_capture_dir>/<name>.img: a 16-byte header (width,
  // height, VkFormat, bytes per pixel; little endian), then the rows. tools/rgbx_to_png.py converts it.
  void DebugDumpImage(VkImage image, uint32_t width, uint32_t height, VkFormat format, const std::string& name,
                      uint32_t level = 0) {
    const std::string dir = REXCVAR_GET(bo1_debug_capture_dir);
    std::string host = REXCVAR_GET(bo1_debug_dump_host);
    // Without a dump host, the machine receiving the network log (port 28772): the netloader cuts the arguments
    // at about 256 characters, so a second address did not fit.
    const std::string log_host = REXCVAR_GET(log_network);
    if (host.empty() && dir.empty() && !log_host.empty()) {
      host = log_host.substr(0, log_host.rfind(':')) + ":28772";
    }
    if ((dir.empty() && host.empty()) || !image || !width || !height) return;
    uint32_t bpp = 0;
    switch (format) {
      case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM:
      case VK_FORMAT_R16G16_SFLOAT:
        bpp = 4;
        break;
      case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R32G32_SFLOAT:
        bpp = 8;
        break;
      case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R8G8_UNORM:
        bpp = 2;
        break;
      // Block compressed: "bpp" is bytes per 4x4 block; the copy below works in texels either way.
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK:
        bpp = 8;
        break;
      case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
        bpp = 16;
        break;
      default:
        REXLOG_INFO("[trace]   (no dump of {}: format {})", name, int(format));
        return;
    }
    const bool blocks = format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
    const VkDeviceSize size = blocks ? VkDeviceSize((width + 3) / 4) * ((height + 3) / 4) * bpp
                                     : VkDeviceSize(width) * height * bpp;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rex::ui::vulkan::util::MemoryPurpose::kReadback,
            buffer, buffer_memory)) {
      return;
    }
    BeginRecording();
    EndPass();
    VkCommandBuffer c = cmd();
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT};
    dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                               &barrier, 0, nullptr, 0, nullptr);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {width, height, 1};
    dfn().vkCmdCopyImageToBuffer(c, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    Submit();
    vkDeviceWaitIdle(vk);
    void* mapping = nullptr;
    if (dfn().vkMapMemory(vk, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS && !host.empty()) {
      const uint32_t header[4] = {width, height, uint32_t(format), bpp};
      if (SendDump(host, name + ".img", header, mapping, size_t(size))) {
        REXLOG_INFO("[trace]   sent {}x{} format {} as {}.img to {}", width, height, int(format), name, host);
      }
      dfn().vkUnmapMemory(vk, buffer_memory);
    } else if (mapping) {
      const std::string path = fmt::format("{}/{}.img", dir, name);
      if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        const uint32_t header[4] = {width, height, uint32_t(format), bpp};
        std::fwrite(header, sizeof(header), 1, f);
        std::fwrite(mapping, 1, size_t(size), f);
        std::fclose(f);
        REXLOG_INFO("[trace]   dumped {}x{} format {} to {}", width, height, int(format), path);
      }
      dfn().vkUnmapMemory(vk, buffer_memory);
    }
    dfn().vkDestroyBuffer(vk, buffer, nullptr);
    dfn().vkFreeMemory(vk, buffer_memory, nullptr);
  }
  // One TCP connection per dump: a 64-byte name, the 16-byte header, then the data.
  static bool SendDump(const std::string& host, const std::string& name, const uint32_t header[4], const void* data,
                       size_t size) {
    const size_t colon = host.rfind(':');
    if (colon == std::string::npos) return false;
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(std::atoi(host.c_str() + colon + 1)));
    if (inet_pton(AF_INET, host.substr(0, colon).c_str(), &address.sin_addr) != 1) return false;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    auto send_all = [&](const void* p, size_t n) {
      const char* c = static_cast<const char*>(p);
      while (ok && n) {
        const ssize_t sent = send(fd, c, n, 0);
        if (sent <= 0) {
          ok = false;
          break;
        }
        c += sent;
        n -= size_t(sent);
      }
    };
    char name_field[64] = {};
    std::snprintf(name_field, sizeof(name_field), "%s", name.c_str());
    send_all(name_field, sizeof(name_field));
    send_all(header, 16);
    // Through a plain heap buffer, in 64 KB pieces: Horizon's socket service can't read GPU-mapped memory (the
    // send failed and only the name and header arrived).
    std::vector<char> bounce(64 << 10);
    for (size_t offset = 0; ok && offset < size; offset += bounce.size()) {
      const size_t n = std::min(bounce.size(), size - offset);
      std::memcpy(bounce.data(), static_cast<const char*>(data) + offset, n);
      send_all(bounce.data(), n);
    }
    close(fd);
    return ok;
  }
  void DebugDumpResolve() {
    if (!trace_frame || !trace_last_dest) return;
    ResolveDestination* d = trace_last_dest;
    trace_last_dest = nullptr;
    DebugDumpImage(d->image, d->width, d->height, d->format,
                   fmt::format("f{}_r{:02d}_{:08X}", frame, trace_dumps++, d->address));
  }
  static std::string TraceTarget(const RenderTarget* rt) {
    if (!rt) return "-";
    return fmt::format("{}{}@{} p{} {}x{} ms{}", rt->key.is_depth ? "D" : "C", rt->key.format, rt->key.base_tiles,
                       rt->key.pitch, rt->width, rt->height, rt->key.msaa);
  }
  struct ProfileSegment {
    uint64_t key;
    uint32_t draws;
  };
  std::vector<ProfileSegment> profile_segments[kSubmissionSlots];
  bool profile_closed[kSubmissionSlots] = {};
  struct ProfileTotal {
    double ns = 0;
    uint64_t count = 0;
    uint64_t draws = 0;
  };
  std::unordered_map<uint64_t, ProfileTotal> profile_totals;
  uint32_t profiled_frames = 0;
  double profiled_ns = 0;
  // Kinds, in the key's top 4 bits.
  enum : uint64_t { kProfileOther = 0, kProfilePass = 1, kProfileResolve = 2 };
  static uint64_t ProfileTargetBits(const RenderTarget* rt) {
    if (!rt) return 0xFFFFFF;
    return (uint64_t(rt->key.base_tiles & 0xFFF) << 12) | (uint64_t(rt->key.pitch & 0x7FF) << 1) |
           (rt->key.format & 1);
  }
  void ProfileMark(uint64_t key) {
    if (!profile_frame || !profile_pool || !recording) return;
    uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
    auto& segments = profile_segments[slot];
    // The last query is kept for the closing timestamp.
    if (segments.size() + 1 >= kProfileQueries) return;
    dfn().vkCmdWriteTimestamp(cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_pool,
                              slot * kProfileQueries + uint32_t(segments.size()));
    segments.push_back({key, 0});
  }
  void ProfileDraw() {
    if (!profile_frame) return;
    auto& segments = profile_segments[submission_number % kSubmissionSlots];
    if (!segments.empty()) ++segments.back().draws;
  }
  void ProfileCollect(uint32_t slot);
  struct TextureTimer {
    uint64_t& total;
    uint64_t start;
    explicit TextureTimer(uint64_t& t) : total(t), start(NowNs()) {}
    ~TextureTimer() { total += NowNs() - start; }
  };
  void ProfileReport();
  uint32_t HostMsaa(xenos::MsaaSamples guest) const {
    return multisample ? uint32_t(guest) : 0;
  }
  bool null_images_initialized = false;

  // Render targets.
  std::unordered_map<RenderTargetKey, std::unique_ptr<RenderTarget>, RenderTargetKeyHasher>
      render_targets;
  bool in_pass = false;
  PassTargets pass;
  VkExtent2D pass_extent = {};

  // Pipelines.
  std::unordered_map<uint64_t, VkPipeline> pipelines;
  std::unordered_map<uint32_t, VkShaderModule> geometry_shaders;
  VkPipelineCache driver_cache = VK_NULL_HANDLE;
  std::filesystem::path driver_cache_path;
  uint64_t pipelines_at_last_save = 0;
  std::chrono::steady_clock::time_point last_pipeline_time;
  size_t driver_cache_saved_size = 0;
  std::atomic<bool> driver_cache_writing{false};
  std::thread driver_cache_writer;

  // Frame time tracking.
  std::chrono::steady_clock::time_point last_swap_time;
  RendererStats at_last_swap;

  // What the main command buffer has bound.
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout bound_layout = VK_NULL_HANDLE;
  bool bound_static_sets = false;
  VkViewport bound_viewport = {};
  VkRect2D bound_scissor = {};
  float bound_depth_bias[2] = {NAN, NAN};
  float bound_blend_constants[4] = {NAN, NAN, NAN, NAN};
  uint32_t bound_stencil[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};

  // Vertex and index data already in the stream: stream position by the
  // range and its content.
  struct RangeCopy {
    VertexRangeKey key = {};
    uint64_t position = 0;
  };
  FlatMap64<RangeCopy> vertex_ranges;
  static uint64_t RangeSlot(const VertexRangeKey& k) {
    uint64_t h = k.hash ^ ((uint64_t(k.address) << 32) | k.size);
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
    return h == ~uint64_t(0) ? 0 : h;
  }
  // The stream position of a copy of this range, or UINT64_MAX.
  uint64_t FindRange(const VertexRangeKey& k) {
    RangeCopy* c = vertex_ranges.find(RangeSlot(k));
    return c && c->key == k ? c->position : UINT64_MAX;
  }
  void AddRange(const VertexRangeKey& k, uint64_t position) {
    vertex_ranges[RangeSlot(k)] = {k, position};
  }

  TextureCache textures;
  UploadTarget upload;

  // Texture descriptor sets, by content: the game uses the same few hundred
  // combinations of textures and samplers every frame, and allocating and
  // writing a set per draw (then freeing them one by one on pool reset) was
  // an eighth of the renderer's time.
  std::unordered_map<uint64_t, VkDescriptorSet> texture_set_cache;
  std::vector<VkDescriptorPool> set_pools;
  size_t set_pool_used = 0;
  struct RetiredPools {
    uint64_t frame;
    std::vector<VkDescriptorPool> pools;
  };
  std::vector<RetiredPools> retired_set_pools;
  uint64_t texture_views_epoch = 0;

  // Vertex buffer content hashes, computed once per frame for each range: many
  // draws share one buffer.
  struct VertexHash {
    uint64_t frame = UINT64_MAX;
    uint64_t hash = 0;
    // Last real content hash, and how many checks in a row found it changed.
    uint64_t content = 0;
    uint32_t changes = 0;
    ChangeDetector detector;
  };
  // This frame's hash of a vertex or index range. Data the game rewrites
  // every frame (skinned meshes, particles) was hashed in full every frame
  // only to be copied anyway, as the hash never matched: once a range has
  // changed on 4 checks in a row it gets a hash unique to the frame without
  // reading it (still copied once per frame, shared by its draws), and is
  // checked for real every 16 frames to notice it settling.
  uint64_t RangeHash(VertexHash& known, const uint8_t* data, uint32_t bytes, uint32_t address) {
    if (known.frame == frame) return known.hash;
    known.frame = frame;
    if (known.changes >= 4 && (frame & 15)) {
      known.hash = (frame + 1) * 0xD1B54A32D192ED03ull ^ (uint64_t(address) << 20) ^ bytes;
      return known.hash;
    }
    const bool measure_time = REXCVAR_GET(bo1_native_hash_counters);
    const uint64_t hash_start = measure_time ? NowNs() : 0;
    uint64_t content = known.detector.Check(data, bytes, frame, address >> 4, geometry_hash_bytes);
    if (measure_time) geometry_hash_ns += NowNs() - hash_start;
    known.changes = content != known.content ? std::min(known.changes + 1, 8u) : 0;
    known.content = content;
    known.hash = content;
    return content;
  }
  uint64_t geometry_hash_ns = 0;
  uint64_t geometry_hash_bytes = 0;
  uint64_t vertex_copy_ns = 0;
  FlatMap64<VertexHash> vertex_hashes;

  // Swap.
  VkDescriptorSetLayout swap_layout_ramp = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_layout_source = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_layout_dest = VK_NULL_HANDLE;
  VkPipelineLayout swap_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline swap_pipeline = VK_NULL_HANDLE;
  VkBuffer ramp_buffer = VK_NULL_HANDLE;
  VkDeviceMemory ramp_memory = VK_NULL_HANDLE;
  uint8_t* ramp_mapping = nullptr;
  VkBufferView ramp_views[kSubmissionSlots] = {};
  VkDescriptorPool swap_pool = VK_NULL_HANDLE;
  VkDescriptorSet swap_sets[kSubmissionSlots][3] = {};
  uint32_t gamma_ramp[256] = {};

  SpirvShaderTranslator::SystemConstants system_constants = {};

  struct DrawSetup {
    bool valid = false;
    uint64_t state_generation = 0;
    uint64_t fetch_generation = 0;
    uint64_t rt_generation = 0;
    VulkanShader* active_vs = nullptr;
    VulkanShader* active_ps = nullptr;
    uint32_t initiator_state = 0;
    uint32_t skip_reason = UINT32_MAX;
    RenderTarget* alias_container = nullptr;  // bo1-nx: see bo1_native_skip_aliased_depth
    bool shadow_map = false;                   // bo1-nx: see bo1_native_skip_shadow_maps
    bool prepass = false;                      // bo1-nx: see bo1_native_skip_prepass
    VulkanShader* vs = nullptr;
    VulkanShader* ps = nullptr;
    bool primitive_polygonal = false;
    reg::RB_DEPTHCONTROL depth_control = {};
    bool convert_fan = false, convert_loop = false, strip = false, reset_enabled = false;
    draw_util::Scissor scissor = {};
    draw_util::ViewportInfo viewport = {};
    PassTargets targets;
    const PipelineLayout* layout = nullptr;
    uint32_t counts[4] = {};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport vp = {};
    float bias_constant = 0.0f, bias_slope = 0.0f;
    float blend_constants[4] = {};
    uint32_t stencil[6] = {};
  };
  DrawSetup setup;
  std::unordered_map<uint64_t, DrawSetup> setup_cache;
  uint64_t setup_cache_rt_generation = 0;
  // Counts setup rebuilds, so derived caches can tell a setup apart.
  uint64_t setup_generation = 0;
  struct SystemInputs {
    uint64_t setup_generation = ~0ull;
    uint64_t fetch_generation = 0;
    uint64_t index_generation = 0;
    uint32_t used_mask = 0;
  };
  SystemInputs system_inputs;
  struct RetiredRenderTarget {
    uint64_t frame;
    VkImage image;
    VkImageView view;
    VkImageView sample_view;
    VkDeviceMemory memory;
  };
  std::vector<RetiredRenderTarget> retired_render_targets;
  // Staging for depth resolves (depth to buffer to color image).
  static constexpr VkDeviceSize kDepthScratchSize = 16u << 20;
  VkBuffer depth_scratch = VK_NULL_HANDLE;
  VkDeviceMemory depth_scratch_memory = VK_NULL_HANDLE;
  // Bumped whenever a render target is created, as setups hold pointers.
  uint64_t rt_generation = 1;
  bool BuildDrawSetup(uint32_t initiator_value);
  // bo1-nx: BO1's GPU memcpy, a point list whose vertex shader (kBo1MemcpyVs) reads 64 bytes per vertex from vertex
  // fetch 0 and memexports them as 8 elements of 8 bytes (k_16_16_16_16) to the stream in c0, at element index
  // c0.y + (c255.w * i) * c255.y and, for the second half, + c255.z. Its texture streamer copies low mips with it;
  // skipped, the mips kept stale data (the rainbow noise on distant surfaces). Done here on the CPU, in ring order.
  static constexpr uint64_t kBo1MemcpyVs = 0xA4827826E2589916ull;
  void EmulateMemcpyExport(const reg::VGT_DRAW_INITIATOR& initiator, VulkanShader* vs);
  uint64_t memexport_copies = 0, memexport_bytes = 0;

  struct TextureBindings {
    bool valid = false;
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint64_t views_epoch = 0;
    const VulkanShader* vs = nullptr;
    const VulkanShader* ps = nullptr;
    const PipelineLayout* layout = nullptr;
    uint32_t used_mask = 0;
    uint32_t slots = 0;
    VkDescriptorSet sets[2] = {};
    // Reusable in later frames through this one: no texture behind it is due
    // a content check before then. The textures are marked used on reuse.
    uint64_t valid_until = 0;
    uint64_t set_generation = 0;
    uint64_t answers_epoch = 0;
    // The views the sets hold, to check them alive when views_epoch moved.
    uint32_t view_count = 0;
    VkImageView views[24] = {};
    uint64_t view_ids[24] = {};
    uint32_t texture_count = 0;
    void* textures[24] = {};
  };
  FlatMap64<TextureBindings> tb_cache;

  // What each fetch slot last resolved to, this frame and submission: most
  // binding rebuilds change one or two slots of several.
  struct SlotViewEntry {
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint64_t views_epoch = 0;
    uint32_t words[6] = {};
    uint32_t variant = 0;
    VkImageView view = VK_NULL_HANDLE;
    void* texture = nullptr;
    uint64_t valid_until = 0;
  };
  SlotViewEntry slot_views[32][8];
  // Set by SlotView: the texture behind its answer and how long it holds.
  void* slot_texture = nullptr;
  uint64_t slot_valid_until = 0;
  struct SlotSamplerEntry {
    bool valid = false;
    uint32_t words[6] = {};
    uint32_t binding_bits = 0;
    VkSampler sampler = VK_NULL_HANDLE;
  };
  SlotSamplerEntry slot_samplers[32][4];

  VkImageView SlotView(uint32_t slot, xenos::FetchOpDimension dimension, bool is_signed) {
    const uint32_t* words = &(*regs)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                     slot * 6];
    uint32_t variant = uint32_t(dimension) | (uint32_t(is_signed) << 2);
    SlotViewEntry& e = slot_views[slot][variant & 7];
    if (e.frame == frame && e.submission == submission_number &&
        e.views_epoch == textures.views_epoch() && e.variant == variant &&
        !std::memcmp(e.words, words, sizeof(e.words))) {
      slot_texture = e.texture;
      slot_valid_until = e.valid_until;
      return e.view;
    }
    VkImageView view =
        textures.GetView(regs->GetTextureFetch(slot), dimension, is_signed, upload);
    if (REXCVAR_GET(bo1_debug_mip_watch)) WatchMips(regs->GetTextureFetch(slot));
    if (trace_frame) {
      xenos::xe_gpu_texture_fetch_t f = regs->GetTextureFetch(slot);
      const std::string key = fmt::format(
          "fmt {:2} dim {} tiled {} packed_mips {} endian {} mips {}-{} signs {}{}{}{} {}", uint32_t(f.format),
          uint32_t(f.dimension), uint32_t(f.tiled), uint32_t(f.packed_mips), uint32_t(f.endianness),
          uint32_t(f.mip_min_level), uint32_t(f.mip_max_level), uint32_t(f.sign_x), uint32_t(f.sign_y),
          uint32_t(f.sign_z), uint32_t(f.sign_w), view ? "" : "(no view)");
      auto& entry = trace_texture_formats[key];
      if (entry.second.empty()) {
        entry.second = fmt::format("{:08X} mips at {:08X} {}x{} pitch {}", f.base_address << 12, f.mip_address << 12,
                                   f.size_2d.width + 1, f.size_2d.height + 1, uint32_t(f.pitch));
      }
      ++entry.first;
      // Queue level 0 of the formats under suspicion for saving after the frame.
      const uint32_t fmt_id = uint32_t(f.format);
      const uint32_t address = f.base_address << 12;
      static const std::unordered_map<uint32_t, uint32_t> kDumpQuota = {{18, 4}, {19, 3}, {20, 4}, {49, 4}};
      auto quota = kDumpQuota.find(fmt_id);
      if (quota != kDumpQuota.end() && f.size_2d.width >= 127 && trace_texture_quota[fmt_id] < quota->second &&
          trace_texture_dumped.insert(address).second) {
        VkFormat host_format = VK_FORMAT_UNDEFINED;
        VkImage image = TextureCache::DebugTextureImage(textures.last_texture(), &host_format);
        if (image) {
          if (fmt_id == 18 && f.size_2d.width + 1 == 512 && !trace_raw_mip_address) {
            trace_raw_mip_address = f.mip_address << 12;
            trace_raw_base_address = f.base_address << 12;
          }
          ++trace_texture_quota[fmt_id];
          trace_texture_dumps.push_back({image, f.size_2d.width + 1u, f.size_2d.height + 1u, host_format,
                                         fmt::format("f{}_tex_fmt{}_{:08X}", frame, fmt_id, address)});
        }
      }
    }
    e.texture = slot_texture = textures.last_texture();
    e.valid_until = slot_valid_until = textures.last_valid_until();
    e.frame = frame;
    e.submission = submission_number;
    e.views_epoch = textures.views_epoch();
    e.variant = variant;
    std::memcpy(e.words, words, sizeof(e.words));
    e.view = view;
    return view;
  }

  VkSampler SlotSampler(const SpirvShader::SamplerBinding& binding) {
    uint32_t slot = binding.fetch_constant;
    const uint32_t* words = &(*regs)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                     slot * 6];
    uint32_t bits = uint32_t(binding.mag_filter) | (uint32_t(binding.min_filter) << 2) |
                    (uint32_t(binding.mip_filter) << 4) |
                    (uint32_t(binding.aniso_filter) << 6);
    SlotSamplerEntry* entries = slot_samplers[slot];
    for (uint32_t i = 0; i < 4; ++i) {
      SlotSamplerEntry& e = entries[i];
      if (e.valid && e.binding_bits == bits && !std::memcmp(e.words, words, sizeof(e.words))) {
        return e.sampler;
      }
    }
    VkSampler sampler = textures.GetSampler(regs->GetTextureFetch(slot), binding);
    // Samplers are never destroyed, so these stay valid across frames.
    SlotSamplerEntry& e = entries[(bits ^ words[0]) & 3];
    e.valid = true;
    e.binding_bits = bits;
    std::memcpy(e.words, words, sizeof(e.words));
    e.sampler = sampler;
    return sampler;
  }
  uint64_t tb_cache_frame = 0;

  // The fetch constants as the shaders get them: the guest's, with vertex
  // buffer addresses pointing into the stream.
  std::array<uint32_t, 6 * 32> fetch_words = {};
  std::array<uint64_t, 96> fetch_resolved_frame = {};
  uint64_t fetch_words_frame = 0;
  uint64_t fetch_words_submission = 0;
  uint32_t last_fetch_words[6 * 32] = {};

  // bo1-nx: vertex copies limited to the range of vertices a draw reads (see VertexIndexAnalyzer).
  std::unordered_map<const Shader*, VertexShaderRanges> vs_ranges;
  VertexIndexAnalyzer vs_analyzer;
  // What part of each fetch slot was copied this frame, and the fetch word that points at it.
  struct SlotRange {
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint32_t base = 0;   // guest address of the buffer
    uint32_t bytes = 0;  // its size
    uint32_t start = 0;  // copied bytes [start, end) of the buffer
    uint32_t end = 0;
    uint32_t dword0 = 0;
  };
  std::array<SlotRange, 96> slot_ranges = {};
  // Lowest and highest index of guest index buffers, by address and size, valid while the content hash matches.
  struct IndexBounds {
    uint64_t hash = 0;
    uint32_t min = 0;
    uint32_t max = 0;
  };
  FlatMap64<IndexBounds> index_bounds;
  uint64_t vertex_bytes_whole = 0;  // copied by unsafe shaders (whole buffers), for the report

  const VertexShaderRanges& GetVertexShaderRanges(VulkanShader* vs) {
    auto it = vs_ranges.find(vs);
    if (it != vs_ranges.end()) return it->second;
    VertexShaderRanges info;
    // A modification no draw uses: the walk leaves an empty translation behind under it.
    auto* translation = vs->GetOrCreateTranslation(0xB01F00000000B01Full);
    if (translation && vs_analyzer.TranslateAnalyzedShader(*translation)) {
      info.index_safe = vs_analyzer.safe();
    }
    for (const auto& binding : vs->vertex_bindings()) {
      VertexShaderRanges::Binding b = {binding.fetch_constant, binding.stride_words * 4, 16, true};
      for (const auto& attribute : binding.attributes) {
        const int32_t offset = attribute.fetch_instr.attributes.offset;
        if (offset < 0) b.usable = false;
        b.vertex_extent = std::max(b.vertex_extent, uint32_t(std::max(offset, 0)) * 4 + 16);
      }
      info.bindings.push_back(b);
    }
    REXLOG_INFO("[native] vertex shader {:016X}: {} binding(s), fetches {}", vs->ucode_data_hash(),
                info.bindings.size(), info.index_safe ? "by vertex index (ranged copies)" : "unsafe (whole copies)");
    return vs_ranges.emplace(vs, std::move(info)).first->second;
  }

  uint32_t bound_offsets[SpirvShaderTranslator::kConstantBufferCount] = {};
  bool bound_offsets_valid = false;
  VkDescriptorSet bound_texture_sets[2] = {};
  VkDeviceSize bound_index_offset = 0;
  VkIndexType bound_index_type = VK_INDEX_TYPE_UINT16;
  bool bound_index_valid = false;

  // The last upload of each constant buffer, reused by the next draws while
  // the registers behind it are unchanged (the ring counts register writes)
  // and the stream still holds it (same submission).
  struct ReusedUpload {
    uint64_t submission = 0;
    uint64_t generation = 0;
    const void* shader = nullptr;
    uint64_t frame = 0;
    VkDeviceSize offset = 0;
  };
  ReusedUpload last_fetch, last_vs_float, last_ps_float, last_bool_loop;
  SpirvShaderTranslator::SystemConstants last_system_constants = {};
  ReusedUpload last_system;
  bool Reusable(const ReusedUpload& u, uint64_t generation, const void* shader,
                bool per_frame = false) const {
    return u.submission == submission_number && u.generation == generation &&
           u.shader == shader && (!per_frame || u.frame == frame);
  }

  const VulkanDevice::Functions& dfn() const { return device->functions(); }
  // Not in ReXGlue's device function table.
  PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage = nullptr;
  PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
  Submission& current() { return submissions[submission_number % kSubmissionSlots]; }
  // Waits for queued draws first: the recording thread and this one never
  // record into the command buffer at the same time.
  VkCommandBuffer cmd() {
    DrainRecorder();
    return current().main;
  }

  // --- Recording thread ------------------------------------------------------------
  // A draw's Vulkan calls (binds, dynamic state, the draw), decided on the
  // ring thread and made on the recording thread (bo1_native_record_thread)
  // or straight away. In the busiest areas the driver's share of a draw was
  // about a quarter of the ring thread's time; on another core it overlaps
  // with the ring preparing the next draw. Everything else recorded into the
  // main command buffer goes through cmd(), which waits for the queue to
  // empty, and the texture uploads go into the setup command buffer, which
  // has a pool of its own so the two threads never share one.
  struct DrawCommand {
    VkCommandBuffer commands;
    uint32_t flags;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet set_shared;
    VkDescriptorSet set_constants;
    uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount];
    VkDescriptorSet texture_sets[2];
    VkViewport viewport;
    VkRect2D scissor;
    float bias_constant, bias_slope;
    float blend[4];
    uint32_t stencil[6];
    VkBuffer index_buffer;
    VkDeviceSize index_offset;
    VkIndexType index_type;
    uint32_t count;
  };
  enum : uint32_t {
    kDrawPipeline = 1u << 0,
    kDrawSharedSet = 1u << 1,
    kDrawConstants = 1u << 2,
    kDrawTexturesVertex = 1u << 3,
    kDrawTexturesPixel = 1u << 4,
    kDrawViewport = 1u << 5,
    kDrawScissor = 1u << 6,
    kDrawDepthBias = 1u << 7,
    kDrawBlend = 1u << 8,
    kDrawStencilCompare = 1u << 9,
    kDrawStencilWrite = 1u << 10,
    kDrawStencilReference = 1u << 11,
    kDrawIndexBuffer = 1u << 12,
    kDrawIndexed = 1u << 13,
  };
  void ExecuteDrawCommand(const DrawCommand& d) const {
    VkCommandBuffer c = d.commands;
    const auto& f = device->functions();
    if (d.flags & kDrawPipeline) {
      f.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
    }
    if (d.flags & kDrawSharedSet) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram, 1,
                                &d.set_shared, 0, nullptr);
    }
    if (d.flags & kDrawConstants) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetConstants, 1,
                                &d.set_constants, SpirvShaderTranslator::kConstantBufferCount,
                                d.offsets);
    }
    if (d.flags & kDrawTexturesVertex) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetTexturesVertex, 1,
                                &d.texture_sets[0], 0, nullptr);
    }
    if (d.flags & kDrawTexturesPixel) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetTexturesPixel, 1,
                                &d.texture_sets[1], 0, nullptr);
    }
    if (d.flags & kDrawViewport) f.vkCmdSetViewport(c, 0, 1, &d.viewport);
    if (d.flags & kDrawScissor) f.vkCmdSetScissor(c, 0, 1, &d.scissor);
    if (d.flags & kDrawDepthBias) f.vkCmdSetDepthBias(c, d.bias_constant, 0.0f, d.bias_slope);
    if (d.flags & kDrawBlend) f.vkCmdSetBlendConstants(c, d.blend);
    if (d.flags & kDrawStencilCompare) {
      f.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[0]);
      f.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[1]);
    }
    if (d.flags & kDrawStencilWrite) {
      f.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[2]);
      f.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[3]);
    }
    if (d.flags & kDrawStencilReference) {
      f.vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[4]);
      f.vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[5]);
    }
    if (d.flags & kDrawIndexBuffer) {
      f.vkCmdBindIndexBuffer(c, d.index_buffer, d.index_offset, d.index_type);
    }
    if (d.flags & kDrawIndexed) {
      f.vkCmdDrawIndexed(c, d.count, 1, 0, 0, 0);
    } else {
      f.vkCmdDraw(c, d.count, 1, 0, 0);
    }
  }
  static constexpr uint32_t kRecordQueueSize = 4096;
  std::unique_ptr<DrawCommand[]> record_queue;
  std::atomic<uint32_t> record_head{0};
  std::atomic<uint32_t> record_tail{0};
  std::atomic<bool> record_stop{false};
  std::atomic<bool> record_sleeping{false};
  std::mutex record_mutex;
  std::condition_variable record_cv;
  std::thread record_thread;
  bool record_threaded = false;

  void WakeRecorder() {
    if (record_sleeping.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lock(record_mutex);
      record_cv.notify_one();
    }
  }

  void RecordThread() {
#if defined(__SWITCH__)
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    // Off the ring thread's core.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 0x3);
#endif
    uint32_t idle = 0;
    while (true) {
      uint32_t tail = record_tail.load(std::memory_order_relaxed);
      if (tail != record_head.load(std::memory_order_acquire)) {
        ExecuteDrawCommand(record_queue[tail % kRecordQueueSize]);
        record_tail.store(tail + 1, std::memory_order_release);
        idle = 0;
        continue;
      }
      if (record_stop.load(std::memory_order_acquire)) return;
      // Briefly: spinning longer took cores from the game's own threads.
      if (++idle < 16) {
        std::this_thread::yield();
        continue;
      }
      std::unique_lock<std::mutex> lock(record_mutex);
      record_sleeping.store(true, std::memory_order_seq_cst);
      if (record_head.load(std::memory_order_seq_cst) == tail &&
          !record_stop.load(std::memory_order_acquire)) {
        record_cv.wait_for(lock, std::chrono::milliseconds(2));
      }
      record_sleeping.store(false, std::memory_order_relaxed);
      idle = 0;
    }
  }

  void SubmitDraw(const DrawCommand& d) {
    if (!record_threaded) {
      ExecuteDrawCommand(d);
      return;
    }
    uint32_t head = record_head.load(std::memory_order_relaxed);
    while (head - record_tail.load(std::memory_order_acquire) >= kRecordQueueSize) {
      WakeRecorder();
      std::this_thread::yield();
    }
    record_queue[head % kRecordQueueSize] = d;
    record_head.store(head + 1, std::memory_order_seq_cst);
    // In batches: the recording thread kept up draw by draw, slept between
    // them, and waking it for every draw (a lock and a system call) showed as
    // 6% of this thread in lock contention. It is also woken when this
    // thread is about to wait for the game (KickRecorder), and anything
    // recording into the command buffer drains the queue first.
    if (head + 1 - record_tail.load(std::memory_order_relaxed) >= kRecordBatch) WakeRecorder();
  }
  static constexpr uint32_t kRecordBatch = 32;

  void KickRecorder() {
    if (record_threaded &&
        record_tail.load(std::memory_order_acquire) != record_head.load(std::memory_order_relaxed)) {
      WakeRecorder();
    }
  }

  void DrainRecorder() {
    if (!record_threaded) return;
    uint32_t head = record_head.load(std::memory_order_relaxed);
    if (record_tail.load(std::memory_order_acquire) == head) return;
    uint64_t start = NowNs();
    uint32_t spins = 0;
    while (record_tail.load(std::memory_order_acquire) != head) {
      WakeRecorder();
      if (++spins > 32) std::this_thread::yield();
    }
    frame_drain_wait_ns += NowNs() - start;
  }
  RendererStats& stats() { return owner->stats_; }

  // --- Setup ---------------------------------------------------------------------

  bool Initialize();
  void Shutdown();
  bool CreateSwapResources();

  // --- Submissions and the stream ----------------------------------------------

  // A frame is complete when every submission up to it is: a frame can be
  // split over several submissions (when the stream fills up), and counting it
  // complete when its first one finished freed textures and staging memory the
  // rest still used, an MMU fault that lost the GPU. The frame being recorded
  // is never complete.
  void UpdateCompletedFrame() {
    uint64_t oldest = frame;
    for (const Submission& other : submissions) {
      if (other.pending) oldest = std::min(oldest, other.frame);
    }
    if (oldest > 0) {
      completed_frame = std::max(completed_frame, oldest - 1);
    }
  }

  void Completed(Submission& s) {
    s.pending = false;
    UpdateCompletedFrame();
    ProfileCollect(uint32_t(&s - submissions));
    if (s.timed && timestamps) {
      uint32_t slot = uint32_t(&s - submissions);
      uint64_t ticks[2] = {};
      if (dfn().vkGetQueryPoolResults(vk, timestamps, slot * 2, 2, sizeof(ticks), ticks,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          ticks[1] > ticks[0]) {
        stats().gpu_ns += uint64_t(double(ticks[1] - ticks[0]) * timestamp_period);
        ++stats().gpu_submissions;
      }
      s.timed = false;
    }
  }

  // This frame's waits and texture time (RendererStats::frame_*).
  uint64_t frame_upload_wait_ns = 0, frame_fence_wait_ns = 0, frame_drain_wait_ns = 0,
           frame_texture_ns = 0, frame_draw_count = 0, frame_stream_wait_ns = 0;
  static uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  }

  void WaitSubmission(Submission& s) {
    if (s.pending) {
      uint64_t start = NowNs();
      dfn().vkWaitForFences(vk, 1, &s.fence, VK_TRUE, UINT64_MAX);
      frame_fence_wait_ns += NowNs() - start;
      Completed(s);
    }
  }

  void PollCompleted() {
    for (Submission& s : submissions) {
      if (s.pending && dfn().vkGetFenceStatus(vk, s.fence) == VK_SUCCESS) {
        Completed(s);
      }
    }
    UpdateCompletedFrame();
  }

  void BeginRecording() {
    if (recording) {
      return;
    }
    ++submission_number;
    Submission& s = current();
    WaitSubmission(s);
    dfn().vkResetFences(vk, 1, &s.fence);
    dfn().vkResetCommandPool(vk, s.pool, 0);
    dfn().vkResetCommandPool(vk, s.main_pool, 0);
    for (VkDescriptorPool pool : s.descriptor_pools) {
      dfn().vkResetDescriptorPool(vk, pool, 0);
    }
    s.descriptor_pool_used = 0;
    s.number = submission_number;
    s.frame = frame;
    s.stream_oldest = stream_head;
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn().vkBeginCommandBuffer(s.setup, &begin);
    dfn().vkBeginCommandBuffer(s.main, &begin);
    if (timestamps) {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      dfn().vkCmdResetQueryPool(s.setup, timestamps, slot * 2, 2);
      dfn().vkCmdWriteTimestamp(s.setup, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, slot * 2);
      s.timed = true;
    }
    if (!null_images_initialized) {
      textures.InitializeNullImages(s.setup);
      null_images_initialized = true;
    }
    {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      profile_segments[slot].clear();
      profile_closed[slot] = false;
      if (profile_frame && profile_pool) {
        dfn().vkCmdResetQueryPool(s.setup, profile_pool, slot * kProfileQueries,
                                  kProfileQueries);
      }
    }
    // Everything the setup command buffer uploads is visible to the draws.
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dfn().vkCmdPipelineBarrier(s.main, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                               nullptr);
    recording = true;
    bound_pipeline = VK_NULL_HANDLE;
    bound_layout = VK_NULL_HANDLE;
    bound_static_sets = false;
    std::memset(&bound_viewport, 0, sizeof(bound_viewport));
    std::memset(&bound_scissor, 0, sizeof(bound_scissor));
    bound_depth_bias[0] = bound_depth_bias[1] = NAN;
    bound_offsets_valid = false;
    bound_texture_sets[0] = bound_texture_sets[1] = VK_NULL_HANDLE;
    bound_index_valid = false;
    for (float& f : bound_blend_constants) f = NAN;
    for (uint32_t& v : bound_stencil) v = ~0u;
  }

  void Submit() {
    if (!recording) {
      return;
    }
    DrainRecorder();
    EndPass();
    // The setup command buffer copies staging data the upload worker fills.
    {
      uint64_t start = NowNs();
      textures.WaitForUploads();
      frame_upload_wait_ns += NowNs() - start;
    }
    // Submission boundaries order everything anyway.
    resolve_barrier_pending = false;
    Submission& s = current();
    {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      if (profile_pool && !profile_segments[slot].empty()) {
        dfn().vkCmdWriteTimestamp(s.main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_pool,
                                  slot * kProfileQueries +
                                      uint32_t(profile_segments[slot].size()));
        profile_closed[slot] = true;
      }
    }
    if (s.timed) {
      dfn().vkCmdWriteTimestamp(s.main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps,
                                uint32_t(submission_number % kSubmissionSlots) * 2 + 1);
    }
    dfn().vkEndCommandBuffer(s.setup);
    dfn().vkEndCommandBuffer(s.main);
    VkCommandBuffer buffers[] = {s.setup, s.main};
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = buffers;
    {
      auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
      if (dfn().vkQueueSubmit(queue.queue(), 1, &submit, s.fence) != VK_SUCCESS) {
        REXLOG_ERROR("[native] vkQueueSubmit failed");
      }
    }
    s.pending = true;
    recording = false;
    ++stats().submissions;
  }

  // Stream memory for this submission; stalls on older submissions if the
  // ring would run over data they still read.
  // The start of the stream's next lap. Not rex::align: that rounds with a
  // mask, right only for powers of two, and the stream is 160 MB. Wrapping
  // with it put allocations at arbitrary places, far enough ahead that the
  // stream looked full and the renderer waited for the GPU to finish all its
  // work (30-45 ms, every wrap), or across the end of the buffer.
  static uint64_t NextStreamLap(uint64_t position) {
    return (position / kStreamSize + 1) * kStreamSize;
  }

  uint8_t* Allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset_out) {
    if (size > kStreamSize / 4) {
      return nullptr;
    }
    BeginRecording();
    // Allocations stay kStreamGuard short of the end, so a constant binding's
    // fixed range starting at any allocation stays inside the buffer.
    uint64_t start = rex::align(stream_head, uint64_t(alignment));
    if ((start % kStreamSize) + size > kStreamSize - kStreamGuard) {
      start = NextStreamLap(start);
    }
    uint64_t end = start + size;
    // The current submission itself must not wrap onto what it still reads.
    const uint64_t fence_before = frame_fence_wait_ns;
    if (end - current().stream_oldest > kStreamSize) {
      Submit();
      WaitSubmission(submissions[submission_number % kSubmissionSlots]);
      BeginRecording();
      start = rex::align(stream_head, uint64_t(alignment));
      if ((start % kStreamSize) + size > kStreamSize - kStreamGuard) {
        start = NextStreamLap(start);
      }
      end = start + size;
    }
    for (Submission& s : submissions) {
      if (&s != &current() && s.pending && end - s.stream_oldest > kStreamSize) {
        WaitSubmission(s);
      }
    }
    frame_stream_wait_ns += frame_fence_wait_ns - fence_before;
    stream_head = end;
    offset_out = VkDeviceSize(start % kStreamSize);
    return stream_mapping + offset_out;
  }

  VkDescriptorSet AllocatePersistentSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    while (true) {
      if (set_pool_used >= set_pools.size()) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 4096;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool) != VK_SUCCESS) {
          return VK_NULL_HANDLE;
        }
        set_pools.push_back(pool);
      }
      info.descriptorPool = set_pools[set_pool_used];
      if (dfn().vkAllocateDescriptorSets(vk, &info, &set) == VK_SUCCESS) {
        return set;
      }
      ++set_pool_used;
    }
  }

  // Forgets every cached set; the pools are freed once the GPU is done with
  // the frames that may use them.
  // Bumped when the cached texture sets are dropped: their pools are freed
  // once the GPU is done, so bindings kept from earlier frames must not name
  // them any more.
  uint64_t set_cache_generation = 1;
  void FlushSetCache() {
    ++set_cache_generation;
    texture_set_cache.clear();
    if (!set_pools.empty()) {
      retired_set_pools.push_back({frame, std::move(set_pools)});
      set_pools.clear();
    }
    set_pool_used = 0;
  }

  void FreeRetiredSetPools() {
    for (auto it = retired_set_pools.begin(); it != retired_set_pools.end();) {
      if (it->frame <= completed_frame) {
        for (VkDescriptorPool pool : it->pools) {
          dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
        }
        it = retired_set_pools.erase(it);
      } else {
        ++it;
      }
    }
  }

  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout) {
    Submission& s = current();
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    while (true) {
      if (s.descriptor_pool_used >= s.descriptor_pools.size()) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 4096;
        pool_info.poolSizeCount = 3;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool) != VK_SUCCESS) {
          return VK_NULL_HANDLE;
        }
        s.descriptor_pools.push_back(pool);
      }
      info.descriptorPool = s.descriptor_pools[s.descriptor_pool_used];
      if (dfn().vkAllocateDescriptorSets(vk, &info, &set) == VK_SUCCESS) {
        return set;
      }
      ++s.descriptor_pool_used;
    }
  }

  // --- Shaders, layouts and pipelines ----------------------------------------------

  VkDescriptorSetLayout GetTextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  const PipelineLayout* GetPipelineLayout(uint32_t tex_vs, uint32_t samp_vs, uint32_t tex_ps,
                                          uint32_t samp_ps);
  bool EnsureTranslated(VulkanShader::VulkanTranslation* translation, bool* pending_out);
  void DrainTranslations();
  void TranslationThread();
  std::unique_ptr<SpirvShaderTranslator> worker_translator;
  // 1 = being translated on the worker, 2 = done.
  std::unordered_map<const void*, uint8_t> translation_state;
  std::thread translation_thread;
  std::mutex translation_mutex;
  std::condition_variable translation_cv;
  std::deque<VulkanShader::VulkanTranslation*> translation_jobs;
  std::vector<VulkanShader::VulkanTranslation*> translations_done;
  std::atomic<uint32_t> translations_done_count{0};
  bool translation_stop = false;
  // SPIR-V from earlier sessions (a file next to the driver cache), by shader type, ucode hash and modification:
  // a hit skips the translation. Only the thread calling EnsureTranslated uses the map; the file is appended to
  // under spirv_cache_mutex by whichever thread translated.
  struct CachedSpirv {
    uint32_t type = 0;
    uint64_t ucode_hash = 0;
    uint64_t modification = 0;
    uint32_t ucode_dwords = 0;
    std::vector<SpirvShader::TextureBinding> textures;
    std::vector<SpirvShader::SamplerBinding> samplers;
    std::vector<uint8_t> binary;
  };
  std::unordered_map<uint64_t, CachedSpirv> spirv_cache;
  std::mutex spirv_cache_mutex;
  FILE* spirv_cache_file = nullptr;
  bool TakeCachedTranslation(VulkanShader::VulkanTranslation* translation);
  void StoreTranslation(VulkanShader::VulkanTranslation* translation);
  struct PipelineJob {
    uint64_t hash = 0;
    PipelineKey key = {};
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule pixel = VK_NULL_HANDLE;
    VkShaderModule geometry = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    bool prewarm = false;  // from the list of earlier sessions, compiled at boot
  };
  VkPipeline GetPipeline(const PipelineKey& key, VulkanShader::VulkanTranslation* vs,
                         VulkanShader::VulkanTranslation* ps, VkShaderModule geometry,
                         VkPipelineLayout layout, bool* pending_out);
  VkPipeline CreatePipeline(const PipelineJob& job);
  void DrainCompletedPipelines();
  void PipelineThread();
  std::unordered_set<uint64_t> pending_pipelines;
  std::thread pipeline_thread;
  std::mutex pipeline_mutex;
  std::condition_variable pipeline_cv;
  std::deque<PipelineJob> pipeline_jobs;
  std::vector<std::pair<uint64_t, VkPipeline>> pipelines_completed;
  std::atomic<uint32_t> pipelines_completed_count{0};
  bool pipeline_stop = false;
  // Pipeline list (bo1-nx): the keys of every pipeline earlier sessions made, in a file next to the driver cache.
  // At boot they are compiled on the worker, after any pipeline a draw is waiting for, with shader modules made from
  // the SPIR-V cache; those modules are destroyed once the last of them is compiled.
  std::deque<PipelineJob> prewarm_jobs;
  std::vector<VkShaderModule> prewarm_modules;
  uint32_t prewarm_remaining = 0;  // under pipeline_mutex
  std::chrono::steady_clock::time_point prewarm_start;
  std::unordered_set<uint64_t> listed_pipelines;  // in the file, or queued for it
  std::unordered_map<uint64_t, PipelineKey> pending_list_keys;  // asked for, being compiled
  std::vector<PipelineKey> pipelines_to_list;     // made this session, not yet written
  FILE* pipeline_list_file = nullptr;              // written only by driver_cache_writer
  void ListPipeline(uint64_t hash, const PipelineKey& key);

  // --- Render targets ------------------------------------------------------------

  RenderTarget* GetRenderTarget(const RenderTargetKey& key, uint32_t min_height);
  // bo1-nx: see GetRenderTarget and FindClearAlias.
  RenderTargetKey CanonicalTargetKey(const RenderTargetKey& guest_key);
  RenderTarget* GetRenderTargetCanonical(const RenderTargetKey& key, uint32_t min_height);
  RenderTarget* FindClearAlias(const RenderTargetKey& guest_key, uint32_t& scale_x, uint32_t& scale_y);
  RenderTarget* DepthContainer(const RenderTarget& rt) const;
  void EndPass() {
    if (in_pass) {
      dfn().vkCmdEndRendering(cmd());
      in_pass = false;
    }
  }
  bool BeginPass(const PassTargets& targets);

  // --- Draws ---------------------------------------------------------------------

  void Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words);
  void UpdateSystemConstants(bool primitive_polygonal, const draw_util::ViewportInfo& viewport,
                             uint32_t used_texture_mask, uint32_t vertex_index_endian_flags,
                             const VulkanShader* vs, const VulkanShader* ps);
  void Resolve();
  bool ResolveDownsampled(RenderTarget* full, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          xenos::CopyCommand command);
  bool resolve_barrier_pending = false;
  void FlushResolveBarrier();
  void Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width, uint32_t height);
  void SaveDriverCache();
};

// --- Setup -------------------------------------------------------------------------

bool Renderer::Impl::Initialize() {
  device = provider->vulkan_device();
  vk = device->device();
  auto get_proc = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
  vkCmdClearDepthStencilImage = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
      get_proc(vk, "vkCmdClearDepthStencilImage"));
  vkCmdResolveImage =
      reinterpret_cast<PFN_vkCmdResolveImage>(get_proc(vk, "vkCmdResolveImage"));
  vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(get_proc(vk, "vkDeviceWaitIdle"));
  if (!vkCmdClearDepthStencilImage || !vkCmdResolveImage || !vkDeviceWaitIdle) {
    REXLOG_ERROR("[native] Missing Vulkan device functions");
    return false;
  }
  const auto& properties = device->properties();
  if (!properties.dynamicRendering) {
    REXLOG_ERROR("[native] The native renderer needs dynamic rendering");
    return false;
  }
  specialize_signs = false;
  multisample = REXCVAR_GET(bo1_native_msaa);
  ubo_alignment = std::max<VkDeviceSize>(properties.minUniformBufferOffsetAlignment, 16);

  translator = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(device),
                                                       true, true, false);

  VkShaderStageFlags guest_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                    (properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);

  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_empty) != VK_SUCCESS) {
    return false;
  }
  // Set 0: the translator's shared memory, here the stream. With a small
  // maxStorageBufferRange the translator splits it into several bindings.
  shared_binding_count = 1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
                             properties.maxStorageBufferRange);
  VkDescriptorSetLayoutBinding shared_binding = {};
  shared_binding.binding = 0;
  shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_binding.descriptorCount = shared_binding_count;
  shared_binding.stageFlags = guest_stages;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &shared_binding;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_shared) != VK_SUCCESS) {
    return false;
  }
  // Set 1: the five constant buffers, dynamic so one set serves every draw.
  VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i].binding = i;
    constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    constant_bindings[i].descriptorCount = 1;
    constant_bindings[i].stageFlags = guest_stages;
  }
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT;
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  layout_info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  layout_info.pBindings = constant_bindings;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_constants) !=
      VK_SUCCESS) {
    return false;
  }

  // The stream.
  VkDeviceSize stream_allocation = 0;
  uint32_t stream_memory_type = 0;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kStreamSize + kStreamSlack,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kUpload, stream_buffer, stream_memory,
          &stream_memory_type, &stream_allocation)) {
    REXLOG_ERROR("[native] Could not create the {} MB stream buffer", kStreamSize >> 20);
    return false;
  }
  if (!(device->memory_types().host_coherent & (1u << stream_memory_type))) {
    REXLOG_WARN("[native] The stream memory is not host-coherent");
  }
  void* mapping = nullptr;
  if (dfn().vkMapMemory(vk, stream_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    return false;
  }
  stream_mapping = static_cast<uint8_t*>(mapping);

  VkDescriptorPoolSize static_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SpirvShaderTranslator::kConstantBufferCount},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = static_sizes;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &static_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout static_layouts[] = {layout_shared, layout_constants};
  VkDescriptorSet static_sets[2];
  VkDescriptorSetAllocateInfo allocate = {};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = static_pool;
  allocate.descriptorSetCount = 2;
  allocate.pSetLayouts = static_layouts;
  if (dfn().vkAllocateDescriptorSets(vk, &allocate, static_sets) != VK_SUCCESS) {
    return false;
  }
  set_shared = static_sets[0];
  set_constants = static_sets[1];
  // With several shared memory bindings each covers a slice of guest memory;
  // the stream is far smaller than the first slice, so all of them can point at
  // the whole stream.
  std::vector<VkDescriptorBufferInfo> shared_infos(shared_binding_count);
  VkDeviceSize slice = VkDeviceSize(512u << 20) / shared_binding_count;
  for (uint32_t i = 0; i < shared_binding_count; ++i) {
    // Binding i serves the addresses from i * slice, so it starts there in
    // the stream (clamped for the slices past the end of the stream, which
    // are never addressed).
    VkDeviceSize offset = std::min(VkDeviceSize(i) * slice, kStreamSize - 256);
    offset &= ~(properties.minStorageBufferOffsetAlignment - 1);
    shared_infos[i].buffer = stream_buffer;
    shared_infos[i].offset = offset;
    shared_infos[i].range = kStreamSize + kStreamSlack - offset;
  }
  VkDescriptorBufferInfo constant_infos[SpirvShaderTranslator::kConstantBufferCount];
  VkDeviceSize constant_ranges[] = {sizeof(SpirvShaderTranslator::SystemConstants),
                                    kFloatConstantsBytes, kFloatConstantsBytes,
                                    kBoolLoopConstantsBytes, kFetchConstantsBytes};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_infos[i].buffer = stream_buffer;
    constant_infos[i].offset = 0;
    constant_infos[i].range = constant_ranges[i];
  }
  VkWriteDescriptorSet writes[2] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = set_shared;
  writes[0].descriptorCount = shared_binding_count;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = shared_infos.data();
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = set_constants;
  writes[1].descriptorCount = SpirvShaderTranslator::kConstantBufferCount;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[1].pBufferInfo = constant_infos;
  dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);

  // Submissions.
  for (Submission& s : submissions) {
    VkCommandPoolCreateInfo command_pool = {};
    command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool.queueFamilyIndex = device->queue_family_graphics_compute();
    if (dfn().vkCreateCommandPool(vk, &command_pool, nullptr, &s.pool) != VK_SUCCESS) {
      return false;
    }
    if (dfn().vkCreateCommandPool(vk, &command_pool, nullptr, &s.main_pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo buffers = {};
    buffers.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    buffers.commandPool = s.pool;
    buffers.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffers.commandBufferCount = 1;
    if (dfn().vkAllocateCommandBuffers(vk, &buffers, &s.setup) != VK_SUCCESS) {
      return false;
    }
    buffers.commandPool = s.main_pool;
    if (dfn().vkAllocateCommandBuffers(vk, &buffers, &s.main) != VK_SUCCESS) {
      return false;
    }
    VkFenceCreateInfo fence = {};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dfn().vkCreateFence(vk, &fence, nullptr, &s.fence) != VK_SUCCESS) {
      return false;
    }
  }

  VkQueryPoolCreateInfo query_info = {};
  query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query_info.queryCount = kSubmissionSlots * 2;
  if (properties.timestampPeriod > 0.0f &&
      dfn().vkCreateQueryPool(vk, &query_info, nullptr, &timestamps) == VK_SUCCESS) {
    timestamp_period = properties.timestampPeriod;
  } else {
    timestamps = VK_NULL_HANDLE;
  }

  VkPipelineCacheCreateInfo cache_info = {};
  cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  dfn().vkCreatePipelineCache(vk, &cache_info, nullptr, &driver_cache);

  if (!textures.Initialize(device, memory)) {
    return false;
  }
  upload.buffer = stream_buffer;
  upload.allocate = [this](size_t size, size_t alignment, VkDeviceSize& offset) {
    uint8_t* result = Allocate(size, alignment, offset);
    // An allocation may have started a new submission.
    upload.setup_commands = current().setup;
    upload.submission = submission_number;
    return result;
  };
  if (!CreateSwapResources()) {
    return false;
  }
  pipeline_thread = std::thread([this]() { PipelineThread(); });
  if (REXCVAR_GET(bo1_native_record_thread)) {
    record_queue = std::make_unique<DrawCommand[]>(kRecordQueueSize);
    record_thread = std::thread([this]() { RecordThread(); });
    record_threaded = true;
  }
  worker_translator = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(device), true, true, false);
  translation_thread = std::thread([this]() { TranslationThread(); });
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kDepthScratchSize,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, depth_scratch,
          depth_scratch_memory)) {
    depth_scratch = VK_NULL_HANDLE;
    REXLOG_WARN("[native] No scratch buffer for depth resolves");
  }
  if (depth_scratch && !CreateDepthConversion()) {
    REXLOG_WARN("[native] No depth resolve pipeline; depth resolves are not done");
    if (depth_convert_pipeline) dfn().vkDestroyPipeline(vk, depth_convert_pipeline, nullptr);
    depth_convert_pipeline = VK_NULL_HANDLE;
  }
  if (copy_color_pipeline) {
    textures.SetComputeCopy([this](VkCommandBuffer c, VkImageView source, VkImageView dest,
                                   int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t width,
                                   uint32_t height) {
      return ComputeColorCopy(c, source, dest, sx, sy, dx, dy, width, height);
    });
  }
  if (REXCVAR_GET(bo1_native_depth24)) {
    VkFormatProperties format_properties = {};
    device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
        device->physical_device(), VK_FORMAT_D24_UNORM_S8_UINT, &format_properties);
    if ((format_properties.optimalTilingFeatures &
         (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) ==
            (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) &&
        depth_scratch && depth_convert_pipeline) {
      depth24 = true;
    } else {
      REXLOG_WARN("[native] 24-bit depth buffers unavailable; using 32-bit float");
    }
  }
  if (REXCVAR_GET(bo1_native_gpu_profile) && timestamps) {
    VkQueryPoolCreateInfo profile_info = {};
    profile_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    profile_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    profile_info.queryCount = kSubmissionSlots * kProfileQueries;
    if (dfn().vkCreateQueryPool(vk, &profile_info, nullptr, &profile_pool) != VK_SUCCESS) {
      profile_pool = VK_NULL_HANDLE;
    }
  }
  // The default linear 256-entry gamma ramp (CommandProcessor::Initialize).
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    gamma_ramp[i] = value | (value << 10) | (value << 20);
  }
  REXLOG_INFO("[native] Renderer ready: {} MB stream, {} shared memory binding(s)",
              kStreamSize >> 20, shared_binding_count);
  return true;
}

bool Renderer::Impl::CreateDepthConversion() {
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &depth_convert_set_layout) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 5};
  VkPipelineLayoutCreateInfo pipeline_layout = {};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 1;
  pipeline_layout.pSetLayouts = &depth_convert_set_layout;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &depth_convert_layout) !=
      VK_SUCCESS) {
    return false;
  }
  depth_convert_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
      device, depth_convert_layout, shaders::kDepthResolveCs, sizeof(shaders::kDepthResolveCs));
  if (!depth_convert_pipeline) {
    return false;
  }
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &size;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &depth_convert_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo allocate = {};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = depth_convert_pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &depth_convert_set_layout;
  if (dfn().vkAllocateDescriptorSets(vk, &allocate, &depth_convert_set) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorBufferInfo buffer = {depth_scratch, 0, kDepthScratchSize};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = depth_convert_set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer;
  dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);

  VkDescriptorSetLayoutBinding copy_bindings[2] = {};
  copy_bindings[0].binding = 0;
  copy_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  copy_bindings[0].descriptorCount = 1;
  copy_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  copy_bindings[1] = copy_bindings[0];
  copy_bindings[1].binding = 1;
  copy_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  layout_info.bindingCount = 2;
  layout_info.pBindings = copy_bindings;
  VkPushConstantRange copy_push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 4};
  pipeline_layout.pSetLayouts = &depth_copy_set_layout;
  pipeline_layout.pPushConstantRanges = &copy_push;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &depth_copy_set_layout) ==
          VK_SUCCESS &&
      dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &depth_copy_layout) ==
          VK_SUCCESS) {
    depth_copy_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
        device, depth_copy_layout, shaders::kDepthToFloatCs, sizeof(shaders::kDepthToFloatCs));
  }
  if (!depth_copy_pipeline) {
    REXLOG_WARN("[native] No direct depth resolve; resolving depth through a buffer");
  }

  VkPhysicalDeviceFeatures features = {};
  device->vulkan_instance()->functions().vkGetPhysicalDeviceFeatures(device->physical_device(),
                                                                     &features);
  if (depth_copy_set_layout && features.shaderStorageImageWriteWithoutFormat) {
    VkPushConstantRange color_push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t) * 6};
    pipeline_layout.pSetLayouts = &depth_copy_set_layout;
    pipeline_layout.pPushConstantRanges = &color_push;
    if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &copy_color_layout) ==
        VK_SUCCESS) {
      copy_color_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
          device, copy_color_layout, shaders::kCopyColorCs, sizeof(shaders::kCopyColorCs));
    }
  }
  if (!copy_color_pipeline) {
    REXLOG_WARN("[native] No compute color copy; resolving color with vkCmdCopyImage");
  }
  return true;
}

bool Renderer::Impl::ComputeColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest,
                                      int32_t sx, int32_t sy, int32_t dx, int32_t dy,
                                      uint32_t width, uint32_t height) {
  if (!copy_color_pipeline || !width || !height) return false;
  VkDescriptorSet set = AllocateSet(depth_copy_set_layout);
  if (!set) return false;
  VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_GENERAL},
                                     {VK_NULL_HANDLE, dest, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
  // Earlier writes of the source (rendering, copies, compute) before the
  // reads, and earlier reads of the destination before the writes.
  VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
  int32_t params[6] = {sx, sy, dx, dy, int32_t(width), int32_t(height)};
  dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, copy_color_pipeline);
  dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, copy_color_layout, 0, 1, &set,
                                0, nullptr);
  dfn().vkCmdPushConstants(c, copy_color_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                           params);
  dfn().vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0, 1, &after, 0, nullptr, 0, nullptr);
  return true;
}

void Renderer::Impl::ProfileCollect(uint32_t slot) {
  auto& segments = profile_segments[slot];
  if (!profile_pool || !profile_closed[slot] || segments.empty()) {
    segments.clear();
    return;
  }
  profile_closed[slot] = false;
  std::vector<uint64_t> ticks(segments.size() + 1);
  if (dfn().vkGetQueryPoolResults(vk, profile_pool, slot * kProfileQueries, uint32_t(ticks.size()),
                                  ticks.size() * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    for (size_t i = 0; i < segments.size(); ++i) {
      if (ticks[i + 1] < ticks[i]) continue;
      double ns = double(ticks[i + 1] - ticks[i]) * timestamp_period;
      ProfileTotal& total = profile_totals[segments[i].key];
      total.ns += ns;
      ++total.count;
      total.draws += segments[i].draws;
      profiled_ns += ns;
    }
  }
  segments.clear();
}

void Renderer::Impl::ProfileReport() {
  if (!profile_pool || !profile_frame) return;
  // One profiled frame in 16; report about every 10 seconds at 30 fps.
  if (++profiled_frames < 20) return;
  std::vector<std::pair<uint64_t, ProfileTotal>> sorted(profile_totals.begin(),
                                                        profile_totals.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
  double frames = double(profiled_frames);
  std::string line = fmt::format("[native] GPU by pass, ms per frame (sampled, as the driver "
                                 "reports it): total {:.1f}",
                                 profiled_ns / 1e6 / frames);
  auto target = [](uint64_t bits) -> std::string {
    if (bits == 0xFFFFFF) return "-";
    return fmt::format("t{}p{}{}", (bits >> 12) & 0xFFF, (bits >> 1) & 0x7FF,
                       (bits & 1) ? "f" : "");
  };
  for (size_t i = 0; i < sorted.size() && i < 14; ++i) {
    uint64_t key = sorted[i].first;
    const ProfileTotal& t = sorted[i].second;
    std::string what;
    switch (key >> 60) {
      case kProfilePass:
        what = fmt::format("pass c={} d={}", target((key >> 24) & 0xFFFFFF), target(key & 0xFFFFFF));
        break;
      case kProfileResolve:
        what = fmt::format("resolve{} p{} {}x{}", ((key >> 56) & 1) ? " depth" : "",
                           (key >> 32) & 0x7FF, (key >> 16) & 0xFFFF, key & 0xFFFF);
        break;
      default:
        what = "other";
        break;
    }
    line += fmt::format(" | {}: {:.2f} ({:.1f}x, {:.0f} draws)", what, t.ns / 1e6 / frames,
                        double(t.count) / frames, double(t.draws) / frames);
  }
  REXLOG_INFO("{}", line);
  profile_totals.clear();
  profiled_frames = 0;
  profiled_ns = 0;
}

bool Renderer::Impl::CreateSwapResources() {
  // The same layout as VulkanCommandProcessor's apply-gamma compute pipeline:
  // set 0 the ramp (uniform texel buffer), set 1 the source (sampled image),
  // set 2 the destination (storage image), and the size as a push constant.
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_ramp) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_source) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_dest) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout set_layouts[] = {swap_layout_ramp, swap_layout_source, swap_layout_dest};
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 2};
  VkPipelineLayoutCreateInfo pipeline_layout = {};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 3;
  pipeline_layout.pSetLayouts = set_layouts;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &swap_pipeline_layout) !=
      VK_SUCCESS) {
    return false;
  }
  swap_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
      device, swap_pipeline_layout, shaders::apply_gamma_table_cs,
      sizeof(shaders::apply_gamma_table_cs));
  if (swap_pipeline == VK_NULL_HANDLE) {
    REXLOG_ERROR("[native] Could not create the swap pipeline");
    return false;
  }

  VkDeviceSize ramp_size = 256 * sizeof(uint32_t) * kSubmissionSlots;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, ramp_size, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kUpload, ramp_buffer, ramp_memory)) {
    return false;
  }
  void* mapping = nullptr;
  dfn().vkMapMemory(vk, ramp_memory, 0, VK_WHOLE_SIZE, 0, &mapping);
  ramp_mapping = static_cast<uint8_t*>(mapping);

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kSubmissionSlots},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSubmissionSlots},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSubmissionSlots},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 3 * kSubmissionSlots;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &swap_pool) != VK_SUCCESS) {
    return false;
  }
  for (uint32_t i = 0; i < kSubmissionSlots; ++i) {
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = swap_pool;
    allocate.descriptorSetCount = 3;
    allocate.pSetLayouts = set_layouts;
    if (dfn().vkAllocateDescriptorSets(vk, &allocate, swap_sets[i]) != VK_SUCCESS) {
      return false;
    }
    VkBufferViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    view.buffer = ramp_buffer;
    view.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    view.offset = 256 * sizeof(uint32_t) * i;
    view.range = 256 * sizeof(uint32_t);
    if (dfn().vkCreateBufferView(vk, &view, nullptr, &ramp_views[i]) != VK_SUCCESS) {
      return false;
    }
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = swap_sets[i][0];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    write.pTexelBufferView = &ramp_views[i];
    dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
  }
  return true;
}

void Renderer::Impl::Shutdown() {
  if (!device) {
    return;
  }
  if (record_thread.joinable()) {
    DrainRecorder();
    record_stop.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(record_mutex);
      record_cv.notify_all();
    }
    record_thread.join();
    record_threaded = false;
  }
  if (driver_cache_writer.joinable()) {
    driver_cache_writer.join();
  }
  if (translation_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(translation_mutex);
      translation_stop = true;
    }
    translation_cv.notify_all();
    translation_thread.join();
  }
  if (spirv_cache_file) {
    fclose(spirv_cache_file);
    spirv_cache_file = nullptr;
  }
  if (pipeline_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(pipeline_mutex);
      pipeline_stop = true;
    }
    pipeline_cv.notify_all();
    pipeline_thread.join();
  }
  for (VkShaderModule module : prewarm_modules) {
    dfn().vkDestroyShaderModule(vk, module, nullptr);
  }
  prewarm_modules.clear();
  if (pipeline_list_file) {
    fclose(pipeline_list_file);
    pipeline_list_file = nullptr;
  }
  DrainCompletedPipelines();
  Submit();
  for (Submission& s : submissions) {
    WaitSubmission(s);
  }
  vkDeviceWaitIdle(vk);
  textures.Shutdown();
  for (auto& [key, rt] : render_targets) {
    if (rt->sample_view) dfn().vkDestroyImageView(vk, rt->sample_view, nullptr);
    if (rt->view) dfn().vkDestroyImageView(vk, rt->view, nullptr);
    if (rt->image) dfn().vkDestroyImage(vk, rt->image, nullptr);
    if (rt->memory) dfn().vkFreeMemory(vk, rt->memory, nullptr);
  }
  render_targets.clear();
  for (auto& retired : retired_render_targets) {
    if (retired.sample_view) dfn().vkDestroyImageView(vk, retired.sample_view, nullptr);
    dfn().vkDestroyImageView(vk, retired.view, nullptr);
    dfn().vkDestroyImage(vk, retired.image, nullptr);
    dfn().vkFreeMemory(vk, retired.memory, nullptr);
  }
  retired_render_targets.clear();
  if (depth_convert_pipeline) dfn().vkDestroyPipeline(vk, depth_convert_pipeline, nullptr);
  if (depth_convert_layout) dfn().vkDestroyPipelineLayout(vk, depth_convert_layout, nullptr);
  if (depth_copy_pipeline) dfn().vkDestroyPipeline(vk, depth_copy_pipeline, nullptr);
  if (copy_color_pipeline) dfn().vkDestroyPipeline(vk, copy_color_pipeline, nullptr);
  if (copy_color_layout) dfn().vkDestroyPipelineLayout(vk, copy_color_layout, nullptr);
  if (depth_copy_layout) dfn().vkDestroyPipelineLayout(vk, depth_copy_layout, nullptr);
  if (depth_copy_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, depth_copy_set_layout, nullptr);
  }
  if (depth_convert_pool) dfn().vkDestroyDescriptorPool(vk, depth_convert_pool, nullptr);
  if (depth_convert_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, depth_convert_set_layout, nullptr);
  }
  if (profile_pool) dfn().vkDestroyQueryPool(vk, profile_pool, nullptr);
  if (depth_scratch) dfn().vkDestroyBuffer(vk, depth_scratch, nullptr);
  if (depth_scratch_memory) dfn().vkFreeMemory(vk, depth_scratch_memory, nullptr);
  for (auto& [key, pipeline] : pipelines) {
    if (pipeline) dfn().vkDestroyPipeline(vk, pipeline, nullptr);
  }
  pipelines.clear();
  for (auto& [key, module] : geometry_shaders) {
    if (module) dfn().vkDestroyShaderModule(vk, module, nullptr);
  }
  geometry_shaders.clear();
  for (auto& [key, layout] : pipeline_layouts) {
    dfn().vkDestroyPipelineLayout(vk, layout.layout, nullptr);
  }
  pipeline_layouts.clear();
  for (auto& [key, layout] : texture_set_layouts) {
    if (layout != layout_empty) dfn().vkDestroyDescriptorSetLayout(vk, layout, nullptr);
  }
  texture_set_layouts.clear();
  shaders.clear();
  FlushSetCache();
  for (auto& retired : retired_set_pools) {
    for (VkDescriptorPool pool : retired.pools) dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
  }
  retired_set_pools.clear();
  for (Submission& s : submissions) {
    for (VkDescriptorPool pool : s.descriptor_pools) {
      dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
    }
    s.descriptor_pools.clear();
    if (s.fence) dfn().vkDestroyFence(vk, s.fence, nullptr);
    if (s.pool) dfn().vkDestroyCommandPool(vk, s.pool, nullptr);
    if (s.main_pool) dfn().vkDestroyCommandPool(vk, s.main_pool, nullptr);
    s = Submission();
  }
  for (VkBufferView view : ramp_views) {
    if (view) dfn().vkDestroyBufferView(vk, view, nullptr);
  }
  if (swap_pool) dfn().vkDestroyDescriptorPool(vk, swap_pool, nullptr);
  if (swap_pipeline) dfn().vkDestroyPipeline(vk, swap_pipeline, nullptr);
  if (swap_pipeline_layout) dfn().vkDestroyPipelineLayout(vk, swap_pipeline_layout, nullptr);
  for (VkDescriptorSetLayout l : {swap_layout_ramp, swap_layout_source, swap_layout_dest,
                                  layout_empty, layout_shared, layout_constants}) {
    if (l) dfn().vkDestroyDescriptorSetLayout(vk, l, nullptr);
  }
  if (ramp_buffer) dfn().vkDestroyBuffer(vk, ramp_buffer, nullptr);
  if (ramp_memory) dfn().vkFreeMemory(vk, ramp_memory, nullptr);
  if (static_pool) dfn().vkDestroyDescriptorPool(vk, static_pool, nullptr);
  if (stream_buffer) dfn().vkDestroyBuffer(vk, stream_buffer, nullptr);
  if (stream_memory) dfn().vkFreeMemory(vk, stream_memory, nullptr);
  if (driver_cache) dfn().vkDestroyPipelineCache(vk, driver_cache, nullptr);
  if (timestamps) dfn().vkDestroyQueryPool(vk, timestamps, nullptr);
  device = nullptr;
}

// --- Layouts and pipelines -----------------------------------------------------------

VkDescriptorSetLayout Renderer::Impl::GetTextureSetLayout(bool vertex, uint32_t texture_count,
                                                          uint32_t sampler_count) {
  if (!texture_count && !sampler_count) {
    return layout_empty;
  }
  uint64_t key = (uint64_t(vertex) << 40) | (uint64_t(texture_count) << 20) | sampler_count;
  auto it = texture_set_layouts.find(key);
  if (it != texture_set_layouts.end()) {
    return it->second;
  }
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  VkShaderStageFlags stages = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < texture_count; ++i) {
    bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr});
  }
  for (uint32_t i = 0; i < sampler_count; ++i) {
    bindings.push_back({texture_count + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &info, nullptr, &layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  texture_set_layouts.emplace(key, layout);
  return layout;
}

const PipelineLayout* Renderer::Impl::GetPipelineLayout(uint32_t tex_vs, uint32_t samp_vs,
                                                        uint32_t tex_ps, uint32_t samp_ps) {
  uint64_t key = (uint64_t(tex_vs) << 48) | (uint64_t(samp_vs) << 32) | (uint64_t(tex_ps) << 16) |
                 samp_ps;
  auto it = pipeline_layouts.find(key);
  if (it != pipeline_layouts.end()) {
    return &it->second;
  }
  PipelineLayout layout;
  layout.textures_vertex = GetTextureSetLayout(true, tex_vs, samp_vs);
  layout.textures_pixel = GetTextureSetLayout(false, tex_ps, samp_ps);
  if (!layout.textures_vertex || !layout.textures_pixel) {
    return nullptr;
  }
  VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = layout_shared;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = layout_constants;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = layout.textures_vertex;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = layout.textures_pixel;
  VkPipelineLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  info.pSetLayouts = sets;
  if (dfn().vkCreatePipelineLayout(vk, &info, nullptr, &layout.layout) != VK_SUCCESS) {
    return nullptr;
  }
  return &pipeline_layouts.emplace(key, layout).first->second;
}

// Translation runs on one worker thread with a translator of its own. Being one
// thread matters: the first completed translation of a shader fills the
// shader's binding lists, and every later one leaves them alone, so a
// translation reported done means its shader's bindings are final.
bool Renderer::Impl::EnsureTranslated(VulkanShader::VulkanTranslation* translation,
                                      bool* pending_out) {
  *pending_out = false;
  DrainTranslations();
  auto state = translation_state.find(translation);
  if (state == translation_state.end()) {
    if (!translation->shader().is_ucode_analyzed()) {
      translation->shader().AnalyzeUcode(disasm);
    }
    if (TakeCachedTranslation(translation)) {
      ++stats().shaders_translated;
      ++stats().shaders_from_cache;
      translation_state[translation] = 2;
    } else if (!REXCVAR_GET(bo1_native_async_pipelines) || !translation_thread.joinable()) {
      bool ok = translator->TranslateAnalyzedShader(*translation);
      if (ok) {
        translation->GetOrCreateShaderModule();
        StoreTranslation(translation);
      }
      ++stats().shaders_translated;
      translation_state[translation] = 2;
      if (!ok) {
        REXLOG_ERROR("[native] Shader {:016X} did not translate",
                     translation->shader().ucode_data_hash());
        return false;
      }
    } else {
      translation_state[translation] = 1;
      {
        std::lock_guard<std::mutex> lock(translation_mutex);
        translation_jobs.push_back(translation);
      }
      translation_cv.notify_one();
      *pending_out = true;
      return false;
    }
  } else if (state->second == 1) {
    *pending_out = true;
    return false;
  }
  return translation->is_valid() && translation->shader_module() != VK_NULL_HANDLE;
}

void Renderer::Impl::DrainTranslations() {
  if (!translations_done_count.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<VulkanShader::VulkanTranslation*> done;
  {
    std::lock_guard<std::mutex> lock(translation_mutex);
    done.swap(translations_done);
    translations_done_count.store(0, std::memory_order_release);
  }
  for (auto* t : done) {
    translation_state[t] = 2;
    ++stats().shaders_translated;
  }
}

void Renderer::Impl::TranslationThread() {
#if defined(__SWITCH__)
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3D);
#endif
  while (true) {
    VulkanShader::VulkanTranslation* translation;
    {
      std::unique_lock<std::mutex> lock(translation_mutex);
      translation_cv.wait(lock, [&] { return translation_stop || !translation_jobs.empty(); });
      if (translation_stop) return;
      translation = translation_jobs.front();
      translation_jobs.pop_front();
    }
    if (worker_translator->TranslateAnalyzedShader(*translation)) {
      translation->GetOrCreateShaderModule();
      StoreTranslation(translation);
    } else {
      REXLOG_ERROR("[native] Shader {:016X} did not translate",
                   translation->shader().ucode_data_hash());
    }
    std::lock_guard<std::mutex> lock(translation_mutex);
    translations_done.push_back(translation);
    translations_done_count.fetch_add(1, std::memory_order_release);
  }
}

VkPipeline Renderer::Impl::GetPipeline(const PipelineKey& key,
                                       VulkanShader::VulkanTranslation* vs,
                                       VulkanShader::VulkanTranslation* ps,
                                       VkShaderModule geometry, VkPipelineLayout layout,
                                       bool* pending_out) {
  *pending_out = false;
  DrainCompletedPipelines();
  uint64_t hash = XXH3_64bits(&key, sizeof(key));
  auto it = pipelines.find(hash);
  if (it != pipelines.end()) {
    return it->second;
  }
  if (pending_pipelines.count(hash)) {
    *pending_out = true;
    return VK_NULL_HANDLE;
  }
  PipelineJob job;
  job.hash = hash;
  job.key = key;
  job.vertex = vs->shader_module();
  job.pixel = ps ? ps->shader_module() : VK_NULL_HANDLE;
  job.geometry = geometry;
  job.layout = layout;
  if (REXCVAR_GET(bo1_native_async_pipelines) && pipeline_thread.joinable()) {
    // Compiled on the worker. Until it is ready the draws that need it are
    // skipped, a frame or two of missing objects instead of a frozen frame.
    pending_pipelines.insert(hash);
    if (!listed_pipelines.count(hash)) pending_list_keys.emplace(hash, key);
    {
      std::lock_guard<std::mutex> lock(pipeline_mutex);
      pipeline_jobs.push_back(job);
    }
    pipeline_cv.notify_one();
    *pending_out = true;
    return VK_NULL_HANDLE;
  }
  VkPipeline pipeline = CreatePipeline(job);
  if (pipeline) {
    ++stats().pipelines_created;
    last_pipeline_time = std::chrono::steady_clock::now();
    ListPipeline(hash, key);
  }
  pipelines.emplace(hash, pipeline);
  return pipeline;
}

void Renderer::Impl::DrainCompletedPipelines() {
  if (!pipelines_completed_count.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::pair<uint64_t, VkPipeline>> done;
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex);
    done.swap(pipelines_completed);
    pipelines_completed_count.store(0, std::memory_order_release);
  }
  for (auto& [hash, pipeline] : done) {
    pending_pipelines.erase(hash);
    pipelines.emplace(hash, pipeline);
    if (pipeline) {
      ++stats().pipelines_created;
      last_pipeline_time = std::chrono::steady_clock::now();
    }
  }
  // The keys of what the draws asked for, to compile at the next boot. Prewarmed ones are already listed.
  if (!pending_list_keys.empty()) {
    for (auto it = pending_list_keys.begin(); it != pending_list_keys.end();) {
      auto made = pipelines.find(it->first);
      if (made == pipelines.end()) {
        ++it;
        continue;
      }
      if (made->second) ListPipeline(it->first, it->second);
      it = pending_list_keys.erase(it);
    }
  }
}

void Renderer::Impl::PipelineThread() {
#if defined(__SWITCH__)
  // Below the game's threads and the ring: compiling is never urgent enough
  // to take a core from them.
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
#endif
  while (true) {
    PipelineJob job;
    {
      std::unique_lock<std::mutex> lock(pipeline_mutex);
      pipeline_cv.wait(lock, [&] {
        return pipeline_stop || !pipeline_jobs.empty() || !prewarm_jobs.empty();
      });
      if (pipeline_stop) {
        return;
      }
      // What a draw is waiting for goes first.
      std::deque<PipelineJob>& queue = pipeline_jobs.empty() ? prewarm_jobs : pipeline_jobs;
      job = queue.front();
      queue.pop_front();
    }
    VkPipeline pipeline = CreatePipeline(job);
    std::lock_guard<std::mutex> lock(pipeline_mutex);
    pipelines_completed.emplace_back(job.hash, pipeline);
    pipelines_completed_count.fetch_add(1, std::memory_order_release);
    if (job.prewarm && --prewarm_remaining == 0) {
      for (VkShaderModule module : prewarm_modules) {
        dfn().vkDestroyShaderModule(vk, module, nullptr);
      }
      prewarm_modules.clear();
      REXLOG_INFO("[native] Pipeline prewarm done in {} ms",
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - prewarm_start)
                      .count());
    }
  }
}

// Builds the pipeline for a key. Only reads immutable state, so it runs on the
// pipeline worker as well as on the ring thread.
VkPipeline Renderer::Impl::CreatePipeline(const PipelineJob& job) {
  const PipelineKey& key = job.key;
  VkShaderModule geometry = job.geometry;
  VkPipelineLayout layout = job.layout;

  VkSpecializationMapEntry sign_entries[8];
  for (uint32_t i = 0; i < 8; ++i) {
    sign_entries[i] = {i, uint32_t(sizeof(uint32_t) * i), sizeof(uint32_t)};
  }
  VkSpecializationInfo signs = {8, sign_entries, sizeof(key.texture_signs), key.texture_signs};
  const VkSpecializationInfo* specialization = specialize_signs ? &signs : nullptr;

  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t stage_count = 0;
  stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                           nullptr,
                           0,
                           VK_SHADER_STAGE_VERTEX_BIT,
                           job.vertex,
                           "main",
                           specialization};
  if (geometry) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             nullptr,
                             0,
                             VK_SHADER_STAGE_GEOMETRY_BIT,
                             geometry,
                             "main",
                             nullptr};
  }
  if (job.pixel) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             nullptr,
                             0,
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                             job.pixel,
                             "main",
                             specialization};
  }

  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  input_assembly.primitiveRestartEnable = key.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
  raster.polygonMode = key.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = key.cull_mode;
  raster.frontFace =
      key.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.depthBiasEnable = key.depth_format ? VK_TRUE : VK_FALSE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples);
  VkPipelineDepthStencilStateCreateInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = VkCompareOp(key.depth_compare);
  depth.stencilTestEnable =
      key.stencil_test && HasStencil(VkFormat(key.depth_format)) ? VK_TRUE : VK_FALSE;
  depth.front.failOp = VkStencilOp(key.stencil_front[0]);
  depth.front.passOp = VkStencilOp(key.stencil_front[1]);
  depth.front.depthFailOp = VkStencilOp(key.stencil_front[2]);
  depth.front.compareOp = VkCompareOp(key.stencil_front[3]);
  depth.back.failOp = VkStencilOp(key.stencil_back[0]);
  depth.back.passOp = VkStencilOp(key.stencil_back[1]);
  depth.back.depthFailOp = VkStencilOp(key.stencil_back[2]);
  depth.back.compareOp = VkCompareOp(key.stencil_back[3]);
  VkPipelineColorBlendAttachmentState attachments[4] = {};
  uint32_t attachment_count = 0;
  VkFormat color_formats[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = VkFormat(key.color_formats[i]);
    if (key.color_formats[i]) {
      attachment_count = i + 1;
    }
    VkPipelineColorBlendAttachmentState& a = attachments[i];
    uint32_t blend = key.blend[i];
    a.colorWriteMask = (key.write_masks >> (4 * i)) & 0xF;
    if (blend != 0x00010001) {
      reg::RB_BLENDCONTROL control;
      control.value = blend;
      a.blendEnable = VK_TRUE;
      a.srcColorBlendFactor = BlendFactorToVk(control.color_srcblend);
      a.dstColorBlendFactor = BlendFactorToVk(control.color_destblend);
      a.colorBlendOp = BlendOpToVk(control.color_comb_fcn);
      a.srcAlphaBlendFactor = BlendFactorToVk(control.alpha_srcblend);
      a.dstAlphaBlendFactor = BlendFactorToVk(control.alpha_destblend);
      a.alphaBlendOp = BlendOpToVk(control.alpha_comb_fcn);
    }
  }
  VkPipelineColorBlendStateCreateInfo blend = {};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = attachment_count;
  blend.pAttachments = attachments;
  VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = attachment_count;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = VkFormat(key.depth_format);
  rendering.stencilAttachmentFormat = VkFormat(key.depth_format);

  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn().vkCreateGraphicsPipelines(vk, driver_cache, 1, &info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    REXLOG_ERROR("[native] Pipeline creation failed (VS {:016X}, PS {:016X})", key.vs_hash,
                 key.ps_hash);
    pipeline = VK_NULL_HANDLE;
  }
  return pipeline;
}

// --- Render targets ------------------------------------------------------------------

RenderTarget* Renderer::Impl::GetRenderTarget(const RenderTargetKey& guest_key, uint32_t min_height) {
  // bo1-nx: color formats with the same host format share one image, as they share the EDRAM bits on the console.
  // BO1 draws its world into k_8_8_8_8_GAMMA and resolves the same tiles as k_8_8_8_8; with an image per guest
  // format the resolve read an empty image and the world came out black.
  RenderTargetKey key = CanonicalTargetKey(guest_key);
  min_height = std::clamp(min_height, 16u, 2048u);
  return GetRenderTargetCanonical(key, min_height);
}

RenderTargetKey Renderer::Impl::CanonicalTargetKey(const RenderTargetKey& guest_key) {
  RenderTargetKey key = guest_key;
  if (!key.is_depth && key.format < 16) {
    static const auto canonical = [] {
      std::array<uint32_t, 16> table{};
      for (uint32_t f = 0; f < 16; ++f) {
        table[f] = f;
        for (uint32_t g = 0; g < f; ++g) {
          if (ColorFormatToVk(xenos::ColorRenderTargetFormat(g)) ==
              ColorFormatToVk(xenos::ColorRenderTargetFormat(f))) {
            table[f] = g;
            break;
          }
        }
      }
      return table;
    }();
    key.format = canonical[key.format];
  }
  return key;
}

// bo1-nx: the depth target, with the same pitch, MSAA and format, whose EDRAM range holds this one's first tile (on
// the console the same memory), or nullptr.
RenderTarget* Renderer::Impl::DepthContainer(const RenderTarget& rt) const {
  const RenderTargetKey& k = rt.key;
  const uint32_t pitch_tiles = (k.pitch * (k.msaa >= 2 ? 2 : 1) + 79) / 80;
  const uint32_t rows_per_tile = k.msaa >= 1 ? 8 : 16;
  if (!pitch_tiles) return nullptr;
  for (const auto& [key, other] : render_targets) {
    if (other.get() == &rt || !key.is_depth || key.pitch != k.pitch || key.msaa != k.msaa ||
        key.format != k.format || key.base_tiles >= k.base_tiles) {
      continue;
    }
    const uint32_t end =
        key.base_tiles + pitch_tiles * ((other->used_height + rows_per_tile - 1) / rows_per_tile);
    if (k.base_tiles < end) return other.get();
  }
  return nullptr;
}

RenderTarget* Renderer::Impl::FindClearAlias(const RenderTargetKey& guest_key, uint32_t& scale_x,
                                             uint32_t& scale_y) {
  // A 4x view at half the pitch covers the same EDRAM tiles as the 2x view (same height) or the 1x view (twice the
  // height) at the full pitch: tiles are 80x16 samples, 4x pixels are 2x2 samples, 2x pixels 1x2.
  if (guest_key.msaa != 2) return nullptr;
  for (uint32_t msaa : {1u, 0u}) {
    RenderTargetKey key = CanonicalTargetKey(guest_key);
    key.pitch *= 2;
    key.msaa = msaa;
    auto it = render_targets.find(key);
    if (it != render_targets.end()) {
      scale_x = 2;
      scale_y = msaa == 0 ? 2 : 1;
      return it->second.get();
    }
  }
  return nullptr;
}

RenderTarget* Renderer::Impl::GetRenderTargetCanonical(const RenderTargetKey& key, uint32_t min_height) {
  auto it = render_targets.find(key);
  RenderTarget* old = nullptr;
  if (it != render_targets.end()) {
    old = it->second.get();
    old->last_used_frame = frame;
    if (old->height >= min_height) {
      old->used_height = std::max(old->used_height, min_height);
      return old;
    }
  }
  auto rt = std::make_unique<RenderTarget>();
  rt->key = key;
  rt->width = std::max(key.pitch, 1u);
  rt->height = rex::align(min_height, 16u);
  rt->used_height = std::max(min_height, old ? old->used_height : 0u);
  // bo1-nx (bo1_native_zcull): depth at the full height of its EDRAM range, so it never grows.
  const bool zcull = key.is_depth && REXCVAR_GET(bo1_native_zcull);
  if (zcull) {
    const uint32_t pitch_tiles = (key.pitch * (key.msaa >= 2 ? 2 : 1) + 79) / 80;
    if (pitch_tiles) {
      const uint32_t full = (2048 - std::min(key.base_tiles, 2047u)) / pitch_tiles * (key.msaa >= 1 ? 8 : 16);
      rt->height = std::max(rt->height, rex::align(std::min(full, 2048u), 16u));
    }
  }
  // A depth target that grows from an old one gets the old content by a copy, which needs TRANSFER_DST.
  const bool zcull_usage = zcull && !old;
  rt->samples = VkSampleCountFlagBits(1u << key.msaa);
  rt->format = key.is_depth ? DepthFormatFor(key.format)
                            : ColorFormatToVk(xenos::ColorRenderTargetFormat(key.format));
  rt->last_used_frame = frame;
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = rt->format;
  info.extent = {rt->width, rt->height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = rt->samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = (key.is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT
                             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (zcull_usage ? 0 : VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          device, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, rt->image,
          rt->memory)) {
    REXLOG_ERROR("[native] Could not create a {}x{} render target", rt->width, rt->height);
    return old;
  }
  VkImageAspectFlags aspect =
      key.is_depth ? (HasStencil(rt->format)
                          ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                          : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT))
                   : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageViewCreateInfo view = {};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = rt->image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = rt->format;
  view.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange(aspect);
  dfn().vkCreateImageView(vk, &view, nullptr, &rt->view);
  if (key.is_depth) {
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dfn().vkCreateImageView(vk, &view, nullptr, &rt->sample_view);
    view.subresourceRange.aspectMask = aspect;
  }

  // Render targets live in GENERAL. A new one starts cleared, or with the
  // content of the smaller one it replaces.
  BeginRecording();
  EndPass();
  VkCommandBuffer c = cmd();
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = zcull_usage ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = rt->image;
  barrier.subresourceRange = view.subresourceRange;
  dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             zcull_usage ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                         : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
  if (zcull_usage) {
    // Cleared by a rendering scope that only clears: no transfer write, so NVK keeps the ZCULL plane valid.
    VkRenderingAttachmentInfo depth = {};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = rt->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo clear_info = {};
    clear_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    clear_info.renderArea.extent = {rt->width, rt->height};
    clear_info.layerCount = 1;
    clear_info.pDepthAttachment = &depth;
    clear_info.pStencilAttachment = HasStencil(rt->format) ? &depth : nullptr;
    dfn().vkCmdBeginRendering(c, &clear_info);
    dfn().vkCmdEndRendering(c);
  } else if (key.is_depth) {
    VkClearDepthStencilValue clear = {1.0f, 0};
    vkCmdClearDepthStencilImage(c, rt->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                      &view.subresourceRange);
  } else {
    VkClearColorValue clear = {};
    dfn().vkCmdClearColorImage(c, rt->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                               &view.subresourceRange);
  }
  if (old) {
    VkImageCopy copy = {};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {std::min(old->width, rt->width), old->height, 1};
    VkMemoryBarrier memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                      VK_ACCESS_TRANSFER_WRITE_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory_barrier, 0, nullptr,
                               0, nullptr);
    dfn().vkCmdCopyImage(c, old->image, VK_IMAGE_LAYOUT_GENERAL, rt->image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    // Destroyed once this frame is done on the GPU (waiting for idle here
    // was a visible hitch).
    retired_render_targets.push_back({frame, old->image, old->view, old->sample_view, old->memory});
  }
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
  RenderTarget* result = rt.get();
  render_targets[key] = std::move(rt);
  ++stats().render_targets_created;
  ++rt_generation;
  REXLOG_INFO("[native] Render target {}x{} {} format {} at tile {}, {}x MSAA{}", result->width,
              result->height, key.is_depth ? "depth" : "color", key.format, key.base_tiles,
              1u << key.msaa, zcull_usage ? ", ZCULL-capable" : (zcull ? ", grew: no ZCULL" : ""));
  return result;
}

bool Renderer::Impl::BeginPass(const PassTargets& targets) {
  if (in_pass) {
    // Keep the scope if every attachment the draw needs is already bound at
    // the same slot.
    bool compatible = true;
    for (uint32_t i = 0; i < 4; ++i) {
      if (targets.color[i] && targets.color[i] != pass.color[i]) compatible = false;
    }
    if (targets.depth && targets.depth != pass.depth) compatible = false;
    if (compatible) {
      return true;
    }
    EndPass();
  }
  if (trace_frame) {
    TraceEndPass();
    REXLOG_INFO("[trace] pass color {} {} {} {} depth {}", TraceTarget(targets.color[0]),
                TraceTarget(targets.color[1]), TraceTarget(targets.color[2]), TraceTarget(targets.color[3]),
                TraceTarget(targets.depth));
  }
  FlushResolveBarrier();
  for (RenderTarget* rt : {targets.color[0], targets.color[1], targets.color[2], targets.color[3], targets.depth}) {
    if (rt) rt->last_drawn_frame = frame;
  }
  ProfileMark((kProfilePass << 60) | (ProfileTargetBits(targets.color[0]) << 24) |
              ProfileTargetBits(targets.depth));
  pass = targets;
  VkRenderingAttachmentInfo colors[4] = {};
  uint32_t color_count = 0;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i < 4; ++i) {
    colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (RenderTarget* rt = targets.color[i]) {
      colors[i].imageView = rt->view;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_count = i + 1;
      width = std::min(width, rt->width);
      height = std::min(height, rt->height);
    }
  }
  VkRenderingAttachmentInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  if (targets.depth) {
    depth.imageView = targets.depth->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    width = std::min(width, targets.depth->width);
    height = std::min(height, targets.depth->height);
  }
  if (width == UINT32_MAX) {
    return false;
  }
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea.extent = {width, height};
  info.layerCount = 1;
  info.colorAttachmentCount = color_count;
  info.pColorAttachments = colors;
  info.pDepthAttachment = targets.depth ? &depth : nullptr;
  info.pStencilAttachment =
      targets.depth && HasStencil(targets.depth->format) ? &depth : nullptr;
  dfn().vkCmdBeginRendering(cmd(), &info);
  in_pass = true;
  pass_extent = {width, height};
  ++stats().render_passes;
  return true;
}

// --- System constants ----------------------------------------------------------------

void Renderer::Impl::UpdateSystemConstants(bool primitive_polygonal,
                                           const draw_util::ViewportInfo& viewport,
                                           uint32_t used_texture_mask, uint32_t,
                                           const VulkanShader* vs, const VulkanShader* ps) {
  // VulkanCommandProcessor::UpdateSystemConstantValues for host render
  // targets, without EDRAM, tessellation or memory export.
  const RegisterFile& r = *regs;
  auto pa_cl_clip_cntl = r.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = r.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = r.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = r.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = r.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  SpirvShaderTranslator::SystemConstants& sc = system_constants;

  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(r)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  flags |= HostMsaa(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test = rb_colorcontrol.alpha_test_enable
                                          ? rb_colorcontrol.alpha_func
                                          : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    // Exponent bias, with 16_16 and 16_16_16_16 stored as -1...1 SNORM
    // instead of -32...32.
    int32_t exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      exp_bias -= 5;
    }
    float scale;
    uint32_t scale_bits = UINT32_C(0x3F800000) + (uint32_t(exp_bias) << 23);
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    sc.color_exp_bias[i] = scale;
  }
  sc.flags = flags;
  sc.vertex_index_endian = xenos::Endian::kNone;
  sc.line_loop_closing_index = 0;
  sc.vertex_base_index = r.Get<int32_t>(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET);
  sc.vertex_index_min = r[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX];
  sc.vertex_index_max = r[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX];
  sc.vertex_index_reset = 0;
  sc.compute_memexport_vertex_count = 0;
  if (!pa_cl_clip_cntl.clip_disable) {
    float* planes = sc.user_clip_planes[0];
    uint32_t remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t index;
    while (rex::bit_scan_forward(remaining, &index)) {
      remaining &= ~(1u << index);
      std::memcpy(planes, &r[rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + index * 4],
                  4 * sizeof(float));
      planes += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport.ndc_scale[i];
    sc.ndc_offset[i] = viewport.ndc_offset[i];
  }
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto minmax = r.Get<reg::PA_SU_POINT_MINMAX>();
    auto size = r.Get<reg::PA_SU_POINT_SIZE>();
    sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sc.point_screen_diameter_to_ndc_radius[0] =
        1.0f / float(std::max(viewport.xy_extent[0], 1u));
    sc.point_screen_diameter_to_ndc_radius[1] =
        1.0f / float(std::max(viewport.xy_extent[1], 1u));
  }
  uint32_t remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining &= ~(1u << index);
    uint32_t& word = sc.texture_swizzled_signs[index >> 2];
    uint32_t shift = 8 * (index & 3);
    uint8_t signs = rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(index));
    word = (word & ~(0xFFu << shift)) | (uint32_t(signs) << shift);
  }
  sc.textures_resolution_scaled = 0;
  sc.alpha_test_reference = r.Get<float>(rex::graphics::XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                         ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8)
                         : 0;
  (void)vs;
  (void)ps;
}

// --- Draws -------------------------------------------------------------------------------

// The render state of a draw, rebuilt only when a register that can change it
// is written (Renderer::state_generation_), the shaders or the primitive type
// change, or a render target is created. Consecutive draws almost always share
// it, and rebuilding it per draw was most of the renderer's CPU time.
void Renderer::Impl::EmulateMemcpyExport(const reg::VGT_DRAW_INITIATOR& initiator, VulkanShader* vs) {
  TextureTimer memexport_timer(stats().memexport_ns);
  const RegisterFile& r = *regs;
  auto constant_bits = [&](uint32_t n, uint32_t c) { return r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X + n * 4 + c]; };
  auto constant = [&](uint32_t n, uint32_t c) {
    float f;
    const uint32_t bits = constant_bits(n, c);
    std::memcpy(&f, &bits, 4);
    return f;
  };
  static uint32_t logged = 0;
  auto refuse = [&](const char* why) {
    if (logged < 8) {
      ++logged;
      REXLOG_WARN("[native] memexport copy not done: {} (c0 {:08X} {:08X} {:08X} {:08X}, c255 {} {} {} {})", why,
                  constant_bits(0, 0), constant_bits(0, 1), constant_bits(0, 2), constant_bits(0, 3), constant(255, 0),
                  constant(255, 1), constant(255, 2), constant(255, 3));
    }
  };
  if (initiator.prim_type != xenos::PrimitiveType::kPointList ||
      initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return refuse("not an auto-indexed point list");
  }
  // eA = (base dwords | 0x40000000, index, format info, count) with the last three as floats in [2^23, 2^24).
  const uint32_t ea_x = constant_bits(0, 0), ea_y = constant_bits(0, 1), ea_z = constant_bits(0, 2),
                 ea_w = constant_bits(0, 3);
  if ((ea_x >> 30) != 1 || (ea_y >> 23) != 0x96 || (ea_z >> 23) != 0x96 || (ea_w >> 23) != 0x96) {
    return refuse("unexpected stream constant");
  }
  const float s_x = constant(255, 0), s_y = constant(255, 1), s_z = constant(255, 2), s_w = constant(255, 3);
  if (s_x != 0.0f || s_y != 1.0f || s_z != 4.0f || s_w != 8.0f) return refuse("unexpected c255");
  const uint32_t format_info = ea_z & 0x7FFFFF;
  const uint32_t dest_base = (ea_x << 2) & 0x1FFFFFFF;
  const uint32_t index0 = ea_y & 0x7FFFFF, count_limit = ea_w & 0x7FFFFF;
  if (!vs->is_ucode_analyzed()) {
    rex::string::StringBuffer disasm;
    vs->AnalyzeUcode(disasm);
  }
  if (vs->vertex_bindings().empty()) return refuse("no vertex binding");
  const uint32_t fetch_index = vs->vertex_bindings()[0].fetch_constant;
  xenos::xe_gpu_vertex_fetch_t fetch = r.GetVertexFetch(fetch_index);
  if (fetch.type != xenos::FetchConstantType::kVertex) return refuse("the vertex fetch is not a vertex buffer");
  const uint32_t source = fetch.address << 2;
  const uint32_t vertices = initiator.num_indices;
  const uint64_t bytes = uint64_t(vertices) * 64;
  if (uint64_t(source) + bytes > 0x20000000u || uint64_t(index0 + vertices * 8) > count_limit ||
      uint64_t(dest_base) + uint64_t(index0 + vertices * 8) * 8 > 0x20000000u) {
    return refuse("out of bounds");
  }
  if (logged < 8 && !memexport_copies) {
    ++logged;
    REXLOG_INFO("[native] memexport copy: {} bytes from {:08X} (fetch endian {}) to {:08X} + {} x 8 (format info "
                "{:06X}: endian {}, format {}, swap {})",
                bytes, source, uint32_t(fetch.endian), dest_base, index0, format_info, format_info & 7,
                (format_info >> 8) & 0x3F, (format_info >> 19) & 1);
  }
  // A byte copy: the 16-bit fetches and the 16-bit export use the same layout, so the swizzles and swaps cancel.
  std::memcpy(memory->TranslatePhysical<uint8_t*>(dest_base + index0 * 8),
              memory->TranslatePhysical<const uint8_t*>(source), size_t(bytes));
  ++memexport_copies;
  ++stats().memexport_copies;
  memexport_bytes += bytes;
  textures.InvalidateRange(dest_base + index0 * 8, uint32_t(bytes));
}

bool Renderer::Impl::BuildDrawSetup(uint32_t initiator_value) {
  DrawSetup& st = setup;
  st = DrawSetup();
  st.valid = true;
  st.state_generation = owner->state_generation_;
  st.fetch_generation = owner->fetch_generation_;
  st.rt_generation = rt_generation;
  st.active_vs = active_vs;
  st.active_ps = active_ps;
  st.initiator_state = initiator_value & 0xFFFF;
  st.skip_reason = UINT32_MAX;

  const RegisterFile& r = *regs;
  const auto& properties = device->properties();
  auto fail = [&](uint32_t reason) {
    st.skip_reason = reason;
    return false;
  };

  xenos::EdramMode edram_mode = r.Get<reg::RB_MODECONTROL>().edram_mode;
  auto surface_info = r.Get<reg::RB_SURFACE_INFO>();
  if (!surface_info.surface_pitch) return fail(0);
  VulkanShader* vs = active_vs;
  if (!vs) return fail(1);
  if (!vs->is_ucode_analyzed()) vs->AnalyzeUcode(disasm);
  if (vs->memexport_eM_written()) {
    static std::unordered_set<uint64_t> logged;
    if (logged.size() < 6 && logged.insert(vs->ucode_data_hash()).second) {
      auto init = r.Get<reg::VGT_DRAW_INITIATOR>();
      REXLOG_INFO("[native] memexport vertex shader {:016X} (prim {}, {} indices), skipped. Disassembly:\n{}",
                  vs->ucode_data_hash(), uint32_t(init.prim_type), uint32_t(init.num_indices),
                  vs->ucode_disassembly());
    }
    return fail(2);
  }
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(r);
  if (!draw_util::IsRasterizationPotentiallyDone(r, primitive_polygonal)) return fail(3);
  VulkanShader* ps = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth && active_ps) {
    ps = active_ps;
    if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(disasm);
    if (ps->memexport_eM_written()) return fail(2);
    if (!draw_util::IsPixelShaderNeededWithRasterization(*ps, r)) ps = nullptr;
  }
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(r);
  uint32_t color_mask = draw_util::GetNormalizedColorMask(r, ps ? ps->writes_color_targets() : 0);

  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = initiator_value;
  xenos::PrimitiveType guest_type = initiator.prim_type;
  VkPrimitiveTopology topology;
  PipelineGeometryShader geometry = PipelineGeometryShader::kNone;
  switch (guest_type) {
    case xenos::PrimitiveType::kPointList:
      topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      geometry = PipelineGeometryShader::kPointList;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      st.strip = true;
      break;
    case xenos::PrimitiveType::kLineLoop:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      st.convert_loop = true;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      st.strip = true;
      break;
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kPolygon:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      st.convert_fan = true;
      break;
    case xenos::PrimitiveType::kRectangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      geometry = PipelineGeometryShader::kRectangleList;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      geometry = PipelineGeometryShader::kQuadList;
      break;
    default:
      return fail(4);
  }
  if (geometry != PipelineGeometryShader::kNone && !properties.geometryShader) return fail(5);
  bool indexed = initiator.source_select != xenos::SourceSelect::kAutoIndex;
  st.reset_enabled =
      indexed && st.strip && r.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;

  // Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification).
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      ps ? (vs->writes_interpolators() &
            ps->GetInterpolatorInputMask(r.Get<reg::SQ_PROGRAM_CNTL>(),
                                         r.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
         : 0;
  SpirvShaderTranslator::Modification vs_mod(translator->GetDefaultVertexShaderModification(
      vs->GetDynamicAddressableRegisterCount(r.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      Shader::HostVertexShaderType::kVertex));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  auto pa_cl_clip_cntl = r.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  if (user_clip_planes) {
    if (pa_cl_clip_cntl.ucp_cull_only_ena && properties.shaderCullDistance) {
      vs_mod.vertex.user_clip_plane_cull = 1;
    } else if (!properties.shaderClipDistance && properties.shaderCullDistance) {
      vs_mod.vertex.user_clip_plane_cull = 1;
    } else if (!properties.shaderClipDistance) {
      user_clip_planes = 0;
    }
  }
  vs_mod.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  vs_mod.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  vs_mod.vertex.output_point_parameters =
      uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               guest_type == xenos::PrimitiveType::kPointList);
  vs_mod.vertex.vertex_kill_and =
      uint32_t(properties.shaderCullDistance &&
               (vs->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);
  SpirvShaderTranslator::Modification ps_mod(0);
  if (ps) {
    ps_mod = SpirvShaderTranslator::Modification(translator->GetDefaultPixelShaderModification(
        ps->GetDynamicAddressableRegisterCount(r.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask &
        ~xenos::GetInterpolatorSamplingPattern(surface_info.msaa_samples,
                                               r.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                               r.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(guest_type == xenos::PrimitiveType::kPointList);
    }
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    if (ps->implicit_early_z_write_allowed() &&
        (!ps->writes_color_target(0) ||
         !draw_util::DoesCoverageDependOnAlpha(r.Get<reg::RB_COLORCONTROL>()))) {
      ps_mod.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
    } else {
      ps_mod.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
    }
  }
  auto* vs_translation =
      static_cast<VulkanShader::VulkanTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
  auto* ps_translation =
      ps ? static_cast<VulkanShader::VulkanTranslation*>(ps->GetOrCreateTranslation(ps_mod.value))
         : nullptr;
  {
    bool vs_pending = false, ps_pending = false;
    bool vs_ok = EnsureTranslated(vs_translation, &vs_pending);
    bool ps_ok = !ps_translation || EnsureTranslated(ps_translation, &ps_pending);
    if (!vs_ok || !ps_ok) {
      // Still being translated: look again on the next draw.
      if (vs_pending || ps_pending) st.valid = false;
      return fail(6);
    }
  }

  // Render targets: the color targets the draw writes and the depth buffer
  // if it is tested or written.
  bool depth_used = depth_control.z_enable || depth_control.stencil_enable;
  // The guest's sample count sizes the EDRAM below; the host's keys the targets.
  uint32_t guest_msaa = uint32_t(surface_info.msaa_samples);
  uint32_t msaa = HostMsaa(surface_info.msaa_samples);
  draw_util::GetScissor(r, st.scissor);
  draw_util::GetHostViewportInfo(r, 1, 1, false, properties.maxViewportDimensions[0],
                                 properties.maxViewportDimensions[1], true, depth_control, false,
                                 true, ps && ps->writes_depth(), st.viewport);
  // Rows the draw can touch: the viewport, cut by the scissor. The scissor
  // alone is often the 8192-pixel default and made every target 2048 tall.
  uint32_t needed_height = st.scissor.offset[1] + st.scissor.extent[1];
  if (st.viewport.xy_extent[1]) {
    needed_height =
        std::min(needed_height, st.viewport.xy_offset[1] + st.viewport.xy_extent[1]);
  }
  PassTargets& targets = st.targets;
  // bo1-nx: the Xbox 360 Direct3D clears a 2x (or 1x) surface by drawing one rectangle into a 4x view of it at half
  // the pitch, which covers the same EDRAM tiles. With an image per view the clear was lost (BO1's scene and shadow
  // depth stayed uncleared and the world drew black). Such a rectangle goes to the existing full-pitch target, with
  // its viewport and scissor scaled.
  bool clear_redirected = false;
  if (initiator.prim_type == xenos::PrimitiveType::kRectangleList && msaa == 2) {
    uint32_t sx = 0, sy = 0;
    bool all_found = true, any = false;
    PassTargets redirected;
    for (uint32_t i = 0; i < 4 && all_found; ++i) {
      if (!((color_mask >> (4 * i)) & 0xF)) continue;
      auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      RenderTargetKey key = {color_info.color_base, surface_info.surface_pitch,
                             uint32_t(color_info.color_format), msaa, 0};
      uint32_t x = 0, y = 0;
      redirected.color[i] = FindClearAlias(key, x, y);
      if (!redirected.color[i] || (any && (x != sx || y != sy))) all_found = false;
      sx = x;
      sy = y;
      any = true;
    }
    if (all_found && depth_used) {
      auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
      RenderTargetKey key = {depth_info.depth_base, surface_info.surface_pitch,
                             uint32_t(depth_info.depth_format), msaa, 1};
      uint32_t x = 0, y = 0;
      redirected.depth = FindClearAlias(key, x, y);
      if (!redirected.depth || (any && (x != sx || y != sy))) all_found = false;
      sx = x;
      sy = y;
      any = true;
    }
    if (all_found && any) {
      targets = redirected;
      clear_redirected = true;
      for (uint32_t a = 0; a < 2; ++a) {
        const uint32_t scale = a ? sy : sx;
        st.viewport.xy_offset[a] *= scale;
        st.viewport.xy_extent[a] *= scale;
        st.scissor.offset[a] *= scale;
        st.scissor.extent[a] *= scale;
      }
      ++clear_redirects;
      if (trace_frame) REXLOG_INFO("[trace]     clear redirected to the full-pitch target ({}x{} scale)", sx, sy);
    }
  }
  for (uint32_t i = 0; i < 4 && !clear_redirected; ++i) {
    if (!((color_mask >> (4 * i)) & 0xF)) continue;
    auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    RenderTargetKey key = {};
    key.base_tiles = color_info.color_base;
    key.pitch = surface_info.surface_pitch;
    key.format = uint32_t(color_info.color_format);
    key.msaa = msaa;
    // The EDRAM below the base limits how tall the target can be.
    uint32_t pitch_tiles = (surface_info.surface_pitch * (guest_msaa >= 2 ? 2 : 1) + 79) / 80 *
                           (IsColorFormat64bpp(color_info.color_format) ? 2 : 1);
    uint32_t rows = pitch_tiles ? (2048 - std::min(key.base_tiles, 2047u)) / pitch_tiles *
                                      (guest_msaa >= 1 ? 8 : 16)
                                : 2048;
    targets.color[i] = GetRenderTarget(key, std::min(needed_height, rows));
  }
  if (depth_used && !clear_redirected) {
    auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    RenderTargetKey key = {};
    key.base_tiles = depth_info.depth_base;
    key.pitch = surface_info.surface_pitch;
    key.format = uint32_t(depth_info.depth_format);
    key.msaa = msaa;
    key.is_depth = 1;
    uint32_t pitch_tiles = (surface_info.surface_pitch * (guest_msaa >= 2 ? 2 : 1) + 79) / 80;
    uint32_t rows = pitch_tiles ? (2048 - std::min(key.base_tiles, 2047u)) / pitch_tiles *
                                      (guest_msaa >= 1 ? 8 : 16)
                                : 2048;
    targets.depth = GetRenderTarget(key, std::min(needed_height, rows));
  }
  // Creating a render target above bumped rt_generation; this setup is built
  // against the new ones.
  st.rt_generation = rt_generation;
  if (targets.Empty()) return fail(7);
  st.alias_container = nullptr;
  const bool depth_only =
      targets.depth && !targets.color[0] && !targets.color[1] && !targets.color[2] && !targets.color[3];
  st.shadow_map = depth_only && guest_msaa == 0;
  st.prepass = depth_only && guest_msaa != 0 && depth_control.z_write_enable;
  if (depth_only && REXCVAR_GET(bo1_native_skip_aliased_depth)) {
    st.alias_container = DepthContainer(*targets.depth);
  }

  // Layout.
  st.vs = vs;
  st.ps = ps;
  const VulkanShader* stage_shaders[2] = {vs, ps};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const VulkanShader* shader = stage_shaders[stage];
    if (!shader) continue;
    st.counts[stage * 2] = uint32_t(shader->GetTextureBindingsAfterTranslation().size());
    st.counts[stage * 2 + 1] = uint32_t(shader->GetSamplerBindingsAfterTranslation().size());
  }
  st.layout = GetPipelineLayout(st.counts[0], st.counts[1], st.counts[2], st.counts[3]);
  if (!st.layout) return fail(10);

  // Pipeline.
  PipelineKey key = {};
  key.vs_hash = vs->ucode_data_hash();
  key.vs_modification = vs_mod.value;
  key.ps_hash = ps ? ps->ucode_data_hash() : 0;
  key.ps_modification = ps ? ps_mod.value : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key.color_formats[i] = targets.color[i] ? uint32_t(targets.color[i]->format) : 0;
  }
  key.depth_format = targets.depth ? uint32_t(targets.depth->format) : 0;
  key.samples = 1u << msaa;
  key.topology = uint32_t(topology);
  key.primitive_restart = st.reset_enabled ? 1 : 0;
  auto sc_mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    key.cull_mode = (sc_mode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) |
                    (sc_mode.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_face_clockwise = sc_mode.face != 0;
    if (properties.fillModeNonSolid &&
        sc_mode.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      xenos::PolygonType type = xenos::PolygonType::kTriangles;
      if (!sc_mode.cull_front) type = std::min(type, sc_mode.polymode_front_ptype);
      if (!sc_mode.cull_back) type = std::min(type, sc_mode.polymode_back_ptype);
      key.polygon_line = type != xenos::PolygonType::kTriangles;
    }
  }
  key.depth_clamp = properties.depthClamp && pa_cl_clip_cntl.clip_disable;
  if (targets.depth) {
    if (depth_control.z_enable) {
      key.depth_test = 1;
      key.depth_write = depth_control.z_write_enable;
      key.depth_compare = uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.zfunc);
    }
    if (depth_control.stencil_enable) {
      key.stencil_test = 1;
      key.stencil_front[0] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilfail);
      key.stencil_front[1] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzpass);
      key.stencil_front[2] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzfail);
      key.stencil_front[3] = uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.stencilfunc);
      if (primitive_polygonal && depth_control.backface_enable) {
        key.stencil_back[0] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilfail_bf);
        key.stencil_back[1] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzpass_bf);
        key.stencil_back[2] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzfail_bf);
        key.stencil_back[3] =
            uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.stencil_back, key.stencil_front, sizeof(key.stencil_back));
      }
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t mask = (color_mask >> (4 * i)) & 0xF;
    if (!targets.color[i]) mask = 0;
    key.write_masks |= mask << (4 * i);
    uint32_t blend = r[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
    key.blend[i] = mask ? blend : 0x00010001;
  }
  if (specialize_signs) {
    for (const VulkanShader* shader : stage_shaders) {
      if (!shader) continue;
      for (const auto& binding : shader->texture_bindings()) {
        uint32_t fetch = binding.fetch_constant;
        uint8_t signs = rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(fetch));
        uint32_t& word = key.texture_signs[fetch >> 2];
        uint32_t shift = 8 * (fetch & 3);
        word = (word & ~(0xFFu << shift)) | (uint32_t(signs) << shift);
      }
    }
  }
  key.layout = (uint64_t(st.counts[0]) << 48) | (uint64_t(st.counts[1]) << 32) |
               (uint64_t(st.counts[2]) << 16) | st.counts[3];
  VkShaderModule geometry_module = VK_NULL_HANDLE;
  if (geometry != PipelineGeometryShader::kNone) {
    GeometryShaderKey gs_key;
    gs_key.type = geometry;
    gs_key.interpolator_count = rex::bit_count(vs_mod.vertex.interpolator_mask);
    gs_key.user_clip_plane_count = vs_mod.vertex.user_clip_plane_count;
    gs_key.user_clip_plane_cull = vs_mod.vertex.user_clip_plane_cull;
    gs_key.has_vertex_kill_and = vs_mod.vertex.vertex_kill_and;
    gs_key.has_point_size = vs_mod.vertex.output_point_parameters;
    gs_key.has_point_coordinates = ps ? ps_mod.pixel.param_gen_point : 0;
    gs_key.point_ps_ucp_mode = vs_mod.vertex.point_ps_ucp_mode;
    key.geometry_shader = gs_key.key;
    auto it = geometry_shaders.find(gs_key.key);
    if (it == geometry_shaders.end()) {
      it = geometry_shaders.emplace(gs_key.key, CreateGeometryShader(device, gs_key)).first;
    }
    geometry_module = it->second;
    if (!geometry_module) return fail(12);
  }
  bool pending = false;
  st.pipeline = GetPipeline(key, vs_translation, ps_translation, geometry_module,
                            st.layout->layout, &pending);
  if (!st.pipeline) {
    // A pipeline being compiled: look again on the next draw.
    if (pending) st.valid = false;
    return fail(13);
  }

  // Dynamic state (VulkanCommandProcessor::UpdateDynamicState): all of it
  // comes from state registers.
  st.primitive_polygonal = primitive_polygonal;
  st.depth_control = depth_control;
  if (st.viewport.xy_extent[0] && st.viewport.xy_extent[1]) {
    st.vp.x = float(st.viewport.xy_offset[0]);
    st.vp.y = float(st.viewport.xy_offset[1]);
    st.vp.width = float(st.viewport.xy_extent[0]);
    st.vp.height = float(st.viewport.xy_extent[1]);
  } else {
    st.vp.x = -1.0f;
    st.vp.y = -1.0f;
    st.vp.width = 1.0f;
    st.vp.height = 1.0f;
  }
  st.vp.minDepth = st.viewport.z_min;
  st.vp.maxDepth = st.viewport.z_max;
  draw_util::GetPreferredFacePolygonOffset(r, primitive_polygonal, st.bias_slope,
                                           st.bias_constant);
  st.bias_constant *= r.Get<reg::RB_DEPTH_INFO>().depth_format ==
                              xenos::DepthRenderTargetFormat::kD24S8
                          ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                          : draw_util::kD3D10PolygonOffsetFactorFloat24;
  st.bias_slope *= xenos::kPolygonOffsetScaleSubpixelUnit;
  st.blend_constants[0] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_RED);
  st.blend_constants[1] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_GREEN);
  st.blend_constants[2] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_BLUE);
  st.blend_constants[3] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_ALPHA);
  auto front = r.Get<reg::RB_STENCILREFMASK>();
  auto back = front;
  if (primitive_polygonal && depth_control.backface_enable) {
    back = r.Get<reg::RB_STENCILREFMASK>(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
  }
  uint32_t stencil[6] = {front.stencilmask,      back.stencilmask, front.stencilwritemask,
                         back.stencilwritemask, front.stencilref, back.stencilref};
  std::memcpy(st.stencil, stencil, sizeof(stencil));
  return true;
}

void Renderer::Impl::Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words) {
  const RegisterFile& r = *regs;
  // The reasons, in order: 0 no surface pitch, 1 no vertex shader, 2 memory
  // export, 3 no rasterization, 4 primitive type, 5 no geometry shaders,
  // 6 translation, 7 no render target, 8 index data, 9 stream full,
  // 10 layout, 11 descriptors, 12 geometry shader, 13 pipeline, 14 pass,
  // 15 immediate indices.
  auto skip = [&](uint32_t reason) {
    ++stats().draws_skipped;
    ++stats().skipped_by_reason[reason & 31];
    if (trace_frame) {
      ++trace_pass_skips[reason & 31];
      if (trace_skip_logged < 300) {
        ++trace_skip_logged;
        auto dc = r.Get<reg::RB_DEPTHCONTROL>();
        auto init = r.Get<reg::VGT_DRAW_INITIATOR>();
        REXLOG_INFO("[trace]     skip {}: prim {} count {} z {} zw {} zf {} st {} colormask {:04X} modecontrol {}",
                    reason, uint32_t(init.prim_type), uint32_t(init.num_indices), uint32_t(dc.z_enable),
                    uint32_t(dc.z_write_enable), uint32_t(dc.zfunc), uint32_t(dc.stencil_enable),
                    r[rex::graphics::XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF,
                    uint32_t(r.Get<reg::RB_MODECONTROL>().edram_mode));
      }
    }
  };

  auto initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t guest_count = initiator.num_indices;
  if (!guest_count) {
    return;
  }
  // bo1-nx: every draw of the GPU memcpy shader is done on the CPU (before the setup cache, which would replay
  // the cached skip without copying).
  if (active_vs && active_vs->ucode_data_hash() == kBo1MemcpyVs && REXCVAR_GET(bo1_native_memexport_copy)) {
    EmulateMemcpyExport(initiator, active_vs);
    return skip(2);
  }
  // The render state: the same as the previous draw (nothing changed), or
  // one seen before (looked up by the hash of the state registers, the
  // shaders and the primitive setup), or built now.
  if (!(setup.valid && setup.state_generation == owner->state_generation_ &&
        setup.active_vs == active_vs && setup.active_ps == active_ps &&
        setup.initiator_state == (initiator.value & 0xFFFF) &&
        setup.rt_generation == rt_generation &&
        (!specialize_signs || setup.fetch_generation == owner->fetch_generation_))) {
    if (setup_cache_rt_generation != rt_generation) {
      // Setups hold render target pointers.
      setup_cache.clear();
      setup_cache_rt_generation = rt_generation;
    }
    uint64_t key = owner->state_hash_ ^
                   Renderer::Mix(0x10000, uint32_t(reinterpret_cast<uintptr_t>(active_vs))) ^
                   Renderer::Mix(0x10001, uint32_t(reinterpret_cast<uintptr_t>(active_vs) >> 32)) ^
                   Renderer::Mix(0x10002, uint32_t(reinterpret_cast<uintptr_t>(active_ps))) ^
                   Renderer::Mix(0x10003, uint32_t(reinterpret_cast<uintptr_t>(active_ps) >> 32)) ^
                   Renderer::Mix(0x10004, (initiator.value & 0xFFFF) | 0x80000000u);
    if (specialize_signs) {
      // The pipeline depends on the signs of the textures the shaders read.
      for (VulkanShader* shader : {active_vs, active_ps}) {
        if (!shader || !shader->is_ucode_analyzed()) continue;
        for (const auto& binding : shader->texture_bindings()) {
          key ^= Renderer::Mix(
              0x10010 + binding.fetch_constant,
              0x100u | rex::graphics::texture_util::SwizzleSigns(
                           r.GetTextureFetch(binding.fetch_constant)));
        }
      }
    }
    auto found = setup_cache.find(key);
    if (found != setup_cache.end() && found->second.active_vs == active_vs &&
        found->second.active_ps == active_ps &&
        found->second.initiator_state == (initiator.value & 0xFFFF)) {
      setup = found->second;
      setup.state_generation = owner->state_generation_;
      setup.fetch_generation = owner->fetch_generation_;
      ++setup_generation;
    } else {
      ++stats().setups_built;
      ++setup_generation;
      BuildDrawSetup(initiator.value);
      if (setup.valid) {
        if (setup_cache.size() >= 4096) setup_cache.clear();
        setup_cache[key] = setup;
      }
    }
  }
  DrawSetup& st = setup;
  if (st.skip_reason != UINT32_MAX) {
    return skip(st.skip_reason);
  }
  // bo1-nx: a depth-only draw into a target inside a larger one drawn this frame and still waiting for its resolve:
  // the second half of BO1's 1024x2048 sun shadow map (base 1216 inside the map at 384), which the resolve of 384
  // never reads here because each base is its own image. After that resolve, the same base 1216 holds a spot light's
  // shadow map, which is drawn.
  if (st.alias_container && st.alias_container->drawn_since_resolve &&
      st.alias_container->last_drawn_frame == frame) {
    return skip(14);
  }
  if (st.shadow_map && REXCVAR_GET(bo1_native_skip_shadow_maps)) {
    return skip(15);
  }
  if (st.prepass && REXCVAR_GET(bo1_native_skip_prepass)) {
    return skip(16);
  }
  for (RenderTarget* rt :
       {st.targets.color[0], st.targets.color[1], st.targets.color[2], st.targets.color[3], st.targets.depth}) {
    if (rt) rt->drawn_since_resolve = true;
  }
  VulkanShader* vs = st.vs;
  VulkanShader* ps = st.ps;
  const PassTargets& targets = st.targets;
  const PipelineLayout* layout = st.layout;

  // Indices.
  BeginRecording();
  auto dma_size = r.Get<reg::VGT_DMA_SIZE>();
  bool indexed = initiator.source_select != xenos::SourceSelect::kAutoIndex;
  bool index32 = initiator.index_size == xenos::IndexFormat::kInt32;
  bool reset_enabled = st.reset_enabled;
  bool convert_fan = st.convert_fan, convert_loop = st.convert_loop;
  uint32_t reset_index = r.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  const uint8_t* guest_indices = nullptr;
  xenos::Endian index_endian = dma_size.swap_mode;
  uint32_t dma_base = 0;
  if (indexed) {
    if (initiator.source_select == xenos::SourceSelect::kDMA) {
      dma_base = r[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFF;
      uint32_t bytes = guest_count * (index32 ? 4 : 2);
      if (uint64_t(dma_base) + bytes > 0x20000000u) {
        return skip(8);
      }
      guest_indices = memory->TranslatePhysical<const uint8_t*>(dma_base);
    } else if (immediate_indices) {
      guest_indices = reinterpret_cast<const uint8_t*>(immediate_indices);
      index_endian = xenos::Endian::kNone;
      guest_count = std::min(guest_count, immediate_index_words * (index32 ? 1u : 2u));
    } else {
      return skip(15);
    }
  }
  auto read_index = [&](uint32_t i) -> uint32_t {
    if (!indexed) {
      return i;
    }
    if (index32) {
      uint32_t v;
      std::memcpy(&v, guest_indices + size_t(i) * 4, 4);
      return xenos::GpuSwap(v, index_endian);
    }
    uint16_t v;
    std::memcpy(&v, guest_indices + size_t(i) * 2, 2);
    return xenos::GpuSwap(v, index_endian);
  };
  uint32_t draw_count = guest_count;
  bool host_indexed = indexed || convert_fan || convert_loop;
  VkIndexType index_type = index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  VkDeviceSize index_offset = 0;
  if (host_indexed) {
    if (convert_fan) {
      if (guest_count < 3) return;
      draw_count = (guest_count - 2) * 3;
    } else if (convert_loop) {
      if (guest_count < 2) return;
      draw_count = guest_count + 1;
    }
    if (!indexed) {
      index_type = VK_INDEX_TYPE_UINT32;
    }
    uint32_t index_size = index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2;
    // Index buffers in guest memory are usually static: reuse the converted
    // copy while the guest data is the same (checked once per frame).
    bool reused = false;
    VertexRangeKey index_key = {};
    if (dma_base && !convert_fan && !convert_loop) {
      uint32_t bytes = guest_count * (index32 ? 4 : 2);
      VertexHash& known = vertex_hashes[(uint64_t(dma_base) << 32) | bytes];
      RangeHash(known, guest_indices, bytes, dma_base);
      // The conversion also depends on the byte order and the reset index.
      index_key = {dma_base, bytes | 0x80000000u,
                   known.hash ^ (uint64_t(index_endian) << 60) ^
                       (reset_enabled ? (uint64_t(reset_index) << 8) | 1 : 0)};
      uint64_t found = FindRange(index_key);
      if (found != UINT64_MAX && stream_head - found < kStreamSize / 2) {
        index_offset = VkDeviceSize(found % kStreamSize);
        current().stream_oldest = std::min(current().stream_oldest, found);
        reused = true;
      }
    }
    if (!reused) {
      uint8_t* out = Allocate(VkDeviceSize(draw_count) * index_size, 4, index_offset);
      if (!out) {
        return skip(9);
      }
      if (index_key.address) {
        AddRange(index_key, stream_head - VkDeviceSize(draw_count) * index_size);
      }
      uint32_t host_reset = index_size == 4 ? 0xFFFFFFFFu : 0xFFFFu;
      auto put = [&](uint32_t i, uint32_t v) {
        if (reset_enabled && v == reset_index) {
          v = host_reset;
        }
        if (index_size == 4) {
          std::memcpy(out + size_t(i) * 4, &v, 4);
        } else {
          uint16_t h = uint16_t(v);
          std::memcpy(out + size_t(i) * 2, &h, 2);
        }
      };
      if (convert_fan) {
        uint32_t first = read_index(0);
        for (uint32_t t = 0; t + 2 < guest_count; ++t) {
          put(t * 3 + 0, first);
          put(t * 3 + 1, read_index(t + 1));
          put(t * 3 + 2, read_index(t + 2));
        }
      } else if (convert_loop) {
        for (uint32_t i = 0; i < guest_count; ++i) put(i, read_index(i));
        put(guest_count, read_index(0));
      } else if (!reset_enabled && index_endian == xenos::Endian::kNone) {
        std::memcpy(out, guest_indices, size_t(draw_count) * index_size);
      } else if (!reset_enabled && index_size == 2 && index_endian == xenos::Endian::k8in16) {
        rex::memory::copy_and_swap_16_unaligned(out, guest_indices, draw_count);
      } else if (!reset_enabled && index_size == 4 && index_endian == xenos::Endian::k8in32) {
        rex::memory::copy_and_swap_32_unaligned(out, guest_indices, draw_count);
      } else {
        for (uint32_t i = 0; i < draw_count; ++i) put(i, read_index(i));
      }
    }
  }

  // bo1-nx: the guest vertex indices this draw reads, after the base index, the 24-bit mask and the clamp the
  // translated vertex shader applies, for copies limited to that range.
  bool have_bounds = false;
  uint32_t bound_lo = 0, bound_hi = 0;
  const VertexShaderRanges* ranges = nullptr;
  if (REXCVAR_GET(bo1_native_ranged_vertices)) {
    ranges = &GetVertexShaderRanges(vs);
    if (!ranges->index_safe) ranges = nullptr;
  }
  if (ranges && guest_count) {
    uint32_t vmin = UINT32_MAX, vmax = 0;
    auto scan = [&] {
      for (uint32_t i = 0; i < guest_count; ++i) {
        uint32_t v = read_index(i);
        if (reset_enabled && v == reset_index) continue;
        v &= xenos::kVertexIndexMask;
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
      }
    };
    if (!indexed) {
      vmin = 0;
      vmax = guest_count - 1;
    } else if (dma_base) {
      const uint32_t bytes = guest_count * (index32 ? 4 : 2);
      const uint64_t key = (uint64_t(dma_base) << 32) | bytes;
      const uint64_t hash = RangeHash(vertex_hashes[key], guest_indices, bytes, dma_base) ^
                            (uint64_t(index_endian) << 60) ^
                            (reset_enabled ? (uint64_t(reset_index) << 8) | 1 : 0);
      IndexBounds& known = index_bounds[key];
      if (known.hash == hash && hash) {
        vmin = known.min;
        vmax = known.max;
      } else {
        scan();
        known = {hash, vmin, vmax};
      }
      if (index_bounds.size() > 65536) index_bounds.clear();
    } else {
      scan();
    }
    if (vmin <= vmax) {
      const uint32_t base = uint32_t(r.Get<int32_t>(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET));
      const uint64_t lo = uint64_t(vmin) + base, hi = uint64_t(vmax) + base;
      const uint32_t clamp_min = r[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX];
      const uint32_t clamp_max = r[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX];
      // Monotonic only if the 24-bit mask doesn't wrap inside the range.
      if ((lo >> 24) == (hi >> 24) && clamp_min <= clamp_max) {
        bound_lo = std::clamp(uint32_t(lo) & xenos::kVertexIndexMask, clamp_min, clamp_max);
        bound_hi = std::clamp(uint32_t(hi) & xenos::kVertexIndexMask, clamp_min, clamp_max);
        have_bounds = true;
      }
    }
  }

  // Fetch constants: the vertex buffers the shader reads are pointed at their
  // copies in the stream. Only slots written since they were last resolved,
  // or not yet resolved this frame, are looked at again.
  const Shader::ConstantRegisterMap& vs_constants = vs->constant_register_map();
  if (fetch_words_frame != frame || fetch_words_submission != submission_number) {
    // New frame: vertex data is checked for changes again. New submission:
    // copies in older submissions may be reused, but the bookkeeping is
    // simpler starting over.
    fetch_resolved_frame.fill(0);
    fetch_words_frame = frame;
    fetch_words_submission = submission_number;
  }
  uint32_t dirty_slots = owner->vertex_fetch_dirty_;
  owner->vertex_fetch_dirty_ = 0;
  if (dirty_slots) {
    for (uint32_t i = 0; i < 96; ++i) {
      if (dirty_slots & (1u << (i / 3))) fetch_resolved_frame[i] = 0;
    }
    // Texture words are copied as they are; vertex words get rewritten below.
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (dirty_slots & (1u << slot)) {
        std::memcpy(&fetch_words[slot * 6],
                    &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                    6 * sizeof(uint32_t));
        if (!resolve_exp_bias.empty()) {
          xenos::xe_gpu_texture_fetch_t f;
          std::memcpy(&f, &fetch_words[slot * 6], sizeof(f));
          if (f.type == xenos::FetchConstantType::kTexture) {
            auto it = resolve_exp_bias.find((f.base_address << 12) & 0x1FFFF000);
            if (it != resolve_exp_bias.end()) {
              f.exp_adjust = std::clamp(int32_t(f.exp_adjust) + it->second, -32, 31);
              std::memcpy(&fetch_words[slot * 6], &f, sizeof(f));
            }
          }
        }
      }
    }
  }
  for (uint32_t word = 0; word < 3; ++word) {
    uint32_t bits = vs_constants.vertex_fetch_bitmap[word];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      uint32_t fetch_index = word * 32 + bit;
      // bo1-nx: ranged copy, when the shader fetches by vertex index and the draw's bounds are known.
      if (have_bounds) {
        const VertexShaderRanges::Binding* binding = nullptr;
        for (const auto& b : ranges->bindings) {
          if (b.fetch_constant == fetch_index) {
            binding = &b;
            break;
          }
        }
        uint32_t guest_dword0 =
            r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2];
        uint32_t dword1 =
            r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2 + 1];
        uint32_t address = guest_dword0 & 0x1FFFFFFC;
        uint32_t bytes = std::min(((dword1 >> 2) & 0xFFFFFF) * 4, 16u << 20);
        if (binding && binding->usable && bytes && uint64_t(address) + bytes <= 0x20000000u) {
          uint64_t start = uint64_t(bound_lo) * binding->stride_bytes;
          uint64_t end = uint64_t(bound_hi) * binding->stride_bytes + binding->vertex_extent;
          end = std::min<uint64_t>(end, bytes);
          if (start < end) {
            uint32_t& dword0 = fetch_words[fetch_index * 2];
            // A later draw with an unsafe shader must look at this slot again.
            fetch_resolved_frame[fetch_index] = 0;
            SlotRange& known_range = slot_ranges[fetch_index];
            if (known_range.frame == frame && known_range.submission == submission_number &&
                known_range.base == address && known_range.bytes == bytes && start >= known_range.start &&
                end <= known_range.end) {
              dword0 = known_range.dword0;
              continue;
            }
            const uint32_t range_address = address + uint32_t(start);
            const uint32_t len = uint32_t(end - start);
            const uint8_t* source = memory->TranslatePhysical<const uint8_t*>(range_address);
            VertexHash& known = vertex_hashes[(uint64_t(range_address) << 32) | len];
            RangeHash(known, source, len, range_address);
            VertexRangeKey range_key = {range_address, len, known.hash};
            uint64_t found = FindRange(range_key);
            VkDeviceSize range_offset = 0;
            bool placed = false;
            if (found != UINT64_MAX && stream_head - found < kStreamSize / 2 &&
                found % kStreamSize >= start) {
              range_offset = VkDeviceSize(found % kStreamSize);
              current().stream_oldest = std::min(current().stream_oldest, found);
              placed = true;
            } else {
              // The fetch base becomes range_offset - start, so the copy must sit at least start bytes into the
              // stream: near the start of a lap, skip ahead first.
              if (rex::align(stream_head, uint64_t(16)) % kStreamSize < start) {
                VkDeviceSize pad_offset;
                Allocate(start, 16, pad_offset);
              }
              uint8_t* out = Allocate(len, 16, range_offset);
              if (out && range_offset >= start) {
                const bool measure_time = REXCVAR_GET(bo1_native_hash_counters);
                const uint64_t copy_start = measure_time ? NowNs() : 0;
                std::memcpy(out, source, len);
                if (measure_time) vertex_copy_ns += NowNs() - copy_start;
                stats().vertex_bytes_copied += len;
                AddRange(range_key, stream_head - len);
                placed = true;
              }
            }
            if (placed) {
              dword0 = (guest_dword0 & 0x3) | uint32_t(range_offset - start);
              known_range = {frame, submission_number, address, bytes, uint32_t(start), uint32_t(end), dword0};
              continue;
            }
          }
        }
      }
      if (fetch_resolved_frame[fetch_index] == frame) {
        continue;
      }
      fetch_resolved_frame[fetch_index] = frame;
      uint32_t guest_dword0 =
          r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2];
      uint32_t dword1 =
          r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2 + 1];
      uint32_t& dword0 = fetch_words[fetch_index * 2];
      dword0 = guest_dword0;
      uint32_t address = guest_dword0 & 0x1FFFFFFC;
      uint32_t size = (dword1 >> 2) & 0xFFFFFF;  // in words
      uint32_t bytes = std::min(size * 4, 16u << 20);
      if (!bytes || uint64_t(address) + bytes > 0x20000000u) {
        continue;
      }
      const uint8_t* source = memory->TranslatePhysical<const uint8_t*>(address);
      VertexHash& known = vertex_hashes[(uint64_t(address) << 32) | bytes];
      RangeHash(known, source, bytes, address);
      VertexRangeKey range_key = {address, bytes, known.hash};
      uint64_t found = FindRange(range_key);
      VkDeviceSize range_offset;
      if (found != UINT64_MAX && stream_head - found < kStreamSize / 2) {
        range_offset = VkDeviceSize(found % kStreamSize);
        current().stream_oldest = std::min(current().stream_oldest, found);
      } else {
        uint8_t* out = Allocate(bytes, 16, range_offset);
        if (!out) continue;
        const bool measure_time = REXCVAR_GET(bo1_native_hash_counters);
        const uint64_t copy_start = measure_time ? NowNs() : 0;
        std::memcpy(out, source, bytes);
        if (measure_time) vertex_copy_ns += NowNs() - copy_start;
        stats().vertex_bytes_copied += bytes;
        vertex_bytes_whole += bytes;
        AddRange(range_key, stream_head - bytes);
      }
      dword0 = (guest_dword0 & 0x3) | uint32_t(range_offset);
    }
  }
  if (vertex_ranges.size() > 65536) {
    vertex_ranges.clear();
  }
  if (vertex_hashes.size() > 65536) {
    vertex_hashes.clear();
  }
  VkDeviceSize fetch_offset;
  if (last_fetch.submission == submission_number &&
      !std::memcmp(last_fetch_words, fetch_words.data(), sizeof(last_fetch_words))) {
    fetch_offset = last_fetch.offset;
  } else {
    uint8_t* fetch_out = Allocate(kFetchConstantsBytes, ubo_alignment, fetch_offset);
    if (!fetch_out) return skip(9);
    std::memcpy(fetch_out, fetch_words.data(), kFetchConstantsBytes);
    std::memcpy(last_fetch_words, fetch_words.data(), sizeof(last_fetch_words));
    last_fetch = {submission_number, 0, nullptr, frame, fetch_offset};
  }

  auto write_float_constants = [&](const Shader::ConstantRegisterMap& map, uint32_t base_register,
                                   VkDeviceSize& offset) -> bool {
    // Only the constants the shader reads: the binding's fixed range reads on
    // into whatever follows (never used), which the stream's guard keeps
    // inside the buffer. Allocating the full 4 KB per draw filled the stream
    // and made it wait for the GPU.
    VkDeviceSize bytes = VkDeviceSize(std::max(map.float_count, 1u)) * 4 * sizeof(float);
    uint8_t* out = Allocate(bytes, ubo_alignment, offset);
    if (!out) return false;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map.float_bitmap[i];
      uint32_t bit;
      while (rex::bit_scan_forward(bits, &bit)) {
        bits &= ~(uint64_t(1) << bit);
        std::memcpy(out, &r[base_register + (i << 8) + (bit << 2)], 4 * sizeof(float));
        out += 4 * sizeof(float);
      }
    }
    return true;
  };
  VkDeviceSize vs_float_offset, ps_float_offset, bool_loop_offset, system_offset;
  if (Reusable(last_vs_float, owner->vs_float_generation_, vs)) {
    vs_float_offset = last_vs_float.offset;
  } else {
    if (!write_float_constants(vs_constants, rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X,
                               vs_float_offset)) {
      return skip(9);
    }
    last_vs_float = {submission_number, owner->vs_float_generation_, vs, frame, vs_float_offset};
  }
  if (ps) {
    if (Reusable(last_ps_float, owner->ps_float_generation_, ps)) {
      ps_float_offset = last_ps_float.offset;
    } else {
      if (!write_float_constants(ps->constant_register_map(),
                                 rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X,
                                 ps_float_offset)) {
        return skip(9);
      }
      last_ps_float = {submission_number, owner->ps_float_generation_, ps, frame,
                       ps_float_offset};
    }
  } else {
    ps_float_offset = vs_float_offset;
  }
  if (Reusable(last_bool_loop, owner->bool_loop_generation_, nullptr)) {
    bool_loop_offset = last_bool_loop.offset;
  } else {
    uint8_t* bool_loop = Allocate(kBoolLoopConstantsBytes, ubo_alignment, bool_loop_offset);
    if (!bool_loop) return skip(9);
    std::memcpy(bool_loop, &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsBytes);
    last_bool_loop = {submission_number, owner->bool_loop_generation_, nullptr, frame,
                      bool_loop_offset};
  }

  // Textures and samplers, reused while no fetch slot they come from was
  // written, within one frame (content changes are looked for once a frame)
  // and one submission (uploads go in its setup command buffer).
  upload.setup_commands = current().setup;
  upload.submission = submission_number;
  const VulkanShader* stage_shaders[2] = {vs, ps};
  // Keyed by the shaders and the fetch constants of the slots they read,
  // valid for a frame: materials repeat across the draws of a frame, and a hit
  // skips every texture and sampler lookup.
  uint32_t slot_mask = 0;
  for (const VulkanShader* shader : stage_shaders) {
    if (!shader) continue;
    for (const auto& b : shader->GetTextureBindingsAfterTranslation()) {
      slot_mask |= 1u << b.fetch_constant;
    }
    for (const auto& b : shader->GetSamplerBindingsAfterTranslation()) {
      slot_mask |= 1u << b.fetch_constant;
    }
  }
  uint64_t tb_key = XXH3_64bits_withSeed(&layout, sizeof(layout),
                                         uint64_t(reinterpret_cast<uintptr_t>(vs)) ^
                                             (uint64_t(reinterpret_cast<uintptr_t>(ps)) << 1));
  {
    uint32_t words[6 * 32];
    uint32_t count = 0;
    uint32_t remaining = slot_mask;
    uint32_t slot;
    while (rex::bit_scan_forward(remaining, &slot)) {
      remaining &= ~(1u << slot);
      std::memcpy(&words[count],
                  &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                  6 * sizeof(uint32_t));
      count += 6;
    }
    tb_key = XXH3_64bits_withSeed(words, count * sizeof(uint32_t), tb_key ^ slot_mask);
  }
  if (tb_cache_frame != frame) {
    if (tb_cache.size() > 8192) tb_cache.clear();
    tb_cache_frame = frame;
  }
  TextureBindings& tb = tb_cache[tb_key];
  // Any view destroyed anywhere moves views_epoch, which while a level
  // streams in is most frames: instead of rebuilding everything then, the
  // bindings check that their own views are still there.
  auto views_alive = [&]() {
    if (tb.views_epoch == textures.views_epoch()) return true;
    if (tb.view_count > 24) return false;
    for (uint32_t i = 0; i < tb.view_count; ++i) {
      if (!textures.ViewAlive(tb.views[i], tb.view_ids[i])) return false;
    }
    tb.views_epoch = textures.views_epoch();
    return true;
  };
  bool tb_hit = tb.valid && tb.vs == vs && tb.ps == ps && tb.layout == layout &&
                tb.set_generation == set_cache_generation &&
                tb.answers_epoch == textures.answers_epoch() && views_alive() &&
                ((tb.frame == frame && tb.submission == submission_number) ||
                 tb.valid_until >= frame);
  if (tb_hit && (tb.frame != frame || tb.submission != submission_number)) {
    for (uint32_t i = 0; i < tb.texture_count; ++i) {
      textures.Touch(tb.textures[i], submission_number);
    }
    tb.frame = frame;
    tb.submission = submission_number;
  }
  if (!tb_hit) {
    TextureTimer texture_timer(frame_texture_ns);
    ++stats().texture_bindings_built;
    tb = TextureBindings();
    tb.valid_until = UINT64_MAX;
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const VulkanShader* shader = stage_shaders[stage];
      if (!shader || (!st.counts[stage * 2] && !st.counts[stage * 2 + 1])) continue;
      const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
      const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
      VkDescriptorImageInfo infos[64];
      uint32_t info_count = 0;
      for (const auto& binding : texture_bindings) {
        if (info_count >= 64) break;
        tb.used_mask |= 1u << binding.fetch_constant;
        VkDescriptorImageInfo& info = infos[info_count++];
        info.sampler = VK_NULL_HANDLE;
        info.imageView = SlotView(binding.fetch_constant, binding.dimension,
                                  bool(binding.is_signed));
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        tb.valid_until = std::min(tb.valid_until, slot_valid_until);
        if (tb.view_count < 24) {
          tb.views[tb.view_count] = info.imageView;
          tb.view_ids[tb.view_count] = textures.ViewId(info.imageView);
        }
        ++tb.view_count;
        if (slot_texture) {
          if (tb.texture_count < 24) {
            tb.textures[tb.texture_count++] = slot_texture;
          } else {
            tb.valid_until = frame;
          }
        }
      }
      for (const auto& binding : sampler_bindings) {
        if (info_count >= 64) break;
        tb.slots |= 1u << binding.fetch_constant;
        VkDescriptorImageInfo& info = infos[info_count++];
        info.sampler = SlotSampler(binding);
        info.imageView = VK_NULL_HANDLE;
        info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      }
      VkDescriptorSetLayout set_layout =
          stage ? layout->textures_pixel : layout->textures_vertex;
      // Keyed by view ids, never reused, rather than handles, which are: the
      // cache was dropped whenever any view was destroyed, which while a level
      // streams in is most frames, and every material then allocated and
      // wrote new sets (10-50 ms in the frames that dropped).
      uint64_t key_words[64];
      for (uint32_t i = 0; i < info_count; ++i) {
        key_words[i] = infos[i].imageView ? textures.ViewId(infos[i].imageView)
                                          : (uint64_t(1) << 63) ^
                                                uint64_t(reinterpret_cast<uintptr_t>(
                                                    infos[i].sampler));
      }
      uint64_t set_key =
          XXH3_64bits_withSeed(key_words, sizeof(uint64_t) * info_count, uint64_t(set_layout));
      auto cached = texture_set_cache.find(set_key);
      if (cached != texture_set_cache.end()) {
        tb.sets[stage] = cached->second;
        continue;
      }
      if (texture_set_cache.size() >= 16384) {
        FlushSetCache();
      }
      VkDescriptorSet set = AllocatePersistentSet(set_layout);
      if (!set) return skip(11);
      texture_set_cache.emplace(set_key, set);
      VkWriteDescriptorSet writes[2] = {};
      uint32_t write_count = 0;
      if (!texture_bindings.empty()) {
        VkWriteDescriptorSet& w = writes[write_count++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = uint32_t(texture_bindings.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = infos;
      }
      if (!sampler_bindings.empty()) {
        VkWriteDescriptorSet& w = writes[write_count++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = uint32_t(texture_bindings.size());
        w.descriptorCount = uint32_t(sampler_bindings.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = infos + texture_bindings.size();
      }
      dfn().vkUpdateDescriptorSets(vk, write_count, writes, 0, nullptr);
      tb.sets[stage] = set;
    }
    tb.slots |= tb.used_mask;
    tb.valid = true;
    tb.frame = frame;
    tb.submission = submission_number;
    tb.vs = vs;
    tb.ps = ps;
    tb.layout = layout;
    tb.views_epoch = textures.views_epoch();
    tb.set_generation = set_cache_generation;
    tb.answers_epoch = textures.answers_epoch();
    // Only the slots these bindings read are cleared: another draw's
    // bindings may still need to see the others as written.
    owner->texture_fetch_dirty_ &= ~tb.slots;
  }

  // System constants come from the render state, the texture fetch
  // constants and the vertex index registers.
  bool system_same = last_system.submission == submission_number &&
                     system_inputs.setup_generation == setup_generation &&
                     system_inputs.fetch_generation == owner->fetch_generation_ &&
                     system_inputs.index_generation == owner->index_generation_ &&
                     system_inputs.used_mask == tb.used_mask;
  if (!system_same) {
    UpdateSystemConstants(st.primitive_polygonal, st.viewport, tb.used_mask, 0, vs, ps);
    system_inputs = {setup_generation, owner->fetch_generation_, owner->index_generation_,
                     tb.used_mask};
    system_same = last_system.submission == submission_number &&
                  !std::memcmp(&last_system_constants, &system_constants,
                               sizeof(system_constants));
  }
  if (system_same) {
    system_offset = last_system.offset;
  } else {
    uint8_t* system_out =
        Allocate(sizeof(SpirvShaderTranslator::SystemConstants), ubo_alignment, system_offset);
    if (!system_out) return skip(9);
    std::memcpy(system_out, &system_constants, sizeof(system_constants));
    std::memcpy(&last_system_constants, &system_constants, sizeof(system_constants));
    last_system = {submission_number, 0, nullptr, frame, system_offset};
  }

  // Record.
  if (!BeginPass(targets)) return skip(14);
  DrawCommand d;
  d.commands = current().main;
  d.flags = 0;
  d.layout = layout->layout;
  if (bound_pipeline != st.pipeline) {
    d.flags |= kDrawPipeline;
    d.pipeline = st.pipeline;
    bound_pipeline = st.pipeline;
  }
  if (bound_layout != layout->layout || !bound_static_sets) {
    d.flags |= kDrawSharedSet;
    d.set_shared = set_shared;
    bound_layout = layout->layout;
    bound_static_sets = true;
    bound_offsets_valid = false;
    bound_texture_sets[0] = bound_texture_sets[1] = VK_NULL_HANDLE;
  }
  uint32_t* dynamic_offsets = d.offsets;
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferSystem] = uint32_t(system_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFloatVertex] = uint32_t(vs_float_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFloatPixel] = uint32_t(ps_float_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferBoolLoop] = uint32_t(bool_loop_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFetch] = uint32_t(fetch_offset);
  if (!bound_offsets_valid || std::memcmp(bound_offsets, d.offsets, sizeof(d.offsets))) {
    d.flags |= kDrawConstants;
    d.set_constants = set_constants;
    std::memcpy(bound_offsets, d.offsets, sizeof(d.offsets));
    bound_offsets_valid = true;
  }
  if (tb.sets[0] && tb.sets[0] != bound_texture_sets[0]) {
    d.flags |= kDrawTexturesVertex;
    d.texture_sets[0] = tb.sets[0];
    bound_texture_sets[0] = tb.sets[0];
  }
  if (tb.sets[1] && tb.sets[1] != bound_texture_sets[1]) {
    d.flags |= kDrawTexturesPixel;
    d.texture_sets[1] = tb.sets[1];
    bound_texture_sets[1] = tb.sets[1];
  }

  // Dynamic state.
  if (std::memcmp(&st.vp, &bound_viewport, sizeof(st.vp))) {
    d.flags |= kDrawViewport;
    d.viewport = st.vp;
    bound_viewport = st.vp;
  }
  VkRect2D sr;
  sr.offset.x = int32_t(std::min(st.scissor.offset[0], pass_extent.width));
  sr.offset.y = int32_t(std::min(st.scissor.offset[1], pass_extent.height));
  sr.extent.width = std::min(st.scissor.extent[0], pass_extent.width - uint32_t(sr.offset.x));
  sr.extent.height = std::min(st.scissor.extent[1], pass_extent.height - uint32_t(sr.offset.y));
  if (std::memcmp(&sr, &bound_scissor, sizeof(sr))) {
    d.flags |= kDrawScissor;
    d.scissor = sr;
    bound_scissor = sr;
  }
  if (std::memcmp(&st.bias_constant, &bound_depth_bias[0], sizeof(float)) ||
      std::memcmp(&st.bias_slope, &bound_depth_bias[1], sizeof(float))) {
    d.flags |= kDrawDepthBias;
    d.bias_constant = st.bias_constant;
    d.bias_slope = st.bias_slope;
    bound_depth_bias[0] = st.bias_constant;
    bound_depth_bias[1] = st.bias_slope;
  }
  if (std::memcmp(st.blend_constants, bound_blend_constants, sizeof(st.blend_constants))) {
    d.flags |= kDrawBlend;
    std::memcpy(d.blend, st.blend_constants, sizeof(d.blend));
    std::memcpy(bound_blend_constants, st.blend_constants, sizeof(st.blend_constants));
  }
  std::memcpy(d.stencil, st.stencil, sizeof(d.stencil));
  if (st.stencil[0] != bound_stencil[0] || st.stencil[1] != bound_stencil[1]) {
    d.flags |= kDrawStencilCompare;
  }
  if (st.stencil[2] != bound_stencil[2] || st.stencil[3] != bound_stencil[3]) {
    d.flags |= kDrawStencilWrite;
  }
  if (st.stencil[4] != bound_stencil[4] || st.stencil[5] != bound_stencil[5]) {
    d.flags |= kDrawStencilReference;
  }
  std::memcpy(bound_stencil, st.stencil, sizeof(st.stencil));

  d.count = draw_count;
  if (host_indexed) {
    d.flags |= kDrawIndexed;
    if (index_offset != bound_index_offset || index_type != bound_index_type ||
        !bound_index_valid) {
      d.flags |= kDrawIndexBuffer;
      d.index_buffer = stream_buffer;
      d.index_offset = index_offset;
      d.index_type = index_type;
      bound_index_offset = index_offset;
      bound_index_type = index_type;
      bound_index_valid = true;
    }
  }
  SubmitDraw(d);
  ProfileDraw();
  ++frame_draw_count;
  ++stats().draws;
  if (trace_frame) {
    ++trace_pass_draws;
    {
      auto dc = r.Get<reg::RB_DEPTHCONTROL>();
      const bool color = (r[rex::graphics::XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF) != 0;
      ++trace_pass_depth[fmt::format("{}{}{}f{}", color ? "c" : "d", dc.z_enable ? "z" : "-",
                                     dc.z_write_enable ? "w" : "-", uint32_t(dc.zfunc))];
    }
    // Details of the draws of small passes (post-processing): shaders, target exponent bias, sampled textures.
    if (trace_pass_draws <= 2 && trace_draws_detailed < 120 &&
        (!targets.depth || targets.Empty() || st.ps)) {
      ++trace_draws_detailed;
      auto ci = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
      std::string tex;
      if (ps) {
        for (const auto& tb : ps->texture_bindings()) {
          xenos::xe_gpu_texture_fetch_t f = r.GetTextureFetch(tb.fetch_constant);
          xenos::xe_gpu_texture_fetch_t patched;
          std::memcpy(&patched, &fetch_words[tb.fetch_constant * 6], sizeof(patched));
          tex += fmt::format(" t{}:{:08X} fmt {} {}x{} exp {}->{} signs {}{}{}{}", tb.fetch_constant,
                             f.base_address << 12, uint32_t(f.format), f.size_2d.width + 1, f.size_2d.height + 1,
                             int32_t(f.exp_adjust), int32_t(patched.exp_adjust),
                             uint32_t(f.sign_x), uint32_t(f.sign_y),
                             uint32_t(f.sign_z), uint32_t(f.sign_w));
        }
      }
      REXLOG_INFO("[trace]     draw vs {:016X} ps {:016X} rt0 fmt {} exp_bias {} blend {:08X}{}",
                  vs ? vs->ucode_data_hash() : 0, ps ? ps->ucode_data_hash() : 0, uint32_t(ci.color_format),
                  int32_t(ci.color_exp_bias), r[rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0], tex);
      if (ps && trace_disassembled.insert(ps->ucode_data_hash()).second) {
        REXLOG_INFO("[trace]     ps {:016X} disassembly:\n{}", ps->ucode_data_hash(), ps->ucode_disassembly());
      }
    }
  }
  for (RenderTarget* rt : targets.color) {
    if (rt) rt->last_used_frame = frame;
  }
}

// --- Resolves ------------------------------------------------------------------------

bool Renderer::Impl::ResolveDownsampled(RenderTarget* full, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                        xenos::CopyCommand command) {
  const RegisterFile& r = *regs;
  const uint32_t dest_base = r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
  if (!dest_base || (command != xenos::CopyCommand::kRaw && command != xenos::CopyCommand::kConvert)) return false;
  const auto dest_info = r.Get<reg::RB_COPY_DEST_INFO>();
  const uint32_t height = std::min(uint32_t(y1 - y0), full->height / 2 - std::min(uint32_t(y0), full->height / 2));
  const uint32_t width = std::min(uint32_t(x1 - x0), full->width / 2 - std::min(uint32_t(x0), full->width / 2));
  if (!width || !height) return false;
  if (trace_frame) {
    TraceEndPass();
    REXLOG_INFO("[trace] resolve (4x view of {}) rect {},{}-{},{} -> {:08X}: 2x2 downsample", TraceTarget(full), x0, y0,
                x1, y1, dest_base);
  }
  ++stats().resolves;
  BeginRecording();
  EndPass();
  VkCommandBuffer c = cmd();
  VkMemoryBarrier to_transfer = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, nullptr, 0, nullptr);
  uint32_t host_swizzle = uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
  if (dest_info.copy_dest_swap) host_swizzle = XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, R, A);
  ResolveDestination* dest = textures.GetResolveDestination(
      dest_base & 0x1FFFFFFF, std::min(uint32_t(x0) + width, 8192u), std::min(uint32_t(y0) + height, 8192u),
      full->format, host_swizzle, c);
  if (!dest) return false;
  if (trace_frame) trace_last_dest = dest;
  NoteResolveExpBias(dest_base, xenos::ColorRenderTargetFormat(full->key.format));
  VkImageBlit blit = {};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.srcOffsets[0] = {x0 * 2, y0 * 2, 0};
  blit.srcOffsets[1] = {int32_t(x0 + width) * 2, int32_t(y0 + height) * 2, 1};
  blit.dstOffsets[0] = {x0, y0, 0};
  blit.dstOffsets[1] = {int32_t(x0 + width), int32_t(y0 + height), 1};
  // Linear filtering at exactly half the size averages each 2x2 block: the 4 samples of the 4x pixel.
  dfn().vkCmdBlitImage(c, full->image, VK_IMAGE_LAYOUT_GENERAL, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_LINEAR);
  textures.CopyToSized(*dest, c);
  resolve_barrier_pending = true;
  return true;
}

void Renderer::Impl::Resolve() {
  const RegisterFile& r = *regs;
  auto copy_control = r.Get<reg::RB_COPY_CONTROL>();
  auto surface_info = r.Get<reg::RB_SURFACE_INFO>();

  // The rectangle (draw_util::GetResolveInfo): Direct3D 9 puts its three
  // vertices in vertex fetch constant 0.
  xenos::xe_gpu_vertex_fetch_t fetch = r.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    return;
  }
  const float* vertices = memory->TranslatePhysical<const float*>(fetch.address << 2);
  float half_pixel =
      r.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t v[6];
  for (uint32_t i = 0; i < 6; ++i) {
    float f = xenos::GpuSwap(vertices[i], fetch.endian) + half_pixel;
    v[i] = int32_t(f * 256.0f);
  }
  int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
  int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
  if (r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    auto offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    x1 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    y1 += offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(r, scissor, false);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  x0 &= ~7;
  y0 &= ~7;
  x1 = rex::align(x1, 8);
  y1 = rex::align(y1, 8);
  x1 = std::min(x1, int32_t(surface_info.surface_pitch));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  uint32_t msaa = HostMsaa(surface_info.msaa_samples);
  bool is_depth = copy_control.copy_src_select >= 4;
  ++stats().resolves;

  BeginRecording();
  EndPass();
  ProfileMark((kProfileResolve << 60) | (uint64_t(is_depth) << 56) |
              (uint64_t(surface_info.surface_pitch & 0x7FF) << 32) |
              (uint64_t(uint32_t(x1 - x0) & 0xFFFF) << 16) | uint32_t(y1 - y0));
  VkCommandBuffer c = cmd();

  RenderTargetKey source_key = {};
  source_key.pitch = surface_info.surface_pitch;
  source_key.msaa = msaa;
  if (is_depth) {
    auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    source_key.base_tiles = depth_info.depth_base;
    source_key.format = uint32_t(depth_info.depth_format);
    source_key.is_depth = 1;
  } else {
    auto color_info =
        r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select]);
    source_key.base_tiles = color_info.color_base;
    source_key.format = uint32_t(color_info.color_format);
  }
  // bo1-nx: a resolve through a 4x view at half the pitch of a 1x surface (nothing drawn into the 4x view
  // itself): on the console each resolved pixel averages the 4 samples, which are a 2x2 block of the 1x surface, so
  // BO1 uses it as a free half-size downsample (its bloom/exposure chain). The empty 4x image read as zeros, the
  // exposure came out 0 and the whole frame black.
  if (!is_depth && source_key.msaa == 2) {
    uint32_t sx = 0, sy = 0;
    RenderTarget* full = FindClearAlias(source_key, sx, sy);
    if (full) {
      auto own = render_targets.find(CanonicalTargetKey(source_key));
      if (own != render_targets.end() && own->second->last_drawn_frame >= full->last_drawn_frame) full = nullptr;
    }
    if (full && sy == 2 && full->samples == VK_SAMPLE_COUNT_1_BIT) {
      if (ResolveDownsampled(full, x0, y0, x1, y1, copy_control.copy_command)) return;
    }
  }
  RenderTarget* source = GetRenderTarget(source_key, uint32_t(y1));
  if (source) source->drawn_since_resolve = false;
  if (trace_frame) {
    TraceEndPass();
    auto trace_dest = r.Get<reg::RB_COPY_DEST_INFO>();
    REXLOG_INFO("[trace] resolve {} rect {},{}-{},{} -> {:08X} cmd {} dest fmt {} swap {} exp {} clear {}{}",
                TraceTarget(source), x0, y0, x1, y1, r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE],
                uint32_t(copy_control.copy_command), uint32_t(trace_dest.copy_dest_format),
                uint32_t(trace_dest.copy_dest_swap), int32_t(trace_dest.copy_dest_exp_bias),
                copy_control.color_clear_enable ? "C" : "", copy_control.depth_clear_enable ? "D" : "");
  }
  if (!source) return;
  c = cmd();
  uint32_t width = uint32_t(x1 - x0);
  uint32_t height = std::min(uint32_t(y1 - y0), source->height - std::min(uint32_t(y0), source->height));
  if (!height) return;

  // Copy.
  uint32_t dest_base = r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
  auto dest_info = r.Get<reg::RB_COPY_DEST_INFO>();
  bool copy = dest_base != 0 &&
              (copy_control.copy_command == xenos::CopyCommand::kRaw ||
               copy_control.copy_command == xenos::CopyCommand::kConvert);
  if (copy) {
    VkMemoryBarrier to_transfer = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    dfn().vkCmdPipelineBarrier(c,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, nullptr,
                               0, nullptr);
    // Sized by what the resolve covers, not by the destination pitch (aligned
    // to 32): textures sample the image across its whole size, and the game
    // samples its full-screen resolves at 1040 wide, where the pitch made the
    // image 1056 and every one of them needed a corrected copy each frame.
    // Smaller textures sampled from a corner still get one (CopyToSized).
    uint32_t dest_width = uint32_t(x1);
    uint32_t dest_height = uint32_t(y0) + height;
    dest_width = std::min(dest_width, 8192u);
    dest_height = std::min(dest_height, 8192u);
    VkFormat dest_format = is_depth ? VK_FORMAT_R32_SFLOAT : source->format;
    uint32_t host_swizzle = is_depth ? uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR)
                                     : uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    if (!is_depth && dest_info.copy_dest_swap) {
      host_swizzle = XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, R, A);
    }
    ResolveDestination* dest = textures.GetResolveDestination(
        dest_base & 0x1FFFFFFF, dest_width, dest_height, dest_format, host_swizzle, c);
    if (trace_frame) trace_last_dest = dest;
    if (!is_depth) NoteResolveExpBias(dest_base, xenos::ColorRenderTargetFormat(source_key.format));
    if (dest) {
      if (is_depth) {
        // Depth cannot be copied into a color image directly. It goes through
        // a scratch buffer, where a compute pass makes both forms a game can
        // read a depth resolve back as: floats, into this R32_SFLOAT image for
        // k_24_8 fetches, and the words the resolve leaves in Xbox 360
        // memory, (depth24 << 8) | stencil, into a second image for fetches
        // as any other format (see TextureCache::kDepthWordsTag). Games read
        // the stencil byte that way to tell the player from the scene: with
        // depth alone in every channel, a post effect meant to skip the skater
        // drew him doubled with a light halo.
        const uint64_t texels = uint64_t(width) * height;
        const VkDeviceSize stencil_offset = rex::align(VkDeviceSize(texels * 4), VkDeviceSize(256));
        const VkDeviceSize packed_offset =
            rex::align(stencil_offset + VkDeviceSize(texels), VkDeviceSize(256));
        const bool has_stencil = HasStencil(source->format);
        const bool unorm_guest = xenos::DepthRenderTargetFormat(source->key.format) ==
                                 xenos::DepthRenderTargetFormat::kD24S8;
        if (msaa == 0 && depth_copy_pipeline && source->sample_view &&
            !textures.WantsDepthWords(dest_base & 0x1FFFFFFF)) {
          // The usual case: the game samples the resolve only as depth.
          VkMemoryBarrier to_compute = {
              VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                  VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(
              c,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_compute, 0, nullptr, 0, nullptr);
          VkDescriptorSet set = AllocateSet(depth_copy_set_layout);
          if (set) {
            VkDescriptorImageInfo images[2] = {
                {VK_NULL_HANDLE, source->sample_view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, dest->view, VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[2] = {};
            for (uint32_t i = 0; i < 2; ++i) {
              writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
              writes[i].dstSet = set;
              writes[i].dstBinding = i;
              writes[i].descriptorCount = 1;
              writes[i].descriptorType =
                  i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
              writes[i].pImageInfo = &images[i];
            }
            dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
            int32_t params[4] = {x0, y0, int32_t(width), int32_t(height)};
            dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_copy_pipeline);
            dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_copy_layout, 0,
                                          1, &set, 0, nullptr);
            dfn().vkCmdPushConstants(c, depth_copy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                     sizeof(params), params);
            dfn().vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
          }
          // Readers: the draws, CopyToSized below, and the next depth test.
          VkMemoryBarrier after_compute = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(
              c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              0, 1, &after_compute, 0, nullptr, 0, nullptr);
        } else if (msaa == 0 && depth_scratch && depth_convert_pipeline &&
                   packed_offset + texels * 4 <= kDepthScratchSize) {
          VkBufferImageCopy regions[2] = {};
          regions[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          regions[0].imageOffset = {x0, y0, 0};
          regions[0].imageExtent = {width, height, 1};
          regions[1] = regions[0];
          regions[1].bufferOffset = stencil_offset;
          regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
          dfn().vkCmdCopyImageToBuffer(c, source->image, VK_IMAGE_LAYOUT_GENERAL, depth_scratch,
                                       has_stencil ? 2 : 1, regions);
          VkMemoryBarrier to_compute = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                        VK_ACCESS_TRANSFER_WRITE_BIT,
                                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_compute, 0,
                                     nullptr, 0, nullptr);
          uint32_t count = uint32_t(texels);
          uint32_t groups = (count + 63) / 64;
          constexpr uint32_t kGroupsPerRow = 4096;
          uint32_t params[5] = {count, kGroupsPerRow * 64, uint32_t(stencil_offset / 4),
                                uint32_t(packed_offset / 4),
                                (source->format == VK_FORMAT_D24_UNORM_S8_UINT ? 1u : 0u) |
                                    (has_stencil ? 2u : 0u)};
          dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_convert_pipeline);
          dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_convert_layout,
                                        0, 1, &depth_convert_set, 0, nullptr);
          dfn().vkCmdPushConstants(c, depth_convert_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(params), params);
          dfn().vkCmdDispatch(c, std::min(groups, kGroupsPerRow),
                              (groups + kGroupsPerRow - 1) / kGroupsPerRow, 1);
          VkMemoryBarrier to_copy = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
          dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_copy, 0, nullptr,
                                     0, nullptr);
          VkBufferImageCopy out = regions[0];
          out.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          dfn().vkCmdCopyBufferToImage(c, depth_scratch, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                       &out);
          // The words only for unorm depth: float24 would need its own
          // encoding, and the game samples those buffers (shadow maps) as depth.
          if (unorm_guest) {
            ResolveDestination* words = textures.GetResolveDestination(
                (dest_base & 0x1FFFFFFF) | TextureCache::kDepthWordsTag, dest_width, dest_height,
                VK_FORMAT_R8G8B8A8_UNORM, uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA), c);
            if (words) {
              out.bufferOffset = packed_offset;
              dfn().vkCmdCopyBufferToImage(c, depth_scratch, words->image,
                                           VK_IMAGE_LAYOUT_GENERAL, 1, &out);
              textures.CopyToSized(*words, c);
            }
          }
        } else {
          static bool logged = false;
          if (!logged) {
            logged = true;
            REXLOG_WARN("[native] Depth resolve not done: {}x{}, {}x MSAA", width, height,
                        1u << msaa);
          }
        }
      } else if (msaa) {
        VkImageResolve resolve = {};
        resolve.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        resolve.dstSubresource = resolve.srcSubresource;
        resolve.srcOffset = {x0, y0, 0};
        resolve.dstOffset = {x0, y0, 0};
        resolve.extent = {width, height, 1};
        vkCmdResolveImage(c, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                                VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
      } else if (source->samples == VK_SAMPLE_COUNT_1_BIT && textures.StorageCapable(dest->format) &&
                 ComputeColorCopy(c, source->view, dest->view, x0, y0, x0, y0, width, height)) {
        // Done by compute (see copy_color.cs.glsl).
      } else {
        VkImageCopy region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.srcOffset = {x0, y0, 0};
        region.dstOffset = {x0, y0, 0};
        region.extent = {width, height, 1};
        dfn().vkCmdCopyImage(c, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      }
      textures.CopyToSized(*dest, c);
    }
  }

  // Clear (RenderTargetCache::PerformTransfersAndResolveClears).
  bool clear_depth = copy_control.depth_clear_enable != 0;
  bool clear_color = !is_depth && copy_control.color_clear_enable != 0;
  if (clear_depth || clear_color) {
    // After a copy the clear must wait for it (it reads what the clear
    // overwrites). Without one, the clear only follows earlier rendering,
    // which the pending resolve barrier or pass order already covers.
    if (copy) {
      VkMemoryBarrier to_clear = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
      dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 1, &to_clear, 0, nullptr, 0, nullptr);
    }
    PassTargets clear_targets;
    RenderTarget* depth_target = nullptr;
    RenderTarget* color_target = nullptr;
    if (clear_depth) {
      auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
      RenderTargetKey key = {};
      key.base_tiles = depth_info.depth_base;
      key.pitch = surface_info.surface_pitch;
      key.format = uint32_t(depth_info.depth_format);
      key.msaa = msaa;
      key.is_depth = 1;
      depth_target = GetRenderTarget(key, uint32_t(y1));
      clear_targets.depth = depth_target;
    }
    if (clear_color) {
      color_target = source;
      clear_targets.color[0] = color_target;
    }
    EndPass();
    if (BeginPass(clear_targets)) {
      c = cmd();
      VkClearAttachment attachments[2] = {};
      uint32_t count = 0;
      if (depth_target) {
        VkClearAttachment& a = attachments[count++];
        a.aspectMask = HasStencil(depth_target->format)
                           ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                           : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT);
        uint32_t clear = r[rex::graphics::XE_GPU_REG_RB_DEPTH_CLEAR];
        uint32_t depth24 = clear >> 8;
        a.clearValue.depthStencil.depth =
            xenos::DepthRenderTargetFormat(depth_target->key.format) ==
                    xenos::DepthRenderTargetFormat::kD24FS8
                ? xenos::Float20e4To32(depth24)
                : xenos::UNorm24To32(depth24);
        a.clearValue.depthStencil.stencil = clear & 0xFF;
      }
      if (color_target) {
        VkClearAttachment& a = attachments[count++];
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.colorAttachment = 0;
        uint64_t value = r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR] |
                         (uint64_t(r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR_LO]) << 32);
        using F = xenos::ColorRenderTargetFormat;
        switch (F(color_target->key.format)) {
          case F::k_8_8_8_8:
          case F::k_8_8_8_8_GAMMA:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] = float((value >> (j * 8)) & 0xFF) / 255.0f;
            }
            break;
          case F::k_2_10_10_10:
          case F::k_2_10_10_10_AS_10_10_10_10:
            for (uint32_t j = 0; j < 3; ++j) {
              a.clearValue.color.float32[j] = float((value >> (j * 10)) & 0x3FF) / 1023.0f;
            }
            a.clearValue.color.float32[3] = float((value >> 30) & 0x3) / 3.0f;
            break;
          case F::k_2_10_10_10_FLOAT:
          case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            for (uint32_t j = 0; j < 3; ++j) {
              a.clearValue.color.float32[j] = xenos::Float7e3To32((value >> (j * 10)) & 0x3FF);
            }
            a.clearValue.color.float32[3] = float((value >> 30) & 0x3) / 3.0f;
            break;
          case F::k_16_16_FLOAT:
          case F::k_16_16_16_16_FLOAT:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] =
                  rex::xenos_half_to_float(uint16_t((value >> (j * 16)) & 0xFFFF));
            }
            break;
          case F::k_16_16:
          case F::k_16_16_16_16:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] = std::max(
                  float(int16_t((value >> (j * 16)) & 0xFFFF)) * (1.0f / 32767.0f), -1.0f);
            }
            break;
          case F::k_32_FLOAT: {
            uint32_t bits = uint32_t(value);
            std::memcpy(&a.clearValue.color.float32[0], &bits, 4);
          } break;
          case F::k_32_32_FLOAT: {
            uint32_t bits[2] = {uint32_t(value), uint32_t(value >> 32)};
            std::memcpy(a.clearValue.color.float32, bits, 8);
          } break;
          default:
            break;
        }
      }
      VkClearRect rect = {};
      rect.rect.offset = {x0, y0};
      rect.rect.extent = {std::min(width, pass_extent.width - std::min(uint32_t(x0), pass_extent.width)),
                          std::min(height, pass_extent.height - std::min(uint32_t(y0), pass_extent.height))};
      rect.layerCount = 1;
      if (rect.rect.extent.width && rect.rect.extent.height) {
        dfn().vkCmdClearAttachments(c, count, attachments, 1, &rect);
      }
      EndPass();
    }
  }

  // The barrier that makes the copies and clears visible is recorded before
  // the next rendering or present, so a run of resolves shares one (each
  // barrier is a full wait for idle on this GPU).
  resolve_barrier_pending = true;
}

void Renderer::Impl::FlushResolveBarrier() {
  if (!resolve_barrier_pending) {
    return;
  }
  resolve_barrier_pending = false;
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_TRANSFER_READ_BIT};
  dfn().vkCmdPipelineBarrier(cmd(),
                             VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
}

// --- Swap ------------------------------------------------------------------------------

void Renderer::Impl::Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width,
                          uint32_t height) {
  // VdSwap describes the frontbuffer in texture fetch constant 0, swizzle
  // included; the SDK presents from it too (RequestSwapTexture). Without the
  // swizzle, red and blue come out swapped.
  xenos::xe_gpu_texture_fetch_t fetch = regs->GetTextureFetch(0);
  uint32_t address = frontbuffer & 0x1FFFFFFF;
  uint32_t guest_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  if (fetch.type == xenos::FetchConstantType::kTexture && fetch.base_address) {
    address = fetch.base_address << 12;
    guest_swizzle = fetch.swizzle;
  }
  uint32_t source_width = 0, source_height = 0;
  VkImageView source_view =
      textures.GetResolveDestinationView2D(address, guest_swizzle, &source_width, &source_height);
  static uint32_t missing_logged = 0;
  if (!source_view && missing_logged < 8) {
    ++missing_logged;
    REXLOG_WARN("[native] Nothing was resolved to the frontbuffer {:08X} ({}x{})", address, width,
                height);
  }
  static bool swap_logged = false;
  if (source_view && !swap_logged) {
    swap_logged = true;
    REXLOG_INFO("[native] Frontbuffer {:08X} {}x{}, swizzle {:03X}", address, width, height,
                guest_swizzle);
  }
  if (presenter && source_view) {
    width = std::min(width ? width : source_width, source_width);
    height = std::min(height ? height : source_height, source_height);
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    uint32_t display_width = std::max(1u, uint32_t(mode.display_width));
    uint32_t display_height = std::max(1u, uint32_t(mode.display_height));
    presenter->RefreshGuestOutput(
        width, height, display_width, display_height,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vulkan_context = static_cast<
              rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          BeginRecording();
          EndPass();
          uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
          std::memcpy(ramp_mapping + 256 * sizeof(uint32_t) * slot, gamma_ramp,
                      sizeof(gamma_ramp));
          VkDescriptorImageInfo source_info = {VK_NULL_HANDLE, source_view,
                                               VK_IMAGE_LAYOUT_GENERAL};
          VkDescriptorImageInfo dest_info = {VK_NULL_HANDLE, vulkan_context.image_view(),
                                             VK_IMAGE_LAYOUT_GENERAL};
          VkWriteDescriptorSet writes[2] = {};
          writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          writes[0].dstSet = swap_sets[slot][1];
          writes[0].descriptorCount = 1;
          writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[0].pImageInfo = &source_info;
          writes[1] = writes[0];
          writes[1].dstSet = swap_sets[slot][2];
          writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          writes[1].pImageInfo = &dest_info;
          dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
          FlushResolveBarrier();
          VkCommandBuffer c = cmd();
          VkImageMemoryBarrier barrier = {};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          barrier.srcAccessMask =
              vulkan_context.image_ever_written_previously()
                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                  : 0;
          barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vulkan_context.image();
          barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
          dfn().vkCmdPipelineBarrier(
              c,
              vulkan_context.image_ever_written_previously()
                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
          dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, swap_pipeline);
          dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, swap_pipeline_layout, 0,
                                        3, swap_sets[slot], 0, nullptr);
          uint32_t size[2] = {width, height};
          dfn().vkCmdPushConstants(c, swap_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(size), size);
          dfn().vkCmdDispatch(c, (width + 15) / 16, (height + 7) / 8, 1);
          barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.dstAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
          barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.newLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
          dfn().vkCmdPipelineBarrier(
              c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
              nullptr, 1, &barrier);
          // Submitted before returning, so the presenter's own work comes
          // after it on the queue.
          Submit();
          context.SetIs8bpc(true);
          return true;
        });
  }
  Submit();
  if (trace_frame && REXCVAR_GET(bo1_debug_dump_swaps)) {
    dump_swaps_left = REXCVAR_GET(bo1_debug_dump_swaps);
  }
  if (dump_swaps_left) {
    --dump_swaps_left;
    if (const ResolveDestination* d = textures.FindResolveDestination(address)) {
      DebugDumpImage(d->image, d->width, d->height, d->format, fmt::format("swap_{:05d}", frame));
    }
  }
  if (trace_frame) {
    for (const auto& dump : trace_texture_dumps) {
      DebugDumpImage(dump.image, dump.width, dump.height, dump.format, dump.name);
      // All mip levels of the first DXT1 512x512 (the noise looked like bad small mips).
      if (dump.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK && dump.width == 512 && !trace_mips_dumped) {
        trace_mips_dumped = true;
        for (uint32_t level = 1; level <= 9; ++level) {
          DebugDumpImage(dump.image, std::max(512u >> level, 1u), std::max(512u >> level, 1u), dump.format,
                         fmt::format("{}_mip{}", dump.name, level), level);
        }
      }
    }
    trace_texture_dumps.clear();
    // Raw guest memory of the first 512x512 DXT1: 128 KB of base and 64 KB from its mip address.
    const std::string dir = REXCVAR_GET(bo1_debug_capture_dir);
    if (trace_raw_mip_address) {
      REXLOG_INFO("[trace] raw texture: base {:08X} (host {}) mips {:08X} (host {}); membase virtual {} physical {}",
                  trace_raw_base_address,
                  static_cast<const void*>(memory->TranslatePhysical<const uint8_t*>(trace_raw_base_address)),
                  trace_raw_mip_address,
                  static_cast<const void*>(memory->TranslatePhysical<const uint8_t*>(trace_raw_mip_address)),
                  static_cast<const void*>(memory->virtual_membase()),
                  static_cast<const void*>(memory->physical_membase()));
    }
    if (trace_raw_mip_address && !dir.empty()) {
      for (auto [address, size, tag] : {std::tuple{trace_raw_base_address, 0x20000u, "base"},
                                        std::tuple{trace_raw_mip_address, 0x10000u, "mips"}}) {
        if (std::FILE* fp = std::fopen(fmt::format("{}/raw_{}_{:08X}.bin", dir, tag, address).c_str(), "wb")) {
          std::fwrite(memory->TranslatePhysical<const uint8_t*>(address), 1, size, fp);
          std::fclose(fp);
        }
      }
      trace_raw_mip_address = 0;
    }
  }
  // Frame boundary.
  auto now = std::chrono::steady_clock::now();
  if (last_swap_time.time_since_epoch().count()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_swap_time).count();
    if (ms > 50) {
      RendererStats& st = stats();
      ++st.frames_over_50ms;
      if (ms > 100) ++st.frames_over_100ms;
      st.slow_frame_pipelines += st.pipelines_created - at_last_swap.pipelines_created;
      st.slow_frame_shaders += st.shaders_translated - at_last_swap.shaders_translated;
      st.slow_frame_uploads += textures.stats().uploads - at_last_swap.textures_uploaded;
      st.slow_frame_render_targets +=
          st.render_targets_created - at_last_swap.render_targets_created;
    }
  }
  last_swap_time = now;
  at_last_swap = stats();
  at_last_swap.textures_uploaded = textures.stats().uploads;
  // Save the driver's pipeline cache once new pipelines stop appearing for a
  // while, off this thread: the file is tens of megabytes on the SD card.
  if (!driver_cache_path.empty() && stats().pipelines_created != pipelines_at_last_save &&
      now - last_pipeline_time > std::chrono::seconds(5) && !driver_cache_writing.load()) {
    SaveDriverCache();
  }
  stats().textures_uploaded = textures.stats().uploads;
  stats().texture_bytes_uploaded = textures.stats().upload_bytes;
  stats().textures_created = textures.stats().created;
  stats().textures_streamed = textures.stats().streamed;
  stats().textures_pending = textures.stats().pending;
  stats().texture_memory_bytes = textures.stats().texture_memory_bytes;
  stats().texture_budget_evictions = textures.stats().budget_evictions;
  stats().texture_placeholders = textures.stats().placeholders;
  stats().texture_hash_ns = textures.stats().hash_ns;
  stats().texture_hash_bytes = textures.stats().hash_bytes;
  stats().texture_create_ns = textures.stats().create_ns;
  stats().texture_creates = textures.stats().creates_on_ring;
  stats().textures_replaced = textures.stats().replaced;
  stats().geometry_hash_ns = geometry_hash_ns;
  stats().geometry_hash_bytes = geometry_hash_bytes;
  stats().vertex_copy_ns = vertex_copy_ns;
  stats().vertex_bytes_whole = vertex_bytes_whole;
  stats().frame_upload_wait_ns = frame_upload_wait_ns;
  stats().frame_fence_wait_ns = frame_fence_wait_ns;
  stats().frame_drain_wait_ns = frame_drain_wait_ns;
  stats().frame_texture_ns = frame_texture_ns;
  stats().texture_lookup_ns += frame_texture_ns;
  stats().frame_draws = frame_draw_count;
  stats().frame_stream_wait_ns = frame_stream_wait_ns;
  frame_stream_wait_ns = 0;
  frame_upload_wait_ns = frame_fence_wait_ns = frame_drain_wait_ns = frame_texture_ns = 0;
  last_frame_draw_count = frame_draw_count;
  frame_draw_count = 0;
  if (trace_frame) {
    TraceEndPass();
    for (const auto& [key, value] : trace_texture_formats) {
      REXLOG_INFO("[trace] textures: {:4} x {} (e.g. {})", value.first, key, value.second);
    }
    trace_texture_formats.clear();
    REXLOG_INFO("[trace] swap, end of frame {}", frame);
  }
  if (REXCVAR_GET(bo1_debug_mip_watch) && frame % 600 == 0) ReportMipWatch();
  ++frame;
  trace_frame = REXCVAR_GET(bo1_debug_trace_frame) && frame == REXCVAR_GET(bo1_debug_trace_frame);
  {
    static const auto trace_clock_start = std::chrono::steady_clock::now();
    static bool traced_after = false;
    const uint32_t after_s = REXCVAR_GET(bo1_debug_trace_after_s);
    // After the delay, the first frame following one with 1000+ draws: in the map, not on a loading screen.
    if (after_s && !traced_after && last_frame_draw_count >= 1000 &&
        std::chrono::steady_clock::now() - trace_clock_start >= std::chrono::seconds(after_s)) {
      traced_after = true;
      trace_frame = true;
    }
  }
  if (trace_frame) REXLOG_INFO("[trace] frame {} begins", frame);
  profile_frame = profile_pool && frame % 16 == 0;
  PollCompleted();
  ProfileReport();
  textures.BeginFrame(frame, completed_frame);
  FreeRetiredSetPools();
  for (auto it = retired_render_targets.begin(); it != retired_render_targets.end();) {
    if (it->frame <= completed_frame) {
      if (it->sample_view) dfn().vkDestroyImageView(vk, it->sample_view, nullptr);
      dfn().vkDestroyImageView(vk, it->view, nullptr);
      dfn().vkDestroyImage(vk, it->image, nullptr);
      dfn().vkFreeMemory(vk, it->memory, nullptr);
      it = retired_render_targets.erase(it);
    } else {
      ++it;
    }
  }
}

// --- Driver pipeline cache ---------------------------------------------------------------

namespace {
constexpr uint64_t kDriverCacheMagic = 0x3148434E54414E52ULL;  // "RNATNCH1"

std::vector<uint8_t> ReadDriverCache(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  if (FILE* file = rex::filesystem::OpenFile(path, "rb")) {
    uint64_t header[3] = {};
    if (fread(header, sizeof(header), 1, file) == 1 && header[0] == kDriverCacheMagic &&
        header[1] >= 32 && header[1] <= 128u * 1024 * 1024) {
      bytes.resize(size_t(header[1]));
      if (fread(bytes.data(), bytes.size(), 1, file) != 1 ||
          XXH3_64bits(bytes.data(), bytes.size()) != header[2]) {
        bytes.clear();
      }
    }
    fclose(file);
  }
  return bytes;
}
// SPIR-V cache file: a header, then records appended as shaders are translated. A record that does not check out
// ends the file: the next record written goes over it.
constexpr uint64_t kSpirvCacheMagic = 0x3156505354414E52ULL;  // "RNATSPV1"
// Bump when the translator or the modifications the renderer uses change, so old SPIR-V is not reused.
constexpr uint32_t kSpirvCacheVersion = 1;
constexpr uint32_t kSpirvRecordMagic = 0x43455253;  // "SREC"
constexpr size_t kSpirvCacheMaxBytes = 96u * 1024 * 1024;

struct SpirvCacheHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t driver_version;
};

struct SpirvRecordHeader {
  uint32_t magic;
  uint32_t type;
  uint64_t ucode_hash;
  uint64_t modification;
  uint32_t ucode_dwords;
  uint32_t texture_count;
  uint32_t sampler_count;
  uint32_t binary_size;
  uint64_t checksum;  // of the record header up to here, the bindings and the binary
};
static_assert(sizeof(SpirvShader::TextureBinding) == 4 && sizeof(SpirvShader::SamplerBinding) == 4);

uint64_t SpirvCacheKey(uint32_t type, uint64_t ucode_hash, uint64_t modification) {
  const uint64_t key[] = {type, ucode_hash, modification};
  return XXH3_64bits(key, sizeof(key));
}

uint64_t SpirvRecordChecksum(const SpirvRecordHeader& header, const void* payload, size_t size) {
  return XXH3_64bits_withSeed(payload, size,
                              XXH3_64bits(&header, offsetof(SpirvRecordHeader, checksum)));
}
}  // namespace

bool Renderer::Impl::TakeCachedTranslation(VulkanShader::VulkanTranslation* translation) {
  if (spirv_cache.empty()) {
    return false;
  }
  Shader& shader = translation->shader();
  auto it = spirv_cache.find(SpirvCacheKey(uint32_t(shader.type()), shader.ucode_data_hash(),
                                           translation->modification()));
  if (it == spirv_cache.end()) {
    return false;
  }
  CachedSpirv entry = std::move(it->second);
  spirv_cache.erase(it);
  if (entry.type != uint32_t(shader.type()) || entry.ucode_hash != shader.ucode_data_hash() ||
      entry.modification != translation->modification() ||
      entry.ucode_dwords != shader.ucode_dword_count()) {
    return false;
  }
  static_cast<SpirvShader&>(shader).SetBindingsFromCache(entry.textures.data(), entry.textures.size(),
                                                         entry.samplers.data(), entry.samplers.size());
  translation->SetTranslatedBinary(std::move(entry.binary));
  return translation->GetOrCreateShaderModule() != VK_NULL_HANDLE;
}

void Renderer::Impl::StoreTranslation(VulkanShader::VulkanTranslation* translation) {
  std::lock_guard<std::mutex> lock(spirv_cache_mutex);
  if (!spirv_cache_file || !translation->is_valid()) {
    return;
  }
  const auto& shader = static_cast<const SpirvShader&>(translation->shader());
  const auto& textures = shader.GetTextureBindingsAfterTranslation();
  const auto& samplers = shader.GetSamplerBindingsAfterTranslation();
  const auto& binary = translation->translated_binary();
  std::vector<uint8_t> payload((textures.size() + samplers.size()) * 4 + binary.size());
  if (!textures.empty()) std::memcpy(payload.data(), textures.data(), textures.size() * 4);
  if (!samplers.empty()) {
    std::memcpy(payload.data() + textures.size() * 4, samplers.data(), samplers.size() * 4);
  }
  if (!binary.empty()) {
    std::memcpy(payload.data() + (textures.size() + samplers.size()) * 4, binary.data(), binary.size());
  }
  SpirvRecordHeader header = {};
  header.magic = kSpirvRecordMagic;
  header.type = uint32_t(shader.type());
  header.ucode_hash = shader.ucode_data_hash();
  header.modification = translation->modification();
  header.ucode_dwords = uint32_t(shader.ucode_dword_count());
  header.texture_count = uint32_t(textures.size());
  header.sampler_count = uint32_t(samplers.size());
  header.binary_size = uint32_t(binary.size());
  header.checksum = SpirvRecordChecksum(header, payload.data(), payload.size());
  bool written = fwrite(&header, sizeof(header), 1, spirv_cache_file) == 1 &&
                 fwrite(payload.data(), payload.size(), 1, spirv_cache_file) == 1 &&
                 fflush(spirv_cache_file) == 0;
  if (!written) {
    // Stop writing; the partial record fails its checksum next time.
    fclose(spirv_cache_file);
    spirv_cache_file = nullptr;
    REXLOG_WARN("[native] SPIR-V cache: write failed, not caching more shaders this session");
  }
}

void Renderer::InitializeSpirvCache(const std::filesystem::path& directory, uint32_t title_id) {
  Impl& im = *impl_;
  const auto& properties = im.device->properties();
  auto path = directory / fmt::format("native-{:08X}-{:08X}-{:08X}-{:08X}.spirv", title_id,
                                      properties.vendorID, properties.deviceID,
                                      properties.driverVersion);
  const SpirvCacheHeader expected = {kSpirvCacheMagic, kSpirvCacheVersion, properties.vendorID,
                                     properties.deviceID, properties.driverVersion};
  // Records with a checksum that fails are where the valid data ends.
  long valid_end = 0;
  size_t loaded_bytes = 0;
  if (FILE* file = rex::filesystem::OpenFile(path, "r+b")) {
    SpirvCacheHeader header = {};
    if (fread(&header, sizeof(header), 1, file) == 1 &&
        std::memcmp(&header, &expected, sizeof(header)) == 0) {
      valid_end = long(sizeof(header));
      std::vector<uint8_t> payload;
      SpirvRecordHeader record;
      while (loaded_bytes < kSpirvCacheMaxBytes && fread(&record, sizeof(record), 1, file) == 1) {
        if (record.magic != kSpirvRecordMagic || record.binary_size == 0 ||
            record.binary_size > 4u * 1024 * 1024 || record.texture_count > 64 ||
            record.sampler_count > 64) {
          break;
        }
        size_t binding_bytes = size_t(record.texture_count + record.sampler_count) * 4;
        payload.resize(binding_bytes + record.binary_size);
        if (fread(payload.data(), payload.size(), 1, file) != 1 ||
            SpirvRecordChecksum(record, payload.data(), payload.size()) != record.checksum) {
          break;
        }
        Impl::CachedSpirv& entry =
            im.spirv_cache[SpirvCacheKey(record.type, record.ucode_hash, record.modification)];
        entry.type = record.type;
        entry.ucode_hash = record.ucode_hash;
        entry.modification = record.modification;
        entry.ucode_dwords = record.ucode_dwords;
        entry.textures.resize(record.texture_count);
        entry.samplers.resize(record.sampler_count);
        if (record.texture_count) {
          std::memcpy(entry.textures.data(), payload.data(), size_t(record.texture_count) * 4);
        }
        if (record.sampler_count) {
          std::memcpy(entry.samplers.data(), payload.data() + size_t(record.texture_count) * 4,
                      size_t(record.sampler_count) * 4);
        }
        entry.binary.assign(payload.begin() + binding_bytes, payload.end());
        loaded_bytes += payload.size();
        valid_end = ftell(file);
      }
    }
    if (valid_end > 0 && fseek(file, valid_end, SEEK_SET) == 0) {
      im.spirv_cache_file = file;
    } else {
      fclose(file);
    }
  }
  if (!im.spirv_cache_file) {
    // Missing, from another driver or version, or unreadable: start over.
    im.spirv_cache.clear();
    loaded_bytes = 0;
    if (FILE* file = rex::filesystem::OpenFile(path, "wb")) {
      if (fwrite(&expected, sizeof(expected), 1, file) == 1 && fflush(file) == 0) {
        im.spirv_cache_file = file;
      } else {
        fclose(file);
      }
    }
  }
  REXLOG_INFO("[native] SPIR-V cache: {} translations ({} KB) loaded from {}{}", im.spirv_cache.size(),
              loaded_bytes >> 10, path.string(), im.spirv_cache_file ? "" : " (not writable)");
}

void Renderer::Impl::ListPipeline(uint64_t hash, const PipelineKey& key) {
  if (listed_pipelines.insert(hash).second) {
    pipelines_to_list.push_back(key);
  }
}

namespace {
constexpr uint64_t kPipelineListMagic = 0x314C505354414E52ULL;  // "RNATSPL1"
// Bump when what a PipelineKey means changes.
constexpr uint32_t kPipelineListVersion = 1;
struct PipelineListHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t key_size;
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t driver_version;
  uint32_t reserved;
};
}  // namespace

void Renderer::PrewarmPipelines(const std::filesystem::path& directory, uint32_t title_id) {
  Impl& im = *impl_;
  const auto& properties = im.device->properties();
  auto path = directory / fmt::format("native-{:08X}-{:08X}-{:08X}-{:08X}.pipelines", title_id,
                                      properties.vendorID, properties.deviceID,
                                      properties.driverVersion);
  const PipelineListHeader expected = {kPipelineListMagic, kPipelineListVersion,
                                       uint32_t(sizeof(PipelineKey)), properties.vendorID,
                                       properties.deviceID, properties.driverVersion, 0};
  std::vector<PipelineKey> keys;
  long valid_end = 0;
  if (FILE* file = rex::filesystem::OpenFile(path, "r+b")) {
    PipelineListHeader header = {};
    if (fread(&header, sizeof(header), 1, file) == 1 &&
        std::memcmp(&header, &expected, sizeof(header)) == 0) {
      valid_end = long(sizeof(header));
      PipelineKey key;
      uint64_t checksum;
      while (keys.size() < 8192 && fread(&key, sizeof(key), 1, file) == 1 &&
             fread(&checksum, sizeof(checksum), 1, file) == 1 &&
             XXH3_64bits(&key, sizeof(key)) == checksum) {
        keys.push_back(key);
        valid_end = ftell(file);
      }
    }
    if (valid_end > 0 && fseek(file, valid_end, SEEK_SET) == 0) {
      im.pipeline_list_file = file;
    } else {
      fclose(file);
    }
  }
  if (!im.pipeline_list_file) {
    keys.clear();
    if (FILE* file = rex::filesystem::OpenFile(path, "wb")) {
      if (fwrite(&expected, sizeof(expected), 1, file) == 1 && fflush(file) == 0) {
        im.pipeline_list_file = file;
      } else {
        fclose(file);
      }
    }
  }
  if (!im.pipeline_thread.joinable() || !im.driver_cache) {
    for (const PipelineKey& key : keys) {
      im.listed_pipelines.insert(XXH3_64bits(&key, sizeof(key)));
    }
    return;
  }
  // One module per cached translation used, shared by the pipelines that use it.
  std::unordered_map<uint64_t, VkShaderModule> modules;
  auto module_for = [&](xenos::ShaderType type, uint64_t hash, uint64_t modification) -> VkShaderModule {
    uint64_t cache_key = SpirvCacheKey(uint32_t(type), hash, modification);
    auto made = modules.find(cache_key);
    if (made != modules.end()) return made->second;
    VkShaderModule module = VK_NULL_HANDLE;
    auto entry = im.spirv_cache.find(cache_key);
    if (entry != im.spirv_cache.end() && entry->second.ucode_hash == hash &&
        entry->second.modification == modification && entry->second.type == uint32_t(type)) {
      VkShaderModuleCreateInfo info = {};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = entry->second.binary.size();
      info.pCode = reinterpret_cast<const uint32_t*>(entry->second.binary.data());
      if (im.dfn().vkCreateShaderModule(im.vk, &info, nullptr, &module) != VK_SUCCESS) {
        module = VK_NULL_HANDLE;
      } else {
        im.prewarm_modules.push_back(module);
      }
    }
    modules.emplace(cache_key, module);
    return module;
  };
  std::deque<Impl::PipelineJob> jobs;
  uint32_t missing = 0;
  for (const PipelineKey& key : keys) {
    uint64_t hash = XXH3_64bits(&key, sizeof(key));
    im.listed_pipelines.insert(hash);
    if (im.pipelines.count(hash) || im.pending_pipelines.count(hash)) continue;
    Impl::PipelineJob job;
    job.hash = hash;
    job.key = key;
    job.prewarm = true;
    job.vertex = module_for(xenos::ShaderType::kVertex, key.vs_hash, key.vs_modification);
    if (key.ps_hash) {
      job.pixel = module_for(xenos::ShaderType::kPixel, key.ps_hash, key.ps_modification);
    }
    const PipelineLayout* layout =
        im.GetPipelineLayout(uint32_t(key.layout >> 48), uint32_t(key.layout >> 32) & 0xFFFF,
                             uint32_t(key.layout >> 16) & 0xFFFF, uint32_t(key.layout) & 0xFFFF);
    job.layout = layout ? layout->layout : VK_NULL_HANDLE;
    if (key.geometry_shader) {
      auto it = im.geometry_shaders.find(key.geometry_shader);
      if (it == im.geometry_shaders.end()) {
        GeometryShaderKey gs_key;
        gs_key.key = key.geometry_shader;
        it = im.geometry_shaders.emplace(key.geometry_shader, CreateGeometryShader(im.device, gs_key)).first;
      }
      job.geometry = it->second;
    }
    if (!job.vertex || (key.ps_hash && !job.pixel) || !job.layout ||
        (key.geometry_shader && !job.geometry)) {
      ++missing;
      continue;
    }
    im.pending_pipelines.insert(hash);
    jobs.push_back(job);
  }
  REXLOG_INFO("[native] Pipeline list: {} known, {} to compile at boot, {} without their shaders cached; {}",
              keys.size(), jobs.size(), missing, path.string());
  if (jobs.empty()) {
    for (VkShaderModule module : im.prewarm_modules) {
      im.dfn().vkDestroyShaderModule(im.vk, module, nullptr);
    }
    im.prewarm_modules.clear();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(im.pipeline_mutex);
    im.prewarm_start = std::chrono::steady_clock::now();
    im.prewarm_remaining = uint32_t(jobs.size());
    for (auto& job : jobs) im.prewarm_jobs.push_back(job);
  }
  im.pipeline_cv.notify_one();
}

void Renderer::InitializeDriverCache(const std::filesystem::path& cache_root,
                                     uint32_t title_id) {
  Impl& im = *impl_;
  if (!im.device) {
    return;
  }
  std::error_code error;
  auto directory = cache_root / "shaders" / "driver";
  std::filesystem::create_directories(directory, error);
  if (error) {
    return;
  }
  InitializeSpirvCache(directory, title_id);
  const auto& properties = im.device->properties();
  im.driver_cache_path =
      directory / fmt::format("native-{:08X}-{:08X}-{:08X}-{:08X}.cache", title_id,
                              properties.vendorID, properties.deviceID, properties.driverVersion);
  // The newest complete file: the rename that replaces it can fail on Horizon
  // (it does not overwrite), leaving the data in .tmp.
  std::vector<uint8_t> data = ReadDriverCache(im.driver_cache_path);
  auto temporary = im.driver_cache_path;
  temporary += ".tmp";
  std::vector<uint8_t> newer = ReadDriverCache(temporary);
  if (newer.size() > data.size()) {
    data = std::move(newer);
  }
  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = data.size();
  info.pInitialData = data.empty() ? nullptr : data.data();
  VkPipelineCache cache = VK_NULL_HANDLE;
  if (im.dfn().vkCreatePipelineCache(im.vk, &info, nullptr, &cache) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (im.dfn().vkCreatePipelineCache(im.vk, &info, nullptr, &cache) != VK_SUCCESS) {
      return;
    }
    data.clear();
  }
  // Called before the guest runs, so the ring thread is not using the old one.
  if (im.driver_cache) {
    im.dfn().vkDestroyPipelineCache(im.vk, im.driver_cache, nullptr);
  }
  im.driver_cache = cache;
  im.driver_cache_saved_size = data.size();
  REXLOG_INFO("[native] Driver pipeline cache: {} KB loaded from {}", data.size() >> 10,
              im.driver_cache_path.string());
  PrewarmPipelines(directory, title_id);
}

void Renderer::Impl::SaveDriverCache() {
  pipelines_at_last_save = stats().pipelines_created;
  auto keys = std::make_shared<std::vector<PipelineKey>>(std::move(pipelines_to_list));
  pipelines_to_list.clear();
  size_t size = 0;
  std::shared_ptr<std::vector<uint8_t>> data;
  if (dfn().vkGetPipelineCacheData(vk, driver_cache, &size, nullptr) == VK_SUCCESS && size >= 32 &&
      size != driver_cache_saved_size) {
    data = std::make_shared<std::vector<uint8_t>>(size);
    if (dfn().vkGetPipelineCacheData(vk, driver_cache, &size, data->data()) == VK_SUCCESS) {
      data->resize(size);
      driver_cache_saved_size = size;
    } else {
      data.reset();
    }
  }
  if (!data && keys->empty()) {
    return;
  }
  driver_cache_writing = true;
  std::filesystem::path path = driver_cache_path;
  // Joined, never detached: detaching throws on Horizon, which ended the game.
  if (driver_cache_writer.joinable()) {
    driver_cache_writer.join();
  }
  driver_cache_writer = std::thread([this, data, keys, path]() {
    if (pipeline_list_file && !keys->empty()) {
      bool written = true;
      for (const PipelineKey& key : *keys) {
        const uint64_t checksum = XXH3_64bits(&key, sizeof(key));
        written = written && fwrite(&key, sizeof(key), 1, pipeline_list_file) == 1 &&
                  fwrite(&checksum, sizeof(checksum), 1, pipeline_list_file) == 1;
      }
      if (!written || fflush(pipeline_list_file) != 0) {
        fclose(pipeline_list_file);
        pipeline_list_file = nullptr;
      }
    }
    if (!data) {
      driver_cache_writing = false;
      return;
    }
    auto temporary = path;
    temporary += ".tmp";
    if (FILE* file = rex::filesystem::OpenFile(temporary, "wb")) {
      const uint64_t header[] = {kDriverCacheMagic, uint64_t(data->size()),
                                 XXH3_64bits(data->data(), data->size())};
      bool written = fwrite(header, sizeof(header), 1, file) == 1 &&
                     fwrite(data->data(), data->size(), 1, file) == 1;
      if (fclose(file) != 0) written = false;
      if (written) {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::rename(temporary, path, error);
      }
    }
    driver_cache_writing = false;
  });
}

// --- Renderer ---------------------------------------------------------------------------

Renderer::Renderer() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Initialize(rex::ui::vulkan::VulkanProvider* provider, rex::memory::Memory* memory,
                          RegisterFile* regs) {
  impl_->provider = provider;
  impl_->memory = memory;
  impl_->regs = regs;
  if (!impl_->Initialize()) {
    impl_->Shutdown();
    return false;
  }
  return true;
}

std::string Renderer::TakeTextureUploadReport() {
  return impl_ ? impl_->textures.TakeUploadReport() : std::string();
}

void Renderer::Flush() {
  if (impl_) impl_->Submit();
}

void Renderer::KickRecorder() {
  if (impl_) impl_->KickRecorder();
}

void Renderer::Shutdown() {
  if (impl_) {
    impl_->Shutdown();
  }
}

void Renderer::LoadShader(xenos::ShaderType type, const uint32_t* ucode, uint32_t dword_count,
                          uint32_t guest_address) {
  // The game switches shaders for most draws, loading them from the same few
  // hundred addresses: remember them by address and size, confirmed by a hash
  // of all of the code. A sample of it (the first, middle and last 16 words)
  // was tried first and was not enough: a level loading its shaders where the
  // previous screen's had been got the old shader whenever the two agreed at
  // the sampled words, and drew its world with the wrong vertex shader.
  // A full hash on every load (twice a draw, 12 MB a frame at 3000 draws) was
  // too slow, so each location is verified in full once a frame, and within
  // the frame by its first words: shaders are replaced between screens, not
  // in the middle of drawing one.
  uint64_t location = 0;
  uint64_t sample = 0;
  uint64_t head = 0;
  if (guest_address) {
    location = (uint64_t(guest_address) << 20) | (uint64_t(dword_count) << 2) | uint64_t(type);
    auto known = impl_->shaders_by_location.find(location);
    head = XXH3_64bits(ucode, std::min(dword_count, 16u) * sizeof(uint32_t));
    if (known != impl_->shaders_by_location.end() &&
        known->second.verified_frame == impl_->frame && known->second.head == head) {
      if (type == xenos::ShaderType::kVertex) {
        impl_->active_vs = known->second.shader;
      } else {
        impl_->active_ps = known->second.shader;
      }
      return;
    }
    sample = XXH3_64bits(ucode, dword_count * sizeof(uint32_t));
    if (known != impl_->shaders_by_location.end() && known->second.sample == sample) {
      known->second.verified_frame = impl_->frame;
      known->second.head = head;
      if (type == xenos::ShaderType::kVertex) {
        impl_->active_vs = known->second.shader;
      } else {
        impl_->active_ps = known->second.shader;
      }
      return;
    }
  }
  uint64_t hash = guest_address ? sample : XXH3_64bits(ucode, dword_count * sizeof(uint32_t));
  auto& shaders = impl_->shaders;
  auto it = shaders.find(hash);
  VulkanShader* shader;
  if (it != shaders.end()) {
    shader = it->second.get();
  } else {
    auto created = std::make_unique<VulkanShader>(impl_->device, type, hash, ucode, dword_count);
    shader = created.get();
    shaders.emplace(hash, std::move(created));
  }
  if (type == xenos::ShaderType::kVertex) {
    impl_->active_vs = shader;
  } else {
    impl_->active_ps = shader;
  }
  if (location) {
    impl_->shaders_by_location[location] = {shader, sample, impl_->frame, head};
  }
}

namespace {
struct CpuTimer {
  uint64_t& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  explicit CpuTimer(uint64_t& t) : total(t) {}
  ~CpuTimer() {
    total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count());
  }
};
}  // namespace

void Renderer::Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words) {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Draw(immediate_indices, immediate_index_words);
}

void Renderer::Resolve() {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Resolve();
  impl_->DebugDumpResolve();
}

void Renderer::Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width,
                    uint32_t height) {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Swap(presenter, frontbuffer, width, height);
}

void Renderer::SetGammaRamp256(const uint32_t* entries) {
  std::memcpy(impl_->gamma_ramp, entries, sizeof(impl_->gamma_ramp));
}

}  // namespace bo1::native
