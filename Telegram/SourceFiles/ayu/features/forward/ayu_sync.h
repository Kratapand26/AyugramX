// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "apiwrap.h"
#include "ayu/features/forward/ayu_forward_result.h"
#include "base/flat_map.h"
#include "base/random.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "history/history_item.h"
#include "storage/file_download.h"
#include "storage/file_upload.h"
#include "storage/storage_account.h"
#include "ui/chat/attach/attach_prepare.h"

namespace Iv {
struct RichPage;
} // namespace Iv

namespace AyuSync {

void RunOnMain(FnMut<void()> callback);
void RunOnMainSync(FnMut<void()> callback);

using DocumentPaths = base::flat_map<not_null<DocumentData*>, QString>;

struct UploadedFile
{
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
};

QString pathForSave(not_null<Main::Session*> session);
QString documentFileName(not_null<DocumentData*> document);
QString filePath(not_null<Main::Session*> session, not_null<PhotoData*> photo);
QString filePath(not_null<Main::Session*> session, not_null<DocumentData*> document);
qint64 fileSize(const QString &path);
[[nodiscard]] DocumentPaths loadDocuments(
	not_null<Main::Session*> session,
	const std::vector<not_null<HistoryItem*>> &items,
	const Fn<bool()> &cancelled,
	int concurrency = 1);
SendResult sendMessageSync(
	not_null<Main::Session*> session,
	Api::MessageToSend &&message,
	Fn<bool()> cancelled = nullptr);

SendResult sendDocumentSync(
	not_null<Main::Session*> session,
	Ui::PreparedGroup &group,
	SendMediaType type,
	TextWithTags &&caption,
	const Api::SendAction &action,
	Fn<bool()> cancelled = nullptr);
SendResult sendStickerSync(
	not_null<Main::Session*> session,
	Api::MessageToSend &&message,
	not_null<DocumentData*> document,
	Fn<bool()> cancelled = nullptr);
void loadPhotoSync(
	not_null<Main::Session*> session,
	not_null<PhotoData*> photo,
	Data::FileOrigin origin,
	const Fn<bool()> &cancelled);
[[nodiscard]] QString loadDocumentSync(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document,
	Data::FileOrigin origin,
	const Fn<bool()> &cancelled);
SendResult forwardMessagesSync(
	not_null<Main::Session*> session,
	const std::vector<not_null<HistoryItem*>> &items,
	const ApiWrap::SendAction &action,
	Data::ForwardOptions options,
	Fn<bool()> cancelled = nullptr);
SendResult sendVoiceSync(
	not_null<Main::Session*> session,
	const QByteArray &data,
	int64_t duration,
	bool video,
	Api::MessageToSend &&message,
	Fn<bool()> cancelled = nullptr);
UploadedFile uploadFileSync(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	const QString &path,
	SendMediaType type,
	bool forceFile,
	const QString &displayName = {},
	Fn<bool()> cancelled = nullptr);

std::shared_ptr<const Iv::RichPage> loadFullRichPageSync(
	not_null<Main::Session*> session,
	FullMsgId itemId,
	Fn<bool()> cancelled = nullptr);

bool sendRichMessageSync(
	not_null<Main::Session*> session,
	const MTPInputRichMessage &richMessage,
	const Api::SendAction &action,
	Fn<bool()> cancelled = nullptr);

} // namespace AyuSync
