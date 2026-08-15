// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/forward/forwarder_engine.h"

#include "ayu/features/forward/ayu_forward.h"
#include "ayu/features/forward/ayu_sync.h"
#include "ayu/features/forward/forwarder_log.h"
#include "ayu/features/forward/forwarder_state.h"
#include "ayu/utils/telegram_helpers.h"
#include "apiwrap.h"
#include "core/application.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_channel.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_document.h"
#include "ui/userpic_view.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

#include <QRegularExpression>
#include <QThread>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <map>
#include <set>
#include <variant>

using AyuForwarder::ForwarderLog;
using AyuForwarder::ForwarderState;

// Definitions for the global flags declared in ayu_forward.h
std::atomic<bool> AyuForward::g_isForwarderCanceled{false};
std::atomic<bool> AyuForward::g_isForwarderRunning{false};

namespace {

constexpr int kFetchBatchSize = 100;               // messages per server fetch
constexpr int kPeriodicStateSaveInterval = 20;      // save state every N messages

// ---- Message type classification ----
QString classifyItem(not_null<HistoryItem*> item) {
	if (item->isService()) return QString();        // skip service messages

	const auto media = item->media();
	if (!media) {
		return item->originalText().text.isEmpty() ? QString() : QStringLiteral("text");
	}

	if (media->poll()) return QStringLiteral("poll");
	if (media->game()) return QStringLiteral("dice");  // closest TDesktop equivalent
	if (media->webpage()) return QStringLiteral("webpage");

	if (const auto doc = media->document()) {
		if (doc->sticker()) return QStringLiteral("sticker");
		if (doc->isGifv()) return QStringLiteral("gif");
		if (doc->isVideoFile() || doc->isVideoMessage()) return QStringLiteral("video");
		if (doc->isVoiceMessage() || doc->isSong()) return QStringLiteral("audio");
		return QStringLiteral("document");
	}

	if (media->photo()) return QStringLiteral("photo");

	return QStringLiteral("document");  // fallback
}

// ---- Text extraction helpers ----
QString resolveMessageText(not_null<HistoryItem*> item) {
	const auto text = item->originalText().text;
	auto &log = AyuForwarder::ForwarderLog::instance();
	log.debug(QString("resolveMessageText MsgID %1: len=%2, newlines=%3, first60='%4'")
		.arg(item->id.bare).arg(text.size()).arg(text.count('\n')).arg(QString(text.left(60)).replace('\n', "\\n")));
	return text;
}

QString extractFilename(not_null<HistoryItem*> item) {
	if (const auto media = item->media()) {
		if (const auto doc = media->document()) {
			return doc->filename();
		}
	}
	return {};
}

// ---- Peer ID resolution helper ----
PeerId resolvePeerId(const QString &rawIdStr) {
	const auto rawId = rawIdStr.toLongLong();
	return rawId < 0
		? PeerId(peerFromChannel(ChannelId(BareId(-rawId - 1000000000000LL))))
		: PeerId(peerFromUser(UserId(BareId(rawId))));
}

} // namespace

// ============================================================================
// ForwarderEngine
// ============================================================================

// Static members
ForwarderEngine* ForwarderEngine::g_activeEngine = nullptr;

void ForwarderEngine::stopActiveEngine() {
	if (g_activeEngine) {
		g_activeEngine->stop();
	}
}

ForwarderEngine::ForwarderEngine(const ForwarderConfig &config, QObject *parent)
	: QObject(parent)
	, _config(config) {
}

ForwarderEngine::~ForwarderEngine() {
	stop();
	if (g_activeEngine == this) {
		g_activeEngine = nullptr;
	}
}

bool ForwarderEngine::sleepWithCancelCheck(int totalMs) const {
	constexpr int kCheckIntervalMs = 100;
	int elapsed = 0;
	while (elapsed < totalMs) {
		if (_stopRequested || AyuForward::g_isForwarderCanceled) {
			return true; // cancelled
		}
		const int chunk = std::min(kCheckIntervalMs, totalMs - elapsed);
		QThread::msleep(static_cast<unsigned long>(chunk));
		elapsed += chunk;
	}
	return _stopRequested || AyuForward::g_isForwarderCanceled;
}

void ForwarderEngine::start() {
	auto &log = ForwarderLog::instance();
	log.debug("start(): Resetting flags and creating engine thread.");
	_stopRequested = false;
	AyuForward::g_isForwarderCanceled = false;
	AyuForward::g_isForwarderRunning = true;
	g_activeEngine = this;

	_thread = QThread::create([this] { run(); });
	connect(_thread, &QThread::finished, _thread, &QThread::deleteLater);
	connect(_thread, &QThread::finished, this, [this] {
		auto &log = ForwarderLog::instance();
		log.debug("Engine thread finished. Cleaning up.");
		AyuForward::g_isForwarderRunning = false;
		if (g_activeEngine == this) g_activeEngine = nullptr;
		_thread = nullptr;
		Q_EMIT finished();
	});
	_thread->start();
}

void ForwarderEngine::stop() {
	_stopRequested = true;
	AyuForward::g_isForwarderCanceled = true;

	// Save state on stop — persist progress to last successfully sent message
	auto &stateManager = ForwarderState::instance();
	stateManager.save();

	auto &log = ForwarderLog::instance();
	log.append("Stop requested — state saved to last successful message.");
	log.debug(QString("stop(): _stopRequested=%1, g_isForwarderCanceled=%2, g_isForwarderRunning=%3, thread=%4")
		.arg(_stopRequested.load()).arg(AyuForward::g_isForwarderCanceled.load()).arg(AyuForward::g_isForwarderRunning.load())
		.arg(_thread ? "active" : "null"));

	// Don't block the UI thread. The cancel flags are already set;
	// sleepWithCancelCheck will pick them up within 100ms.
	// The QThread::finished signal handles cleanup.
}

// ============================================================================
// Concurrent Batch Download for D/U Mode
// ============================================================================

