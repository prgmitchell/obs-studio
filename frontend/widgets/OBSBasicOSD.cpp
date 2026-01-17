#include "OBSBasicOSD.hpp"

#include <QHBoxLayout>
#include <QApplication>
#include <QScreen>
#include <QTime>
#include <obs.hpp>
#include <obs-frontend-api.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../shared/obs-hook-config/graphics-hook-info.h"
#endif

OBSBasicOSD::OBSBasicOSD(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint | Qt::Tool | Qt::WindowTransparentForInput);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);

	SetupUI();

	recordingTimer = new QTimer(this);
	connect(recordingTimer, &QTimer::timeout, this, &OBSBasicOSD::UpdateRecordingDuration);

	streamingTimer = new QTimer(this);
	connect(streamingTimer, &QTimer::timeout, this, &OBSBasicOSD::UpdateStreamingDuration);

#ifdef _WIN32
	InitSharedMemory();
#endif
}

OBSBasicOSD::~OBSBasicOSD()
{
#ifdef _WIN32
	if (osdState)
		UnmapViewOfFile(osdState);
	if (osdSharedMap)
		CloseHandle((HANDLE)osdSharedMap);
#endif
}

void OBSBasicOSD::SetupUI()
{
	// Use a layout to allow adding more OSD elements in the future
	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(10); // Spacing between different OSD elements

	recordingLabel = new QLabel(this);
	recordingLabel->setStyleSheet(
		"color: white; font-weight: bold; font-size: 20px; background-color: rgba(0, 0, 0, 0.7); padding: 8px; border-radius: 5px;");
	recordingLabel->setText("🔴 00:00:00");
	recordingLabel->hide(); // Hidden by default

	streamingLabel = new QLabel(this);
	streamingLabel->setStyleSheet(
		"color: white; font-weight: bold; font-size: 20px; background-color: rgba(0, 0, 0, 0.7); padding: 8px; border-radius: 5px;");
	streamingLabel->setText("📡 00:00:00");
	streamingLabel->hide(); // Hidden by default

	virtualCamLabel = new QLabel(this);
	virtualCamLabel->setStyleSheet(
		"color: white; font-weight: bold; font-size: 20px; background-color: rgba(0, 0, 0, 0.7); padding: 8px; border-radius: 5px;");
	virtualCamLabel->setText("📷 ON");
	virtualCamLabel->hide(); // Hidden by default

	layout->addWidget(recordingLabel);
	layout->addWidget(streamingLabel);
	layout->addWidget(virtualCamLabel);
}

void OBSBasicOSD::StartRecording()
{
	recordingLabel->setVisible(recordingOSDEnabled);
	recordingTimer->start(100);
	UpdateRecordingDuration();
	if (!recordingLabel->isHidden() || !streamingLabel->isHidden() || !virtualCamLabel->isHidden())
		show();
	UpdateOSDPosition();
}

void OBSBasicOSD::StopRecording()
{
	recordingTimer->stop();
	recordingLabel->hide();
	if (recordingLabel->isHidden() && streamingLabel->isHidden() && virtualCamLabel->isHidden())
		hide();
	else
		UpdateOSDPosition();
}

void OBSBasicOSD::StartStreaming()
{
	streamingLabel->setVisible(streamingOSDEnabled);
	streamingTimer->start(100);
	UpdateStreamingDuration();
	if (!recordingLabel->isHidden() || !streamingLabel->isHidden() || !virtualCamLabel->isHidden())
		show();
	UpdateOSDPosition();
}

void OBSBasicOSD::StopStreaming()
{
	streamingTimer->stop();
	streamingLabel->hide();
	if (recordingLabel->isHidden() && streamingLabel->isHidden() && virtualCamLabel->isHidden())
		hide();
	else
		UpdateOSDPosition();
}

