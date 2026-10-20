// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/forward/ayu_sync.h"

#include "api/api_common.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "ayu/utils/telegram_helpers.h"
#include "ayu/features/forward/forwarder_progress.h"
#include "base/base_file_utilities.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "iv/iv_rich_page.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"

#include <QCoreApplication>
#include <QThread>
#include <algorithm>
#include <future>
#include <set>
#include <mutex>
#include <utility>

namespace AyuSync {
namespace {

constexpr auto kDocumentDownloadTimeout = std::chrono::minutes(15);
constexpr auto kDownloadWaitStep = std::chrono::milliseconds(100);

enum class DownloadWaitResult {
	Completed,
	Cancelled,
	TimedOut,
};

struct ActiveDocumentDownload {
	QString path;
	QString readyPath;
	TimedCountDownLatch done{ 1 };
	rpl::lifetime lifetime;
	int consumers = 1;
	bool allowSizeFallback = false;
};

struct DocumentDownloadRegistration {
	std::shared_ptr<ActiveDocumentDownload> state;
	bool owner = false;
};

using ActiveDocumentDownloads = base::flat_map<
	DocumentData*,
	std::shared_ptr<ActiveDocumentDownload>>;

[[nodiscard]] std::mutex &DocumentDownloadsMutex() {
	static auto result = std::mutex();
	return result;
}

[[nodiscard]] ActiveDocumentDownloads &DocumentDownloads() {
	static auto result = ActiveDocumentDownloads();
	return result;
}

[[nodiscard]] DocumentDownloadRegistration RegisterDocumentDownload(
		not_null<DocumentData*> document) {
	const auto lock = std::lock_guard(DocumentDownloadsMutex());
	auto &active = DocumentDownloads();
	const auto i = active.find(document);
	if (i != active.end()) {
		++i->second->consumers;
		return { i->second, false };
	}
	auto state = std::make_shared<ActiveDocumentDownload>();
	active.emplace(document, state);
	return { std::move(state), true };
}

void FinishDocumentDownload(
		DocumentData *document,
		const std::shared_ptr<ActiveDocumentDownload> &state) {
	{
		const auto lock = std::lock_guard(DocumentDownloadsMutex());
		auto &active = DocumentDownloads();
		const auto i = active.find(document);
		if (i != active.end() && i->second == state) {
			active.erase(i);
		}
	}
	state->done.countDown();
}

void ReleaseDocumentDownloadConsumer(
		DocumentData *document,
		const std::shared_ptr<ActiveDocumentDownload> &state) {
	auto released = false;
	{
		const auto lock = std::lock_guard(DocumentDownloadsMutex());
		Expects(state->consumers > 0);
		--state->consumers;
		if (state->consumers == 0) {
			auto &active = DocumentDownloads();
			const auto i = active.find(document);
			if (i != active.end() && i->second == state) {
				active.erase(i);
				released = true;
			}
		}
	}
	if (released) {
		state->done.countDown();
	}
}

[[nodiscard]] DownloadWaitResult WaitForDownload(
		TimedCountDownLatch &done,
		std::chrono::milliseconds timeout,
		const Fn<bool()> &cancelled) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (true) {
		if (done.await(std::chrono::milliseconds(0))) {
			return DownloadWaitResult::Completed;
		}
		if (cancelled && cancelled()) {
			return DownloadWaitResult::Cancelled;
		}
		const auto remaining = std::chrono::duration_cast<
			std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now());
		if (remaining <= std::chrono::milliseconds(0)) {
			return DownloadWaitResult::TimedOut;
		}
		if (done.await(std::min(
				remaining,
				kDownloadWaitStep))) {
			return DownloadWaitResult::Completed;
		}
	}
}

[[nodiscard]] bool SameFilePath(const QString &left, const QString &right) {
	const auto leftInfo = QFileInfo(left);
	const auto rightInfo = QFileInfo(right);
	const auto leftCanonical = leftInfo.canonicalFilePath();
	const auto rightCanonical = rightInfo.canonicalFilePath();
	if (!leftCanonical.isEmpty() && !rightCanonical.isEmpty()) {
		return leftCanonical == rightCanonical;
	}
#ifdef Q_OS_WIN
	return leftInfo.absoluteFilePath().compare(
		rightInfo.absoluteFilePath(),
		Qt::CaseInsensitive) == 0;
#else // Q_OS_WIN
	return leftInfo.absoluteFilePath() == rightInfo.absoluteFilePath();
#endif // !Q_OS_WIN
}

