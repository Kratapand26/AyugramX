#include "ayu/ui/boxes/chat_stickers_box.h"

#include "api/api_toggling_media.h"
#include "ayu/features/chat_stickers/chat_stickers.h"
#include "base/weak_ptr.h"
#include "boxes/sticker_set_box.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QFocusEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QShortcut>

#include <algorithm>
#include <optional>
#include <utility>

#include "styles/style_ayu_styles.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"

namespace Ayu {
namespace {

constexpr auto kGridPref = "ayu_chat_stickers_grid"_cs;
constexpr auto kSortPref = "ayu_chat_stickers_sort"_cs;
constexpr auto kPacksPref = "ayu_chat_stickers_packs"_cs;
constexpr auto kPreviewCacheLimit = 128;
constexpr auto kSearchLengthLimit = 128;

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
	QString searchText;
};

[[nodiscard]] QString OrderName(StickerOrder order) {
	switch (order) {
	case StickerOrder::Newest: return tr::ayu_ChatStickersNewest(tr::now);
	case StickerOrder::Usage: return tr::ayu_ChatStickersUsage(tr::now);
	case StickerOrder::Pack: return tr::ayu_ChatStickersPack(tr::now);
	}
	Unexpected("Invalid chat sticker order.");
}

[[nodiscard]] rpl::producer<QString> OrderLabel(StickerOrder order) {
	switch (order) {
	case StickerOrder::Newest: return tr::ayu_ChatStickersNewest();
	case StickerOrder::Usage: return tr::ayu_ChatStickersUsage();
	case StickerOrder::Pack: return tr::ayu_ChatStickersPack();
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

[[nodiscard]] QString SearchText(QString text) {
	return text.normalized(QString::NormalizationForm_KC)
		.remove(u'\uFE0E')
		.remove(u'\uFE0F')
		.simplified()
		.toCaseFolded();
}

[[nodiscard]] QString ContextTitle(not_null<PeerData*> peer, MsgId topicRootId) {
	if (topicRootId) {
		if (const auto channel = peer->asChannel()) {
			if (const auto forum = channel->forum()) {
				if (const auto topic = forum->topicFor(topicRootId)) {
					return peer->name() + u" · "_q + topic->title();
				}
			}
		}
	}
	return peer->name();
}

class StickerToolbar final : public Ui::RpWidget {
public:
	explicit StickerToolbar(QWidget *parent);

	void setGrid(bool grid);
	void setOrder(StickerOrder order);

	const not_null<Ui::SettingsSlider*> tabs;
	const not_null<Ui::RoundButton*> sort;
	const not_null<Ui::IconButton*> grid;
	const not_null<Ui::IconButton*> list;

private:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *e) override;

	bool _grid = true;
	StickerOrder _order = StickerOrder::Newest;

};

class StickerSearch final : public Ui::RpWidget {
public:
	explicit StickerSearch(QWidget *parent);

	const not_null<Ui::InputField*> field;

private:
	int resizeGetHeight(int width) override;

	const not_null<Ui::IconButton*> _icon;
	const not_null<Ui::IconButton*> _clear;

};

class StickerStatus final : public Ui::RpWidget {
public:
	explicit StickerStatus(QWidget *parent);

	void setText(QString text, bool loading);
	QString accessibilityName() override;

private:
	int resizeGetHeight(int width) override;
	void paintEvent(QPaintEvent *e) override;
	void updateAnimation();

	QString _text;
	Ui::InfiniteRadialAnimation _animation;
	bool _loading = false;
	bool _shown = false;

};

class StickerResults final : public Ui::RpWidget {
public:
	StickerResults(
		QWidget *parent,
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<ChatStickersCache> cache);

	void refresh(
		StickerOrder order,
		bool grid,
		bool packs,
		QString query);
	void invalidate();
	void focusFirst();
	[[nodiscard]] int count() const;
	[[nodiscard]] rpl::producer<QRect> scrollRequests() const;
	QAccessible::Role accessibilityRole() override;
	Qt::FocusPolicy accessibilityFocusPolicy() override;
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
	void focusInEvent(QFocusEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	[[nodiscard]] QRect cellRect(int index) const;
	[[nodiscard]] int indexAt(QPoint point) const;
	void select(int index);
	void openPack(int index);
	void showActions(int index, QPoint position);
	void paintSticker(Painter &p, int index);
	[[nodiscard]] int cellHeight() const;
	[[nodiscard]] DisplaySticker &itemAt(int index);

	const not_null<Ui::GenericBox*> _box;
	const base::weak_ptr<Window::SessionController> _controller;
	const not_null<Main::Session*> _session;
	const std::shared_ptr<ChatStickersCache> _cache;
	std::vector<DisplaySticker> _items;
	std::vector<int> _filtered;
	std::map<DocumentId, std::shared_ptr<Data::DocumentMedia>> _media;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::event_stream<QRect> _scrollRequests;
	StickerOrder _order = StickerOrder::Newest;
	QString _query;
	bool _grid = true;
	bool _packs = false;
	int _revision = -1;
	int _columns = 1;
	int _selected = -1;
	int _pressed = -1;
	int _visibleTop = 0;
	int _visibleBottom = -1;

};

StickerToolbar::StickerToolbar(QWidget *parent)
: RpWidget(parent)
, tabs(Ui::CreateChild<Ui::SettingsSlider>(this, st::chatStickersTabs))
, sort(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::ayu_ChatStickersNewest(),
	st::chatStickersSort))
, grid(Ui::CreateChild<Ui::IconButton>(this, st::chatStickersGrid))
, list(Ui::CreateChild<Ui::IconButton>(this, st::chatStickersList)) {
	tabs->setSections({
		tr::ayu_ChatStickersStickersTab(tr::now),
		tr::ayu_ChatStickersPacksTab(tr::now),
	});
	grid->setAccessibleName(tr::ayu_ChatStickersGrid(tr::now));
	list->setAccessibleName(tr::ayu_ChatStickersList(tr::now));
	grid->setIconColorOverride(st::windowActiveTextFg->c);
	sort->setAccessibleName(tr::ayu_ChatStickersSort(tr::now)
		+ u": "_q + OrderName(_order));
	for (const auto button : { grid, list }) {
		button->setFocusPolicy(Qt::StrongFocus);
		Ui::InstallTooltip(button, [=] { return button->accessibleName(); });
	}
	Ui::InstallTooltip(sort, [=] { return sort->accessibleName(); });
}

void StickerToolbar::setGrid(bool enabled) {
	if (_grid == enabled) {
		return;
	}
	_grid = enabled;
	grid->setIconColorOverride(enabled
		? std::make_optional(st::windowActiveTextFg->c)
		: std::nullopt);
	list->setIconColorOverride(!enabled
		? std::make_optional(st::windowActiveTextFg->c)
		: std::nullopt);
	update();
}

void StickerToolbar::setOrder(StickerOrder order) {
	if (_order == order) {
		return;
	}
	_order = order;
	sort->setText(OrderLabel(order));
	sort->setAccessibleName(tr::ayu_ChatStickersSort(tr::now)
		+ u": "_q + OrderName(order));
}

int StickerToolbar::resizeGetHeight(int width) {
	const auto gap = st::chatStickersToolbarGap;
	const auto viewsWidth = grid->width() + list->width();
	sort->setFullWidth(std::min(
		st::chatStickersSortMaxWidth,
		std::max(0, width - viewsWidth - gap)));
	const auto controlsWidth = sort->width() + gap + viewsWidth;
	const auto oneRow = (width >= st::chatStickersTabsMinWidth
		+ gap + controlsWidth);
	tabs->resizeToWidth(oneRow ? width - controlsWidth - gap : width);
	const auto top = oneRow
		? (tabs->height() - grid->height()) / 2
		: tabs->height() + gap;
	list->moveToRight(0, top, width);
	grid->moveToRight(list->width(), top, width);
	sort->moveToRight(viewsWidth + gap, top, width);
	return std::max(tabs->height(), top + grid->height());
}

void StickerToolbar::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawRoundedRect(
		(_grid ? grid : list)->geometry(),
		st::chatStickersCellRadius,
		st::chatStickersCellRadius);
}

StickerSearch::StickerSearch(QWidget *parent)
: RpWidget(parent)
, field(Ui::CreateChild<Ui::InputField>(
	this,
	st::chatStickersSearchInput,
	tr::ayu_ChatStickersSearch()))
, _icon(Ui::CreateChild<Ui::IconButton>(field, st::chatStickersSearchIcon))
, _clear(Ui::CreateChild<Ui::IconButton>(field, st::chatStickersSearchClear)) {
	field->setMaxLength(kSearchLengthLimit);
	_icon->setClickedCallback([=] { field->setFocusFast(); });
	_icon->setAccessibleName(tr::ayu_ChatStickersSearch(tr::now));
	_clear->setAccessibleName(tr::lng_sr_clear_search(tr::now));
	_clear->setFocusPolicy(Qt::StrongFocus);
	_clear->setClickedCallback([=] {
		field->setText(QString());
		field->setFocusFast();
	});
	field->changes() | rpl::on_next([=] {
		_clear->setVisible(!field->empty());
	}, lifetime());
	_clear->hide();
}

int StickerSearch::resizeGetHeight(int width) {
	field->resizeToWidth(width);
	_icon->moveToLeft(0, 0, width);
	_clear->moveToRight(0, 0, width);
	return field->height();
}

StickerStatus::StickerStatus(QWidget *parent)
: RpWidget(parent)
, _animation([=] { update(); }, st::chatStickersLoading) {
	shownValue() | rpl::on_next([=](bool shown) {
		_shown = shown;
		updateAnimation();
	}, lifetime());
	Ui::InstallTooltip(this, [=] { return _text; });
}

void StickerStatus::setText(QString text, bool loading) {
	if (_text != text) {
		_text = std::move(text);
		accessibilityNameChanged();
	}
	_loading = loading;
	updateAnimation();
	update();
}

QString StickerStatus::accessibilityName() {
	return _text;
}

int StickerStatus::resizeGetHeight(int width) {
	return st::chatStickersStatusHeight;
}

void StickerStatus::updateAnimation() {
	if (_shown && _loading) {
		if (!_animation.animating()) {
			_animation.start();
		}
	} else {
		_animation.stop(anim::type::instant);
	}
}

void StickerStatus::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto &font = st::chatStickersStatusFont;
	const auto size = st::chatStickersLoading.size;
	const auto left = _loading ? size.width() + st::chatStickersTextGap : 0;
	if (_loading) {
		_animation.draw(p, QPoint(0, (height() - size.height()) / 2), size, width());
	}
	p.setFont(font);
	p.setPen(st::windowSubTextFg);
	p.drawTextLeft(
		left,
		(height() - font->height) / 2,
		width(),
		font->elided(_text, std::max(0, width() - left)));
}

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

