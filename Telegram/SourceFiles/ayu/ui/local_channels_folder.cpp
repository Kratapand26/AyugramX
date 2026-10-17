#include "ayu/ui/local_channels_folder.h"

#include "ayu/features/local_channels/local_channels.h"
#include "ayu/ui/boxes/local_channels_box.h"
#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QResizeEvent>

#include <algorithm>
#include <utility>

#include "styles/style_ayu_styles.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Ayu {
namespace {

constexpr auto kChannelsPerPage = 50;

} // namespace

struct LocalChannelsFolder::State {
	LocalChannels *channels = nullptr;
	Ui::RpWidget *header = nullptr;
	Ui::FlatLabel *title = nullptr;
	Ui::IconButton *manage = nullptr;
	Ui::IconButton *info = nullptr;
	Ui::ScrollArea *scroll = nullptr;
	Ui::VerticalLayout *rows = nullptr;
	LocalChannelOpener opener;
	std::vector<QPointer<Ui::SettingsButton>> buttons;
	std::vector<Fn<void()>> actions;
	QString query;
	int page = 0;
	int selected = -1;
	bool scheduled = false;
};

LocalChannelsFolder::LocalChannelsFolder(
		QWidget *parent,
		not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _state(std::make_unique<State>()) {
	setFocusPolicy(Qt::StrongFocus);
	const auto state = _state.get();
	state->channels = &controller->session().localChannels();
	state->opener = MakeLocalChannelOpener(this, controller, [] {});
	state->header = Ui::CreateChild<Ui::RpWidget>(this);
	state->title = Ui::CreateChild<Ui::FlatLabel>(
		state->header,
		tr::ayu_LocalChannelsFolder(),
		st::boxLabel);
	state->manage = Ui::CreateChild<Ui::IconButton>(
		state->header,
		st::localChannelsFolderManage);
	state->info = Ui::CreateChild<Ui::IconButton>(
		state->header,
		st::localChannelsFolderInfo);
	state->manage->setAccessibleName(tr::ayu_LocalChannelsManage(tr::now));
	state->info->setAccessibleName(tr::ayu_LocalChannelsInfo(tr::now));
	state->manage->setFocusPolicy(Qt::StrongFocus);
	state->info->setFocusPolicy(Qt::StrongFocus);
	Ui::InstallTooltip(state->manage, [] {
		return tr::ayu_LocalChannelsManage(tr::now);
	});
	Ui::InstallTooltip(state->info, [] {
		return tr::ayu_LocalChannelsAbout(tr::now);
	});
	const auto weak = base::make_weak(controller);
	state->manage->setClickedCallback([=] {
		if (weak) {
			ShowLocalChannelsBox(weak.get());
		}
	});
	state->info->setClickedCallback([=] {
		if (weak) {
			weak->show(Ui::MakeInformBox(tr::ayu_LocalChannelsAbout(tr::now)));
		}
	});
	state->scroll = Ui::CreateChild<Ui::ScrollArea>(this);
	state->rows = state->scroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(state->scroll));
	state->scroll->widthValue() | rpl::on_next([=](int width) {
		state->rows->resizeToWidth(width);
	}, lifetime());
	state->channels->changes() | rpl::on_next([=] {
		scheduleRefresh();
	}, lifetime());
	paintOn([=](QPainter &p) {
		p.fillRect(rect(), st::windowBg);
	});
	refresh();
}

LocalChannelsFolder::~LocalChannelsFolder() {
	_state->opener.cancel();
}

void LocalChannelsFolder::setQuery(QString query) {
	query = query.trimmed();
	if (_state->query != query) {
		_state->query = std::move(query);
		_state->page = 0;
		_state->scroll->scrollToY(0);
		scheduleRefresh();
	}
}

void LocalChannelsFolder::scheduleRefresh() {
	if (std::exchange(_state->scheduled, true)) {
		return;
	}
	crl::on_main(this, [=] {
		_state->scheduled = false;
		refresh();
	});
}

