#pragma once

#include "base/basic_types.h"

class ChannelData;

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu {

void ShowLocalChannelsBox(not_null<Window::SessionController*> controller);
void ToggleLocalChannel(
	not_null<Window::SessionController*> controller,
	not_null<ChannelData*> channel);

} // namespace Ayu
