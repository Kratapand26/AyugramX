// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "data/data_msg_id.h"

#include <QString>

namespace AyuSync {

enum class SendOutcome {
	Sent,
	Failed,
	Cancelled,
	Unconfirmed,
};

struct SendResult {
	SendOutcome outcome = SendOutcome::Failed;
	FullMsgId messageId;
	int confirmed = 0;
	bool retryable = false;
	QString error;

	[[nodiscard]] bool sent() const {
		return outcome == SendOutcome::Sent;
	}
};

} // namespace AyuSync