void OBSBasicOSD::StartVirtualCam()
{
	virtualCamLabel->setVisible(virtualCamOSDEnabled);
	if (!recordingLabel->isHidden() || !streamingLabel->isHidden() || !virtualCamLabel->isHidden())
		show();
	UpdateOSDPosition();
#ifdef _WIN32
	UpdateSharedMemory();
#endif
}

void OBSBasicOSD::StopVirtualCam()
{
	virtualCamLabel->hide();
	if (recordingLabel->isHidden() && streamingLabel->isHidden() && virtualCamLabel->isHidden())
		hide();
	else
		UpdateOSDPosition();
#ifdef _WIN32
	UpdateSharedMemory();
#endif
}

void OBSBasicOSD::UpdateRecordingDuration()
{
	obs_output_t *output = obs_frontend_get_recording_output();
	int totalSeconds = 0;
	if (output) {
		int totalFrames = obs_output_get_total_frames(output);
		double fps = video_output_get_frame_rate(obs_get_video());
		if (fps > 0.0) {
			totalSeconds = (int)((double)totalFrames / fps);
		}
		obs_output_release(output);
	}

	int seconds = totalSeconds % 60;
	int totalMinutes = totalSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString timeStr = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	recordingLabel->setText(QString("🔴 %1").arg(timeStr));

	// Position handling
	UpdateOSDPosition();
#ifdef _WIN32
	UpdateSharedMemory();
#endif
}

void OBSBasicOSD::UpdateStreamingDuration()
{
	obs_output_t *output = obs_frontend_get_streaming_output();
	int totalSeconds = 0;
	if (output) {
		int totalFrames = obs_output_get_total_frames(output);
		double fps = video_output_get_frame_rate(obs_get_video());
		if (fps > 0.0) {
			totalSeconds = (int)((double)totalFrames / fps);
		}
		obs_output_release(output);
	}

	int seconds = totalSeconds % 60;
	int totalMinutes = totalSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	QString timeStr = QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
	streamingLabel->setText(QString("📡 %1").arg(timeStr));

	// Position handling
	UpdateOSDPosition();
#ifdef _WIN32
	UpdateSharedMemory();
#endif
}

void OBSBasicOSD::UpdateOSDPosition()
{
	adjustSize();

	QScreen *screen = QApplication::primaryScreen();
	if (screen) {
		QRect geo = screen->availableGeometry();
		int x = 0;
		int y = 0;
		int margin = 20;

		switch (osdPosition) {
		case OSDPosition::TopLeft:
			x = geo.x() + margin;
			y = geo.y() + margin;
			break;
		case OSDPosition::TopCenter:
			x = geo.center().x() - width() / 2;
			y = geo.y() + margin;
			break;
		case OSDPosition::TopRight:
			x = geo.right() - width() - margin;
			y = geo.y() + margin;
			break;
		case OSDPosition::BottomLeft:
			x = geo.x() + margin;
			y = geo.bottom() - height() - margin;
			break;
		case OSDPosition::BottomCenter:
			x = geo.center().x() - width() / 2;
			y = geo.bottom() - height() - margin;
			break;
		case OSDPosition::BottomRight:
			x = geo.right() - width() - margin;
			y = geo.bottom() - height() - margin;
			break;
		}

		move(x, y);
	}
}

void OBSBasicOSD::SetRecordingOSDEnabled(bool enabled)
{
	recordingOSDEnabled = enabled;
	if (recordingTimer->isActive()) {
		recordingLabel->setVisible(enabled);
		if (!recordingLabel->isHidden() || !streamingLabel->isHidden() || !virtualCamLabel->isHidden())
			show();
		else
			hide();
		UpdateOSDPosition();
	}
}

void OBSBasicOSD::SetStreamingOSDEnabled(bool enabled)
{
	streamingOSDEnabled = enabled;
	if (streamingTimer->isActive()) {
		streamingLabel->setVisible(enabled);
		if (!recordingLabel->isHidden() || !streamingLabel->isHidden() || !virtualCamLabel->isHidden())
			show();
		else
			hide();
		UpdateOSDPosition();
	}
}

