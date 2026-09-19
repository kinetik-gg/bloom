// Vulkan Memory Allocator's single-header implementation, isolated in its own translation unit.
//
// This is the Bloom analogue of the vendored stb_truetype object target: third-party code with its
// own warning profile, never Bloom's strict warnings-as-errors. VMA is an implementation detail
// behind src/render's Bloom-owned resources and is never exposed through a public header.
//
// VMA_STATIC_VULKAN_FUNCTIONS is disabled and VMA_DYNAMIC_VULKAN_FUNCTIONS enabled so the
// implementation contains no static Vulkan entry-point references. The allocator is created with
// an explicit VmaVulkanFunctions carrying only vkGetInstanceProcAddr and vkGetDeviceProcAddr
// resolved privately from the runtime loader; VMA derives the rest from those.

#define VK_NO_PROTOTYPES 1
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include <vk_mem_alloc.h>
