#include "ayu/ui/local_channels_folder.h"

#include "ayu/features/local_channels/local_channels.h"
#include "ayu/ui/boxes/local_channels_box.h"
#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "dialogs/ui/dialogs_layout.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/empty_userpic.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"

#include <QtGui/QFocusEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QResizeEvent>

#include <algorithm>
#include <utility>

#include "styles/style_ayu_styles.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"

namespace Ayu {
namespace {

constexpr auto kChannelsPerPage = 50;

class LocalChannelChatRow final : public Ui::RippleButton {
public:
	LocalChannelChatRow(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		LocalChannelEntry entry);

	QString accessibilityName() override;
	Ui::AccessibilityState accessibilityState() const override;

private:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *e) override;
	void focusInEvent(QFocusEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	[[nodiscard]] bool active() const;

	const base::weak_ptr<Window::SessionController> _controller;
	const LocalChannelEntry _entry;
	Ui::EmptyUserpic _emptyUserpic;
	History *_history = nullptr;
	std::unique_ptr<Dialogs::Row> _row;

};

LocalChannelChatRow::LocalChannelChatRow(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		LocalChannelEntry entry)
: RippleButton(parent, st::dialogsRipple)
, _controller(base::make_weak(controller))
, _entry(std::move(entry))
, _emptyUserpic(
	Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(
		_entry.id ? _entry.id : qHash(_entry.username))),
	_entry.title.isEmpty() ? _entry.username : _entry.title) {
	setIsListItem(true);
	setFocusPolicy(Qt::StrongFocus);
	setPointerCursor(true);
	const auto channel = controller->session().localChannels().cachedChannel(_entry);
	if (channel) {
		_history = controller->session().data().history(channel).get();
		_row = std::make_unique<Dialogs::Row>(Dialogs::Key(_history), 0, 0);
		_row->recountHeight(1., 0);
		controller->session().changes().entryUpdates(
			_history,
			Data::EntryUpdate::Flag::Repaint | Data::EntryUpdate::Flag::Height
		) | rpl::on_next([=] { update(); }, lifetime());
		controller->session().changes().peerUpdates(
			channel,
			Data::PeerUpdate::Flag::Name
				| Data::PeerUpdate::Flag::Photo
				| Data::PeerUpdate::Flag::Notifications
				| Data::PeerUpdate::Flag::VerifyInfo
		) | rpl::on_next([=] {
			accessibilityNameChanged();
			update();
		}, lifetime());
	}
	controller->activeChatEntryValue() | rpl::on_next([=] {
		update();
	}, lifetime());
	controller->session().downloaderTaskFinished() | rpl::on_next([=] {
		update();
	}, lifetime());
}

QString LocalChannelChatRow::accessibilityName() {
	const auto title = _history ? _history->chatListName() : _entry.title;
	return title.isEmpty()
		? '@' + _entry.username
		: title + u" (@"_q + _entry.username + ')';
}

Ui::AccessibilityState LocalChannelChatRow::accessibilityState() const {
	auto result = RippleButton::accessibilityState();
	result.selectable = true;
	result.selected = active() || hasFocus();
	return result;
}

bool LocalChannelChatRow::active() const {
	return _history
		&& _controller
		&& (_controller->activeChatEntryCurrent().key.history() == _history);
}

int LocalChannelChatRow::resizeGetHeight(int width) {
	return st::defaultDialogRow.height;
}

void LocalChannelChatRow::focusInEvent(QFocusEvent *e) {
	RippleButton::focusInEvent(e);
	update();
}

void LocalChannelChatRow::focusOutEvent(QFocusEvent *e) {
	RippleButton::focusOutEvent(e);
	update();
}

void LocalChannelChatRow::paintEvent(QPaintEvent *e) {
	const auto selected = isOver() || hasFocus() || forceRippled();
	const auto narrow = (width() < st::columnMinimalWidthLeft / 2);
	auto p = Painter(this);
	if (_row && _controller) {
		Dialogs::Ui::RowPainter::Paint(p, _row.get(), nullptr, {
			.st = &st::defaultDialogRow,
			.currentBg = st::dialogsBg,
			.now = crl::now(),
			.width = width(),
			.active = active(),
			.selected = selected,
			.paused = _controller->isGifPausedAtLeastFor(
				Window::GifPauseReason::Any),
			.narrow = narrow,
			.preloadData = false,
		});
	} else {
		p.fillRect(rect(), selected ? st::dialogsBgOver : st::dialogsBg);
		const auto &row = st::defaultDialogRow;
		_emptyUserpic.paintCircle(
			p,
			row.padding.left(),
			row.padding.top(),
			width(),
			row.photoSize);
		if (!narrow) {
			const auto available = std::max(
				0,
				width() - row.nameLeft - row.padding.right());
			const auto title = _entry.title.isEmpty()
				? _entry.username
				: _entry.title;
			p.setFont(st::semiboldFont);
			p.setPen(selected ? st::dialogsNameFgOver : st::dialogsNameFg);
			p.drawTextLeft(
				row.nameLeft,
				row.nameTop,
				width(),
				st::semiboldFont->elided(title, available));
			p.setFont(st::dialogsTextFont);
			p.setPen(selected ? st::dialogsTextFgOver : st::dialogsTextFg);
			p.drawTextLeft(
				row.textLeft,
				row.textTop,
				width(),
				st::dialogsTextFont->elided('@' + _entry.username, available));
		}
	}
	const auto &ripple = active()
		? st::dialogsRippleBgActive
		: st::dialogsRippleBg;
	paintRipple(p, 0, 0, &ripple->c);
}

} // namespace

struct LocalChannelsFolder::State {
	base::weak_ptr<Window::SessionController> controller;
	LocalChannels *channels = nullptr;
	Ui::RpWidget *header = nullptr;
	Ui::FlatLabel *title = nullptr;
	Ui::IconButton *manage = nullptr;
	Ui::IconButton *info = nullptr;
	Ui::ScrollArea *scroll = nullptr;
	Ui::VerticalLayout *rows = nullptr;
	LocalChannelOpener opener;
	std::vector<QPointer<LocalChannelChatRow>> buttons;
	std::vector<Fn<void()>> actions;
	QString query;
	int page = 0;
	int selected = -1;
	int titleLeft = 0;
	bool scheduled = false;
};

LocalChannelsFolder::LocalChannelsFolder(
		QWidget *parent,
		not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _state(std::make_unique<State>()) {
	setFocusPolicy(Qt::StrongFocus);
	const auto state = _state.get();
	state->controller = base::make_weak(controller);
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

void LocalChannelsFolder::setTitleLeft(int left) {
	if (_state->titleLeft != left) {
		_state->titleLeft = left;
		updateHeaderGeometry();
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
	if (!state->controller) {
		return;
	}
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
		const auto button = state->rows->add(object_ptr<LocalChannelChatRow>(
			state->rows,
			state->controller.get(),
			entry));
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
	updateHeaderGeometry();
	const auto headerHeight = st::localChannelsFolderHeaderHeight;
	_state->scroll->setGeometry(
		0,
		headerHeight,
		width(),
		std::max(0, height() - headerHeight));
}

void LocalChannelsFolder::updateHeaderGeometry() {
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
		width() - state->titleLeft - padding
			- infoWidth - state->manage->width());
	state->title->setVisible(titleWidth >= state->title->textMaxWidth());
	state->title->resizeToWidth(titleWidth);
	state->title->moveToLeft(
		state->titleLeft,
		(headerHeight - state->title->height()) / 2,
		width());
}

} // namespace Ayu
