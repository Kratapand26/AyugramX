// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/forwarder_box.h"

#include "ayu/features/forward/forwarder_engine.h"
#include "ayu/features/forward/forwarder_log.h"
#include "ayu/features/forward/forwarder_state.h"
#include "ayu/features/forward/ayu_forward.h"
#include "data/data_peer.h"
#include "data/data_channel.h"
#include "data/data_forum_topic.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/vertical_list.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QtGui/QCursor>

namespace AyuForwarder {
namespace {

// --- File paths (engine runtime files) ---

QString configPath() {
	return QDir::currentPath() + "/config.ini";
}

QString statePath() {
	return QDir::currentPath() + "/forward_state.json";
}

QString logPath() {
	return QDir::currentPath() + "/forwarder.log";
}

bool parseBool(const QString &value) {
	const auto v = value.trimmed().toLower();
	return v == "true" || v == "1" || v == "yes";
}

// --- Choice option tables (stable-address, required by SingleChoiceBoxArgs.options const&) ---

const std::vector<QString> &kModeOptions() {
	static const auto v = std::vector<QString>{
		u"Native (Drop Author)"_q,
		u"Copy (Re-send)"_q,
		u"Download & Upload"_q,
	};
	return v;
}
const std::vector<QString> &kModeRawValues() {
	static const auto v = std::vector<QString>{
		u"native_drop_author"_q,
		u"copy"_q,
		u"download_upload"_q,
	};
	return v;
}

const std::vector<QString> &kPresetOptions() {
	static const auto v = std::vector<QString>{
		u"All Types (Default)"_q,
		u"Media Only"_q,
		u"Documents Only"_q,
		u"Text Only"_q,
		u"Clear"_q,
	};
	return v;
}

const std::vector<QString> &kLogLevelOptions() {
	static const auto v = std::vector<QString>{ u"INFO"_q, u"DEBUG"_q };
	return v;
}

QString modeRawToLabel(const QString &raw) {
	const auto &opts = kModeOptions();
	const auto &vals = kModeRawValues();
	for (auto i = 0, n = int(vals.size()); i < n; ++i) {
		if (vals[i] == raw) return opts[i];
	}
	return opts.front();
}

int modeRawToIndex(const QString &raw) {
	const auto &vals = kModeRawValues();
	for (auto i = 0, n = int(vals.size()); i < n; ++i) {
		if (vals[i] == raw) return i;
	}
	return 0;
}

QString applyPresetByIndex(int index) {
	switch (index) {
	case 0: return u"text,photo,video,document,sticker,gif,album,poll,dice,webpage,audio,voice"_q;
	case 1: return u"photo,video,document,sticker,gif,album,audio,voice"_q;
	case 2: return u"document"_q;
	case 3: return u"text"_q;
	case 4: return QString(); // Clear
	default: return QString();
	}
}

} // namespace

// ============================================================================
// Construction / launch
// ============================================================================

ForwarderBox::ForwarderBox(
	QWidget *,
	not_null<Window::SessionController*> controller,
	PeerData *peer,
	int64 topicRootId)
: _controller(controller)
, _peer(peer)
, _topicRootId(topicRootId) {
}

void ForwarderBox::Show(
		not_null<Window::SessionController*> controller,
		PeerData *peer,
		int64 topicRootId) {
	controller->show(Box<ForwarderBox>(controller, peer, topicRootId));
}

// ============================================================================
// prepare() — assemble the dialog chrome + scrollable content
// ============================================================================

void ForwarderBox::prepare() {
	setTitle(rpl::single(QString("Auto-Forwarder")));

	auto content = object_ptr<Ui::VerticalLayout>(this);
	_content = content.data();

	setInnerWidget(object_ptr<Ui::OverrideMargins>(this, std::move(content)));

	// Load persisted values first so toggle rows are born with the right state.
	// (SettingsButton has no public setter; we pass initial values at build time.)
	loadFromConfig();
	buildSections();

	// Force a layout pass so VerticalLayout computes actual child heights.
	const auto screen = QGuiApplication::primaryScreen()
		? QGuiApplication::primaryScreen()->availableGeometry()
		: QRect(0, 0, 800, 600);
	const auto boxWidth = std::min(int(st::forwarderBoxWidth), screen.width() - 48);
	const auto maxBoxHeight = int(screen.height() * 0.75);

	_content->resizeToWidth(boxWidth);

	// Reactively track height changes (e.g. collapsible sections toggling).
	_content->heightValue(
	) | rpl::on_next([=](int height) {
		setDimensions(boxWidth, std::min(height, maxBoxHeight));
	}, _content->lifetime());

	// --- Bottom buttons ---
	addButton(tr::lng_settings_save(), [=] { saveToConfig(); });
	addButton(rpl::single(QString("Start")), [=] { startForwarding(); });

	// Left button: clear the resume-state file so the next run starts fresh.
	addLeftButton(rpl::single(QString("Reset Resume State")), [=] {
		QFile(statePath()).remove();
		auto &fwState = ForwarderState::instance();
		fwState.setFilePath(statePath());
		fwState.load();
		_controller->showToast("Resume state cleared.");
	});

	// Top-right close (X).
	addTopButton(st::boxTitleClose, [=] { closeBox(); });
}

void ForwarderBox::setInnerFocus() {
	if (_destId) {
		_destId->setFocusFast();
	}
}

// ============================================================================
// buildSections() — the section skeleton
// ============================================================================

void ForwarderBox::buildSections() {
	const auto inner = _content;
	Expects(inner != nullptr);

	Ui::AddSkip(inner);
	buildChannelBindings(inner);
	Ui::AddSkip(inner);
	Ui::AddDividerText(inner, rpl::single(QString(
		"Comma-separated IDs. Source channels are mirrored into the destination.")));

	buildMessageFiltering(inner);
	buildDispatch(inner);
	buildTimings(inner);
	buildAdvanced(inner);
	buildExecutionOverrides(inner);
	buildLogging(inner);
	Ui::AddSkip(inner);
}

// ============================================================================
// Row helpers (idiomatic, consistently padded) — fixes all alignment issues
// ============================================================================

Ui::InputField *ForwarderBox::addLabeledInput(
		Ui::VerticalLayout *inner,
		const QString &title,
		const QString &placeholder,
		const QString &initialValue,
		bool multiline) {
	inner->add(
		object_ptr<Ui::FlatLabel>(this, title, st::forwarderFieldTitle),
		st::forwarderFieldTitlePadding);
	const auto field = inner->add(
		object_ptr<Ui::InputField>(
			this,
			multiline ? st::forwarderMultilineInput : st::forwarderInputField,
			rpl::single(placeholder),
			initialValue),
		style::margins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			st::boxRowPadding.left() / 2));
	return field;
}

