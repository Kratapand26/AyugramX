#include "ayu/features/local_channels/local_channels_data.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>

namespace Ayu {
namespace {

constexpr auto kFormat = "ayugramx.local_channels";
} // namespace

QString LocalChannelUsername(QString text) {
	text = text.trimmed();
	if (text.startsWith('@')) {
		text.remove(0, 1);
	} else if (text.contains('/') || text.contains(':')) {
		if (!text.contains(u"://"_q)) {
			text.prepend(u"https://"_q);
		}
		const auto url = QUrl(text, QUrl::StrictMode);
		const auto host = url.host().toLower();
		if (!url.isValid()
			|| (url.scheme() != u"https"_q && url.scheme() != u"http"_q)
			|| (host != u"t.me"_q && host != u"telegram.me"_q)
			|| !url.userInfo().isEmpty()
			|| url.port() != -1
			|| url.hasQuery()
			|| url.hasFragment()) {
			return {};
		}
		auto parts = url.path().split('/', Qt::SkipEmptyParts);
		if (!parts.isEmpty() && parts.front() == u"s"_q) {
			parts.removeFirst();
		}
		if (parts.size() == 2) {
			auto validPost = false;
			const auto post = parts.back().toULongLong(&validPost);
			if (!validPost || !post) {
				return {};
			}
			parts.removeLast();
		}
		if (parts.size() != 1) {
			return {};
		}
		text = parts.front();
	}
	static const auto pattern = QRegularExpression(
		u"\\A[A-Za-z][A-Za-z0-9_]{0,31}\\z"_q);
	if (!pattern.match(text).hasMatch()
		|| text.compare(u"joinchat"_q, Qt::CaseInsensitive) == 0
		|| text.compare(u"c"_q, Qt::CaseInsensitive) == 0) {
		return {};
	}
	return text.toLower();
}

bool SameLocalChannel(
		const LocalChannelEntry &a,
		const LocalChannelEntry &b) {
	return (a.id && b.id && a.id == b.id)
		|| a.username.compare(b.username, Qt::CaseInsensitive) == 0;
}

std::optional<std::vector<LocalChannelEntry>> ParseLocalChannels(
		const QByteArray &bytes) {
	if (bytes.size() > kLocalChannelsFileLimit) {
		return std::nullopt;
	}
	const auto document = QJsonDocument::fromJson(bytes);
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto root = document.object();
	if (root.value(u"format"_q).toString() != QLatin1String(kFormat)
		|| root.value(u"version"_q).toDouble() != 1.
		|| !root.value(u"channels"_q).isArray()) {
		return std::nullopt;
	}
	const auto channels = root.value(u"channels"_q).toArray();
	if (channels.size() > kLocalChannelsLimit) {
		return std::nullopt;
	}
	auto result = std::vector<LocalChannelEntry>();
	result.reserve(channels.size());
	for (const auto &value : channels) {
		if (!value.isObject()) {
			return std::nullopt;
		}
		const auto object = value.toObject();
		const auto username = LocalChannelUsername(
			object.value(u"username"_q).toString());
		const auto title = object.value(u"title"_q).toString();
		if (username.isEmpty() || title.size() > kLocalChannelTitleLimit) {
			return std::nullopt;
		}
		auto id = uint64(0);
		if (object.contains(u"id"_q)) {
			auto validId = false;
			id = object.value(u"id"_q).toString().toULongLong(&validId);
			if (!validId || id >= (uint64(1) << 52)) {
				return std::nullopt;
			}
		}
		const auto entry = LocalChannelEntry{ username, title, id };
		if (!ranges::any_of(result, [&](const LocalChannelEntry &other) {
			return SameLocalChannel(entry, other);
		})) {
			result.push_back(entry);
		}
	}
	return result;
}

QByteArray SerializeLocalChannels(
		const std::vector<LocalChannelEntry> &entries) {
	auto channels = QJsonArray();
	for (const auto &entry : entries) {
		channels.push_back(QJsonObject{
			{ u"username"_q, entry.username },
			{ u"url"_q, u"https://t.me/"_q + entry.username },
			{ u"title"_q, entry.title },
			{ u"id"_q, QString::number(entry.id) },
		});
	}
	return QJsonDocument(QJsonObject{
		{ u"format"_q, QLatin1String(kFormat) },
		{ u"version"_q, 1 },
		{ u"channels"_q, channels },
	}).toJson(QJsonDocument::Indented);
}

} // namespace Ayu
