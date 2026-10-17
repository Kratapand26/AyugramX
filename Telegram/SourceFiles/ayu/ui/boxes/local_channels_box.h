#pragma once

#include "base/basic_types.h"

class ChannelData;

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu {

struct LocalChannelEntry;

struct LocalChannelOpener {
	Fn<void(const LocalChannelEntry&)> open;
	Fn<void()> cancel;
};

[[nodiscard]] LocalChannelOpener MakeLocalChannelOpener(
	not_null<Ui::RpWidget*> context,
	not_null<Window::SessionController*> controller,
	Fn<void()> beforeOpen);

void ShowLocalChannelsBox(not_null<Window::SessionController*> controller);
void ToggleLocalChannel(
	not_null<Window::SessionController*> controller,
	not_null<ChannelData*> channel);

} // namespace Ayu
