#pragma once

#include "ayu/features/local_channels/local_channels_data.h"
#include "mtproto/sender.h"
#include "rpl/event_stream.h"
#include "rpl/variable.h"

class ChannelData;

namespace Main {
class Session;
} // namespace Main

namespace Ayu {

class LocalChannels final {
public:
	explicit LocalChannels(not_null<Main::Session*> session);

	[[nodiscard]] bool enabled() const;
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	void setEnabled(bool enabled);

	[[nodiscard]] const std::vector<LocalChannelEntry> &entries() const;
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] bool contains(not_null<ChannelData*> channel) const;
	[[nodiscard]] ChannelData *cachedChannel(
		const LocalChannelEntry &entry) const;
	[[nodiscard]] QString add(not_null<ChannelData*> channel);
	void remove(const QString &username);
	[[nodiscard]] std::optional<int> importChannels(const QByteArray &bytes);
	[[nodiscard]] QByteArray exportChannels() const;

	mtpRequestId resolve(
		const LocalChannelEntry &entry,
		Fn<void(not_null<ChannelData*>)> done,
		Fn<void(QString)> fail);
	[[nodiscard]] bool resolving(mtpRequestId requestId) const;
	void cancel(mtpRequestId requestId);

private:
	void save();
	[[nodiscard]] QString waitText() const;

	const not_null<Main::Session*> _session;
	std::vector<LocalChannelEntry> _entries;
	rpl::variable<bool> _enabled = false;
	rpl::event_stream<> _changes;
	int64 _retryAt = 0;
	crl::time _nextResolveAt = 0;
	mtpRequestId _requestId = 0;
	MTP::Sender _api;

};

} // namespace Ayu