ForwarderEngine::PreDownloadMap ForwarderEngine::batchDownloadConcurrent(
	Main::Session *session,
	const std::vector<not_null<HistoryItem*>> &items)
{
	PreDownloadMap result;
	if (items.empty() || !session) return result;

	auto &log = ForwarderLog::instance();

	// Identify downloadable items and their target paths
	struct DlTask {
		not_null<HistoryItem*> item;
		QString path;
		DocumentData *doc = nullptr;
	};

	std::vector<DlTask> tasks;
	for (const auto &item : items) {
		if (_stopRequested || AyuForward::g_isForwarderCanceled) return result;

		const auto media = item->media();
		if (!media) continue;

		const auto doc = media->document();
		if (!doc) continue; // photos don't need D/U — only documents/videos/etc.

		auto path = AyuSync::filePath(session, doc);
		if (path.isEmpty()) continue;

		// Apply filename sanitization (words_to_remove / patterns)
		const auto origName = doc->filename();
		if (!origName.isEmpty()) {
			const QFileInfo info(origName);
			const auto baseName = info.completeBaseName();
			const auto ext = info.suffix().isEmpty() ? QString() : ("." + info.suffix());
			const auto editedBaseName = editCaption(baseName, true);
			if (editedBaseName != baseName && !editedBaseName.isEmpty()) {
				const auto editedName = editedBaseName + ext;
				// Replace the filename portion of the path
				const auto dir = QFileInfo(path).absolutePath() + '/';
				path = dir + editedName;
			}
		}

		tasks.push_back({ item, path, doc });
	}

	if (tasks.empty()) return result;

	const int count = static_cast<int>(tasks.size());
	log.append(QString("Concurrent batch download: starting %1 downloads...").arg(count));

	// Create a latch with count = number of downloads
	auto latch = std::make_shared<TimedCountDownLatch>(count);
	auto lifetime = std::make_shared<rpl::lifetime>();

	// Track which tasks have completed
	auto completed = std::make_shared<std::set<int64>>();
	auto completedMutex = std::make_shared<std::mutex>();

	// Start ALL downloads on the main thread at once
	crl::on_main([=, &result] {
		for (const auto &task : tasks) {
			task.doc->save(
				Data::FileOriginMessage(task.item->fullId()),
				task.path);
		}

		// Watch for download completions
		session->downloaderTaskFinished() | rpl::on_next([=, &result]() mutable {
			std::lock_guard lock(*completedMutex);
			for (const auto &task : tasks) {
				const auto id = task.item->id.bare;
				if (completed->count(id)) continue; // already done

				const bool failed = !task.doc || task.doc->status == FileDownloadFailed;
				const bool done = QFile::exists(task.path) && QFileInfo(task.path).size() >= task.doc->size;

				if (failed || done) {
					completed->insert(id);
					if (done) {
						result[id] = task.path;
					}
					latch->countDown();
				}
			}
		}, *lifetime);
	});

	// Wait for all downloads (up to 15 minutes per file)
	const auto timeout = std::chrono::minutes(15);
	const auto startTime = std::chrono::steady_clock::now();

	while (std::chrono::steady_clock::now() - startTime < timeout) {
		if (_stopRequested || AyuForward::g_isForwarderCanceled) break;
		if (latch->await(std::chrono::milliseconds(500))) break;
	}

	base::take(lifetime)->destroy();

	log.append(QString("Batch download complete: %1/%2 files downloaded successfully.")
		.arg(result.size()).arg(count));

	return result;
}

// ---- Main run loop ----
void ForwarderEngine::run() {
	auto &log = ForwarderLog::instance();

	// Configure log level
	if (_config.logLevel.toUpper() == "DEBUG") {
		log.setLevel(ForwarderLog::Level::LogDebug);
		log.append("Logging fully configured. Level: DEBUG. File: forwarder.log");
	} else {
		log.setLevel(ForwarderLog::Level::LogInfo);
		log.append("Logging fully configured. Level: INFO. File: forwarder.log");
	}

	const auto modeStr = _config.mode == ForwardingMode::NativeDropAuthor ? "native_drop_author"
		: _config.mode == ForwardingMode::Copy ? "copy" : "download_upload";
	log.append(QString("Initializing ForwarderEngine — Mode: %1, Sources: %2, Destination: %3 (topic %4)")
		.arg(modeStr)
		.arg(_config.sourceIds.size())
		.arg(_config.destinationId)
		.arg(_config.destinationTopicId));

	if (!_config.allowedTypes.isEmpty()) {
		log.append("Allowed types: " + _config.allowedTypes.join(", "));
	}
	if (!_config.wordsToRemove.isEmpty()) {
		log.append("Words to remove: " + _config.wordsToRemove.join(", "));
	}
	if (!_config.primaryKeywords.isEmpty()) {
		log.append("Primary keywords filter: " + _config.primaryKeywords.join(", "));
	}
	log.append(QString("Config: delay=%1s, retryAttempts=%2, retryDelay=%3s, failThreshold=%4")
		.arg(_config.delaySeconds).arg(_config.retryAttempts).arg(_config.retryDelay).arg(_config.failureThreshold));
	log.debug(QString("Config: messageLimit=%1, concurrentBatch=%2, inlineButtonsToText=%3, duSendIndividually=%4")
		.arg(_config.messageLimit).arg(_config.concurrentBatchSize).arg(_config.inlineButtonsToText).arg(_config.duSendIndividually));
	if (!_config.removePatterns.isEmpty()) {
		for (const auto &p : _config.removePatterns) {
			log.debug(QString("Pattern removal rule: [%1] ... [%2]").arg(p.first, p.second));
		}
	}

	for (int i = 0; i < _config.sourceIds.size() && !_stopRequested && !AyuForward::g_isForwarderCanceled; ++i) {
		// Split comma-separated topic IDs for this source
		const auto topicStr = (i < _config.sourceTopicIds.size())
			? _config.sourceTopicIds[i].trimmed()
			: QString("0");
		const auto topicParts = topicStr.split(',', Qt::SkipEmptyParts);
		QList<int> topicIds;
		for (const auto &tp : topicParts) {
			const int tid = tp.trimmed().toInt();
			if (tid > 0) topicIds.append(tid);
		}
		if (topicIds.isEmpty()) topicIds.append(0); // no topic filter

		// Process each topic ID sequentially
		for (const int topicId : topicIds) {
			if (_stopRequested || AyuForward::g_isForwarderCanceled) break;

			// --- Per-topic Start Message Hook ---
			if (_config.session) {
				const auto destPeerId = resolvePeerId(_config.destinationId);
				PeerData *destPeer = nullptr;
				{
					QSemaphore sem;
					crl::on_main([&] { destPeer = _config.session->data().peerLoaded(destPeerId); sem.release(); });
					sem.acquire();
				}

				if (destPeer) {
					// Resolve source peer for name
					const auto sourcePeerId = resolvePeerId(_config.sourceIds[i]);
					PeerData *srcPeer = nullptr;
					{
						QSemaphore sem;
						crl::on_main([&] { srcPeer = _config.session->data().peerLoaded(sourcePeerId); sem.release(); });
						sem.acquire();
					}

					// Determine start message text
					QString startText = _config.startMessage;

					if (!startText.isEmpty()) {
						const auto history = _config.session->data().history(destPeer);
						auto action = Api::SendAction(history);
						if (_config.destinationTopicId) {
							action.replyTo.topicRootId = MsgId(_config.destinationTopicId);
						}
						auto message = Api::MessageToSend(action);
						message.textWithTags.text = startText;
						AyuSync::sendMessageSync(_config.session, std::move(message));
						log.append(QString("Sent Start Message: '%1'").arg(startText));

						// Pin start message using MTP API
						if (_config.pinStartMessage) {
							sleepWithCancelCheck(300);
							crl::on_main([session = _config.session, destPeer] {
								const auto history = session->data().history(destPeer);
								const auto lastMsg = history->lastMessage();
								if (lastMsg && lastMsg->out()) {
									const auto msgId = lastMsg->id;
									auto flags = MTPmessages_UpdatePinnedMessage::Flags(0);
									flags |= MTPmessages_UpdatePinnedMessage::Flag::f_silent;
									session->api().request(MTPmessages_UpdatePinnedMessage(
										MTP_flags(flags),
										destPeer->input(),
										MTP_int(msgId)
									)).done([session](const MTPUpdates &result) {
										session->api().applyUpdates(result);
									}).send();
								}
							});
							sleepWithCancelCheck(500);
							log.append("Pinned start message.");
						}
					}
				}
			}

			processSource(i, _config.sourceIds[i], topicId);

		} // end topic loop
	}

	// --- End Message Hook ---
	if (!_config.endMessage.isEmpty() && _config.session) {
		const auto destPeerId = resolvePeerId(_config.destinationId);
		PeerData *destPeer = nullptr;
		{
			QSemaphore sem;
			crl::on_main([&] { destPeer = _config.session->data().peerLoaded(destPeerId); sem.release(); });
			sem.acquire();
		}
		if (destPeer) {
			const auto history = _config.session->data().history(destPeer);
			auto action = Api::SendAction(history);
			if (_config.destinationTopicId) {
				action.replyTo.topicRootId = MsgId(_config.destinationTopicId);
			}
			auto message = Api::MessageToSend(action);
			message.textWithTags.text = _config.endMessage;
			AyuSync::sendMessageSync(_config.session, std::move(message));
			log.append(QString("Sent End Message: '%1'").arg(_config.endMessage));
		}
	}

	log.append("All configured source channels have been processed for this run.");
	log.append("Initiating shutdown sequence...");
	log.append("=== ForwarderEngine finished ===");
}

