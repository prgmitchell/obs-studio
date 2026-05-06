#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>

#include "osd-common.h"

struct d3d11_osd_compositor {
	struct osd_payload_map payload = {};
	uint32_t sequence = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	ID3D11Texture2D *texture = nullptr;
	ID3D11ShaderResourceView *texture_view = nullptr;
	ID3D11VertexShader *vertex_shader = nullptr;
	ID3D11PixelShader *pixel_shader = nullptr;
	ID3D11InputLayout *input_layout = nullptr;
	ID3D11Buffer *vertex_buffer = nullptr;
	ID3D11SamplerState *sampler = nullptr;
	ID3D11BlendState *blend = nullptr;
	ID3D11RasterizerState *rasterizer = nullptr;
	ID3D11DepthStencilState *depth_stencil = nullptr;
};

void d3d11_osd_compositor_free(struct d3d11_osd_compositor *osd);
bool d3d11_osd_compositor_render(struct d3d11_osd_compositor *osd, ID3D11Device *device,
				 ID3D11DeviceContext *context, ID3D11Resource *backbuffer, uint32_t cx,
				 uint32_t cy, DXGI_FORMAT format, bool validate_swap_format, const char *log_prefix);
