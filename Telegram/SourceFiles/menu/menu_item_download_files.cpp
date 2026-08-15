/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_download_files.h"

#include "base/base_file_utilities.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_download_manager.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h" // HistoryView::SelectedItem.
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QRegularExpression>

namespace Menu {
namespace {

using Documents = std::vector<std::pair<not_null<DocumentData*>, FullMsgId>>;
using Photos = std::vector<std::pair<not_null<PhotoData*>, FullMsgId>>;

// Returns the next available sequential number for a download folder.
// Scans for existing files matching "N- *" pattern and returns max(N) + 1.
[[nodiscard]] int nextSequentialNumber(const QString &folderPath) {
	QDir dir(folderPath);
	if (!dir.exists()) return 1;

	int maxNum = 0;
	static const QRegularExpression re(u"^(\\d+)\\- "_q);
	const auto entries = dir.entryList(QDir::Files);
	for (const auto &entry : entries) {
		const auto match = re.match(entry);
		if (match.hasMatch()) {
			const int num = match.captured(1).toInt();
			if (num > maxNum) maxNum = num;
		}
	}
	return maxNum + 1;
}

// Builds a numbered filename: "N- originalName"
// Falls back to "N- file_N.ext" when the document has no filename (e.g., voice
// messages, unnamed media). The extension is derived from the MIME type so
// that unnamed files still get their correct format extension.
// Without the fallback, Windows rejects paths with trailing
// spaces or dots, triggering FileWriteFailure that permanently resets
// the user's custom download directory to the default.
[[nodiscard]] QString numberedName(
		int number,
		const QString &originalName,
		const QString &mimeString = QString()) {
	if (!originalName.isEmpty()) {
		return QString("%1- %2").arg(number).arg(originalName);
	}
	// Derive extension from MIME type (e.g. "video/mp4" -> ".mp4").
	auto ext = QString();
	if (!mimeString.isEmpty()) {
		const auto patterns = Core::MimeTypeForName(mimeString).globPatterns();
		if (!patterns.isEmpty()) {
			ext = patterns.front();
			ext.replace('*', QString());
		}
	}
	const auto name = QString("file_%1%2").arg(number).arg(ext);
	return QString("%1- %2").arg(number).arg(name);
}

[[nodiscard]] bool Added(
		HistoryItem *item,
		Documents &documents,
		Photos &photos) {
	if (item && !item->forbidsForward()) {
		if (const auto media = item->media()) {
			const bool isForum = (item->topicRootId() != 0);
			if (const auto photo = media->photo()) {
				photos.emplace_back(photo, item->fullId());
				return true;
			} else if (const auto document = media->document()) {
				if (isForum && document->sticker()) return false;
				documents.emplace_back(document, item->fullId());
				return true;
			}
		}
	}
	return false;
}

void AddAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		Documents &&documents,
		Photos &&photos,
		Fn<void()> callback) {
	const auto text = documents.empty()
		? tr::lng_context_save_images_selected(tr::now)
		: tr::lng_context_save_documents_selected(tr::now);
	const auto icon = documents.empty()
		? &st::menuIconSaveImage
		: &st::menuIconDownload;
	const auto shouldShowToast = !photos.empty();

	const auto weak = base::make_weak(controller);
	const auto saveImages = [=](const QString &folderPath, int startNum) {
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		const auto session = &controller->session();
		const auto path = folderPath;

		const auto showToast = !shouldShowToast
			? Fn<void(const QString &)>(nullptr)
			: [=](const QString &lastPath) {
				const auto filter = [lastPath](const auto ...) {
					File::ShowInFolder(lastPath);
					return false;
				};
				controller->showToast({
					.text = (photos.size() > 1
							? tr::lng_mediaview_saved_images_to
							: tr::lng_mediaview_saved_to)(
						tr::now,
						lt_downloads,
						tr::link(
							tr::lng_mediaview_downloads(tr::now),
							"internal:show_saved_message"),
						tr::marked),
					.filter = filter,
					.iconLottie = u"toast/save_to_gallery"_q,
					.iconLottieSize = st::toastLottieIconSize,
					.st = &st::defaultToast,
				});
			};

		auto views = std::vector<std::shared_ptr<Data::PhotoMedia>>();
		auto dates = std::vector<TimeId>();
		for (const auto &[photo, fullId] : photos) {
			if (const auto view = photo->createMediaView()) {
				view->wanted(Data::PhotoSize::Large, fullId);
				views.push_back(view);
				const auto photoDate = photo->date();
				const auto item = session->data().message(fullId);
				dates.push_back(photoDate
					? photoDate
					: (item ? item->date() : TimeId(0)));
			}
		}

		const auto finalCheck = [=] {
			for (const auto &view : views) {
				if (!view->loaded()) {
					return false;
				}
			}
			return true;
		};

		const auto saveToFiles = [=] {
			int seqNum = startNum;
			auto lastPath = QString();
			for (auto i = 0; i < views.size(); i++) {
				const auto photoName = u"photo_"_q
					+ QString::number(i + 1)
					+ u".jpg"_q;
				lastPath = path + numberedName(seqNum++, photoName);
				if (views[i]->saveToFile(lastPath) && dates[i] > 0) {
					auto f = QFile(lastPath);
					if (f.open(QIODevice::ReadWrite)) {
						const auto when = base::unixtime::parse(dates[i]);
						f.setFileTime(
							when,
							QFileDevice::FileModificationTime);
						f.setFileTime(
							when,
							QFileDevice::FileAccessTime);
					}
				}
			}
			if (showToast) {
				showToast(lastPath);
			}
		};

		if (finalCheck()) {
			saveToFiles();
		} else {
			auto lifetime = std::make_shared<rpl::lifetime>();
			session->downloaderTaskFinished(
			) | rpl::on_next([=]() mutable {
				if (finalCheck()) {
					saveToFiles();
					base::take(lifetime)->destroy();
				}
			}, *lifetime);
		}
	};
	const auto saveDocuments = [=](const QString &folderPath, int startNum) {
		int seqNum = startNum;
		crl::time delayMs = 0;
		for (const auto &[document, origin] : documents) {
			if (!folderPath.isEmpty()) {
				const auto name = numberedName(seqNum++, document->filename(), document->mimeString());
				const auto path = folderPath + name;
				const auto doc = document;
				const auto orig = origin;
				// Stagger downloads to prevent overwhelming the MTP layer.
				// Firing 50-100 save() calls synchronously causes random
				// CHANNEL_INVALID failures because the download manager
				// can't queue them all fast enough.
				base::call_delayed(delayMs, weak, [=] {
					if (doc->loading()) {
						doc->cancel();
					}
					doc->save(orig, path);
					if (doc->loading() && !doc->loadingFilePath().isEmpty()) {
						if (const auto item = doc->owner().message(orig)) {
							Core::App().downloadManager().addLoading({
								.item = item,
								.document = doc,
							});
						}
					}
				});
				delayMs += 100;
			} else {
				DocumentSaveClickHandler::SaveAndTrack(origin, document);
			}
		}
	};

	menu->addAction(text, [=] {
		const auto save = [=](const QString &folderPath) {
			const auto controller = weak.get();
			if (!controller) {
				return;
			}
			auto path = folderPath;
			if (path.isEmpty()) {
				const auto session = &controller->session();
				auto downloadPath = Core::App().settings().downloadPath();
				path = downloadPath.isEmpty()
					? File::DefaultDownloadPath(session)
					: (downloadPath == FileDialog::Tmp())
					? session->local().tempDirectory()
					: downloadPath;
			}
			if (path.isEmpty()) {
				return;
			}
			QDir().mkpath(path);

			// Compute starting number once to avoid overlap between
			// photos and documents when both are being saved.
			const auto startNum = nextSequentialNumber(path);
			saveImages(path, startNum);
			saveDocuments(path, startNum + static_cast<int>(photos.size()));
			callback();
		};
		const auto controller = weak.get();
		if (!controller) {
			return;
		}
		if (Core::App().settings().askDownloadPath()) {
			const auto initialPath = [] {
				const auto path = Core::App().settings().downloadPath();
				if (!path.isEmpty() && path != FileDialog::Tmp()) {
					return path.left(path.size()
						- (path.endsWith('/') ? 1 : 0));
				}
				return QString();
			}();
			const auto handleFolder = [=](const QString &result) {
				if (!result.isEmpty()) {
					const auto folderPath = result.endsWith('/')
						? result
						: (result + '/');
					save(folderPath);
				}
			};
			FileDialog::GetFolder(
				controller->window().widget().get(),
				tr::lng_download_path_choose(tr::now),
				initialPath,
				handleFolder);
		} else {
			save(QString());
		}
	}, icon);
}

} // namespace

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<HistoryView::SelectedItem> &selectedItems,
		not_null<HistoryView::ListWidget*> list) {
	if (selectedItems.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &selectedItem : selectedItems) {
		const auto &id = selectedItem.msgId;
		const auto item = window->session().data().message(id);
		Added(item, docs, photos);
	}
	if (docs.empty() && photos.empty()) {
		return;
	}
       std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) {
               return a.second < b.second;
       });
       std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) {
               return a.second < b.second;
       });
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->cancelSelection();
		}
	};
	AddAction(menu, window, std::move(docs), std::move(photos), done);
}

void AddDownloadFilesAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> window,
		const std::vector<not_null<HistoryItem*>> &items,
		not_null<HistoryInner*> list) {
	if (items.empty()) {
		return;
	}
	auto docs = Documents();
	auto photos = Photos();
	for (const auto &item : items) {
		if (!Added(item, docs, photos)) {
			return;
		}
	}
	
	if (docs.empty() && photos.empty()) {
		return;
	}
       std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) {
               return a.second < b.second;
       });
       std::sort(photos.begin(), photos.end(), [](const auto &a, const auto &b) {
               return a.second < b.second;
       });
	const auto done = [weak = base::make_weak(list)] {
		if (const auto strong = weak.get()) {
			strong->clearSelected();
		}
	};
	AddAction(menu, window, std::move(docs), std::move(photos), done);
}

} // namespace Menu