Ui::SettingsButton *ForwarderBox::addToggleRow(
		Ui::VerticalLayout *inner,
		const QString &title,
		bool initial) {
	const auto button = Settings::AddButtonWithIcon(
		inner,
		rpl::single(title),
		st::settingsButtonNoIcon);
	button->toggleOn(rpl::single(initial));
	return button;
}

void ForwarderBox::addHint(Ui::VerticalLayout *inner, const QString &text) {
	inner->add(
		object_ptr<Ui::FlatLabel>(this, text, st::forwarderFieldHint),
		style::margins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			st::boxRowPadding.left()));
}

// ============================================================================
// Section: Channel Bindings
// ============================================================================

void ForwarderBox::buildChannelBindings(Ui::VerticalLayout *inner) {
	Ui::AddSubsectionTitle(inner, rpl::single(QString("Channel Bindings")));
	_sourceIds = addLabeledInput(
		inner,
		"Source Channel IDs",
		"-1001234567890,-1009876543210",
		_loaded.sourceIds);
	_sourceTopicIds = addLabeledInput(
		inner,
		"Source Topic IDs (optional, parallel to sources)",
		"0,17,0",
		_loaded.sourceTopicIds);
	_destId = addLabeledInput(inner, "Destination ID", "-1001111111111", _loaded.destId);
	_destTopicId = addLabeledInput(
		inner,
		"Destination Topic ID (optional)",
		"0",
		_loaded.destTopicId);
	addHint(inner, "Use 0 for no topic filter. Topic IDs must line up with the source IDs above.");
}

