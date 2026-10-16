#pragma once

#include "ayu/features/chat_stickers/chat_stickers_data.h"
#include "storage/cache/storage_cache_types.h"

#include <optional>

namespace Ayu {

inline constexpr auto kChatStickersCacheBytesLimit = 8 * 1024 * 1024;
inline constexpr auto kChatStickerDocumentBytesLimit = 128 * 1024;

struct SavedChatStickerDocument {
	DocumentId id = 0;
	uint64 setId = 0;
	uint64 setAccessHash = 0;
	QString setShortName;
	QByteArray bytes;
};

struct SavedChatStickers {
	uint64 accountId = 0;
	PeerId peerId = 0;
	MsgId topicRootId = 0;
	MsgId offsetId = 0;
	int scanned = 0;
	int appVersion = 0;
	bool finished = false;
	std::vector<SavedChatStickerDocument> documents;
	std::vector<ChatStickerMessage> messages;
};

[[nodiscard]] Storage::Cache::Key ChatStickersCacheKey(
	PeerId peerId,
	MsgId topicRootId);
[[nodiscard]] QByteArray SerializeChatStickersCache(
	const SavedChatStickers &cache);
[[nodiscard]] std::optional<SavedChatStickers> ParseChatStickersCache(
	const QByteArray &bytes,
	uint64 accountId,
	PeerId peerId,
	MsgId topicRootId,
	int appVersion);

} // namespace Ayu
