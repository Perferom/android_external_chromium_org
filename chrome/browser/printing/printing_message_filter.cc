// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/printing/printing_message_filter.h"

#include <string>

#include "base/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/printing/printer_query.h"
#include "chrome/browser/printing/print_job_manager.h"
#include "chrome/browser/printing/printing_ui_web_contents_observer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_io_data.h"
#include "chrome/common/print_messages.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"

#if defined(ENABLE_FULL_PRINTING)
#include "chrome/browser/ui/webui/print_preview/print_preview_ui.h"
#endif

#if defined(OS_CHROMEOS)
#include <fcntl.h>

#include <map>

#include "base/file_util.h"
#include "base/lazy_instance.h"
#include "chrome/browser/printing/print_dialog_cloud.h"
#include "content/public/browser/web_contents_view.h"
#endif

#if defined(OS_ANDROID)
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/printing/print_view_manager_basic.h"
#include "printing/printing_context_android.h"
#endif

using content::BrowserThread;

namespace {

#if defined(OS_CHROMEOS)
typedef std::map<int, base::FilePath> SequenceToPathMap;

struct PrintingSequencePathMap {
  SequenceToPathMap map;
  int sequence;
};

// No locking, only access on the FILE thread.
static base::LazyInstance<PrintingSequencePathMap>
    g_printing_file_descriptor_map = LAZY_INSTANCE_INITIALIZER;
#endif

void RenderParamsFromPrintSettings(const printing::PrintSettings& settings,
                                   PrintMsg_Print_Params* params) {
  params->page_size = settings.page_setup_device_units().physical_size();
  params->content_size.SetSize(
      settings.page_setup_device_units().content_area().width(),
      settings.page_setup_device_units().content_area().height());
  params->printable_area.SetRect(
      settings.page_setup_device_units().printable_area().x(),
      settings.page_setup_device_units().printable_area().y(),
      settings.page_setup_device_units().printable_area().width(),
      settings.page_setup_device_units().printable_area().height());
  params->margin_top = settings.page_setup_device_units().content_area().y();
  params->margin_left = settings.page_setup_device_units().content_area().x();
  params->dpi = settings.dpi();
  // Currently hardcoded at 1.25. See PrintSettings' constructor.
  params->min_shrink = settings.min_shrink();
  // Currently hardcoded at 2.0. See PrintSettings' constructor.
  params->max_shrink = settings.max_shrink();
  // Currently hardcoded at 72dpi. See PrintSettings' constructor.
  params->desired_dpi = settings.desired_dpi();
  // Always use an invalid cookie.
  params->document_cookie = 0;
  params->selection_only = settings.selection_only();
  params->supports_alpha_blend = settings.supports_alpha_blend();
  params->should_print_backgrounds = settings.should_print_backgrounds();
  params->display_header_footer = settings.display_header_footer();
  params->title = settings.title();
  params->url = settings.url();
}

}  // namespace

PrintingMessageFilter::PrintingMessageFilter(int render_process_id,
                                             Profile* profile)
    : profile_io_data_(ProfileIOData::FromResourceContext(
          profile->GetResourceContext())),
      render_process_id_(render_process_id),
      queue_(g_browser_process->print_job_manager()->queue()) {
  DCHECK(queue_);
}

PrintingMessageFilter::~PrintingMessageFilter() {
}

void PrintingMessageFilter::OverrideThreadForMessage(
    const IPC::Message& message, BrowserThread::ID* thread) {
#if defined(OS_CHROMEOS)
  if (message.type() == PrintHostMsg_AllocateTempFileForPrinting::ID ||
      message.type() == PrintHostMsg_TempFileForPrintingWritten::ID) {
    *thread = BrowserThread::FILE;
  }
#elif defined(OS_ANDROID)
  if (message.type() == PrintHostMsg_AllocateTempFileForPrinting::ID ||
      message.type() == PrintHostMsg_TempFileForPrintingWritten::ID) {
    *thread = BrowserThread::UI;
  }
#endif
}

bool PrintingMessageFilter::OnMessageReceived(const IPC::Message& message,
                                              bool* message_was_ok) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_EX(PrintingMessageFilter, message, *message_was_ok)
#if defined(OS_WIN)
    IPC_MESSAGE_HANDLER(PrintHostMsg_DuplicateSection, OnDuplicateSection)
#endif
#if defined(OS_CHROMEOS) || defined(OS_ANDROID)
    IPC_MESSAGE_HANDLER(PrintHostMsg_AllocateTempFileForPrinting,
                        OnAllocateTempFileForPrinting)
    IPC_MESSAGE_HANDLER(PrintHostMsg_TempFileForPrintingWritten,
                        OnTempFileForPrintingWritten)