[[nodiscard]] bool FileHasExactSize(const QString &path, int64 expectedSize) {
	const auto info = QFileInfo(path);
	return !path.isEmpty()
		&& info.isFile()
		&& info.size() == expectedSize;
}

[[nodiscard]] QString LoadedDocumentPath(not_null<DocumentData*> document) {
	auto result = QString();
	AyuSync::RunOnMainSync([&] {
		result = document->filepath(true);
	});
	return result;
}

[[nodiscard]] bool DocumentReady(
		not_null<DocumentData*> document,
		const QString &path) {
	const auto loaded = document->filepath(true);
	return !path.isEmpty()
		&& !loaded.isEmpty()
		&& SameFilePath(path, loaded)
		&& FileHasExactSize(path, document->size);
}

void CompleteDocumentDownload(
		not_null<DocumentData*> document,
		const std::shared_ptr<ActiveDocumentDownload> &state) {
	if (document->loading()) {
		return;
	}
	const auto released = state->done.await(std::chrono::milliseconds(0));
	const auto ready = state->allowSizeFallback
		? FileHasExactSize(state->path, document->size)
		: (document->status != FileDownloadFailed
			&& !document->cancelled()
			&& DocumentReady(document, state->path));
	if (!ready) {
		QFile::remove(state->path);
		state->path.clear();
	} else if (!released) {
		state->readyPath = state->path;
	}
	state->lifetime.destroy();
	if (!released) {
		FinishDocumentDownload(document, state);
	}
}

[[nodiscard]] QString GeneratedDocumentName(
		not_null<DocumentData*> document,
		const QString &prefix,
		const QString &extension) {
	return prefix
		+ QString::number(document->getDC())
		+ u"_"_q
		+ QString::number(document->id)
		+ extension;
}

[[nodiscard]] QString DocumentFileName(not_null<DocumentData*> document) {
	if (!document->filename().isEmpty()) {
		return base::FileNameFromUserString(document->filename());
	}
	if (document->isVoiceMessage()) {
		return GeneratedDocumentName(document, u"audio_"_q, u".ogg"_q);
	}
	if (document->isVideoMessage()) {
		return GeneratedDocumentName(document, u"round_"_q, u".mp4"_q);
	}
	if (document->isGifv()) {
		return GeneratedDocumentName(document, u"gif_"_q, u".gif"_q);
	}
	if (document->isVideoFile()) {
		return GeneratedDocumentName(document, u"video_"_q, u".mp4"_q);
	}
	return {};
}

[[nodiscard]] QString NextDocumentPath(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document) {
	const auto directory = pathForSave(session);
	const auto filename = DocumentFileName(document);
	if (directory.isEmpty() || filename.isEmpty()) {
		return {};
	}
	return filedialogNextFilename(filename, QString(), directory);
}

struct SendReceipt {
	TimedCountDownLatch done{ 1 };
	rpl::lifetime lifetime;
	AyuForwarder::SendProgress<FullMsgId> progress;
	SendResult result;
	bool finished = false;
};

void FinishSend(
		const std::shared_ptr<SendReceipt> &receipt,
		SendOutcome outcome,
		QString error = {}) {
	if (receipt->finished) {
		return;
	}
	receipt->finished = true;
	receipt->result.outcome = outcome;
	receipt->result.error = std::move(error);
	receipt->done.countDown();
}

void CheckSend(const std::shared_ptr<SendReceipt> &receipt) {
	if (receipt->progress.complete()) {
		FinishSend(receipt, SendOutcome::Sent);
	}
}

void MessageCreated(
		const std::shared_ptr<SendReceipt> &receipt,
		FullMsgId id) {
	if (!receipt->finished) {
		receipt->progress.created(id);
	}
}

