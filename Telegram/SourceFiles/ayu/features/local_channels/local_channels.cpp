#include "ayu/features/local_channels/local_channels.h"

#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/text/format_values.h"

namespace Ayu {
namespace {

constexpr auto kEnabledPref = "ayu_local_channels_enabled"_cs;
constexpr auto kEntriesPref = "ayu_local_channels_entries"_cs;
constexpr auto kRetryAtPref = "ayu_local_channels_retry_at"_cs;
constexpr auto kResolveInterval = crl::time(1000);

[[nodiscard]] LocalChannelEntry EntryFor(not_null<ChannelData*> channel) {
	return {
		LocalChannelUsername(channel->username()),
		channel->name().left(kLocalChannelTitleLimit),
		peerToChannel(channel->id).bare,
	};
}

} // namespace

LocalChannels::LocalChannels(not_null<Main::Session*> session)
: _session(session)
, _enabled(session->local().readPref<bool>(kEnabledPref))
, _api(&session->mtp()) {
	const auto stored = session->local().readPref<QString>(kEntriesPref);
	if (const auto parsed = ParseLocalChannels(stored.toUtf8())) {
		_entries = *parsed;
	}
	_retryAt = session->local().readPref<QString>(kRetryAtPref).toLongLong();
}

bool LocalChannels::enabled() const {
	return _enabled.current();
}

rpl::producer<bool> LocalChannels::enabledValue() const {
	return _enabled.value();
}

void LocalChannels::setEnabled(bool enabled) {
	if (enabled == this->enabled()) {
		return;
	}
	_session->local().writePref<bool>(kEnabledPref, enabled);
	_enabled = enabled;
	if (!enabled) {
		cancel(_requestId);
	}
	_changes.fire({});
}

const std::vector<LocalChannelEntry> &LocalChannels::entries() const {
	return _entries;
}

rpl::producer<> LocalChannels::changes() const {
	return _changes.events();
}

bool LocalChannels::contains(not_null<ChannelData*> channel) const {
	const auto entry = EntryFor(channel);
	return ranges::any_of(_entries, [&](const LocalChannelEntry &other) {
		return other.id
			? other.id == entry.id
			: other.username == entry.username;
	});
}

QString LocalChannels::add(not_null<ChannelData*> channel) {
	const auto entry = EntryFor(channel);
	if (!enabled()
		|| !channel->isBroadcast()
		|| channel->isForbidden()
		|| entry.username.isEmpty()) {
		return tr::ayu_LocalChannelsPublicOnly(tr::now);
	}
	const auto i = ranges::find_if(_entries, [&](const auto &other) {
		return SameLocalChannel(entry, other);
	});
	if (i != end(_entries)) {
		if (*i == entry) {
			return {};
		}
		*i = entry;
	} else if (_entries.size() < kLocalChannelsLimit) {
		_entries.push_back(entry);
	} else {
		return tr::ayu_LocalChannelsFull(tr::now);
	}
	save();
	return {};
}

void LocalChannels::remove(const QString &username) {
	const auto i = ranges::find(_entries, username, &LocalChannelEntry::username);
	if (i != end(_entries)) {
		_entries.erase(i);
		save();
	}
}

std::optional<int> LocalChannels::importChannels(const QByteArray &bytes) {
	const auto parsed = ParseLocalChannels(bytes);
	if (!parsed) {
		return std::nullopt;
	}
	auto merged = _entries;
	auto added = 0;
	for (const auto &entry : *parsed) {
		if (ranges::any_of(merged, [&](const LocalChannelEntry &other) {
			return SameLocalChannel(entry, other);
		})) {
			continue;
		} else if (merged.size() == kLocalChannelsLimit) {
			return std::nullopt;
		}
		merged.push_back(entry);
		++added;
	}
	if (added) {
		_entries = std::move(merged);
		save();
	}
	return added;
}

QByteArray LocalChannels::exportChannels() const {
	return SerializeLocalChannels(_entries);
}

QString LocalChannels::waitText() const {
	return tr::ayu_LocalChannelsWait(
		tr::now,
		lt_duration,
		Ui::FormatDurationWords(std::max(
			int64(1),
			_retryAt - base::unixtime::now())));
}

mtpRequestId LocalChannels::resolve(
		const LocalChannelEntry &entry,
		Fn<void(not_null<ChannelData*>)> done,
		Fn<void(QString)> fail) {
	const auto username = LocalChannelUsername(entry.username);
	if (!enabled()) {
		fail(tr::ayu_LocalChannelsEnableFirst(tr::now));
		return 0;
	} else if (username.isEmpty()) {
		fail(tr::ayu_LocalChannelsPublicOnly(tr::now));
		return 0;
	} else if (_retryAt > base::unixtime::now()) {
		fail(waitText());
		return 0;
	} else if (_requestId || crl::now() < _nextResolveAt) {
		fail(tr::ayu_LocalChannelsBusy(tr::now));
		return 0;
	}
	_nextResolveAt = crl::now() + kResolveInterval;
	_requestId = _api.request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(username),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		_requestId = 0;
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		const auto peerId = peerFromMTP(data.vpeer());
		const auto channel = peerId
			? _session->data().peer(peerId)->asBroadcast()
			: nullptr;
		if (!channel || channel->isForbidden()) {
			fail(tr::ayu_LocalChannelsPublicOnly(tr::now));
		} else if (entry.id && entry.id != peerToChannel(channel->id).bare) {
			fail(tr::ayu_LocalChannelsChanged(tr::now));
		} else {
			done(channel);
		}
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		if (MTP::IsFloodError(error)) {
			_retryAt = int64(base::unixtime::now())
				+ std::max(1, MTP::FloodWaitDuration(error));
			_session->local().writePref<QString>(
				kRetryAtPref,
				QString::number(_retryAt));
			fail(waitText());
		} else if (MTP::IgnoreError(error)) {
			fail({});
		} else {
			fail(tr::ayu_LocalChannelsUnavailable(tr::now));
		}
	}).handleAllErrors().send();
	return _requestId;
}

bool LocalChannels::resolving(mtpRequestId requestId) const {
	return requestId && requestId == _requestId;
}

void LocalChannels::cancel(mtpRequestId requestId) {
	if (resolving(requestId)) {
		_api.request(base::take(_requestId)).cancel();
	}
}

void LocalChannels::save() {
	_session->local().writePref<QString>(
		kEntriesPref,
		QString::fromUtf8(exportChannels()));
	_changes.fire({});
}

} // namespace Ayu
