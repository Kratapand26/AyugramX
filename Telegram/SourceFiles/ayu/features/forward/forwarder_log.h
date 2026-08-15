// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>

namespace AyuForwarder {

class ForwarderLog {
public:
	enum class Level { LogDebug, LogInfo, LogWarning, LogError };

	static ForwarderLog& instance();

	void setFilePath(const QString &path);
	void setLevel(Level level);
	void debug(const QString &message);
	void info(const QString &message);
	void append(const QString &message) { info(message); } // convenience alias
	void warning(const QString &message);
	void error(const QString &message);

private:
	ForwarderLog() = default;
	void write(Level level, const QString &message);

	QString _filePath;
	Level _minLevel = Level::LogInfo;
	QMutex _mutex;
};

} // namespace AyuForwarder