void StickerResults::refresh(
		StickerOrder order,
		bool grid,
		bool packs,
		QString query) {
	query = SearchText(std::move(query));
	const auto rebuild = (_revision != _cache->revision)
		|| (_packs != packs);
	const auto resort = rebuild || (_order != order);
	if (rebuild) {
		_revision = _cache->revision;
		_packs = packs;
		_items.clear();
		auto packRows = std::map<QString, int>();
		for (const auto &entry : _cache->data.entries()) {
			const auto document = _session->data().document(entry.documentId);
			const auto sticker = document->sticker();
			if (!sticker) {
				continue;
			}
			const auto &set = sticker->set;
			if (packs && set.empty()) {
				continue;
			}
			const auto title = PackTitle(document);
			const auto key = set.id
				? u"id:"_q + QString::number(set.id)
				: u"name:"_q + set.shortName.toLower();
			if (packs) {
				const auto i = packRows.find(key);
				if (i != end(packRows)) {
					auto &row = _items[i->second];
					const auto count = row.entry.count + entry.count;
					if (entry.messageId > row.entry.messageId) {
						row.entry = entry;
					}
					row.entry.count = count;
					row.searchText += u" "_q + SearchText(sticker->alt);
					continue;
				}
				packRows.emplace(key, int(_items.size()));
			}
			const auto search = title
				+ u" "_q + set.shortName
				+ u" "_q + sticker->alt;
			_items.push_back({
				entry,
				title,
				{},
				key,
				SearchText(search),
			});
		}
	}
	if (resort) {
		_order = order;
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
	}
	if (resort || _query != query) {
		_query = std::move(query);
		_filtered.clear();
		_filtered.reserve(_items.size());
		for (auto i = 0; i != int(_items.size()); ++i) {
			if (_query.isEmpty() || _items[i].searchText.contains(_query)) {
				_filtered.push_back(i);
			}
		}
		_pressed = -1;
		select(-1);
	}
	_grid = grid;
	resizeToWidth(width());
	update();
}

