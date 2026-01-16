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
	recordingLabel->show();
	recordingTimer->start(100);
	UpdateRecordingDuration();
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
	streamingLabel->show();
	streamingTimer->start(100);
	UpdateStreamingDuration();
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
	virtualCamLabel->show();
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
		move(geo.right() - width() - 20, geo.y() + 20);
	}
}
