#pragma once

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "osd-common.h"

#ifdef __cplusplus
extern "C" {
#endif

enum vulkan_osd_compositor_state {
	VULKAN_OSD_COMPOSITOR_STATE_NO_PAYLOAD,
	VULKAN_OSD_COMPOSITOR_STATE_PAYLOAD_MAPPED,
	VULKAN_OSD_COMPOSITOR_STATE_UNSUPPORTED_FORMAT,
	VULKAN_OSD_COMPOSITOR_STATE_UNSUPPORTED_SWAPCHAIN_USAGE,
	VULKAN_OSD_COMPOSITOR_STATE_READY,
	VULKAN_OSD_COMPOSITOR_STATE_COPY_FAILED,
};

struct vulkan_osd_compositor {
	struct osd_payload_map payload;
	uint32_t sequence;
	uint32_t width;
	uint32_t height;
	VkBuffer buffer;
	VkDeviceMemory buffer_mem;
	uint8_t *buffer_data;
	size_t buffer_size;
	uint32_t present_log_count;
	uint64_t last_present_log_time;
	enum vulkan_osd_compositor_state state;
	bool unsupported_logged;
};

struct vulkan_osd_compositor_context {
	VkDevice device;
	VkPhysicalDevice physical_device;
	const VkAllocationCallbacks *alloc;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkUnmapMemory UnmapMemory;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
};

struct vulkan_osd_compositor_draw {
	VkBuffer buffer;
	struct osd_rect rect;
};

void vulkan_osd_compositor_init(struct vulkan_osd_compositor *osd);
void vulkan_osd_compositor_free(struct vulkan_osd_compositor *osd,
				const struct vulkan_osd_compositor_context *ctx);
const char *vulkan_osd_compositor_state_name(enum vulkan_osd_compositor_state state);
bool vulkan_osd_compositor_prepare(struct vulkan_osd_compositor *osd,
				   const struct vulkan_osd_compositor_context *ctx, VkFormat swap_format,
				   const char *swap_format_name, bool transfer_dst_supported,
				   uint32_t surface_width, uint32_t surface_height,
				   struct vulkan_osd_compositor_draw *draw);
void vulkan_osd_compositor_record_copy(const struct vulkan_osd_compositor_context *ctx, VkCommandBuffer cmd_buffer,
				       VkImage image, bool capture_frame,
				       const struct vulkan_osd_compositor_draw *draw);

#ifdef __cplusplus
}
#endif
