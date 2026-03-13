#include <graphics/math-defs.h>
#include <util/platform.h>
#include <util/util.hpp>
#include <util/windows/ComPtr.hpp>
#include <obs-module.h>
#include <sys/stat.h>
#include <combaseapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <d2d1.h>
#include <dwrite.h>
#include <locale>
#include <memory>
#include <string>
#include <vector>
#include <wincodec.h>

using namespace std;

#define warning(format, ...) blog(LOG_WARNING, "[%s] " format, obs_source_get_name(source), ##__VA_ARGS__)

#define MIN_SIZE_CX 2
#define MIN_SIZE_CY 2
#define MAX_SIZE_CX 16384
#define MAX_SIZE_CY 16384
#define MAX_AREA (4096LL * 4096LL)

/* clang-format off */
#define S_FONT           "font"
#define S_USE_FILE       "read_from_file"
#define S_FILE           "file"
#define S_TEXT           "text"
#define S_COLOR          "color"
#define S_GRADIENT       "gradient"
#define S_GRADIENT_COLOR "gradient_color"
#define S_GRADIENT_DIR   "gradient_dir"
#define S_GRADIENT_OPACITY "gradient_opacity"
#define S_ALIGN          "align"
#define S_VALIGN         "valign"
#define S_OPACITY        "opacity"
#define S_BKCOLOR        "bk_color"
#define S_BKOPACITY      "bk_opacity"
#define S_OUTLINE        "outline"
#define S_OUTLINE_SIZE   "outline_size"
#define S_OUTLINE_COLOR  "outline_color"
#define S_OUTLINE_OPACITY "outline_opacity"
#define S_CHATLOG_MODE   "chatlog"
#define S_CHATLOG_LINES  "chatlog_lines"
#define S_EXTENTS        "extents"
#define S_EXTENTS_WRAP   "extents_wrap"
#define S_EXTENTS_CX     "extents_cx"
#define S_EXTENTS_CY     "extents_cy"
#define S_TRANSFORM      "transform"
#define S_ANTIALIASING   "antialiasing"

#define S_ALIGN_LEFT     "left"
#define S_ALIGN_CENTER   "center"
#define S_ALIGN_RIGHT    "right"
#define S_VALIGN_TOP     "top"
#define S_VALIGN_CENTER  S_ALIGN_CENTER
#define S_VALIGN_BOTTOM  "bottom"

#define S_TRANSFORM_NONE      0
#define S_TRANSFORM_UPPERCASE 1
#define S_TRANSFORM_LOWERCASE 2
#define S_TRANSFORM_STARTCASE 3

#define T_(v)                obs_module_text(v)
#define T_FONT               T_("Font")
#define T_USE_FILE           T_("ReadFromFile")
#define T_FILE               T_("TextFile")
#define T_TEXT               T_("Text")
#define T_COLOR              T_("Color")
#define T_GRADIENT           T_("Gradient")
#define T_GRADIENT_COLOR     T_("Gradient.Color")
#define T_GRADIENT_DIR       T_("Gradient.Direction")
#define T_GRADIENT_OPACITY   T_("Gradient.Opacity")
#define T_ALIGN              T_("Alignment")
#define T_VALIGN             T_("VerticalAlignment")
#define T_OPACITY            T_("Opacity")
#define T_BKCOLOR            T_("BkColor")
#define T_BKOPACITY          T_("BkOpacity")
#define T_OUTLINE            T_("Outline")
#define T_OUTLINE_SIZE       T_("Outline.Size")
#define T_OUTLINE_COLOR      T_("Outline.Color")
#define T_OUTLINE_OPACITY    T_("Outline.Opacity")
#define T_CHATLOG_MODE       T_("ChatlogMode")
#define T_CHATLOG_LINES      T_("ChatlogMode.Lines")
#define T_EXTENTS            T_("UseCustomExtents")
#define T_EXTENTS_WRAP       T_("UseCustomExtents.Wrap")
#define T_EXTENTS_CX         T_("Width")
#define T_EXTENTS_CY         T_("Height")
#define T_TRANSFORM          T_("Transform")
#define T_ANTIALIASING       T_("Antialiasing")
#define T_FILTER_TEXT_FILES  T_("Filter.TextFiles")
#define T_FILTER_ALL_FILES   T_("Filter.AllFiles")
#define T_ALIGN_LEFT         T_("Alignment.Left")
#define T_ALIGN_CENTER       T_("Alignment.Center")
#define T_ALIGN_RIGHT        T_("Alignment.Right")
#define T_VALIGN_TOP         T_("VerticalAlignment.Top")
#define T_VALIGN_CENTER      T_ALIGN_CENTER
#define T_VALIGN_BOTTOM      T_("VerticalAlignment.Bottom")
#define T_TRANSFORM_NONE     T_("Transform.None")
#define T_TRANSFORM_UPPERCASE T_("Transform.Uppercase")
#define T_TRANSFORM_LOWERCASE T_("Transform.Lowercase")
#define T_TRANSFORM_STARTCASE T_("Transform.Startcase")
/* clang-format on */