// ---- Process a single source channel ----
void ForwarderEngine::processSource(int sourceIndex, const QString &sourceId, int topicId) {
	auto &log = ForwarderLog::instance();
	auto &stateManager = ForwarderState::instance();
	auto *session = _config.session;

	if (!session) {
		log.append("ERROR: No session available");
		Q_EMIT error("No session available");
		return;
	}

	// Resolve source peer
	const auto sourcePeerId = resolvePeerId(sourceId);

	PeerData *sourcePeer = nullptr;
	{
		QSemaphore sem;
		crl::on_main([&] {
			sourcePeer = session->data().peerLoaded(sourcePeerId);
			sem.release();
		});
		sem.acquire();
	}

	if (!sourcePeer) {
		log.append(QString("ERROR: Cannot resolve source peer %1").arg(sourceId));
		Q_EMIT error(QString("Cannot resolve source peer %1").arg(sourceId));
		return;
	}

	// Resolve destination peer
	const auto destPeerId = resolvePeerId(_config.destinationId);

	PeerData *destPeer = nullptr;
	{
		QSemaphore sem;
		crl::on_main([&] {
			destPeer = session->data().peerLoaded(destPeerId);
			sem.release();
		});
		sem.acquire();
	}

	if (!destPeer) {
		log.append(QString("ERROR: Cannot resolve destination peer %1").arg(_config.destinationId));
		Q_EMIT error(QString("Cannot resolve dest peer %1").arg(_config.destinationId));
		return;
	}

	const auto sourceName = sourcePeer->name();
	const auto destName = destPeer->name();

	// State key — matches Python's format
	const auto stateKey = topicId > 0
		? QString("source_%1_topic_%2").arg(sourceId).arg(topicId)
		: QString("source_%1").arg(sourceId);

	bool idRangeOverrideActive = (_config.startMessageId > 0 && _config.endMessageId >= _config.startMessageId);
	MsgId lastProcessedId = idRangeOverrideActive ? MsgId(_config.startMessageId - 1) : MsgId(stateManager.getLastProcessedId(stateKey));
	const auto destTopicRootId = MsgId(_config.destinationTopicId);

	// --- Python-style source header ---
	const auto topicStr = topicId > 0 ? QString(" (Topic %1)").arg(topicId) : QString(" (No Topic Filter)");
	log.append(QString(""));
	log.append(QString("========================= Processing Source %1/%2: '%3'%4 =========================")
		.arg(sourceIndex + 1).arg(_config.sourceIds.size()).arg(sourceId).arg(topicStr));
	log.append(QString("Resolved '%1' to: '%2' (Type: Channel)").arg(sourceId, sourceName));
	log.append(QString("  Destination: '%1' (ID:%2)").arg(destName).arg(_config.destinationId));
	log.append(QString("For source key '%1', current known last successful ID is %2. Fetching messages after this ID.")
		.arg(stateKey).arg(lastProcessedId.bare));

	QElapsedTimer sourceTimer;
	sourceTimer.start();

	int totalProcessed = 0;
	int totalSent = 0;
	int totalSkipped = 0;
	int totalFailed = 0;
	int consecutiveFailures = 0;

	const auto modeStr = _config.mode == ForwardingMode::NativeDropAuthor ? "native_drop_author"
		: _config.mode == ForwardingMode::Copy ? "copy" : "download_upload";

	// ---- Fetch loop ----
	bool moreMessages = true;
	MsgId fetchOffsetId = lastProcessedId;
	bool firstFetch = true;
	int processedInCycle = 0;
	int totalRawFetched = 0;

	while (moreMessages && !_stopRequested && !AyuForward::g_isForwarderCanceled) {
		if (!firstFetch && _config.iterMessagesChunkWaitTime > 0.0f) {
			if (sleepWithCancelCheck(static_cast<int>(_config.iterMessagesChunkWaitTime * 1000))) break;
		}
		firstFetch = false;

		// Fetch batch from server
		// Fetch messages synchronously via MTP API
		std::vector<not_null<HistoryItem*>> items;
		{
			QSemaphore sem;
			crl::on_main([&] {
				if (topicId > 0) {
					// For forum topics, use GetReplies
					session->api().request(MTPmessages_GetReplies(
						sourcePeer->input(),
						MTP_int(topicId),
						MTP_int(fetchOffsetId.bare ? fetchOffsetId.bare : 1), // offset_id
						MTP_int(0), // offset_date
						MTP_int(-kFetchBatchSize), // add_offset
						MTP_int(kFetchBatchSize), // limit
						MTP_int(0), // min_id
						MTP_int(0), // max_id
						MTP_long(0) // hash
					)).done([&](const MTPmessages_Messages &result) {
						result.match([&](const MTPDmessages_messagesNotModified &) {
							// Nothing to process
						}, [&](const auto &data) {
							session->data().processUsers(data.vusers());
							session->data().processChats(data.vchats());
							session->data().processMessages(data.vmessages(), NewMessageType::Existing);
							for (const auto &msg : data.vmessages().v) {
								const auto peerId = PeerFromMessage(msg);
								const auto msgId = IdFromMessage(msg);
								if (const auto item = session->data().message(peerId, msgId)) {
									items.push_back(item);
								}
							}
						});
						sem.release();
					}).fail([&] {
						sem.release();
					}).send();
				} else {
					session->api().request(MTPmessages_GetHistory(
						sourcePeer->input(),
						MTP_int(fetchOffsetId.bare ? fetchOffsetId.bare : 1), // offset_id
						MTP_int(0), // offset_date
						MTP_int(-kFetchBatchSize), // add_offset
						MTP_int(kFetchBatchSize), // limit
						MTP_int(0), // max_id
						MTP_int(0), // min_id
						MTP_long(0)  // hash
					)).done([&](const MTPmessages_Messages &result) {
						result.match([&](const MTPDmessages_messagesNotModified &) {
							// Nothing to process
						}, [&](const auto &data) {
							session->data().processUsers(data.vusers());
							session->data().processChats(data.vchats());
							session->data().processMessages(data.vmessages(), NewMessageType::Existing);
							for (const auto &msg : data.vmessages().v) {
								const auto peerId = PeerFromMessage(msg);
								const auto msgId = IdFromMessage(msg);
								if (fetchOffsetId.bare > 0 && msgId.bare <= fetchOffsetId.bare) continue;
								if (const auto item = session->data().message(peerId, msgId)) {
									items.push_back(item);
								}
							}
						});
						sem.release();
					}).fail([&] {
						sem.release();
					}).send();
				}
			});
			sem.acquire();
		}

		// Reverse items to process chronologically (oldest to newest)
		std::reverse(items.begin(), items.end());

		if (items.empty()) {
			log.debug(QString("No new messages in this fetch cycle for source key '%1'. fetchOffsetId=%2")
				.arg(stateKey).arg(fetchOffsetId.bare));
			moreMessages = false;
			break;
		}

		totalRawFetched += items.size();
		log.append(QString("Iterator yielded %1 raw items for source key '%2' to process.")
			.arg(items.size()).arg(stateKey));

		// ---- Group albums by groupId ----
		std::map<uint64, std::vector<not_null<HistoryItem*>>> albumBuffer;
		std::vector<std::variant<not_null<HistoryItem*>, uint64>> taskBatch;
		std::set<uint64> groupIdsSeen;

		for (const auto &item : items) {
			auto groupId = item->groupId().value;
			if (groupId && _config.splitDocAlbums) {
				const auto media = item->media();
				if (media && media->document()) {
					const auto doc = media->document();
					// In python script, 'document' means not video/photo/etc.
					// We split it if it's a general document.
					if (!doc->isVideoFile() && !doc->isVideoMessage() && !doc->isGifv()) {
						groupId = 0;
					}
				}
			}
			if (groupId) {
				albumBuffer[groupId].push_back(item);
				if (groupIdsSeen.find(groupId) == groupIdsSeen.end()) {
					groupIdsSeen.insert(groupId);
					taskBatch.push_back(groupId);
				}
			} else {
				taskBatch.push_back(item);
			}
		}

		log.debug(QString("Batch: %1 tasks (%2 albums, %3 singles)")
			.arg(taskBatch.size()).arg(albumBuffer.size()).arg(taskBatch.size() - albumBuffer.size()));

		// NOTE: batchDownloadConcurrent is available for future use but currently
		// AyuForward::forwardMessages handles its own downloads via loadDocuments.

		// ---- Process task batch ----
		for (const auto &task : taskBatch) {
			if (_stopRequested || AyuForward::g_isForwarderCanceled) break;

			if (_config.messageLimit > 0 && processedInCycle >= _config.messageLimit) {
				log.append(QString("Hit message batch limit (%1), delaying next fetch cycle by %2s")
					.arg(_config.messageLimit).arg(_config.fetchCycleDelaySeconds));
				if (sleepWithCancelCheck(static_cast<int>(_config.fetchCycleDelaySeconds * 1000))) break;
				processedInCycle = 0;
			}

			if (_config.failureThreshold > 0 && consecutiveFailures >= _config.failureThreshold) {
				log.append(QString("Failure threshold (%1) reached. Stopping source %2")
					.arg(_config.failureThreshold).arg(sourceId));
				moreMessages = false;
				break;
			}

			std::vector<not_null<HistoryItem*>> msgGroup;
			bool isAlbum = false;
			QString logPrefix;

			if (auto *gid = std::get_if<uint64>(&task)) {
				isAlbum = true;
				msgGroup = albumBuffer[*gid];
				if (msgGroup.empty()) continue;
				logPrefix = QString("Album(gid=%1, %2 items, first=%3)")
					.arg(*gid).arg(msgGroup.size()).arg(msgGroup.front()->id.bare);
			} else if (auto *itemPtr = std::get_if<not_null<HistoryItem*>>(&task)) {
				msgGroup.push_back(*itemPtr);
				logPrefix = QString("MsgID %1").arg((*itemPtr)->id.bare);
			}

			if (msgGroup.empty()) continue;
			const auto firstItem = msgGroup.front();

			auto markSkipped = [&](int skipCount) {
				totalSkipped += skipCount;
				MsgId highestId = 0;
				for (const auto &m : msgGroup) {
					if (m->id > highestId) highestId = m->id;
				}
				if (highestId > lastProcessedId) {
					lastProcessedId = highestId;
				}
			};

			// ---- ID Range Override Bounds Check ----
			if (idRangeOverrideActive && firstItem->id.bare > _config.endMessageId) {
				log.append(QString("Reached end_message_id (%1). Stopping source %2")
					.arg(_config.endMessageId).arg(sourceId));
				moreMessages = false;
				break;
			}
			if (idRangeOverrideActive && firstItem->id.bare < _config.startMessageId) {
				log.debug(logPrefix + " skipped: below start_message_id override bound");
				markSkipped(1);
				continue;
			}

			// ---- Service message skip ----
			if (firstItem->isService()) {
				log.debug(logPrefix + " skipped: service message");
				markSkipped(1);
				continue;
			}

			// ---- Message type filter ----
			const auto msgType = isAlbum ? QStringLiteral("album") : classifyItem(firstItem);
			if (msgType.isEmpty()) {
				log.debug(logPrefix + " skipped: empty type");
				markSkipped(1);
				continue;
			}

			log.debug(logPrefix + QString(": classified as '%1'").arg(msgType));

			if (!passesTypeFilter(isAlbum ? QStringLiteral("album") : msgType)) {
				if (isAlbum) {
					const auto firstItemType = classifyItem(firstItem);
					if (!passesTypeFilter(firstItemType)) {
						log.debug(logPrefix + QString(" skipped: type '%1' not allowed").arg(firstItemType));
						markSkipped(msgGroup.size());
						continue;
					}
				} else {
					log.debug(logPrefix + QString(" skipped: type '%1' not allowed").arg(msgType));
					markSkipped(1);
					continue;
				}
			}

			// ---- Keyword filter ----
			if (!passesKeywordFilter(firstItem, msgType)) {
				log.debug(logPrefix + " skipped: keyword filter");
				markSkipped(1);
				continue;
			}

			// ---- Log mode being used (Python-style) ----
			log.debug(logPrefix + QString(": Using '%1' forwarding mode.").arg(modeStr));

			// ---- Forward the message(s) ----
			bool success = false;
			int retries = 0;

			while (retries <= _config.retryAttempts && !_stopRequested && !AyuForward::g_isForwarderCanceled) {
				try {
					auto effectiveMode = _config.mode;

					switch (effectiveMode) {
					case ForwardingMode::NativeDropAuthor:
						success = forwardNativeDropAuthor(session, msgGroup, destPeer, destTopicRootId);
						break;
					case ForwardingMode::Copy:
						success = forwardCopyMode(session, msgGroup, destPeer, destTopicRootId);
						break;
					case ForwardingMode::DownloadUpload:
						success = forwardDownloadUpload(session, msgGroup, destPeer, destTopicRootId);
						break;
					}

					if (success) break;

				} catch (const std::exception &e) {
					log.debug(logPrefix + QString(" exception caught: %1").arg(e.what()));
				} catch (...) {
					log.debug(logPrefix + " unknown exception caught during forward");
				}

				retries++;
				if (retries <= _config.retryAttempts) {
					// User requested specific progression: 10m, 20m, 40m, 60m, then +20m each
					int waitMinutes = 0;
					if (retries == 1) waitMinutes = 10;
					else if (retries == 2) waitMinutes = 20;
					else if (retries == 3) waitMinutes = 40;
					else waitMinutes = 60 + (retries - 4) * 20;
					
					int waitMs = waitMinutes * 60 * 1000;
					log.append(logPrefix + QString(" retry %1/%2 in %3ms (backoff)")
						.arg(retries).arg(_config.retryAttempts).arg(waitMs));
					if (sleepWithCancelCheck(waitMs)) break;
				}
			}

			if (success) {
				totalSent++;
				consecutiveFailures = 0;

				MsgId highestId = 0;
				for (const auto &m : msgGroup) {
					if (m->id > highestId) highestId = m->id;
				}
				if (highestId > lastProcessedId) {
					lastProcessedId = highestId;
				}

				// Python-style success log
				const auto origDate = firstItem->date();
				const auto dateStr = QDateTime::fromSecsSinceEpoch(origDate).toString("yy-MM-dd hh:mm");
				const auto effectiveModeStr = _config.mode == ForwardingMode::NativeDropAuthor
					? QString("NativeFwdDropAuthor (%1)").arg(msgType)
					: _config.mode == ForwardingMode::Copy
						? QString("Copy (%1)").arg(msgType)
						: QString("Media D/U (%1)").arg(msgType);
				log.append(QString("Successfully sent MsgID %1 (Type: %2) (Orig.Date: %3)")
					.arg(firstItem->id.bare).arg(effectiveModeStr, dateStr));
			} else {
				totalFailed++;
				consecutiveFailures++;
				log.append(logPrefix + QString(" FAILED after %1 retries").arg(retries));

				MsgId highestId = 0;
				for (const auto &m : msgGroup) {
					if (m->id > highestId) highestId = m->id;
				}
				if (highestId > lastProcessedId) {
					lastProcessedId = highestId;
				}
			}

			totalProcessed++;
			processedInCycle++;

			// Periodic state save
			if (!idRangeOverrideActive && totalProcessed % kPeriodicStateSaveInterval == 0) {
				stateManager.setLastProcessedId(stateKey, lastProcessedId.bare, topicId, sourceName);
				stateManager.save();
			}

			// Delay between messages (cancel-aware)
			if (!_stopRequested && !AyuForward::g_isForwarderCanceled && _config.delaySeconds > 0) {
				log.debug(logPrefix + QString(": sleeping %1ms before next message").arg(static_cast<int>(_config.delaySeconds * 1000)));
				if (sleepWithCancelCheck(static_cast<int>(_config.delaySeconds * 1000))) {
					log.debug(logPrefix + ": sleep interrupted by cancel");
					break;
				}
			}

			Q_EMIT progress(sourceIndex, totalProcessed, 0);
		}

		// Update fetchOffsetId for next batch
		if (!items.empty()) {
			fetchOffsetId = items.back()->id;
		}

		// Final state save for this batch
		if (!idRangeOverrideActive) {
			stateManager.setLastProcessedId(stateKey, lastProcessedId.bare, topicId, sourceName);
			stateManager.save();
			if (_stopRequested || AyuForward::g_isForwarderCanceled) {
				log.append(QString("Saved last processed message id %1 for source '%2' upon script pause.").arg(lastProcessedId.bare).arg(sourceName));
			}
		}
	}

	// --- Python-style source completion summary ---
	const auto elapsed = sourceTimer.elapsed() / 1000.0;
	log.append(QString("--- Finished Message Processing for Source Key: '%1' (Display: '%2') ---")
		.arg(stateKey, sourceName));
	log.append(QString("Summary for '%1': Iterated=%2, Sent=%3, Skipped=%4, Failed=%5")
		.arg(stateKey).arg(totalProcessed).arg(totalSent).arg(totalSkipped).arg(totalFailed));
	log.append(QString("Duration for this source: %1s").arg(elapsed, 0, 'f', 2));
	log.append(QString("Final Last Successful ID for '%1' for this run: %2")
		.arg(stateKey).arg(lastProcessedId.bare));
	log.append("----------------------------------------------------------------------");
}