// ============================================================================
// Section: Message Filtering
// ============================================================================

void ForwarderBox::buildMessageFiltering(Ui::VerticalLayout *inner) {
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, rpl::single(QString("Message Filtering")));

	_allowedTypesInput = addLabeledInput(
		inner,
		"Allowed Types (comma-sep)",
		"text,photo,video,document",
		_loaded.allowedTypes);

	// Quick Presets picker row — native choice box, fixes the old broken dropdown.
	_allowedTypesPresetRow = Settings::AddButtonWithLabel(
		inner,
		rpl::single(QString("Quick Presets")),
		rpl::single(QString("Choose\u2026")),
		st::settingsButtonNoIcon);
	_allowedTypesPresetRow->addClickHandler([=] {
		_menu = base::make_unique_q<Ui::PopupMenu>(_allowedTypesPresetRow, st::defaultPopupMenu);
		const auto options = kPresetOptions();
		for (int i = 0; i < options.size(); i++) {
			_menu->addAction(options[i], [=] {
				if (_allowedTypesInput) {
					_allowedTypesInput->setText(applyPresetByIndex(i));
				}
			});
		}
		_menu->popup(QCursor::pos());
	});

	_primaryKeywords = addLabeledInput(
		inner,
		"Primary Keywords (caption must contain; comma-sep)",
		"news,update",
		_loaded.primaryKeywords);
	_secondaryKeywords = addLabeledInput(
		inner,
		"Secondary Keywords (content match; comma-sep)",
		"breaking,alert",
		_loaded.secondaryKeywords);

	_caseSensitiveKeywords = addToggleRow(inner, "Keywords Case Sensitive", _loaded.caseSensitiveKeywords);

	_wordsToRemove = addLabeledInput(
		inner,
		"Words to Remove (comma-sep)",
		"unsubscribe,bot",
		_loaded.wordsToRemove);

	_removalCaseSensitive = addToggleRow(inner, "Removal Case Sensitive", _loaded.removalCaseSensitive);
	_removeWholeWordOnly = addToggleRow(inner, "Remove Whole Word Only", _loaded.removeWholeWordOnly);

	_removePatterns = addLabeledInput(
		inner,
		"Remove Patterns (start:end,start:end)",
		"[link]:[/link]",
		_loaded.removePatterns,
		true /*multiline*/);
	addHint(inner, "Pairs of markers stripped from captions, e.g. <b>:</b> removes HTML-like tags.");
}

// ============================================================================
// Section: Dispatch
// ============================================================================

void ForwarderBox::buildDispatch(Ui::VerticalLayout *inner) {
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, rpl::single(QString("Dispatch")));

	// Forwarding Mode — native choice row, reactive label.
	_forwardingModeRow = Settings::AddButtonWithLabel(
		inner,
		rpl::single(QString("Forwarding Mode")),
		_forwardingModeValue.value() | rpl::map(modeRawToLabel),
		st::settingsButtonNoIcon);
	_forwardingModeRow->addClickHandler([=] {
		_menu = base::make_unique_q<Ui::PopupMenu>(_forwardingModeRow, st::defaultPopupMenu);
		const auto options = kModeOptions();
		for (int i = 0; i < options.size(); i++) {
			_menu->addAction(options[i], [=] {
				_forwardingModeValue = kModeRawValues()[i];
			});
		}
		_menu->popup(QCursor::pos());
	});

	_dropCaptions = addToggleRow(inner, "Drop Captions (Native Mode)", _loaded.dropCaptions);
	_duSendIndividually = addToggleRow(inner, "Send Download/Upload Albums Individually", _loaded.duSendIndividually);
	_splitDocAlbums = addToggleRow(inner, "Split Document Albums", _loaded.splitDocAlbums);
	_inlineButtonsToText = addToggleRow(inner, "Convert Inline Buttons to Text", _loaded.inlineButtonsToText);
	_filenameAsCaption = addToggleRow(inner, "Filename as Caption (if empty)", _loaded.filenameAsCaption);

	addHint(inner, "Native keeps authorship hidden; Copy/Download re-send as new media.");
}

