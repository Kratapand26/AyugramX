// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <QObject>
#include <QThread>
#include <QString>
#include <QStringList>
#include <vector>
#include <functional>
#include <atomic>
#include <QPair>
#include <QMap>
#include "history/history_item.h"

namespace Main {
class Session;
} // namespace Main

class PeerData;

// Forwarding mode enum
enum class ForwardingMode {
	NativeDropAuthor,  // forward_messages with drop_author
	Copy,              // send as new message using media object
	DownloadUpload,    // download to disk, re-upload (for restricted chats)
};

struct ForwarderConfig {
	// --- Channel IDs ---
	QStringList sourceIds;           // e.g. ["-1001234567890", "-1009876543210"]
	QStringList sourceTopicIds;      // parallel list, "0" = no topic filter
	QString destinationId;           // e.g. "-1001111111111"
	int destinationTopicId = 0;

	// --- Forwarding mode ---
	ForwardingMode mode = ForwardingMode::NativeDropAuthor;
	bool dropCaptions = false;       // only for native_drop_author
	bool splitDocAlbums = true;      // split document albums in copy mode
	bool duSendIndividually = false; // D/U album: send items individually

	// --- Message type filter ---
	QStringList allowedTypes;        // e.g. ["text","photo","video","document"]

	// --- Keyword filtering ---
	QStringList primaryKeywords;     // document_caption_must_contain
	QStringList secondaryKeywords;   // secondary_content_keywords
	bool keywordCaseSensitive = false;

	// --- Caption editing ---
	QStringList wordsToRemove;
	bool removalCaseSensitive = false;
	bool removeWholeWordOnly = true;

	// --- Timing & resilience ---
	float delaySeconds = 1.0f;
	int failureThreshold = 10;       // 0 = disabled
	int retryAttempts = 2;
	float retryDelay = 5.0f;

	// --- Batch Processing ---
	int messageLimit = 0;
	float fetchCycleDelaySeconds = 0.0f;
	float iterMessagesChunkWaitTime = 1.0f;

	// --- ID Range Override ---
	int startMessageId = 0;
	int endMessageId = 0;

	// --- Start/End Message Hooks ---
	QString startMessage;
	QString endMessage;
	bool pinStartMessage = false;

	// --- Pattern Removal ---
	QList<QPair<QString,QString>> removePatterns; // pairs of (startMarker, endMarker)

	// --- Inline Buttons ---
	bool inlineButtonsToText = false;

	// --- Filename as Caption ---
	bool filenameAsCaption = false;  // if media has no caption, use filename

	// --- Concurrent Dispatch ---
	int concurrentBatchSize = 1;

	// --- Log Level ---
	QString logLevel = "INFO"; // INFO or DEBUG

	// --- Session reference ---
	Main::Session *session = nullptr;
};

class ForwarderEngine : public QObject {
	Q_OBJECT
public:
	explicit ForwarderEngine(const ForwarderConfig &config, QObject *parent = nullptr);
	~ForwarderEngine() override;

	void start();
	void stop();

	// Global engine access for stop buttons
	static ForwarderEngine* g_activeEngine;
	static void stopActiveEngine();

Q_SIGNALS:
	void progress(int sourceIndex, int processed, int total);
	void finished();
	void error(const QString &message);

private:
	// --- Core processing ---
	void run();
	void processSource(int sourceIndex, const QString &sourceId, int topicId);

	// --- Message classification ---
	QString classifyMessageType(not_null<HistoryItem*> item) const;
	bool passesTypeFilter(const QString &type) const;
	bool passesKeywordFilter(not_null<HistoryItem*> item, const QString &type) const;
	bool checkKeywords(const QString &text, const QStringList &keywords) const;

	// --- Caption editing ---
	QString editCaption(const QString &text, bool isFilename = false) const;
	QString applyPatternRemoval(const QString &text) const;
	QString extractInlineButtons(not_null<HistoryItem*> item) const;

	// --- Forwarding dispatch ---
	bool forwardNativeDropAuthor(
		Main::Session *session,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<PeerData*> destPeer,
		MsgId destTopicRootId);
	bool forwardCopyMode(
		Main::Session *session,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<PeerData*> destPeer,
		MsgId destTopicRootId);
	bool forwardDownloadUpload(
		Main::Session *session,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<PeerData*> destPeer,
		MsgId destTopicRootId);

	// --- Concurrent batch download for D/U mode ---
	using PreDownloadMap = QMap<int64, QString>; // msgId -> local file path
	PreDownloadMap batchDownloadConcurrent(
		Main::Session *session,
		const std::vector<not_null<HistoryItem*>> &items);

	// Cancel-aware sleep: returns true if cancelled during sleep
	bool sleepWithCancelCheck(int totalMs) const;

	ForwarderConfig _config;
	std::atomic<bool> _stopRequested{false};
	QThread *_thread = nullptr;
};
