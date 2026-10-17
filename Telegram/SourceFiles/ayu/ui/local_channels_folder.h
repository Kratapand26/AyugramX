#pragma once

#include "ui/rp_widget.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu {

class LocalChannelsFolder final : public Ui::RpWidget {
public:
	LocalChannelsFolder(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~LocalChannelsFolder() override;

	void setQuery(QString query);
	void moveSelection(int delta);
	void activateSelected();
	QAccessible::Role accessibilityRole() override;
	QString accessibilityName() override;

private:
	void resizeEvent(QResizeEvent *e) override;
	void scheduleRefresh();
	void refresh();

	struct State;
	const std::unique_ptr<State> _state;

};

} // namespace Ayu
