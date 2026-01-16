#pragma once

#include <QWidget>
#include <QLabel>
#include <QTimer>

class OBSBasicOSD : public QWidget {
	Q_OBJECT

public:
	explicit OBSBasicOSD(QWidget *parent = nullptr);
	~OBSBasicOSD();

	void StartRecording();
	void StopRecording();
	void StartStreaming();
	void StopStreaming();
	void StartVirtualCam();
	void StopVirtualCam();

private slots:
	void UpdateRecordingDuration();
	void UpdateStreamingDuration();

private:
	QLabel *recordingLabel;
	QLabel *streamingLabel;
	QLabel *virtualCamLabel;
	QTimer *recordingTimer;
	QTimer *streamingTimer;
	void SetupUI();
	void UpdateOSDPosition();
};