// ============================================================================
// Message Classification & Filtering
// ============================================================================

QString ForwarderEngine::classifyMessageType(not_null<HistoryItem*> item) const {
	return classifyItem(item);
}

bool ForwarderEngine::passesTypeFilter(const QString &type) const {
	if (_config.allowedTypes.isEmpty()) return true;  // no filter = allow all
	return _config.allowedTypes.contains(type, Qt::CaseInsensitive);
}

bool ForwarderEngine::passesKeywordFilter(not_null<HistoryItem*> item, const QString &type) const {
	auto &log = ForwarderLog::instance();
	if (_config.primaryKeywords.isEmpty()) return true;  // no filter = allow all

	// Keyword filter only applies to file-based media types.
	// Non-media types (text, photo, album, webpage, poll, dice) always pass through.
	static const QStringList mediaTypes = {
		"video", "document", "audio", "voice", "gif", "sticker"
	};
	if (!mediaTypes.contains(type, Qt::CaseInsensitive)) {
		log.debug(QString("passesKeywordFilter MsgID %1: type '%2' is not file-media, bypassing filter")
			.arg(item->id.bare).arg(type));
		return true; // non-media always passes
	}

	// Build searchable text: caption/text + filename
	auto text = resolveMessageText(item);
	const auto filename = extractFilename(item);
	if (!filename.isEmpty()) {
		text += " " + filename;
	}

	if (text.trimmed().isEmpty()) {
		// Media with no caption and no filename — let it through
		log.debug(QString("passesKeywordFilter MsgID %1: media with no searchable text, allowing through").arg(item->id.bare));
		return true;
	}

	// Primary filter: at least one keyword must match (case-insensitive substring)
	if (!checkKeywords(text, _config.primaryKeywords)) {
		log.debug(QString("passesKeywordFilter MsgID %1: no primary keyword match in '%2'").arg(item->id.bare).arg(text.left(80)));
		return false;
	}

	// Secondary filter: if set, at least one secondary keyword must also match
	if (!_config.secondaryKeywords.isEmpty()) {
		if (!checkKeywords(text, _config.secondaryKeywords)) {
			log.debug(QString("passesKeywordFilter MsgID %1: primary matched but no secondary keyword match").arg(item->id.bare));
			return false;
		}
	}

	log.debug(QString("passesKeywordFilter MsgID %1: PASSED (matched in '%2')").arg(item->id.bare).arg(text.left(80)));
	return true;
}

