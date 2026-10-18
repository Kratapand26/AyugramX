#include "ayu/features/chat_stickers/chat_stickers.h"

#include "base/unixtime.h"
#include "core/version.h"
#include "data/stickers/data_stickers.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/serialize_document.h"
#include "storage/storage_account.h"
#include "ui/text/format_values.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <QtCore/QDataStream>
#include <QtCore/QIODevice>

#include <algorithm>
#include <iterator>
#include <limits>

namespace Ayu {
namespace {

constexpr auto kEnabledPref = "ayu_chat_stickers_enabled"_cs;
constexpr auto kRetryAtPref = "ayu_chat_stickers_retry_at"_cs;
constexpr auto kCacheLimit = 8;
constexpr auto kPageSize = 100;
constexpr auto kRequestInterval = crl::time(2000);
constexpr auto kSaveDelay = crl::time(500);
constexpr auto kRestoreBatch = 128;

[[nodiscard]] DocumentId StickerId(DocumentData *document) {
	const auto sticker = document ? document->sticker() : nullptr;
	return (sticker && sticker->setType != Data::StickersType::Emoji)
		? document->id
		: 0;
}

[[nodiscard]] const MTPDocument *StickerDocument(const MTPDmessage &item) {
	const auto media = item.vmedia();
	if (!media || media->type() != mtpc_messageMediaDocument) {
		return nullptr;
	}
	const auto document = media->c_messageMediaDocument().vdocument();
	if (!document || document->type() != mtpc_document) {
		return nullptr;
	}
	const auto &attributes = document->c_document().vattributes().v;
	return ranges::any_of(attributes, [](const auto &attribute) {
		return attribute.type() == mtpc_documentAttributeSticker;
	}) ? document : nullptr;
}

} // namespace

ChatStickers::ChatStickers(not_null<Main::Session*> session)
: _session(session)
, _enabled(session->local().readPref<bool>(kEnabledPref))
, _saveTimer([=] {
	for (const auto &cache : _caches) {
		save(cache);
	}
})
, _requestTimer([=] {
	updateRequestTimer();
	_changes.fire({});
})
, _api(&session->mtp()) {
	_retryAt = session->local().readPref<QString>(kRetryAtPref).toLongLong();
	updateRequestTimer();
	_session->changes().messageUpdates(
		Data::MessageUpdate::Flag::NewAdded
		| Data::MessageUpdate::Flag::Edited
		| Data::MessageUpdate::Flag::NewMaybeAdded
		| Data::MessageUpdate::Flag::ReplyToTopAdded
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		applyItem(update.item);
	}, _lifetime);
	_session->data().itemRemoved() | rpl::on_next([=](
			not_null<const HistoryItem*> item) {
		if (_stopping) {
			return;
		}
		auto changed = false;
		for (const auto &cache : _caches) {
			if (cache->peerId == item->history()->peer->id) {
				if (!cache->loaded) {
					cache->removedWhileLoading.emplace(item->id);
				}
				if (cache->data.remove(item->id)) {
					++cache->revision;
					markDirty(cache.get());
					changed = true;
				}
			}
		}
		if (changed) {
			_changes.fire({});
		}
	}, _lifetime);
	_session->data().sessionDataAboutToBeCleared() | rpl::on_next([=] {
		_saveTimer.cancel();
		cancel(_requestId);
		for (const auto &cache : _caches) {
			save(cache, true);
		}
		_stopping = true;
		_requestTimer.cancel();
	}, _lifetime);
}

bool ChatStickers::enabled() const {
	return _enabled.current();
}

rpl::producer<bool> ChatStickers::enabledValue() const {
	return _enabled.value();
}

void ChatStickers::setEnabled(bool enabled) {
	if (enabled == this->enabled()) {
		return;
	}
	_session->local().writePref<bool>(kEnabledPref, enabled);
	_enabled = enabled;
	if (!enabled) {
		cancel(_requestId);
		for (const auto &cache : _caches) {
			save(cache, true);
			++cache->generation;
		}
		_caches.clear();
	}
}

rpl::producer<> ChatStickers::changes() const {
	return _changes.events();
}

bool ChatStickers::canRequest() const {
	return !_stopping && enabled() && !_requestId && !requestWait();
}

QString ChatStickers::requestStatus() const {
	if (_retryAt > base::unixtime::now()) {
		return waitText();
	} else if (_requestId) {
		return tr::ayu_ChatStickersLoading(tr::now);
	} else if (const auto wait = requestWait()) {
		return tr::ayu_ChatStickersCooldown(
			tr::now,
			lt_duration,
			Ui::FormatDurationWords((wait + 999) / 1000));
	}
	return {};
}