// ============================================================================
// Section: Concurrency & Timings
// ============================================================================

void ForwarderBox::buildTimings(Ui::VerticalLayout *inner) {
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, rpl::single(QString("Concurrency & Timings")));

	_delaySeconds = addLabeledInput(inner, "Delay Seconds (between messages)", "1.0", _loaded.delaySeconds);
	_batchSize = addLabeledInput(inner, "Message Limit (0 = unlimited)", "0", _loaded.batchSize);
	_fetchCycleDelay = addLabeledInput(inner, "Fetch Cycle Delay Seconds", "0.0", _loaded.fetchCycleDelay);
	_concurrentBatchSize = addLabeledInput(inner, "Concurrent Batch Size", "1", _loaded.concurrentBatchSize);
	addHint(inner, "Higher concurrency speeds up Download & Upload mode but uses more bandwidth.");
}

// ============================================================================
// Section: Advanced / Resilience (collapsible)
// ============================================================================

void ForwarderBox::buildAdvanced(Ui::VerticalLayout *inner) {
	Ui::AddSkip(inner);

	const auto wrap = inner->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inner,
			object_ptr<Ui::VerticalLayout>(inner)))
		->setDuration(0);
	const auto advanced = wrap->entity();

	Ui::AddSubsectionTitle(advanced, rpl::single(QString("Advanced / Resilience")));

	_retryAttempts = addLabeledInput(advanced, "Retry Attempts", "2", _loaded.retryAttempts);
	_retryDelay = addLabeledInput(advanced, "Retry Delay Seconds", "5.0", _loaded.retryDelay);
	_failureThreshold = addLabeledInput(advanced, "Failure Threshold (0 = disabled)", "10", _loaded.failureThreshold);
	_iterChunkWait = addLabeledInput(advanced, "Iter Messages Chunk Wait Time", "1.0", _loaded.iterChunkWait);
	addHint(advanced, "Controls how the engine recovers from transient MTP/network failures.");

	// Collapse toggle: clicking the row toggles advanced-section visibility.
	auto textStream = inner->lifetime().make_state<rpl::event_stream<QString>>();
	const auto toggle = Settings::AddButtonWithIcon(
		inner,
		rpl::single(u"Show Advanced"_q) | rpl::then(textStream->events()),
		st::settingsButtonNoIcon);
	toggle->toggleOn(rpl::single(false));
	wrap->toggleOn(toggle->toggledValue());
	toggle->toggledValue()
		| rpl::on_next([=](bool shown) {
			textStream->fire(shown ? u"Hide Advanced"_q : u"Show Advanced"_q);
		}, inner->lifetime());
}

// ============================================================================
// Section: Execution Overrides
// ============================================================================

void ForwarderBox::buildExecutionOverrides(Ui::VerticalLayout *inner) {
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, rpl::single(QString("Execution Overrides")));

	_startMessageId = addLabeledInput(inner, "Start Message ID (0 = newest)", "0", _loaded.startMessageId);
	_endMessageId = addLabeledInput(inner, "End Message ID (0 = oldest)", "0", _loaded.endMessageId);
	_startMessage = addLabeledInput(
		inner,
		"Start Message Text Hook",
		"\xf0\x9f\x9a\x80 Forwarding started",
		_loaded.startMessage,
		true /*multiline*/);
	_endMessage = addLabeledInput(
		inner,
		"End Message Text Hook",
		"\xe2\x9c\x85 Forwarding complete",
		_loaded.endMessage,
		true /*multiline*/);

	_pinStartMessage = addToggleRow(inner, "Pin Start Message", _loaded.pinStartMessage);

	addHint(inner, "Hooks are posted to the destination at the start/end of a run.");
}

// ============================================================================
// Section: Logging
// ============================================================================