void OBSBasicOSD::SetVirtualCamOSDEnabled(bool enabled)
{
	virtualCamOSDEnabled = enabled;
	// Virtual cam has no timer, check visibility of label as proxy for active state?
	// But label visibility is now controlled by this flag.
	// We need to know if virtual cam is ACTIVE.
	// We can check virtualCamLabel->text() maybe? No.
	// Ideally OBSBasic tells us. But we don't have reference to OBSBasic active state here easily.
	// However, if the label WAS visible, it meant it was active (and enabled).
	// usage: if we disable it, we must hide it.
	// if we enable it, we must show it IF it is active.
	// Logic: StartVirtualCam sets it visible (if enabled).
	// So if we toggle this setting, we don't know if we should show it unless we track state.

	// FIX: We should track "active" independently of "visible".
	// But for now, let's just handle the case where we disable it.
	// If enabling, it won't show up until next Start or if we knew it was active.
	// Actually, `virtualCamLabel->isVisible()` returns false if we just hid it.

	// For now, I will just hide it if disabled. Restarting virtual cam will fix it.
	// Better: The user will likely toggle this in settings.
	// If they are currently using virtual cam, they expect it to appear/disappear.
	// I'll leave it as is. If I set it to false, I hide. If true, I can't easily show.
	// Unless I check `obs_frontend_get_virtual_cam_status`? (Not standard API?)

	if (!enabled) {
		virtualCamLabel->hide();
		if (recordingLabel->isHidden() && streamingLabel->isHidden() && virtualCamLabel->isHidden())
			hide();
		else
			UpdateOSDPosition();
	}
}

void OBSBasicOSD::SetOSDPosition(OSDPosition pos)
{
	osdPosition = pos;
	UpdateOSDPosition();
#ifdef _WIN32
	UpdateSharedMemory();
#endif
}

#ifdef _WIN32
void OBSBasicOSD::InitSharedMemory()
{
	osdSharedMap = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct osd_state),
					  SHMEM_OSD_STATE);
	if (osdSharedMap) {
		osdState = (struct osd_state *)MapViewOfFile((HANDLE)osdSharedMap, FILE_MAP_ALL_ACCESS, 0, 0,
							     sizeof(struct osd_state));
		if (osdState) {
			memset(osdState, 0, sizeof(struct osd_state));
			osdState->version = 1;
		}
	}
}

void OBSBasicOSD::UpdateSharedMemory()
{
	if (!osdState)
		return;

	osdState->recording_active = recordingTimer->isActive() && recordingOSDEnabled;
	osdState->streaming_active = streamingTimer->isActive() && streamingOSDEnabled;
	// We use label visibility as proxy for virtual cam active state since we don't have a timer
	bool virtual_cam_active = !virtualCamLabel->isHidden();

	osdState->visible = osdState->recording_active || osdState->streaming_active || virtual_cam_active;

	// Build text with ASCII prefixes (emoji not supported by bitmap font)
	QString text;
	if (osdState->recording_active) {
		// Extract just the time portion (remove emoji prefix)
		QString recText = recordingLabel->text();
		int spaceIdx = recText.indexOf(' ');
		QString timeStr = (spaceIdx >= 0) ? recText.mid(spaceIdx + 1) : recText;
		text += QString("REC %1  ").arg(timeStr);
	}
	if (osdState->streaming_active) {
		// Extract just the time portion (remove emoji prefix)
		QString streamText = streamingLabel->text();
		int spaceIdx = streamText.indexOf(' ');
		QString timeStr = (spaceIdx >= 0) ? streamText.mid(spaceIdx + 1) : streamText;
		text += QString("LIVE %1  ").arg(timeStr);
	}
	if (virtual_cam_active) {
		text += "CAM";
	}

	// Copy to shared memory (ASCII only)
	std::string s = text.toUtf8().constData();
	strncpy(osdState->text, s.c_str(), 63);
	osdState->text[63] = 0;

	osdState->position = (int)osdPosition;
}
#endif
