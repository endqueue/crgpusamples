cmake_minimum_required(VERSION 3.15)
project(CrGPUSamples LANGUAGES C CXX)

# Pull in carrottus if nobody above us already has (mirrors rabbittus'
# CMakeLists.txt). When built through the eqdev root aggregator, carrottus is
# already a target by the time this runs and CMAKE_MSVC_RUNTIME_LIBRARY has
# already been force-set there -- see ../CLAUDE.md. When built standalone,
# set CMAKE_MSVC_RUNTIME_LIBRARY yourself before configuring to match
# whatever else you link against.
if(NOT TARGET carrottus)
    add_subdirectory(../carrottus ${CMAKE_CURRENT_BINARY_DIR}/carrottus)
endif()

# NOT ENABLED YET: a FetchContent path so this repo can be `git clone`d
# standalone by the public, without the ../carrottus sibling-checkout
# assumption above (which only holds inside the eqdev workspace via
# sync.sh). carrottus/crgpu is still an immature, actively-changing product
# (Metal backend only just went from scaffolded to real; bindless-on-Metal
# is a known, deliberate gap) with no tagged releases -
# pinning a public sample repo's build to it now would mean either floating
# on a raw commit SHA with no compatibility guarantee, or cutting a "v0.1.0"
# that overpromises stability the library doesn't have yet. Revisit once
# carrottus has an actual first tagged release to pin to.
#
# if(NOT TARGET carrottus)
#     if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../carrottus/CMakeLists.txt")
#         # Inside the eqdev workspace - build against the live sibling
#         # checkout so in-progress carrottus edits are picked up directly.
#         add_subdirectory(../carrottus ${CMAKE_CURRENT_BINARY_DIR}/carrottus)
#     else()
#         # Standalone public clone - fetch carrottus directly.
#         include(FetchContent)
#         FetchContent_Declare(
#             carrottus
#             GIT_REPOSITORY https://github.com/vigatel2717/carrottus.git
#             GIT_TAG <pinned commit or tag>
#         )
#         FetchContent_MakeAvailable(carrottus)
#     endif()
# endif()

# SDL3 supports Windows, Linux, and macOS, so samples build on all three,
# including GRAPHICS_BACKEND=Metal on Apple Silicon (see carrottus/CLAUDE.md).
if(WIN32)
    if(NOT SDL3_DIR)
        set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SDL3-3.4.8/cmake")
    endif()
endif()
find_package(SDL3 REQUIRED)

# The samples' shaders (crgpusamples_add_shader), over carrottus's shader
# step, which carrottus's build names. After carrottus.
include(cmake/crgpusamples_shader.cmake)

add_subdirectory(Samples/HelloTriangle)
add_subdirectory(Samples/HelloConstBuffers)
add_subdirectory(Samples/HelloTexture)
add_subdirectory(Samples/CrGPUExecuteIndirect)

# Bindless descriptor indexing (CRGPU_EXT_BINDLESS_DESCRIPTOR_INDEXING) is
# 0 on Metal -- it needs a MTLHeap-backed resource allocator CrGPU doesn't
# have yet (see carrottus/CLAUDE.md). Calling any bindless function against a
# Metal build is a compile/link error by design, so this sample is only
# buildable against Vulkan/D3D12.
if(NOT GRAPHICS_BACKEND STREQUAL "Metal")
    add_subdirectory(Samples/CrGPUDynamicIndexing)
endif()

# Mesh shading (CRGPU_EXT_MESH_SHADING) is implemented on all three
# backends, so this sample builds everywhere -- no guard needed.
add_subdirectory(Samples/CrGPUMeshShaders)

# Bundles (CRGPU_EXT_BUNDLES) are 0 on Metal -- MTLCommandBuffer/
# MTLRenderCommandEncoder are single-use, so there's no CPU-side reusable
# secondary-command mechanism to record a bundle into at all (see
# carrottus/CLAUDE.md). Calling crgpu_command_list_execute_bundle against a Metal
# build is a compile/link error by design, so this sample is only buildable
# against Vulkan/D3D12.
if(NOT GRAPHICS_BACKEND STREQUAL "Metal")
    add_subdirectory(Samples/CrGPUBundles)
endif()

# Depth bounds test (CRGPU_EXT_DEPTH_BOUNDS_TEST) is implemented on all
# three backends -- unlike bindless/bundles, Metal's
# [MTLRenderCommandEncoder setDepthTestMinBound:maxBound:] (macOS/iOS 26+) is
# a real primitive, not a structural gap, so this sample builds everywhere.
# Whether a given device actually honors it at runtime
# (crgpu_depth_bounds_capabilities::supported, gated to MTLGPUFamilyApple10
# on Metal) is this sample's own concern, not a build-time one -- see
# carrottus/include/crgpu.h.
add_subdirectory(Samples/CrGPUDepthBoundsTest)

# Heaps and placed resources have no CRGPU_EXT_* gate: all three backends
# implement them, so this sample builds everywhere. A device whose heaps
# can't be mapped (CRGPU_DEVICE_PROPERTIES::mappable_heaps false: a Mac
# without Apple silicon) still runs it, with its staging buffer made on its
# own instead of placed.
add_subdirectory(Samples/CrGPUHeaps)

# Ray queries (CRGPU_EXT_RAY_QUERY) are implemented on all three backends,
# so this sample builds everywhere -- no guard needed. Whether a device can
# trace is CRGPU_FEATURE_RAY_QUERY, the sample's own concern at run time
# (it stops on an adapter without it), not a build-time one -- see
# carrottus/include/crgpu.h.
add_subdirectory(Samples/CrGPURayTracing)