bool ForwarderEngine::checkKeywords(const QString &text, const QStringList &keywords) const {
	const auto options = _config.keywordCaseSensitive
		? QRegularExpression::NoPatternOption
		: QRegularExpression::CaseInsensitiveOption;

	for (const auto &kw : keywords) {
		const auto trimmed = kw.trimmed();
		if (trimmed.isEmpty()) continue;

		// Python logic: `(?<!\w)` + kw + `(?!\w)`
		// We implement this using smart boundaries, ensuring no adjacent word characters.
		QString leftBoundary = "(?<!\\w)";
		QString rightBoundary = "(?!\\w)";

		const auto pattern = leftBoundary + QRegularExpression::escape(trimmed) + rightBoundary;
		QRegularExpression re(pattern, options);
		if (text.contains(re)) {
			return true;
		}
	}
	return false;
}

// ============================================================================
// Caption Editing
// ============================================================================

QString ForwarderEngine::editCaption(const QString &text, bool isFilename) const {
	if (_config.wordsToRemove.isEmpty() && _config.removePatterns.isEmpty()) {
		return text;
	}

	auto &log = ForwarderLog::instance();
	QString result = text;
	for (const auto &word : _config.wordsToRemove) {
		if (word.trimmed().isEmpty()) continue;

		if (_config.removeWholeWordOnly && !isFilename) {
			// Smart boundary check: Only enforce \b (word boundary) if the pattern starts/ends with a word char.
			const auto trimmed = word.trimmed();
			QString leftBoundary, rightBoundary;
			
			// In regex, \w is letter, number, or underscore.
			QRegularExpression wordChar("^\\w");
			if (wordChar.match(trimmed).hasMatch()) {
				leftBoundary = "(?<!\\w)";
			}
			QRegularExpression wordCharEnd("\\w$");
			if (wordCharEnd.match(trimmed).hasMatch()) {
				rightBoundary = "(?!\\w)";
			}

			const auto pattern = leftBoundary + QRegularExpression::escape(trimmed) + rightBoundary;
			const auto options = _config.removalCaseSensitive
				? QRegularExpression::NoPatternOption
				: QRegularExpression::CaseInsensitiveOption;
			QRegularExpression re(pattern, options);
			if (result.contains(re)) {
				log.debug(QString("Removed whole word: '%1'").arg(trimmed));
				result.replace(re, QString());
			}
		} else {
			// Simple substring removal
			if (_config.removalCaseSensitive) {
				if (result.contains(word.trimmed())) {
					log.debug(QString("Removed exact substring: '%1'").arg(word.trimmed()));
					result.replace(word.trimmed(), QString());
				}
			} else {
				if (result.contains(word.trimmed(), Qt::CaseInsensitive)) {
					log.debug(QString("Removed case-insensitive substring: '%1'").arg(word.trimmed()));
					result.replace(word.trimmed(), QString(), Qt::CaseInsensitive);
				}
			}
		}
	}

	// Apply pattern removal (Python applies this to filenames as well)
	result = applyPatternRemoval(result);

	if (result != text) {
		if (isFilename) {
			result = result.simplified();
			while (result.endsWith(' ') || result.endsWith('.') || result.endsWith('_') || result.endsWith('-')) {
				result.chop(1);
			}
			while (result.startsWith(' ') || result.startsWith('.') || result.startsWith('_') || result.startsWith('-')) {
				result.remove(0, 1);
			}
		} else {
			QRegularExpression newlinesRe("(\\n\\s*){3,}");
			result.replace(newlinesRe, "\n\n");
			result = result.trimmed();
		}
		log.debug(QString("Original caption size: %1, Edited size: %2").arg(text.size()).arg(result.size()));
	}
	return result;
}

