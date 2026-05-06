#include "vulkan-osd-compositor.h"

#include "graphics-hook.h"

#include <string.h>

const char *result_to_str(VkResult result);

void vulkan_osd_compositor_init(struct vulkan_osd_compositor *osd)
{
	memset(osd, 0, sizeof(*osd));
	osd->state = VULKAN_OSD_COMPOSITOR_STATE_NO_PAYLOAD;
}

const char *vulkan_osd_compositor_state_name(enum vulkan_osd_compositor_state state)
{
	switch (state) {
	case VULKAN_OSD_COMPOSITOR_STATE_PAYLOAD_MAPPED:
		return "payload-mapped";
	case VULKAN_OSD_COMPOSITOR_STATE_UNSUPPORTED_FORMAT:
		return "unsupported-format";
	case VULKAN_OSD_COMPOSITOR_STATE_UNSUPPORTED_SWAPCHAIN_USAGE:
		return "unsupported-swapchain-usage";
	case VULKAN_OSD_COMPOSITOR_STATE_READY:
		return "ready";
	case VULKAN_OSD_COMPOSITOR_STATE_COPY_FAILED:
		return "copy-failed";
	case VULKAN_OSD_COMPOSITOR_STATE_NO_PAYLOAD:
	default:
		return "no-payload";
	}
}

static bool vulkan_osd_compositor_format_supported(VkFormat format)
{
	return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
	       format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

static void vulkan_osd_compositor_free_buffer(struct vulkan_osd_compositor *osd,
					      const struct vulkan_osd_compositor_context *ctx)
{
	if (osd->buffer_data) {
		ctx->UnmapMemory(ctx->device, osd->buffer_mem);
		osd->buffer_data = NULL;
	}
	if (osd->buffer) {
		ctx->DestroyBuffer(ctx->device, osd->buffer, ctx->alloc);
		osd->buffer = VK_NULL_HANDLE;
	}
	if (osd->buffer_mem) {
		ctx->FreeMemory(ctx->device, osd->buffer_mem, ctx->alloc);
		osd->buffer_mem = VK_NULL_HANDLE;
	}
	osd->buffer_size = 0;
}

void vulkan_osd_compositor_free(struct vulkan_osd_compositor *osd,
				const struct vulkan_osd_compositor_context *ctx)
{
	vulkan_osd_compositor_free_buffer(osd, ctx);
	osd_payload_close(&osd->payload);

	osd->sequence = 0;
	osd->width = 0;
	osd->height = 0;
	osd->state = VULKAN_OSD_COMPOSITOR_STATE_NO_PAYLOAD;
}

static bool vulkan_osd_compositor_open_map(struct vulkan_osd_compositor *osd,
					   const struct vulkan_osd_compositor_context *ctx)
{
	if (!osd_enabled() || !global_hook_info->osd_map_id) {
		osd->state = VULKAN_OSD_COMPOSITOR_STATE_NO_PAYLOAD;
		return false;
	}

	if (osd->payload.data && osd->payload.id == global_hook_info->osd_map_id) {
		osd->state = VULKAN_OSD_COMPOSITOR_STATE_PAYLOAD_MAPPED;
		return true;
	}

	vulkan_osd_compositor_free(osd, ctx);
	if (!osd_payload_open(&osd->payload, "vk_osd_open_map"))
		return false;

	osd->sequence = 0;
	osd->state = VULKAN_OSD_COMPOSITOR_STATE_PAYLOAD_MAPPED;
	return true;
}

static bool vulkan_osd_compositor_find_memory_type(const struct vulkan_osd_compositor_context *ctx,
						   uint32_t type_bits, VkMemoryPropertyFlags flags,
						   uint32_t *type_index)
{
	VkPhysicalDeviceMemoryProperties props;
	ctx->GetPhysicalDeviceMemoryProperties(ctx->physical_device, &props);

	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
			*type_index = i;
			return true;
		}
	}

	return false;
}

