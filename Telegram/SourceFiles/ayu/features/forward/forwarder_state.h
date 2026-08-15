// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <QString>
#include <QJsonObject>

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
	void load();
	void save() const;

	int64 getLastProcessedId(const QString &sourceKey) const;
	void setLastProcessedId(const QString &sourceKey, int64 msgId, int64 topicId, const QString &name);
	void setLastProcessedId(const QString &sourceKey, int64 msgId); // preserves existing name and topic

	[[nodiscard]] const QMap<QString, SourceProgress>& allProgress() const { return _progress; }

private:
	ForwarderState() = default;

	QString _filePath;
	QMap<QString, SourceProgress> _progress;
};

} // namespace AyuForwarder
