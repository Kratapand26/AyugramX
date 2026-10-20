// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/forward/forwarder_state.h"

#include "ayu/features/forward/forwarder_log.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <limits>
#include <cmath>
#include <optional>

namespace AyuForwarder {
namespace {

std::optional<QMap<QString, SourceProgress>> ReadProgress(
		const QJsonObject &sources) {
	auto result = QMap<QString, SourceProgress>();
	for (auto it = sources.begin(); it != sources.end(); ++it) {
		const auto entry = it.value().toObject();
		const auto id = entry.value(u"last_processed_id"_q).toDouble(-1);
		const auto topic = entry.value(u"topic_id"_q).toDouble();
		if (id < 0 || id > std::numeric_limits<int>::max()
			|| topic < 0 || topic > std::numeric_limits<int>::max()
			|| !std::isfinite(id) || !std::isfinite(topic)
			|| id != std::trunc(id) || topic != std::trunc(topic)) {
			return std::nullopt;
		}
		result.insert(it.key(), SourceProgress{
			int64(id), entry.value(u"name"_q).toString(), int64(topic) });
	}
	return result;
}

QJsonObject WriteProgress(const QMap<QString, SourceProgress> &progress) {
	auto sources = QJsonObject();
	for (auto it = progress.begin(); it != progress.end(); ++it) {
		auto entry = QJsonObject();
		entry[u"last_processed_id"_q] = double(it->lastProcessedId);
		entry[u"name"_q] = it->name;
		entry[u"topic_id"_q] = double(it->topicId);
		sources[it.key()] = entry;
	}
	return sources;
}

} // namespace

ForwarderState& ForwarderState::instance() {
	static auto instance = ForwarderState();
	return instance;
}

void ForwarderState::setFilePath(const QString &path) {
	const auto lock = QMutexLocker(&_mutex);
	_filePath = path;
}

bool ForwarderState::load() {
	const auto lock = QMutexLocker(&_mutex);
	if (_filePath.isEmpty()) {
		return false;
	}
	auto file = QFile(_filePath);
	if (!file.exists()) {
		_progress.clear();
		_contexts.clear();
		_context.clear();
		return true;
	}
	if (!file.open(QIODevice::ReadOnly)) {
		ForwarderLog::instance().error(u"Could not read the forwarding resume file."_q);
		return false;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject()
		|| !doc.object().value(u"sources_progress"_q).isObject()) {
		ForwarderLog::instance().error(u"The forwarding resume file is invalid; existing progress was kept."_q);
		return false;
	}
	const auto root = doc.object();
	const auto progress = ReadProgress(root.value(u"sources_progress"_q).toObject());
	if (!progress || (root.contains(u"contexts_progress"_q)
		&& !root.value(u"contexts_progress"_q).isObject())) {
		return false;
	}
	auto loaded = QMap<QString, QMap<QString, SourceProgress>>();
	const auto contexts = root.value(u"contexts_progress"_q).toObject();
	for (auto it = contexts.begin(); it != contexts.end(); ++it) {
		const auto value = it.value().isObject() ? ReadProgress(it.value().toObject()) : std::nullopt;
		if (!value) {
			return false;
		}
		loaded.insert(it.key(), *value);
	}
	_progress = *progress;
	_context.clear();
	_contexts = std::move(loaded);
	return true;
}

void ForwarderState::selectContext(const QString &context) {
	const auto lock = QMutexLocker(&_mutex);
	if (_context == context) {
		return;
	}
	if (!_context.isEmpty()) {
		_contexts[_context] = _progress;
	}
	if (_contexts.isEmpty() && !_progress.isEmpty()) {
		_contexts[context] = _progress;
		ForwarderLog::instance().append(
			u"Legacy resume positions adopted for the current account and destination."_q);
	}
	_context = context;
	_progress = _contexts.value(context);
}

void ForwarderState::clear() {
	const auto lock = QMutexLocker(&_mutex);
	_progress.clear();
	_contexts.clear();
	_context.clear();
}

bool ForwarderState::save() const {
	const auto lock = QMutexLocker(&_mutex);
	if (_filePath.isEmpty()) {
		return false;
	}
	auto contexts = QJsonObject();
	for (auto it = _contexts.begin(); it != _contexts.end(); ++it) {
		contexts[it.key()] = WriteProgress(it.value());
	}
	if (!_context.isEmpty()) {
		contexts[_context] = WriteProgress(_progress);
	}
	auto root = QJsonObject();
	root[u"sources_progress"_q] = WriteProgress(_progress);
	root[u"contexts_progress"_q] = contexts;
	const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
	auto file = QSaveFile(_filePath);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(data) != data.size()
		|| !file.commit()) {
		ForwarderLog::instance().error(u"Could not save forwarding progress; the previous file was kept."_q);
		return false;
	}
	return true;
}

int64 ForwarderState::getLastProcessedId(const QString &sourceKey) const {
	const auto lock = QMutexLocker(&_mutex);
	return _progress.value(sourceKey).lastProcessedId;
}

void ForwarderState::setLastProcessedId(
		const QString &sourceKey,
		int64 msgId,
		int64 topicId,
		const QString &name) {
	const auto lock = QMutexLocker(&_mutex);
	_progress[sourceKey] = SourceProgress{ msgId, name, topicId };
}

void ForwarderState::setLastProcessedId(const QString &sourceKey, int64 msgId) {
	const auto lock = QMutexLocker(&_mutex);
	_progress[sourceKey].lastProcessedId = msgId;
}

QMap<QString, SourceProgress> ForwarderState::allProgress() const {
	const auto lock = QMutexLocker(&_mutex);
	return _progress;
}

} // namespace AyuForwarder
