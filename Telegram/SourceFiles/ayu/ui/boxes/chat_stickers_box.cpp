#include "ayu/ui/boxes/chat_stickers_box.h"

#include "api/api_toggling_media.h"
#include "ayu/features/chat_stickers/chat_stickers.h"
#include "base/weak_ptr.h"
#include "boxes/sticker_set_box.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>

#include <algorithm>
#include <utility>

#include "styles/style_ayu_styles.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Ayu {
namespace {

constexpr auto kGridPref = "ayu_chat_stickers_grid"_cs;
constexpr auto kSortPref = "ayu_chat_stickers_sort"_cs;
constexpr auto kPacksPref = "ayu_chat_stickers_packs"_cs;
constexpr auto kPreviewCacheLimit = 128;

enum class StickerOrder {
	Newest,
	Usage,
	Pack,
};

struct DisplaySticker {
	ChatStickerEntry entry;
	QString title;
	QString uses;
	QString packKey;
};

[[nodiscard]] QString OrderName(StickerOrder order) {
	switch (order) {
	case StickerOrder::Newest: return tr::ayu_ChatStickersNewest(tr::now);
	case StickerOrder::Usage: return tr::ayu_ChatStickersUsage(tr::now);
	case StickerOrder::Pack: return tr::ayu_ChatStickersPack(tr::now);
	}
	Unexpected("Invalid chat sticker order.");
}

[[nodiscard]] QString PackTitle(not_null<DocumentData*> document) {
	const auto sticker = document->sticker();
	if (!sticker) {
		return tr::ayu_ChatStickersSticker(tr::now);
	}
	const auto &sets = document->owner().stickers().sets();
	const auto &set = sticker->set;
	const auto i = sets.find(set.id);
	return (i != end(sets))
		? i->second->title
		: !set.shortName.isEmpty()
		? set.shortName
		: tr::ayu_ChatStickersSticker(tr::now);
}

class StickerToolbar final : public Ui::RpWidget {
public:
	explicit StickerToolbar(QWidget *parent)
	: RpWidget(parent)
	, view(Ui::CreateChild<Ui::LinkButton>(this, QString()))
	, sort(Ui::CreateChild<Ui::LinkButton>(this, QString()))
	, packs(Ui::CreateChild<Ui::LinkButton>(this, QString())) {
	}

	const not_null<Ui::LinkButton*> view;
	const not_null<Ui::LinkButton*> sort;
	const not_null<Ui::LinkButton*> packs;

private:
	int resizeGetHeight(int width) override {
		auto left = 0;
		auto top = st::chatStickersToolbarPadding;
		auto rowHeight = 0;
		for (const auto button : { view, sort, packs }) {
			if (left && left + button->width() > width) {
				left = 0;
				top += rowHeight + st::chatStickersToolbarGap;
				rowHeight = 0;
			}
			button->moveToLeft(left, top, width);
			left += button->width() + st::chatStickersToolbarGap;
			rowHeight = std::max(rowHeight, button->height());
		}
		return top + rowHeight + st::chatStickersToolbarPadding;
	}

};

class StickerResults final : public Ui::RpWidget {
public:
	StickerResults(
		QWidget *parent,
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<ChatStickersCache> cache);

	void refresh(StickerOrder order, bool grid, bool packs);
	[[nodiscard]] rpl::producer<QRect> scrollRequests() const;
	QAccessible::Role accessibilityRole() override {
		return QAccessible::Role::List;
	}
	Qt::FocusPolicy accessibilityFocusPolicy() override {
		return Qt::StrongFocus;
	}
	QString accessibilityName() override;

private:
	int resizeGetHeight(int width) override;
	void visibleTopBottomUpdated(int top, int bottom) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	[[nodiscard]] QRect cellRect(int index) const;
	[[nodiscard]] int indexAt(QPoint point) const;
	void select(int index);
	void openPack(int index);
	void showActions(int index, QPoint position);
	void paintSticker(Painter &p, int index);