static inline wstring to_wide(const char *utf8)
{
	wstring text;
	size_t len = os_utf8_to_wcs(utf8, 0, nullptr, 0);
	text.resize(len);
	if (len)
		os_utf8_to_wcs(utf8, 0, &text[0], len + 1);
	return text;
}

static inline uint32_t rgb_to_bgr(uint32_t rgb)
{
	return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb & 0xFF0000) >> 16);
}

static inline time_t get_modified_timestamp(const char *filename)
{
	struct stat stats;
	if (os_stat(filename, &stats) != 0)
		return -1;
	return stats.st_mtime;
}

static inline D2D1::ColorF to_d2d_color(uint32_t color, uint32_t opacity)
{
	return D2D1::ColorF(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
			   opacity / 100.0f);
}

static inline uint8_t unpremultiply(uint8_t channel, uint8_t alpha)
{
	if (alpha == 0 || alpha == 255)
		return channel;
	return (uint8_t)min<uint32_t>((uint32_t(channel) * 255u + alpha / 2u) / uint32_t(alpha), 255u);
}

static bool has_emoji(const wstring &text)
{
	for (size_t i = 0; i < text.size(); i++) {
		uint32_t cp = text[i];
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
			uint32_t low = text[i + 1];
			if (low >= 0xDC00 && low <= 0xDFFF) {
				cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
				i++;
			}
		}
		if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || cp == 0x200D || cp == 0xFE0F)
			return true;
	}
	return false;
}

enum class Align { Left, Center, Right };
enum class VAlign { Top, Center, Bottom };

struct DirectWriteTextSource {
	obs_source_t *source = nullptr;
	gs_texture_t *tex = nullptr;
	uint32_t cx = 0;
	uint32_t cy = 0;
	ComPtr<ID2D1Factory> d2d;
	ComPtr<IDWriteFactory> dwrite;
	ComPtr<IWICImagingFactory> wic;
	ComPtr<IWICBitmap> bitmap;
	ComPtr<ID2D1RenderTarget> target;
	ComPtr<IDWriteTextFormat> format;
	bool com_initialized = false;
	bool read_from_file = false;
	string file;
	time_t file_timestamp = 0;
	bool update_file = false;
	float update_time_elapsed = 0.0f;
	wstring text;
	wstring face;
	int face_size = 0;
	uint32_t color = 0xFFFFFF, color2 = 0xFFFFFF;
	float gradient_dir = 90.0f;
	uint32_t opacity = 100, opacity2 = 100;
	uint32_t bk_color = 0, bk_opacity = 0;
	Align align = Align::Left;
	VAlign valign = VAlign::Top;
	bool gradient = false, bold = false, italic = false, underline = false, strikeout = false, antialiasing = true;
	bool use_outline = false, use_extents = false, wrap = false, chatlog_mode = false;
	float outline_size = 0.0f;
	uint32_t outline_color = 0, outline_opacity = 100, extents_cx = 0, extents_cy = 0;
	int text_transform = S_TRANSFORM_NONE, chatlog_lines = 6;

	DirectWriteTextSource(obs_source_t *source_, obs_data_t *settings) : source(source_)
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		com_initialized = SUCCEEDED(hr);
		D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.Assign());
		DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(dwrite.Assign()));
		CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.Assign()));
		obs_source_update(source, settings);
	}

	~DirectWriteTextSource()
	{
		if (tex) {
			obs_enter_graphics();
			gs_texture_destroy(tex);
			obs_leave_graphics();
		}
		if (com_initialized)
			CoUninitialize();
	}

	const char *main_string(const char *str);
	void load_file_text();
	void transform_text();
	void update_format();
	void render_text();
	void update(obs_data_t *settings);
	void tick(float seconds);
	void render();
};