void ObserveSend(
		not_null<Main::Session*> session,
		const std::shared_ptr<SendReceipt> &receipt) {
	session->data().itemIdChanged() | rpl::on_next([=](
			const Data::Session::IdChange &update) {
		if (!receipt->finished
			&& receipt->progress.confirm(FullMsgId(
				update.newId.peer,
				update.oldId))) {
			receipt->result.confirmed = receipt->progress.confirmed();
			receipt->result.messageId = update.newId;
			CheckSend(receipt);
		}
	}, receipt->lifetime);
	session->data().itemDataChanges() | rpl::on_next([=](
			not_null<HistoryItem*> item) {
		if (receipt->progress.contains(item->fullId()) && item->hasFailed()) {
			FinishSend(receipt, SendOutcome::Failed);
		}
	}, receipt->lifetime);
	session->data().itemRemoved() | rpl::on_next([=](
			not_null<const HistoryItem*> item) {
		const auto id = item->fullId();
		if (receipt->progress.contains(id)) {
			AyuSync::RunOnMain([=] {
				if (receipt->progress.contains(id)) {
					FinishSend(receipt, SendOutcome::Failed);
				}
			});
		}
	}, receipt->lifetime);
	session->data().sessionDataAboutToBeCleared() | rpl::on_next([=] {
		FinishSend(receipt, SendOutcome::Cancelled);
	}, receipt->lifetime);
}

SendResult AwaitSend(
		not_null<Main::Session*> session,
		const std::shared_ptr<SendReceipt> &receipt,
		const Fn<bool()> &cancelled) {
	const auto weak = base::make_weak(session);
	const auto waited = WaitForDownload(
		receipt->done,
		std::chrono::minutes(30),
		cancelled);
	auto result = SendResult();
	AyuSync::RunOnMainSync([&] {
		if (!receipt->finished) {
			FinishSend(
				receipt,
				waited == DownloadWaitResult::Cancelled
					? SendOutcome::Cancelled
					: SendOutcome::Unconfirmed,
				waited == DownloadWaitResult::Cancelled
					? u"SEND_CANCELLED"_q : u"SEND_UNCONFIRMED"_q);
		}
		receipt->lifetime.destroy();
		result = receipt->result;
		if (!result.sent() && weak) {
			for (const auto id : receipt->progress.pending()) {
				weak->uploader().cancel(id);
			}
		}
	});
	return result;
}

} // namespace

void RunOnMain(FnMut<void()> callback) {
	QMetaObject::invokeMethod(
		QCoreApplication::instance(), std::move(callback), Qt::QueuedConnection);
}

void RunOnMainSync(FnMut<void()> callback) {
	const auto application = QCoreApplication::instance();
	if (QThread::currentThread() == application->thread()) {
		callback();
	} else {
		QMetaObject::invokeMethod(application, std::move(callback), Qt::BlockingQueuedConnection);
	}
}

QString pathForSave(not_null<Main::Session*> session) {
	auto path = Core::App().settings().downloadPath();
	if (path.isEmpty()) {
		return File::DefaultDownloadPath(session);
	}
	if (path == FileDialog::Tmp()) {
		return session->local().tempDirectory();
	}
	return path;
}

QString documentFileName(not_null<DocumentData*> document) {
	return DocumentFileName(document);
}

QString filePath(not_null<Main::Session*> session, not_null<PhotoData*> photo) {
	const auto directory = pathForSave(session);
	if (directory.isEmpty()) {
		return {};
	}
	const auto filename = QString::number(photo->getDC())
		+ u"_"_q
		+ QString::number(photo->id)
		+ u".jpg"_q;
	return QDir(directory).filePath(filename);
}

QString filePath(not_null<Main::Session*> session, not_null<DocumentData*> document) {
	return NextDocumentPath(session, document);
}

qint64 fileSize(const QString &path) {
	if (path.isEmpty()) {
		return 0;
	}
	QFile file(path);
	return file.exists() ? file.size() : 0;
}

DocumentPaths loadDocuments(
		not_null<Main::Session*> session,
		const std::vector<not_null<HistoryItem*>> &items,
		const Fn<bool()> &cancelled,
		int concurrency) {
	const auto load = [=](not_null<HistoryItem*> item) {
		if ((cancelled && cancelled()) || !item->media()) {
			return std::pair<DocumentData*, QString>();
		}
		if (const auto document = item->media()->document()) {
			return std::make_pair(document, loadDocumentSync(
				session,
				document,
				item->fullId(),
				cancelled));
		} else if (const auto photo = item->media()->photo()) {
			if (fileSize(filePath(session, photo))
				!= photo->imageByteSize(Data::PhotoSize::Large)) {
				loadPhotoSync(session, photo, item->fullId(), cancelled);
			}
		}
		return std::pair<DocumentData*, QString>();
	};
	auto result = DocumentPaths();
	const auto add = [&](const std::pair<DocumentData*, QString> &loaded) {
		if (loaded.first && !loaded.second.isEmpty()) {
			result.emplace(loaded.first, loaded.second);
		}
	};
	concurrency = std::clamp(concurrency, 1, 8);
	for (auto i = size_t(0); i < items.size();) {
		if (cancelled && cancelled()) {
			break;
		}
		if (concurrency == 1) {
			add(load(items[i++]));
			continue;
		}
		auto pending = std::vector<
			std::future<std::pair<DocumentData*, QString>>>();
		for (auto j = 0; j < concurrency && i < items.size(); ++j, ++i) {
			pending.push_back(std::async(std::launch::async, load, items[i]));
		}
		for (auto &future : pending) {
			add(future.get());
		}
	}
	return result;
}