// ============================================================================
// Pattern Removal
// ============================================================================

QString ForwarderEngine::applyPatternRemoval(const QString &text) const {
	if (_config.removePatterns.isEmpty() || text.isEmpty()) return text;

	auto &log = ForwarderLog::instance();
	QString result = text;

	for (const auto &pair : _config.removePatterns) {
		if (pair.first.isEmpty() || pair.second.isEmpty()) continue;

		const auto pattern = QRegularExpression::escape(pair.first)
			+ ".*?" + QRegularExpression::escape(pair.second);
		const auto options = _config.removalCaseSensitive
			? QRegularExpression::DotMatchesEverythingOption
			: (QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
		QRegularExpression re(pattern, options);

		int safetyCounter = 0;
		while (safetyCounter++ < 100) {
			const auto match = re.match(result);
			if (!match.hasMatch()) break;
			log.debug(QString("Pattern removal: stripped [%1..%2] span %3-%4")
				.arg(pair.first, pair.second)
				.arg(match.capturedStart()).arg(match.capturedEnd()));
			result = result.left(match.capturedStart()) + result.mid(match.capturedEnd());
		}
	}

	return result;
}

// ============================================================================
// Inline Button Extraction
// ============================================================================

QString ForwarderEngine::extractInlineButtons(not_null<HistoryItem*> item) const {
	if (!_config.inlineButtonsToText) return {};

	const auto markup = item->inlineReplyMarkup();
	if (!markup) return {};

	auto &log = ForwarderLog::instance();
	QString result;

	for (const auto &row : markup->data.rows) {
		for (const auto &button : row) {
			// Only extract URL buttons
			if (button.type == HistoryMessageMarkupButton::Type::Url && !button.data.isEmpty()) {
				const auto url = QString::fromUtf8(button.data);
				result += QString("\n%1: %2").arg(button.text, url);
				log.debug(QString("Extracted inline button: %1 -> %2").arg(button.text, url));
			}
		}
	}

	return result;
}

// ============================================================================
// Forwarding Mode Implementations
// ============================================================================

bool ForwarderEngine::forwardNativeDropAuthor(
	Main::Session *session,
	const std::vector<not_null<HistoryItem*>> &items,
	not_null<PeerData*> destPeer,
	MsgId destTopicRootId) {

	auto &log = ForwarderLog::instance();
	const auto firstId = items.empty() ? 0 : items.front()->id.bare;
	log.debug(QString("forwardNativeDropAuthor: %1 items, first MsgID=%2, dropCaptions=%3")
		.arg(items.size()).arg(firstId).arg(_config.dropCaptions));

	const auto history = session->data().history(destPeer);
	auto action = Api::SendAction(history);
	if (destTopicRootId) {
		action.replyTo.topicRootId = destTopicRootId;
	}

	const auto options = _config.dropCaptions
		? Data::ForwardOptions::NoNamesAndCaptions
		: Data::ForwardOptions::NoSenderNames;

	AyuSync::forwardMessagesSync(session, items, action, options);
	log.debug(QString("forwardNativeDropAuthor MsgID %1: forwarded").arg(firstId));
	return true;
}

bool ForwarderEngine::forwardCopyMode(
	Main::Session *session,
	const std::vector<not_null<HistoryItem*>> &items,
	not_null<PeerData*> destPeer,
	MsgId destTopicRootId) {

	auto &log = ForwarderLog::instance();
	log.debug(QString("forwardCopyMode: %1 items").arg(items.size()));

	const auto history = session->data().history(destPeer);

	if (items.size() > 1) {
		log.debug(QString("forwardCopyMode: multi-item album, forwarding %1 items as group").arg(items.size()));
		auto action = Api::SendAction(history);
		if (destTopicRootId) action.replyTo.topicRootId = destTopicRootId;
		auto draft = Data::ResolvedForwardDraft(items);
		draft.options = Data::ForwardOptions::NoSenderNames;
		AyuForward::forwardMessages(session, action, false, draft);
		return true;
	}

	const auto item = items.front();
	auto action = Api::SendAction(history);
	if (destTopicRootId) action.replyTo.topicRootId = destTopicRootId;

	// Extract text WITH formatting tags from the original message
	auto extracted = extractText(item);
	auto textContent = extracted.text;
	const auto originalText = textContent;
	log.debug(QString("forwardCopyMode MsgID %1: original text length=%2, tags=%3").arg(item->id.bare).arg(textContent.size()).arg(extracted.tags.size()));

	// Filename as caption: if no caption and has media filename, use it
	if (_config.filenameAsCaption && textContent.trimmed().isEmpty()) {
		const auto fname = extractFilename(item);
		if (!fname.isEmpty()) {
			// Use filename without extension as caption
			const auto dotPos = fname.lastIndexOf('.');
			textContent = (dotPos > 0) ? fname.left(dotPos) : fname;
			extracted.tags.clear();
			log.debug(QString("forwardCopyMode MsgID %1: using filename as caption: '%2'").arg(item->id.bare).arg(textContent));
		}
	}

	textContent = editCaption(textContent);

	// Only clear formatting tags when editCaption actually removed
	// words/patterns (changing content and invalidating offsets).
	// Don't clear for whitespace-only normalization (trimming, etc).
	const auto hasRemovals = !_config.wordsToRemove.isEmpty()
		|| !_config.removePatterns.isEmpty();
	if (hasRemovals && textContent != originalText) {
		extracted.tags.clear();
	}

	// Append inline buttons as text (fixing formatting loss and extra gap lines)
	const auto btnText = extractInlineButtons(item);
	if (!btnText.isEmpty()) {
		QString prefix = "\n\n";
		if (textContent.endsWith("\n\n")) {
			prefix = "";
		} else if (textContent.endsWith('\n')) {
			prefix = "\n";
		}
		
		auto appendedStr = prefix + btnText.mid(1);
		textContent += appendedStr;
		
		log.debug(QString("forwardCopyMode MsgID %1: appended %2 chars of inline buttons").arg(item->id.bare).arg(btnText.size()));
	}

	extracted.text = textContent;

	const auto hasMedia = item->media() && (
		item->media()->document() ||
		item->media()->photo() ||
		item->media()->poll() ||
		item->media()->webpage());

	if (hasMedia) {
		log.debug(QString("forwardCopyMode MsgID %1: has media, forwarding via AyuForward").arg(item->id.bare));
		std::map<FullMsgId, TextWithTags> customTexts;
		customTexts[item->fullId()] = extracted;
		auto draft = Data::ResolvedForwardDraft(items);
		draft.options = Data::ForwardOptions::NoSenderNames;
		AyuForward::forwardMessages(session, action, false, draft);
		return true;
	} else {
		log.debug(QString("forwardCopyMode MsgID %1: text-only, sending via sendMessageSync (len=%2)").arg(item->id.bare).arg(textContent.size()));
		auto message = Api::MessageToSend(action);
		message.textWithTags = extracted;
		AyuSync::sendMessageSync(session, std::move(message));
		return true;
	}
}

bool ForwarderEngine::forwardDownloadUpload(
	Main::Session *session,
	const std::vector<not_null<HistoryItem*>> &items,
	not_null<PeerData*> destPeer,
	MsgId destTopicRootId) {

	auto &log = ForwarderLog::instance();
	log.debug(QString("forwardDownloadUpload: %1 items, sendIndividually=%2")
		.arg(items.size()).arg(_config.duSendIndividually));

	const auto history = session->data().history(destPeer);

	std::map<FullMsgId, TextWithTags> customTexts;
	for (const auto &item : items) {
		// Extract text WITH formatting tags from the original message
		auto extracted = extractText(item);
		auto textContent = extracted.text;
		const auto originalText = textContent;
		const auto originalLen = textContent.size();

		// Filename as caption: if no caption and has media filename, use it
		if (_config.filenameAsCaption && textContent.trimmed().isEmpty()) {
			const auto fname = extractFilename(item);
			if (!fname.isEmpty()) {
				const auto dotPos = fname.lastIndexOf('.');
				textContent = (dotPos > 0) ? fname.left(dotPos) : fname;
				extracted.tags.clear();
				log.debug(QString("D/U MsgID %1: using filename as caption: '%2'").arg(item->id.bare).arg(textContent));
			}
		}

		textContent = editCaption(textContent);

		// Only clear formatting tags when editCaption actually removed
		// words/patterns (changing content and invalidating offsets).
		// Don't clear for whitespace-only normalization (trimming, etc).
		const auto hasRemovals = !_config.wordsToRemove.isEmpty()
			|| !_config.removePatterns.isEmpty();
		if (hasRemovals && textContent != originalText) {
			extracted.tags.clear();
		}

		// Append inline buttons as text
		const auto btnText = extractInlineButtons(item);
		if (!btnText.isEmpty()) {
			QString prefix = "\n\n";
			if (textContent.endsWith("\n\n")) {
				prefix = "";
			} else if (textContent.endsWith('\n')) {
				prefix = "\n";
			}
			auto appendedStr = prefix + btnText.mid(1);
			textContent += appendedStr;
		}
		extracted.text = textContent;
		customTexts[item->fullId()] = extracted;
		log.debug(QString("D/U MsgID %1: caption prepared (orig=%2, edited=%3, hasButtons=%4)")
			.arg(item->id.bare).arg(originalLen).arg(textContent.size()).arg(!btnText.isEmpty()));
	}

	// Pre-download ALL media concurrently before any sending
	// Filter to only items that actually have downloadable media
	std::vector<not_null<HistoryItem*>> mediaItems;
	for (const auto &item : items) {
		if (item->media() && (item->media()->document() || item->media()->photo())) {
			mediaItems.push_back(item);
		}
	}
	if (!mediaItems.empty()) {
		log.debug(QString("Pre-downloading %1 media items concurrently before send phase").arg(mediaItems.size()));
		AyuSync::loadDocuments(session, mediaItems, [] {
			return AyuForward::g_isForwarderCanceled.load();
		});
		log.debug("Pre-download phase complete. Starting send phase.");
	} else {
		log.debug("No media items to pre-download. Proceeding to send phase.");
	}

	if (AyuForward::g_isForwarderCanceled || _stopRequested) return false;

	// Use AyuForward::forwardMessages which handles the full D/U pipeline:
	if (_config.duSendIndividually && items.size() > 1) {
		bool allOk = true;
		for (const auto &item : items) {
			if (_stopRequested || AyuForward::g_isForwarderCanceled) break;

			auto action = Api::SendAction(history);
			if (destTopicRootId) action.replyTo.topicRootId = destTopicRootId;

			std::vector<not_null<HistoryItem*>> singleItem = { item };

			auto draft = Data::ResolvedForwardDraft(singleItem);
			draft.options = Data::ForwardOptions::NoSenderNames;
			AyuForward::forwardMessages(session, action, false, draft);
			const bool ok = true;

			log.debug(QString("D/U individual item %1: %2")
				.arg(item->id.bare).arg(ok ? "OK" : "FAILED"));

			if (_config.delaySeconds > 0) {
				if (sleepWithCancelCheck(static_cast<int>(_config.delaySeconds * 1000))) break;
			}
		}
		return allOk;
	}

	// Standard grouped send via AyuForward D/U pipeline
	auto action = Api::SendAction(history);
	if (destTopicRootId) action.replyTo.topicRootId = destTopicRootId;

	auto draft = Data::ResolvedForwardDraft(items);
	draft.options = Data::ForwardOptions::NoSenderNames;

	AyuForward::forwardMessages(session, action, false, draft);
	return true;
}