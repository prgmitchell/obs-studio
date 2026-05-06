#include "d3d11-osd-compositor.hpp"

#include <d3dcompiler.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "graphics-hook.h"

namespace {
struct d3d11_osd_vertex {
	float x;
	float y;
	float u;
	float v;
};

template<typename T> static inline void release(T *&obj)
{
	if (obj) {
		obj->Release();
		obj = nullptr;
	}
}

static void log_hr(const char *prefix, const char *message, HRESULT hr)
{
	char text[256];
	snprintf(text, sizeof(text), "%s: %s", prefix, message);
	hlog_hr(text, hr);
}

static bool d3d11_osd_open_map(struct d3d11_osd_compositor *osd, const char *log_prefix)
{
	if (!osd_enabled())
		return false;

	if (osd->payload.data && osd->payload.id == global_hook_info->osd_map_id)
		return true;

	d3d11_osd_compositor_free(osd);
	if (!osd_payload_open(&osd->payload, log_prefix))
		return false;

	osd->sequence = 0;
	return true;
}

static bool d3d11_osd_create_texture(struct d3d11_osd_compositor *osd, ID3D11Device *device, uint32_t width,
				     uint32_t height, const char *log_prefix)
{
	if (osd->texture && osd->width == width && osd->height == height)
		return true;

	release(osd->texture_view);
	release(osd->texture);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	HRESULT hr = device->CreateTexture2D(&desc, nullptr, &osd->texture);
	if (FAILED(hr)) {
		osd_mark_failed();
		log_hr(log_prefix, "failed to create OSD texture", hr);
		return false;
	}

	hr = device->CreateShaderResourceView(osd->texture, nullptr, &osd->texture_view);
	if (FAILED(hr)) {
		release(osd->texture);
		osd_mark_failed();
		log_hr(log_prefix, "failed to create OSD shader resource view", hr);
		return false;
	}

	osd->width = width;
	osd->height = height;
	return true;
}

static inline bool d3d11_osd_format_supported(DXGI_FORMAT format)
{
	return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
	       format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static void d3d11_osd_copy_payload(D3D11_MAPPED_SUBRESOURCE &mapped, const struct osd_payload_snapshot &payload)
{
	const uint8_t *src = payload.pixels;
	uint8_t *dst = (uint8_t *)mapped.pData;

	for (uint32_t y = 0; y < payload.height; y++) {
		const uint8_t *src_row = src + (size_t)y * payload.pitch;
		uint8_t *dst_row = dst + (size_t)y * mapped.RowPitch;
		memcpy(dst_row, src_row, (size_t)payload.width * 4);
	}
}

static bool d3d11_osd_compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **blob,
				     const char *log_prefix)
{
	static HMODULE compiler = nullptr;
	if (!compiler)
		compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	if (!compiler)
		compiler = LoadLibraryW(L"d3dcompiler_43.dll");
	if (!compiler) {
		hlog("%s: d3dcompiler not available", log_prefix);
		return false;
	}

	typedef HRESULT(WINAPI * d3d_compile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
						LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
	d3d_compile_t compile = (d3d_compile_t)GetProcAddress(compiler, "D3DCompile");
	if (!compile) {
		hlog("%s: D3DCompile not found", log_prefix);
		return false;
	}

	ID3DBlob *errors = nullptr;
	HRESULT hr = compile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, blob, &errors);
	if (FAILED(hr)) {
		if (errors) {
			hlog("%s: %s", log_prefix, (const char *)errors->GetBufferPointer());
			errors->Release();
		}
		return false;
	}

	if (errors)
		errors->Release();
	return true;
}

static bool d3d11_osd_create_render_resources(struct d3d11_osd_compositor *osd, ID3D11Device *device,
					      const char *log_prefix)
{
	if (osd->vertex_shader && osd->pixel_shader && osd->input_layout && osd->vertex_buffer && osd->sampler &&
	    osd->blend && osd->rasterizer && osd->depth_stencil)
		return true;

	static const char *vs_source =
		"struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
		"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
		"VSOut main(VSIn input) { VSOut output; output.pos = float4(input.pos, 0.0, 1.0);"
		"output.uv = input.uv; return output; }";
	static const char *ps_source =
		"Texture2D osd_tex : register(t0);"
		"SamplerState osd_sampler : register(s0);"
		"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET"
		"{ return osd_tex.Sample(osd_sampler, uv); }";

	ID3DBlob *vs = nullptr;
	ID3DBlob *ps = nullptr;
	HRESULT hr;
	D3D11_INPUT_ELEMENT_DESC layout[2] = {};
	D3D11_BUFFER_DESC buffer_desc = {};
	D3D11_SAMPLER_DESC sampler_desc = {};
	D3D11_BLEND_DESC blend_desc = {};
	D3D11_RASTERIZER_DESC rasterizer_desc = {};
	D3D11_DEPTH_STENCIL_DESC depth_desc = {};

	if (!d3d11_osd_compile_shader(vs_source, "main", "vs_4_0", &vs, log_prefix))
		goto fail;
	if (!d3d11_osd_compile_shader(ps_source, "main", "ps_4_0", &ps, log_prefix))
		goto fail;

	hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &osd->vertex_shader);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD vertex shader", hr);
		goto fail;
	}

	hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &osd->pixel_shader);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD pixel shader", hr);
		goto fail;
	}

	layout[0] = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
	layout[1] = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0};
	hr = device->CreateInputLayout(layout, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &osd->input_layout);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD input layout", hr);
		goto fail;
	}

	buffer_desc.ByteWidth = sizeof(d3d11_osd_vertex) * 6;
	buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
	buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&buffer_desc, nullptr, &osd->vertex_buffer);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD vertex buffer", hr);
		goto fail;
	}

	sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	hr = device->CreateSamplerState(&sampler_desc, &osd->sampler);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD sampler", hr);
		goto fail;
	}

	blend_desc.RenderTarget[0].BlendEnable = true;
	blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = device->CreateBlendState(&blend_desc, &osd->blend);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD blend state", hr);
		goto fail;
	}

	rasterizer_desc.FillMode = D3D11_FILL_SOLID;
	rasterizer_desc.CullMode = D3D11_CULL_NONE;
	rasterizer_desc.DepthClipEnable = true;
	hr = device->CreateRasterizerState(&rasterizer_desc, &osd->rasterizer);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD rasterizer state", hr);
		goto fail;
	}

	depth_desc.DepthEnable = false;
	depth_desc.StencilEnable = false;
	hr = device->CreateDepthStencilState(&depth_desc, &osd->depth_stencil);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to create OSD depth stencil state", hr);
		goto fail;
	}

	vs->Release();
	ps->Release();
	hlog("%s: compositor ready", log_prefix);
	return true;