	const not_null<Ui::GenericBox*> _box;
	const base::weak_ptr<Window::SessionController> _controller;
	const not_null<Main::Session*> _session;
	const std::shared_ptr<ChatStickersCache> _cache;
	std::vector<DisplaySticker> _items;
	std::map<DocumentId, std::shared_ptr<Data::DocumentMedia>> _media;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::event_stream<QRect> _scrollRequests;
	StickerOrder _order = StickerOrder::Newest;
	bool _grid = true;
	bool _packs = false;
	int _revision = -1;
	int _columns = 1;
	int _selected = -1;
	int _pressed = -1;
	int _visibleTop = 0;
	int _visibleBottom = -1;

};

StickerResults::StickerResults(
		QWidget *parent,
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<ChatStickersCache> cache)
: RpWidget(parent)
, _box(box)
, _controller(base::make_weak(controller))
, _session(&controller->session())
, _cache(std::move(cache)) {
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	_session->downloaderTaskFinished() | rpl::on_next([=] {
		update();
	}, lifetime());
}

void StickerResults::refresh(StickerOrder order, bool grid, bool packs) {
	if (_revision != _cache->revision || _order != order || _packs != packs) {
		_revision = _cache->revision;
		_order = order;
		_packs = packs;
		_items.clear();
		auto packRows = std::map<QString, int>();
		for (const auto &entry : _cache->data.entries()) {
			if (!packs && order != StickerOrder::Pack) {
				_items.push_back({ entry });
				continue;
			}
			const auto document = _session->data().document(entry.documentId);
			const auto sticker = document->sticker();
			if (!sticker) {
				continue;
			}
			const auto &set = sticker->set;
			if (packs && set.empty()) {
				continue;
			}
			const auto key = set.id
				? u"id:"_q + QString::number(set.id)
				: u"name:"_q + set.shortName.toLower();
			if (packs) {
				const auto i = packRows.find(key);
				if (i != end(packRows)) {
					auto &row = _items[i->second].entry;
					const auto count = row.count + entry.count;
					if (entry.messageId > row.messageId) {
						row = entry;
					}
					row.count = count;
					continue;
				}
				packRows.emplace(key, int(_items.size()));
			}
			_items.push_back({ entry, PackTitle(document), {}, key });
		}
		ranges::sort(_items, [=](const auto &a, const auto &b) {
			if (order == StickerOrder::Usage && a.entry.count != b.entry.count) {
				return a.entry.count > b.entry.count;
			} else if (order == StickerOrder::Pack) {
				const auto compared = a.title.compare(b.title, Qt::CaseInsensitive);
				if (compared) {
					return compared < 0;
				} else if (a.packKey != b.packKey) {
					return a.packKey < b.packKey;
				}
			}
			return (a.entry.date != b.entry.date)
				? a.entry.date > b.entry.date
				: a.entry.messageId > b.entry.messageId;
		});
		_selected = _pressed = -1;
	}
	_grid = grid;
	resizeToWidth(width());
	update();
}

rpl::producer<QRect> StickerResults::scrollRequests() const {
	return _scrollRequests.events();
}

QString StickerResults::accessibilityName() {
	if (_selected < 0) {
		return tr::ayu_ChatStickersTitle(tr::now);
	}
	const auto &entry = _items[_selected].entry;
	return PackTitle(_session->data().document(entry.documentId))
		+ u", "_q
		+ tr::ayu_ChatStickersUses(tr::now, lt_count, entry.count);
}

int StickerResults::resizeGetHeight(int width) {
	_columns = _grid ? std::max(1, width / st::chatStickersCellWidth) : 1;
	const auto rows = (int(_items.size()) + _columns - 1) / _columns;
	return std::max(
		st::chatStickersEmptyHeight,
		rows * (_grid ? st::chatStickersCellHeight : st::chatStickersListHeight));
}

void StickerResults::visibleTopBottomUpdated(int top, int bottom) {
	_visibleTop = top;
	_visibleBottom = bottom;
}

QRect StickerResults::cellRect(int index) const {
	const auto left = (index % _columns) * width() / _columns;
	const auto right = ((index % _columns) + 1) * width() / _columns;
	const auto height = _grid
		? st::chatStickersCellHeight
		: st::chatStickersListHeight;
	return QRect(
		style::RightToLeft() ? width() - right : left,
		(index / _columns) * height,
		right - left,
		height);
}

int StickerResults::indexAt(QPoint point) const {
	if (point.x() < 0 || point.x() >= width() || point.y() < 0) {
		return -1;
	}
	const auto x = style::RightToLeft() ? width() - point.x() - 1 : point.x();
	const auto row = point.y()
		/ (_grid ? st::chatStickersCellHeight : st::chatStickersListHeight);
	const auto column = ((x + 1) * _columns - 1) / std::max(1, width());
	const auto index = row * _columns + column;
	return (index < int(_items.size())) ? index : -1;
}

void StickerResults::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_items.empty()) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			rect(),
			Qt::AlignCenter | Qt::TextWordWrap,
			tr::ayu_ChatStickersEmpty(tr::now));
		return;
	}
	const auto height = _grid
		? st::chatStickersCellHeight
		: st::chatStickersListHeight;
	const auto top = std::max(e->rect().top(), _visibleTop);
	const auto bottom = (_visibleBottom < 0)
		? e->rect().bottom()
		: std::min(e->rect().bottom(), _visibleBottom - 1);
	if (bottom < top) {
		return;
	}
	const auto first = std::max(0, top / height) * _columns;
	const auto last = std::min(
		int(_items.size()),
		(bottom / height + 1) * _columns);
	for (auto i = first; i < last; ++i) {
		paintSticker(p, i);
	}
	if (_media.size() > kPreviewCacheLimit) {
		auto visible = base::flat_set<DocumentId>();
		for (auto i = first; i < last; ++i) {
			visible.emplace(_items[i].entry.documentId);
		}
		for (auto i = begin(_media); i != end(_media) && _media.size() > kPreviewCacheLimit;) {
			if (!visible.contains(i->first)) {
				i = _media.erase(i);
			} else {
				++i;
			}
		}
	}
}