QString loadDocumentSync(
		not_null<Main::Session*> session,
		not_null<DocumentData*> data,
		Data::FileOrigin origin,
		const Fn<bool()> &cancelled) {
	// const auto application = QCoreApplication::instance();
	// Expects(QThread::currentThread() != application->thread());

	if (cancelled && cancelled()) {
		return {};
	}
	const auto expectedSize = data->size;
	const auto registration = RegisterDocumentDownload(data);
	const auto releaseConsumer = gsl::finally([&] {
		ReleaseDocumentDownloadConsumer(data, registration.state);
	});
	if (!registration.owner) {
		const auto waitResult = WaitForDownload(
				registration.state->done,
				kDocumentDownloadTimeout,
				cancelled);
		if (waitResult != DownloadWaitResult::Completed) {
			return {};
		}
		const auto path = registration.state->readyPath;
		return FileHasExactSize(path, expectedSize) ? path : QString();
	}
	auto registrationActive = true;
	auto reserved = false;
	const auto cleanup = gsl::finally([&] {
		if (!registrationActive) {
			return;
		}
		if (reserved) {
			QFile::remove(registration.state->path);
		}
		registration.state->path.clear();
		FinishDocumentDownload(data, registration.state);
	});
	auto path = LoadedDocumentPath(data);
	if (FileHasExactSize(path, expectedSize)) {
		registration.state->path = path;
		registration.state->readyPath = path;
		registrationActive = false;
		FinishDocumentDownload(data, registration.state);
		return path;
	}
	while (true) {
		path = NextDocumentPath(session, data);
		if (path.isEmpty()) {
			return {};
		}
		if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
			return {};
		}
		auto reservation = QFile(path);
		if (reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
			reservation.close();
			reserved = true;
			break;
		}
		if (!QFile::exists(path)) {
			return {};
		}
	}
	registration.state->path = path;
	if (cancelled && cancelled()) {
		return {};
	}

	const auto state = registration.state;
	const auto documentId = data->id;
	AyuSync::RunOnMainSync([=] {
		data->save(origin, path);
		state->allowSizeFallback = !data->loading();
		if (!data->loading()) {
			CompleteDocumentDownload(data, state);
			return;
		}

		session->data().documentLoadProgress(
		) | rpl::filter([=](not_null<DocumentData*> changed) {
			return changed->id == documentId
				&& !changed->loading();
		}) | rpl::on_next([=](not_null<DocumentData*> changed) {
			CompleteDocumentDownload(changed, state);
		}, state->lifetime);
	});
	registrationActive = false;
	const auto waitResult = WaitForDownload(
			registration.state->done,
			kDocumentDownloadTimeout,
			cancelled);
	if (waitResult != DownloadWaitResult::Completed) {
		return {};
	}
	path = registration.state->readyPath;
	return FileHasExactSize(path, expectedSize) ? path : QString();
}

SendResult forwardMessagesSync(
		not_null<Main::Session*> session,
		const std::vector<not_null<HistoryItem*>> &items,
		const ApiWrap::SendAction &action,
		Data::ForwardOptions options,
		Fn<bool()> cancelled) {
	const auto receipt = std::make_shared<SendReceipt>();
	const auto weak = base::make_weak(session);
	AyuSync::RunOnMain([=] {
		if (!weak || (cancelled && cancelled())) {
			FinishSend(receipt, SendOutcome::Cancelled);
			return;
		}
		session->data().sessionDataAboutToBeCleared() | rpl::on_next([=] {
			FinishSend(receipt, SendOutcome::Cancelled);
		}, receipt->lifetime);
		session->api().forwardMessages(
			Data::ResolvedForwardDraft(items, options),
			action,
			[=] {
				if (!receipt->finished) {
					receipt->result.confirmed = int(items.size());
					FinishSend(receipt, SendOutcome::Sent);
				}
			},
			[=](const MTP::Error &error) {
				FinishSend(
					receipt,
					SendOutcome::Failed,
					MTP::IgnoreError(error) ? QString() : error.type());
			});
	});
	return AwaitSend(session, receipt, cancelled);
}

