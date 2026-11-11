# Carrottus GPU Samples

Ports of the [`microsoft/DirectX-Graphics-Samples`](https://github.com/microsoft/DirectX-Graphics-Samples)
(cloned alongside this repo as `../d3d12samples`) rebuilt against **Carrottus GPU**
(`carrottus`'s GPU HAL module) instead of raw D3D12/DXGI. Each sample under
`Samples/` mirrors one sample from `d3d12samples/Samples/Desktop/` as closely
as its feature set allows, replacing every `ID3D12*`/DXGI call with the
equivalent `crgpu_*` call.

The point isn't a demo reel — it's a maturity test for Carrottus. Every sample
that ports cleanly validates a slice of Carrottus GPU's API; every sample that
*can't* port as written (no ray tracing pipeline, no VRS, no cross-adapter
API as of this writing) is a concrete, prioritized gap to close in `carrottus` itself — not a
workaround to invent here.

## Status

| Sample | D3D12 source | Status |
|---|---|---|
| HelloTriangle | `D3D12HelloWorld/HelloTriangle` | Ported |
| HelloConstBuffers | `D3D12HelloWorld/HelloConstBuffers` | Ported |
| HelloTexture | `D3D12HelloWorld/HelloTexture` | Ported, all backends (see note below) |
| CrGPUExecuteIndirect | `D3D12ExecuteIndirect` | Ported (see note below) |
| CrGPUDynamicIndexing | `D3D12DynamicIndexing` | Ported, Vulkan/D3D12 only (see note below) |
| CrGPUMeshShaders | `D3D12MeshShaders/MeshletRender` | Ported, all backends (see note below) |
| CrGPUBundles | `D3D12Bundles` | Ported, Vulkan/D3D12 only (see note below) |
| CrGPUDepthBoundsTest | `D3D12DepthBoundsTest` | Ported, all backends (see note below) |
| CrGPUHeaps | `D3D12SmallResources` | Ported, all backends; run on D3D12, Vulkan and Metal (see note below) |
| CrGPURayTracing | `D3D12Raytracing`: `D3D12RaytracingHelloWorld`, `D3D12RaytracingSimpleLighting`, `D3D12RaytracingProceduralGeometry` | Ported with inline ray queries in place of ray tracing pipelines, all backends; run on D3D12, Vulkan and Metal (see note below) |

`HelloTexture` closed a real gap and worked around another. The gap it
closed: `crgpu_command_list_copy_buffer_to_image`/`_copy_image_to_buffer` and
`crgpu_image_get_buffer_copy_size` were unimplemented placeholders on
Metal — `crgpumetalrenderpass.m`'s file comment already flagged its
copy/blit functions as such, "no caller needs them yet" (see
`CrGPUMeshShaders` above, whose buffer-to-buffer copy hit the same
placeholder). This sample's texture upload was the first caller to actually
need buffer-to-image, and it silently produced a solid-black texture until
these were implemented for real via `MTLBlitCommandEncoder`
(`copyFromBuffer:toTexture:`/`copyFromTexture:toBuffer:`), gated behind a new
`_active_blit_encoder` on the command list that ends any active render/compute
encoder before opening (mirroring `_active_compute_encoder`'s existing
pattern) since Metal disallows two live encoders of different kinds on one
command buffer at once.

The gap this port works around instead of closing:
`CRGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` — declared in `crgpu.h` and
implemented on Vulkan/D3D12 since the type was added — doesn't actually work
on Metal. `crgpumetaldescriptors.m` already documented that a combined
descriptor's paired sampler is "intentionally not written" there, and
cross-compiling a GLSL `sampler2D` confirms why: under Carrottus GPU's
per-descriptor-set-argument-buffer scheme, SPIRV-Cross's synthesized sampler
member overlaps the texture's own binding slot, which it only permits via
"full mutable aliasing" on Metal 3+ (`spirv_msl.cpp`). This port doesn't fix
that gap — it works around it the same way `CrGPUDynamicIndexing`'s
bindless design already had to: a `texture2D` at one binding and a plain
`sampler` at the next, written as separate `CRGPU_DESCRIPTOR_TYPE_SAMPLED_
IMAGE`/`CRGPU_DESCRIPTOR_TYPE_SAMPLER` descriptors instead of one combined
one. This is the first sample to exercise that split-descriptor idiom outside
bindless, and the first to exercise ordinary (non-bindless) texture sampling
on Metal at all. Two smaller, harmless deviations alongside it: a real
dynamic `crgpu_sampler` stands in for the original's static/immutable
root-signature sampler (`crgpu_sampler_desc` has no such mechanism yet —
see `carrottus/CLAUDE.md`'s "Known gaps"), and `CLAMP_TO_EDGE` addressing
replaces the original's `CLAMP_TO_BORDER` with a transparent-black border
(`crgpu_sampler_desc` has no border-color field) — this sample's UVs never
leave `[0, 1]`, so the two modes look identical here regardless.

`CrGPUExecuteIndirect` closed a real gap: `crgpu` had no compute-dispatch
call at all, and no buffer pipeline barriers on any backend, both now added
alongside `crgpu_command_list_draw_indirect`/`_indexed_indirect`. The port deviates
from the original in one deliberate, documented way, and initially shipped a
second deviation that turned out to be a genuine D3D12 portability bug rather
than a valid simplification (see below):

- **No GPU-side compaction.** The original appends visible triangles into a
  tightly-packed buffer (an HLSL `AppendStructuredBuffer`, i.e. a UAV atomic
  counter) and issues a GPU-authored *variable* draw count
  (`ExecuteIndirect`'s count-buffer argument / `vkCmdDrawIndirectCount`).
  Metal has no equivalent primitive for that shape — a GPU-authored variable
  indirect draw count needs an `MTLIndirectCommandBuffer`, a structurally
  different object encoded via its own API. This port's compute pass instead
  writes one draw-args entry per triangle at a fixed index (zeroed
  `vertex_count` for a culled triangle), and the renderer always issues a
  single fixed-count `crgpu_command_list_draw_indirect` — fully portable, same
  demonstrated mechanism (a compute pass decides per-triangle what gets
  drawn), just without the bandwidth-compaction optimization.
- **No per-draw root CBV update, and no `gl_InstanceIndex` either.** The
  original's command signature updates a root constant-buffer-view descriptor
  per draw, alongside the draw itself — a D3D12-specific indirect-argument
  shape with no Vulkan/Metal equivalent, so this port doesn't use it. The
  first attempt at a portable substitute baked each triangle's index into its
  draw's `first_instance` and read it back in the vertex shader via
  `gl_InstanceIndex`/`SV_InstanceID` — this is broken on D3D12: unlike
  Vulkan's `gl_InstanceIndex`, D3D12's `SV_InstanceID` does not include
  `StartInstanceLocation`, so every one of the 256 indirect draws read
  instance 0's data regardless of its actual `first_instance` (SPIRV-Cross
  can emulate this via a `SPIRV_Cross_BaseInstance` root constant, but that
  constant has to be fed per sub-draw through `ExecuteIndirect`'s own
  per-draw root-constant-update argument — exactly the D3D12-only mechanism
  this port was trying to avoid). The actual portable substitute: `scene`'s
  per-triangle offset/color is bound as a second, **per-instance vertex
  buffer** (`per_instance = true` on its `crgpu_vertex_binding_desc`)
  instead of a storage/uniform buffer indexed by instance ID. The
  fixed-function input assembler's per-instance vertex fetch *does* correctly
  offset by `StartInstanceLocation` on every backend — this is a separate
  mechanism from the `SV_InstanceID`/`gl_InstanceIndex` system value, and is
  the standard, actually-portable technique for per-instance data. A
  `first_instance` other than 0 in an indirect draw's arguments needs a
  device created with `CRGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE`
  (Vulkan's `drawIndirectFirstInstance`; every D3D12 and Metal adapter), so
  the sample asks for it and stops on an adapter without it.

`CrGPUDynamicIndexing` exercises `crgpu`'s bindless descriptor indexing
feature for the first time (`crgpu_bindless_register_sampled_image`,
`crgpu_command_list_bind_graphics_bindless_resources`, ...) — a 15x8 grid of building meshes,
each dynamically indexing into one shared unbounded texture table for its own
procedurally-generated material color. That feature was already fully
implemented on Vulkan/D3D12 before this port (just never exercised by a
sample); the one real gap this port closed was **Carrottus GPU had no sampler
object on any backend** — now added (`crgpu_sampler_create`). Bindless is
`0` on Metal (needs a `MTLHeap`-backed allocator Carrottus GPU doesn't have yet —
see `carrottus/CLAUDE.md`), so this sample only builds against Vulkan/D3D12;
`spudgpusamples/CMakeLists.txt` skips it entirely when
`GRAPHICS_BACKEND=Metal`. One deliberate simplification: the original's
diffuse texture is `BC1_UNORM` (block-compressed); this port decodes it to
plain `R8G8B8A8_UNORM` on the CPU at load time rather than exercising
`crgpu`'s (real, but never-yet-uploaded-to) BC1 format support, to avoid
stacking an unverified compressed-texture-upload path on top of bindless
textures, a real sampler, and a real depth buffer all being exercised for
the first time in one sample.

`CrGPUMeshShaders` closed the biggest remaining gap this suite tracked:
`crgpu` had no mesh-shader pipeline at all (mesh shaders replace
vertex-fetch/input-assembly entirely with a shader stage that emits meshlet
geometry directly) — now added on **all three** backends (`VK_EXT_mesh_shader`
on Vulkan, a hand-rolled `D3D12_PIPELINE_STATE_STREAM_DESC` on D3D12 since the
vendored `d3dx12.h` predates Microsoft's own mesh-PSO helpers, and
`MTLMeshRenderPipelineDescriptor` on Metal), gated behind the new
`CRGPU_EXT_MESH_SHADING` — see `carrottus/CLAUDE.md` for why this `EXT` is a
different flavor from bindless's (the macro is `1` everywhere; it's *runtime*
driver/hardware support that varies, surfaced via
`crgpu_get_mesh_shading_capabilities`). The sample renders the real
`Dragon_LOD0.bin` meshlet asset shipped with the original D3D12 sample,
one Slang mesh shader built for all three backends exactly like every other
sample's shaders (see Building below). This sample's four
meshlet data buffers need `CRGPU_BUFFER_USAGE_STORAGE`, which
`CRGPU_MEMORY_KIND_UPLOAD` refuses on every backend — the rule comes from
D3D12, where an UPLOAD-heap resource can't carry
`D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`, and Carrottus GPU holds every
backend to it (the mappable kind that does take `STORAGE`,
`CRGPU_MEMORY_KIND_DEVICE_MAPPABLE`, exists only on some devices).
Getting this sample running on D3D12 closed a real gap in `crgpu` itself:
**`crgpu_command_list_copy_buffer` (buffer-to-buffer copy) didn't exist on any
backend**, so the textbook fix — stage the CPU data through a small
`TRANSFER_SRC` buffer, then copy into a real `CRGPU_MEMORY_KIND_DEVICE` buffer — wasn't
available. Now added (`vkCmdCopyBuffer` / `CopyBufferRegion`; Metal's
`MTLBlitCommandEncoder` copy was initially left as an unimplemented
placeholder like this file's other blit functions, unverified without Apple
hardware — see `HelloTexture` below for where that caught up with this
sample), each of the four buffers gets a small synchronous staging upload
plus a `COMMON` →
`UNORDERED_ACCESS` pipeline barrier (needed only on D3D12 in practice — that
transition isn't one of the states D3D12 promotes to implicitly the way it
does `COPY_DEST`/read-only states, but costs nothing on Vulkan given the
`crgpu_command_queue_wait_idle` already between the copy and first use).

`CrGPUBundles` closed the last real gap `CRGPU_COMMAND_LIST_TYPE_BUNDLE`
had been sitting on since it was added to the base command-list-type enum:
the type existed and D3D12's allocator/list creation already honored it, but
nothing could actually *execute* a bundle once recorded. Now added, gated
behind the new `CRGPU_EXT_BUNDLES` (`crgpu_command_list_begin_bundle`,
`crgpu_command_list_execute_bundle`) — `1` on Vulkan/D3D12, `0` on Metal, the same
flavor of `EXT` as bindless (a structural capability gap, not a runtime
driver check): Metal's `MTLCommandBuffer`/`MTLRenderCommandEncoder` are
single-use, so there's no CPU-side reusable secondary-command mechanism to
record a bundle into at all. D3D12's implementation is the straightforward
native case (`ID3D12GraphicsCommandList::ExecuteBundle`); Vulkan's bundle is
a `VK_COMMAND_BUFFER_LEVEL_SECONDARY` buffer replayed via
`vkCmdExecuteCommands`, which — since this codebase is dynamic-rendering-only
(no `VkRenderPass`) — needed `VkCommandBufferInheritanceRenderingInfo` wired
through a new `crgpu_command_list_begin_bundle`/`crgpu_bundle_inheritance_
desc` (the attachment formats a secondary buffer will replay under, since
there's no framebuffer object to infer them from) plus a new
`will_execute_bundles` flag on `crgpu_rendering_begin_desc` so the primary
list's `vkCmdBeginRendering` opens with
`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT` only when the caller
actually intends to execute one — Carrottus never infers that on the caller's
behalf (see `carrottus/CLAUDE.md`'s zero-policy rule). The sample itself
demonstrates the mechanism the original does: a `CityRowCount x
CityColumnCount` (10x3) grid of `occcity` buildings (the same asset
`CrGPUDynamicIndexing` uses), each with its own per-object CBV, drawn
either by replaying one bundle recorded once at startup (default) or by
re-recording the identical bind+draw sequence into the direct command list
every frame — press `C` to toggle, matching the original's own keybinding.
Two deliberate simplifications versus the original: no diffuse texture (this
sample isn't about texturing — each building instead gets a fixed
HSL-gradient color baked into its CBV, so it only exercises the bundle
mechanism itself), and single-buffered CBV storage overwritten in place every
frame, matching every other sample in this suite, rather than the original's
3 rotating `FrameResource`s — the bundle's recorded descriptor-set bindings
point at fixed buffer offsets regardless, so this doesn't touch the actual
mechanism being demonstrated.

`CrGPUDepthBoundsTest` closed a gap that had sat unaddressed since the very
first sample: `crgpu` had no depth bounds test at all — a mechanism, distinct
from the ordinary per-fragment depth test, that discards a fragment based on
whether the depth attachment's *existing* value at that pixel (already
written by an earlier draw) falls inside a caller-set `[min, max]` window, set
per-draw via the new `crgpu_command_list_set_depth_bounds`. Added on Vulkan
(`VkPhysicalDeviceFeatures::depthBounds` + `vkCmdSetDepthBounds`, both core
1.0, no extension), D3D12 (`D3D12_DEPTH_STENCIL_DESC1::
DepthBoundsTestEnable` + `ID3D12GraphicsCommandList1::OMSetDepthBounds`), and
Metal (`[MTLRenderCommandEncoder setDepthTestMinBound:maxBound:]`, macOS/iOS
26+), gated behind the new `CRGPU_EXT_DEPTH_BOUNDS_TEST` — `1` on **every**
backend, since unlike bindless/bundles this isn't a structural gap on Metal
(the primitive genuinely exists there, just very recently added). What does
vary per-device is runtime hardware support, the `CRGPU_EXT_MESH_SHADING`
flavor of this pattern rather than the bindless/bundles flavor:
`crgpu_depth_bounds_capabilities::supported` reflects
`VkPhysicalDeviceFeatures::depthBounds` (Vulkan),
`D3D12_FEATURE_DATA_D3D12_OPTIONS2::DepthBoundsTestSupported` (D3D12), or
`[MTLDevice supportsFamily:MTLGPUFamilyApple10]` (Metal — the newest Apple
GPU family as of this writing, so expect `false` on most Metal hardware in
the field today even though the sample still builds and runs there). Metal
also has no pipeline-level enable toggle at all, unlike Vulkan/D3D12 —
`crgpu_graphics_pipeline_desc::depth_bounds_test_enable` is ignored there,
and `crgpu_command_list_set_depth_bounds` alone both enables and configures the test
per draw (passing `(0, 1)` disables it, matching Metal's own default-disabled
semantics). Adding `DepthBoundsTestEnable` to D3D12's pipeline creation also
closed a smaller, adjacent gap: that field only exists on
`D3D12_DEPTH_STENCIL_DESC1`, reachable only through `CreatePipelineState`'s
PSO-stream path — `crgpu_graphics_pipeline_create`'s classic (non-mesh)
pipeline path was still going through the older `CreateGraphicsPipelineState`/
`D3D12_GRAPHICS_PIPELINE_STATE_DESC`, so it now builds a
`CD3DX12_PIPELINE_STATE_STREAM1` from that same desc instead (a lossless,
purely additive upgrade — see `crgpud3d12shader.cpp`) rather than
maintaining two divergent pipeline-creation paths.

The port itself deviates from the original in one deliberate, documented way:
**no colorless depth-only priming pass.** The D3D12 original's first draw
uses a PSO with no pixel shader at all (`DEPTH_ONLY_PSO_STREAM` has no `PS`
subobject) purely to write real per-vertex depth into the depth buffer without
touching the render target's color. `crgpu_graphics_pipeline_create` requires
a fragment module on every pipeline (`CR_RESULT_GPU_VERTEX_AND_FRAGMENT_
SHADER_REQUIRED` otherwise) — lifting that would mean threading an optional
fragment stage (and, to fully match the original's *visual* result, a
color-write-mask override neither backend's `crgpu_blend_attachment_desc`
exposes yet) through both backends for a single sample's sake. Instead, this
port's priming pass renders the triangle's real colors and real depth (an
ordinary opaque draw), and the depth-bounds-tested second pass redraws the
same triangle in a fixed highlight color instead of the original's vertex
colors — wherever the bounds window clips a fragment, the first pass's real
color already shows through underneath, so the mechanism being demonstrated
(an animated depth-bounds window carving a band out of the triangle) reads
just as clearly, just with a highlight-over-base look rather than
background-showing-through.

`CrGPUHeaps` is `D3D12SmallResources` under the name of the Carrottus GPU
feature it exercises: heaps and the resources placed in them
(`crgpu_heap_create`, `crgpu_get_*_placement_requirements`,
`crgpu_buffer_create_placed`/`_image`). The grid of 77 small textures is
made either placed, one after another in a single heap, or committed, one
device allocation each; Space switches between them, as in the original.
The port goes further than the original in one place and falls short of it
in two:

- **Buffers are placed too.** The vertex buffer goes in a heap of device
  memory, and the staging buffer that fills it and the textures in a heap of
  upload memory, where `CRGPU_DEVICE_PROPERTIES::mappable_heaps` allows it
  (a Mac without Apple silicon can't map a heap; the staging buffer is then
  made with `crgpu_buffer_create`).
- **No memory figure for committed resources.** The original's title shows
  `QueryVideoMemoryInfo`'s figure for each mode. Carrottus GPU has no memory
  budget query yet, so the title shows the placed mode's heap size and
  per-texture requirements, and says nothing measured for the committed
  mode.
- **No small-resource packing on D3D12 yet.** The original exists to show
  `D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT` (4 KiB) letting small textures
  share 64 KiB pages. Carrottus GPU's D3D12 backend places every texture at
  64 KiB for now (see `carrottus/CLAUDE.md`), so on D3D12 the two modes cost
  the same. On Vulkan and Metal the textures pack at whatever alignment the
  driver reports.

The sample was written against Carrottus GPU's heap calls the day they landed. It
type-checks against the Vulkan and Metal configurations of `crgpu.h`
(its shaders were then GLSL, compiled with glslang). It was first built and run on 2026-10-22,
on D3D12 and Vulkan on Windows (see the end of this file); it has not run on
Metal.

`CrGPURayTracing` is the three tutorials of Microsoft's
`D3D12Raytracing` (`Samples/Desktop/D3D12Raytracing/src/` in
`DirectX-Graphics-Samples`, MIT License), written again in Carrottus GPU's C and
in Slang, as one sample with three modes chosen with `--mode`:

- `hello` (`D3D12RaytracingHelloWorld`): the triangle traced in screen
  space with orthographic rays along +z, inside the original's stencil
  window (a 0.1 border), its hits coloured by their barycentrics and its
  misses black; outside the window each pixel is its place in the target,
  as the original's ray generation shader writes it.
- `lighting` (`D3D12RaytracingSimpleLighting`): the original's cube (its 24
  vertices with face normals and 36 16-bit indices), its camera going round
  in 24 seconds and its point light in 8 the other way, diffuse plus
  ambient from its constants, the miss its dark blue.
- `procedural` (`D3D12RaytracingProceduralGeometry`): the ground plane, a
  triangle bottom level scaled out by its instance, with its filtered
  checkers; and every procedural primitive of the original, in an AABB
  bottom level of ten geometries: the analytic box and spheres, the
  volumetric metaballs, and the seven signed distance primitives (mini
  spheres, an intersected round cube, a square torus, a twisted torus, a
  cog, cylinders and the fractal pyramid), with the original's materials,
  its animation (`G`; the camera's `C` and the light's `L` off at start,
  as there), shadow rays, and reflections to its recursion depth of 3. The
  inline form carries all of them: an intersection shader is code, and a
  procedural candidate runs the same code. The signed distance functions
  are Inigo Quilez's (MIT), as the original credits them.

It asks for `CRGPU_FEATURE_RAY_QUERY` and, on an adapter without it,
prints that the device has no ray queries and exits 1, as
`CrGPUMeshShaders` does without mesh shading. `CRGPU_EXT_RAY_QUERY` is
1 on every backend, so it builds everywhere. The port deviates from the
originals in these ways:

- **Inline ray queries, not ray tracing pipelines.** The originals are DXR
  1.0: state objects, shader tables, and ray generation, miss,
  closest-hit and intersection shaders. Carrottus GPU has no ray tracing
  pipeline EXT (card S12), so each mode traces with inline ray queries
  (DXR 1.1's form; `SPV_KHR_ray_query`, `RayQuery`, Metal's
  `intersection_query`) in one compute shader,
  `shaders/rt_<mode>_comp.slang`, and the original's stages are functions
  of it: the ray generation shader is its body; the closest-hit and miss
  shaders are the two ends of a query's committed status; the
  intersection shaders (`shaders/rt_primitives.slang`) are what each
  procedural candidate runs before it is committed, with the query's t
  min, committed t and flags standing in for `RayTMin()`, `RayTCurrent()`
  and `RayFlags()`. Which primitive and material a candidate is comes from
  its geometry index, where the original reads its hit group's local root
  arguments from the shader table. `TraceRay`'s recursion in the
  procedural mode is a loop over the bounces: a hit's colour in the
  original is linear in its reflected ray's, so each bounce adds its own
  term, weighted by what the bounces before it pass on, which gives the
  original's result. The pictures match; the shader structure doesn't.
- **The output is drawn onto the back buffer, not copied.** The originals
  copy their `R8G8B8A8_UNORM` output texture to the back buffer
  (`CopyResource`). Here the trace writes a storage image of that format,
  and a pass of one triangle over the target (`shaders/rt_present_*.slang`)
  loads it, texel for pixel, into the swap chain's `B8G8R8A8_UNORM` back
  buffer, which is neither a copy destination nor a storage image unless
  asked for.
- **What changes each frame is pushed.** The originals keep a constant
  buffer per frame in flight, and the procedural one a buffer of each
  primitive's transforms, rewritten every frame. Here the scene's fixed
  constants are a uniform buffer written once, and each frame pushes only
  what moves (the projection-to-world rows, the camera and the light, and
  in the procedural mode the animation time); the procedural shader makes
  each primitive's transform (its scale, a turn about y, a move to its
  AABB's centre: the original's) from per-primitive data.
- **Instances written both ways (card S14).** Instances are in the
  device's layout (`crgpu_device_get_acceleration_structure_instance_layout()`).
  The hello and lighting modes write theirs on the CPU with
  `crgpu_device_write_acceleration_structure_instances()` into an upload buffer
  the top level reads. The procedural mode writes the plane's that way and
  copies it into a buffer of the device's memory, where the AABBs' is
  written by the sample's own compute shader for the layout's kind
  (`shaders/rt_instances_row64_comp.slang` for Vulkan and D3D12,
  `rt_instances_col72_comp.slang` for Metal's 72-byte layout), so that
  both writers run before the top level is built.
- Smaller ones: the shadow ray's `RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` has no
  meaning for a query and is dropped (the first hit ends it); the title
  shows no frame rate or rays a second; there is no Alt+Enter full screen,
  and the window can't be resized, as no sample's can.

The acceleration structures are built once, in one setup submission: the
geometry staged into a buffer of the device's memory, the bottom levels in
one build call, the top level after a barrier, one scratch buffer with a
range for each build at the device's scratch alignment, and the barriers
`crgpu.h` states (scratch to `ACCELERATION_STRUCTURE_SCRATCH`, inputs to
`ACCELERATION_STRUCTURE_BUILD_INPUT`, `BUILD_WRITE` to `BUILD_READ`
between the levels, `BUILD_WRITE` to `TRACE` before the first frame).
What only that submission used (the staging buffers, the instances, the
scratch, the GPU writer) is destroyed once it completes. A frame then
moves the output to `GENERAL`, traces, moves it back to
`SHADER_READ_ONLY` and draws it: no object is created in the loop. It was
first built and run on 2026-10-31, on D3D12 and Vulkan on Windows (see the
end of this file), and on 2026-11-01 on Metal (Apple M5 Pro, macOS 27.0.1,
Debug, Metal API and shader validation with warnings fatal): each mode 300
frames, exit 0, silent, the instances written by the 72-byte Metal writer
(`rt_instances_col72_comp`) and drawn the right way up, hello and lighting
as on D3D12 and procedural with every primitive. Metal's compiler refused
two things SPIRV-Cross wrote from Slang's SPIR-V under `-Werror`, a vector
filled a component at a time and an unused candidate transform, so
`rt_common.slang` builds the index and unprojected vectors whole and
`candidate_hit` returns the bottom level's normal, transformed only where
it is used (the same arithmetic; D3D12 and Vulkan not rerun since).
Every other sample that builds on Metal (HelloTriangle, HelloConstBuffers,
HelloTexture, CrGPUExecuteIndirect, CrGPUMeshShaders,
CrGPUDepthBoundsTest, CrGPUHeaps) ran 300 frames the same day, exit 0,
silent.

Later on 2026-11-01 the shaders moved to carrottus's one shader step
(`cr_add_shader()`, card X18), whose Metal source pads each set to every
binding the shader declares, and Carrottus GPU's Metal backend began refusing a
module whose sets aren't its layouts' (card S27). Every sample that builds
on Metal ran again on the Mac (Apple M5 Pro, Debug, Metal API and shader
validation with warnings fatal): 300 frames each, CrGPURayTracing in all
three modes, exit 0, no message; the three modes screenshotted, drawn as
before. Windows and Linux not built with the new step.

See `../carrottus/CLAUDE.md` and `../CLAUDE.md` for the architecture this
repo sits alongside.

## Building

Each sample is a standalone executable target linking `carrottus`. Windowing
and surface creation use SDL3 + `crgpu_sdl3.h` (the same approach as
`Erethal_SDL3_ImGui`, which is deprecated), so samples build and run identically on Windows and
Linux. Pick a graphics backend via the `GRAPHICS_BACKEND` cache variable
(`Vulkan` or `D3D12`), same as `carrottus` itself.

```
cmake --preset windows-vulkan   # or windows-d3d12, linux-vulkan, macos-metal
cmake --build --preset windows-vulkan-debug --target HelloTriangle
```

Every preset is multi-config: `-debug`, `-release` and `-relwithdebinfo` build
presets pick the configuration, and each sample lands in
`build-<preset>/Samples/<Sample>/<Config>/`. Built from the workspace root
(`../CMakeLists.txt`, Windows and Linux), every sample lands in `<Config>/`
under the root's build directory instead.

### Shaders

Carrottus GPU compiles no shader: a module takes code in the one format its
adapter reports (`CRGPU_ADAPTER_PROPERTIES::shader_formats`): SPIR-V on
Vulkan, DXIL on D3D12, a `.metallib` on Metal. Each sample's shaders are
Slang (`Samples/<Sample>/shaders/<stem>.slang`, one entry point each, named
for the file's stem, never `main`), built at build time by carrottus's shader
step (`cr_add_shader()`, `../carrottus/cmake/cr_shaders.cmake`), the one
step every repository of the workspace uses; `cmake/crgpusamples_shader.cmake`
calls it and adds only the samples' `<stem>.shader.h`:

- SPIR-V 1.5 on every platform (`slangc -fvk-use-entrypoint-name
  -matrix-layout-column-major -warnings-as-errors all`, plus the mesh
  shader's `spvMeshShadingEXT` and the ray query shaders'
  `spvRayQueryKHR`), checked by `spirv-val`, with Slang's reflection.
- DXIL on Windows: Slang writes HLSL and DXC compiles it
  (`dxc -T <profile> -E <entry> -WX`; `vs_6_0`, `ps_6_0`, `cs_6_0`,
  `ms_6_5` for the mesh shader and `cs_6_5` for the ray query shaders),
  its validator signing what it passes.
- Shared code is a Slang module a shader imports from beside it
  (`CrGPURayTracing`'s `rt_common.slang` and `rt_primitives.slang`),
  named in the call's `DEPENDS`, so that a change to one builds its
  importers again.
- Metal source by both routes where a `.metallib` is made (and on Windows
  and Linux under `CR_SHADERS_CHECK_METAL=ON`): Slang's own, and SPIR-V
  through carrottus's `crgpu_msl` tool, built for the build machine, each
  set padded to every binding the shader declares. A shader that neither
  route takes is marked `NO_METAL` with its reason
  (`CrGPUDynamicIndexing`'s bindless fragment shader, a sample that never
  builds on Metal).
- On Apple platforms, a `.metallib` from the route the shader names
  (`crgpu_msl`'s unless `METAL_ROUTE SLANG`, card S21; Metal 3.1).

Each binary is embedded in the executable as a C array (carrottus's
`cmake/cr_embed.cmake`); a generated `<stem>.shader.h` declares
`<stem>_shader` with the entry point and the code in each format the build
made, and `Samples/crgpusamples_shader.h` makes a module from it in the
adapter's format. Nothing is read from disk at run time but the samples'
`.bin` assets. `slangc`, `spirv-val` and `dxc` come from the Vulkan SDK
(`VULKAN_SDK`); their versions are printed at configure time.

Carrottus GPU's clip space is +Y up on every backend, as Direct3D's is, so the
samples use the originals' clip-space vertices and projections unflipped.

On Apple Silicon, samples also build and run against `GRAPHICS_BACKEND=Metal`
(see `carrottus/CLAUDE.md`) — except `CrGPUDynamicIndexing`, which needs
bindless descriptor indexing, and `CrGPUBundles`, which needs
`CRGPU_EXT_BUNDLES`; neither is buildable on Metal (see above).
`CrGPUDepthBoundsTest` *does* build and run on Metal — its
`CRGPU_EXT_DEPTH_BOUNDS_TEST` gap isn't structural there — but
`crgpu_depth_bounds_capabilities::supported` will be `false` on anything
short of `MTLGPUFamilyApple10` hardware, so the animated bounds window has
no visible effect on most Metal devices today even though the sample runs.

## Running

Every sample takes two optional arguments:

- `--frames N`: render N frames, then leave the render loop as closing the
  window does, through the same cleanup. Without it a sample runs until its
  window is closed. Use it to run a sample unattended, under a validation
  layer or a debugger.
- `--device N`: use the Nth adapter `crgpu_adapter_enumerate` lists (0 if
  not given).

`CrGPURayTracing` also takes `--mode hello|lighting|procedural`, which of
the three originals it is (`hello` if not given). Without `--device` it
takes the first adapter whose `supported_features` has
`CRGPU_FEATURE_RAY_QUERY` and `CRGPU_FEATURE_PRESENT`, the features its
device is created with: chosen by what the adapter can do, never by its
name or vendor, so the choice is the same on every backend.

Every sample's swap chain names `CRGPU_PRESENT_SCALING_ONE_TO_ONE`: its
window can't be resized and its back buffers are the window's size, so
nothing is ever scaled, and one-to-one is the scaling every surface takes.
HelloTriangle also takes two arguments for looking at the scalings:

- `--scaling one-to-one|stretch|aspect-fit`: the swap chain's present
  scaling (one-to-one if not given). It prints which ones the surface takes
  (`crgpu_surface_get_present_scalings`) and stops if that isn't one,
  then prints the back buffers' real size (`crgpu_swap_chain_get_desc`)
  and draws at that size: on Vulkan without a scaling the surface gives
  its own size.
- `--window WxH`: a window of W by H while the back buffers stay 1280x720,
  so the compositor shows them at another size. On macOS the window can't
  be resized from outside instead: SDL's Metal view sets the layer's
  `drawableSize` from the window at every resize (`SDL_cocoametalview.m`,
  SDL 3.4.16), so a back buffer never lags a resized window there.

## Validation run, 2026-10-16

On Windows 11, Debug, with the `windows-vulkan` preset and
`VK_LAYER_KHRONOS_validation`, `--frames 300`, on both devices of one
laptop (Intel Iris Xe, `--device 0`; NVIDIA RTX A2000, `--device 1`), every
sample exits cleanly with no validation message. `CrGPUMeshShaders` exits 1
on the Intel device, by design: that driver reports no mesh shading.

Before that run the samples no longer compiled: Carrottus GPU had replaced
`color_attachment_format` with `color_attachment_formats[]` and
`color_attachment_count`. Once built, the run found:

- `CrGPUBundles`: Carrottus GPU's header said a bundle inherits the direct
  list's viewport and scissor, which a Vulkan secondary command buffer does
  not; the Intel driver crashed on the bundle's first draw. A bundle is now
  given its viewport and scissor in `crgpu_bundle_inheritance_desc`, and
  `crgpu.h` states the bundle rules for every backend.
- `CrGPUDynamicIndexing`: the depth image's transition was recorded into
  the upload list after it had been closed and submitted, so the depth image
  was never transitioned, and the NVIDIA driver crashed recording into the
  finished list. The transition is now recorded each frame.
- `CrGPUExecuteIndirect`: Carrottus GPU's Vulkan device never enabled
  `multiDrawIndirect`, so a 256-draw indirect call was invalid. It is enabled
  where reported, and `CRGPU_DEVICE_PROPERTIES::max_draw_indirect_count`
  says how many draws one call takes; the sample splits its call by it.
- `CrGPUMeshShaders`: Carrottus GPU's Vulkan `crgpu_command_list_push_graphics_constants` knew
  only vertex, fragment and compute stages and dropped a mesh-stage push.
  With that fixed, the sample read freed memory every frame (its meshlet
  subsets pointed into the file buffer it had freed); they are now copied.

The same day, with the `windows-d3d12` preset (Debug) under cdb, every
sample runs 300 frames with no D3D12 debug layer message on both devices:
the Iris Xe (`--device 0`) and the NVIDIA RTX A2000 (`--device 1`).
`CrGPUMeshShaders` exits 1 on the Iris Xe, which has no mesh shading. The
Iris Xe was listed on D3D12 only after Carrottus GPU's D3D12 backend stopped
requiring feature level 12_2 (it now requires 12_0); before that, D3D12
listed the NVIDIA device alone, as device 0. Two samples failed there
first, both from Carrottus GPU's D3D12 backend:

- `CrGPUMeshShaders`: `CreateGraphicsPipelineState` failed, the root
  signature not covering the shader's `t3`. SPIRV-Cross declared the GLSL
  `readonly` storage buffers as SRVs, while the backend's root signature and
  descriptors make every storage buffer a UAV; it now compiles them as UAVs.
- `CrGPUDynamicIndexing`, and `CrGPUMeshShaders` once its pipeline was
  made: two errors on every draw, a bound table in a descriptor heap that
  was no longer set. Each descriptor pool, and the bindless state, had its
  own shader-visible heaps, and D3D12 sets one CBV/SRV/UAV heap and one
  sampler heap at a time, so binding a set from a second pool unbound the
  first's. They now all take ranges of the device's two heaps.

Later on 2026-10-16 every sample ran again, the same way, against Carrottus
with the Mac's Metal work, the D3D12 fix for 2D views of a layer past the
first and for cube views to render to or store into, storage cube images
in D3D12 shaders, and Vulkan's `imageCubeArray`. On Vulkan every sample
exited cleanly with no validation message on both devices (`CrGPUMeshShaders`
exits 1 on the Iris Xe, as before). On D3D12 there was no debug layer
message on either device but one: `CrGPUMeshShaders` on the Iris Xe
returned from `main` without destroying the instance when it found no mesh
shading, and the debug layer reported the device's live objects at process
exit. Every sample now leaves `main` through one cleanup on every failure:
each object `main` makes starts as NULL, a failure jumps to the cleanup at
the end, which waits for the queue if there is one and destroys whatever
exists, and the helpers that called `exit(1)` (reading a file, loading a
shader, uploading a texture, `CrGPUHeaps`' `check`) return NULL or false
for the caller to jump. Only a failed `SDL_Init` still returns directly. Run
again on both devices and both backends, every sample is clean as above, and
each run with `--device 9` (a failure right after the instance is made)
reports no live objects on D3D12. Under cdb, each run on the RTX A2000 also logs two first-chance
C++ exceptions inside Intel's D3D12 driver as it loads, caught there, with
no debug layer message.

On 2026-10-22 every sample was built (Debug) and run again on D3D12 and
Vulkan on Windows 11, on the Intel Iris Xe and the NVIDIA RTX A2000, and on
D3D12's WARP adapter, after Carrottus GPU's adapter and device change. That was the
first build and run of `CrGPUHeaps` on any backend. Neither backend built at
first: Carrottus Audio's WASAPI backend named `max_align_t`, which MSVC's C compiler
doesn't have (`CR_MAX_ALIGN`, `carrottus/src/crallocator.h`, now). The plain
runs, 300 frames each, were clean on both backends: no validation layer or
debug layer message and exit 0, but for `CrGPUMeshShaders` on the Iris Xe,
which has no mesh shading and exits 1 as before. Three stricter runs found two
Vulkan bugs, both fixed in Carrottus:

- Synchronization validation (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`)
  reported a WRITE_AFTER_READ against `vkAcquireNextImageKHR` in every
  sample: the barrier that takes a back buffer out of `UNDEFINED` or
  `PRESENT_SRC` started at `TOP_OF_PIPE` (or `BOTTOM_OF_PIPE` out of
  `CRGPU_RESOURCE_STATE_PRESENT`), which doesn't chain to the acquire
  semaphore's wait at `COLOR_ATTACHMENT_OUTPUT`. Such a barrier now starts at
  its destination stage.
- GPU-assisted validation (`VK_LAYER_GPUAV_ENABLE=1`) reported
  `VUID-VkDrawIndirectCommand-firstInstance-00501` in
  `CrGPUExecuteIndirect`: its draws' `first_instance` is the triangle's
  index, and the Vulkan device never enabled `drawIndirectFirstInstance`.
  That is `CRGPU_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE` now, which the
  sample asks for.

D3D12 with GPU-based validation on (a temporary
`SetEnableGPUBasedValidation(TRUE)`, both devices) reported nothing. After
the fixes every sample is clean on both backends and both devices in every
mode above, and a screenshot of each sample on the RTX A2000 showed the same
image on D3D12 and on Vulkan.

## Present scaling run, 2026-10-23

After Carrottus GPU's present scaling (every sample names one), on Windows 11,
Debug, both presets, both devices (Intel Iris Xe `--device 0`, NVIDIA RTX
A2000 `--device 1`): the first build of the change on D3D12 and the first
run on either backend. Every sample ran 300 frames with the D3D12 debug
layer, the Vulkan validation layer, and the validation layer with
`VK_LAYER_VALIDATE_SYNC=1`: no layer message, exit 0, but for
`CrGPUMeshShaders` on the Iris Xe (no mesh shading, exit 1 as before).

HelloTriangle, `--window 900x900 --frames 600`, each `--scaling`:

| Backend, device | Surface takes | Back buffers | one-to-one | stretch | aspect-fit |
|---|---|---|---|---|---|
| D3D12, Iris Xe | one-to-one stretch | 1280x720 | top left, cropped right | fills the window | refused |
| D3D12, RTX A2000 | one-to-one stretch | 1280x720 | top left, cropped right | fills the window | refused |
| Vulkan, Iris Xe | one-to-one | 900x900 | the window's size, nothing scaled | refused | refused |
| Vulkan, RTX A2000 | one-to-one stretch | 1280x720 | top left, cropped right | fills the window | refused |

A screenshot of each was taken while it ran. On the RTX A2000 the Vulkan
captures of one-to-one and stretch are byte for byte the D3D12 ones. Two
Carrottus GPU Vulkan bugs were found and fixed first (`carrottus/CLAUDE.md`,
"Present scaling"): HelloTriangle crashed on the RTX A2000 in
`crgpu_surface_get_present_scalings`, and its surface offered aspect-fit
and showed a stretch, so a Win32 surface no longer offers it. HelloTriangle
itself drew a 1280x720 render area whatever its back buffers were, larger
than the Iris Xe's 900x900; it now reads their size and prints it (the old
draw was never run on 900x900 back buffers).

## Linux run, 2026-10-25

Every sample built (Debug, `linux-vulkan`) and ran on Fedora 44 under KDE
Plasma on Wayland, on an Intel Iris Xe (Mesa ANV 26.2.4), with
`VK_LAYER_KHRONOS_validation` reporting errors, warnings and performance
warnings: no message from any of them, and each closed cleanly. SDL3 opens
the windows as native Wayland windows, and on Vulkan SDL makes the surface
itself, so `CRGPU_LINUX_WSI` doesn't apply to the samples. None of the
windows is resizable (each is made with no flags, as the originals' are
fixed-size), so this run tested no swap chain resize; the compositor kept
each at its size. `CrGPUMeshShaders` refused to start: the Iris Xe has no
mesh shading.

The screenshots showed six samples upside down. Carrottus GPU's clip space is
Vulkan's on every backend (+Y down; D3D12 and Metal flip Y when they
translate a shader), and those ports had kept the originals' +Y-up data:
`HelloTexture`, `CrGPUDepthBoundsTest` and `CrGPUHeaps` now negate Y
in their clip-space vertices, and `CrGPUExecuteIndirect`,
`CrGPUBundles` and `CrGPUDynamicIndexing` negate the projection's Y
scale. Run again, all six draw the right way up, still with no validation
message. `CrGPUMeshShaders` has the same projection and also culls with
D3D12's clockwise front faces, which the flip reverses; it is left as it
was until it can be run on a device with mesh shading.

## Ray tracing run, 2026-10-31

`CrGPURayTracing`'s first build and run, on Windows 11 (ThinkPad P1 Gen
5), Debug, from the workspace root's `build-windows-d3d12` and
`build-windows-vulkan` (the sample target alone: the root build stops in
libdemo, which is deprecated), with slangc 2026.17 and DXC 1.9.0.5512 from
the Vulkan SDK 1.4.363.0; the RTX A2000's driver is 597.30. It built with
no warning. Every run below is `--frames 300`, from the executable's own
directory, in each of the three modes:

| Backend, adapter | Validation | Result |
|---|---|---|
| D3D12, RTX A2000 (`--device 1`) | debug layer; GPU-based validation (`d3dconfig`, scoped to the sample, set back after) | exit 0, no message |
| D3D12, Iris Xe (`--device 0`) | debug layer | refused: "doesn't support ray queries (no CRGPU_FEATURE_RAY_QUERY)", exit 1, no message |
| D3D12, WARP (`--device 2`) | debug layer; GPU-based validation (hello and lighting) | WARP reports ray queries (raytracing tier 1.1) and draws all three modes as the A2000 does: exit 0, no message. The procedural mode takes about 0.4 s a frame there (60 frames in 26 s; 300 in 123 s) |
| Vulkan, RTX A2000 (`--device 1`) | validation layer (errors, warnings, performance warnings); synchronization validation (`VK_LAYER_VALIDATE_SYNC=1`); GPU-assisted validation (`VK_LAYER_GPUAV_ENABLE=1`) | exit 0, no message but GPU-AV's notices of its own settings |
| Vulkan, Iris Xe (`--device 0`) | the same three | refused as on D3D12, exit 1, no message |

The first D3D12 run found one bug of the sample's own, fixed: it unmapped
its scene constants a second time at exit (debug layer error 310).

Each mode was screenshotted on the RTX A2000 on both backends and compared
by eye with the original's `Screenshot.png`:

- hello: the same picture as the original's, pixel geometry and colours
  alike: the stencil window's border (black at the top left, red at the
  top right, green at the bottom left, yellow at the bottom right), the
  black window, and the triangle with its apex red at the top, green at
  the bottom left and blue at the bottom right. D3D12 and Vulkan look the
  same.
- lighting: the cube at the original's size and place in the frame, the
  same dark blue background, faces lit away from the light at the 0.5
  grey of the ambient and lit faces shading towards red. The cube is
  turned another way and lit on another face than in the original's
  capture, and between the two backends' captures: the camera and the
  light go round all the time, and each capture is of another moment.
- procedural: the original's scene, every primitive where the original
  has it and of its colour and material: the red cylinders, the green
  round cube, the reflective square torus, the fractal pyramid, the
  spheres, the green mini spheres, the red box, the twisted torus, the cog
  and the metaballs; shadows, reflections in the plane and in the chrome,
  the checkers fading into the sky. What differs is the animation's
  phase (the metaballs' shape, the turn of the spheres, twisted torus and
  cog), as between the two backends' captures; the metaballs' edge shows
  the same thin light fringe in the original. WARP's capture is the same
  picture.
- Every capture differs from the originals' in the title bar only by
  what it says: the originals show the frame rate and rays a second.

Not run on Metal (written for it: the Metal sources of every shader are
build outputs here, and the 72-byte instance writer is chosen by the
layout's kind, but nothing was compiled for Apple); Linux not built.

## Linux run, 2026-11-04

Every sample built (Debug, `linux-vulkan`, no warning) and ran on the
ThinkPad P1 Gen 5 under Fedora 44, KDE Plasma 6.6.4 on Wayland, with the
Vulkan SDK 1.4.363.1, on all three of its adapters: the Intel Iris Xe
(Mesa ANV 26.0.3, `--device 0`), the NVIDIA RTX A2000 8GB Laptop (the
in-kernel `nouveau` driver with Mesa's NVK 26.0.3, `--device 1`) and
llvmpipe (Mesa 26.0.3 on LLVM 22.1.1, `--device 2`). Every run was
`--frames 300` from the executable's own directory, under the validation
layer with errors, warnings and performance warnings reported
(`VK_LAYER_REPORT_FLAGS=error,warn,perf`), then with synchronization
validation (`VK_LAYER_VALIDATE_SYNC=1`), then with GPU-assisted
validation too (`VK_LAYER_GPUAV_ENABLE=1`); the ray tracing sample in each
of its three modes. This is the first Linux run on an NVIDIA GPU.

| Adapter | Result, in all three modes |
|---|---|
| RTX A2000 (NVK) | HelloTriangle, HelloConstBuffers, HelloTexture, Bundles, DepthBoundsTest, DynamicIndexing, ExecuteIndirect and Heaps: 300 frames, exit 0, no message but GPU-AV's notices of its own settings, nothing in the kernel log. MeshShaders and RayTracing refused as they should, exit 1: NVK 26.0.3 offers neither mesh shading nor ray queries on this GPU (NVIDIA's driver does, on Windows) |
| Iris Xe | the same, the same refusals |
| llvmpipe | the first six and Heaps as above; ExecuteIndirect and the three ray tracing modes segfault and MeshShaders aborts, each inside LLVM while llvmpipe compiles a shader (`InstCombine`'s `simplifyMaskedScatter`; `Cannot select ... X86ISD::MGATHER`), validation silent: llvmpipe's, taken up in `../DECISIONS.md` card S36 (llvmpipe reports ray queries here) |

**No hang.** The user saw these samples freeze on the NVIDIA card on an
earlier Fedora install (not recorded). Nothing froze in any of the 108
runs, every one of them under a time limit with a thread dump and the
kernel log ready for a hang. Not run: a sample left running unlimited, and
a resize (the samples' windows are fixed-size). SDL3 made native Wayland
windows; `CRGPU_LINUX_WSI` (`Xlib` in the preset) doesn't apply, SDL
making the surface.

The same day under the sanitizers (`../prompts/fedora_linux_sanitizers.md`;
Debug, `build-linux-vulkan-asan` and `-tsan` on the `linux-vulkan` preset
with carrottus's `sanitize-asan`/`-tsan` flags, the validation layer on):
every sample `--frames 300` on the Iris Xe and the A2000 under each, the
ray tracing sample in its three modes. One report was ours:
CrGPUDynamicIndexing's BC1 decoder shifted a byte promoted to `int` left
by 24 (UBSan), widened first now, then clean on both. The rest are
third-party (`../DECISIONS.md` card S37): NVK leaks 64 bytes per device
(every sample on the A2000); ANV leaks an internal kernel it loads from
Mesa's disk cache in `vkCmdDrawIndirect` (ExecuteIndirect on the Iris Xe,
gone with `MESA_SHADER_CACHE_DISABLE=true`); under TSan the validation
layer's queue thread races its own `DestroyFence` at shutdown (every
windowed sample, after `crgpu_command_queue_wait_idle`; gone with the layer off),
and with the layer off libdbus, loaded by SDL3, reports a lock-order
inversion of its own at exit. MeshShaders and RayTracing refused as before.


Screenshots: HelloTriangle, HelloTexture and DynamicIndexing were seen
the right way up on the A2000, and Bundles on both GPUs. Bundles on the
A2000 drew the city with triangles missing, specks in the sky and, in one
capture, a flat green triangle over half the window; the Iris Xe's frame
had none of it. Validation was silent there too: the same NVK fault as
card S36's (`../DECISIONS.md`). **The upside-down comparison of every
sample wasn't finished:** capturing each sample's window needed it
focused, and KWin didn't focus most of them in time (the samples open
fixed-size windows behind the active one).