void StickerResults::paintSticker(Painter &p, int index) {
	auto &item = _items[index];
	const auto cell = cellRect(index);
	if (index == _selected) {
		p.fillRect(cell, st::windowBgOver);
	}
	const auto document = _session->data().document(item.entry.documentId);
	if (!document->sticker()) {
		return;
	}
	if (item.uses.isEmpty()) {
		item.uses = tr::ayu_ChatStickersUses(
			tr::now,
			lt_count,
			item.entry.count);
	}
	if (!_grid && item.title.isEmpty()) {
		item.title = PackTitle(document);
	}
	auto &media = _media[item.entry.documentId];
	if (!media) {
		media = document->createMediaView();
	}
	media->thumbnailWanted(FullMsgId(_cache->peerId, item.entry.messageId));
	if (document->sticker()->isStatic()) {
		media->checkStickerSmall();
	}
	const auto padding = st::chatStickersCellPadding;
	const auto preview = std::max(0, std::min(
		_grid ? st::chatStickersPreviewSize : st::chatStickersListPreviewSize,
		cell.width() - 2 * padding));
	const auto previewRect = _grid
		? QRect(cell.x() + (cell.width() - preview) / 2, cell.y() + padding, preview, preview)
		: QRect(
			style::RightToLeft() ? cell.right() - padding - preview + 1 : cell.x() + padding,
			cell.y() + (cell.height() - preview) / 2,
			preview,
			preview);
	auto image = media->getStickerSmall();
	if (!image) {
		image = media->thumbnailInline();
	}
	if (image && !image->isNull()) {
		const auto size = image->size().scaled(previewRect.size(), Qt::KeepAspectRatio);
		const auto position = previewRect.center()
			- QPoint(size.width() / 2, size.height() / 2);
		p.drawPixmap(position, image->pixSingle(size));
	} else {
		p.setFont(st::chatStickersPlaceholderFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(previewRect, Qt::AlignCenter, document->sticker()->alt);
	}
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	if (_grid) {
		p.drawText(
			QRect(
				cell.x() + padding,
				previewRect.bottom() + st::chatStickersTextGap,
				std::max(0, cell.width() - 2 * padding),
				st::normalFont->height),
			Qt::AlignCenter,
			st::normalFont->elided(item.uses, std::max(0, cell.width() - 2 * padding)));
	} else {
		const auto left = padding + preview + padding;
		const auto available = std::max(0, cell.width() - left - padding);
		const auto top = cell.y()
			+ (cell.height() - 2 * st::normalFont->height - st::chatStickersTextGap) / 2;
		p.setPen(st::windowFg);
		p.drawTextLeft(left, top, width(), st::normalFont->elided(item.title, available));
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(
			left,
			top + st::normalFont->height + st::chatStickersTextGap,
			width(),
			st::normalFont->elided(item.uses, available));
	}
}

void StickerResults::select(int index) {
	if (_selected == index) {
		return;
	}
	if (_selected >= 0) {
		update(cellRect(_selected));
	}
	_selected = index;
	if (_selected >= 0) {
		update(cellRect(_selected));
	}
	setCursor(index >= 0 ? style::cur_pointer : style::cur_default);
	accessibilityNameChanged();
}

void StickerResults::mouseMoveEvent(QMouseEvent *e) {
	select(indexAt(e->pos()));
}

void StickerResults::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		setFocus(Qt::MouseFocusReason);
		_pressed = indexAt(e->pos());
		select(_pressed);
	}
}