void LocalChannelsFolder::refresh() {
	const auto state = _state.get();
	state->buttons.clear();
	state->actions.clear();
	state->selected = -1;
	state->rows->clear();
	auto matching = std::vector<LocalChannelEntry>();
	for (const auto &entry : state->channels->entries()) {
		if (entry.username.contains(state->query, Qt::CaseInsensitive)
			|| entry.title.contains(state->query, Qt::CaseInsensitive)) {
			matching.push_back(entry);
		}
	}
	const auto pages = std::max(
		1,
		(int(matching.size()) + kChannelsPerPage - 1) / kChannelsPerPage);
	state->page = std::clamp(state->page, 0, pages - 1);
	const auto first = state->page * kChannelsPerPage;
	const auto last = std::min(first + kChannelsPerPage, int(matching.size()));
	for (auto i = first; i != last; ++i) {
		const auto entry = matching[i];
		const auto label = entry.title.isEmpty()
			? '@' + entry.username
			: entry.title + u" (@"_q + entry.username + ')';
		const auto button = state->rows->add(object_ptr<Ui::SettingsButton>(
			state->rows,
			rpl::single(label),
			st::settingsButtonNoIcon));
		const auto action = [=] { state->opener.open(entry); };
		button->setClickedCallback(action);
		button->setFocusPolicy(Qt::StrongFocus);
		const auto index = int(state->buttons.size());
		base::install_event_filter(button, [=](not_null<QEvent*> event) {
			if (event->type() != QEvent::KeyPress) {
				return base::EventFilterResult::Continue;
			}
			const auto key = static_cast<QKeyEvent*>(event.get())->key();
			if (key != Qt::Key_Up && key != Qt::Key_Down) {
				return base::EventFilterResult::Continue;
			}
			if (state->selected >= 0) {
				state->buttons[state->selected]->setForceRippled(false);
			}
			state->selected = index;
			moveSelection(key == Qt::Key_Down ? 1 : -1);
			return base::EventFilterResult::Cancel;
		});
		state->buttons.push_back(button);
		state->actions.push_back(action);
	}
	if (matching.empty()) {
		state->rows->add(object_ptr<Ui::FlatLabel>(
			state->rows,
			tr::ayu_LocalChannelsEmpty(),
			st::boxLabel), st::boxPadding);
	}
	if (pages <= 1) {
		return;
	}
	state->rows->add(object_ptr<Ui::FlatLabel>(
		state->rows,
		tr::ayu_LocalChannelsPage(
			lt_current,
			rpl::single(QString::number(state->page + 1)),
			lt_total,
			rpl::single(QString::number(pages))),
		st::boxLabel), st::boxPadding);
	const auto addPage = [&](rpl::producer<QString> text, int delta) {
		const auto button = state->rows->add(object_ptr<Ui::SettingsButton>(
			state->rows,
			std::move(text),
			st::settingsButtonNoIcon));
		button->setEnabled(state->page + delta >= 0
			&& state->page + delta < pages);
		button->setClickedCallback([=] {
			state->page += delta;
			state->scroll->scrollToY(0);
			scheduleRefresh();
		});
	};
	addPage(tr::ayu_LocalChannelsPrevious(), -1);
	addPage(tr::ayu_LocalChannelsNext(), 1);
}

void LocalChannelsFolder::moveSelection(int delta) {
	const auto state = _state.get();
	if (state->buttons.empty()) {
		return;
	}
	if (state->selected >= 0) {
		state->buttons[state->selected]->setForceRippled(false);
	}
	state->selected = (state->selected < 0)
		? ((delta > 0) ? 0 : int(state->buttons.size()) - 1)
		: std::clamp(state->selected + delta, 0, int(state->buttons.size()) - 1);
	const auto button = state->buttons[state->selected].data();
	button->setForceRippled(true);
	button->setFocus();
	state->scroll->scrollToY(button->y(), button->y() + button->height());
}

void LocalChannelsFolder::activateSelected() {
	if (!_state->actions.empty()) {
		const auto index = std::max(0, _state->selected);
		const auto action = _state->actions[index];
		action();
	}
}

QAccessible::Role LocalChannelsFolder::accessibilityRole() {
	return QAccessible::List;
}

QString LocalChannelsFolder::accessibilityName() {
	return tr::ayu_LocalChannelsFolder(tr::now);
}

void LocalChannelsFolder::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);
	const auto state = _state.get();
	const auto headerHeight = st::localChannelsFolderHeaderHeight;
	const auto padding = st::localChannelsFolderPadding;
	const auto showInfo = (width()
		>= 2 * padding + state->info->width() + state->manage->width());
	state->info->setVisible(showInfo);
	const auto infoWidth = showInfo ? state->info->width() : 0;
	state->header->setGeometry(0, 0, width(), headerHeight);
	state->info->moveToRight(
		padding,
		(headerHeight - state->info->height()) / 2,
		width());
	state->manage->moveToRight(
		padding + infoWidth,
		(headerHeight - state->manage->height()) / 2,
		width());
	const auto titleWidth = std::max(
		0,
		width() - 2 * padding - infoWidth - state->manage->width());
	state->title->setVisible(titleWidth >= state->title->textMaxWidth());
	state->title->resizeToWidth(titleWidth);
	state->title->moveToLeft(
		padding,
		(headerHeight - state->title->height()) / 2,
		width());
	state->scroll->setGeometry(
		0,
		headerHeight,
		width(),
		std::max(0, height() - headerHeight));
}

} // namespace Ayu
