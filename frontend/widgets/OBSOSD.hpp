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

#pragma once

#include <obs-frontend-api.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <memory>
#include <vector>

class OBSOSDWindow;
class QTimer;
struct hook_info;

enum class OBSOSDSourceKind {
	Default,
	Game,
	Display,
	Camera,
	Audio,
	Scene,
	Browser,
};

struct OBSOSDSourceRow {
	QString name;
	OBSOSDSourceKind kind = OBSOSDSourceKind::Default;
	bool visible = true;
};

struct OBSOSDModel {
	bool streaming = false;
	bool recording = false;
	bool recordingPaused = false;
	bool virtualCam = false;
	QString currentScene;
	std::vector<OBSOSDSourceRow> sources;
};

struct OBSOSDConfig {
	bool enabled = false;
	bool showStatus = true;
	bool showScenes = true;
	bool showSources = true;
	double opacity = 0.85;
	double scale = 1.0;
	QString anchor = QStringLiteral("top-right");
	QString backend = QStringLiteral("auto");
	QString layoutMode = QStringLiteral("compact");
	QString activeBackend;
	bool showBackendBadge = true;
};

class OBSOSDController : public QObject {
public:
	explicit OBSOSDController(QObject *parent = nullptr);
	~OBSOSDController() override;

	void ReloadSettings();
	void HandleFrontendEvent(enum obs_frontend_event event);
	void Shutdown();

private:
	QPointer<OBSOSDWindow> window;
	std::unique_ptr<QTimer> refreshTimer;
	OBSOSDConfig config;
	uint32_t hookProcessId = 0;
	uint32_t osdMapId = 0;
	uint32_t osdSequence = 0;
	void *osdMapHandle = nullptr;
	void *osdMapView = nullptr;
	size_t osdMapSize = 0;
	void *hookInfoHandle = nullptr;
	hook_info *hookInfo = nullptr;
	uint64_t hookWaitingSinceTime = 0;
	bool hookWaitingWarningLogged = false;
	QString hookLastChoice;

	OBSOSDModel BuildModel() const;
	void Refresh();
	void UpdateWindowVisibility();
	bool UpdateHookBackend(const OBSOSDModel &model);
	void DisableHookBackend(const char *reason);
	bool FindHookedGameCapture(uint32_t &processId, QString &name) const;
	bool EnsureHookMapping(uint32_t processId, const QString &sourceName);
	bool PublishHookPayload(const OBSOSDModel &model);
	bool EnsureHookPayloadMapping(size_t requiredSize, uint32_t width, uint32_t height, uint32_t pitch);
	void CloseHookPayloadMapping();
};