void StickerResults::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		const auto pressed = std::exchange(_pressed, -1);
		if (pressed >= 0 && pressed == indexAt(e->pos())) {
			openPack(pressed);
		}
	}
}

void StickerResults::leaveEventHook(QEvent *e) {
	select(-1);
	RpWidget::leaveEventHook(e);
}

void StickerResults::contextMenuEvent(QContextMenuEvent *e) {
	const auto index = (e->reason() == QContextMenuEvent::Keyboard)
		? _selected
		: indexAt(e->pos());
	if (index >= 0) {
		select(index);
		showActions(index, e->globalPos());
	}
}

void StickerResults::keyPressEvent(QKeyEvent *e) {
	if (_items.empty()) {
		return RpWidget::keyPressEvent(e);
	}
	if (e->key() == Qt::Key_F10 && (e->modifiers() & Qt::ShiftModifier)) {
		const auto current = std::max(0, _selected);
		showActions(current, mapToGlobal(cellRect(current).center()));
		return;
	}
	const auto current = std::max(0, _selected);
	auto next = current;
	switch (e->key()) {
	case Qt::Key_Left: next += style::RightToLeft() ? 1 : -1; break;
	case Qt::Key_Right: next += style::RightToLeft() ? -1 : 1; break;
	case Qt::Key_Up: next -= _columns; break;
	case Qt::Key_Down: next += _columns; break;
	case Qt::Key_Home: next = 0; break;
	case Qt::Key_End: next = int(_items.size()) - 1; break;
	case Qt::Key_Enter:
	case Qt::Key_Return: openPack(current); return;
	case Qt::Key_Menu: showActions(current, mapToGlobal(cellRect(current).center())); return;
	default: return RpWidget::keyPressEvent(e);
	}
	if (_selected < 0 && e->key() != Qt::Key_End) {
		next = 0;
	}
	select(std::clamp(next, 0, int(_items.size()) - 1));
	_scrollRequests.fire_copy(cellRect(_selected));
	e->accept();
}