void StickerResults::invalidate() {
	_revision = -1;
}

void StickerResults::focusFirst() {
	setFocus(Qt::ShortcutFocusReason);
	if (!_filtered.empty()) {
		select(0);
		_scrollRequests.fire_copy(cellRect(0));
	}
}

int StickerResults::count() const {
	return int(_filtered.size());
}

QAccessible::Role StickerResults::accessibilityRole() {
	return QAccessible::Role::List;
}

Qt::FocusPolicy StickerResults::accessibilityFocusPolicy() {
	return Qt::StrongFocus;
}

rpl::producer<QRect> StickerResults::scrollRequests() const {
	return _scrollRequests.events();
}

QString StickerResults::accessibilityName() {
	if (_selected < 0) {
		return tr::ayu_ChatStickersTitle(tr::now);
	}
	const auto &entry = itemAt(_selected).entry;
	return PackTitle(_session->data().document(entry.documentId))
		+ u", "_q
		+ tr::ayu_ChatStickersUses(tr::now, lt_count, entry.count);
}

int StickerResults::resizeGetHeight(int width) {
	_columns = _grid ? std::max(1, width / st::chatStickersCellWidth) : 1;
	const auto rows = (count() + _columns - 1) / _columns;
	return std::max(
		st::chatStickersEmptyHeight,
		rows * cellHeight());
}