#endif
    IPC_MESSAGE_HANDLER(PrintHostMsg_IsPrintingEnabled, OnIsPrintingEnabled)
    IPC_MESSAGE_HANDLER_DELAY_REPLY(PrintHostMsg_GetDefaultPrintSettings,
                                    OnGetDefaultPrintSettings)
    IPC_MESSAGE_HANDLER_DELAY_REPLY(PrintHostMsg_ScriptedPrint, OnScriptedPrint)
    IPC_MESSAGE_HANDLER_DELAY_REPLY(PrintHostMsg_UpdatePrintSettings,
                                    OnUpdatePrintSettings)
#if defined(ENABLE_FULL_PRINTING)
    IPC_MESSAGE_HANDLER(PrintHostMsg_CheckForCancel, OnCheckForCancel)
#endif
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

#if defined(OS_WIN)
void PrintingMessageFilter::OnDuplicateSection(
    base::SharedMemoryHandle renderer_handle,
    base::SharedMemoryHandle* browser_handle) {
  // Duplicate the handle in this process right now so the memory is kept alive
  // (even if it is not mapped)
  base::SharedMemory shared_buf(renderer_handle, true, PeerHandle());
  shared_buf.GiveToProcess(base::GetCurrentProcessHandle(), browser_handle);
}
#endif

#if defined(OS_CHROMEOS) || defined(OS_ANDROID)
void PrintingMessageFilter::OnAllocateTempFileForPrinting(
    int render_view_id,
    base::FileDescriptor* temp_file_fd,
    int* sequence_number) {
#if defined(OS_CHROMEOS)
  // TODO(thestig): Use |render_view_id| for Chrome OS.
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::FILE));
  temp_file_fd->fd = *sequence_number = -1;
  temp_file_fd->auto_close = false;

  SequenceToPathMap* map = &g_printing_file_descriptor_map.Get().map;
  *sequence_number = g_printing_file_descriptor_map.Get().sequence++;

  base::FilePath path;
  if (base::CreateTemporaryFile(&path)) {
    int fd = open(path.value().c_str(), O_WRONLY);
    if (fd >= 0) {
      SequenceToPathMap::iterator it = map->find(*sequence_number);
      if (it != map->end()) {
        NOTREACHED() << "Sequence number already in use. seq=" <<
            *sequence_number;
      } else {
        (*map)[*sequence_number] = path;
        temp_file_fd->fd = fd;
        temp_file_fd->auto_close = true;
      }
    }
  }
#elif defined(OS_ANDROID)
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::WebContents* wc = GetWebContentsForRenderView(render_view_id);
  if (!wc)
    return;
  printing::PrintViewManagerBasic* print_view_manager =
      printing::PrintViewManagerBasic::FromWebContents(wc);
  // The file descriptor is originally created in & passed from the Android
  // side, and it will handle the closing.
  const base::FileDescriptor& file_descriptor =
      print_view_manager->file_descriptor();
  temp_file_fd->fd = file_descriptor.fd;
  temp_file_fd->auto_close = false;
#endif
}

void PrintingMessageFilter::OnTempFileForPrintingWritten(int render_view_id,
                                                         int sequence_number) {
#if defined(OS_CHROMEOS)
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::FILE));
  SequenceToPathMap* map = &g_printing_file_descriptor_map.Get().map;
  SequenceToPathMap::iterator it = map->find(sequence_number);
  if (it == map->end()) {
    NOTREACHED() << "Got a sequence that we didn't pass to the "
                    "renderer: " << sequence_number;
    return;
  }
  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::Bind(&PrintingMessageFilter::CreatePrintDialogForFile,
                 this, render_view_id, it->second));

  // Erase the entry in the map.
  map->erase(it);
#elif defined(OS_ANDROID)
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::WebContents* wc = GetWebContentsForRenderView(render_view_id);
  if (!wc)
    return;
  printing::PrintViewManagerBasic* print_view_manager =
      printing::PrintViewManagerBasic::FromWebContents(wc);
  const base::FileDescriptor& file_descriptor =
      print_view_manager->file_descriptor();
  printing::PrintingContextAndroid::PdfWritingDone(file_descriptor.fd, true);
  // Invalidate the file descriptor so it doesn't accidentally get reused.
  print_view_manager->set_file_descriptor(base::FileDescriptor(-1, false));
#endif
}
#endif  // defined(OS_CHROMEOS) || defined(OS_ANDROID)