void ForwarderBox::buildLogging(Ui::VerticalLayout *inner) {
	Ui::AddSkip(inner);
	Ui::AddSubsectionTitle(inner, rpl::single(QString("Logging")));

	_logLevelRow = Settings::AddButtonWithLabel(
		inner,
		rpl::single(QString("Log Level")),
		_logLevelValue.value(),
		st::settingsButtonNoIcon);
	_logLevelRow->addClickHandler([=] {
		_menu = base::make_unique_q<Ui::PopupMenu>(_logLevelRow, st::defaultPopupMenu);
		const auto options = kLogLevelOptions();
		for (int i = 0; i < options.size(); i++) {
			_menu->addAction(options[i], [=] {
				_logLevelValue = options[i];
			});
		}
		_menu->popup(QCursor::pos());
	});
	addHint(inner, "Written to forwarder.log in the working directory.");
}

// ============================================================================
// Config persistence
// ============================================================================

void ForwarderBox::loadFromConfig() {
	const auto path = configPath();
	if (!QFileInfo::exists(path)) return;

	QSettings cfg(path, QSettings::IniFormat);

	auto get = [&](const QString &key) {
		return cfg.value(key).toString().trimmed();
	};

	// --- Channel Bindings ---
	_loaded.sourceIds = get("Channels/source_channels");
	_loaded.sourceTopicIds = get("Channels/source_topic_ids");
	_loaded.destId = get("Channels/destination");
	_loaded.destTopicId = get("Channels/destination_topic_id");

	// --- Message Filtering ---
	_loaded.allowedTypes = get("Forwarding/allowed_message_types");
	_loaded.primaryKeywords = get("Forwarding/document_caption_must_contain");
	_loaded.secondaryKeywords = get("Forwarding/secondary_content_keywords");
	_loaded.wordsToRemove = get("CaptionEditing/words_to_remove");
	_loaded.removePatterns = get("CaptionEditing/remove_patterns");
	_loaded.caseSensitiveKeywords = parseBool(cfg.value("Forwarding/keyword_case_sensitive", "false").toString());
	_loaded.removalCaseSensitive = parseBool(cfg.value("CaptionEditing/removal_case_sensitive", "false").toString());
	_loaded.removeWholeWordOnly = parseBool(cfg.value("CaptionEditing/remove_whole_word_only", "true").toString());

	// --- Dispatch mode (with download_then_upload backwards compat) ---
	auto mode = get("Forwarding/non_du_forwarding_mode");
	if (mode.isEmpty()) mode = u"native_drop_author"_q;
	if (parseBool(cfg.value("Forwarding/download_then_upload", "false").toString())) {
		mode = u"download_upload"_q;
	}
	_forwardingModeValue = mode;

	_loaded.dropCaptions = parseBool(cfg.value("Forwarding/native_forward_drop_captions", "false").toString());
	_loaded.duSendIndividually = parseBool(cfg.value("Forwarding/du_send_albums_individually", "false").toString());
	_loaded.splitDocAlbums = parseBool(cfg.value("Forwarding/split_document_albums", "true").toString());
	_loaded.inlineButtonsToText = parseBool(cfg.value("CaptionEditing/inline_buttons_to_text", "false").toString());
	_loaded.filenameAsCaption = parseBool(cfg.value("CaptionEditing/filename_as_caption", "false").toString());

	// --- Timings ---
	_loaded.delaySeconds = get("Forwarding/delay_seconds");
	_loaded.batchSize = get("Forwarding/message_limit");
	_loaded.fetchCycleDelay = get("Forwarding/fetch_cycle_delay_seconds");
	_loaded.concurrentBatchSize = get("Forwarding/concurrent_batch_size");

	// --- Advanced / Resilience ---
	_loaded.retryAttempts = get("Forwarding/retry_attempts");
	_loaded.retryDelay = get("Forwarding/retry_delay");
	_loaded.failureThreshold = get("Forwarding/failure_threshold");
	_loaded.iterChunkWait = get("Forwarding/iter_messages_chunk_wait_time");

	// --- Execution Overrides ---
	_loaded.startMessageId = get("Forwarding/start_message_id");
	_loaded.endMessageId = get("Forwarding/end_message_id");
	_loaded.startMessage = get("Hooks/start_message");
	_loaded.endMessage = get("Hooks/end_message");
	_loaded.pinStartMessage = parseBool(cfg.value("Hooks/pin_start_message", "false").toString());

	// --- Logging ---
	const auto lvl = get("Logging/log_level").toUpper();
	_logLevelValue = lvl.isEmpty() ? u"INFO"_q : lvl;
}

