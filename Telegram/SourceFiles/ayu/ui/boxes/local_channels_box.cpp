#include "ayu/ui/boxes/local_channels_box.h"

#include "ayu/features/local_channels/local_channels.h"
#include "base/weak_ptr.h"
#include "core/file_utilities.h"
#include "data/data_channel.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QFile>
#include <QtCore/QSaveFile>

#include <algorithm>

#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Ayu {
namespace {

constexpr auto kChannelsPerPage = 50;

class LocalChannelRow final : public Ui::RpWidget {
public:
	LocalChannelRow(
		QWidget *parent,
		const QString &label,
		Fn<void()> open,
		Fn<void()> remove);

private:
	int resizeGetHeight(int width) override;

	const not_null<Ui::SettingsButton*> _open;
	const not_null<Ui::LinkButton*> _remove;

};

struct OperationState {
	mtpRequestId requestId = 0;
	bool refreshScheduled = false;
	QString query;
	int page = 0;
};

LocalChannelRow::LocalChannelRow(
		QWidget *parent,
		const QString &label,
		Fn<void()> open,
		Fn<void()> remove)
: RpWidget(parent)
, _open(Ui::CreateChild<Ui::SettingsButton>(
	this,
	rpl::single(label),
	st::settingsButtonNoIcon))
, _remove(Ui::CreateChild<Ui::LinkButton>(
	this,
	tr::ayu_LocalChannelsRemove(tr::now))) {
	_open->setClickedCallback(std::move(open));
	_remove->setClickedCallback(std::move(remove));
}

int LocalChannelRow::resizeGetHeight(int width) {
	_open->resizeToWidth(std::max(
		0,
		width - _remove->width() - st::boxPadding.right()));
	_open->moveToLeft(0, 0, width);
	_remove->moveToRight(
		st::boxPadding.right(),
		(_open->height() - _remove->height()) / 2,
		width);
	return _open->height();
}

void ShowError(
		not_null<Window::SessionController*> controller,
		const QString &text) {
	if (!text.isEmpty()) {
		controller->showToast(text);
	}
}

void AddChannelBox(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	const auto channels = &controller->session().localChannels();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_LocalChannelsAdd());
		box->setWidth(st::boxWideWidth);
		const auto state = box->lifetime().make_state<OperationState>();
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::ayu_LocalChannelsPlaceholder()));
		const auto add = [=] {
			if (!weak || channels->resolving(state->requestId)) {
				return;
			}
			const auto username = LocalChannelUsername(field->getLastText());
			if (username.isEmpty()) {
				field->showError();
				return;
			}
			state->requestId = channels->resolve(
				{ username },
				crl::guard(box, [=](not_null<ChannelData*> channel) {
					state->requestId = 0;
					if (!weak) {
						return;
					}
					const auto error = channels->add(channel);
					if (!error.isEmpty()) {
						ShowError(weak.get(), error);
						return;
					}
					weak->showToast(tr::ayu_LocalChannelsSaved(tr::now));
					box->closeBox();
				}),
				crl::guard(box, [=](QString error) {
					state->requestId = 0;
					if (weak) {
						ShowError(weak.get(), error);
					}
				}));
		};
		box->addButton(tr::ayu_LocalChannelsFollow(), add);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		field->submits() | rpl::on_next(add, box->lifetime());
		box->boxClosing() | rpl::on_next([=] {
			channels->cancel(state->requestId);
		}, box->lifetime());
		box->setFocusCallback([=] { field->setFocusFast(); });
	}));
}

void ImportChannels(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	const auto channels = &controller->session().localChannels();
	FileDialog::GetOpenPath(
		box.get(),
		tr::ayu_LocalChannelsImport(tr::now),
		u"JSON (*.json)"_q,
		crl::guard(box, [=](FileDialog::OpenResult &&result) {
			if (!weak) {
				return;
			}
			auto bytes = result.remoteContent;
			if (!result.paths.isEmpty()) {
				auto file = QFile(result.paths.front());
				if (!file.open(QIODevice::ReadOnly)
					|| file.size() > kLocalChannelsFileLimit) {
					ShowError(weak.get(), tr::ayu_LocalChannelsInvalidFile(tr::now));
					return;
				}
				bytes = file.read(kLocalChannelsFileLimit + 1);
			} else if (bytes.isEmpty()) {
				return;
			}
			const auto parsed = ParseLocalChannels(bytes);
			if (!parsed) {
				ShowError(weak.get(), tr::ayu_LocalChannelsInvalidFile(tr::now));
				return;
			}
			weak->show(Ui::MakeConfirmBox({
				.text = tr::ayu_LocalChannelsImportConfirm(
					tr::now,
					lt_count,
					int(parsed->size())),
				.confirmed = crl::guard(box, [=] {
					if (!weak) {
						return;
					}
					const auto added = channels->importChannels(bytes);
					if (added) {
						weak->showToast(tr::ayu_LocalChannelsImported(
							tr::now,
							lt_count,
							*added));
					} else {
						ShowError(weak.get(), tr::ayu_LocalChannelsFull(tr::now));
					}
				}),
				.confirmText = tr::ayu_LocalChannelsImport(tr::now),
			}));
		}));
}

