#pragma once

#include "ayu/features/chat_stickers/chat_stickers_data.h"
#include "mtproto/sender.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"

#include <list>

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
	bool finished = false;
};

class ChatStickers final {
public:
	explicit ChatStickers(not_null<Main::Session*> session);

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	void setEnabled(bool enabled);
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] std::shared_ptr<ChatStickersCache> cache(
		not_null<PeerData*> peer,
		MsgId topicRootId);
	mtpRequestId loadMore(
		std::shared_ptr<ChatStickersCache> cache,
		Fn<void(QString)> done);
	[[nodiscard]] bool loading(
		const std::shared_ptr<ChatStickersCache> &cache) const;
	void cancel(mtpRequestId requestId);
	void restart(const std::shared_ptr<ChatStickersCache> &cache);

private:
	void applyItem(not_null<HistoryItem*> item);
	void seed(not_null<ChatStickersCache*> cache);
	void applyPage(
		not_null<ChatStickersCache*> cache,
		const MTPmessages_Messages &result);
	[[nodiscard]] QString waitText() const;

	const not_null<Main::Session*> _session;
	rpl::variable<bool> _enabled = false;
	rpl::event_stream<> _changes;
	std::list<std::shared_ptr<ChatStickersCache>> _caches;
	std::shared_ptr<ChatStickersCache> _active;
	int64 _retryAt = 0;
	crl::time _nextRequestAt = 0;
	mtpRequestId _requestId = 0;
	MTP::Sender _api;
	rpl::lifetime _lifetime;

};

} // namespace Ayu