void loadPhotoSync(
		not_null<Main::Session*> session,
		not_null<PhotoData*> photo,
		Data::FileOrigin origin,
		const Fn<bool()> &cancelled) {
	const auto path = pathForSave(session);
	if (path.isEmpty() || !QDir().mkpath(path)) {
		return;
	}

	auto latch = std::make_shared<TimedCountDownLatch>(1);
	auto lifetime = std::make_shared<rpl::lifetime>();

	AyuSync::RunOnMain([=]
	{
		const auto view = photo->createMediaView();
		if (!view) {
			latch->countDown();
			return;
		}
		view->wanted(Data::PhotoSize::Large, origin);

		const auto saveToFiles = [=]
		{
			view->saveToFile(filePath(session, photo));
		};

		if (view->loaded()) {
			saveToFiles();
			latch->countDown();
			return;
		}

		session->downloaderTaskFinished() | rpl::filter([=]
		{
			return view->loaded();
		}) | rpl::on_next([=]
								  {
									  saveToFiles();
									  latch->countDown();
								  },
								  *lifetime);
	});

	if (WaitForDownload(*latch, std::chrono::minutes(5), cancelled)
		== DownloadWaitResult::TimedOut) {
		LOG(("AyuForward: photo download timed out."));
	}
	AyuSync::RunOnMainSync([lifetime = base::take(lifetime)]
	{
		lifetime->destroy();
	});
}

SendResult sendMessageSync(
		not_null<Main::Session*> session,
		Api::MessageToSend &&message,
		Fn<bool()> cancelled) {
	const auto receipt = std::make_shared<SendReceipt>();
	const auto weak = base::make_weak(session);
	AyuSync::RunOnMain([=, message = std::move(message)]() mutable {
		if (!weak || (cancelled && cancelled())) {
			FinishSend(receipt, SendOutcome::Cancelled);
			return;
		}
		ObserveSend(session, receipt);
		session->api().sendMessage(std::move(message), std::nullopt, [=](FullMsgId id) {
			MessageCreated(receipt, id);
		});
		receipt->progress.finishCreating();
		if (!receipt->progress.expected()) {
			FinishSend(receipt, SendOutcome::Failed);
		} else {
			CheckSend(receipt);
		}
	});
	return AwaitSend(session, receipt, cancelled);
}

SendResult sendDocumentSync(
		not_null<Main::Session*> session,
		Ui::PreparedGroup &group,
		SendMediaType type,
		TextWithTags &&caption,
		const Api::SendAction &action,
		Fn<bool()> cancelled) {
	const auto receipt = std::make_shared<SendReceipt>();
	const auto weak = base::make_weak(session);
	auto album = std::make_shared<SendingAlbum>();
	album->groupId = base::RandomValue<uint64>();
	AyuSync::RunOnMain([=, list = std::move(group.list), caption = std::move(caption)]() mutable {
		if (!weak || (cancelled && cancelled())) {
			FinishSend(receipt, SendOutcome::Cancelled);
			return;
		}
		receipt->progress.expect(int(list.files.size()));
		if (!receipt->progress.expected()) {
			FinishSend(receipt, SendOutcome::Failed);
			return;
		}
		ObserveSend(session, receipt);
		list.files.front().caption = std::move(caption);
		session->api().sendFiles(
			std::move(list),
			type,
			receipt->progress.expected() > 1 ? album : nullptr,
			action,
			[=](FullMsgId id) { MessageCreated(receipt, id); },
			[=] { return receipt->finished || (cancelled && cancelled()); },
			[=] { FinishSend(receipt, SendOutcome::Failed, u"MEDIA_PREPARATION_FAILED"_q); });
	});
	return AwaitSend(session, receipt, cancelled);
}