#if defined(OS_CHROMEOS)
void PrintingMessageFilter::CreatePrintDialogForFile(
    int render_view_id,
    const base::FilePath& path) {
  content::WebContents* wc = GetWebContentsForRenderView(render_view_id);
  if (!wc)
    return;
  print_dialog_cloud::CreatePrintDialogForFile(
      wc->GetBrowserContext(),
      wc->GetView()->GetTopLevelNativeWindow(),
      path,
      wc->GetTitle(),
      base::string16(),
      std::string("application/pdf"),
      false);
}
#endif  // defined(OS_CHROMEOS)

content::WebContents* PrintingMessageFilter::GetWebContentsForRenderView(
    int render_view_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::RenderViewHost* view = content::RenderViewHost::FromID(
      render_process_id_, render_view_id);
  return view ? content::WebContents::FromRenderViewHost(view) : NULL;
}

struct PrintingMessageFilter::GetPrintSettingsForRenderViewParams {
  printing::PrinterQuery::GetSettingsAskParam ask_user_for_settings;
  int expected_page_count;
  bool has_selection;
  printing::MarginType margin_type;
};

void PrintingMessageFilter::GetPrintSettingsForRenderView(
    int render_view_id,
    GetPrintSettingsForRenderViewParams params,
    const base::Closure& callback,
    scoped_refptr<printing::PrinterQuery> printer_query) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::WebContents* wc = GetWebContentsForRenderView(render_view_id);
  if (wc) {
    scoped_ptr<PrintingUIWebContentsObserver> wc_observer(
        new PrintingUIWebContentsObserver(wc));
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        base::Bind(&printing::PrinterQuery::GetSettings, printer_query,
                   params.ask_user_for_settings, base::Passed(&wc_observer),
                   params.expected_page_count, params.has_selection,
                   params.margin_type, callback));
  } else {
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        base::Bind(&PrintingMessageFilter::OnGetPrintSettingsFailed, this,
                   callback, printer_query));
  }
}

void PrintingMessageFilter::OnGetPrintSettingsFailed(
    const base::Closure& callback,
    scoped_refptr<printing::PrinterQuery> printer_query) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  printer_query->GetSettingsDone(printing::PrintSettings(),
                                 printing::PrintingContext::FAILED);
  callback.Run();
}

void PrintingMessageFilter::OnIsPrintingEnabled(bool* is_enabled) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  *is_enabled = profile_io_data_->printing_enabled()->GetValue();
}

void PrintingMessageFilter::OnGetDefaultPrintSettings(IPC::Message* reply_msg) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  scoped_refptr<printing::PrinterQuery> printer_query;
  if (!profile_io_data_->printing_enabled()->GetValue()) {
    // Reply with NULL query.
    OnGetDefaultPrintSettingsReply(printer_query, reply_msg);
    return;
  }
  printer_query = queue_->PopPrinterQuery(0);
  if (!printer_query)
    printer_query = queue_->CreatePrinterQuery();

  // Loads default settings. This is asynchronous, only the IPC message sender
  // will hang until the settings are retrieved.
  GetPrintSettingsForRenderViewParams params;
  params.ask_user_for_settings = printing::PrinterQuery::DEFAULTS;
  params.expected_page_count = 0;
  params.has_selection = false;
  params.margin_type = printing::DEFAULT_MARGINS;
  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::Bind(&PrintingMessageFilter::GetPrintSettingsForRenderView, this,
          reply_msg->routing_id(), params,
          base::Bind(&PrintingMessageFilter::OnGetDefaultPrintSettingsReply,
              this, printer_query, reply_msg),
          printer_query));
}

void PrintingMessageFilter::OnGetDefaultPrintSettingsReply(
    scoped_refptr<printing::PrinterQuery> printer_query,
    IPC::Message* reply_msg) {
  PrintMsg_Print_Params params;
  if (!printer_query.get() ||
      printer_query->last_status() != printing::PrintingContext::OK) {
    params.Reset();
  } else {
    RenderParamsFromPrintSettings(printer_query->settings(), &params);
    params.document_cookie = printer_query->cookie();
  }
  PrintHostMsg_GetDefaultPrintSettings::WriteReplyParams(reply_msg, params);
  Send(reply_msg);
  // If printing was enabled.
  if (printer_query.get()) {
    // If user hasn't cancelled.
    if (printer_query->cookie() && printer_query->settings().dpi()) {
      queue_->QueuePrinterQuery(printer_query.get());
    } else {
      printer_query->StopWorker();
    }
  }
}

