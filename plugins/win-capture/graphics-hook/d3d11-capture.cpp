#include <d3d11.h>
#include <dxgi.h>

#include "dxgi-helpers.hpp"
#include "graphics-hook.h"

struct d3d11_data {
	ID3D11Device *device;         /* do not release */
	ID3D11DeviceContext *context; /* do not release */
	uint32_t cx;
	uint32_t cy;
	DXGI_FORMAT format;
	bool using_shtex;
	bool multisampled;

	union {
		/* shared texture */
		struct {
			struct shtex_data *shtex_info;
			ID3D11Texture2D *texture;
			HANDLE handle;
		};
		/* shared memory */
		struct {
			ID3D11Texture2D *copy_surfaces[NUM_BUFFERS];
			bool texture_ready[NUM_BUFFERS];
			bool texture_mapped[NUM_BUFFERS];
			uint32_t pitch;
			struct shmem_data *shmem_info;
			int cur_tex;
			int copy_wait;
		};
	};
};

static struct d3d11_data data = {};

// Forward declaration for OSD cleanup
static void osd_free_resources(void);

void d3d11_free(void)
{
	osd_free_resources();
	capture_free();

	if (data.using_shtex) {
		if (data.texture)
			data.texture->Release();
	} else {
		for (size_t i = 0; i < NUM_BUFFERS; i++) {
			if (data.copy_surfaces[i]) {
				if (data.texture_mapped[i])
					data.context->Unmap(data.copy_surfaces[i], 0);
				data.copy_surfaces[i]->Release();
			}
		}
	}

	memset(&data, 0, sizeof(data));

	hlog("----------------- d3d11 capture freed ----------------");
}

static bool create_d3d11_stage_surface(ID3D11Texture2D **tex)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = data.cx;
	desc.Height = data.cy;
	desc.Format = data.format;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	hr = data.device->CreateTexture2D(&desc, nullptr, tex);
	if (FAILED(hr)) {
		hlog_hr("create_d3d11_stage_surface: failed to create texture", hr);
		return false;
	}

	return true;
}

static bool create_d3d11_tex(uint32_t cx, uint32_t cy, ID3D11Texture2D **tex, HANDLE *handle)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = cx;
	desc.Height = cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = apply_dxgi_format_typeless(data.format, global_hook_info->allow_srgb_alias);
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	hr = data.device->CreateTexture2D(&desc, nullptr, tex);
	if (FAILED(hr)) {
		hlog_hr("create_d3d11_tex: failed to create texture", hr);
		return false;
	}

	if (!!handle) {
		IDXGIResource *dxgi_res;
		hr = (*tex)->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgi_res);
		if (FAILED(hr)) {
			hlog_hr("create_d3d11_tex: failed to query "
				"IDXGIResource interface from texture",
				hr);
			return false;
		}

		hr = dxgi_res->GetSharedHandle(handle);
		dxgi_res->Release();
		if (FAILED(hr)) {
			hlog_hr("create_d3d11_tex: failed to get shared handle", hr);
			return false;
		}
	}

	return true;
}

static inline bool d3d11_init_format(IDXGISwapChain *swap, HWND &window)
{
	DXGI_SWAP_CHAIN_DESC desc;
	HRESULT hr;

	hr = swap->GetDesc(&desc);
	if (FAILED(hr)) {
		hlog_hr("d3d11_init_format: swap->GetDesc failed", hr);
		return false;
	}

	print_swap_desc(&desc);

	data.format = strip_dxgi_format_srgb(desc.BufferDesc.Format);
	data.multisampled = desc.SampleDesc.Count > 1;
	window = desc.OutputWindow;
	data.cx = desc.BufferDesc.Width;
	data.cy = desc.BufferDesc.Height;

	return true;
}