SendResult sendStickerSync(
		not_null<Main::Session*> session,
		Api::MessageToSend &&message,
		not_null<DocumentData*> document,
		Fn<bool()> cancelled) {
	const auto receipt = std::make_shared<SendReceipt>();
	const auto weak = base::make_weak(session);
	AyuSync::RunOnMain([=, message = std::move(message)]() mutable {
		if (!weak || (cancelled && cancelled())) {
			FinishSend(receipt, SendOutcome::Cancelled);
			return;
		}
		ObserveSend(session, receipt);
		receipt->progress.expect(1);
		const auto id = FullMsgId(
			message.action.history->peer->id,
			session->data().nextLocalMessageId());
		MessageCreated(receipt, id);
		Api::SendExistingDocument(std::move(message), document, id.msg);
	});
	return AwaitSend(session, receipt, cancelled);
}

SendResult sendVoiceSync(
		not_null<Main::Session*> session,
		const QByteArray &data,
		int64_t duration,
		bool video,
		Api::MessageToSend &&message,
		Fn<bool()> cancelled) {
	const auto receipt = std::make_shared<SendReceipt>();
	const auto weak = base::make_weak(session);
	AyuSync::RunOnMain([=] {
		if (!weak || (cancelled && cancelled())) {
			FinishSend(receipt, SendOutcome::Cancelled);
			return;
		}
		ObserveSend(session, receipt);
		receipt->progress.expect(1);
		const auto &action = message.action;
		auto to = FileLoadTo(
			action.history->peer->id,
			action.options,
			action.replyTo,
			action.replaceMediaOf);
		to.messageCreated = [=](FullMsgId id) { MessageCreated(receipt, id); };
		to.cancelled = [=] { return receipt->finished || (cancelled && cancelled()); };
		to.preparationFailed = [=] {
			FinishSend(receipt, SendOutcome::Failed, u"MEDIA_PREPARATION_FAILED"_q);
		};
		session->api().fileLoader()->addTask(std::make_unique<FileLoadTask>(FileLoadTask::VoiceArgs{
			.session = session,
			.voice = data,
			.duration = duration,
			.waveform = QVector<signed char>(),
			.video = video,
			.to = std::move(to),
			.caption = message.textWithTags,
		}));
	});
	return AwaitSend(session, receipt, cancelled);
}

namespace {

[[nodiscard]] MTPInputMedia UploadedInputMedia(
	const std::shared_ptr<FilePrepareResult> &prepared,
	const Api::RemoteFileInfo &info) {
	if (prepared->type == SendMediaType::Photo) {
		return MTP_inputMediaUploadedPhoto(
			MTP_flags(0),
			info.file,
			MTP_vector<MTPInputDocument>(),
			MTP_int(0),
			MTPInputDocument());
	}

	auto attributes = QVector<MTPDocumentAttribute>();
	prepared->document.match([&](const MTPDdocument &data)
							 {
								 attributes = data.vattributes().v;
							 },
							 [](const auto &)
							 {
							 });
	if (attributes.isEmpty()) {
		attributes.push_back(MTP_documentAttributeFilename(MTP_string(prepared->filename)));
	}

	using Flag = MTPDinputMediaUploadedDocument::Flag;
	auto flags = MTPDinputMediaUploadedDocument::Flags();
	if (prepared->forceFile) {
		flags |= Flag::f_force_file;
	}
	if (info.thumb) {
		flags |= Flag::f_thumb;
	}
	return MTP_inputMediaUploadedDocument(
		MTP_flags(flags),
		info.file,
		info.thumb.value_or(MTPInputFile()),
		MTP_string(prepared->filemime),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)),
		MTP_vector<MTPInputDocument>(),
		MTPInputPhoto(),
		MTP_int(0),
		MTP_int(0));
}

} // namespace

