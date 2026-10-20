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
#include "ayu/features/forward/forwarder_progress.h"
#include "ayu/utils/telegram_helpers.h"
#include "apiwrap.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"

#include <QRegularExpression>
#include <QThread>
#include <QEventLoop>
#include <QPointer>
#include <QSignalBlocker>
#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <optional>

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
	if (media->game() || dynamic_cast<Data::MediaDice*>(media)) return u"dice"_q;
	if (media->webpage()) return QStringLiteral("webpage");

	if (const auto doc = media->document()) {
		if (doc->sticker()) return QStringLiteral("sticker");
		if (doc->isGifv()) return QStringLiteral("gif");
		if (doc->isVideoFile() || doc->isVideoMessage()) return QStringLiteral("video");
		if (doc->isVoiceMessage()) return u"voice"_q;
		if (doc->isSong()) return u"audio"_q;
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
PeerId resolvePeerId(const QString &text) {
	return AyuForwarder::ParseForwardPeerId(text);
}

Api::SendAction MakeSendAction(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	auto result = std::optional<Api::SendAction>();
	AyuSync::RunOnMainSync([&] {
		result.emplace(session->data().history(peer));
		result->replyTo.topicRootId = topicRootId;
	});
	return *result;
}

struct FetchedMessages {
	std::vector<not_null<HistoryItem*>> items;
	TimedCountDownLatch done{ 1 };
	QString error;
	int floodWait = 0;
	int count = 0;
	MsgId lastId = 0;
	bool received = false;
};

[[nodiscard]] std::shared_ptr<FetchedMessages> FetchMessages(
		not_null<Main::Session*> session,
		PeerId sourceId,
		int topicId,
		MsgId offset,
		int endId,
		Fn<bool()> cancelled) {
	const auto result = std::make_shared<FetchedMessages>();
	const auto weak = base::make_weak(session);
	auto api = std::make_shared<MTP::Sender>(&session->mtp());
	AyuSync::RunOnMain([=] {
		const auto source = weak ? session->data().peerLoaded(sourceId) : nullptr;
		if (!source || cancelled()) {
			result->error = u"HISTORY_REQUEST_CANCELLED"_q;
			result->done.countDown();
			return;
		}
		const auto completed = [=](const MTPmessages_Messages &messages) {
			messages.match([](const MTPDmessages_messagesNotModified &) {
			}, [&](const auto &data) {
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				source->processTopics(data.vtopics());
				session->data().processMessages(data.vmessages(), NewMessageType::Existing);
				result->count = int(data.vmessages().v.size());
				for (const auto &message : data.vmessages().v) {
					const auto id = IdFromMessage(message);
					if (PeerFromMessage(message) != sourceId || id <= offset) {
						continue;
					}
					result->lastId = std::max(result->lastId, id);
					if (!endId || id.bare <= endId) {
						if (const auto item = session->data().message(sourceId, id)) {
							result->items.push_back(item);
						}
					}
				}
			});
			result->received = true;
			result->done.countDown();
		};
		const auto failed = [=](const MTP::Error &error) {
			result->error = error.type();
			result->floodWait = MTP::FloodWaitDuration(error);
			result->done.countDown();
		};
		const auto maximum = (endId && endId < std::numeric_limits<int>::max())
			? endId + 1
			: 0;
		if (topicId) {
			api->request(MTPmessages_GetReplies(
				source->input(), MTP_int(topicId),
				MTP_int(offset ? offset.bare : 1), MTP_int(0),
				MTP_int(-kFetchBatchSize), MTP_int(kFetchBatchSize),
				MTP_int(maximum), MTP_int(offset), MTP_long(0)
			)).done(completed).fail(failed).handleAllErrors().send();
		} else {
			api->request(MTPmessages_GetHistory(
				source->input(), MTP_int(offset ? offset.bare : 1), MTP_int(0),
				MTP_int(-kFetchBatchSize), MTP_int(kFetchBatchSize),
				MTP_int(maximum), MTP_int(offset), MTP_long(0)
			)).done(completed).fail(failed).handleAllErrors().send();
		}
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (!result->done.await(std::chrono::milliseconds(100))) {
		if (cancelled() || std::chrono::steady_clock::now() >= deadline) {
			break;
		}
	}
	AyuSync::RunOnMainSync([&] {
		api.reset();
		if (!result->received && result->error.isEmpty()) {
			result->error = u"HISTORY_REQUEST_UNCONFIRMED"_q;
		}
	});
	return result;
}

} // namespace

PeerId AyuForwarder::ParseForwardPeerId(const QString &text) {
	auto valid = false;
	const auto id = text.trimmed().toLongLong(&valid);
	if (!valid || !id || id == std::numeric_limits<int64>::min()) {
		return 0;
	}
	constexpr auto kChannelOffset = int64(1000000000000LL);
	if (id < -kChannelOffset) {
		const auto bare = uint64(-(id + kChannelOffset));
		return (bare <= PeerId::kChatTypeMask) ? peerFromChannel(ChannelId(bare)) : PeerId();
	} else if (id == -kChannelOffset) {
		return 0;
	} else if (id < 0) {
		return peerFromChat(ChatId(uint64(-id)));
	}
	return (uint64(id) <= PeerId::kChatTypeMask) ? peerFromUser(UserId(uint64(id))) : PeerId();
}

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
	if (_config.session) {
		_config.session->data().sessionDataAboutToBeCleared() | rpl::on_next([=] {
			stopAndWait();
		}, _lifetime);
	}
}

ForwarderEngine::~ForwarderEngine() {
	stopAndWait();
	if (g_activeEngine == this) {
		g_activeEngine = nullptr;
	}
}

bool ForwarderEngine::sleepWithCancelCheck(int64 totalMs) const {
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(totalMs);
	while (std::chrono::steady_clock::now() < deadline) {
		if (_stopRequested || AyuForward::g_isForwarderCanceled) {
			return true;
		}
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		QThread::msleep(static_cast<unsigned long>(std::min<int64>(100, std::max<int64>(1, left))));
	}
	return _stopRequested || AyuForward::g_isForwarderCanceled;
}

bool ForwarderEngine::start() {
	if (_thread || AyuForward::g_isForwarderRunning.exchange(true)) {
		return false;
	}
	_stopRequested = false;
	AyuForward::g_isForwarderCanceled = false;
	g_activeEngine = this;
	_thread = QThread::create([this] {
		try {
			run();
		} catch (const std::exception &exception) {
			ForwarderState::instance().save();
			ForwarderLog::instance().error(QString::fromUtf8(exception.what()));
			Q_EMIT error(u"Auto-Forwarder stopped after an internal error."_q);
		} catch (...) {
			ForwarderState::instance().save();
			ForwarderLog::instance().error(u"Auto-Forwarder stopped after an unknown error."_q);
		}
	});
	connect(_thread, &QThread::finished, _thread, &QThread::deleteLater);
	connect(_thread, &QThread::finished, this, [this] {
		if (g_activeEngine == this) {
			AyuForward::g_isForwarderRunning = false;
			g_activeEngine = nullptr;
		}
		_thread = nullptr;
		Q_EMIT finished();
	});
	_thread->start();
	return true;
}

void ForwarderEngine::stop() {
	_stopRequested = true;
	if (g_activeEngine == this) {
		AyuForward::g_isForwarderCanceled = true;
		if (!ForwarderState::instance().save()) {
			Q_EMIT error(u"Could not save resume progress. Check the destination before restarting."_q);
		}
		ForwarderLog::instance().append(u"Stop requested; no further groups will be sent."_q);
	}
}

void ForwarderEngine::stopAndWait() {
	if (!_thread || !_thread->isRunning()) {
		return;
	}
	const auto thread = QPointer<QThread>(_thread);
	const auto blocked = signalsBlocked();
	const auto blocker = QSignalBlocker(this);
	stop();
	auto loop = QEventLoop();
	connect(thread.data(), &QThread::finished, &loop, &QEventLoop::quit);
	if (thread && thread->isRunning()) {
		loop.exec();
	}
	if (thread) {
		thread->wait();
	}
	if (!_thread && !blocked) {
		QMetaObject::invokeMethod(this, [=] { Q_EMIT finished(); }, Qt::QueuedConnection);
	}
}

void ForwarderEngine::run() {
	auto &log = ForwarderLog::instance();
	log.setLevel(_config.logLevel.toUpper() == u"DEBUG"_q
		? ForwarderLog::Level::LogDebug
		: ForwarderLog::Level::LogInfo);
	if (!_config.session) {
		Q_EMIT error(u"No session available."_q);
		return;
	}
	auto peersAvailable = true;
	AyuSync::RunOnMainSync([&] {
		peersAvailable = (_config.session->data().peerLoaded(resolvePeerId(_config.destinationId)) != nullptr);
		for (const auto &source : _config.sourceIds) {
			peersAvailable = peersAvailable
				&& _config.session->data().peerLoaded(resolvePeerId(source));
		}
	});
	if (!peersAvailable) {
		Q_EMIT error(u"Open the source and destination chats once before starting the forwarder."_q);
		return;
	}
	ForwarderState::instance().selectContext(
		u"account_%1_destination_%2_topic_%3"_q
			.arg(_config.session->uniqueId())
			.arg(_config.destinationId).arg(_config.destinationTopicId));
	auto completed = true;
	for (auto i = 0; i < _config.sourceIds.size(); ++i) {
		const auto topics = (i < _config.sourceTopicIds.size())
			? _config.sourceTopicIds[i].split(';', Qt::KeepEmptyParts)
			: QStringList{ u"0"_q };
		for (const auto &topic : topics) {
			if (_stopRequested || AyuForward::g_isForwarderCanceled) {
				completed = false;
				break;
			}
			if (!sendHook(_config.startMessage, _config.pinStartMessage)
				|| !processSource(i, _config.sourceIds[i], topic.trimmed().toInt())) {
				completed = false;
				break;
			}
		}
		if (!completed) {
			break;
		}
	}
	if (completed && !_stopRequested && !AyuForward::g_isForwarderCanceled) {
		completed = sendHook(_config.endMessage, false);
	}
	if (!ForwarderState::instance().save()) {
		completed = false;
		Q_EMIT error(u"Could not save resume progress. Check the destination before restarting."_q);
	}
	log.append(completed
		? u"Forwarding completed; all selected messages were confirmed or filtered out."_q
		: u"Forwarding stopped; unconfirmed work remains after the saved resume position."_q);
}

bool ForwarderEngine::sendHook(const QString &text, bool pin) {
	if (text.isEmpty()) {
		return true;
	}
	const auto session = _config.session;
	const auto cancelled = [=] {
		return _stopRequested.load() || AyuForward::g_isForwarderCanceled.load();
	};
	auto destination = static_cast<PeerData*>(nullptr);
	AyuSync::RunOnMainSync([&] {
		destination = session->data().peerLoaded(resolvePeerId(_config.destinationId));
	});
	if (!destination || cancelled()) {
		return false;
	}
	auto action = MakeSendAction(session, destination, MsgId(_config.destinationTopicId));
	auto message = Api::MessageToSend(action);
	message.textWithTags.text = text;
	const auto sent = AyuSync::sendMessageSync(session, std::move(message), cancelled);
	if (!sent.sent()) {
		Q_EMIT error(u"The hook message was not confirmed. Forwarding stopped."_q);
		return false;
	}
	if (!pin || cancelled()) {
		return !cancelled();
	}
	if (!IsServerMsgId(sent.messageId.msg)) {
		Q_EMIT error(u"The start message is scheduled. Pin it after delivery."_q);
		return false;
	}
	struct PinResult {
		TimedCountDownLatch done{ 1 };
		bool pinned = false;
	};
	const auto result = std::make_shared<PinResult>();
	auto api = std::make_shared<MTP::Sender>(&session->mtp());
	AyuSync::RunOnMain([=] {
		if (cancelled()) {
			result->done.countDown();
			return;
		}
		api->request(MTPmessages_UpdatePinnedMessage(
			MTP_flags(MTPmessages_UpdatePinnedMessage::Flag::f_silent),
			destination->input(), MTP_int(sent.messageId.msg)
		)).done([=](const MTPUpdates &updates) {
			session->api().applyUpdates(updates);
			result->pinned = true;
			result->done.countDown();
		}).fail([=](const MTP::Error &error) {
			ForwarderLog::instance().error(u"Could not pin the start message: "_q + error.type());
			result->done.countDown();
		}).handleAllErrors().send();
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
	while (!result->done.await(std::chrono::milliseconds(100))) {
		if (cancelled() || std::chrono::steady_clock::now() >= deadline) {
			break;
		}
	}
	AyuSync::RunOnMainSync([&] { api.reset(); });
	if (!result->pinned) {
		Q_EMIT error(u"The start message was sent, but pinning was not confirmed."_q);
	}
	return result->pinned && !cancelled();
}

bool ForwarderEngine::processSource(
		int sourceIndex,
		const QString &sourceId,
		int topicId) {
	auto &log = ForwarderLog::instance();
	auto &state = ForwarderState::instance();
	const auto session = _config.session;
	const auto cancelled = [=] {
		return _stopRequested.load() || AyuForward::g_isForwarderCanceled.load();
	};
	const auto sourcePeerId = resolvePeerId(sourceId);
	auto sourcePeer = static_cast<PeerData*>(nullptr);
	auto destination = static_cast<PeerData*>(nullptr);
	AyuSync::RunOnMainSync([&] {
		sourcePeer = session->data().peerLoaded(sourcePeerId);
		destination = session->data().peerLoaded(resolvePeerId(_config.destinationId));
	});
	if (!sourcePeer || !destination) {
		Q_EMIT error(u"Open the source and destination chats once before starting the forwarder."_q);
		return false;
	}
	const auto stateKey = topicId
		? u"source_%1_topic_%2"_q.arg(sourceId).arg(topicId)
		: u"source_%1"_q.arg(sourceId);
	const auto rangeOverride = _config.startMessageId || _config.endMessageId;
	auto lastProcessed = _config.startMessageId
		? int64(_config.startMessageId - 1)
		: state.getLastProcessedId(stateKey);
	auto offset = MsgId(lastProcessed);
	auto pending = std::vector<not_null<HistoryItem*>>();
	auto totalSent = 0;
	auto totalSkipped = 0;
	auto cycleCount = 0;
	auto firstFetch = true;
	const auto sourceName = sourcePeer->name();
	log.append(u"Processing %1, topic %2, after message %3."_q
		.arg(sourceId).arg(topicId).arg(lastProcessed));
	const auto saveProgress = [&] {
		if (!rangeOverride) {
			state.setLastProcessedId(stateKey, lastProcessed, topicId, sourceName);
		}
	};
	while (!cancelled()) {
		if (!firstFetch && sleepWithCancelCheck(int64(_config.iterMessagesChunkWaitTime * 1000))) {
			return false;
		}
		firstFetch = false;
		auto fetched = std::shared_ptr<FetchedMessages>();
		auto fetchAttempts = 0;
		while (!cancelled()) {
			fetched = FetchMessages(session, sourcePeerId, topicId, offset,
				_config.endMessageId, cancelled);
			if (fetched->received) {
				break;
			}
			log.error(u"History request failed: "_q + fetched->error);
			if (fetched->floodWait > 0) {
				if (sleepWithCancelCheck(int64(fetched->floodWait) * 1000)) {
					return false;
				}
			} else if (++fetchAttempts <= _config.retryAttempts) {
				if (sleepWithCancelCheck(int64(std::max(1.0f, _config.retryDelay) * 1000))) {
					return false;
				}
			} else {
				Q_EMIT error(u"Could not fetch source history. Resume progress was preserved."_q);
				return false;
			}
		}
		if (cancelled() || !fetched) {
			return false;
		}
		const auto exhausted = !fetched->count || fetched->lastId <= offset
			|| (_config.endMessageId && fetched->lastId.bare >= _config.endMessageId);
		pending.insert(pending.end(), fetched->items.begin(), fetched->items.end());
		std::sort(pending.begin(), pending.end(), [](const auto a, const auto b) {
			return a->id < b->id;
		});
		pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
		if (fetched->lastId > offset) {
			offset = fetched->lastId;
		}
		auto items = AyuForwarder::TakeCompleteHistoryPage(pending, exhausted, [](const auto item) {
			return item->groupId().value;
		});
		auto ids = std::vector<std::int64_t>();
		for (const auto item : items) {
			ids.push_back(item->id.bare);
		}
		auto progress = AyuForwarder::BatchProgress(std::move(ids), lastProcessed);
		const auto complete = [&](not_null<HistoryItem*> item) {
			progress.complete(item->id.bare);
			lastProcessed = progress.lastProcessedId();
			saveProgress();
		};
		auto groups = std::vector<std::vector<not_null<HistoryItem*>>>();
		auto groupIndices = std::map<uint64, std::size_t>();
		for (const auto item : items) {
			auto group = item->groupId().value;
			if (_config.splitDocAlbums && _config.mode != ForwardingMode::NativeDropAuthor
				&& classifyItem(item) == u"document"_q) {
				group = 0;
			}
			if (!group) {
				groups.push_back({ item });
			} else {
				const auto [it, inserted] = groupIndices.emplace(group, groups.size());
				if (inserted) {
					groups.emplace_back();
				}
				groups[it->second].push_back(item);
			}
		}
		for (const auto &group : groups) {
			if (cancelled()) {
				return false;
			}
			auto selected = std::vector<not_null<HistoryItem*>>();
			const auto allowAlbum = group.size() > 1
				&& _config.allowedTypes.contains(u"album"_q, Qt::CaseInsensitive);
			for (const auto item : group) {
				const auto type = classifyItem(item);
				if (type.isEmpty() || (!allowAlbum && !passesTypeFilter(type))
					|| !passesKeywordFilter(item, type)) {
					++totalSkipped;
					complete(item);
				} else {
					selected.push_back(item);
				}
			}
			if (selected.empty()) {
				continue;
			}
			if (_config.messageLimit > 0 && cycleCount >= _config.messageLimit) {
				if (sleepWithCancelCheck(int64(_config.fetchCycleDelaySeconds * 1000))) {
					return false;
				}
				cycleCount = 0;
			}
			auto success = false;
			for (auto attempt = 0; attempt <= _config.retryAttempts && !cancelled(); ++attempt) {
				_lastSendResult = {};
				switch (_config.mode) {
				case ForwardingMode::NativeDropAuthor:
					success = forwardNativeDropAuthor(session, selected, destination,
						MsgId(_config.destinationTopicId));
					break;
				case ForwardingMode::Copy:
					success = forwardCopyMode(session, selected, destination,
						MsgId(_config.destinationTopicId));
					break;
				case ForwardingMode::DownloadUpload:
					success = forwardDownloadUpload(session, selected, destination,
						MsgId(_config.destinationTopicId));
					break;
				}
				if (success || !_lastSendResult.retryable || _lastSendResult.confirmed
					|| _lastSendResult.outcome != AyuSync::SendOutcome::Failed
					|| attempt == _config.retryAttempts
					|| (_config.failureThreshold > 0 && attempt + 1 >= _config.failureThreshold)) {
					break;
				}
				// Keep the established conservative retry schedule: ten, twenty,
				// forty, sixty minutes, then twenty-minute increments. A configured
				// retry delay may increase this minimum. Only failures known to have
				// sent nothing are retried; uncertain delivery needs a manual check.
				const auto minutes = attempt == 0 ? 10 : attempt == 1 ? 20
					: attempt == 2 ? 40 : 60 + (attempt - 3) * 20;
				const auto wait = std::max(int64(minutes) * 60000, int64(_config.retryDelay * 1000));
				log.warning(u"Retrying message %1 after %2 seconds."_q
					.arg(selected.front()->id.bare).arg(wait / 1000));
				if (sleepWithCancelCheck(wait)) {
					return false;
				}
			}
			if (!success) {
				state.save();
				log.error(u"Stopped at message %1: %2 (confirmed items: %3)."_q
					.arg(selected.front()->id.bare).arg(_lastSendResult.error)
					.arg(_lastSendResult.confirmed));
				if (!cancelled() && !_lastSendResult.error.isEmpty()) {
					Q_EMIT error(u"Forwarding stopped at an unconfirmed message. Check the destination before resuming."_q);
				}
				return false;
			}
			for (const auto item : selected) {
				complete(item);
			}
			totalSent += int(selected.size());
			cycleCount += int(selected.size());
			if ((totalSent + totalSkipped) % kPeriodicStateSaveInterval == 0 && !state.save()) {
				Q_EMIT error(u"Could not save resume progress. Forwarding stopped."_q);
				return false;
			}
			Q_EMIT progress(sourceIndex, totalSent + totalSkipped, 0);
			if (sleepWithCancelCheck(int64(_config.delaySeconds * 1000))) {
				return false;
			}
		}
		if (!state.save()) {
			Q_EMIT error(u"Could not save resume progress. Forwarding stopped."_q);
			return false;
		}
		if (exhausted) {
			log.append(u"Source finished: %1 sent, %2 filtered out; resume ID %3."_q
				.arg(totalSent).arg(totalSkipped).arg(lastProcessed));
			return true;
		}
	}
	return false;
}

QString ForwarderEngine::classifyMessageType(not_null<HistoryItem*> item) const {
	return classifyItem(item);
}

bool ForwarderEngine::passesTypeFilter(const QString &type) const {
	if (_config.allowedTypes.isEmpty()) return true;  // no filter = allow all
	return _config.allowedTypes.contains(type, Qt::CaseInsensitive);
}

bool ForwarderEngine::passesKeywordFilter(not_null<HistoryItem*> item, const QString &type) const {
	auto &log = ForwarderLog::instance();
	if (_config.primaryKeywords.isEmpty() && _config.secondaryKeywords.isEmpty()) return true;

	// Keyword filter only applies to file-based media types.
	// Non-file media and text always pass through.
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
	if (!_config.primaryKeywords.isEmpty() && !checkKeywords(text, _config.primaryKeywords)) {
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

TextWithTags ForwarderEngine::prepareCaption(not_null<HistoryItem*> item) const {
	auto result = extractText(item);
	if (_config.filenameAsCaption && result.text.trimmed().isEmpty()) {
		const auto filename = extractFilename(item);
		const auto dot = filename.lastIndexOf('.');
		result.text = (dot > 0) ? filename.left(dot) : filename;
		result.tags.clear();
	}
	const auto edited = editCaption(result.text);
	if (edited != result.text) {
		result.text = edited;
		result.tags.clear();
	}
	const auto buttons = extractInlineButtons(item);
	if (!buttons.isEmpty()) {
		if (!result.text.isEmpty() && !result.text.endsWith(u"\n\n"_q)) {
			result.text += result.text.endsWith('\n') ? u"\n"_q : u"\n\n"_q;
		}
		result.text += buttons.mid(1);
	}
	return result;
}

bool ForwarderEngine::forwardNativeDropAuthor(
		Main::Session *session,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<PeerData*> destPeer,
		MsgId destTopicRootId) {
	auto action = MakeSendAction(session, destPeer, destTopicRootId);
	const auto options = _config.dropCaptions
		? Data::ForwardOptions::NoNamesAndCaptions
		: Data::ForwardOptions::NoSenderNames;
	const auto cancelled = [=] {
		return _stopRequested.load() || AyuForward::g_isForwarderCanceled.load();
	};
	const auto needsCopy = ranges::any_of(items, [](const auto item) {
		return item->isSavedMusicItem()
			|| AyuForward::isAyuForwardNeeded(item)
			|| AyuForward::isFullAyuForwardNeeded(item);
	});
	if (needsCopy) {
		auto draft = Data::ResolvedForwardDraft(items);
		draft.options = options;
		_lastSendResult = AyuForward::forwardMessages(
			session, action, false, draft, {}, cancelled, _config.concurrentBatchSize);
	} else {
		_lastSendResult = AyuSync::forwardMessagesSync(session, items, action, options, cancelled);
	}
	return _lastSendResult.sent();
}

bool ForwarderEngine::forwardCopyMode(
		Main::Session *session,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<PeerData*> destPeer,
		MsgId destTopicRootId) {
	auto action = MakeSendAction(session, destPeer, destTopicRootId);
	auto captions = std::map<FullMsgId, TextWithTags>();
	for (const auto item : items) {
		captions.emplace(item->fullId(), prepareCaption(item));
	}
	auto draft = Data::ResolvedForwardDraft(items);
	draft.options = Data::ForwardOptions::NoSenderNames;
	_lastSendResult = AyuForward::forwardMessages(session, action, false, draft, captions, [=] {
		return _stopRequested.load() || AyuForward::g_isForwarderCanceled.load();
	}, _config.concurrentBatchSize);
	return _lastSendResult.sent();
}

bool ForwarderEngine::forwardDownloadUpload(
		Main::Session *session,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<PeerData*> destPeer,
		MsgId destTopicRootId) {
	if (!_config.duSendIndividually || items.size() <= 1) {
		return forwardCopyMode(session, items, destPeer, destTopicRootId);
	}
	auto confirmed = 0;
	for (const auto item : items) {
		if (_stopRequested || AyuForward::g_isForwarderCanceled) {
			_lastSendResult = { AyuSync::SendOutcome::Cancelled };
			_lastSendResult.confirmed = confirmed;
			return false;
		}
		const auto success = forwardCopyMode(session, { item }, destPeer, destTopicRootId);
		_lastSendResult.confirmed += confirmed;
		if (!success) {
			return false;
		}
		confirmed = _lastSendResult.confirmed;
		if (item != items.back() && sleepWithCancelCheck(int64(_config.delaySeconds * 1000))) {
			_lastSendResult.outcome = AyuSync::SendOutcome::Cancelled;
			return false;
		}
	}
	return true;
}
