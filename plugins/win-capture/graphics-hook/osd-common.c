#include "osd-common.h"

#include "graphics-hook.h"

#include <stdio.h>

bool osd_enabled(void)
{
	return global_hook_info && (global_hook_info->osd_flags & OSD_HOOK_ENABLED) != 0;
}

void osd_payload_close(struct osd_payload_map *map)
{
	if (map->data) {
		UnmapViewOfFile(map->data);
		map->data = NULL;
	}
	if (map->handle) {
		CloseHandle(map->handle);
		map->handle = NULL;
	}
	map->id = 0;
}

bool osd_payload_open(struct osd_payload_map *map, const char *log_prefix)
{
	if (!osd_enabled() || !global_hook_info->osd_map_id)
		return false;

	if (map->data && map->id == global_hook_info->osd_map_id)
		return true;

	osd_payload_close(map);

	wchar_t name[64];
	swprintf(name, _countof(name), SHMEM_OSD L"%lu_%u", (unsigned long)GetCurrentProcessId(),
		 global_hook_info->osd_map_id);

	map->handle = OpenFileMappingW(FILE_MAP_READ, false, name);
	if (!map->handle) {
		osd_mark_failed();
		hlog("%s: failed to open OSD map %ls: %lu", log_prefix, name, GetLastError());
		return false;
	}

	map->data = (struct osd_data *)MapViewOfFile(map->handle, FILE_MAP_READ, 0, 0, 0);
	if (!map->data) {
		osd_mark_failed();
		hlog("%s: failed to map OSD payload: %lu", log_prefix, GetLastError());
		CloseHandle(map->handle);
		map->handle = NULL;
		return false;
	}

	map->id = global_hook_info->osd_map_id;
	hlog("%s: mapped OSD payload id=%u", log_prefix, map->id);
	return true;
}

bool osd_payload_snapshot_read(const struct osd_data *payload, struct osd_payload_snapshot *snapshot)
{
	if (!payload || !snapshot)
		return false;

	const uint32_t sequence = payload->sequence;
	if (sequence & 1)
		return false;

	if (!payload->width || !payload->height || !payload->pitch || !payload->pixels)
		return false;

	snapshot->pixels = payload->pixels;
	snapshot->sequence = sequence;
	snapshot->width = payload->width;
	snapshot->height = payload->height;
	snapshot->pitch = payload->pitch;
	snapshot->format = payload->format;

	return sequence == payload->sequence && !(payload->sequence & 1);
}

bool osd_payload_snapshot_changed(const struct osd_payload_snapshot *snapshot, uint32_t sequence, uint32_t width,
				  uint32_t height)
{
	return snapshot->sequence != sequence || snapshot->width != width || snapshot->height != height;
}

struct osd_rect osd_anchor_rect(uint32_t surface_width, uint32_t surface_height, uint32_t width, uint32_t height)
{
	const LONG margin = 24;
	struct osd_rect rect = {0};
	const int surface_w = (int)surface_width;
	const int surface_h = (int)surface_height;
	const int w = (int)width;
	const int h = (int)height;

	rect.width = width;
	rect.height = height;

	switch (global_hook_info ? global_hook_info->osd_anchor : 0) {
	case 1:
		rect.x = margin;
		rect.y = margin;
		break;
	case 2:
		rect.x = (LONG)max(surface_w - w - margin, margin);
		rect.y = (LONG)max(surface_h - h - margin, margin);
		break;
	case 3:
		rect.x = margin;
		rect.y = (LONG)max(surface_h - h - margin, margin);
		break;
	default:
		rect.x = (LONG)max(surface_w - w - margin, margin);
		rect.y = margin;
		break;
	}

	return rect;
}

void osd_mark_ready(void)
{
	if (global_hook_info) {
		global_hook_info->osd_flags &= ~OSD_HOOK_COMPOSITOR_FAILED;
		global_hook_info->osd_flags |= OSD_HOOK_COMPOSITOR_READY;
	}
}

void osd_mark_failed(void)
{
	if (global_hook_info)
		global_hook_info->osd_flags |= OSD_HOOK_COMPOSITOR_FAILED;
}