const char *DirectWriteTextSource::main_string(const char *str)
{
	if (!str)
		return "";
	if (!chatlog_mode || !chatlog_lines)
		return str;

	int lines = chatlog_lines;
	size_t len = strlen(str);
	const char *temp = str + len;
	while (temp != str) {
		temp--;
		if (temp[0] == '\n' && temp[1] != 0 && !--lines)
			break;
	}
	return *temp == '\n' ? temp + 1 : temp;
}

void DirectWriteTextSource::load_file_text()
{
	BPtr<char> file_text = os_quick_read_utf8_file(file.c_str());
	text = to_wide(main_string(file_text));
}

void DirectWriteTextSource::transform_text()
{
	if (text.empty())
		return;

	const locale loc = locale(obs_get_locale());
	const ctype<wchar_t> &f = use_facet<ctype<wchar_t>>(loc);
	if (text_transform == S_TRANSFORM_UPPERCASE) {
		f.toupper(&text[0], &text[0] + text.size());
	} else if (text_transform == S_TRANSFORM_LOWERCASE) {
		f.tolower(&text[0], &text[0] + text.size());
	} else if (text_transform == S_TRANSFORM_STARTCASE) {
		bool upper = true;
		for (wchar_t &ch : text) {
			const wchar_t upper_char = f.toupper(ch);
			const wchar_t lower_char = f.tolower(ch);
			if (upper && lower_char != upper_char) {
				ch = upper_char;
				upper = false;
			} else if (lower_char != upper_char) {
				ch = lower_char;
			} else {
				upper = iswspace(ch) != 0;
			}
		}
	}
}

void DirectWriteTextSource::update_format()
{
	format.Clear();
	if (!dwrite)
		return;

	const wchar_t *face_name = face.empty() ? L"Segoe UI" : face.c_str();
	dwrite->CreateTextFormat(face_name, nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
				 italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
				 face_size > 0 ? float(face_size) : 36.0f, L"", format.Assign());
}

static DWRITE_TEXT_ALIGNMENT dwrite_align(Align align)
{
	switch (align) {
	case Align::Center:
		return DWRITE_TEXT_ALIGNMENT_CENTER;
	case Align::Right:
		return DWRITE_TEXT_ALIGNMENT_TRAILING;
	default:
		return DWRITE_TEXT_ALIGNMENT_LEADING;
	}
}

static DWRITE_PARAGRAPH_ALIGNMENT dwrite_valign(VAlign valign)
{
	switch (valign) {
	case VAlign::Center:
		return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
	case VAlign::Bottom:
		return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
	default:
		return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
	}
}

static void apply_layout_options(IDWriteTextLayout *layout, size_t len, bool underline, bool strikeout, bool wrap, bool use_extents,
				 Align align, VAlign valign)
{
	if (!layout)
		return;
	DWRITE_TEXT_RANGE range = {0, (UINT32)len};
	layout->SetUnderline(underline, range);
	layout->SetStrikethrough(strikeout, range);
	layout->SetTextAlignment(dwrite_align(align));
	layout->SetParagraphAlignment(dwrite_valign(valign));
	layout->SetWordWrapping(wrap || !use_extents ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
}

static HRESULT ensure_target(DirectWriteTextSource &src, uint32_t width, uint32_t height)
{
	if (src.bitmap && src.target && src.cx == width && src.cy == height)
		return S_OK;

	src.bitmap.Clear();
	src.target.Clear();
	HRESULT hr = src.wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, src.bitmap.Assign());
	if (FAILED(hr))
		return hr;

	D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
	return src.d2d->CreateWicBitmapRenderTarget(src.bitmap, props, src.target.Assign());
}

