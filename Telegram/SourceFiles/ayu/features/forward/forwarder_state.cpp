// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/forward/forwarder_state.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace AyuForwarder {

ForwarderState& ForwarderState::instance() {
	static ForwarderState inst;
	return inst;
}

void ForwarderState::setFilePath(const QString &path) {
	_filePath = path;
}

void ForwarderState::load() {
	_progress.clear();
	if (_filePath.isEmpty()) return;

	QFile file(_filePath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;

	const auto doc = QJsonDocument::fromJson(file.readAll());
	file.close();

	if (!doc.isObject()) return;
	const auto root = doc.object();
	const auto sources = root.value("sources_progress").toObject();

	for (auto it = sources.begin(); it != sources.end(); ++it) {
		const auto entry = it.value().toObject();
		SourceProgress sp;
		sp.lastProcessedId = static_cast<int64>(entry.value("last_processed_id").toDouble());
		sp.name = entry.value("name").toString();
		if (entry.contains("topic_id")) {
			sp.topicId = static_cast<int64>(entry.value("topic_id").toDouble());
		}
		_progress.insert(it.key(), sp);
	}
}

void ForwarderState::save() const {
	if (_filePath.isEmpty()) return;

	QJsonObject sources;
	for (auto it = _progress.begin(); it != _progress.end(); ++it) {
		QJsonObject entry;
		entry["last_processed_id"] = static_cast<double>(it.value().lastProcessedId);
		entry["name"] = it.value().name;
		if (it.value().topicId) {
			entry["topic_id"] = static_cast<double>(it.value().topicId);
		}
		sources[it.key()] = entry;
	}

	QJsonObject root;
	root["sources_progress"] = sources;

	QFile file(_filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	file.close();
}

int64 ForwarderState::getLastProcessedId(const QString &sourceKey) const {
	const auto it = _progress.find(sourceKey);
	return (it != _progress.end()) ? it.value().lastProcessedId : 0;
}

void ForwarderState::setLastProcessedId(const QString &sourceKey, int64 msgId, int64 topicId, const QString &name) {
	_progress[sourceKey] = SourceProgress{ msgId, name, topicId };
}

void ForwarderState::setLastProcessedId(const QString &sourceKey, int64 msgId) {
	const auto it = _progress.find(sourceKey);
	const auto existingName = (it != _progress.end()) ? it.value().name : QString();
	const auto existingTopic = (it != _progress.end()) ? it.value().topicId : 0;
	_progress[sourceKey] = SourceProgress{ msgId, existingName, existingTopic };
}

} // namespace AyuForwarder