static bool d3d11_shmem_init_buffers(size_t idx)
{
	bool success;

	success = create_d3d11_stage_surface(&data.copy_surfaces[idx]);
	if (!success) {
		hlog("d3d11_shmem_init_buffers: failed to create copy surface");
		return false;
	}

	if (idx == 0) {
		D3D11_MAPPED_SUBRESOURCE map = {};
		HRESULT hr;

		hr = data.context->Map(data.copy_surfaces[idx], 0, D3D11_MAP_READ, 0, &map);
		if (FAILED(hr)) {
			hlog_hr("d3d11_shmem_init_buffers: failed to get "
				"pitch",
				hr);
			return false;
		}

		data.pitch = map.RowPitch;
		data.context->Unmap(data.copy_surfaces[idx], 0);
	}

	return true;
}

static bool d3d11_shmem_init(HWND window)
{
	data.using_shtex = false;

	for (size_t i = 0; i < NUM_BUFFERS; i++) {
		if (!d3d11_shmem_init_buffers(i)) {
			return false;
		}
	}
	if (!capture_init_shmem(&data.shmem_info, window, data.cx, data.cy, data.pitch, data.format, false)) {
		return false;
	}

	hlog("d3d11 memory capture successful");
	return true;
}

static bool d3d11_shtex_init(HWND window)
{
	bool success;

	data.using_shtex = true;

	success = create_d3d11_tex(data.cx, data.cy, &data.texture, &data.handle);

	if (!success) {
		hlog("d3d11_shtex_init: failed to create texture");
		return false;
	}
	if (!capture_init_shtex(&data.shtex_info, window, data.cx, data.cy, data.format, false,
				(uintptr_t)data.handle)) {
		return false;
	}

	hlog("d3d11 shared texture capture successful");
	return true;
}

static void d3d11_init(IDXGISwapChain *swap)
{
	HWND window;
	HRESULT hr;

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&data.device);
	if (FAILED(hr)) {
		hlog_hr("d3d11_init: failed to get device from swap", hr);
		return;
	}

	data.device->Release();

	data.device->GetImmediateContext(&data.context);
	data.context->Release();

	if (!d3d11_init_format(swap, window)) {
		return;
	}

	const bool success = global_hook_info->force_shmem ? d3d11_shmem_init(window) : d3d11_shtex_init(window);
	if (!success)
		d3d11_free();
}

static inline void d3d11_copy_texture(ID3D11Resource *dst, ID3D11Resource *src)
{
	if (data.multisampled) {
		data.context->ResolveSubresource(dst, 0, src, 0, data.format);
	} else {
		data.context->CopyResource(dst, src);
	}
}

static inline void d3d11_shtex_capture(ID3D11Resource *backbuffer)
{
	if (data.texture) {
		d3d11_copy_texture(data.texture, backbuffer);
	}
}

static void d3d11_shmem_capture_copy(int i)
{
	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hr;

	if (data.texture_ready[i]) {
		data.texture_ready[i] = false;

		hr = data.context->Map(data.copy_surfaces[i], 0, D3D11_MAP_READ, 0, &map);
		if (SUCCEEDED(hr)) {
			data.texture_mapped[i] = true;
			shmem_copy_data(i, map.pData);
		}
	}
}

static inline void d3d11_shmem_capture(ID3D11Resource *backbuffer)
{
	int next_tex;

	next_tex = (data.cur_tex + 1) % NUM_BUFFERS;
	d3d11_shmem_capture_copy(next_tex);

	if (data.copy_wait < NUM_BUFFERS - 1) {
		data.copy_wait++;
	} else {
		if (shmem_texture_data_lock(data.cur_tex)) {
			data.context->Unmap(data.copy_surfaces[data.cur_tex], 0);
			data.texture_mapped[data.cur_tex] = false;
			shmem_texture_data_unlock(data.cur_tex);
		}

		d3d11_copy_texture(data.copy_surfaces[data.cur_tex], backbuffer);
		data.texture_ready[data.cur_tex] = true;
	}

	data.cur_tex = next_tex;
}

