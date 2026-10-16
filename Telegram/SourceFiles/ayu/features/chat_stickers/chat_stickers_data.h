#pragma once

#include "data/data_types.h"

#include <map>
#include <vector>

namespace Ayu {

inline constexpr auto kChatStickersMessageLimit = 50000;

struct ChatStickerEntry {
	DocumentId documentId = 0;
	MsgId messageId = 0;
	TimeId date = 0;
	int count = 0;
};

class ChatStickersData final {
public:
	bool add(MsgId messageId, DocumentId documentId, TimeId date);
	bool remove(MsgId messageId);
	void clear();

	[[nodiscard]] bool full() const;
	[[nodiscard]] std::vector<ChatStickerEntry> entries() const;

private:
	std::map<MsgId, DocumentId> _messages;
	std::map<DocumentId, std::map<MsgId, TimeId>> _stickers;

};

} // namespace Ayu
