// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "boxes/abstract_box.h"
#include "rpl/variable.h"

namespace Ui {
class InputField;
class FlatLabel;
class SettingsButton;
class VerticalLayout;
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

class PeerData;

namespace AyuForwarder {

// Redesigned Auto-Forwarder settings dialog. Built from native Telegram
// settings primitives (toggle rows, choice pickers, labeled inputs, section
// headers, dividers) instead of hand-tuned widgets, so it stays aligned and
// themed. Exposes every config key the ForwarderEngine can read.
class ForwarderBox : public Ui::BoxContent {
public:
	ForwarderBox(
		QWidget *,
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		int64 topicRootId);

	static void Show(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		int64 topicRootId = 0);

protected:
	void prepare() override;
	void setInnerFocus() override;

private:
	// Persisted values read from config.ini before the UI is built. Toggle rows
	// are born with the correct state from these (SettingsButton has no public
	// setter), and inputs are pre-filled from them.
	struct LoadedValues {
		QString sourceIds;
		QString sourceTopicIds;
		QString destId;
		QString destTopicId;

		QString allowedTypes;
		QString primaryKeywords;
		QString secondaryKeywords;
		QString wordsToRemove;
		QString removePatterns;
		bool caseSensitiveKeywords = false;
		bool removalCaseSensitive = false;
		bool removeWholeWordOnly = true;

		bool dropCaptions = false;
		bool duSendIndividually = false;
		bool splitDocAlbums = true;
		bool inlineButtonsToText = false;
		bool filenameAsCaption = false;

		QString delaySeconds;
		QString batchSize;
		QString fetchCycleDelay;
		QString concurrentBatchSize;

		QString retryAttempts;
		QString retryDelay;
		QString failureThreshold;
		QString iterChunkWait;

		QString startMessageId;
		QString endMessageId;
		QString startMessage;
		QString endMessage;
		bool pinStartMessage = false;
	};

	// --- Section builders ---
	void buildSections();
	void buildChannelBindings(Ui::VerticalLayout *inner);
	void buildMessageFiltering(Ui::VerticalLayout *inner);
	void buildDispatch(Ui::VerticalLayout *inner);
	void buildTimings(Ui::VerticalLayout *inner);
	void buildAdvanced(Ui::VerticalLayout *inner);
	void buildExecutionOverrides(Ui::VerticalLayout *inner);
	void buildLogging(Ui::VerticalLayout *inner);

	// --- Row helpers (idiomatic, consistently padded) ---
	Ui::InputField *addLabeledInput(
		Ui::VerticalLayout *inner,
		const QString &title,
		const QString &placeholder,
		const QString &initialValue,
		bool multiline = false);
	Ui::SettingsButton *addToggleRow(
		Ui::VerticalLayout *inner,
		const QString &title,
		bool initial);
	void addHint(Ui::VerticalLayout *inner, const QString &text);

	// --- Config persistence ---
	void loadFromConfig();
	void saveToConfig();

	// --- Engine start ---
	void startForwarding();

	not_null<Window::SessionController*> _controller;
	PeerData *_peer = nullptr;
	int64 _topicRootId = 0;
	Ui::VerticalLayout *_content = nullptr;

	LoadedValues _loaded;

	// --- Labeled text inputs ---
	Ui::InputField *_sourceIds = nullptr;
	Ui::InputField *_sourceTopicIds = nullptr;
	Ui::InputField *_destId = nullptr;
	Ui::InputField *_destTopicId = nullptr;

	Ui::InputField *_allowedTypesInput = nullptr;
	Ui::InputField *_primaryKeywords = nullptr;
	Ui::InputField *_secondaryKeywords = nullptr;
	Ui::InputField *_wordsToRemove = nullptr;
	Ui::InputField *_removePatterns = nullptr;

	Ui::InputField *_delaySeconds = nullptr;
	Ui::InputField *_batchSize = nullptr;
	Ui::InputField *_fetchCycleDelay = nullptr;
	Ui::InputField *_concurrentBatchSize = nullptr;

	Ui::InputField *_retryAttempts = nullptr;
	Ui::InputField *_retryDelay = nullptr;
	Ui::InputField *_failureThreshold = nullptr;
	Ui::InputField *_iterChunkWait = nullptr;

	Ui::InputField *_startMessageId = nullptr;
	Ui::InputField *_endMessageId = nullptr;
	Ui::InputField *_startMessage = nullptr;
	Ui::InputField *_endMessage = nullptr;

	// --- Choice (picker) rows ---
	Ui::SettingsButton *_allowedTypesPresetRow = nullptr;
	Ui::SettingsButton *_forwardingModeRow = nullptr;
	Ui::SettingsButton *_logLevelRow = nullptr;

	// Backing values for choice rows (also drive the on-row labels).
	rpl::variable<QString> _forwardingModeValue = u"native_drop_author"_q;
	rpl::variable<QString> _logLevelValue = u"INFO"_q;

	// --- Toggle rows ---
	Ui::SettingsButton *_dropCaptions = nullptr;
	Ui::SettingsButton *_duSendIndividually = nullptr;
	Ui::SettingsButton *_splitDocAlbums = nullptr;
	Ui::SettingsButton *_inlineButtonsToText = nullptr;
	Ui::SettingsButton *_filenameAsCaption = nullptr;
	Ui::SettingsButton *_caseSensitiveKeywords = nullptr;
	Ui::SettingsButton *_removalCaseSensitive = nullptr;
	Ui::SettingsButton *_removeWholeWordOnly = nullptr;
	Ui::SettingsButton *_pinStartMessage = nullptr;

	base::unique_qptr<Ui::PopupMenu> _menu;
};

} // namespace AyuForwarder
