/******************************************************************************
    Copyright (C) 2026 by OBS Project contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSOSD.hpp"

#include <OBSApp.hpp>
#include <obs-audio-controls.h>
#include <obs.h>
#include <obs.hpp>
#include <qt-wrappers.hpp>
#include <util/platform.h>

#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <graphics-hook-info.h>
#endif

namespace {
constexpr int MaxSources = 8;
constexpr int RefreshIntervalMs = 250;

static QString ToQString(const char *text)
{
	return text ? QT_UTF8(text) : QString();
}

static OBSOSDSourceKind SourceKindFromId(const char *id)
{
	if (!id)
		return OBSOSDSourceKind::Default;
	if (strstr(id, "game_capture"))
		return OBSOSDSourceKind::Game;
	if (strstr(id, "monitor_capture") || strstr(id, "display_capture"))
		return OBSOSDSourceKind::Display;
	if (strstr(id, "dshow") || strstr(id, "camera") || strstr(id, "video_capture"))
		return OBSOSDSourceKind::Camera;
	if (strstr(id, "audio"))
		return OBSOSDSourceKind::Audio;
	if (strcmp(id, "scene") == 0)
		return OBSOSDSourceKind::Scene;
	if (strstr(id, "browser_source") || strstr(id, "browser"))
		return OBSOSDSourceKind::Browser;
	return OBSOSDSourceKind::Default;
}

static bool EnumSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto &sources = *static_cast<std::vector<OBSOSDSourceRow> *>(data);
	if (sources.size() >= MaxSources)
		return false;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	OBSOSDSourceRow row;
	row.name = ToQString(obs_source_get_name(source));
	row.kind = SourceKindFromId(obs_source_get_unversioned_id(source));
	row.visible = obs_sceneitem_visible(item);
	sources.emplace_back(std::move(row));
	return true;
}

#ifdef _WIN32
struct HookedGameCaptureSearch {
	uint32_t processId = 0;
	QString name;
};

static bool EnumHookedGameCapture(void *data, obs_source_t *source)
{
	auto *search = static_cast<HookedGameCaptureSearch *>(data);
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || strcmp(id, "game_capture") != 0)
		return true;

	proc_handler_t *proc = obs_source_get_proc_handler(source);
	if (!proc)
		return true;

	calldata_t cd = {};
	calldata_init(&cd);
	bool called = proc_handler_call(proc, "get_hooked_pid", &cd);
	bool hooked = called && calldata_bool(&cd, "hooked");
	uint32_t processId = (uint32_t)calldata_int(&cd, "process_id");
	calldata_free(&cd);

	if (!hooked || !processId)
		return true;

	search->processId = processId;
	search->name = ToQString(obs_source_get_name(source));
	return false;
}

static std::wstring MakeHookInfoName(uint32_t processId)
{
	wchar_t name[64];
	swprintf(name, _countof(name), SHMEM_HOOK_INFO L"%lu", (unsigned long)processId);
	return name;
}

static std::wstring MakeOSDName(uint32_t processId, uint32_t mapId)
{
	wchar_t name[64];
	swprintf(name, _countof(name), SHMEM_OSD L"%lu_%u", (unsigned long)processId, mapId);
	return name;
}

static uint32_t AnchorToHookValue(const QString &anchor)
{
	if (anchor == QStringLiteral("top-left"))
		return 1;
	if (anchor == QStringLiteral("bottom-right"))
		return 2;
	if (anchor == QStringLiteral("bottom-left"))
		return 3;
	return 0;
}

static const char *HookChoiceName(uint32_t flags, const QString &backend, bool published)
{
	if (!published)
		return "window";
	if ((flags & OSD_HOOK_COMPOSITOR_FAILED) != 0)
		return "hook-failed";
	if ((flags & OSD_HOOK_COMPOSITOR_READY) != 0)
		return "hook-ready";
	return backend == QStringLiteral("hook") ? "hook-forced-waiting" : "hook-waiting";
}
#endif
} // namespace

class OBSOSDWindow final : public QWidget {
public:
	OBSOSDWindow()
	{
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_AlwaysStackOnTop);
		setWindowFlag(Qt::Tool, true);
		setWindowFlag(Qt::FramelessWindowHint, true);
		setWindowFlag(Qt::WindowStaysOnTopHint, true);
		setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
		setWindowFlag(Qt::NoDropShadowWindowHint, true);
		setFocusPolicy(Qt::NoFocus);
		setMouseTracking(false);

		setMinimumSize(280, 120);
		resize(320, 162);
	}

	void SetModel(const OBSOSDModel &newModel, const OBSOSDConfig &newConfig)
	{
		model = newModel;
		config = newConfig;
		setWindowOpacity(config.opacity);
		UpdateGeometry();
		update();
	}

	QImage RenderToImage(const OBSOSDModel &newModel, const OBSOSDConfig &newConfig)
	{
		SetModel(newModel, newConfig);
		QImage image(size(), QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		render(&painter);
		ApplyImageOpacity(image, newConfig.opacity);
		return image;
	}

	void UpdateGeometry()
	{
		QScreen *screen = QGuiApplication::primaryScreen();
		if (!screen)
			return;

		const QRect available = screen->availableGeometry();
		const QSize base = BaseSize();
		const int width = int(base.width() * config.scale);
		const int height = int(base.height() * config.scale);
		const int margin = int(24 * config.scale);

		QPoint pos = available.topRight() - QPoint(width + margin, -margin);
		if (config.anchor == QStringLiteral("top-left")) {
			pos = available.topLeft() + QPoint(margin, margin);
		} else if (config.anchor == QStringLiteral("bottom-left")) {
			pos = available.bottomLeft() + QPoint(margin, -height - margin);
		} else if (config.anchor == QStringLiteral("bottom-right")) {
			pos = available.bottomRight() - QPoint(width + margin, height + margin);
		}

		setGeometry(QRect(pos, QSize(width, height)));

#ifdef _WIN32
		HWND hwnd = reinterpret_cast<HWND>(winId());
		LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
		exStyle |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
		SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
#endif
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setRenderHint(QPainter::TextAntialiasing);

		const qreal s = qMax(0.5, config.scale);
		const int pad = int(18 * s);
		const QRectF panel = rect().adjusted(0, 0, -1, -1);

		DrawPanel(painter, panel, s);

		QFont titleFont = font();
		titleFont.setPointSizeF(11.5 * s);
		titleFont.setBold(true);
		QFont bodyFont = font();
		bodyFont.setPointSizeF(10 * s);

		int y = pad;
		if (config.showStatus) {
			DrawStatusRail(painter, QRect(pad, y, width() - pad * 2, int(34 * s)), s);
			y += int(48 * s);
			DrawDivider(painter, y - int(12 * s), pad, width(), s);
		}

		if (config.showScenes) {
			painter.setFont(titleFont);
			DrawRowIcon(painter, QRect(pad, y, int(20 * s), int(20 * s)), OBSOSDSourceKind::Scene,
				    QColor(238, 241, 247), s);
			painter.setPen(QColor(244, 246, 251));
			painter.drawText(QRect(pad + int(30 * s), y - int(1 * s), width() - pad * 2 - int(30 * s),
					       int(24 * s)),
					 Qt::AlignVCenter | Qt::AlignLeft,
					 Elide(model.currentScene, titleFont, width() - pad * 2 - int(34 * s)));
			y += int(32 * s);
		}

		painter.setFont(bodyFont);
		if (config.showSources && !model.sources.empty() && config.layoutMode != QStringLiteral("collapsed")) {
			if (config.layoutMode == QStringLiteral("expanded")) {
				DrawDivider(painter, y - int(7 * s), pad, width(), s);
			} else {
				y -= int(1 * s);
			}
			for (int i = 0; i < SourceRowsToDraw(); i++) {
				if (y > height() - int((config.showBackendBadge ? 34 : 16) * s))
					break;
				DrawSourceRow(painter, model.sources[size_t(i)], y, bodyFont, s);
				y += int(26 * s);
			}
		}

		if (config.showBackendBadge)
			DrawBackendBadge(painter, s, pad);
	}

private:
	OBSOSDModel model;
	OBSOSDConfig config;

	QSize BaseSize() const
	{
		const int rows = SourceRowsToDraw();
		if (config.layoutMode == QStringLiteral("expanded"))
			return QSize(320, qBound(188, 140 + rows * 26, 360));
		if (config.layoutMode == QStringLiteral("collapsed"))
			return QSize(320, 132);
		return QSize(320, qBound(164, 140 + rows * 26, 244));
	}

	int SourceRowsToDraw() const
	{
		if (!config.showSources || config.layoutMode == QStringLiteral("collapsed"))
			return 0;

		const int count = int(model.sources.size());
		if (config.layoutMode == QStringLiteral("expanded"))
			return qMin(count, 8);
		return qMin(count, 4);
	}

	static QString Elide(const QString &text, const QFont &font, int width)
	{
		return QFontMetrics(font).elidedText(text, Qt::ElideRight, width);
	}

	static void ApplyImageOpacity(QImage &image, double opacity)
	{
		const int factor = qRound(qBound(0.0, opacity, 1.0) * 256.0);
		if (factor >= 256)
			return;

		for (int y = 0; y < image.height(); y++) {
			uchar *pixel = image.scanLine(y);
			for (int x = 0; x < image.width(); x++, pixel += 4) {
				pixel[0] = uchar((int(pixel[0]) * factor + 128) >> 8);
				pixel[1] = uchar((int(pixel[1]) * factor + 128) >> 8);
				pixel[2] = uchar((int(pixel[2]) * factor + 128) >> 8);
				pixel[3] = uchar((int(pixel[3]) * factor + 128) >> 8);
			}
		}
	}

	static void DrawPanel(QPainter &painter, const QRectF &panel, qreal s)
	{
		QPainterPath path;
		path.addRoundedRect(panel, 8 * s, 8 * s);

		QLinearGradient fill(panel.topLeft(), panel.bottomRight());
		fill.setColorAt(0.0, QColor(17, 18, 24, 235));
		fill.setColorAt(0.55, QColor(10, 12, 17, 226));
		fill.setColorAt(1.0, QColor(16, 19, 27, 232));
		painter.fillPath(path, fill);

		painter.setPen(QPen(QColor(255, 82, 85, 90), 1.1 * s));
		painter.drawLine(panel.topLeft() + QPointF(14 * s, 0), panel.topLeft() + QPointF(76 * s, 0));
		painter.setPen(QPen(QColor(112, 149, 255, 95), 1.1 * s));
		painter.drawLine(panel.topRight() - QPointF(78 * s, 0), panel.topRight() - QPointF(14 * s, 0));

		painter.setPen(QPen(QColor(210, 218, 236, 72), 1));
		painter.drawPath(path);
	}

	static void DrawDivider(QPainter &painter, int y, int pad, int widgetWidth, qreal s)
	{
		painter.setPen(QPen(QColor(180, 190, 210, 38), 1));
		painter.drawLine(QPointF(pad, y), QPointF(widgetWidth - pad, y));
		UNUSED_PARAMETER(s);
	}

	void DrawStatusRail(QPainter &painter, const QRect &rect, qreal s) const
	{
		const int gap = int(52 * s);
		int x = rect.left() + int(34 * s);
		DrawStatusIcon(painter, QPointF(x, rect.center().y()), StatusIcon::Stream, model.streaming,
			       QColor(255, 75, 75), s);
		x += gap;
		DrawStatusIcon(painter, QPointF(x, rect.center().y()), StatusIcon::Record,
			       model.recording || model.recordingPaused, QColor(255, 75, 75), s);
		x += gap;
		DrawStatusIcon(painter, QPointF(x, rect.center().y()), StatusIcon::Camera, model.virtualCam,
			       QColor(102, 137, 255), s);
	}

	enum class StatusIcon { Stream, Record, Camera };

	static void DrawStatusIcon(QPainter &painter, QPointF center, StatusIcon icon, bool active, QColor activeColor,
				   qreal s)
	{
		const QColor color = active ? activeColor : QColor(103, 110, 124);
		if (active) {
			QRadialGradient glow(center, 22 * s);
			glow.setColorAt(0.0, QColor(activeColor.red(), activeColor.green(), activeColor.blue(), 92));
			glow.setColorAt(1.0, QColor(activeColor.red(), activeColor.green(), activeColor.blue(), 0));
			painter.setPen(Qt::NoPen);
			painter.setBrush(glow);
			painter.drawEllipse(center, 22 * s, 22 * s);
		}

		painter.setPen(QPen(color, 2.2 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.setBrush(Qt::NoBrush);
		const QRectF r(center.x() - 11 * s, center.y() - 11 * s, 22 * s, 22 * s);
		if (icon == StatusIcon::Record) {
			painter.drawEllipse(center, 7 * s, 7 * s);
			if (active)
				painter.setBrush(color), painter.drawEllipse(center, 3.2 * s, 3.2 * s);
		} else if (icon == StatusIcon::Camera) {
			painter.drawRoundedRect(r.adjusted(2 * s, 5 * s, -2 * s, -4 * s), 3 * s, 3 * s);
			painter.drawEllipse(center, 3.2 * s, 3.2 * s);
			painter.drawLine(r.right() - 1 * s, center.y() - 3 * s, r.right() + 5 * s, center.y() - 6 * s);
			painter.drawLine(r.right() - 1 * s, center.y() + 3 * s, r.right() + 5 * s, center.y() + 6 * s);
		} else {
			painter.drawEllipse(center, 2.8 * s, 2.8 * s);
			QPainterPath leftInner;
			leftInner.moveTo(center.x() - 5 * s, center.y() - 6 * s);
			leftInner.cubicTo(center.x() - 9 * s, center.y() - 3 * s, center.x() - 9 * s, center.y() + 3 * s,
					  center.x() - 5 * s, center.y() + 6 * s);
			QPainterPath leftOuter;
			leftOuter.moveTo(center.x() - 9 * s, center.y() - 10 * s);
			leftOuter.cubicTo(center.x() - 15 * s, center.y() - 4 * s, center.x() - 15 * s, center.y() + 4 * s,
					  center.x() - 9 * s, center.y() + 10 * s);
			QPainterPath rightInner;
			rightInner.moveTo(center.x() + 5 * s, center.y() - 6 * s);
			rightInner.cubicTo(center.x() + 9 * s, center.y() - 3 * s, center.x() + 9 * s, center.y() + 3 * s,
					   center.x() + 5 * s, center.y() + 6 * s);
			QPainterPath rightOuter;
			rightOuter.moveTo(center.x() + 9 * s, center.y() - 10 * s);
			rightOuter.cubicTo(center.x() + 15 * s, center.y() - 4 * s, center.x() + 15 * s, center.y() + 4 * s,
					   center.x() + 9 * s, center.y() + 10 * s);
			painter.drawPath(leftInner);
			painter.drawPath(leftOuter);
			painter.drawPath(rightInner);
			painter.drawPath(rightOuter);
		}
	}

	void DrawSourceRow(QPainter &painter, const OBSOSDSourceRow &source, int y, const QFont &font, qreal s) const
	{
		const int pad = int(18 * s);
		DrawRowIcon(painter, QRect(pad, y, int(20 * s), int(20 * s)), source.kind,
			    source.visible ? QColor(170, 180, 198) : QColor(93, 99, 112), s);
		painter.setFont(font);
		painter.setPen(source.visible ? QColor(180, 188, 204) : QColor(104, 110, 122));
		const int rightReserve = int(24 * s);
		painter.drawText(QRect(pad + int(30 * s), y - int(1 * s), width() - pad * 2 - int(30 * s) - rightReserve,
				       int(22 * s)),
				 Qt::AlignVCenter | Qt::AlignLeft,
				 Elide(source.name, font, width() - pad * 2 - int(34 * s) - rightReserve));
		DrawEye(painter, QPointF(width() - pad - int(10 * s), y + int(10 * s)), source.visible, s);
	}

	static void DrawRowIcon(QPainter &painter, const QRect &rect, OBSOSDSourceKind kind, QColor color, qreal s)
	{
		painter.setPen(QPen(color, 1.6 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.setBrush(Qt::NoBrush);
		const QRectF r = rect.adjusted(2 * s, 2 * s, -2 * s, -2 * s);
		if (kind == OBSOSDSourceKind::Game) {
			painter.drawRoundedRect(r.adjusted(0, 5 * s, 0, -1 * s), 4 * s, 4 * s);
			painter.drawEllipse(r.left() + 4 * s, r.center().y() + 3 * s, 1.2 * s, 1.2 * s);
			painter.drawEllipse(r.right() - 4 * s, r.center().y() + 3 * s, 1.2 * s, 1.2 * s);
			painter.drawLine(r.left() + 4 * s, r.center().y(), r.left() + 8 * s, r.center().y());
			painter.drawLine(r.left() + 6 * s, r.center().y() - 2 * s, r.left() + 6 * s, r.center().y() + 2 * s);
		} else if (kind == OBSOSDSourceKind::Display) {
			painter.drawRect(r.adjusted(1 * s, 2 * s, -1 * s, -5 * s));
			painter.drawLine(r.center().x(), r.bottom() - 4 * s, r.center().x(), r.bottom());
			painter.drawLine(r.center().x() - 5 * s, r.bottom(), r.center().x() + 5 * s, r.bottom());
		} else if (kind == OBSOSDSourceKind::Camera) {
			painter.drawRoundedRect(r.adjusted(1 * s, 4 * s, -2 * s, -3 * s), 3 * s, 3 * s);
			painter.drawEllipse(r.center(), 2.5 * s, 2.5 * s);
			painter.drawLine(r.right() - 1 * s, r.center().y() - 2 * s, r.right() + 4 * s, r.center().y() - 5 * s);
			painter.drawLine(r.right() - 1 * s, r.center().y() + 2 * s, r.right() + 4 * s, r.center().y() + 5 * s);
		} else if (kind == OBSOSDSourceKind::Audio) {
			painter.drawLine(r.center().x(), r.top() + 2 * s, r.center().x(), r.bottom() - 5 * s);
			painter.drawRoundedRect(QRectF(r.center().x() - 4 * s, r.top(), 8 * s, 12 * s), 4 * s, 4 * s);
			painter.drawLine(r.center().x() - 5 * s, r.bottom() - 4 * s, r.center().x() + 5 * s,
					 r.bottom() - 4 * s);
			painter.drawLine(r.center().x(), r.bottom() - 4 * s, r.center().x(), r.bottom());
		} else if (kind == OBSOSDSourceKind::Browser) {
			painter.drawRoundedRect(r.adjusted(1 * s, 3 * s, -1 * s, -2 * s), 2 * s, 2 * s);
			painter.drawLine(r.left() + 2 * s, r.top() + 7 * s, r.right() - 2 * s, r.top() + 7 * s);
			painter.drawEllipse(QPointF(r.left() + 4 * s, r.top() + 5 * s), 0.7 * s, 0.7 * s);
			painter.drawEllipse(QPointF(r.left() + 7 * s, r.top() + 5 * s), 0.7 * s, 0.7 * s);
		} else {
			painter.drawRect(r.adjusted(1 * s, 4 * s, -1 * s, -1 * s));
			painter.drawLine(r.left() + 3 * s, r.top() + 4 * s, r.right() - 2 * s, r.top() + 4 * s);
			painter.drawLine(r.left() + 5 * s, r.top(), r.left() + 8 * s, r.top() + 4 * s);
			painter.drawLine(r.left() + 11 * s, r.top(), r.left() + 14 * s, r.top() + 4 * s);
		}
	}

	static void DrawEye(QPainter &painter, QPointF center, bool visible, qreal s)
	{
		QColor color = visible ? QColor(142, 153, 173) : QColor(86, 93, 107);
		painter.setPen(QPen(color, 1.5 * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.setBrush(Qt::NoBrush);
		QPainterPath eye;
		eye.moveTo(center.x() - 8 * s, center.y());
		eye.quadTo(center.x(), center.y() - 7 * s, center.x() + 8 * s, center.y());
		eye.quadTo(center.x(), center.y() + 7 * s, center.x() - 8 * s, center.y());
		painter.drawPath(eye);
		if (visible)
			painter.drawEllipse(center, 2.3 * s, 2.3 * s);
	}

	void DrawBackendBadge(QPainter &painter, qreal scale, int pad) const
	{
		const QString label = config.activeBackend.isEmpty() ? QStringLiteral("Window") : config.activeBackend;
		QFont badgeFont = font();
		badgeFont.setPointSizeF(8 * scale);
		badgeFont.setBold(true);

		QFontMetrics metrics(badgeFont);
		const int badgeHeight = int(20 * scale);
		const int badgeWidth = qMax(int(72 * scale), metrics.horizontalAdvance(label) + int(18 * scale));
		const QRect badge(width() - pad - badgeWidth, height() - pad - badgeHeight, badgeWidth, badgeHeight);
		const bool hook = label.contains(QStringLiteral("hook"), Qt::CaseInsensitive);

		painter.setFont(badgeFont);
		painter.setPen(QPen(hook ? QColor(116, 214, 146) : QColor(108, 177, 255), 1));
		painter.setBrush(hook ? QColor(20, 80, 45, 225) : QColor(24, 58, 96, 225));
		painter.drawRoundedRect(badge, badgeHeight / 2.0, badgeHeight / 2.0);
		painter.setPen(QColor(245, 248, 252));
		painter.drawText(badge, Qt::AlignCenter, label.toUpper());
	}
};

OBSOSDController::OBSOSDController(QObject *parent) : QObject(parent), window(new OBSOSDWindow)
{
	refreshTimer = std::make_unique<QTimer>();
	refreshTimer->setInterval(RefreshIntervalMs);
	QObject::connect(refreshTimer.get(), &QTimer::timeout, this, [this]() { Refresh(); });
	ReloadSettings();
}

OBSOSDController::~OBSOSDController()
{
	Shutdown();
}

void OBSOSDController::ReloadSettings()
{
	config_t *userConfig = App()->GetUserConfig();
	config.enabled = config_get_bool(userConfig, "OSD", "Enabled");
	config.showStatus = config_get_bool(userConfig, "OSD", "ShowStatus");
	config.showScenes = config_get_bool(userConfig, "OSD", "ShowScenes");
	config.showSources = config_get_bool(userConfig, "OSD", "ShowSources");
	config.showBackendBadge = config_get_bool(userConfig, "OSD", "ShowBackendBadge");
	config.opacity = config_get_double(userConfig, "OSD", "Opacity");
	config.scale = config_get_double(userConfig, "OSD", "Scale");
	config.anchor = ToQString(config_get_string(userConfig, "OSD", "Anchor"));
	config.backend = ToQString(config_get_string(userConfig, "OSD", "Backend"));
	config.layoutMode = ToQString(config_get_string(userConfig, "OSD", "LayoutMode"));

	if (config.anchor.isEmpty())
		config.anchor = QStringLiteral("top-right");
	if (config.backend.isEmpty())
		config.backend = QStringLiteral("auto");
	if (config.layoutMode.isEmpty())
		config.layoutMode = QStringLiteral("compact");

	UpdateWindowVisibility();
	Refresh();
}

void OBSOSDController::HandleFrontendEvent(enum obs_frontend_event)
{
	Refresh();
}

void OBSOSDController::Shutdown()
{
	if (refreshTimer)
		refreshTimer->stop();
	if (window)
		window->hide();
	DisableHookBackend("shutdown");
	delete window;
	window = nullptr;
}

OBSOSDModel OBSOSDController::BuildModel() const
{
	OBSOSDModel model;
	model.streaming = obs_frontend_streaming_active();
	model.recording = obs_frontend_recording_active();
	model.recordingPaused = obs_frontend_recording_paused();
	model.virtualCam = obs_frontend_virtualcam_active();

	OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
	if (currentScene) {
		model.currentScene = ToQString(obs_source_get_name(currentScene));
		obs_scene_t *scene = obs_scene_from_source(currentScene);
		if (scene)
			obs_scene_enum_items(scene, EnumSceneItem, &model.sources);
	} else {
		model.currentScene = QStringLiteral("No active scene");
	}

	return model;
}

void OBSOSDController::Refresh()
{
	if (!config.enabled || !window) {
		DisableHookBackend("disabled");
		return;
	}

	OBSOSDModel model = BuildModel();
	const bool hookActive = config.backend != QStringLiteral("window") && UpdateHookBackend(model);

	if (hookActive) {
		window->hide();
	} else {
		OBSOSDConfig windowConfig = config;
		windowConfig.activeBackend = QStringLiteral("Window");
		window->SetModel(model, windowConfig);
		if (config.backend != QStringLiteral("hook"))
			window->show();
	}
}

void OBSOSDController::UpdateWindowVisibility()
{
	if (!window)
		return;

	if (!config.enabled) {
		refreshTimer->stop();
		window->hide();
		return;
	}

	if (!refreshTimer->isActive())
		refreshTimer->start();

	if (config.backend != QStringLiteral("hook"))
		window->show();
}

bool OBSOSDController::UpdateHookBackend(const OBSOSDModel &model)
{
#ifndef _WIN32
	UNUSED_PARAMETER(model);
	return false;
#else
	uint32_t processId = 0;
	QString sourceName;
	if (!FindHookedGameCapture(processId, sourceName)) {
		DisableHookBackend("no hooked Game Capture source");
		return false;
	}

	if (!EnsureHookMapping(processId, sourceName))
		return false;

	if (!PublishHookPayload(model))
		return false;

	const char *choice = HookChoiceName(hookInfo->osd_flags, config.backend, true);
	const uint64_t now = os_gettime_ns();
	const bool choiceChanged = hookLastChoice != QString::fromUtf8(choice);
	if (choiceChanged) {
		blog(LOG_INFO,
		     "OBS OSD hook backend selected: %s (process %u, map=%u, seq=%u, size=%ux%u)", choice, processId,
		     hookInfo->osd_map_id, hookInfo->osd_sequence, hookInfo->osd_width, hookInfo->osd_height);
		hookLastChoice = QString::fromUtf8(choice);
	}

	if ((hookInfo->osd_flags & OSD_HOOK_COMPOSITOR_FAILED) != 0) {
		if (config.backend == QStringLiteral("hook"))
			blog(LOG_WARNING, "OBS OSD hook compositor failed for process %u", processId);
		hookWaitingSinceTime = 0;
		return false;
	}

	if ((hookInfo->osd_flags & OSD_HOOK_COMPOSITOR_READY) == 0 && config.backend != QStringLiteral("hook")) {
		if (!hookWaitingSinceTime)
			hookWaitingSinceTime = now;
		else if (!hookWaitingWarningLogged && now - hookWaitingSinceTime >= 2000000000ULL) {
			blog(LOG_WARNING,
			     "OBS OSD hook compositor has not reported ready/failed after 2 seconds: process=%u map=%u "
			     "seq=%u",
			     processId, hookInfo->osd_map_id, hookInfo->osd_sequence);
			hookWaitingWarningLogged = true;
		}
		return false;
	}

	hookWaitingSinceTime = 0;
	hookWaitingWarningLogged = false;
	return true;
#endif
}

void OBSOSDController::DisableHookBackend(const char *reason)
{
#ifdef _WIN32
	if (hookInfo) {
		hookInfo->osd_flags = 0;
		hookInfo->osd_width = 0;
		hookInfo->osd_height = 0;
		hookInfo->osd_pitch = 0;
		hookInfo->osd_map_id = 0;
		hookInfo->osd_sequence = 0;
	}

	CloseHookPayloadMapping();
	if (hookInfo) {
		UnmapViewOfFile(hookInfo);
		hookInfo = nullptr;
	}
	if (hookInfoHandle) {
		CloseHandle((HANDLE)hookInfoHandle);
		hookInfoHandle = nullptr;
	}

	if (hookProcessId != 0)
		blog(LOG_INFO, "OBS OSD hook backend disabled for process %u: %s", hookProcessId, reason);

	hookProcessId = 0;
	osdMapId = 0;
	osdSequence = 0;
	osdMapSize = 0;
	hookWaitingSinceTime = 0;
	hookWaitingWarningLogged = false;
	hookLastChoice.clear();
#else
	UNUSED_PARAMETER(reason);
#endif
}

bool OBSOSDController::FindHookedGameCapture(uint32_t &processId, QString &name) const
{
#ifndef _WIN32
	UNUSED_PARAMETER(processId);
	UNUSED_PARAMETER(name);
	return false;
#else
	HookedGameCaptureSearch search;
	obs_enum_sources(EnumHookedGameCapture, &search);
	if (!search.processId)
		return false;

	processId = search.processId;
	name = search.name;
	return true;
#endif
}

bool OBSOSDController::EnsureHookMapping(uint32_t processId, const QString &sourceName)
{
#ifndef _WIN32
	UNUSED_PARAMETER(processId);
	UNUSED_PARAMETER(sourceName);
	return false;
#else
	if (hookInfo && hookProcessId == processId && hookInfo->hook_ver_major != 0)
		return true;

	DisableHookBackend("process changed");

	std::wstring hookName = MakeHookInfoName(processId);
	HANDLE hookMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, false, hookName.c_str());
	if (!hookMap) {
		blog(LOG_WARNING, "OBS OSD failed to open hook info for process %u: %lu", processId, GetLastError());
		return false;
	}

	hook_info *info = (hook_info *)MapViewOfFile(hookMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook_info));
	if (!info) {
		blog(LOG_WARNING, "OBS OSD failed to map hook info for process %u: %lu", processId, GetLastError());
		CloseHandle(hookMap);
		return false;
	}

	hookProcessId = processId;
	hookInfoHandle = hookMap;
	hookInfo = info;
	osdMapId = 1;
	blog(LOG_INFO, "OBS OSD hook backend attached to '%s' (process %u)", QT_TO_UTF8(sourceName), processId);
	return true;
#endif
}

bool OBSOSDController::PublishHookPayload(const OBSOSDModel &model)
{
#ifndef _WIN32
	UNUSED_PARAMETER(model);
	return false;
#else
	if (!hookInfo || !window)
		return false;

	OBSOSDConfig hookConfig = config;
	hookConfig.activeBackend = QStringLiteral("In-game hook");
	QImage image = window->RenderToImage(model, hookConfig).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const uint32_t width = (uint32_t)image.width();
	const uint32_t height = (uint32_t)image.height();
	const uint32_t pitch = (uint32_t)image.bytesPerLine();
	const size_t pixelBytes = (size_t)pitch * (size_t)height;
	const size_t requiredSize = offsetof(osd_data, pixels) + pixelBytes;

	if (!osdMapView || osdMapSize != requiredSize) {
		if (!EnsureHookPayloadMapping(requiredSize, width, height, pitch))
			return false;
	}

	osd_data *payload = (osd_data *)osdMapView;
	const uint32_t nextSequence = osdSequence + 2;
	payload->sequence = nextSequence | 1;
	payload->width = width;
	payload->height = height;
	payload->pitch = pitch;
	payload->format = OSD_FORMAT_BGRA;
	memcpy(payload->pixels, image.constBits(), pixelBytes);
	payload->sequence = nextSequence;
	osdSequence = nextSequence;

	hookInfo->osd_width = width;
	hookInfo->osd_height = height;
	hookInfo->osd_pitch = pitch;
	hookInfo->osd_map_id = osdMapId;
	hookInfo->osd_sequence = osdSequence;
	hookInfo->osd_anchor = AnchorToHookValue(config.anchor);
	hookInfo->osd_flags = (hookInfo->osd_flags & (OSD_HOOK_COMPOSITOR_READY | OSD_HOOK_COMPOSITOR_FAILED)) |
			      OSD_HOOK_ENABLED | OSD_HOOK_PREMULTIPLIED_BGRA;
	return true;
#endif
}

bool OBSOSDController::EnsureHookPayloadMapping(size_t requiredSize, uint32_t width, uint32_t height, uint32_t pitch)
{
#ifndef _WIN32
	UNUSED_PARAMETER(requiredSize);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(pitch);
	return false;
#else
	CloseHookPayloadMapping();

	++osdMapId;
	std::wstring osdName = MakeOSDName(hookProcessId, osdMapId);
	HANDLE osdMap =
		CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)requiredSize, osdName.c_str());
	if (!osdMap) {
		blog(LOG_WARNING, "OBS OSD failed to create shared payload for process %u: %lu", hookProcessId,
		     GetLastError());
		return false;
	}

	void *view = MapViewOfFile(osdMap, FILE_MAP_ALL_ACCESS, 0, 0, requiredSize);
	if (!view) {
		blog(LOG_WARNING, "OBS OSD failed to map shared payload for process %u: %lu", hookProcessId,
		     GetLastError());
		CloseHandle(osdMap);
		return false;
	}

	osdMapHandle = osdMap;
	osdMapView = view;
	osdMapSize = requiredSize;
	blog(LOG_INFO, "OBS OSD shared payload created for process %u: %ux%u pitch=%u map=%u", hookProcessId, width,
	     height, pitch, osdMapId);
	return true;
#endif
}

void OBSOSDController::CloseHookPayloadMapping()
{
#ifdef _WIN32
	if (osdMapView) {
		UnmapViewOfFile(osdMapView);
		osdMapView = nullptr;
	}
	if (osdMapHandle) {
		CloseHandle((HANDLE)osdMapHandle);
		osdMapHandle = nullptr;
	}
	osdMapSize = 0;
#endif
}