static void upload_bitmap(DirectWriteTextSource &src, uint32_t width, uint32_t height)
{
	WICRect rect = {0, 0, (INT)width, (INT)height};
	ComPtr<IWICBitmapLock> lock;
	if (FAILED(src.bitmap->Lock(&rect, WICBitmapLockRead, lock.Assign())))
		return;

	UINT stride = 0, data_size = 0;
	BYTE *data = nullptr;
	lock->GetStride(&stride);
	lock->GetDataPointer(&data_size, &data);

	vector<uint8_t> pixels(width * height * 4);
	for (uint32_t y = 0; y < height; y++) {
		uint8_t *dst = pixels.data() + (size_t)y * width * 4;
		const uint8_t *row = data + (size_t)y * stride;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t b = row[x * 4 + 0];
			const uint8_t g = row[x * 4 + 1];
			const uint8_t r = row[x * 4 + 2];
			const uint8_t a = row[x * 4 + 3];
			dst[x * 4 + 0] = unpremultiply(b, a);
			dst[x * 4 + 1] = unpremultiply(g, a);
			dst[x * 4 + 2] = unpremultiply(r, a);
			dst[x * 4 + 3] = a;
		}
	}

	if (!src.tex || src.cx != width || src.cy != height) {
		obs_enter_graphics();
		if (src.tex)
			gs_texture_destroy(src.tex);
		const uint8_t *ptr = pixels.data();
		src.tex = gs_texture_create(width, height, GS_BGRA, 1, &ptr, GS_DYNAMIC);
		obs_leave_graphics();
	} else {
		obs_enter_graphics();
		gs_texture_set_image(src.tex, pixels.data(), width * 4, false);
		obs_leave_graphics();
	}

	src.cx = width;
	src.cy = height;
}

