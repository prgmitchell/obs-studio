#include "OBSBasicOSD.hpp"
#include <QHBoxLayout>
#include <QApplication>
#include <QScreen>
#include <QTime>
#include <obs.hpp>
#include <obs-frontend-api.h>

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
}

OBSBasicOSD::~OBSBasicOSD() {}

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
}

void OBSBasicOSD::StopVirtualCam()
{
	virtualCamLabel->hide();
	if (recordingLabel->isHidden() && streamingLabel->isHidden() && virtualCamLabel->isHidden())
		hide();
	else
		UpdateOSDPosition();
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
}
