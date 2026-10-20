// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/forward/ayu_forward.h"

#include "apiwrap.h"
#include "lang_auto.h"
#include "ayu/features/forward/ayu_forward_rich.h"
#include "ayu/features/forward/ayu_sync.h"
#include "ayu/utils/telegram_helpers.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "storage/localimageloader.h"
#include "storage/storage_account.h"
#include "storage/storage_media_prepare.h"
#include "styles/style_boxes.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/text/text_utilities.h"

#include <mutex>

namespace AyuForward {

namespace {

using ForwardKey = std::pair<uint64, PeerId>;
auto ForwardStates = std::map<ForwardKey, std::shared_ptr<ForwardState>>();
auto ForwardStatesMutex = std::mutex();

[[nodiscard]] std::shared_ptr<ForwardState> StateFor(
		const Main::Session *session,
		PeerId id) {
	const auto lock = std::lock_guard(ForwardStatesMutex);
	const auto found = ForwardStates.find({ session->uniqueId(), id });
	return (found != ForwardStates.end()) ? found->second : nullptr;
}

void SetStateFor(
		const Main::Session *session,
		PeerId id,
		std::shared_ptr<ForwardState> state) {
	const auto lock = std::lock_guard(ForwardStatesMutex);
	ForwardStates[{ session->uniqueId(), id }] = std::move(state);
}

} // namespace

bool isForwarding(const PeerId &id, const Main::Session &session) {
	const auto state = StateFor(&session, id);
	return state
		&& state->state != ForwardState::State::Finished
		&& state->currentChunk < state->totalChunks
		&& !state->stopRequested
		&& ((state->totalChunks && state->totalMessages)
			|| state->state == ForwardState::State::Downloading);
}

void cancelForward(const PeerId &id, const Main::Session &session) {
	if (const auto state = StateFor(&session, id)) {
		state->stopRequested = true;
		state->updateBottomBar(session, &id, ForwardState::State::Finished);
	}
}

std::pair<QString, QString> stateName(
		const PeerId &id,
		const Main::Session &session) {
	const auto state = StateFor(&session, id);
	if (!state) {
		return {};
	}

	QString messagesString = tr::ayu_AyuForwardStatusSentCount(tr::now,
															   lt_count1,
															   QString::number(state->sentMessages.load()),
															   lt_count2,
															   QString::number(state->totalMessages.load())

	);

	QString chunkString = tr::ayu_AyuForwardStatusChunkCount(tr::now,
															 lt_count1,
															 QString::number(state->currentChunk + 1),
															 lt_count2,
															 QString::number(state->totalChunks.load())

	);

	const auto partString = state->totalChunks <= 1 ? messagesString : (messagesString + " • " + chunkString);

	QString status;

	if (state->state == ForwardState::State::Preparing) {
		status = tr::ayu_AyuForwardStatusPreparing(tr::now);
	} else if (state->state == ForwardState::State::Downloading) {
		return std::make_pair(tr::ayu_AyuForwardStatusLoadingMedia(tr::now), "");
	} else if (state->state == ForwardState::State::Sending) {
		status = tr::ayu_AyuForwardStatusForwarding(tr::now);
	} else {
		// ForwardState::State::Finished
		status = tr::ayu_AyuForwardStatusFinished(tr::now);
	}

	return std::make_pair(status, partString);
}

void ForwardState::updateBottomBar(const Main::Session &session, const PeerId *peer, const State &st) {
	state = st;
	const auto peerCopy = *peer;
	const auto weak = base::make_weak(&session);
	AyuSync::RunOnMain(crl::guard(weak, [weak, peerCopy] {
		if (const auto peer = weak->data().peerLoaded(peerCopy)) {
			weak->changes().peerUpdated(peer, Data::PeerUpdate::Flag::Rights);
		}
	}));
}

static Ui::PreparedList prepareMedia(
		not_null<Main::Session*> session,
		const std::vector<not_null<HistoryItem*>> &items,
		int &index,
		std::vector<not_null<Data::Media*>> &groupMedia,
		const AyuSync::DocumentPaths &documentPaths,
		const std::map<FullMsgId, TextWithTags> &customTexts,
		bool dropCaptions) {
	auto list = Ui::PreparedList();
	const auto append = [&](not_null<HistoryItem*> item) {
		const auto media = item->media();
		if (!media) {
			return;
		}
		auto path = QString();
		const auto document = media->document();
		if (document) {
			const auto found = documentPaths.find(document);
			if (found != documentPaths.end()) {
				path = found->second;
			}
		} else if (const auto photo = media->photo()) {
			path = AyuSync::filePath(session, photo);
		}
		auto prepared = Ui::PreparedFile(path);
		if (prepared.path.isEmpty()) {
			return;
		}
		if (document) {
			prepared.displayName = AyuSync::documentFileName(document);
		}
		if (!dropCaptions) {
			const auto text = customTexts.find(item->fullId());
			prepared.caption = (text != customTexts.end())
				? text->second
				: extractText(item);
		}
		Storage::PrepareDetails(prepared, st::sendMediaPreviewSize, PhotoSideLimit());
		groupMedia.emplace_back(media);
		list.files.emplace_back(std::move(prepared));
	};
	const auto groupId = items[index]->groupId();
	append(items[index]);
	if (groupId) {
		while (index + 1 < items.size()
			&& items[index + 1]->groupId() == groupId) {
			append(items[++index]);
		}
	}
	return list;
}

AyuSync::SendResult sendMedia(
		not_null<Main::Session*> session,
		const std::shared_ptr<Ui::PreparedBundle> &bundle,
		not_null<Data::Media*> primaryMedia,
		Api::MessageToSend &&message,
		bool sendImagesAsPhotos,
		const Fn<bool()> &cancelled) {
	const auto document = primaryMedia->document();
	if (document && document->sticker()) {
		return AyuSync::sendStickerSync(session, std::move(message), document, cancelled);
	}
	auto mediaType = !document
		? SendMediaType::Photo
		: document->isVoiceMessage()
		? SendMediaType::Audio
		: document->isVideoMessage()
		? SendMediaType::Round
		: (document->isVideoFile() || document->isGifv())
		? SendMediaType::Photo
		: SendMediaType::File;
	if (mediaType == SendMediaType::Round || mediaType == SendMediaType::Audio) {
		const auto path = bundle->groups.front().list.files.front().path;
		auto file = QFile(path);
		if (file.open(QIODevice::ReadOnly)) {
			const auto data = file.readAll();
			if (!data.isEmpty()) {
				return AyuSync::sendVoiceSync(
					session,
					data,
					document->duration(),
					mediaType == SendMediaType::Round,
					std::move(message),
					cancelled);
			}
		}
		return {};
	}
	if (sendImagesAsPhotos) {
		mediaType = SendMediaType::Photo;
	}
	auto combined = AyuSync::SendResult();
	combined.outcome = AyuSync::SendOutcome::Sent;
	for (auto &group : bundle->groups) {
		if (cancelled && cancelled()) {
			combined.outcome = AyuSync::SendOutcome::Cancelled;
			return combined;
		}
		auto caption = group.list.files.empty()
			? TextWithTags()
			: group.list.files.front().caption;
		auto result = AyuSync::sendDocumentSync(
			session, group, mediaType, std::move(caption), message.action, cancelled);
		result.confirmed += combined.confirmed;
		if (!result.sent()) {
			return result;
		}
		combined = std::move(result);
	}
	return combined;
}

bool isAyuForwardNeeded(const std::vector<not_null<HistoryItem*>> &items) {
	const auto needAyuForward = [&](const auto &item)
	{
		return isAyuForwardNeeded(item);
	};
	return std::ranges::any_of(items, needAyuForward);
}

bool isAyuForwardNeeded(not_null<HistoryItem*> item) {
	if (item->isDeleted() || item->isAyuNoForwards() || (item->media() && item->media()->ttlSeconds())) {
		return true;
	}
	return false;
}

bool isFullAyuForwardNeeded(not_null<HistoryItem*> item) {
	return item->from()->isAyuNoForwards() || item->history()->peer->isAyuNoForwards();
}

struct ForwardChunk
{
	bool isAyuForwardNeeded;
	std::vector<not_null<HistoryItem*>> items;
};

AyuSync::SendResult intelligentForward(
	not_null<Main::Session*> session,
	const Api::SendAction &action,
	const Data::ResolvedForwardDraft &draft) {
	const auto history = action.history;
	const auto topicRootId = action.replyTo.topicRootId;
	const auto monoforumPeerId = action.replyTo.monoforumPeerId;
	crl::on_main([=]
	{
		history->setForwardDraft(topicRootId, monoforumPeerId, {});
	});

	const auto items = draft.items;
	if (items.empty()) {
		return {};
	}
	const auto peer = history->peer;

	auto chunks = std::vector<ForwardChunk>();
	auto currentArray = std::vector<not_null<HistoryItem*>>();

	auto currentChunk = ForwardChunk({
		.isAyuForwardNeeded = isAyuForwardNeeded(items[0]),
		.items = currentArray
	});

	for (const auto &item : items) {
		if (isAyuForwardNeeded(item) != currentChunk.isAyuForwardNeeded) {
			currentChunk.items = currentArray;
			chunks.push_back(currentChunk);

			currentArray = std::vector<not_null<HistoryItem*>>();

			currentChunk = ForwardChunk({
				.isAyuForwardNeeded = isAyuForwardNeeded(item),
				.items = currentArray
			});
		}
		currentArray.push_back(item);
	}

	currentChunk.items = currentArray;
	chunks.push_back(currentChunk);

	auto state = std::make_shared<ForwardState>(chunks.size());
	SetStateFor(session, peer->id, state);

	auto combined = AyuSync::SendResult();
	combined.outcome = AyuSync::SendOutcome::Sent;
	for (const auto &chunk : chunks) {
		auto result = AyuSync::SendResult();
		if (chunk.isAyuForwardNeeded) {
			result = forwardMessages(
				session, action, true, Data::ResolvedForwardDraft(chunk.items, draft.options));
		} else {
			state->totalMessages = chunk.items.size();
			state->sentMessages = 0;
			state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);

			result = AyuSync::forwardMessagesSync(
				session, chunk.items, action, draft.options,
				[state] { return state->stopRequested.load(); });

			state->sentMessages = result.confirmed;

			state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
		}
		result.confirmed += combined.confirmed;
		if (!result.sent()) {
			state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
			return result;
		}
		combined = std::move(result);
		state->currentChunk++;
	}