void d3d11_capture(void *swap_ptr, void *backbuffer_ptr)
{
	IDXGIResource *dxgi_backbuffer = (IDXGIResource *)backbuffer_ptr;
	IDXGISwapChain *swap = (IDXGISwapChain *)swap_ptr;

	HRESULT hr;
	if (capture_should_stop()) {
		d3d11_free();
	}
	if (capture_should_init()) {
		d3d11_init(swap);
	}
	if (data.handle != nullptr && capture_ready()) {
		ID3D11Resource *backbuffer;

		hr = dxgi_backbuffer->QueryInterface(__uuidof(ID3D11Resource), (void **)&backbuffer);
		if (FAILED(hr)) {
			hlog_hr("d3d11_shtex_capture: failed to get "
				"backbuffer",
				hr);
			return;
		}

		if (data.using_shtex)
			d3d11_shtex_capture(backbuffer);
		else
			d3d11_shmem_capture(backbuffer);

		backbuffer->Release();
	}
}

#include "osd-font.h"
#include "osd-shaders.h"
#include <d3dcompiler.h>

// OSD rendering resources
static ID3D11Device *osd_device = nullptr;
static ID3D11VertexShader *osd_vs = nullptr;
static ID3D11PixelShader *osd_ps = nullptr;
static ID3D11InputLayout *osd_layout = nullptr;
static ID3D11Buffer *osd_vb = nullptr;
static ID3D11Buffer *osd_cb_screen = nullptr;
static ID3D11Buffer *osd_cb_color = nullptr;
static ID3D11Texture2D *osd_font_tex = nullptr;
static ID3D11ShaderResourceView *osd_font_srv = nullptr;
static ID3D11SamplerState *osd_sampler = nullptr;
static ID3D11BlendState *osd_blend = nullptr;
static ID3D11RasterizerState *osd_raster = nullptr;
static ID3D11DepthStencilState *osd_depth = nullptr;
static bool osd_init_failed = false;

struct OSDVertex {
	float x, y;
	float u, v;
};

static void osd_free_resources(void)
{
	if (osd_vs) {
		osd_vs->Release();
		osd_vs = nullptr;
	}
	if (osd_ps) {
		osd_ps->Release();
		osd_ps = nullptr;
	}
	if (osd_layout) {
		osd_layout->Release();
		osd_layout = nullptr;
	}
	if (osd_vb) {
		osd_vb->Release();
		osd_vb = nullptr;
	}
	if (osd_cb_screen) {
		osd_cb_screen->Release();
		osd_cb_screen = nullptr;
	}
	if (osd_cb_color) {
		osd_cb_color->Release();
		osd_cb_color = nullptr;
	}
	if (osd_font_tex) {
		osd_font_tex->Release();
		osd_font_tex = nullptr;
	}
	if (osd_font_srv) {
		osd_font_srv->Release();
		osd_font_srv = nullptr;
	}
	if (osd_sampler) {
		osd_sampler->Release();
		osd_sampler = nullptr;
	}
	if (osd_blend) {
		osd_blend->Release();
		osd_blend = nullptr;
	}
	if (osd_raster) {
		osd_raster->Release();
		osd_raster = nullptr;
	}
	if (osd_depth) {
		osd_depth->Release();
		osd_depth = nullptr;
	}
	if (osd_device) {
		osd_device->Release();
		osd_device = nullptr;
	}
	osd_init_failed = false;
}