void PrintingMessageFilter::OnScriptedPrint(
    const PrintHostMsg_ScriptedPrint_Params& params,
    IPC::Message* reply_msg) {
  scoped_refptr<printing::PrinterQuery> printer_query =
      queue_->PopPrinterQuery(params.cookie);
  if (!printer_query)
    printer_query = queue_->CreatePrinterQuery();
  GetPrintSettingsForRenderViewParams settings_params;
  settings_params.ask_user_for_settings = printing::PrinterQuery::ASK_USER;
  settings_params.expected_page_count = params.expected_pages_count;
  settings_params.has_selection = params.has_selection;
  settings_params.margin_type = params.margin_type;

  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::Bind(&PrintingMessageFilter::GetPrintSettingsForRenderView, this,
                 reply_msg->routing_id(), settings_params,
                 base::Bind(&PrintingMessageFilter::OnScriptedPrintReply, this,
                            printer_query, reply_msg),
                 printer_query));
}

void PrintingMessageFilter::OnScriptedPrintReply(
    scoped_refptr<printing::PrinterQuery> printer_query,
    IPC::Message* reply_msg) {
  PrintMsg_PrintPages_Params params;
#if defined(OS_ANDROID)
  // We need to save the routing ID here because Send method below deletes the
  // |reply_msg| before we can get the routing ID for the Android code.
  int routing_id = reply_msg->routing_id();
#endif
  if (printer_query->last_status() != printing::PrintingContext::OK ||
      !printer_query->settings().dpi()) {
    params.Reset();
  } else {
    RenderParamsFromPrintSettings(printer_query->settings(), &params.params);
    params.params.document_cookie = printer_query->cookie();
    params.pages =
        printing::PageRange::GetPages(printer_query->settings().ranges());
  }
  PrintHostMsg_ScriptedPrint::WriteReplyParams(reply_msg, params);
  Send(reply_msg);
  if (params.params.dpi && params.params.document_cookie) {
#if defined(OS_ANDROID)
    int file_descriptor;
    const base::string16& device_name = printer_query->settings().device_name();
    if (base::StringToInt(device_name, &file_descriptor)) {
      BrowserThread::PostTask(
          BrowserThread::UI, FROM_HERE,
          base::Bind(&PrintingMessageFilter::UpdateFileDescriptor, this,
                     routing_id, file_descriptor));
    }
#endif
    queue_->QueuePrinterQuery(printer_query.get());
  } else {
    printer_query->StopWorker();
  }
}

#if defined(OS_ANDROID)
void PrintingMessageFilter::UpdateFileDescriptor(int render_view_id, int fd) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::WebContents* wc = GetWebContentsForRenderView(render_view_id);
  if (!wc)
    return;
  printing::PrintViewManagerBasic* print_view_manager =
      printing::PrintViewManagerBasic::FromWebContents(wc);
  print_view_manager->set_file_descriptor(base::FileDescriptor(fd, false));
}
#endif

void PrintingMessageFilter::OnUpdatePrintSettings(
    int document_cookie, const DictionaryValue& job_settings,
    IPC::Message* reply_msg) {
  scoped_refptr<printing::PrinterQuery> printer_query;
  if (!profile_io_data_->printing_enabled()->GetValue()) {
    // Reply with NULL query.
    OnUpdatePrintSettingsReply(printer_query, reply_msg);
    return;
  }
  printer_query = queue_->PopPrinterQuery(document_cookie);
  if (!printer_query)
    printer_query = queue_->CreatePrinterQuery();
  printer_query->SetSettings(
      job_settings,
      base::Bind(&PrintingMessageFilter::OnUpdatePrintSettingsReply, this,
                 printer_query, reply_msg));
}

void PrintingMessageFilter::OnUpdatePrintSettingsReply(
    scoped_refptr<printing::PrinterQuery> printer_query,
    IPC::Message* reply_msg) {
  PrintMsg_PrintPages_Params params;
  if (!printer_query.get() ||
      printer_query->last_status() != printing::PrintingContext::OK) {
    params.Reset();
  } else {
    RenderParamsFromPrintSettings(printer_query->settings(), &params.params);
    params.params.document_cookie = printer_query->cookie();
    params.pages =
        printing::PageRange::GetPages(printer_query->settings().ranges());
  }
  PrintHostMsg_UpdatePrintSettings::WriteReplyParams(reply_msg, params);
  Send(reply_msg);
  // If user hasn't cancelled.
  if (printer_query.get()) {
    if (printer_query->cookie() && printer_query->settings().dpi()) {
      queue_->QueuePrinterQuery(printer_query.get());
    } else {
      printer_query->StopWorker();
    }
  }
}

#if defined(ENABLE_FULL_PRINTING)
void PrintingMessageFilter::OnCheckForCancel(int32 preview_ui_id,
                                             int preview_request_id,
                                             bool* cancel) {
  PrintPreviewUI::GetCurrentPrintPreviewStatus(preview_ui_id,
                                               preview_request_id,
                                               cancel);
}
#endif