	state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
	return combined;
}

AyuSync::SendResult forwardMessages(
		not_null<Main::Session*> session,
		const Api::SendAction &action,
		bool forwardState,
		const Data::ResolvedForwardDraft &draft,
		const std::map<FullMsgId, TextWithTags> &customTexts,
		Fn<bool()> cancelled,
		int concurrency) {
	const auto items = draft.items;
	const auto history = action.history;
	const auto peer = history->peer;
	const auto topicRootId = action.replyTo.topicRootId;
	const auto monoforumPeerId = action.replyTo.monoforumPeerId;
	const auto weak = base::make_weak(session);
	AyuSync::RunOnMain(crl::guard(weak, [=] {
		history->setForwardDraft(topicRootId, monoforumPeerId, {});
	}));
	if (items.empty()) {
		return {};
	}
	auto state = std::make_shared<ForwardState>(1);
	if (forwardState) {
		if (const auto previous = StateFor(session, peer->id)) {
			state = previous;
		}
	}
	SetStateFor(session, peer->id, state);
	const auto stopped = [=] {
		return state->stopRequested.load() || (cancelled && cancelled());
	};
	const auto finished = gsl::finally([&] {
		state->updateBottomBar(*session, &peer->id, ForwardState::State::Finished);
	});
	state->totalMessages = int(items.size());
	state->updateBottomBar(*session, &peer->id, ForwardState::State::Downloading);
	auto downloadable = std::vector<not_null<HistoryItem*>>();
	for (const auto item : items) {
		if (mediaDownloadable(item->media())) {
			downloadable.push_back(item);
		}
	}
	const auto paths = AyuSync::loadDocuments(session, downloadable, stopped, concurrency);
	if (stopped()) {
		return { AyuSync::SendOutcome::Cancelled };
	}
	state->sentMessages = 0;
	state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);
	auto combined = AyuSync::SendResult();
	combined.outcome = AyuSync::SendOutcome::Sent;
	for (auto index = 0; index < items.size(); ++index) {
		if (stopped()) {
			combined.outcome = AyuSync::SendOutcome::Cancelled;
			return combined;
		}
		const auto item = items[index];
		auto result = AyuSync::SendResult();
		if (item->richPage()) {
			const auto edited = customTexts.find(item->fullId());
			if (edited != customTexts.end() && edited->second.text != extractText(item).text) {
				combined.outcome = AyuSync::SendOutcome::Failed;
				combined.error = u"RICH_MESSAGE_CAPTION_EDIT_UNSUPPORTED"_q;
				return combined;
			}
			if (!forwardRichMessage(session, item->fullId(), action, stopped)) {
				combined.outcome = stopped()
					? AyuSync::SendOutcome::Cancelled
					: AyuSync::SendOutcome::Unconfirmed;
				return combined;
			}
			result.outcome = AyuSync::SendOutcome::Sent;
			result.confirmed = 1;
		} else {
			auto message = Api::MessageToSend(action);
			message.action.options.invertCaption = item->invertMedia();
			message.action.options.scheduled = 0;
			message.action.options.suggest = {};
			message.action.options.effectId = 0;
			message.action.replaceMediaOf = 0;
			const auto dropCaptions = draft.options == Data::ForwardOptions::NoNamesAndCaptions;
			if (!dropCaptions || !mediaDownloadable(item->media())) {
				const auto text = customTexts.find(item->fullId());
				message.textWithTags = (text != customTexts.end())
					? text->second
					: extractText(item);
			}
			const auto media = item->media();
			if (media && !mediaDownloadable(media) && !media->webpage()) {
				if (message.textWithTags.text != extractText(item).text) {
					combined.outcome = AyuSync::SendOutcome::Failed;
					combined.error = u"MEDIA_CAPTION_EDIT_UNSUPPORTED"_q;
					return combined;
				}
				if (isAyuForwardNeeded(item) || isFullAyuForwardNeeded(item)) {
					combined.outcome = AyuSync::SendOutcome::Failed;
					combined.error = u"UNSUPPORTED_RESTRICTED_MEDIA"_q;
					return combined;
				}
				result = AyuSync::forwardMessagesSync(
					session, { item }, action, draft.options, stopped);
			} else if (!mediaDownloadable(media)) {
				if (message.textWithTags.text.trimmed().isEmpty()) {
					result.outcome = AyuSync::SendOutcome::Sent;
				} else {
					result = AyuSync::sendMessageSync(session, std::move(message), stopped);
				}
			} else {
				auto grouped = std::vector<not_null<Data::Media*>>();
				const auto start = index;
				auto prepared = prepareMedia(
					session, items, index, grouped, paths, customTexts, dropCaptions);
				auto complete = int(prepared.files.size()) == index - start + 1;
				for (auto j = 0; complete && j < prepared.files.size(); ++j) {
					const auto info = QFileInfo(prepared.files[j].path);
					const auto document = grouped[j]->document();
					const auto photo = grouped[j]->photo();
					complete = info.isFile()
						&& (document ? info.size() == document->size
							: photo && info.size() >= photo->imageByteSize(Data::PhotoSize::Large));
				}
				if (!complete) {
					combined.outcome = AyuSync::SendOutcome::Failed;
					combined.retryable = !combined.confirmed;
					combined.error = u"MEDIA_DOWNLOAD_INCOMPLETE"_q;
					return combined;
				}
				auto way = Ui::SendFilesWay();
				way.setGroupFiles(true);
				way.setSendImagesAsPhotos(ranges::any_of(grouped, [](auto media) {
					return media->photo() != nullptr;
				}));
				auto groups = Ui::DivideByGroups(std::move(prepared), way, peer->slowmodeApplied());
				const auto bundle = Ui::PrepareFilesBundle(std::move(groups), way, false);
				result = sendMedia(
					session, bundle, grouped.front(), std::move(message), way.sendImagesAsPhotos(), stopped);
			}
		}
		result.confirmed += combined.confirmed;
		if (!result.sent()) {
			return result;
		}
		combined = std::move(result);
		state->sentMessages = index + 1;
		state->updateBottomBar(*session, &peer->id, ForwardState::State::Sending);
	}
	return combined;
}

} // namespace AyuForward