UploadedFile uploadFileSync(not_null<Main::Session*> session,
							not_null<PeerData*> peer,
							const QString &path,
							SendMediaType type,
							bool forceFile,
							const QString &displayName,
							Fn<bool()> cancelled) {
	if ((cancelled && cancelled()) || path.isEmpty() || !QFile::exists(path)) {
		return {};
	}

	auto task = FileLoadTask(FileLoadTask::Args{
		.session = session,
		.filepath = path,
		.type = type,
		.to = FileLoadTo(peer->id, Api::SendOptions(), FullReplyTo(), MsgId()),
		.forceFile = forceFile,
		.sendLargePhotos = (type == SendMediaType::Photo),
		.displayName = displayName,
	});
	task.process({.generateGoodThumbnail = false});

	const auto prepared = task.peekResult();
	if (!prepared) {
		return {};
	}

	const auto info = std::make_shared<std::optional<Api::RemoteFileInfo>>();
	const auto activeUpload = std::make_shared<FullMsgId>();
	auto uploadLatch = std::make_shared<TimedCountDownLatch>(1);
	auto lifetime = std::make_shared<rpl::lifetime>();

	AyuSync::RunOnMain([=]
	{
		if (cancelled && cancelled()) {
			uploadLatch->countDown();
			return;
		}
		const auto uploadId = FullMsgId(peer->id, session->data().nextLocalMessageId());
		*activeUpload = uploadId;
		const auto ready = [=](const Storage::UploadedMedia &data)
		{
			if (data.fullId != uploadId) {
				return;
			}
			*info = data.info;
			uploadLatch->countDown();
		};
		const auto failed = [=](const FullMsgId &id)
		{
			if (id == uploadId) {
				uploadLatch->countDown();
			}
		};

		session->uploader().photoReady() | rpl::on_next(ready, *lifetime);
		session->uploader().documentReady() | rpl::on_next(ready, *lifetime);
		session->uploader().photoFailed() | rpl::on_next(failed, *lifetime);
		session->uploader().documentFailed() | rpl::on_next(failed, *lifetime);

		session->uploader().upload(uploadId, prepared);
	});

	const auto uploadFinished = WaitForDownload(
		*uploadLatch, std::chrono::minutes(30), cancelled)
		== DownloadWaitResult::Completed;
	AyuSync::RunOnMainSync([&]
	{
		lifetime->destroy();
		if (!uploadFinished && activeUpload->msg) {
			session->uploader().cancel(*activeUpload);
		}
	});

	if (!uploadFinished || !info->has_value()) {
		return {};
	}

	const auto result = std::make_shared<UploadedFile>();
	auto api = std::make_shared<MTP::Sender>(&session->mtp());
	auto mediaLatch = std::make_shared<TimedCountDownLatch>(1);

	AyuSync::RunOnMain([=]
	{
		if (cancelled && cancelled()) {
			mediaLatch->countDown();
			return;
		}
		api->request(MTPmessages_UploadMedia(
			MTP_flags(0),
			MTPstring(),
			peer->input(),
			UploadedInputMedia(prepared, **info)
		)).done([=](const MTPMessageMedia &media)
		{
			media.match([&](const MTPDmessageMediaPhoto &data)
						{
							const auto photo = data.vphoto();
							if (photo && photo->type() == mtpc_photo) {
								result->photo = session->data().processPhoto(*photo);
							}
						},
						[&](const MTPDmessageMediaDocument &data)
						{
							const auto document = data.vdocument();
							if (document && document->type() == mtpc_document) {
								result->document = session->data().processDocument(*document);
							}
						},
						[](const auto &)
						{
						});
			mediaLatch->countDown();
		}).fail([=](const MTP::Error &)
		{
			mediaLatch->countDown();
		}).send();
	});

	const auto mediaFinished = WaitForDownload(
		*mediaLatch, std::chrono::minutes(5), cancelled)
		== DownloadWaitResult::Completed;
	AyuSync::RunOnMainSync([&] { api.reset(); });

	return mediaFinished ? *result : UploadedFile();
}