void DirectWriteTextSource::render_text()
{
	if (!format || !dwrite || !wic || !d2d)
		return;

	ComPtr<IDWriteTextLayout> layout;
	DWRITE_TEXT_METRICS metrics = {};
	const float max_width = use_extents ? float(extents_cx) : float(MAX_SIZE_CX);
	const float max_height = use_extents ? float(extents_cy) : float(MAX_SIZE_CY);
	if (FAILED(dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, max_width, max_height, layout.Assign())))
		return;

	apply_layout_options(layout, text.size(), underline, strikeout, wrap, use_extents, align, valign);
	layout->GetMetrics(&metrics);

	float outline_pad = use_outline ? outline_size : 0.0f;
	uint32_t width = use_extents ? extents_cx : (uint32_t)ceilf(metrics.widthIncludingTrailingWhitespace + outline_pad * 2.0f);
	uint32_t height = use_extents ? extents_cy : (uint32_t)ceilf(metrics.height + outline_pad * 2.0f);
	if (text.empty() && !use_extents) {
		width = MIN_SIZE_CX;
		height = MIN_SIZE_CY;
	}
	width += width % 2;
	height += height % 2;
	width = max<uint32_t>(MIN_SIZE_CX, min<uint32_t>(MAX_SIZE_CX, width));
	height = max<uint32_t>(MIN_SIZE_CY, min<uint32_t>(MAX_SIZE_CY, height));
	if ((int64_t)width * height > MAX_AREA) {
		if (width > height)
			width = (uint32_t)(MAX_AREA / height);
		else
			height = (uint32_t)(MAX_AREA / width);
	}

	if (FAILED(ensure_target(*this, width, height)))
		return;

	target->BeginDraw();
	target->SetTransform(D2D1::Matrix3x2F::Identity());
	target->SetTextAntialiasMode(antialiasing ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
	target->Clear(D2D1::ColorF(0, 0.0f));

	if (bk_opacity > 0 || text.empty()) {
		ComPtr<ID2D1SolidColorBrush> brush;
		target->CreateSolidColorBrush(to_d2d_color(bk_color, bk_opacity), brush.Assign());
		target->FillRectangle(D2D1::RectF(0.0f, 0.0f, (float)width, (float)height), brush);
	}

	if (!text.empty()) {
		ComPtr<ID2D1Brush> fill_brush;
		if (gradient && (color != color2 || opacity != opacity2)) {
			D2D1_GRADIENT_STOP stops[2] = {
				{0.0f, to_d2d_color(color, opacity)},
				{1.0f, to_d2d_color(color2, opacity2)},
			};
			ComPtr<ID2D1GradientStopCollection> stop_collection;
			target->CreateGradientStopCollection(stops, 2, stop_collection.Assign());
			float radians = gradient_dir * (float)M_PI / 180.0f;
			D2D1_POINT_2F center = D2D1::Point2F(width / 2.0f, height / 2.0f);
			D2D1_POINT_2F dir = D2D1::Point2F(cosf(radians) * width / 2.0f, -sinf(radians) * height / 2.0f);
			target->CreateLinearGradientBrush(
				D2D1::LinearGradientBrushProperties(D2D1::Point2F(center.x - dir.x, center.y - dir.y),
								    D2D1::Point2F(center.x + dir.x, center.y + dir.y)),
				stop_collection, reinterpret_cast<ID2D1LinearGradientBrush **>(fill_brush.Assign()));
		} else {
			target->CreateSolidColorBrush(to_d2d_color(color, opacity),
						      reinterpret_cast<ID2D1SolidColorBrush **>(fill_brush.Assign()));
		}

		if (use_outline && !has_emoji(text)) {
			ComPtr<ID2D1SolidColorBrush> outline_brush;
			target->CreateSolidColorBrush(to_d2d_color(outline_color, outline_opacity), outline_brush.Assign());
			static const D2D1_POINT_2F offsets[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
			for (const D2D1_POINT_2F &offset : offsets) {
				target->DrawTextLayout(D2D1::Point2F(outline_pad + offset.x * outline_size, outline_pad + offset.y * outline_size),
						       layout, outline_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
			}
		}

		D2D1_DRAW_TEXT_OPTIONS draw_options = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT;
		if (!antialiasing)
			draw_options = (D2D1_DRAW_TEXT_OPTIONS)(draw_options | D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
		target->DrawTextLayout(D2D1::Point2F(outline_pad, outline_pad), layout, fill_brush, draw_options);
	}

	target->EndDraw();
	upload_bitmap(*this, width, height);
}

void DirectWriteTextSource::update(obs_data_t *s)
{
	obs_data_t *font_obj = obs_data_get_obj(s, S_FONT);
	wstring new_face = to_wide(obs_data_get_string(font_obj, "face"));
	int font_size = (int)obs_data_get_int(font_obj, "size");
	int64_t font_flags = obs_data_get_int(font_obj, "flags");
	bool new_bold = (font_flags & OBS_FONT_BOLD) != 0;
	bool new_italic = (font_flags & OBS_FONT_ITALIC) != 0;
	bool new_underline = (font_flags & OBS_FONT_UNDERLINE) != 0;
	bool new_strikeout = (font_flags & OBS_FONT_STRIKEOUT) != 0;

	if (new_face != face || face_size != font_size || bold != new_bold || italic != new_italic ||
	    underline != new_underline || strikeout != new_strikeout) {
		face = new_face;
		face_size = font_size;
		bold = new_bold;
		italic = new_italic;
		underline = new_underline;
		strikeout = new_strikeout;
		update_format();
	}

	color = rgb_to_bgr((uint32_t)obs_data_get_int(s, S_COLOR));
	color2 = rgb_to_bgr((uint32_t)obs_data_get_int(s, S_GRADIENT_COLOR));
	opacity = (uint32_t)obs_data_get_int(s, S_OPACITY);
	opacity2 = (uint32_t)obs_data_get_int(s, S_GRADIENT_OPACITY);
	gradient = obs_data_get_bool(s, S_GRADIENT);
	gradient_dir = (float)obs_data_get_double(s, S_GRADIENT_DIR);
	if (!gradient) {
		color2 = color;
		opacity2 = opacity;
	}

	bk_color = rgb_to_bgr((uint32_t)obs_data_get_int(s, S_BKCOLOR));
	bk_opacity = (uint32_t)obs_data_get_int(s, S_BKOPACITY);
	use_outline = obs_data_get_bool(s, S_OUTLINE);
	outline_size = roundf((float)obs_data_get_int(s, S_OUTLINE_SIZE));
	outline_color = rgb_to_bgr((uint32_t)obs_data_get_int(s, S_OUTLINE_COLOR));
	outline_opacity = (uint32_t)obs_data_get_int(s, S_OUTLINE_OPACITY);
	read_from_file = obs_data_get_bool(s, S_USE_FILE);
	chatlog_mode = obs_data_get_bool(s, S_CHATLOG_MODE);
	chatlog_lines = (int)obs_data_get_int(s, S_CHATLOG_LINES);
	use_extents = obs_data_get_bool(s, S_EXTENTS);
	wrap = obs_data_get_bool(s, S_EXTENTS_WRAP);
	extents_cx = (uint32_t)obs_data_get_int(s, S_EXTENTS_CX);
	extents_cy = (uint32_t)obs_data_get_int(s, S_EXTENTS_CY);
	text_transform = (int)obs_data_get_int(s, S_TRANSFORM);
	antialiasing = obs_data_get_bool(s, S_ANTIALIASING);

	const char *align_str = obs_data_get_string(s, S_ALIGN);
	const char *valign_str = obs_data_get_string(s, S_VALIGN);
	align = strcmp(align_str, S_ALIGN_CENTER) == 0 ? Align::Center : strcmp(align_str, S_ALIGN_RIGHT) == 0 ? Align::Right : Align::Left;
	valign = strcmp(valign_str, S_VALIGN_CENTER) == 0 ? VAlign::Center
		: strcmp(valign_str, S_VALIGN_BOTTOM) == 0   ? VAlign::Bottom
							      : VAlign::Top;

	if (read_from_file) {
		file = obs_data_get_string(s, S_FILE);
		file_timestamp = get_modified_timestamp(file.c_str());
		load_file_text();
	} else {
		text = to_wide(main_string(obs_data_get_string(s, S_TEXT)));
	}

	transform_text();
	render_text();
	update_time_elapsed = 0.0f;
	obs_data_release(font_obj);
}

void DirectWriteTextSource::tick(float seconds)
{
	if (!read_from_file)
		return;
	update_time_elapsed += seconds;
	if (update_time_elapsed < 1.0f)
		return;

	update_time_elapsed = 0.0f;
	time_t modified = get_modified_timestamp(file.c_str());
	if (update_file) {
		load_file_text();
		transform_text();
		render_text();
		update_file = false;
	}
	if (modified != file_timestamp) {
		file_timestamp = modified;
		update_file = true;
	}
}

void DirectWriteTextSource::render()
{
	if (!tex)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_draw_sprite(tex, 0, cx, cy);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_enable_framebuffer_srgb(previous);
}

#define set_vis(var, val, show)                           \
	do {                                              \
		p = obs_properties_get(props, val);       \
		obs_property_set_visible(p, var == show); \
	} while (false)

static bool use_file_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	bool use_file = obs_data_get_bool(s, S_USE_FILE);
	set_vis(use_file, S_TEXT, false);
	set_vis(use_file, S_FILE, true);
	return true;
}

static bool outline_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	bool outline = obs_data_get_bool(s, S_OUTLINE);
	set_vis(outline, S_OUTLINE_SIZE, true);
	set_vis(outline, S_OUTLINE_COLOR, true);
	set_vis(outline, S_OUTLINE_OPACITY, true);
	return true;
}

static bool chatlog_mode_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	set_vis(obs_data_get_bool(s, S_CHATLOG_MODE), S_CHATLOG_LINES, true);
	return true;
}

