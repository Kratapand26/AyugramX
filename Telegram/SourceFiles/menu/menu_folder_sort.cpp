/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_folder_sort.h"

#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "dialogs/dialogs_common.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/popup_menu.h"

#include <array>

#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"

namespace FolderSortMenu {
namespace {

void FillMenu(
		not_null<Main::Session*> session,
		FilterId id,
		not_null<Ui::PopupMenu*> menu) {
	using Order = Dialogs::FolderSortOrder;
	const auto current = session->data().chatsFilters().folderSortOrder(id);
	const auto options = std::array{
		std::pair{ Order::Default, tr::lng_filters_sort_default(tr::now) },
		std::pair{
			Order::UnreadDescending,
			tr::lng_filters_sort_unread_descending(tr::now),
		},
		std::pair{
			Order::UnreadAscending,
			tr::lng_filters_sort_unread_ascending(tr::now),
		},
	};
	for (const auto &[order, text] : options) {
		menu->addAction(
			text,
			crl::guard(session, [=] {
				session->data().chatsFilters().setFolderSortOrder(id, order);
			}),
			(current == order) ? &st::mediaPlayerMenuCheck : nullptr);
	}
}

} // namespace

void AddAction(
		not_null<Main::Session*> session,
		FilterId id,
		const Ui::Menu::MenuCallback &addAction) {
	addAction({
		.text = tr::lng_filters_context_sort(tr::now),
		.handler = nullptr,
		.icon = &st::menuIconChats,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> menu) {
			FillMenu(session, id, menu);
		},
	});
}

} // namespace FolderSortMenu