crl::time ChatStickers::requestWait() const {
	return std::max<crl::time>({
		crl::time(0),
		_nextRequestAt - crl::now(),
		(_retryAt - int64(base::unixtime::now())) * crl::time(1000),
	});
}

void ChatStickers::postponeRequests() {
	_nextRequestAt = crl::now() + kRequestInterval;
	updateRequestTimer();
}

void ChatStickers::updateRequestTimer() {
	_requestTimer.cancel();
	if (const auto wait = requestWait()) {
		_requestTimer.callOnce(std::min(
			wait,
			crl::time(std::numeric_limits<int>::max())));
	}
}

std::shared_ptr<ChatStickersCache> ChatStickers::cache(
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	Expects(&peer->session() == _session);
	const auto i = ranges::find_if(_caches, [&](const auto &cache) {
		return cache->peerId == peer->id
			&& cache->topicRootId == topicRootId;
	});
	if (i != end(_caches)) {
		_caches.splice(begin(_caches), _caches, i);
	} else {
		auto cache = std::make_shared<ChatStickersCache>();
		cache->peerId = peer->id;
		cache->topicRootId = topicRootId;
		_caches.push_front(cache);
		load(cache);
	}
	const auto result = _caches.front();
	trim();
	const auto revision = result->revision;
	seed(result.get());
	if (result->revision != revision) {
		markDirty(result.get());
		_changes.fire({});
	}
	return result;
}

void ChatStickers::applyItem(not_null<HistoryItem*> item) {
	if (_stopping || !IsServerMsgId(item->id)) {
		return;
	}
	auto changed = false;
	for (const auto &cache : _caches) {
		if (cache->peerId != item->history()->peer->id) {
			continue;
		}
		const auto matches = !cache->topicRootId
			|| item->topicRootId() == cache->topicRootId;
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		const auto id = matches ? StickerId(document) : 0;
		if (!cache->loaded && !id) {
			cache->removedWhileLoading.emplace(item->id);
		}
		if (cache->data.add(
			item->id,
			id,
			item->date())) {
			++cache->revision;
			markDirty(cache.get());
			changed = true;
		}
		if (id && cache->data.contains(item->id)) {
			remember(cache.get(), document);
		}
	}
	if (changed) {
		_changes.fire({});
	}
}

void ChatStickers::seed(not_null<ChatStickersCache*> cache) {
	auto remembered = std::set<DocumentId>();
	_session->data().enumerateMessages(cache->peerId, [=, &remembered](
			not_null<HistoryItem*> item) {
		if (!IsServerMsgId(item->id)) {
			return;
		}
		const auto matches = !cache->topicRootId
			|| item->topicRootId() == cache->topicRootId;
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		const auto id = matches ? StickerId(document) : 0;
		cache->revision += cache->data.add(
			item->id,
			id,
			item->date());
		if (id
			&& cache->data.contains(item->id)
			&& remembered.emplace(id).second) {
			remember(cache, document);
		}
	});
}

QString ChatStickers::waitText() const {
	return tr::ayu_ChatStickersWait(
		tr::now,
		lt_duration,
		Ui::FormatDurationWords(std::max(
			int64(1),
			_retryAt - base::unixtime::now())));
}

mtpRequestId ChatStickers::loadMore(
		std::shared_ptr<ChatStickersCache> cache,
		Fn<void(QString)> done) {
	return requestPage(std::move(cache), std::move(done), false);
}

mtpRequestId ChatStickers::refresh(
		std::shared_ptr<ChatStickersCache> cache,
		Fn<void(QString)> done) {
	return requestPage(std::move(cache), std::move(done), true);
}

