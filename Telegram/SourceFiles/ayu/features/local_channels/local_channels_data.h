#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>
#include <vector>

namespace Ayu {

inline constexpr auto kLocalChannelsLimit = 1000;
inline constexpr auto kLocalChannelsFileLimit = 1024 * 1024;
inline constexpr auto kLocalChannelTitleLimit = 128;

struct LocalChannelEntry {
	QString username;
	QString title;
	uint64 id = 0;

	friend bool operator==(
		const LocalChannelEntry &,
		const LocalChannelEntry &) = default;
};

[[nodiscard]] QString LocalChannelUsername(QString text);
[[nodiscard]] std::optional<std::vector<LocalChannelEntry>> ParseLocalChannels(
	const QByteArray &bytes);
[[nodiscard]] QByteArray SerializeLocalChannels(
	const std::vector<LocalChannelEntry> &entries);
[[nodiscard]] bool SameLocalChannel(
	const LocalChannelEntry &a,
	const LocalChannelEntry &b);

} // namespace Ayu
