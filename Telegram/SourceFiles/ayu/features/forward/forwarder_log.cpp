// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/forward/forwarder_log.h"

namespace AyuForwarder {

ForwarderLog& ForwarderLog::instance() {
	static ForwarderLog inst;
	return inst;
}

void ForwarderLog::setFilePath(const QString &path) {
	QMutexLocker lock(&_mutex);
	_filePath = path;
}

void ForwarderLog::setLevel(Level level) {
	QMutexLocker lock(&_mutex);
	_minLevel = level;
}

void ForwarderLog::debug(const QString &message) {
	write(Level::LogDebug, message);
}

void ForwarderLog::info(const QString &message) {
	write(Level::LogInfo, message);
}

void ForwarderLog::warning(const QString &message) {
	write(Level::LogWarning, message);
}

void ForwarderLog::error(const QString &message) {
	write(Level::LogError, message);
}

void ForwarderLog::write(Level level, const QString &message) {
	QMutexLocker lock(&_mutex);
	if (_filePath.isEmpty()) return;
	if (level < _minLevel) return;

	QFile file(_filePath);
	if (!file.open(QIODevice::Append | QIODevice::Text)) return;

	const auto timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
	const auto levelStr = [&]() -> QString {
		switch (level) {
		case Level::LogDebug: return "DEBUG";
		case Level::LogInfo: return "INFO";
		case Level::LogWarning: return "WARN";
		case Level::LogError: return "ERROR";
		}
		return "INFO";
	}();

	QTextStream out(&file);
	out << timestamp << " - " << levelStr << " - [Forwarder] - " << message << "\n";
	file.close();
}

} // namespace AyuForwarder