void ExportChannels(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	const auto bytes = controller->session().localChannels().exportChannels();
	FileDialog::GetWritePath(
		box.get(),
		tr::ayu_LocalChannelsExport(tr::now),
		u"JSON (*.json)"_q,
		u"local-channels.json"_q,
		crl::guard(box, [=](QString &&path) {
			if (!weak || path.isEmpty()) {
				return;
			}
			auto file = QSaveFile(path);
			if (!file.open(QIODevice::WriteOnly)
				|| file.write(bytes) != bytes.size()
				|| !file.commit()) {
				ShowError(weak.get(), tr::ayu_LocalChannelsExportFailed(tr::now));
				return;
			}
			weak->showToast(tr::ayu_LocalChannelsExported(tr::now));
		}));
}

void OpenChannel(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<OperationState*> state,
		const LocalChannelEntry &entry) {
	const auto weak = base::make_weak(controller);
	const auto channels = &controller->session().localChannels();
	if (const auto channel = channels->cachedChannel(entry)) {
		channels->cancel(state->requestId);
		state->requestId = 0;
		const auto error = channels->add(channel);
		if (!error.isEmpty()) {
			ShowError(controller, error);
			return;
		}
		box->closeBox();
		if (weak) {
			weak->showPeerHistory(channel);
		}
		return;
	}
	if (channels->resolving(state->requestId)) {
		return;
	}
	state->requestId = channels->resolve(
		entry,
		crl::guard(box, [=](not_null<ChannelData*> channel) {
			state->requestId = 0;
			if (!weak) {
				return;
			}
			const auto error = channels->add(channel);
			if (!error.isEmpty()) {
				ShowError(weak.get(), error);
				return;
			}
			box->closeBox();
			weak->showPeerHistory(channel);
		}),
		crl::guard(box, [=](QString error) {
			state->requestId = 0;
			if (weak) {
				ShowError(weak.get(), error);
			}
		}));
}

void FillRows(
		not_null<Ui::VerticalLayout*> rows,
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<OperationState*> state,
		const QString &query) {
	const auto weak = base::make_weak(controller);
	const auto channels = &controller->session().localChannels();
	rows->clear();
	if (state->query != query) {
		state->query = query;
		state->page = 0;
	}
	auto matching = std::vector<LocalChannelEntry>();
	for (const auto &entry : channels->entries()) {
		if (entry.username.contains(query, Qt::CaseInsensitive)
			|| entry.title.contains(query, Qt::CaseInsensitive)) {
			matching.push_back(entry);
		}
	}
	const auto pages = std::max(
		1,
		(int(matching.size()) + kChannelsPerPage - 1) / kChannelsPerPage);
	state->page = std::clamp(state->page, 0, pages - 1);
	const auto first = state->page * kChannelsPerPage;
	const auto last = std::min(first + kChannelsPerPage, int(matching.size()));
	for (auto index = first; index != last; ++index) {
		const auto entry = matching[index];
		const auto label = entry.title.isEmpty()
			? '@' + entry.username
			: entry.title + u" (@"_q + entry.username + ')';
		const auto open = [=] {
			if (weak) {
				OpenChannel(box, weak.get(), state, entry);
			}
		};
		const auto remove = [=] {
			if (!weak) {
				return;
			}
			channels->cancel(state->requestId);
			state->requestId = 0;
			channels->remove(entry.username);
		};
		rows->add(object_ptr<LocalChannelRow>(rows, label, open, remove));
	}
	if (!rows->count()) {
		rows->add(
			object_ptr<Ui::FlatLabel>(
				rows,
				tr::ayu_LocalChannelsEmpty(),
				st::boxLabel),
			st::boxPadding);
	}
	if (pages > 1) {
		rows->add(object_ptr<Ui::FlatLabel>(
			rows,
			tr::ayu_LocalChannelsPage(
				lt_current,
				rpl::single(QString::number(state->page + 1)),
				lt_total,
				rpl::single(QString::number(pages))),
			st::boxLabel), st::boxPadding);
		const auto changePage = [=](int direction) {
			state->page += direction;
			crl::on_main(rows, [=] {
				if (weak) {
					FillRows(rows, box, weak.get(), state, query);
					box->scrollToWidget(rows);
				}
			});
		};
		const auto previous = rows->add(object_ptr<Ui::SettingsButton>(
			rows,
			tr::ayu_LocalChannelsPrevious(),
			st::settingsButtonNoIcon));
		previous->setEnabled(state->page > 0);
		previous->setClickedCallback([=] { changePage(-1); });
		const auto next = rows->add(object_ptr<Ui::SettingsButton>(
			rows,
			tr::ayu_LocalChannelsNext(),
			st::settingsButtonNoIcon));
		next->setEnabled(state->page + 1 < pages);
		next->setClickedCallback([=] { changePage(1); });
	}
}

} // namespace