mtpRequestId ChatStickers::requestPage(
		std::shared_ptr<ChatStickersCache> cache,
		Fn<void(QString)> done,
		bool refresh) {
	if (_stopping || !enabled()) {
		done(tr::ayu_ChatStickersEnableFirst(tr::now));
		return 0;
	} else if (!cache->loaded) {
		done({});
		return 0;
	} else if (!refresh && (cache->finished || cache->data.full())) {
		done({});
		return 0;
	} else if (_retryAt > base::unixtime::now()) {
		done(waitText());
		return 0;
	} else if (_requestId || crl::now() < _nextRequestAt) {
		done(tr::ayu_ChatStickersBusy(tr::now));
		return 0;
	}
	postponeRequests();
	_active = cache;
	const auto peer = _session->data().peer(cache->peerId);
	const auto generation = cache->generation;
	const auto completed = [=](const MTPmessages_Messages &result) {
		if (_stopping || cache->generation != generation) {
			return;
		}
		_requestId = 0;
		_active = nullptr;
		postponeRequests();
		applyPage(cache.get(), result, refresh);
		markDirty(cache.get());
		flush(cache);
		_changes.fire({});
		done({});
	};
	const auto failed = [=](const MTP::Error &error) {
		if (_stopping || cache->generation != generation) {
			return;
		}
		_requestId = 0;
		_active = nullptr;
		auto text = QString();
		if (MTP::IsFloodError(error)) {
			_retryAt = int64(base::unixtime::now())
				+ std::max(1, MTP::FloodWaitDuration(error));
			_session->local().writePref<QString>(
				kRetryAtPref,
				QString::number(_retryAt));
			text = waitText();
		} else if (!MTP::IgnoreError(error)) {
			text = tr::ayu_ChatStickersUnavailable(tr::now);
		}
		postponeRequests();
		_changes.fire({});
		done(text);
	};
	const auto offset = refresh ? MsgId(0) : cache->offsetId;
	if (cache->topicRootId) {
		_requestId = _api.request(MTPmessages_GetReplies(
			peer->input(),
			MTP_int(cache->topicRootId),
			MTP_int(offset),
			MTP_int(0),
			MTP_int(0),
			MTP_int(kPageSize),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done(completed).fail(failed).handleAllErrors().send();
	} else {
		_requestId = _api.request(MTPmessages_GetHistory(
			peer->input(),
			MTP_int(offset),
			MTP_int(0),
			MTP_int(0),
			MTP_int(kPageSize),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done(completed).fail(failed).handleAllErrors().send();
	}
	_changes.fire({});
	return _requestId;
}

void ChatStickers::applyPage(
		not_null<ChatStickersCache*> cache,
		const MTPmessages_Messages &result,
		bool refresh) {
	auto remembered = std::set<DocumentId>();
	auto oldest = MsgId(0);
	auto count = 0;
	const auto record = [&](const MTPint &value) {
		const auto id = MsgId(value.v);
		if (IsServerMsgId(id)) {
			oldest = oldest ? std::min(oldest, id) : id;
			++count;
		}
	};
	result.match([&](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		for (const auto &message : data.vmessages().v) {
			message.match([&](const MTPDmessage &item) {
				if (peerFromMTP(item.vpeer_id()) != cache->peerId) {
					return;
				}
				record(item.vid());
				const auto value = StickerDocument(item);
				const auto document = value
					? _session->data().processDocument(*value).get()
					: nullptr;
				cache->revision += cache->data.add(
					MsgId(item.vid().v),
					StickerId(document),
					item.vdate().v);
				if (document
					&& cache->data.contains(MsgId(item.vid().v))
					&& remembered.emplace(document->id).second) {
					remember(cache, document);
				}
			}, [&](const MTPDmessageService &item) {
				if (peerFromMTP(item.vpeer_id()) == cache->peerId) {
					record(item.vid());
				}
			}, [&](const MTPDmessageEmpty &item) {
				const auto peer = item.vpeer_id();
				if (!peer || peerFromMTP(*peer) == cache->peerId) {
					record(item.vid());
				}
			});
		}
	});
	if (refresh && cache->offsetId) {
		return;
	}
	cache->scanned = int(std::min(
		int64(std::numeric_limits<int>::max()),
		int64(cache->scanned) + count));
	cache->finished = !oldest
		|| (cache->offsetId && oldest >= cache->offsetId);
	if (oldest) {
		cache->offsetId = oldest;
	}
}

bool ChatStickers::loading(
		const std::shared_ptr<ChatStickersCache> &cache) const {
	return _requestId && _active == cache;
}

void ChatStickers::cancel(mtpRequestId requestId) {
	if (requestId && requestId == _requestId) {
		_api.request(base::take(_requestId)).cancel();
		_active = nullptr;
		postponeRequests();
		_changes.fire({});
	}
}

struct ChatStickers::Restore {
	SavedChatStickers saved;
	size_t documentIndex = 0;
	size_t messageIndex = 0;
	bool valid = true;
};

void ChatStickers::load(const std::shared_ptr<ChatStickersCache> &cache) {
	const auto weak = base::make_weak(this);
	const auto generation = cache->generation;
	const auto accountId = _session->uniqueId();
	const auto peerId = cache->peerId;
	const auto topicRootId = cache->topicRootId;
	_session->data().cache().get(ChatStickersCacheKey(peerId, topicRootId), [=](
			QByteArray bytes) {
		auto saved = ParseChatStickersCache(
			bytes,
			accountId,
			peerId,
			topicRootId,
			AppVersion);
		crl::on_main(weak, [=, saved = std::move(saved)]() mutable {
			if (weak->_stopping || cache->generation != generation) {
				return;
			}
			if (saved) {
				auto state = std::make_shared<Restore>();
				state->saved = std::move(*saved);
				weak->restore(cache, state, generation);
			} else {
				cache->loaded = true;
				cache->removedWhileLoading.clear();
				if (!bytes.isEmpty()) {
					weak->_session->data().cache().remove(
						ChatStickersCacheKey(cache->peerId, cache->topicRootId));
				}
				weak->save(cache);
				weak->trim();
				weak->_changes.fire({});
			}
		});
	});
}

void ChatStickers::restore(
		const std::shared_ptr<ChatStickersCache> &cache,
		const std::shared_ptr<Restore> &state,
		uint64 generation) {
	if (_stopping || cache->generation != generation) {
		return;
	}
	const auto &saved = state->saved;
	const auto documentEnd = std::min(
		state->documentIndex + kRestoreBatch,
		saved.documents.size());
	while (state->documentIndex != documentEnd) {
		const auto &record = saved.documents[state->documentIndex++];
		auto document = _session->data().document(record.id).get();
		if (!StickerId(document)) {
			auto stream = QDataStream(record.bytes);
			stream.setVersion(QDataStream::Qt_5_1);
			document = Serialize::Document::readStickerFromStream(
				_session,
				saved.appVersion,
				stream,
				Serialize::Document::StickerSetInfo(
					record.setId,
					record.setAccessHash,
					record.setShortName));
			if (stream.status() != QDataStream::Ok
				|| !stream.atEnd()
				|| !document
				|| document->id != record.id
				|| !StickerId(document)) {
				state->valid = false;
				continue;
			}
			if (document->sticker()->set.empty()) {
				document->sticker()->set = {
					record.setId,
					record.setAccessHash,
					record.setShortName,
				};
			}
		}
		remember(cache.get(), document);
	}
	if (state->documentIndex == saved.documents.size() && state->valid) {
		const auto messageEnd = std::min(
			state->messageIndex + kRestoreBatch,
			saved.messages.size());
		while (state->messageIndex != messageEnd) {
			const auto &message = saved.messages[state->messageIndex++];
			if (!cache->data.contains(message.messageId)
				&& !cache->removedWhileLoading.contains(message.messageId)) {
				cache->revision += cache->data.add(
					message.messageId,
					message.documentId,
					message.date);
			}
		}
	}
	if (state->documentIndex != saved.documents.size()
		|| (state->valid && state->messageIndex != saved.messages.size())) {
		crl::on_main(base::make_weak(this), [=] {
			restore(cache, state, generation);
		});
		return;
	}
	if (state->valid) {
		cache->offsetId = saved.offsetId;
		cache->scanned = saved.scanned;
		cache->finished = saved.finished;
	}
	cache->loaded = true;
	cache->removedWhileLoading.clear();
	seed(cache.get());
	markDirty(cache.get());
	_changes.fire({});
}

void ChatStickers::remember(
		not_null<ChatStickersCache*> cache,
		not_null<DocumentData*> document) {
	const auto sticker = document->sticker();
	if (!sticker) {
		return;
	}
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	Serialize::Document::writeToStream(stream, document);
	if (cache->documentsBytes + bytes.size() > kChatStickersCacheBytesLimit) {
		auto used = std::set<DocumentId>{ document->id };
		for (const auto &entry : cache->data.entries()) {
			used.emplace(entry.documentId);
		}
		for (auto i = begin(cache->documents); i != end(cache->documents);) {
			if (used.contains(i->first)) {
				++i;
			} else {
				cache->documentsBytes -= i->second.bytes.size();
				i = cache->documents.erase(i);
			}
		}
	}
	const auto existing = cache->documents.find(document->id);
	const auto oldSize = (existing != end(cache->documents))
		? existing->second.bytes.size()
		: 0;
	if (stream.status() != QDataStream::Ok
		|| bytes.size() > kChatStickerDocumentBytesLimit
		|| cache->documentsBytes - oldSize + bytes.size()
			> kChatStickersCacheBytesLimit) {
		cache->saveFailed = true;
		return;
	}
	const auto &set = sticker->set;
	if (existing != end(cache->documents)
		&& existing->second.bytes == bytes
		&& existing->second.setId == set.id
		&& existing->second.setAccessHash == set.accessHash
		&& existing->second.setShortName == set.shortName) {
		return;
	}
	cache->documentsBytes += bytes.size() - oldSize;
	cache->documents[document->id] = {
		document->id,
		set.id,
		set.accessHash,
		set.shortName,
		std::move(bytes),
	};
	markDirty(cache);
}

void ChatStickers::markDirty(not_null<ChatStickersCache*> cache) {
	cache->dirty = true;
	if (!_stopping && cache->loaded && !_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
}

void ChatStickers::flush(const std::shared_ptr<ChatStickersCache> &cache) {
	save(cache);
}

void ChatStickers::save(
		const std::shared_ptr<ChatStickersCache> &cache,
		bool synchronous) {
	if (_stopping
		|| !cache->loaded
		|| (!cache->dirty && !(synchronous && cache->saving))
		|| (cache->saving && !synchronous)) {
		return;
	}
	if (synchronous) {
		++cache->generation;
	}
	auto saved = SavedChatStickers{
		.accountId = _session->uniqueId(),
		.peerId = cache->peerId,
		.topicRootId = cache->topicRootId,
		.offsetId = cache->offsetId,
		.scanned = cache->scanned,
		.appVersion = AppVersion,
		.finished = cache->finished,
		.messages = cache->data.messages(),
	};
	for (const auto &entry : cache->data.entries()) {
		const auto i = cache->documents.find(entry.documentId);
		if (i == end(cache->documents)) {
			cache->saveFailed = true;
			cache->saving = false;
			_changes.fire({});
			return;
		}
		saved.documents.push_back(i->second);
	}
	cache->dirty = false;
	cache->saving = true;
	const auto generation = cache->generation;
	const auto weak = base::make_weak(this);
	if (synchronous) {
		store(cache, generation, SerializeChatStickersCache(saved));
	} else {
		crl::async([=, saved = std::move(saved)] {
			auto bytes = SerializeChatStickersCache(saved);
			crl::on_main(weak, [=, bytes = std::move(bytes)]() mutable {
				weak->store(cache, generation, std::move(bytes));
			});
		});
	}
}

void ChatStickers::store(
		const std::shared_ptr<ChatStickersCache> &cache,
		uint64 generation,
		QByteArray bytes) {
	if (_stopping || cache->generation != generation) {
		return;
	}
	if (bytes.isEmpty()) {
		cache->saving = false;
		cache->saveFailed = true;
		cache->dirty = true;
		_changes.fire({});
		return;
	}
	const auto weak = base::make_weak(this);
	_session->data().cache().put(
		ChatStickersCacheKey(cache->peerId, cache->topicRootId),
		Storage::Cache::Database::TaggedValue(
			std::move(bytes),
			Data::kChatStickersCacheTag),
		[=](Storage::Cache::Error error) {
			crl::on_main(weak, [=] {
				if (weak->_stopping || cache->generation != generation) {
					return;
				}
				cache->saving = false;
				cache->saveFailed = (error.type != Storage::Cache::Error::Type::None);
				if (cache->saveFailed) {
					cache->dirty = true;
				} else {
					weak->save(cache);
					weak->trim();
				}
				weak->_changes.fire({});
			});
		});
}

void ChatStickers::trim() {
	while (_caches.size() > kCacheLimit) {
		const auto unused = std::find_if(
			_caches.rbegin(),
			_caches.rend(),
			[](const auto &cache) {
				return cache.use_count() == 1
					&& cache->loaded
					&& !cache->dirty
					&& !cache->saving;
			});
		if (unused == _caches.rend()) {
			break;
		}
		_caches.erase(std::next(unused).base());
	}
}

void ChatStickers::reset(not_null<ChatStickersCache*> cache) {
	++cache->generation;
	++cache->revision;
	cache->data.clear();
	cache->documents.clear();
	cache->removedWhileLoading.clear();
	cache->documentsBytes = 0;
	cache->offsetId = 0;
	cache->scanned = 0;
	cache->finished = false;
	cache->loaded = true;
	cache->dirty = false;
	cache->saving = false;
	cache->saveFailed = false;
}

void ChatStickers::clear(const std::shared_ptr<ChatStickersCache> &cache) {
	if (loading(cache)) {
		cancel(_requestId);
	}
	reset(cache.get());
	_session->data().cache().remove(
		ChatStickersCacheKey(cache->peerId, cache->topicRootId));
	_changes.fire({});
}

void ChatStickers::resetAll() {
	_saveTimer.cancel();
	cancel(_requestId);
	for (const auto &cache : _caches) {
		reset(cache.get());
	}
	_changes.fire({});
}

} // namespace Ayu