void StickerResults::visibleTopBottomUpdated(int top, int bottom) {
	_visibleTop = top;
	_visibleBottom = bottom;
}

QRect StickerResults::cellRect(int index) const {
	const auto left = (index % _columns) * width() / _columns;
	const auto right = ((index % _columns) + 1) * width() / _columns;
	const auto height = cellHeight();
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
	const auto row = point.y() / cellHeight();
	const auto column = ((x + 1) * _columns - 1) / std::max(1, width());
	const auto index = row * _columns + column;
	return (index < count()) ? index : -1;
}

void StickerResults::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_filtered.empty()) {
		const auto &icon = st::chatStickersEmptyIcon;
		const auto textHeight = 3 * st::normalFont->height;
		const auto top = std::max(0, (height()
			- icon.height() - st::chatStickersEmptyGap - textHeight) / 2);
		icon.paint(p, (width() - icon.width()) / 2, top, width());
		const auto textWidth = std::min(width(), st::chatStickersEmptyTextWidth);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(
				(width() - textWidth) / 2,
				top + icon.height() + st::chatStickersEmptyGap,
				textWidth,
				textHeight),
			Qt::AlignCenter | Qt::TextWordWrap,
			!_cache->loaded
				? tr::ayu_ChatStickersRestoring(tr::now)
				: !_query.isEmpty()
				? tr::ayu_ChatStickersNoMatches(tr::now)
				: _packs
				? tr::ayu_ChatStickersNoPacks(tr::now)
				: tr::ayu_ChatStickersEmpty(tr::now));
		return;
	}
	const auto height = cellHeight();
	const auto top = std::max(e->rect().top(), _visibleTop);
	const auto bottom = (_visibleBottom < 0)
		? e->rect().bottom()
		: std::min(e->rect().bottom(), _visibleBottom - 1);
	if (bottom < top) {
		return;
	}
	const auto first = std::max(0, top / height) * _columns;
	const auto last = std::min(
		count(),
		(bottom / height + 1) * _columns);
	for (auto i = first; i < last; ++i) {
		paintSticker(p, i);
	}
	if (_media.size() > kPreviewCacheLimit) {
		auto visible = base::flat_set<DocumentId>();
		for (auto i = first; i < last; ++i) {
			visible.emplace(itemAt(i).entry.documentId);
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
	auto &item = itemAt(index);
	const auto cell = cellRect(index);
	if (index == _selected) {
		auto hq = PainterHighQualityEnabler(p);
		const auto inset = st::chatStickersCellInset;
		p.setPen(hasFocus()
			? QPen(st::windowActiveTextFg->c, st::lineWidth)
			: QPen(Qt::NoPen));
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(
			cell.adjusted(inset, inset, -inset, -inset),
			st::chatStickersCellRadius,
			st::chatStickersCellRadius);
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
		if (_packs) {
			p.drawText(
				QRect(
					cell.x() + padding,
					previewRect.bottom() + st::chatStickersTextGap,
					std::max(0, cell.width() - 2 * padding),
					st::normalFont->height),
				Qt::AlignCenter,
				st::normalFont->elided(item.title, cell.width() - 2 * padding));
		}
		const auto &font = st::chatStickersBadgeFont;
		const auto text = font->elided(
			item.uses,
			std::max(0, cell.width() - 2 * (padding + st::chatStickersBadgePadding)));
		const auto badgeWidth = font->width(text) + 2 * st::chatStickersBadgePadding;
		const auto badge = QRect(
			cell.x() + (cell.width() - badgeWidth) / 2,
			cell.y() + cell.height() - padding - st::chatStickersBadgeHeight,
			badgeWidth,
			st::chatStickersBadgeHeight);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(index == _selected ? st::lightButtonBgOver : st::windowBgOver);
		p.drawRoundedRect(
			badge,
			st::chatStickersBadgeRadius,
			st::chatStickersBadgeRadius);
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			badge,
			Qt::AlignCenter,
			text);
	} else {
		const auto &titleFont = st::semiboldFont;
		const auto &subtitleFont = st::chatStickersStatusFont;
		const auto left = padding + preview + padding;
		const auto available = std::max(0, cell.width() - left - padding);
		const auto top = cell.y()
			+ (cell.height() - titleFont->height - subtitleFont->height
				- st::chatStickersTextGap) / 2;
		p.setFont(titleFont);
		p.setPen(st::windowFg);
		p.drawTextLeft(left, top, width(), titleFont->elided(item.title, available));
		p.setFont(subtitleFont);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(
			left,
			top + titleFont->height + st::chatStickersTextGap,
			width(),
			subtitleFont->elided(item.uses, available));
	}
}

