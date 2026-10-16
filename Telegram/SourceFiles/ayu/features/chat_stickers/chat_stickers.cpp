#include "ayu/features/chat_stickers/chat_stickers.h"

#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/text/format_values.h"

#include <algorithm>
#include <iterator>

namespace Ayu {
namespace {

constexpr auto kEnabledPref = "ayu_chat_stickers_enabled"_cs;
constexpr auto kRetryAtPref = "ayu_chat_stickers_retry_at"_cs;
constexpr auto kCacheLimit = 8;
constexpr auto kPageSize = 100;
constexpr auto kRequestInterval = crl::time(1000);

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
, _api(&session->mtp()) {
	_retryAt = session->local().readPref<QString>(kRetryAtPref).toLongLong();
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
		auto changed = false;
		for (const auto &cache : _caches) {
			if (cache->peerId == item->history()->peer->id) {
				if (cache->data.remove(item->id)) {
					++cache->revision;
					changed = true;
				}
			}
		}
		if (changed) {
			_changes.fire({});
		}
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
		_caches.clear();
	}
}

rpl::producer<> ChatStickers::changes() const {
	return _changes.events();
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
		_caches.push_front(std::move(cache));
	}
	const auto result = _caches.front();
	while (_caches.size() > kCacheLimit) {
		const auto unused = std::find_if(
			_caches.rbegin(),
			_caches.rend(),
			[](const auto &cache) { return cache.use_count() == 1; });
		if (unused == _caches.rend()) {
			break;
		}
		_caches.erase(std::next(unused).base());
	}
	const auto revision = result->revision;
	seed(result.get());
	if (result->revision != revision) {
		_changes.fire({});
	}
	return result;
}

void ChatStickers::applyItem(not_null<HistoryItem*> item) {
	if (!IsServerMsgId(item->id)) {
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
		if (cache->data.add(
			item->id,
			matches ? StickerId(document) : 0,
			item->date())) {
			++cache->revision;
			changed = true;
		}
	}
	if (changed) {
		_changes.fire({});
	}
}

void ChatStickers::seed(not_null<ChatStickersCache*> cache) {
	_session->data().enumerateMessages(cache->peerId, [=](
			not_null<HistoryItem*> item) {
		if (!IsServerMsgId(item->id)) {
			return;
		}
		const auto matches = !cache->topicRootId
			|| item->topicRootId() == cache->topicRootId;
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		cache->revision += cache->data.add(
			item->id,
			matches ? StickerId(document) : 0,
			item->date());
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
	if (!enabled()) {
		done(tr::ayu_ChatStickersEnableFirst(tr::now));
		return 0;
	} else if (cache->finished || cache->data.full()) {
		done({});
		return 0;
	} else if (_retryAt > base::unixtime::now()) {
		done(waitText());
		return 0;
	} else if (_requestId || crl::now() < _nextRequestAt) {
		done(tr::ayu_ChatStickersBusy(tr::now));
		return 0;
	}
	_nextRequestAt = crl::now() + kRequestInterval;
	_active = cache;
	const auto peer = _session->data().peer(cache->peerId);
	const auto completed = [=](const MTPmessages_Messages &result) {
		_requestId = 0;
		_active = nullptr;
		applyPage(cache.get(), result);
		_changes.fire({});
		done({});
	};
	const auto failed = [=](const MTP::Error &error) {
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
		_changes.fire({});
		done(text);
	};
	if (cache->topicRootId) {
		_requestId = _api.request(MTPmessages_GetReplies(
			peer->input(),
			MTP_int(cache->topicRootId),
			MTP_int(cache->offsetId),
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
			MTP_int(cache->offsetId),
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
		const MTPmessages_Messages &result) {
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
	cache->scanned += count;
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
		_changes.fire({});
	}
}

void ChatStickers::restart(const std::shared_ptr<ChatStickersCache> &cache) {
	if (loading(cache)) {
		cancel(_requestId);
	}
	if (cache->data.full()) {
		cache->data.clear();
		++cache->revision;
	}
	cache->offsetId = 0;
	cache->scanned = 0;
	cache->finished = false;
	seed(cache.get());
	_changes.fire({});
}

} // namespace Ayu
