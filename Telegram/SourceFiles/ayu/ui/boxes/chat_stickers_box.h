#pragma once

#include "data/data_msg_id.h"

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu {

void ShowChatStickersBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId topicRootId = 0);

} // namespace Ayu