void ForwarderBox::saveToConfig() {
	const auto path = configPath();
	QSettings cfg(path, QSettings::IniFormat);

	// --- Channels ---
	cfg.setValue("Channels/source_channels", _sourceIds ? _sourceIds->getLastText().trimmed() : QString());
	cfg.setValue("Channels/source_topic_ids", _sourceTopicIds ? _sourceTopicIds->getLastText().trimmed() : QString());
	cfg.setValue("Channels/destination", _destId ? _destId->getLastText().trimmed() : QString());
	cfg.setValue("Channels/destination_topic_id", _destTopicId ? _destTopicId->getLastText().trimmed() : QString());

	// --- Message Filtering ---
	cfg.setValue("Forwarding/allowed_message_types", _allowedTypesInput ? _allowedTypesInput->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/document_caption_must_contain", _primaryKeywords ? _primaryKeywords->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/secondary_content_keywords", _secondaryKeywords ? _secondaryKeywords->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/keyword_case_sensitive", (_caseSensitiveKeywords && _caseSensitiveKeywords->toggled()) ? "true" : "false");
	cfg.setValue("CaptionEditing/words_to_remove", _wordsToRemove ? _wordsToRemove->getLastText().trimmed() : QString());
	cfg.setValue("CaptionEditing/remove_patterns", _removePatterns ? _removePatterns->getLastText().trimmed() : QString());
	cfg.setValue("CaptionEditing/removal_case_sensitive", (_removalCaseSensitive && _removalCaseSensitive->toggled()) ? "true" : "false");
	cfg.setValue("CaptionEditing/remove_whole_word_only", (_removeWholeWordOnly && _removeWholeWordOnly->toggled()) ? "true" : "false");

	// --- Dispatch mode (dual-write for backwards compatibility) ---
	const auto mode = _forwardingModeValue.current();
	cfg.setValue("Forwarding/non_du_forwarding_mode", mode);
	cfg.setValue("Forwarding/download_then_upload", (mode == u"download_upload"_q) ? "true" : "false");
	cfg.setValue("Forwarding/native_forward_drop_captions", (_dropCaptions && _dropCaptions->toggled()) ? "true" : "false");
	cfg.setValue("Forwarding/du_send_albums_individually", (_duSendIndividually && _duSendIndividually->toggled()) ? "true" : "false");
	cfg.setValue("Forwarding/split_document_albums", (_splitDocAlbums && _splitDocAlbums->toggled()) ? "true" : "false");
	cfg.setValue("CaptionEditing/inline_buttons_to_text", (_inlineButtonsToText && _inlineButtonsToText->toggled()) ? "true" : "false");
	cfg.setValue("CaptionEditing/filename_as_caption", (_filenameAsCaption && _filenameAsCaption->toggled()) ? "true" : "false");

	// --- Timings ---
	cfg.setValue("Forwarding/delay_seconds", _delaySeconds ? _delaySeconds->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/message_limit", _batchSize ? _batchSize->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/fetch_cycle_delay_seconds", _fetchCycleDelay ? _fetchCycleDelay->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/concurrent_batch_size", _concurrentBatchSize ? _concurrentBatchSize->getLastText().trimmed() : QString());

	// --- Advanced / Resilience ---
	cfg.setValue("Forwarding/retry_attempts", _retryAttempts ? _retryAttempts->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/retry_delay", _retryDelay ? _retryDelay->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/failure_threshold", _failureThreshold ? _failureThreshold->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/iter_messages_chunk_wait_time", _iterChunkWait ? _iterChunkWait->getLastText().trimmed() : QString());

	// --- Execution Overrides ---
	cfg.setValue("Forwarding/start_message_id", _startMessageId ? _startMessageId->getLastText().trimmed() : QString());
	cfg.setValue("Forwarding/end_message_id", _endMessageId ? _endMessageId->getLastText().trimmed() : QString());
	cfg.setValue("Hooks/start_message", _startMessage ? _startMessage->getLastText().trimmed() : QString());
	cfg.setValue("Hooks/end_message", _endMessage ? _endMessage->getLastText().trimmed() : QString());
	cfg.setValue("Hooks/pin_start_message", (_pinStartMessage && _pinStartMessage->toggled()) ? "true" : "false");

	// --- Logging ---
	const auto lvl = _logLevelValue.current().trimmed().toUpper();
	cfg.setValue("Logging/log_level", lvl.isEmpty() ? "INFO" : lvl);

	cfg.sync();
	_controller->showToast("Configuration saved.");
}

// ============================================================================
// Engine start
// ============================================================================

void ForwarderBox::startForwarding() {
	if (!_sourceIds || _sourceIds->getLastText().trimmed().isEmpty()) {
		if (_sourceIds) { _sourceIds->setFocus(); _sourceIds->showError(); }
		return;
	}
	if (!_destId || _destId->getLastText().trimmed().isEmpty()) {
		if (_destId) { _destId->setFocus(); _destId->showError(); }
		return;
	}

	saveToConfig();

	auto &fwState = ForwarderState::instance();
	fwState.setFilePath(statePath());
	fwState.load();
	auto &fwLog = ForwarderLog::instance();
	fwLog.setFilePath(logPath());
	fwLog.append("Config saved. Source: " + _sourceIds->getLastText().trimmed());

	const auto cfgPath = configPath();
	QSettings cfg(cfgPath, QSettings::IniFormat);

	ForwarderConfig engineConfig;
	engineConfig.session = &_controller->session();
	engineConfig.sourceIds = _sourceIds->getLastText().trimmed().split(',', Qt::SkipEmptyParts);
	for (auto &s : engineConfig.sourceIds) s = s.trimmed();
	engineConfig.sourceTopicIds = _sourceTopicIds->getLastText().trimmed().split(',', Qt::SkipEmptyParts);
	for (auto &s : engineConfig.sourceTopicIds) s = s.trimmed();
	engineConfig.destinationId = _destId->getLastText().trimmed();
	engineConfig.destinationTopicId = _destTopicId->getLastText().trimmed().toInt();

	const auto modeStr = _forwardingModeValue.current().toLower();
	if (modeStr == u"download_upload"_q) engineConfig.mode = ForwardingMode::DownloadUpload;
	else if (modeStr == u"copy"_q) engineConfig.mode = ForwardingMode::Copy;
	else engineConfig.mode = ForwardingMode::NativeDropAuthor;

	engineConfig.dropCaptions = _dropCaptions && _dropCaptions->toggled();
	engineConfig.splitDocAlbums = parseBool(cfg.value("Forwarding/split_document_albums", "true").toString());
	engineConfig.duSendIndividually = parseBool(cfg.value("Forwarding/du_send_albums_individually", "false").toString());

	const auto typesStr = _allowedTypesInput ? _allowedTypesInput->getLastText().trimmed() : QString();
	if (!typesStr.isEmpty()) {
		engineConfig.allowedTypes = typesStr.split(',', Qt::SkipEmptyParts);
		for (auto &t : engineConfig.allowedTypes) t = t.trimmed().toLower();
	}

	const auto kwStr = _primaryKeywords ? _primaryKeywords->getLastText().trimmed() : QString();
	if (!kwStr.isEmpty()) {
		engineConfig.primaryKeywords = kwStr.split(',', Qt::SkipEmptyParts);
		for (auto &k : engineConfig.primaryKeywords) k = k.trimmed();
	}

	const auto secKwStr = _secondaryKeywords ? _secondaryKeywords->getLastText().trimmed() : QString();
	if (!secKwStr.isEmpty()) {
		engineConfig.secondaryKeywords = secKwStr.split(',', Qt::SkipEmptyParts);
		for (auto &k : engineConfig.secondaryKeywords) k = k.trimmed();
	}
	engineConfig.keywordCaseSensitive = parseBool(cfg.value("Forwarding/keyword_case_sensitive", "false").toString());

	const auto wordsStr = _wordsToRemove ? _wordsToRemove->getLastText().trimmed() : QString();
	if (!wordsStr.isEmpty()) {
		engineConfig.wordsToRemove = wordsStr.split(',', Qt::SkipEmptyParts);
		for (auto &w : engineConfig.wordsToRemove) w = w.trimmed();
	}
	engineConfig.removalCaseSensitive = parseBool(cfg.value("CaptionEditing/removal_case_sensitive", "false").toString());
	engineConfig.removeWholeWordOnly = parseBool(cfg.value("CaptionEditing/remove_whole_word_only", "true").toString());

	engineConfig.delaySeconds = cfg.value("Forwarding/delay_seconds", 1.0).toFloat();
	engineConfig.failureThreshold = cfg.value("Forwarding/failure_threshold", 10).toInt();
	engineConfig.retryAttempts = cfg.value("Forwarding/retry_attempts", 2).toInt();
	engineConfig.retryDelay = cfg.value("Forwarding/retry_delay", 5.0).toFloat();
	engineConfig.messageLimit = cfg.value("Forwarding/message_limit", 0).toInt();
	engineConfig.fetchCycleDelaySeconds = cfg.value("Forwarding/fetch_cycle_delay_seconds", 0.0f).toFloat();
	engineConfig.iterMessagesChunkWaitTime = cfg.value("Forwarding/iter_messages_chunk_wait_time", 1.0f).toFloat();
	engineConfig.startMessageId = cfg.value("Forwarding/start_message_id", 0).toInt();
	engineConfig.endMessageId = cfg.value("Forwarding/end_message_id", 0).toInt();

	engineConfig.startMessage = _startMessage ? _startMessage->getLastText().trimmed() : QString();
	engineConfig.endMessage = _endMessage ? _endMessage->getLastText().trimmed() : QString();
	engineConfig.pinStartMessage = _pinStartMessage && _pinStartMessage->toggled();
	engineConfig.inlineButtonsToText = _inlineButtonsToText && _inlineButtonsToText->toggled();
	engineConfig.filenameAsCaption = _filenameAsCaption && _filenameAsCaption->toggled();

	const auto concBatch = _concurrentBatchSize ? _concurrentBatchSize->getLastText().trimmed().toInt() : 1;
	engineConfig.concurrentBatchSize = concBatch > 0 ? concBatch : 1;

	// Parse remove_patterns: "start1:end1,start2:end2" or separated by newlines.
	const auto rpStr = _removePatterns ? _removePatterns->getLastText().trimmed() : QString();
	if (!rpStr.isEmpty()) {
		auto normalizedRpStr = rpStr;
		normalizedRpStr.replace('\n', ',');
		const auto pairs = normalizedRpStr.split(',', Qt::SkipEmptyParts);
		for (const auto &pair : pairs) {
			const auto parts = pair.split(':', Qt::SkipEmptyParts);
			if (parts.size() == 2) {
				engineConfig.removePatterns.append({ parts[0].trimmed(), parts[1].trimmed() });
			}
		}
	}

	engineConfig.logLevel = _logLevelValue.current().trimmed().toUpper();
	if (engineConfig.logLevel.isEmpty()) {
		engineConfig.logLevel = u"INFO"_q;
	}

	// Guard against starting a second engine.
	if (AyuForward::g_isForwarderRunning) {
		if (AyuForward::g_isForwarderCanceled) {
			for (int i = 0; i < 20; ++i) {
				QThread::msleep(100);
				QCoreApplication::processEvents();
				if (!AyuForward::g_isForwarderRunning) break;
			}
		}
		if (AyuForward::g_isForwarderRunning) {
			_controller->showToast("Engine is already running! Stop it first.");
			return;
		}
	}

	auto *engine = new ForwarderEngine(engineConfig);
	QObject::connect(engine, &ForwarderEngine::finished, engine, &QObject::deleteLater);
	engine->start();
	_controller->showToast("Auto-Forwarder pipeline initialized!");
	closeBox();
}

} // namespace AyuForwarder