static bool osd_init_resources(ID3D11Device *device)
{
	HRESULT hr;

	// Compile vertex shader
	ID3DBlob *vs_blob = nullptr;
	ID3DBlob *error_blob = nullptr;
	hr = D3DCompile(osd_vs_hlsl, strlen(osd_vs_hlsl), "osd_vs", nullptr, nullptr, "VSMain", "vs_4_0", 0, 0,
			&vs_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob) {
			hlog("OSD VS compile error: %s", (char *)error_blob->GetBufferPointer());
			error_blob->Release();
		}
		return false;
	}
	hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &osd_vs);
	if (FAILED(hr)) {
		vs_blob->Release();
		return false;
	}

	// Create input layout
	D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	hr = device->CreateInputLayout(layout_desc, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
				       &osd_layout);
	vs_blob->Release();
	if (FAILED(hr))
		return false;

	// Compile pixel shader
	ID3DBlob *ps_blob = nullptr;
	hr = D3DCompile(osd_ps_hlsl, strlen(osd_ps_hlsl), "osd_ps", nullptr, nullptr, "PSMain", "ps_4_0", 0, 0,
			&ps_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob) {
			hlog("OSD PS compile error: %s", (char *)error_blob->GetBufferPointer());
			error_blob->Release();
		}
		return false;
	}
	hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &osd_ps);
	ps_blob->Release();
	if (FAILED(hr))
		return false;

	// Create vertex buffer (enough for 64 characters, 6 verts each)
	D3D11_BUFFER_DESC vb_desc = {};
	vb_desc.ByteWidth = sizeof(OSDVertex) * 64 * 6;
	vb_desc.Usage = D3D11_USAGE_DYNAMIC;
	vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&vb_desc, nullptr, &osd_vb);
	if (FAILED(hr))
		return false;

	// Create constant buffers
	D3D11_BUFFER_DESC cb_desc = {};
	cb_desc.ByteWidth = 16;
	cb_desc.Usage = D3D11_USAGE_DYNAMIC;
	cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&cb_desc, nullptr, &osd_cb_screen);
	if (FAILED(hr))
		return false;
	hr = device->CreateBuffer(&cb_desc, nullptr, &osd_cb_color);
	if (FAILED(hr))
		return false;

	// Create font texture (20 chars * 8 wide, 16 tall = 160x16)
	const int num_chars = 20; // space, 0-9, :, A, C, E, I, L, M, R, V
	D3D11_TEXTURE2D_DESC tex_desc = {};
	tex_desc.Width = num_chars * OSD_FONT_CHAR_WIDTH;
	tex_desc.Height = OSD_FONT_CHAR_HEIGHT;
	tex_desc.MipLevels = 1;
	tex_desc.ArraySize = 1;
	tex_desc.Format = DXGI_FORMAT_R8_UNORM;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.Usage = D3D11_USAGE_DEFAULT;
	tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	// Convert bitmap font to texture data
	uint8_t *font_pixels = new uint8_t[tex_desc.Width * tex_desc.Height];
	memset(font_pixels, 0, tex_desc.Width * tex_desc.Height);
	for (int c = 0; c < num_chars; c++) {
		for (int row = 0; row < OSD_FONT_CHAR_HEIGHT; row++) {
			uint8_t byte = osd_font_data[c * OSD_FONT_BYTES_PER_CHAR + row];
			for (int bit = 0; bit < 8; bit++) {
				int px = c * OSD_FONT_CHAR_WIDTH + (7 - bit);
				int py = row;
				font_pixels[py * tex_desc.Width + px] = (byte & (1 << bit)) ? 255 : 0;
			}
		}
	}

	D3D11_SUBRESOURCE_DATA tex_data = {};
	tex_data.pSysMem = font_pixels;
	tex_data.SysMemPitch = tex_desc.Width;
	hr = device->CreateTexture2D(&tex_desc, &tex_data, &osd_font_tex);
	delete[] font_pixels;
	if (FAILED(hr))
		return false;

	hr = device->CreateShaderResourceView(osd_font_tex, nullptr, &osd_font_srv);
	if (FAILED(hr))
		return false;

	// Create sampler
	D3D11_SAMPLER_DESC samp_desc = {};
	samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	hr = device->CreateSamplerState(&samp_desc, &osd_sampler);
	if (FAILED(hr))
		return false;

	// Create blend state (alpha blending)
	D3D11_BLEND_DESC blend_desc = {};
	blend_desc.RenderTarget[0].BlendEnable = TRUE;
	blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = device->CreateBlendState(&blend_desc, &osd_blend);
	if (FAILED(hr))
		return false;

	// Create rasterizer state
	D3D11_RASTERIZER_DESC rast_desc = {};
	rast_desc.FillMode = D3D11_FILL_SOLID;
	rast_desc.CullMode = D3D11_CULL_NONE;
	hr = device->CreateRasterizerState(&rast_desc, &osd_raster);
	if (FAILED(hr))
		return false;

	// Create depth stencil state (disabled)
	D3D11_DEPTH_STENCIL_DESC ds_desc = {};
	ds_desc.DepthEnable = FALSE;
	ds_desc.StencilEnable = FALSE;
	hr = device->CreateDepthStencilState(&ds_desc, &osd_depth);
	if (FAILED(hr))
		return false;

	hlog("OSD text renderer initialized successfully");
	return true;
}