static bool vulkan_osd_compositor_create_buffer(struct vulkan_osd_compositor *osd,
						const struct vulkan_osd_compositor_context *ctx, size_t size)
{
	if (osd->buffer && osd->buffer_size >= size)
		return true;

	vulkan_osd_compositor_free_buffer(osd, ctx);

	VkBufferCreateInfo bci;
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.pNext = NULL;
	bci.flags = 0;
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bci.queueFamilyIndexCount = 0;
	bci.pQueueFamilyIndices = NULL;

	VkResult res = ctx->CreateBuffer(ctx->device, &bci, ctx->alloc, &osd->buffer);
	if (res != VK_SUCCESS) {
		osd_mark_failed();
		hlog("vk_osd_create_buffer: CreateBuffer failed: %s", result_to_str(res));
		goto fail;
	}

	VkMemoryRequirements req;
	ctx->GetBufferMemoryRequirements(ctx->device, osd->buffer, &req);

	uint32_t type_index = 0;
	if (!vulkan_osd_compositor_find_memory_type(
		    ctx, req.memoryTypeBits,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type_index)) {
		osd_mark_failed();
		hlog("vk_osd_create_buffer: no host-visible coherent memory type");
		goto fail;
	}

	VkMemoryAllocateInfo mai;
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.pNext = NULL;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = type_index;

	res = ctx->AllocateMemory(ctx->device, &mai, ctx->alloc, &osd->buffer_mem);
	if (res != VK_SUCCESS) {
		osd_mark_failed();
		hlog("vk_osd_create_buffer: AllocateMemory failed: %s", result_to_str(res));
		goto fail;
	}

	res = ctx->BindBufferMemory(ctx->device, osd->buffer, osd->buffer_mem, 0);
	if (res != VK_SUCCESS) {
		osd_mark_failed();
		hlog("vk_osd_create_buffer: BindBufferMemory failed: %s", result_to_str(res));
		goto fail;
	}

	res = ctx->MapMemory(ctx->device, osd->buffer_mem, 0, size, 0, (void **)&osd->buffer_data);
	if (res != VK_SUCCESS) {
		osd_mark_failed();
		hlog("vk_osd_create_buffer: MapMemory failed: %s", result_to_str(res));
		goto fail;
	}

	osd->buffer_size = size;
	hlog("vk_osd_create_buffer: created %zu bytes", size);
	return true;

fail:
	vulkan_osd_compositor_free_buffer(osd, ctx);
	return false;
}

static void vulkan_osd_compositor_copy_payload(struct vulkan_osd_compositor *osd,
					       const struct osd_payload_snapshot *payload, VkFormat swap_format,
					       uint32_t width, uint32_t height)
{
	const bool swap_rgba = swap_format == VK_FORMAT_R8G8B8A8_UNORM || swap_format == VK_FORMAT_R8G8B8A8_SRGB;
	uint8_t *dst = osd->buffer_data;

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *src_row = payload->pixels + (size_t)y * payload->pitch;
		uint8_t *dst_row = dst + (size_t)y * (size_t)width * 4;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t b = src_row[x * 4 + 0];
			const uint8_t g = src_row[x * 4 + 1];
			const uint8_t r = src_row[x * 4 + 2];
			const uint8_t a = src_row[x * 4 + 3];
			if (swap_rgba) {
				dst_row[x * 4 + 0] = r;
				dst_row[x * 4 + 1] = g;
				dst_row[x * 4 + 2] = b;
				dst_row[x * 4 + 3] = a;
			} else {
				dst_row[x * 4 + 0] = b;
				dst_row[x * 4 + 1] = g;
				dst_row[x * 4 + 2] = r;
				dst_row[x * 4 + 3] = a;
			}
		}
	}
}

