#include "ayu/features/chat_stickers/chat_stickers_data.h"

namespace Ayu {

bool ChatStickersData::add(
		MsgId messageId,
		DocumentId documentId,
		TimeId date) {
	if (!IsServerMsgId(messageId)) {
		return false;
	} else if (!documentId) {
		return remove(messageId);
	}
	const auto existing = _messages.find(messageId);
	if (existing != end(_messages)) {
		if (existing->second == documentId) {
			auto &stored = _stickers.at(documentId).at(messageId);
			if (stored == date) {
				return false;
			}
			stored = date;
			return true;
		}
		remove(messageId);
	} else if (full()) {
		return false;
	}
	_messages.emplace(messageId, documentId);
	_stickers[documentId].emplace(messageId, date);
	return true;
}

bool ChatStickersData::remove(MsgId messageId) {
	const auto i = _messages.find(messageId);
	if (i == end(_messages)) {
		return false;
	}
	const auto sticker = _stickers.find(i->second);
	sticker->second.erase(messageId);
	if (sticker->second.empty()) {
		_stickers.erase(sticker);
	}
	_messages.erase(i);
	return true;
}

void ChatStickersData::clear() {
	_messages.clear();
	_stickers.clear();
}

bool ChatStickersData::full() const {
	return _messages.size() >= kChatStickersMessageLimit;
}

bool ChatStickersData::contains(MsgId messageId) const {
	return _messages.find(messageId) != end(_messages);
}

std::vector<ChatStickerEntry> ChatStickersData::entries() const {
	auto result = std::vector<ChatStickerEntry>();
	result.reserve(_stickers.size());
	for (const auto &[documentId, messages] : _stickers) {
		result.push_back({
			documentId,
			messages.rbegin()->first,
			messages.rbegin()->second,
			int(messages.size()),
		});
	}
	return result;
}

std::vector<ChatStickerMessage> ChatStickersData::messages() const {
	auto result = std::vector<ChatStickerMessage>();
	result.reserve(_messages.size());
	for (const auto &[documentId, messages] : _stickers) {
		for (const auto &[messageId, date] : messages) {
			result.push_back({ messageId, documentId, date });
		}
	}
	return result;
}

} // namespace Ayu
