#pragma once

#include "ayu/features/chat_stickers/chat_stickers_cache.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "mtproto/sender.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"

#include <list>
#include <set>

class DocumentData;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ayu {

struct ChatStickersCache {
	PeerId peerId = 0;
	MsgId topicRootId = 0;
	ChatStickersData data;
	MsgId offsetId = 0;
	int scanned = 0;
	int revision = 0;
	uint64 generation = 0;
	std::map<DocumentId, SavedChatStickerDocument> documents;
	std::set<MsgId> removedWhileLoading;
	int documentsBytes = 0;
	bool finished = false;
	bool loaded = false;
	bool dirty = false;
	bool saving = false;
	bool saveFailed = false;
};

class ChatStickers final : public base::has_weak_ptr {
public:
	explicit ChatStickers(not_null<Main::Session*> session);

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	void setEnabled(bool enabled);
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] bool canRequest() const;
	[[nodiscard]] QString requestStatus() const;
	[[nodiscard]] std::shared_ptr<ChatStickersCache> cache(
		not_null<PeerData*> peer,
		MsgId topicRootId);
	mtpRequestId loadMore(
		std::shared_ptr<ChatStickersCache> cache,
		Fn<void(QString)> done);
	mtpRequestId refresh(
		std::shared_ptr<ChatStickersCache> cache,
		Fn<void(QString)> done);
	[[nodiscard]] bool loading(
		const std::shared_ptr<ChatStickersCache> &cache) const;
	void cancel(mtpRequestId requestId);
	void flush(const std::shared_ptr<ChatStickersCache> &cache);
	void clear(const std::shared_ptr<ChatStickersCache> &cache);
	void resetAll();

private:
	struct Restore;
	void load(const std::shared_ptr<ChatStickersCache> &cache);
	void restore(
		const std::shared_ptr<ChatStickersCache> &cache,
		const std::shared_ptr<Restore> &state,
		uint64 generation);
	void remember(
		not_null<ChatStickersCache*> cache,
		not_null<DocumentData*> document);
	void markDirty(not_null<ChatStickersCache*> cache);
	void save(
		const std::shared_ptr<ChatStickersCache> &cache,
		bool synchronous = false);
	void store(
		const std::shared_ptr<ChatStickersCache> &cache,
		uint64 generation,
		QByteArray bytes);
	void trim();
	void reset(not_null<ChatStickersCache*> cache);
	mtpRequestId requestPage(
		std::shared_ptr<ChatStickersCache> cache,
		Fn<void(QString)> done,
		bool refresh);
	void applyItem(not_null<HistoryItem*> item);
	void seed(not_null<ChatStickersCache*> cache);
	void applyPage(
		not_null<ChatStickersCache*> cache,
		const MTPmessages_Messages &result,
		bool refresh);
	[[nodiscard]] QString waitText() const;
	[[nodiscard]] crl::time requestWait() const;
	void postponeRequests();
	void updateRequestTimer();

	const not_null<Main::Session*> _session;
	rpl::variable<bool> _enabled = false;
	rpl::event_stream<> _changes;
	std::list<std::shared_ptr<ChatStickersCache>> _caches;
	std::shared_ptr<ChatStickersCache> _active;
	int64 _retryAt = 0;
	crl::time _nextRequestAt = 0;
	mtpRequestId _requestId = 0;
	bool _stopping = false;
	base::Timer _saveTimer;
	base::Timer _requestTimer;
	MTP::Sender _api;
	rpl::lifetime _lifetime;

};

} // namespace Ayu
