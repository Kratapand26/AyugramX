#include "ayu/features/chat_stickers/chat_stickers_cache.h"

#include <QtCore/QDataStream>
#include <QtCore/QIODevice>

#include <set>

namespace Ayu {
namespace {

constexpr auto kMagic = quint32(0x41594353);
constexpr auto kVersion = quint32(1);
constexpr auto kShortNameBytesLimit = 1024;

void WriteBytes(QDataStream &stream, const QByteArray &bytes) {
	stream << quint32(bytes.size());
	stream.writeRawData(bytes.constData(), bytes.size());
}

QByteArray ReadBytes(QDataStream &stream, int limit) {
	auto size = quint32(0);
	stream >> size;
	if (size > quint32(limit) || size > stream.device()->bytesAvailable()) {
		stream.setStatus(QDataStream::ReadCorruptData);
		return {};
	}
	auto result = QByteArray(int(size), Qt::Uninitialized);
	if (stream.readRawData(result.data(), result.size()) != result.size()) {
		stream.setStatus(QDataStream::ReadCorruptData);
		return {};
	}
	return result;
}

} // namespace

Storage::Cache::Key ChatStickersCacheKey(PeerId peerId, MsgId topicRootId) {
	return Data::UrlCacheKey(u"ayugramx:chat-stickers:v1:"_q
		+ QString::number(peerId.value)
		+ ':'
		+ QString::number(topicRootId.bare));
}

QByteArray SerializeChatStickersCache(const SavedChatStickers &cache) {
	if (cache.documents.size() > kChatStickersMessageLimit
		|| cache.messages.size() > kChatStickersMessageLimit) {
		return {};
	}
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< kMagic
		<< kVersion
		<< qint32(cache.appVersion)
		<< quint64(cache.accountId)
		<< quint64(cache.peerId.value)
		<< qint64(cache.topicRootId.bare)
		<< qint64(cache.offsetId.bare)
		<< qint32(cache.scanned)
		<< quint8(cache.finished ? 1 : 0)
		<< quint32(cache.documents.size());
	for (const auto &document : cache.documents) {
		const auto name = document.setShortName.toUtf8();
		if (name.size() > kShortNameBytesLimit
			|| document.bytes.isEmpty()
			|| document.bytes.size() > kChatStickerDocumentBytesLimit) {
			return {};
		}
		stream
			<< quint64(document.id)
			<< quint64(document.setId)
			<< quint64(document.setAccessHash);
		WriteBytes(stream, name);
		WriteBytes(stream, document.bytes);
		if (result.size() > kChatStickersCacheBytesLimit) {
			return {};
		}
	}
	stream << quint32(cache.messages.size());
	for (const auto &message : cache.messages) {
		stream
			<< qint64(message.messageId.bare)
			<< quint64(message.documentId)
			<< qint32(message.date);
	}
	return (stream.status() == QDataStream::Ok
		&& result.size() <= kChatStickersCacheBytesLimit) ? result : QByteArray();
}

std::optional<SavedChatStickers> ParseChatStickersCache(
		const QByteArray &bytes,
		uint64 accountId,
		PeerId peerId,
		MsgId topicRootId,
		int appVersion) {
	if (bytes.isEmpty() || bytes.size() > kChatStickersCacheBytesLimit) {
		return std::nullopt;
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32(0);
	auto version = quint32(0);
	auto savedVersion = qint32(0);
	auto savedAccount = quint64(0);
	auto savedPeer = quint64(0);
	auto topic = qint64(0);
	auto offset = qint64(0);
	auto scanned = qint32(0);
	auto finished = quint8(0);
	auto count = quint32(0);
	stream >> magic >> version >> savedVersion >> savedAccount >> savedPeer
		>> topic >> offset >> scanned >> finished >> count;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version != kVersion
		|| savedVersion <= 0
		|| savedVersion > appVersion
		|| savedAccount != accountId
		|| savedPeer != peerId.value
		|| MsgId(topic) != topicRootId
		|| (topic && !IsServerMsgId(topic))
		|| (offset && !IsServerMsgId(offset))
		|| scanned < 0
		|| finished > 1
		|| count > kChatStickersMessageLimit) {
		return std::nullopt;
	}
	auto result = SavedChatStickers();
	result.accountId = accountId;
	result.peerId = peerId;
	result.topicRootId = topicRootId;
	result.offsetId = offset;
	result.scanned = scanned;
	result.appVersion = savedVersion;
	result.finished = finished;
	result.documents.reserve(count);
	auto documents = std::set<DocumentId>();
	for (auto i = quint32(0); i != count; ++i) {
		auto id = quint64(0);
		auto setId = quint64(0);
		auto accessHash = quint64(0);
		stream >> id >> setId >> accessHash;
		auto name = ReadBytes(stream, kShortNameBytesLimit);
		auto metadata = ReadBytes(stream, kChatStickerDocumentBytesLimit);
		auto metadataId = quint64(0);
		auto metadataStream = QDataStream(metadata);
		metadataStream.setVersion(QDataStream::Qt_5_1);
		metadataStream >> metadataId;
		if (stream.status() != QDataStream::Ok
			|| metadataStream.status() != QDataStream::Ok
			|| !id
			|| metadataId != id
			|| metadata.isEmpty()
			|| !documents.emplace(id).second) {
			return std::nullopt;
		}
		result.documents.push_back({
			id,
			setId,
			accessHash,
			QString::fromUtf8(name),
			std::move(metadata),
		});
	}
	stream >> count;
	if (count > kChatStickersMessageLimit) {
		return std::nullopt;
	}
	result.messages.reserve(count);
	auto messages = std::set<MsgId>();
	for (auto i = quint32(0); i != count; ++i) {
		auto id = qint64(0);
		auto documentId = quint64(0);
		auto date = qint32(0);
		stream >> id >> documentId >> date;
		if (stream.status() != QDataStream::Ok
			|| !IsServerMsgId(id)
			|| !documents.contains(documentId)
			|| !messages.emplace(id).second) {
			return std::nullopt;
		}
		result.messages.push_back({ id, documentId, date });
	}
	return (stream.status() == QDataStream::Ok && stream.atEnd())
		? std::make_optional(std::move(result))
		: std::nullopt;
}

} // namespace Ayu