void StickerResults::openPack(int index) {
	if (!_controller) {
		return;
	}
	const auto document = _session->data().document(_items[index].entry.documentId);
	if (document->sticker() && !document->sticker()->set.empty()) {
		StickerSetBox::Show(_controller->uiShow(), document);
	} else {
		showActions(index, mapToGlobal(cellRect(index).center()));
	}
}

void StickerResults::showActions(int index, QPoint position) {
	if (!_controller) {
		return;
	}
	const auto entry = _items[index].entry;
	const auto document = _session->data().document(entry.documentId);
	const auto origin = FullMsgId(_cache->peerId, entry.messageId);
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	if (document->sticker() && !document->sticker()->set.empty()) {
		_menu->addAction(tr::ayu_ChatStickersOpenPack(tr::now), crl::guard(this, [=] {
			if (_controller) {
				StickerSetBox::Show(_controller->uiShow(), document);
			}
		}), &st::menuIconStickers);
	}
	const auto faved = _session->data().stickers().isFaved(document);
	_menu->addAction(
		faved ? tr::ayu_ChatStickersUnfave(tr::now) : tr::ayu_ChatStickersFave(tr::now),
		crl::guard(this, [=] {
			if (_controller) {
				Api::ToggleFavedSticker(_controller->uiShow(), document, origin);
			}
		}),
		faved ? &st::menuIconUnfave : &st::menuIconFave);
	_menu->addAction(tr::ayu_ChatStickersSave(tr::now), crl::guard(this, [=] {
		DocumentSaveClickHandler::Save(
			origin,
			document,
			DocumentSaveClickHandler::Mode::ToNewFile);
	}), &st::menuIconDownload);
	_menu->addAction(tr::ayu_ChatStickersJump(tr::now), crl::guard(this, [=] {
		const auto controller = _controller;
		const auto topicRootId = _cache->topicRootId;
		const auto peerId = _cache->peerId;
		_box->closeBox();
		if (!controller) {
			return;
		}
		if (topicRootId) {
			controller->showRepliesForMessage(
				controller->session().data().history(peerId),
				topicRootId,
				entry.messageId);
		} else {
			controller->showPeerHistory(
				peerId,
				Window::SectionShow::Way::Forward,
				entry.messageId);
		}
	}), &st::menuIconShowInChat);
	_menu->popup(position);
}

struct BrowserState {
	mtpRequestId requestId = 0;
	StickerOrder order = StickerOrder::Newest;
	bool grid = true;
	bool packs = false;
	bool scheduled = false;
	base::unique_qptr<Ui::PopupMenu> menu;
};

} // namespace

void ShowChatStickersBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	const auto stickers = &controller->session().chatStickers();
	if (!stickers->enabled()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto cache = stickers->cache(peer, topicRootId);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_ChatStickersTitle());
		box->setWidth(st::chatStickersBoxWidth);
		box->setMaxHeight(st::boxMaxListHeight);
		const auto state = box->lifetime().make_state<BrowserState>();
		auto &local = controller->session().local();
		const auto viewPref = local.readPref<QString>(kGridPref);
		state->grid = (viewPref != u"list"_q);
		state->packs = local.readPref<bool>(kPacksPref);
		const auto orderPref = local.readPref<QString>(kSortPref);
		state->order = (orderPref == u"usage"_q)
			? StickerOrder::Usage
			: (orderPref == u"pack"_q) ? StickerOrder::Pack : StickerOrder::Newest;
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_ChatStickersAbout(),
			st::boxLabel));
		const auto toolbar = box->addRow(object_ptr<StickerToolbar>(box));
		const auto results = box->addRow(object_ptr<StickerResults>(
			box,
			box,
			controller,
			cache), style::margins());
		const auto bottom = box->setPinnedToBottomContent(
			object_ptr<Ui::VerticalLayout>(box));
		const auto status = bottom->add(object_ptr<Ui::FlatLabel>(
			bottom,
			QString(),
			st::boxLabel), st::boxPadding);
		const auto loadText = box->lifetime().make_state<rpl::variable<QString>>();
		const auto load = bottom->add(object_ptr<Ui::SettingsButton>(
			bottom,
			loadText->value(),
			st::settingsButtonNoIcon));
		const auto update = [=] {
			const auto loading = stickers->loading(cache);
			*loadText = loading
				? tr::ayu_ChatStickersLoading(tr::now)
				: tr::ayu_ChatStickersLoadMore(tr::now);
			load->setEnabled(!loading && !cache->finished && !cache->data.full());
			status->setText(cache->data.full()
				? tr::ayu_ChatStickersFull(tr::now)
				: cache->finished
				? tr::ayu_ChatStickersFinished(tr::now)
				: tr::ayu_ChatStickersScanned(
					tr::now,
					lt_count,
					cache->scanned));
			toolbar->view->setText(state->grid
				? tr::ayu_ChatStickersGrid(tr::now)
				: tr::ayu_ChatStickersList(tr::now));
			toolbar->sort->setText(OrderName(state->order));
			toolbar->packs->setText(state->packs
				? tr::ayu_ChatStickersPacksOnly(tr::now)
				: tr::ayu_ChatStickersAll(tr::now));
			toolbar->resizeToWidth(toolbar->width());
			results->refresh(state->order, state->grid, state->packs);
		};
		const auto loadMore = [=] {
			if (stickers->loading(cache)) {
				return;
			}
			state->requestId = stickers->loadMore(cache, crl::guard(box, [=](QString error) {
				state->requestId = 0;
				if (weak && !error.isEmpty()) {
					weak->showToast(error);
				}
			}));
			update();
		};
		load->setClickedCallback(loadMore);
		toolbar->view->setClickedCallback([=] {
			state->grid = !state->grid;
			controller->session().local().writePref<QString>(
				kGridPref,
				state->grid ? u"grid"_q : u"list"_q);
			update();
			box->scrollToY(0);
		});
		toolbar->packs->setClickedCallback([=] {
			state->packs = !state->packs;
			controller->session().local().writePref<bool>(kPacksPref, state->packs);
			update();
			box->scrollToY(0);
		});
		toolbar->sort->setClickedCallback([=] {
			state->menu = base::make_unique_q<Ui::PopupMenu>(toolbar);
			for (const auto order : {
				StickerOrder::Newest,
				StickerOrder::Usage,
				StickerOrder::Pack,
			}) {
				state->menu->addAction(OrderName(order), crl::guard(box, [=] {
					state->order = order;
					controller->session().local().writePref<QString>(
						kSortPref,
						order == StickerOrder::Usage ? u"usage"_q
						: order == StickerOrder::Pack ? u"pack"_q : u"newest"_q);
					update();
					box->scrollToY(0);
				}));
			}
			state->menu->popup(toolbar->sort->mapToGlobal(QPoint(0, toolbar->sort->height())));
		});
		stickers->changes() | rpl::on_next([=] {
			if (!state->scheduled) {
				state->scheduled = true;
				crl::on_main(box, [=] {
					state->scheduled = false;
					update();
				});
			}
		}, box->lifetime());
		stickers->enabledValue() | rpl::on_next([=](bool enabled) {
			if (!enabled) {
				box->closeBox();
			}
		}, box->lifetime());
		results->scrollRequests() | rpl::on_next([=](QRect rect) {
			const auto top = results->mapTo(box->verticalLayout(), QPoint()).y();
			box->scrollToY(top + rect.top(), top + rect.bottom());
		}, box->lifetime());
		box->boxClosing() | rpl::on_next([=] {
			stickers->cancel(state->requestId);
		}, box->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		box->addButton(tr::ayu_ChatStickersRefresh(), [=] {
			stickers->restart(cache);
			loadMore();
			box->scrollToY(0);
		});
		box->setFocusCallback([=] { results->setFocus(); });
		update();
	}));
}

} // namespace Ayu