int StickerResults::cellHeight() const {
	return !_grid
		? st::chatStickersListHeight
		: _packs
		? st::chatStickersPackCellHeight
		: st::chatStickersCellHeight;
}

DisplaySticker &StickerResults::itemAt(int index) {
	return _items[_filtered[index]];
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

void StickerResults::focusInEvent(QFocusEvent *e) {
	RpWidget::focusInEvent(e);
	if (_selected >= 0) {
		update(cellRect(_selected));
	}
}

void StickerResults::focusOutEvent(QFocusEvent *e) {
	RpWidget::focusOutEvent(e);
	if (_selected >= 0) {
		update(cellRect(_selected));
	}
}

void StickerResults::keyPressEvent(QKeyEvent *e) {
	if (_filtered.empty()) {
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
	case Qt::Key_End: next = count() - 1; break;
	case Qt::Key_Enter:
	case Qt::Key_Return: openPack(current); return;
	case Qt::Key_Menu: showActions(current, mapToGlobal(cellRect(current).center())); return;
	default: return RpWidget::keyPressEvent(e);
	}
	if (_selected < 0 && e->key() != Qt::Key_End) {
		next = 0;
	}
	select(std::clamp(next, 0, count() - 1));
	_scrollRequests.fire_copy(cellRect(_selected));
	e->accept();
}

void StickerResults::openPack(int index) {
	if (!_controller) {
		return;
	}
	const auto document = _session->data().document(itemAt(index).entry.documentId);
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
	const auto entry = itemAt(index).entry;
	const auto document = _session->data().document(entry.documentId);
	const auto origin = FullMsgId(_cache->peerId, entry.messageId);
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
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
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });
		const auto info = box->addTopButton(st::chatStickersBoxInfo, [=] {
			if (weak) {
				weak->show(Ui::MakeInformBox(tr::ayu_ChatStickersAbout(tr::now)));
			}
		});
		info->setAccessibleName(tr::ayu_ChatStickersInfo(tr::now));
		info->setFocusPolicy(Qt::StrongFocus);
		Ui::InstallTooltip(info.data(), [] {
			return tr::ayu_ChatStickersAbout(tr::now);
		});
		const auto more = box->addTopButton(st::boxTitleMenu);
		more->setAccessibleName(tr::ayu_ChatStickersOptions(tr::now));
		more->setFocusPolicy(Qt::StrongFocus);
		Ui::InstallTooltip(more.data(), [] {
			return tr::ayu_ChatStickersOptions(tr::now);
		});
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
		const auto top = box->setPinnedToTopContent(
			object_ptr<Ui::VerticalLayout>(box));
		top->add(
			object_ptr<Ui::FlatLabel>(
				top,
				ContextTitle(peer, topicRootId),
				st::chatStickersContext),
			st::chatStickersContextPadding);
		const auto search = top->add(
			object_ptr<StickerSearch>(top),
			st::chatStickersControlsPadding);
		const auto toolbar = top->add(
			object_ptr<StickerToolbar>(top),
			st::chatStickersControlsPadding);
		toolbar->tabs->setActiveSectionFast(state->packs ? 1 : 0);
		const auto results = box->addRow(object_ptr<StickerResults>(
			box,
			box,
			controller,
			cache), st::chatStickersBodyPadding);
		const auto bottom = box->setPinnedToBottomContent(
			object_ptr<Ui::VerticalLayout>(box));
		const auto status = bottom->add(
			object_ptr<StickerStatus>(bottom),
			st::chatStickersStatusPadding);
		const auto loadNew = box->addButton(tr::ayu_ChatStickersRefresh());
		const auto loadOld = box->addButton(tr::ayu_ChatStickersLoadMore());
		const auto update = [=] {
			const auto loading = stickers->loading(cache);
			results->refresh(
				state->order,
				state->grid,
				state->packs,
				search->field->getLastText());
			loadOld->setEnabled(cache->loaded
				&& !loading
				&& !cache->finished
				&& !cache->data.full());
			loadNew->setEnabled(cache->loaded && !loading);
			const auto detail = !cache->loaded
				? tr::ayu_ChatStickersRestoring(tr::now)
				: loading
				? tr::ayu_ChatStickersLoading(tr::now)
				: cache->saveFailed
				? tr::ayu_ChatStickersSaveFailed(tr::now)
				: cache->data.full()
				? tr::ayu_ChatStickersFull(tr::now)
				: cache->finished
				? tr::ayu_ChatStickersFinished(tr::now)
				: !cache->scanned
				? tr::ayu_ChatStickersCached(tr::now)
				: tr::ayu_ChatStickersScanned(
					tr::now,
					lt_count,
					cache->scanned);
			const auto summary = state->packs
				? tr::ayu_ChatStickersPacksFound(
					tr::now,
					lt_count,
					results->count())
				: tr::ayu_ChatStickersFound(
					tr::now,
					lt_count,
					results->count());
			status->setText(
				summary + u" · "_q + detail,
				loading || !cache->loaded);
			toolbar->setGrid(state->grid);
			toolbar->setOrder(state->order);
			toolbar->resizeToWidth(toolbar->width());
		};
		const auto requestPage = [=](bool refresh) {
			if (!cache->loaded || stickers->loading(cache)) {
				return;
			}
			const auto done = crl::guard(box, [=](QString error) {
				state->requestId = 0;
				if (weak && !error.isEmpty()) {
					weak->showToast(error);
				}
			});
			state->requestId = refresh
				? stickers->refresh(cache, done)
				: stickers->loadMore(cache, done);
			update();
		};
		loadOld->setClickedCallback([=] { requestPage(false); });
		loadNew->setClickedCallback([=] {
			requestPage(true);
			box->scrollToY(0);
		});
		const auto setGrid = [=](bool grid) {
			if (state->grid == grid) {
				return;
			}
			state->grid = grid;
			controller->session().local().writePref<QString>(
				kGridPref,
				state->grid ? u"grid"_q : u"list"_q);
			update();
			box->scrollToY(0);
		};
		toolbar->grid->setClickedCallback([=] { setGrid(true); });
		toolbar->list->setClickedCallback([=] { setGrid(false); });
		toolbar->tabs->sectionActivated() | rpl::on_next([=](int index) {
			const auto packs = (index == 1);
			if (state->packs == packs) {
				return;
			}
			state->packs = packs;
			controller->session().local().writePref<bool>(kPacksPref, state->packs);
			update();
			box->scrollToY(0);
		}, box->lifetime());
		toolbar->sort->setClickedCallback([=] {
			state->menu = base::make_unique_q<Ui::PopupMenu>(
				toolbar,
				st::popupMenuWithIcons);
			for (const auto order : {
				StickerOrder::Newest,
				StickerOrder::Usage,
				StickerOrder::Pack,
			}) {
				state->menu->addAction(
					OrderName(order),
					crl::guard(box, [=] {
						state->order = order;
						controller->session().local().writePref<QString>(
							kSortPref,
							order == StickerOrder::Usage ? u"usage"_q
							: order == StickerOrder::Pack ? u"pack"_q : u"newest"_q);
						update();
						box->scrollToY(0);
					}),
					(order == state->order) ? &st::mediaPlayerMenuCheck : nullptr);
			}
			state->menu->popup(toolbar->sort->mapToGlobal(QPoint(0, toolbar->sort->height())));
		});
		search->field->changes() | rpl::on_next([=] {
			update();
			box->scrollToY(0);
		}, box->lifetime());
		search->field->submits() | rpl::on_next([=] {
			results->focusFirst();
		}, box->lifetime());
		search->field->cancelled() | rpl::on_next([=] {
			if (search->field->empty()) {
				box->closeBox();
			} else {
				search->field->setText(QString());
			}
		}, box->lifetime());
		const auto find = new QShortcut(QKeySequence::Find, box);
		find->setContext(Qt::WidgetWithChildrenShortcut);
		QObject::connect(find, &QShortcut::activated, box, [=] {
			search->field->setFocusFast();
			search->field->selectAll();
		});
		const auto scheduleUpdate = [=] {
			if (!state->scheduled) {
				state->scheduled = true;
				crl::on_main(box, [=] {
					state->scheduled = false;
					update();
				});
			}
		};
		stickers->changes() | rpl::on_next(scheduleUpdate, box->lifetime());
		controller->session().data().stickers().updated(
			Data::StickersType::Stickers
		) | rpl::on_next([=] {
			results->invalidate();
			scheduleUpdate();
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
			stickers->flush(cache);
		}, box->lifetime());
		const auto clearCache = [=] {
			if (!weak) {
				return;
			}
			weak->show(Ui::MakeConfirmBox({
				.text = cache->topicRootId
					? tr::ayu_ChatStickersClearTopic(tr::now)
					: tr::ayu_ChatStickersClearChat(tr::now),
				.confirmed = crl::guard(box, [=] {
					stickers->clear(cache);
					state->requestId = 0;
					box->scrollToY(0);
				}),
				.confirmText = tr::ayu_ChatStickersClear(),
			}));
		};
		more->setClickedCallback([=] {
			state->menu = base::make_unique_q<Ui::PopupMenu>(
				box,
				st::popupMenuWithIcons);
			state->menu->addAction(
				tr::ayu_ChatStickersClear(tr::now),
				crl::guard(box, clearCache),
				&st::menuIconClear);
			state->menu->popup(more->mapToGlobal(QPoint(0, more->height())));
		});
		box->setFocusCallback([=] { search->field->setFocusFast(); });
		update();
	}));
}

} // namespace Ayu
