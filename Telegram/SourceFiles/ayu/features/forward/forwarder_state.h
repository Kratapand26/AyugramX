// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <QString>
#include <QJsonObject>
#include <QMap>
#include <QMutex>

namespace AyuForwarder {

struct SourceProgress {
	int64 lastProcessedId = 0;
	QString name;
	int64 topicId = 0;
};

class ForwarderState {
public:
	static ForwarderState& instance();

	void setFilePath(const QString &path);
	bool load();
	bool save() const;
	void selectContext(const QString &context);
	void clear();

	int64 getLastProcessedId(const QString &sourceKey) const;
	void setLastProcessedId(const QString &sourceKey, int64 msgId, int64 topicId, const QString &name);
	void setLastProcessedId(const QString &sourceKey, int64 msgId); // preserves existing name and topic

	[[nodiscard]] QMap<QString, SourceProgress> allProgress() const;

private:
	ForwarderState() = default;

	QString _filePath;
	QString _context;
	mutable QMutex _mutex;
	QMap<QString, SourceProgress> _progress;
	QMap<QString, QMap<QString, SourceProgress>> _contexts;

};

} // namespace AyuForwarder
