/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/widgets/menu/menu_add_action_callback.h"

namespace Main {
class Session;
} // namespace Main

namespace FolderSortMenu {

void AddAction(
	not_null<Main::Session*> session,
	FilterId id,
	const Ui::Menu::MenuCallback &addAction);

} // namespace FolderSortMenu