// Forward declaration
void d3d11_draw_overlay_with_device(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *rtv,
				    UINT width, UINT height);

void d3d11_draw_overlay(void *swap_ptr)
{
	if (osd_init_failed)
		return;
	if (!global_osd_state)
		return;

	IDXGISwapChain *swap = (IDXGISwapChain *)swap_ptr;
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	HRESULT hr;

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&device);
	if (FAILED(hr))
		return;
	device->GetImmediateContext(&context);

	// Initialize resources if device changed
	if (device != osd_device) {
		osd_free_resources();
		osd_device = device;
		osd_device->AddRef();
		if (!osd_init_resources(device)) {
			hlog("Failed to initialize OSD resources");
			osd_init_failed = true;
			context->Release();
			device->Release();
			return;
		}
	}

	// Get backbuffer as render target
	ID3D11Texture2D *backbuffer = nullptr;
	hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backbuffer);
	if (FAILED(hr)) {
		context->Release();
		device->Release();
		return;
	}

	D3D11_TEXTURE2D_DESC bb_desc;
	backbuffer->GetDesc(&bb_desc);

	ID3D11RenderTargetView *rtv = nullptr;
	hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
	backbuffer->Release();
	if (FAILED(hr)) {
		context->Release();
		device->Release();
		return;
	}

	// Call the shared OSD rendering function
	d3d11_draw_overlay_with_device(device, context, rtv, bb_desc.Width, bb_desc.Height);

	rtv->Release();
	context->Release();
	device->Release();
}