std::shared_ptr<const Iv::RichPage> loadFullRichPageSync(
		not_null<Main::Session*> session,
		FullMsgId itemId,
		Fn<bool()> cancelled) {
	const auto resolved = std::make_shared<std::shared_ptr<const Iv::RichPage>>();
	auto latch = std::make_shared<TimedCountDownLatch>(1);
	auto api = std::make_shared<MTP::Sender>(&session->mtp());

	AyuSync::RunOnMain([=]
	{
		if (cancelled && cancelled()) {
			latch->countDown();
			return;
		}
		const auto item = session->data().message(itemId);
		if (!item) {
			latch->countDown();
			return;
		}
		if (const auto full = item->fullRichPage()) {
			*resolved = full;
			latch->countDown();
			return;
		}

		const auto page = item->richPage();
		if (!page) {
			latch->countDown();
			return;
		}
		if (!page->part || item->isLocal() || item->isDeleted()) {
			*resolved = page;
			latch->countDown();
			return;
		}

		const auto peer = item->history()->peer;

		api->request(MTPmessages_GetRichMessage(
			peer->input(),
			MTP_int(itemId.msg)
		)).done([=](const MTPmessages_Messages &result)
		{
			auto full = std::shared_ptr<const Iv::RichPage>();
			const auto process = [&](const auto &data)
			{
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				peer->processTopics(data.vtopics());
				for (const auto &message : data.vmessages().v) {
					if (message.type() != mtpc_message) {
						continue;
					}
					const auto &fields = message.c_message();
					if (MsgId(fields.vid().v) != itemId.msg) {
						continue;
					}
					if (const auto richMessage = fields.vrich_message()) {
						full = Iv::ParseRichPage(session, *richMessage);
					}
					break;
				}
			};
			result.match([](const MTPDmessages_messagesNotModified &)
						 {
						 },
						 [&](const MTPDmessages_channelMessages &data)
						 {
							 process(data);
							 if (const auto channel = peer->asChannel()) {
								 channel->ptsReceived(data.vpts().v);
							 }
						 },
						 [&](const auto &data)
						 {
							 process(data);
						 });
			if (full) {
				if (const auto current = session->data().message(itemId)) {
					current->setFullRichPage(full);
				}
				*resolved = full;
			}
			latch->countDown();
		}).fail([=](const MTP::Error &)
		{
			latch->countDown();
		}).send();
	});

	const auto finished = WaitForDownload(
		*latch, std::chrono::minutes(1), cancelled)
		== DownloadWaitResult::Completed;
	AyuSync::RunOnMainSync([&] { api.reset(); });

	return finished ? *resolved : nullptr;
}

bool sendRichMessageSync(not_null<Main::Session*> session,
						 const MTPInputRichMessage &richMessage,
						 const Api::SendAction &action,
						 Fn<bool()> cancelled) {
	const auto sent = std::make_shared<bool>(false);
	auto latch = std::make_shared<TimedCountDownLatch>(1);
	auto api = std::make_shared<MTP::Sender>(&session->mtp());

	AyuSync::RunOnMain([=]
	{
		if (cancelled && cancelled()) {
			latch->countDown();
			return;
		}
		auto sending = action;
		sending.options.scheduled = 0;
		sending.options.scheduleRepeatPeriod = 0;
		sending.options.suggest = {};
		sending.options.effectId = 0;
		StripEphemeralReply(session, sending.replyTo);
		applyGhostScheduling(session, sending.options);

		const auto peer = sending.history->peer;
		const auto starsPaid = std::min(
			peer->starsPerMessageChecked(),
			sending.options.starsApproved);

		using Flag = MTPmessages_SendMessage::Flag;
		auto sendFlags = MTPmessages_SendMessage::Flags(0)
			| Flag::f_rich_message;
		if (sending.replyTo) {
			sendFlags |= Flag::f_reply_to;
		}
		if (ShouldSendSilent(peer, sending.options)) {
			sendFlags |= Flag::f_silent;
		}
		if (sending.options.scheduled) {
			sendFlags |= Flag::f_schedule_date;
		}
		if (sending.options.sendAs) {
			sendFlags |= Flag::f_send_as;
		}
		if (starsPaid) {
			sendFlags |= Flag::f_allow_paid_stars;
		}

		api->request(MTPmessages_SendMessage(
			MTP_flags(sendFlags),
			peer->input(),
			sending.mtpReplyTo(),
			MTP_string(QString()),
			MTP_long(base::RandomValue<uint64>()),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTP_int(sending.options.scheduled),
			MTP_int(0), // schedule_repeat_period
			(sending.options.sendAs
				? sending.options.sendAs->input()
				: MTP_inputPeerEmpty()),
			MTPInputQuickReplyShortcut(),
			MTP_long(0), // effect
			MTP_long(starsPaid),
			Api::SuggestToMTP(sending.options.suggest),
			richMessage
		)).done([=](const MTPUpdates &result)
		{
			session->api().applyUpdates(result);
			*sent = true;
			latch->countDown();
		}).fail([=](const MTP::Error &error)
		{
			LOG(("AyuForward: rich message send failed: %1").arg(error.type()));
			latch->countDown();
		}).send();
	});

	const auto finished = WaitForDownload(
		*latch, std::chrono::minutes(2), cancelled)
		== DownloadWaitResult::Completed;
	AyuSync::RunOnMainSync([&] { api.reset(); });

	return finished && *sent;
}

} // namespace AyuSync