fail:
	if (vs)
		vs->Release();
	if (ps)
		ps->Release();
	osd_mark_failed();
	return false;
}

static bool d3d11_osd_update_vertices(struct d3d11_osd_compositor *osd, ID3D11DeviceContext *context,
				      uint32_t surface_width, uint32_t surface_height, uint32_t width, uint32_t height,
				      const char *log_prefix)
{
	const struct osd_rect rect = osd_anchor_rect(surface_width, surface_height, width, height);
	const float left = ((float)rect.x / (float)surface_width) * 2.0f - 1.0f;
	const float right = ((float)(rect.x + (LONG)rect.width) / (float)surface_width) * 2.0f - 1.0f;
	const float top = 1.0f - ((float)rect.y / (float)surface_height) * 2.0f;
	const float bottom = 1.0f - ((float)(rect.y + (LONG)rect.height) / (float)surface_height) * 2.0f;

	const d3d11_osd_vertex vertices[] = {
		{left, top, 0.0f, 0.0f},     {right, top, 1.0f, 0.0f},   {left, bottom, 0.0f, 1.0f},
		{left, bottom, 0.0f, 1.0f},  {right, top, 1.0f, 0.0f},   {right, bottom, 1.0f, 1.0f},
	};

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = context->Map(osd->vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		log_hr(log_prefix, "failed to map OSD vertex buffer", hr);
		return false;
	}

	memcpy(mapped.pData, vertices, sizeof(vertices));
	context->Unmap(osd->vertex_buffer, 0);
	return true;
}

struct d3d11_state_backup {
	ID3D11InputLayout *layout = nullptr;
	ID3D11Buffer *vertex_buffer = nullptr;
	UINT stride = 0;
	UINT offset = 0;
	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11SamplerState *sampler = nullptr;
	ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView *dsv = nullptr;
	ID3D11BlendState *blend = nullptr;
	FLOAT blend_factor[4] = {};
	UINT sample_mask = 0;
	ID3D11DepthStencilState *depth = nullptr;
	UINT stencil_ref = 0;
	ID3D11RasterizerState *rasterizer = nullptr;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

	~d3d11_state_backup() { release_refs(); }

	void save(ID3D11DeviceContext *context)
	{
		context->IAGetInputLayout(&layout);
		context->IAGetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
		context->IAGetPrimitiveTopology(&topology);
		context->VSGetShader(&vs, nullptr, nullptr);
		context->PSGetShader(&ps, nullptr, nullptr);
		context->PSGetShaderResources(0, 1, &srv);
		context->PSGetSamplers(0, 1, &sampler);
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
		context->OMGetBlendState(&blend, blend_factor, &sample_mask);
		context->OMGetDepthStencilState(&depth, &stencil_ref);
		context->RSGetState(&rasterizer);
		context->RSGetViewports(&viewport_count, viewports);
	}