// Shared OSD rendering function that can be called from both D3D11 and D3D12 overlays
void d3d11_draw_overlay_with_device(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *rtv,
				    UINT width, UINT height)
{
	if (!global_osd_state)
		return;
	if (osd_init_failed)
		return;

	// Initialize resources if device changed
	if (device != osd_device) {
		osd_free_resources();
		osd_device = device;
		osd_device->AddRef();
		if (!osd_init_resources(device)) {
			hlog("Failed to initialize OSD resources for D3D12");
			osd_init_failed = true;
			return;
		}
	}

	HRESULT hr;
	const char *text = global_osd_state->text;
	int text_len = (int)strlen(text);
	if (text_len == 0 || text_len > 63) {
		return;
	}

	// Calculate text dimensions
	float char_w = (float)OSD_FONT_CHAR_WIDTH * 2.5f; // Scale up 2.5x for visibility
	float char_h = (float)OSD_FONT_CHAR_HEIGHT * 2.5f;
	float total_text_w = text_len * char_w;
	float total_text_h = char_h;
	float padding = 10.0f;
	float margin = 20.0f;

	// Calculate position based on shared memory setting
	// OSDPosition enum: TopLeft=0, TopCenter=1, TopRight=2, BottomLeft=3, BottomCenter=4, BottomRight=5
	float x = margin;
	float y = margin;
	int position = global_osd_state->position;

	switch (position) {
	case 0: // TopLeft
		x = margin;
		y = margin;
		break;
	case 1: // TopCenter
		x = ((float)width - total_text_w - padding * 2) / 2.0f;
		y = margin;
		break;
	case 2: // TopRight
		x = (float)width - total_text_w - padding * 2 - margin;
		y = margin;
		break;
	case 3: // BottomLeft
		x = margin;
		y = (float)height - total_text_h - padding * 2 - margin;
		break;
	case 4: // BottomCenter
		x = ((float)width - total_text_w - padding * 2) / 2.0f;
		y = (float)height - total_text_h - padding * 2 - margin;
		break;
	case 5: // BottomRight
		x = (float)width - total_text_w - padding * 2 - margin;
		y = (float)height - total_text_h - padding * 2 - margin;
		break;
	}

	// Build vertex buffer for text
	OSDVertex *verts = nullptr;
	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = context->Map(osd_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		return;
	}
	verts = (OSDVertex *)mapped.pData;

	int num_chars_font = 20;
	int vert_count = 0;
	float text_x = x + padding;
	float text_y = y + padding;

	for (int i = 0; i < text_len; i++) {
		int idx = osd_font_get_index(text[i]);
		if (idx < 0)
			idx = 0; // Use space for unknown chars

		float u0 = (float)(idx * OSD_FONT_CHAR_WIDTH) / (float)(num_chars_font * OSD_FONT_CHAR_WIDTH);
		float u1 = (float)((idx + 1) * OSD_FONT_CHAR_WIDTH) / (float)(num_chars_font * OSD_FONT_CHAR_WIDTH);
		float v0 = 0.0f;
		float v1 = 1.0f;

		// Triangle 1
		verts[vert_count++] = {text_x, text_y, u0, v0};
		verts[vert_count++] = {text_x + char_w, text_y, u1, v0};
		verts[vert_count++] = {text_x, text_y + char_h, u0, v1};
		// Triangle 2
		verts[vert_count++] = {text_x + char_w, text_y, u1, v0};
		verts[vert_count++] = {text_x + char_w, text_y + char_h, u1, v1};
		verts[vert_count++] = {text_x, text_y + char_h, u0, v1};

		text_x += char_w;
	}
	context->Unmap(osd_vb, 0);

	// Update screen size constant buffer
	{
		D3D11_MAPPED_SUBRESOURCE cb_mapped;
		hr = context->Map(osd_cb_screen, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb_mapped);
		if (SUCCEEDED(hr)) {
			float *data = (float *)cb_mapped.pData;
			data[0] = (float)width;
			data[1] = (float)height;
			data[2] = 0;
			data[3] = 0;
			context->Unmap(osd_cb_screen, 0);
		}
	}

	// Update color constant buffer (white text with black shadow would be nice, but just white for now)
	{
		D3D11_MAPPED_SUBRESOURCE cb_mapped;
		hr = context->Map(osd_cb_color, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb_mapped);
		if (SUCCEEDED(hr)) {
			float *data = (float *)cb_mapped.pData;
			data[0] = 1.0f; // R
			data[1] = 1.0f; // G
			data[2] = 1.0f; // B
			data[3] = 1.0f; // A
			context->Unmap(osd_cb_color, 0);
		}
	}

	// Save state (simplified - just what we change)
	ID3D11RenderTargetView *old_rtv = nullptr;
	ID3D11DepthStencilView *old_dsv = nullptr;
	context->OMGetRenderTargets(1, &old_rtv, &old_dsv);

	// Set render state
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetBlendState(osd_blend, nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(osd_depth, 0);
	context->RSSetState(osd_raster);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)width;
	vp.Height = (float)height;
	vp.MaxDepth = 1.0f;
	context->RSSetViewports(1, &vp);

	context->IASetInputLayout(osd_layout);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	UINT stride = sizeof(OSDVertex);
	UINT offset = 0;
	context->IASetVertexBuffers(0, 1, &osd_vb, &stride, &offset);

	context->VSSetShader(osd_vs, nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &osd_cb_screen);
	context->PSSetShader(osd_ps, nullptr, 0);
	context->PSSetConstantBuffers(1, 1, &osd_cb_color);
	context->PSSetShaderResources(0, 1, &osd_font_srv);
	context->PSSetSamplers(0, 1, &osd_sampler);

	// Draw
	context->Draw(vert_count, 0);

	// Restore state
	context->OMSetRenderTargets(1, &old_rtv, old_dsv);
	if (old_rtv)
		old_rtv->Release();
	if (old_dsv)
		old_dsv->Release();
}
