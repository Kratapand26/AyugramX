// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "history/history.h"
#include "ayu/features/forward/ayu_forward_result.h"
#include "main/main_session.h"

#include <atomic>
#include <map>

namespace AyuForward {
extern std::atomic<bool> g_isForwarderCanceled;
extern std::atomic<bool> g_isForwarderRunning;

bool isForwarding(const PeerId &id, const Main::Session &session);
void cancelForward(const PeerId &id, const Main::Session &session);
std::pair<QString, QString> stateName(
	const PeerId &id,
	const Main::Session &session);

class ForwardState
{
public:
	enum class State
	{
		Preparing,
		Downloading,
		Sending,
		Finished
	};

	explicit ForwardState(int totalChunks)
	: totalChunks(totalChunks) {
	}

	ForwardState(const ForwardState &other)
	: totalChunks(other.totalChunks.load())
	, currentChunk(other.currentChunk.load())
	, totalMessages(other.totalMessages.load())
	, sentMessages(other.sentMessages.load())
	, state(other.state.load())
	, stopRequested(other.stopRequested.load()) {
	}

	void updateBottomBar(const Main::Session &session, const PeerId *peer, const State &st);

	std::atomic<int> totalChunks = 0;
	std::atomic<int> currentChunk = 0;
	std::atomic<int> totalMessages = 0;
	std::atomic<int> sentMessages = 0;

	std::atomic<State> state = State::Preparing;
	std::atomic<bool> stopRequested = false;

};

bool isAyuForwardNeeded(const std::vector<not_null<HistoryItem*>> &items);
bool isAyuForwardNeeded(not_null<HistoryItem*> item);
bool isFullAyuForwardNeeded(not_null<HistoryItem*> item);
AyuSync::SendResult intelligentForward(
	not_null<Main::Session*> session,
	const Api::SendAction &action,
	const Data::ResolvedForwardDraft &draft);
AyuSync::SendResult forwardMessages(
	not_null<Main::Session*> session,
	const Api::SendAction &action,
	bool forwardState,
	const Data::ResolvedForwardDraft &draft,
	const std::map<FullMsgId, TextWithTags> &customTexts = {},
	Fn<bool()> cancelled = nullptr,
	int concurrency = 1);

}