bool vulkan_osd_compositor_prepare(struct vulkan_osd_compositor *osd,
				   const struct vulkan_osd_compositor_context *ctx, VkFormat swap_format,
				   const char *swap_format_name, bool transfer_dst_supported,
				   uint32_t surface_width, uint32_t surface_height,
				   struct vulkan_osd_compositor_draw *draw)
{
	if (!transfer_dst_supported) {
		osd->state = VULKAN_OSD_COMPOSITOR_STATE_UNSUPPORTED_SWAPCHAIN_USAGE;
		osd_mark_failed();
		return false;
	}

	if (!vulkan_osd_compositor_open_map(osd, ctx))
		return false;

	struct osd_payload_snapshot payload = {0};
	if (!osd_payload_snapshot_read(osd->payload.data, &payload))
		return false;

	if (payload.format != OSD_FORMAT_BGRA || !vulkan_osd_compositor_format_supported(swap_format)) {
		if (!osd->unsupported_logged) {
			osd_mark_failed();
			hlog("vk_osd_update_buffer: unsupported OSD/swap format osd=%u swap=%s", payload.format,
			     swap_format_name);
			osd->unsupported_logged = true;
		}
		osd->state = VULKAN_OSD_COMPOSITOR_STATE_UNSUPPORTED_FORMAT;
		return false;
	}

	const uint32_t width = min(payload.width, surface_width);
	const uint32_t height = min(payload.height, surface_height);
	if (!width || !height)
		return false;

	const size_t size = (size_t)width * (size_t)height * 4;
	if (!vulkan_osd_compositor_create_buffer(osd, ctx, size))
		return false;

	struct osd_payload_snapshot upload = payload;
	upload.width = width;
	upload.height = height;
	if (osd_payload_snapshot_changed(&upload, osd->sequence, osd->width, osd->height)) {
		vulkan_osd_compositor_copy_payload(osd, &payload, swap_format, width, height);
		osd->sequence = payload.sequence;
		osd->width = width;
		osd->height = height;
	}

	draw->buffer = osd->buffer;
	draw->rect = osd_anchor_rect(surface_width, surface_height, width, height);
	osd->state = VULKAN_OSD_COMPOSITOR_STATE_PAYLOAD_MAPPED;
	return true;
}

void vulkan_osd_compositor_record_copy(const struct vulkan_osd_compositor_context *ctx, VkCommandBuffer cmd_buffer,
				       VkImage image, bool capture_frame,
				       const struct vulkan_osd_compositor_draw *draw)
{
	VkImageMemoryBarrier osd_mb;
	osd_mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	osd_mb.pNext = NULL;
	osd_mb.srcAccessMask = capture_frame ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_MEMORY_READ_BIT;
	osd_mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	osd_mb.oldLayout = capture_frame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	osd_mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	osd_mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	osd_mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	osd_mb.image = image;
	osd_mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	osd_mb.subresourceRange.baseMipLevel = 0;
	osd_mb.subresourceRange.levelCount = 1;
	osd_mb.subresourceRange.baseArrayLayer = 0;
	osd_mb.subresourceRange.layerCount = 1;

	ctx->CmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
				0, NULL, 1, &osd_mb);

	VkBufferImageCopy osd_cpy;
	memset(&osd_cpy, 0, sizeof(osd_cpy));
	osd_cpy.bufferOffset = 0;
	osd_cpy.bufferRowLength = 0;
	osd_cpy.bufferImageHeight = 0;
	osd_cpy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	osd_cpy.imageSubresource.mipLevel = 0;
	osd_cpy.imageSubresource.baseArrayLayer = 0;
	osd_cpy.imageSubresource.layerCount = 1;
	osd_cpy.imageOffset.x = draw->rect.x;
	osd_cpy.imageOffset.y = draw->rect.y;
	osd_cpy.imageOffset.z = 0;
	osd_cpy.imageExtent.width = draw->rect.width;
	osd_cpy.imageExtent.height = draw->rect.height;
	osd_cpy.imageExtent.depth = 1;

	ctx->CmdCopyBufferToImage(cmd_buffer, draw->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &osd_cpy);
}