	void restore(ID3D11DeviceContext *context)
	{
		context->IASetInputLayout(layout);
		context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
		context->IASetPrimitiveTopology(topology);
		context->VSSetShader(vs, nullptr, 0);
		context->PSSetShader(ps, nullptr, 0);
		context->PSSetShaderResources(0, 1, &srv);
		context->PSSetSamplers(0, 1, &sampler);
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
		context->OMSetBlendState(blend, blend_factor, sample_mask);
		context->OMSetDepthStencilState(depth, stencil_ref);
		context->RSSetState(rasterizer);
		context->RSSetViewports(viewport_count, viewports);
		release_refs();
	}

	void release_refs()
	{
		release(layout);
		release(vertex_buffer);
		release(vs);
		release(ps);
		release(srv);
		release(sampler);
		for (size_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
			release(rtv[i]);
		release(dsv);
		release(blend);
		release(depth);
		release(rasterizer);

		stride = 0;
		offset = 0;
		topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		memset(blend_factor, 0, sizeof(blend_factor));
		sample_mask = 0;
		stencil_ref = 0;
		memset(viewports, 0, sizeof(viewports));
		viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	}
};
} // namespace

void d3d11_osd_compositor_free(struct d3d11_osd_compositor *osd)
{
	release(osd->depth_stencil);
	release(osd->rasterizer);
	release(osd->blend);
	release(osd->sampler);
	release(osd->vertex_buffer);
	release(osd->input_layout);
	release(osd->pixel_shader);
	release(osd->vertex_shader);
	release(osd->texture_view);
	release(osd->texture);
	osd_payload_close(&osd->payload);

	*osd = {};
}

bool d3d11_osd_compositor_render(struct d3d11_osd_compositor *osd, ID3D11Device *device,
				 ID3D11DeviceContext *context, ID3D11Resource *backbuffer, uint32_t cx,
				 uint32_t cy, DXGI_FORMAT format, bool validate_swap_format, const char *log_prefix)
{
	if (!device || !context || !backbuffer || !osd_enabled())
		return false;

	if (!d3d11_osd_open_map(osd, log_prefix))
		return false;

	struct osd_payload_snapshot payload = {};
	if (!osd_payload_snapshot_read(osd->payload.data, &payload))
		return false;

	if (payload.format != OSD_FORMAT_BGRA || (validate_swap_format && !d3d11_osd_format_supported(format))) {
		osd_mark_failed();
		hlog("%s: unsupported OSD/swap format osd=%u swap=%u", log_prefix, payload.format, (uint32_t)format);
		return false;
	}

	if (!d3d11_osd_create_texture(osd, device, payload.width, payload.height, log_prefix))
		return false;

	if (!d3d11_osd_create_render_resources(osd, device, log_prefix))
		return false;

	if (payload.sequence != osd->sequence) {
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		HRESULT hr = context->Map(osd->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) {
			osd_mark_failed();
			log_hr(log_prefix, "failed to map OSD texture", hr);
			return false;
		}

		d3d11_osd_copy_payload(mapped, payload);
		context->Unmap(osd->texture, 0);
		osd->sequence = payload.sequence;
	}

	if (!d3d11_osd_update_vertices(osd, context, cx, cy, payload.width, payload.height, log_prefix))
		return false;

	ID3D11RenderTargetView *rtv = nullptr;
	HRESULT hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
	if (FAILED(hr)) {
		osd_mark_failed();
		log_hr(log_prefix, "failed to create OSD render target view", hr);
		return false;
	}

	d3d11_state_backup state;
	state.save(context);

	const UINT stride = sizeof(d3d11_osd_vertex);
	const UINT offset = 0;
	D3D11_VIEWPORT viewport = {};
	viewport.Width = (FLOAT)cx;
	viewport.Height = (FLOAT)cy;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	context->IASetInputLayout(osd->input_layout);
	context->IASetVertexBuffers(0, 1, &osd->vertex_buffer, &stride, &offset);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(osd->vertex_shader, nullptr, 0);
	context->PSSetShader(osd->pixel_shader, nullptr, 0);
	context->PSSetShaderResources(0, 1, &osd->texture_view);
	context->PSSetSamplers(0, 1, &osd->sampler);
	context->RSSetState(osd->rasterizer);
	context->RSSetViewports(1, &viewport);
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetBlendState(osd->blend, nullptr, 0xffffffff);
	context->OMSetDepthStencilState(osd->depth_stencil, 0);
	context->Draw(6, 0);

	ID3D11ShaderResourceView *null_srv = nullptr;
	context->PSSetShaderResources(0, 1, &null_srv);

	state.restore(context);
	rtv->Release();

	osd_mark_ready();
	return true;
}