void ShowLocalChannelsBox(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	const auto channels = &controller->session().localChannels();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_LocalChannelsTitle());
		box->setWidth(st::boxWideWidth);
		box->setMaxHeight(st::boxMaxListHeight);
		const auto state = box->lifetime().make_state<OperationState>();
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_LocalChannelsAbout(),
			st::boxLabel));
		const auto toggle = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				tr::ayu_LocalChannelsEnable(),
				st::settingsButtonNoIcon),
			style::margins());
		toggle->toggleOn(channels->enabledValue());
		toggle->toggledChanges() | rpl::on_next([=](bool enabled) {
			channels->setEnabled(enabled);
			state->requestId = 0;
		}, box->lifetime());
		Settings::AddButtonWithIcon(
			box->verticalLayout(),
			tr::ayu_LocalChannelsAdd(),
			st::settingsButton,
			{ &st::settingsIconAdd }
		)->setClickedCallback([=] {
			if (!weak) {
				return;
			} else if (!channels->enabled()) {
				ShowError(weak.get(), tr::ayu_LocalChannelsEnableFirst(tr::now));
			} else {
				AddChannelBox(weak.get());
			}
		});
		Settings::AddButtonWithIcon(
			box->verticalLayout(),
			tr::ayu_LocalChannelsImport(),
			st::settingsButtonNoIcon
		)->setClickedCallback([=] {
			if (weak) {
				ImportChannels(box, weak.get());
			}
		});
		Settings::AddButtonWithIcon(
			box->verticalLayout(),
			tr::ayu_LocalChannelsExport(),
			st::settingsButtonNoIcon
		)->setClickedCallback([=] {
			if (weak) {
				ExportChannels(box, weak.get());
			}
		});
		const auto search = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::ayu_LocalChannelsSearch()));
		const auto rows = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto refresh = [=] {
			if (state->refreshScheduled) {
				return;
			}
			state->refreshScheduled = true;
			crl::on_main(box, [=] {
				state->refreshScheduled = false;
				if (weak) {
					FillRows(
						rows,
						box,
						weak.get(),
						state,
						search->getLastText().trimmed());
				}
			});
		};
		channels->changes() | rpl::on_next(refresh, box->lifetime());
		search->changes() | rpl::on_next(refresh, box->lifetime());
		FillRows(rows, box, controller, state, QString());
		box->boxClosing() | rpl::on_next([=] {
			channels->cancel(state->requestId);
		}, box->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ToggleLocalChannel(
		not_null<Window::SessionController*> controller,
		not_null<ChannelData*> channel) {
	auto &channels = controller->session().localChannels();
	if (!channels.enabled()) {
		return;
	} else if (channels.contains(channel)) {
		const auto id = peerToChannel(channel->id).bare;
		const auto i = ranges::find_if(channels.entries(), [&](const auto &entry) {
			return entry.id == id
				|| entry.username == LocalChannelUsername(channel->username());
		});
		if (i != end(channels.entries())) {
			const auto username = i->username;
			channels.remove(username);
			controller->showToast(tr::ayu_LocalChannelsRemoved(tr::now));
		}
	} else {
		const auto error = channels.add(channel);
		if (error.isEmpty()) {
			controller->showToast(tr::ayu_LocalChannelsSaved(tr::now));
		} else {
			ShowError(controller, error);
		}
	}
}

} // namespace Ayu
