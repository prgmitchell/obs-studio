#pragma once

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef OBS_LEGACY
#include "../graphics-hook-info.h"
#else
#include <graphics-hook-info.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct osd_payload_map {
	HANDLE handle;
	struct osd_data *data;
	uint32_t id;
};

struct osd_payload_snapshot {
	const uint8_t *pixels;
	uint32_t sequence;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
};

struct osd_rect {
	LONG x;
	LONG y;
	uint32_t width;
	uint32_t height;
};

bool osd_enabled(void);
void osd_payload_close(struct osd_payload_map *map);
bool osd_payload_open(struct osd_payload_map *map, const char *log_prefix);
bool osd_payload_snapshot_read(const struct osd_data *payload, struct osd_payload_snapshot *snapshot);
bool osd_payload_snapshot_changed(const struct osd_payload_snapshot *snapshot, uint32_t sequence, uint32_t width,
				  uint32_t height);
struct osd_rect osd_anchor_rect(uint32_t surface_width, uint32_t surface_height, uint32_t width, uint32_t height);
void osd_mark_ready(void);
void osd_mark_failed(void);

#ifdef __cplusplus
}
#endif
