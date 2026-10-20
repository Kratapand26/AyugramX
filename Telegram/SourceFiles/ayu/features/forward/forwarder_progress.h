// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace AyuForwarder {

class BatchProgress {
public:
	BatchProgress(std::vector<std::int64_t> ids, std::int64_t lastProcessed)
	: _ids(std::move(ids))
	, _lastProcessed(lastProcessed) {
		std::sort(_ids.begin(), _ids.end());
		_ids.erase(std::unique(_ids.begin(), _ids.end()), _ids.end());
		while (_next < _ids.size() && _ids[_next] <= _lastProcessed) {
			++_next;
		}
	}

	void complete(std::int64_t id) {
		if (id <= _lastProcessed
			|| !std::binary_search(_ids.begin() + _next, _ids.end(), id)) {
			return;
		}
		_completed.emplace(id);
		while (_next < _ids.size() && _completed.erase(_ids[_next])) {
			_lastProcessed = _ids[_next++];
		}
	}

	[[nodiscard]] std::int64_t lastProcessedId() const {
		return _lastProcessed;
	}

private:
	std::vector<std::int64_t> _ids;
	std::set<std::int64_t> _completed;
	std::size_t _next = 0;
	std::int64_t _lastProcessed = 0;

};

template <typename Item, typename GroupId>
[[nodiscard]] std::vector<Item> TakeCompleteHistoryPage(
		std::vector<Item> &pending,
		bool exhausted,
		GroupId groupId) {
	auto result = std::move(pending);
	pending.clear();
	if (!exhausted && !result.empty()) {
		const auto lastGroup = groupId(result.back());
		if (lastGroup) {
			const auto tail = std::find_if(result.begin(), result.end(), [&](const auto &item) {
				return groupId(item) == lastGroup;
			});
			pending.assign(tail, result.end());
			result.erase(tail, result.end());
		}
	}
	return result;
}

template <typename Id>
class SendProgress {
public:
	void expect(int count) {
		_expected = count;
	}

	void finishCreating() {
		_expected = _created;
	}

	void created(Id id) {
		if (_pending.emplace(std::move(id)).second) {
			++_created;
		}
	}

	[[nodiscard]] bool confirm(const Id &id) {
		if (!_pending.erase(id)) {
			return false;
		}
		++_confirmed;
		return true;
	}

	[[nodiscard]] bool complete() const {
		return _expected > 0 && _confirmed == _expected;
	}

	[[nodiscard]] int expected() const {
		return _expected;
	}

	[[nodiscard]] int confirmed() const {
		return _confirmed;
	}

	[[nodiscard]] bool contains(const Id &id) const {
		return _pending.find(id) != _pending.end();
	}

	[[nodiscard]] const std::set<Id> &pending() const {
		return _pending;
	}

private:
	std::set<Id> _pending;
	int _expected = -1;
	int _created = 0;
	int _confirmed = 0;

};

} // namespace AyuForwarder
