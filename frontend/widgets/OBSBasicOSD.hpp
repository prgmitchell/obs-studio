#pragma once

#include <QWidget>
#include <QLabel>
#include <QTimer>

class OBSBasicOSD : public QWidget {
	Q_OBJECT

public:
	explicit OBSBasicOSD(QWidget *parent = nullptr);
	~OBSBasicOSD();

	enum class OSDPosition { TopLeft, TopCenter, TopRight, BottomLeft, BottomCenter, BottomRight };

	void StartRecording();
	void StopRecording();
	void StartStreaming();
	void StopStreaming();
	void StartVirtualCam();
	void StopVirtualCam();

	void SetRecordingOSDEnabled(bool enabled);
	void SetStreamingOSDEnabled(bool enabled);
	void SetVirtualCamOSDEnabled(bool enabled);
	void SetOSDPosition(OSDPosition pos);

private slots:
	void UpdateRecordingDuration();
	void UpdateStreamingDuration();

private:
	QLabel *recordingLabel;
	QLabel *streamingLabel;
	QLabel *virtualCamLabel;
	QTimer *recordingTimer;
	QTimer *streamingTimer;

	bool recordingOSDEnabled = true;
	bool streamingOSDEnabled = true;
	bool virtualCamOSDEnabled = true;
	OSDPosition osdPosition = OSDPosition::TopRight;

	void SetupUI();
	void UpdateOSDPosition();
};