static bool gradient_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	bool gradient = obs_data_get_bool(s, S_GRADIENT);
	set_vis(gradient, S_GRADIENT_COLOR, true);
	set_vis(gradient, S_GRADIENT_OPACITY, true);
	set_vis(gradient, S_GRADIENT_DIR, true);
	return true;
}

static bool extents_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	bool extents = obs_data_get_bool(s, S_EXTENTS);
	set_vis(extents, S_EXTENTS_WRAP, true);
	set_vis(extents, S_EXTENTS_CX, true);
	set_vis(extents, S_EXTENTS_CY, true);
	return true;
}

#undef set_vis

static obs_properties_t *get_properties(void *data)
{
	DirectWriteTextSource *src = reinterpret_cast<DirectWriteTextSource *>(data);
	string path;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	obs_properties_add_font(props, S_FONT, T_FONT);
	p = obs_properties_add_bool(props, S_USE_FILE, T_USE_FILE);
	obs_property_set_modified_callback(p, use_file_changed);

	string filter = string(T_FILTER_TEXT_FILES) + " (*.txt);;" + T_FILTER_ALL_FILES + " (*.*)";
	if (src && !src->file.empty()) {
		path = src->file;
		replace(path.begin(), path.end(), '\\', '/');
		const char *slash = strrchr(path.c_str(), '/');
		if (slash)
			path.resize(slash - path.c_str() + 1);
	}

	obs_properties_add_text(props, S_TEXT, T_TEXT, OBS_TEXT_MULTILINE);
	obs_properties_add_path(props, S_FILE, T_FILE, OBS_PATH_FILE, filter.c_str(), path.c_str());
	obs_properties_add_bool(props, S_ANTIALIASING, T_ANTIALIASING);

	p = obs_properties_add_list(props, S_TRANSFORM, T_TRANSFORM, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_TRANSFORM_NONE, S_TRANSFORM_NONE);
	obs_property_list_add_int(p, T_TRANSFORM_UPPERCASE, S_TRANSFORM_UPPERCASE);
	obs_property_list_add_int(p, T_TRANSFORM_LOWERCASE, S_TRANSFORM_LOWERCASE);
	obs_property_list_add_int(p, T_TRANSFORM_STARTCASE, S_TRANSFORM_STARTCASE);

	obs_properties_add_color(props, S_COLOR, T_COLOR);
	p = obs_properties_add_int_slider(props, S_OPACITY, T_OPACITY, 0, 100, 1);
	obs_property_int_set_suffix(p, "%");

	p = obs_properties_add_bool(props, S_GRADIENT, T_GRADIENT);
	obs_property_set_modified_callback(p, gradient_changed);
	obs_properties_add_color(props, S_GRADIENT_COLOR, T_GRADIENT_COLOR);
	p = obs_properties_add_int_slider(props, S_GRADIENT_OPACITY, T_GRADIENT_OPACITY, 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_float_slider(props, S_GRADIENT_DIR, T_GRADIENT_DIR, 0, 360, 0.1);

	obs_properties_add_color(props, S_BKCOLOR, T_BKCOLOR);
	p = obs_properties_add_int_slider(props, S_BKOPACITY, T_BKOPACITY, 0, 100, 1);
	obs_property_int_set_suffix(p, "%");

	p = obs_properties_add_list(props, S_ALIGN, T_ALIGN, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_ALIGN_LEFT, S_ALIGN_LEFT);
	obs_property_list_add_string(p, T_ALIGN_CENTER, S_ALIGN_CENTER);
	obs_property_list_add_string(p, T_ALIGN_RIGHT, S_ALIGN_RIGHT);

	p = obs_properties_add_list(props, S_VALIGN, T_VALIGN, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_VALIGN_TOP, S_VALIGN_TOP);
	obs_property_list_add_string(p, T_VALIGN_CENTER, S_VALIGN_CENTER);
	obs_property_list_add_string(p, T_VALIGN_BOTTOM, S_VALIGN_BOTTOM);

	p = obs_properties_add_bool(props, S_OUTLINE, T_OUTLINE);
	obs_property_set_modified_callback(p, outline_changed);
	obs_properties_add_int(props, S_OUTLINE_SIZE, T_OUTLINE_SIZE, 1, 20, 1);
	obs_properties_add_color(props, S_OUTLINE_COLOR, T_OUTLINE_COLOR);
	p = obs_properties_add_int_slider(props, S_OUTLINE_OPACITY, T_OUTLINE_OPACITY, 0, 100, 1);
	obs_property_int_set_suffix(p, "%");

	p = obs_properties_add_bool(props, S_CHATLOG_MODE, T_CHATLOG_MODE);
	obs_property_set_modified_callback(p, chatlog_mode_changed);
	obs_properties_add_int(props, S_CHATLOG_LINES, T_CHATLOG_LINES, 1, 1000, 1);

	p = obs_properties_add_bool(props, S_EXTENTS, T_EXTENTS);
	obs_property_set_modified_callback(p, extents_changed);
	obs_properties_add_int(props, S_EXTENTS_CX, T_EXTENTS_CX, 32, 8000, 1);
	obs_properties_add_int(props, S_EXTENTS_CY, T_EXTENTS_CY, 32, 8000, 1);
	obs_properties_add_bool(props, S_EXTENTS_WRAP, T_EXTENTS_WRAP);
	return props;
}

static void defaults(obs_data_t *settings)
{
	obs_data_t *font_obj = obs_data_create();
	obs_data_set_default_string(font_obj, "face", "Segoe UI");
	obs_data_set_default_int(font_obj, "size", 256);
	obs_data_set_default_obj(settings, S_FONT, font_obj);
	obs_data_set_default_string(settings, S_ALIGN, S_ALIGN_LEFT);
	obs_data_set_default_string(settings, S_VALIGN, S_VALIGN_TOP);
	obs_data_set_default_int(settings, S_COLOR, 0xFFFFFF);
	obs_data_set_default_int(settings, S_OPACITY, 100);
	obs_data_set_default_int(settings, S_GRADIENT_COLOR, 0xFFFFFF);
	obs_data_set_default_int(settings, S_GRADIENT_OPACITY, 100);
	obs_data_set_default_double(settings, S_GRADIENT_DIR, 90.0);
	obs_data_set_default_int(settings, S_BKCOLOR, 0x000000);
	obs_data_set_default_int(settings, S_BKOPACITY, 0);
	obs_data_set_default_int(settings, S_OUTLINE_SIZE, 2);
	obs_data_set_default_int(settings, S_OUTLINE_COLOR, 0xFFFFFF);
	obs_data_set_default_int(settings, S_OUTLINE_OPACITY, 100);
	obs_data_set_default_int(settings, S_CHATLOG_LINES, 6);
	obs_data_set_default_bool(settings, S_EXTENTS_WRAP, true);
	obs_data_set_default_int(settings, S_EXTENTS_CX, 100);
	obs_data_set_default_int(settings, S_EXTENTS_CY, 100);
	obs_data_set_default_int(settings, S_TRANSFORM, S_TRANSFORM_NONE);
	obs_data_set_default_bool(settings, S_ANTIALIASING, true);
	obs_data_release(font_obj);
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	DirectWriteTextSource *s = reinterpret_cast<DirectWriteTextSource *>(src);
	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_set_string(settings, S_FILE, new_path);
	obs_source_update(s->source, settings);
	obs_data_release(settings);
	UNUSED_PARAMETER(data);
}

void register_directwrite_source()
{
	obs_source_info si = {};
	si.id = "text_directwrite";
	si.type = OBS_SOURCE_TYPE_INPUT;
	si.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	si.get_properties = get_properties;
	si.icon_type = OBS_ICON_TYPE_TEXT;
	si.get_name = [](void *) { return obs_module_text("TextDirectWrite"); };
	si.create = [](obs_data_t *settings, obs_source_t *source) { return (void *)new DirectWriteTextSource(source, settings); };
	si.destroy = [](void *data) { delete reinterpret_cast<DirectWriteTextSource *>(data); };
	si.get_width = [](void *data) { return reinterpret_cast<DirectWriteTextSource *>(data)->cx; };
	si.get_height = [](void *data) { return reinterpret_cast<DirectWriteTextSource *>(data)->cy; };
	si.get_defaults = [](obs_data_t *settings) { defaults(settings); };
	si.update = [](void *data, obs_data_t *settings) { reinterpret_cast<DirectWriteTextSource *>(data)->update(settings); };
	si.video_tick = [](void *data, float seconds) { reinterpret_cast<DirectWriteTextSource *>(data)->tick(seconds); };
	si.video_render = [](void *data, gs_effect_t *) { reinterpret_cast<DirectWriteTextSource *>(data)->render(); };
	si.missing_files = [](void *data) {
		DirectWriteTextSource *s = reinterpret_cast<DirectWriteTextSource *>(data);
		obs_missing_files_t *files = obs_missing_files_create();
		obs_data_t *settings = obs_source_get_settings(s->source);
		bool read = obs_data_get_bool(settings, S_USE_FILE);
		const char *path = obs_data_get_string(settings, S_FILE);
		if (read && strcmp(path, "") != 0 && !os_file_exists(path)) {
			obs_missing_file_t *file = obs_missing_file_create(path, missing_file_callback, OBS_MISSING_FILE_SOURCE, s->source, NULL);
			obs_missing_files_add_file(files, file);
		}
		obs_data_release(settings);
		return files;
	};
	obs_register_source(&si);
}
