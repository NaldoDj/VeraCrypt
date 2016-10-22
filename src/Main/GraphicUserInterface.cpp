/*
 Derived from source code of TrueCrypt 7.1a, which is
 Copyright (c) 2008-2012 TrueCrypt Developers Association and which is governed
 by the TrueCrypt License 3.0.

 Modifications and additions to the original source code (contained in this file)
 and all other portions of this file are Copyright (c) 2013-2016 IDRIX
 and are governed by the Apache License 2.0 the full text of which is
 contained in the file License.txt included in VeraCrypt binary and source
 code distribution packages.
*/

#include "System.h"

#ifdef TC_UNIX
#include <wx/mimetype.h>
#include <wx/sckipc.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include "Platform/Unix/Process.h"
#endif

#include "Common/SecurityToken.h"
#include "Application.h"
#include "GraphicUserInterface.h"
#include "FatalErrorHandler.h"
#include "Forms/DeviceSelectionDialog.h"
#include "Forms/KeyfileGeneratorDialog.h"
#include "Forms/MainFrame.h"
#include "Forms/MountOptionsDialog.h"
#include "Forms/RandomPoolEnrichmentDialog.h"
#include "Forms/SecurityTokenKeyfilesDialog.h"

namespace VeraCrypt
{
	GraphicUserInterface::GraphicUserInterface () :
		ActiveFrame (nullptr),
		BackgroundMode (false),
		mMainFrame (nullptr),
		mWaitDialog (nullptr)
	{
#ifdef TC_UNIX
		signal (SIGHUP, OnSignal);
		signal (SIGINT, OnSignal);
		signal (SIGQUIT, OnSignal);
		signal (SIGTERM, OnSignal);
#endif

#ifdef TC_MACOSX
		wxApp::s_macHelpMenuTitleName = _("&Help");
#endif
	}

	GraphicUserInterface::~GraphicUserInterface ()
	{
		try
		{
			if (RandomNumberGenerator::IsRunning())
				RandomNumberGenerator::Stop();
		}
		catch (...) { }

		FatalErrorHandler::Deregister();

#ifdef TC_UNIX
		signal (SIGHUP, SIG_DFL);
		signal (SIGINT, SIG_DFL);
		signal (SIGQUIT, SIG_DFL);
		signal (SIGTERM, SIG_DFL);
#endif
	}

	void GraphicUserInterface::AppendToListCtrl (wxListCtrl *listCtrl, const vector <wstring> &itemFields, int imageIndex, void *itemDataPtr) const
	{
		InsertToListCtrl (listCtrl, listCtrl->GetItemCount(), itemFields, imageIndex, itemDataPtr);
	}

	wxMenuItem *GraphicUserInterface::AppendToMenu (wxMenu &menu, const wxString &label, wxEvtHandler *handler, wxObjectEventFunction handlerFunction, int itemId) const
	{
		wxMenuItem *item = new wxMenuItem (&menu, itemId, label);
		menu.Append (item);

		if (handler)
			handler->Connect (item->GetId(), wxEVT_COMMAND_MENU_SELECTED, handlerFunction);

		return item;
	}

	bool GraphicUserInterface::AskYesNo (const wxString &message, bool defaultYes, bool warning) const
	{
		return ShowMessage (message,
			wxYES_NO | (warning ? wxICON_EXCLAMATION : wxICON_QUESTION) | (defaultYes ? wxYES_DEFAULT : wxNO_DEFAULT)
			) == wxYES;
	}

	void GraphicUserInterface::AutoDismountVolumes (VolumeInfoList mountedVolumes, bool alwaysForce)
	{
		size_t mountedVolumeCount = Core->GetMountedVolumes().size();
		try
		{
			wxBusyCursor busy;
			DismountVolumes (mountedVolumes, alwaysForce ? true : GetPreferences().ForceAutoDismount, false);
		}
		catch (...) { }

		if (Core->GetMountedVolumes().size() < mountedVolumeCount)
			OnVolumesAutoDismounted();
	}

	void GraphicUserInterface::BackupVolumeHeaders (shared_ptr <VolumePath> volumePath) const
	{
		wxWindow *parent = GetActiveWindow();

		if (!volumePath || volumePath->IsEmpty())
			volumePath = make_shared <VolumePath> (SelectVolumeFile (GetActiveWindow()));

		if (volumePath->IsEmpty())
			throw UserAbort (SRC_POS);

#ifdef TC_WINDOWS
		if (Core->IsVolumeMounted (*volumePath))
		{
			ShowInfo ("DISMOUNT_FIRST");
			return;
		}
#endif

#ifdef TC_UNIX
		// Temporarily take ownership of a device if the user is not an administrator
		UserId origDeviceOwner ((uid_t) -1);

		if (!Core->HasAdminPrivileges() && volumePath->IsDevice())
		{
			origDeviceOwner = FilesystemPath (wstring (*volumePath)).GetOwner();
			Core->SetFileOwner (*volumePath, UserId (getuid()));
		}

		finally_do_arg2 (FilesystemPath, *volumePath, UserId, origDeviceOwner,
		{
			if (finally_arg2.SystemId != (uid_t) -1)
				Core->SetFileOwner (finally_arg, finally_arg2);
		});
#endif

		ShowInfo ("EXTERNAL_VOL_HEADER_BAK_FIRST_INFO");

		shared_ptr <Volume> normalVolume;
		shared_ptr <Volume> hiddenVolume;

		MountOptions normalVolumeMountOptions;
		MountOptions hiddenVolumeMountOptions;

		normalVolumeMountOptions.Path = volumePath;
		hiddenVolumeMountOptions.Path = volumePath;

		VolumeType::Enum volumeType = VolumeType::Normal;

		// Open both types of volumes
		while (true)
		{
			shared_ptr <Volume> volume;
			MountOptions *options = (volumeType == VolumeType::Hidden ? &hiddenVolumeMountOptions : &normalVolumeMountOptions);

			MountOptionsDialog dialog (parent, *options,
				LangString[volumeType == VolumeType::Hidden ? "ENTER_HIDDEN_VOL_PASSWORD" : "ENTER_NORMAL_VOL_PASSWORD"],
				true);

			while (!volume)
			{
				dialog.Hide();
				if (dialog.ShowModal() != wxID_OK)
					return;

				try
				{
					wxBusyCursor busy;
					OpenVolumeThreadRoutine routine(
						options->Path,
						options->PreserveTimestamps,
						options->Password,
						options->Pim,
						options->Kdf,
						false,
						options->Keyfiles,
						options->Protection,
						options->ProtectionPassword,
						options->ProtectionPim,
						options->ProtectionKdf,
						options->ProtectionKeyfiles,
						true,
						volumeType,
						options->UseBackupHeaders
						);

						ExecuteWaitThreadRoutine (parent, &routine);
						volume = routine.m_pVolume;
				}
				catch (PasswordException &e)
				{
					ShowWarning (e);
				}
			}

			if (volumeType == VolumeType::Hidden)
				hiddenVolume = volume;
			else
				normalVolume = volume;

			// Ask whether a hidden volume is present
			if (volumeType == VolumeType::Normal)
			{
				wxArrayString choices;
				choices.Add (LangString["VOLUME_CONTAINS_HIDDEN"]);
				choices.Add (LangString["VOLUME_DOES_NOT_CONTAIN_HIDDEN"]);

				wxSingleChoiceDialog choiceDialog (parent, LangString["DOES_VOLUME_CONTAIN_HIDDEN"], Application::GetName(), choices);
				choiceDialog.SetSize (wxSize (Gui->GetCharWidth (&choiceDialog) * 60, -1));
				choiceDialog.SetSelection (-1);

				if (choiceDialog.ShowModal() != wxID_OK)
					return;

				switch (choiceDialog.GetSelection())
				{
				case 0:
					volumeType = VolumeType::Hidden;
					continue;

				case 1:
					break;

				default:
					return;
				}
			}

			break;
		}

		if (hiddenVolume)
		{
			if (typeid (*normalVolume->GetLayout()) == typeid (VolumeLayoutV1Normal))
				throw ParameterIncorrect (SRC_POS);

			if (typeid (*normalVolume->GetLayout()) == typeid (VolumeLayoutV2Normal) && typeid (*hiddenVolume->GetLayout()) != typeid (VolumeLayoutV2Hidden))
				throw ParameterIncorrect (SRC_POS);
		}

		// Ask user to select backup file path
		wxString confirmMsg = LangString["CONFIRM_VOL_HEADER_BAK"];

		if (!AskYesNo (wxString::Format (confirmMsg, wstring (*volumePath).c_str()), true))
			return;

		FilePathList files = SelectFiles (parent, wxEmptyString, true, false);
		if (files.empty())
			return;

		File backupFile;
		backupFile.Open (*files.front(), File::CreateWrite);

		RandomNumberGenerator::Start();
		/* force the display of the random enriching interface */
		RandomNumberGenerator::SetEnrichedByUserStatus (false);
		UserEnrichRandomPool (nullptr);

		{
			wxBusyCursor busy;

			// Re-encrypt volume header
			SecureBuffer newHeaderBuffer (normalVolume->GetLayout()->GetHeaderSize());
			ReEncryptHeaderThreadRoutine routine(newHeaderBuffer, normalVolume->GetHeader(), normalVolumeMountOptions.Password, normalVolumeMountOptions.Pim, normalVolumeMountOptions.Keyfiles);

			ExecuteWaitThreadRoutine (parent, &routine);

			backupFile.Write (newHeaderBuffer);

			if (hiddenVolume)
			{
				// Re-encrypt hidden volume header
				ReEncryptHeaderThreadRoutine hiddenRoutine(newHeaderBuffer, hiddenVolume->GetHeader(), hiddenVolumeMountOptions.Password, hiddenVolumeMountOptions.Pim, hiddenVolumeMountOptions.Keyfiles);

				ExecuteWaitThreadRoutine (parent, &hiddenRoutine);
			}
			else
			{
				// Store random data in place of hidden volume header
				shared_ptr <EncryptionAlgorithm> ea = normalVolume->GetEncryptionAlgorithm();
				Core->RandomizeEncryptionAlgorithmKey (ea);
				ea->Encrypt (newHeaderBuffer);
			}

			backupFile.Write (newHeaderBuffer);
		}

		ShowWarning ("VOL_HEADER_BACKED_UP");
	}

	void GraphicUserInterface::BeginInteractiveBusyState (wxWindow *window)
	{
		static auto_ptr <wxCursor> arrowWaitCursor;

		if (arrowWaitCursor.get() == nullptr)
			arrowWaitCursor.reset (new wxCursor (wxCURSOR_ARROWWAIT));

		window->SetCursor (*arrowWaitCursor);
	}

	void GraphicUserInterface::CreateKeyfile (shared_ptr <FilePath> keyfilePath) const
	{
		try
		{
			KeyfileGeneratorDialog dialog (GetActiveWindow());
			dialog.ShowModal();
		}
		catch (exception &e)
		{
			ShowError (e);
		}
	}

	void GraphicUserInterface::ClearListCtrlSelection (wxListCtrl *listCtrl) const
	{
		foreach (long item, GetListCtrlSelectedItems (listCtrl))
			listCtrl->SetItemState (item, 0, wxLIST_STATE_SELECTED);
	}

	wxHyperlinkCtrl *GraphicUserInterface::CreateHyperlink (wxWindow *parent, const wxString &linkUrl, const wxString &linkText) const
	{
		wxHyperlinkCtrl *hyperlink = new wxHyperlinkCtrl (parent, wxID_ANY, linkText, linkUrl, wxDefaultPosition, wxDefaultSize, wxHL_DEFAULT_STYLE);

		wxColour color = wxSystemSettings::GetColour (wxSYS_COLOUR_WINDOWTEXT);
		hyperlink->SetHoverColour (color);
		hyperlink->SetNormalColour (color);
		hyperlink->SetVisitedColour (color);

		return hyperlink;
	}

	void GraphicUserInterface::DoShowError (const wxString &message) const
	{
		ShowMessage (message, wxOK | wxICON_ERROR);
	}

	void GraphicUserInterface::DoShowInfo (const wxString &message) const
	{
		ShowMessage (message, wxOK | wxICON_INFORMATION);
	}

	void GraphicUserInterface::DoShowString (const wxString &str) const
	{
		ShowMessage (str, wxOK);
	}

	void GraphicUserInterface::DoShowWarning (const wxString &message) const
	{
		ShowMessage (message, wxOK
#ifndef TC_MACOSX
			| wxICON_EXCLAMATION
#endif
			);
	}

	void GraphicUserInterface::EndInteractiveBusyState (wxWindow *window) const
	{
		static auto_ptr <wxCursor> arrowCursor;

		if (arrowCursor.get() == nullptr)
			arrowCursor.reset (new wxCursor (wxCURSOR_ARROW));

		window->SetCursor (*arrowCursor);
	}

	wxTopLevelWindow *GraphicUserInterface::GetActiveWindow () const
	{
#ifdef TC_WINDOWS
		return dynamic_cast <wxTopLevelWindow *> (wxGetActiveWindow());
#endif

#ifdef __WXGTK__
		// GTK for some reason unhides a hidden window if it is a parent of a new window
		if (IsInBackgroundMode())
			return nullptr;
#endif
		if (wxTopLevelWindows.size() == 1)
			return dynamic_cast <wxTopLevelWindow *> (wxTopLevelWindows.front());

#ifdef __WXGTK__
		wxLongLong startTime = wxGetLocalTimeMillis();
		do
		{
#endif
			foreach (wxWindow *window, wxTopLevelWindows)
			{
				wxTopLevelWindow *topLevelWin = dynamic_cast <wxTopLevelWindow *> (window);
				if (topLevelWin && topLevelWin->IsActive() && topLevelWin->IsShown())
					return topLevelWin;
			}
#ifdef __WXGTK__
			Yield(); // GTK does a lot of operations asynchronously, which makes it prone to many race conditions
		} while	(wxGetLocalTimeMillis() - startTime < 500);
#endif

		return dynamic_cast <wxTopLevelWindow *> (ActiveFrame ? ActiveFrame : GetTopWindow());
	}

	shared_ptr <GetStringFunctor> GraphicUserInterface::GetAdminPasswordRequestHandler ()
	{
		struct AdminPasswordRequestHandler : public GetStringFunctor
		{
			virtual void operator() (string &passwordStr)
			{

				wxString sValue;
				if (Gui->GetWaitDialog())
				{
					Gui->GetWaitDialog()->RequestAdminPassword(sValue);
					if (sValue.IsEmpty())
						throw UserAbort (SRC_POS);
				}
				else
				{
					wxPasswordEntryDialog dialog (Gui->GetActiveWindow(), _("Enter your user password or administrator password:"), _("Administrator privileges required"));
					if (dialog.ShowModal() != wxID_OK)
						throw UserAbort (SRC_POS);
					sValue = dialog.GetValue();
				}
				wstring wPassword (sValue);	// A copy of the password is created here by wxWidgets, which cannot be erased
				finally_do_arg (wstring *, &wPassword, { StringConverter::Erase (*finally_arg); });

				StringConverter::ToSingle (wPassword, passwordStr);
			}
		};

		return shared_ptr <GetStringFunctor> (new AdminPasswordRequestHandler);
	}

	int GraphicUserInterface::GetCharHeight (wxWindow *window) const
	{
		int width;
		int height;
		window->GetTextExtent (L"a", &width, &height);

		if (height < 1)
			return 14;

		return height;
	}

	int GraphicUserInterface::GetCharWidth (wxWindow *window) const
	{
		int width;
		int height;
		window->GetTextExtent (L"a", &width, &height);

		if (width < 1)
			return 7;

		return width;
	}

	wxFont GraphicUserInterface::GetDefaultBoldFont (wxWindow *window) const
	{
		return wxFont (
#ifdef __WXGTK__
			9
#elif defined(TC_MACOSX)
			13
#else
			10
#endif
			* GetCharHeight (window) / 13, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL,
#ifdef __WXGTK__
			wxFONTWEIGHT_BOLD, false);
#elif defined(TC_MACOSX)
			wxFONTWEIGHT_NORMAL, false);
#else
			wxFONTWEIGHT_BOLD, false, L"Arial");
#endif
	}

	list <long> GraphicUserInterface::GetListCtrlSelectedItems (wxListCtrl *listCtrl) const
	{
		list <long> selectedItems;

		long item = -1;
		while ((item = listCtrl->GetNextItem (item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
			selectedItems.push_back (item);

		return selectedItems;
	}

	wxString GraphicUserInterface::GetListCtrlSubItemText (wxListCtrl *listCtrl, long itemIndex, int columnIndex) const
	{
		wxListItem item;
		item.SetId (itemIndex);
		item.SetColumn (columnIndex);
		item.SetText (L"");

		if (!listCtrl->GetItem (item))
			throw ParameterIncorrect (SRC_POS);

		return item.GetText();
	}

	int GraphicUserInterface::GetScrollbarWidth (wxWindow *window, bool noScrollBar) const
	{
		int offset = 0;
#ifdef TC_WINDOWS
		offset = 4;
#elif defined (__WXGTK__)
		offset = 7;
#elif defined (TC_MACOSX)
		offset = 9;
#endif
		if (noScrollBar)
			return offset;

		int width = wxSystemSettings::GetMetric (wxSYS_VSCROLL_X, window);

		if (width == -1)
			return 24;

		return width + offset;
	}

	void GraphicUserInterface::InitSecurityTokenLibrary () const
	{
		if (Preferences.SecurityTokenModule.IsEmpty())
			throw_err (LangString ["NO_PKCS11_MODULE_SPECIFIED"]);

		struct PinRequestHandler : public GetPinFunctor
		{
			virtual void operator() (string &passwordStr)
			{
				if (CmdLine->ArgTokenPin && CmdLine->ArgTokenPin->IsAllocated ())
				{
					passwordStr.clear();
					passwordStr.insert (0, (char*) CmdLine->ArgTokenPin->Ptr (), CmdLine->ArgTokenPin->Size());
					return;
				}

				if (Gui->GetPreferences().NonInteractive)
					throw MissingArgument (SRC_POS);

				wxString sValue;
				if (Gui->GetWaitDialog())
				{
					sValue = StringConverter::ToWide (passwordStr).c_str();
					Gui->GetWaitDialog()->RequestPin (sValue);
					if (sValue.IsEmpty ())
						throw UserAbort (SRC_POS);
				}
				else
				{
					wxPasswordEntryDialog dialog (Gui->GetActiveWindow(), wxString::Format (LangString["ENTER_TOKEN_PASSWORD"], StringConverter::ToWide (passwordStr).c_str()), LangString["IDD_TOKEN_PASSWORD"]);
					dialog.SetSize (wxSize (Gui->GetCharWidth (&dialog) * 50, -1));

					if (dialog.ShowModal() != wxID_OK)
						throw UserAbort (SRC_POS);
					sValue = dialog.GetValue();
				}

				wstring wPassword (sValue);	// A copy of the password is created here by wxWidgets, which cannot be erased
				finally_do_arg (wstring *, &wPassword, { StringConverter::Erase (*finally_arg); });

				StringConverter::ToSingle (wPassword, passwordStr);
			}

			virtual void notifyIncorrectPin ()
			{
				if (CmdLine->ArgTokenPin && CmdLine->ArgTokenPin->IsAllocated ())
				{
					CmdLine->ArgTokenPin->Free ();
				}
			}
		};

		struct WarningHandler : public SendExceptionFunctor
		{
			virtual void operator() (const Exception &e)
			{
				Gui->ShowError (e);
			}
		};

		try
		{
			SecurityToken::InitLibrary (Preferences.SecurityTokenModule, auto_ptr <GetPinFunctor> (new PinRequestHandler), auto_ptr <SendExceptionFunctor> (new WarningHandler));
		}
		catch (Exception &e)
		{
			ShowError (e);
			throw_err (LangString ["PKCS11_MODULE_INIT_FAILED"]);
		}
	}

	void GraphicUserInterface::InsertToListCtrl (wxListCtrl *listCtrl, long itemIndex, const vector <wstring> &itemFields, int imageIndex, void *itemDataPtr) const
	{
		wxListItem item;
		item.SetData (itemDataPtr);
		item.SetId (itemIndex);
		item.SetImage (imageIndex);
		int col = 0;
		foreach (wxString field, itemFields)
		{
			item.SetColumn (col++);
			item.SetText (field);
			if (col == 1)
			{
				throw_sys_if (listCtrl->InsertItem (item) == -1);
				item.SetImage (-1);
				continue;
			}

			listCtrl->SetItem (item);
		}
	}

	bool GraphicUserInterface::IsTheOnlyTopLevelWindow (const wxWindow *window) const
	{
		foreach (wxWindow *w, wxTopLevelWindows)
		{
			if (w != window
				&& (dynamic_cast <const wxFrame *> (w) || dynamic_cast <const wxDialog *> (w))
				&& StringConverter::GetTypeName (typeid (*w)).find ("wxTaskBarIcon") == string::npos)
			{
				return false;
			}
		}
		return true;
	}

	void GraphicUserInterface::ListSecurityTokenKeyfiles () const
	{
		SecurityTokenKeyfilesDialog dialog (nullptr);
		dialog.ShowModal();
	}

#ifdef TC_MACOSX
	void GraphicUserInterface::MacOpenFiles (const wxArrayString &fileNames)
	{
		if (fileNames.GetCount() > 0)
		{
			// we can only put one volume path at a time on the text field
			// so we take the first on the list
			OpenVolumeSystemRequestEventArgs eventArgs (fileNames[0]);
			OpenVolumeSystemRequestEvent.Raise (eventArgs);
		}
	}

	void GraphicUserInterface::MacReopenApp ()
	{
		SetBackgroundMode (false);
	}
#endif

	void GraphicUserInterface::MoveListCtrlItem (wxListCtrl *listCtrl, long itemIndex, long newItemIndex) const
	{
		if (itemIndex == newItemIndex || newItemIndex < 0
			|| (newItemIndex > itemIndex && newItemIndex == listCtrl->GetItemCount()))
			return;

		wxListItem item;
		item.SetId (itemIndex);
		item.SetData ((void *) nullptr);
		item.SetImage (-1);

		if (!listCtrl->GetItem (item))
			throw ParameterIncorrect (SRC_POS);

		int itemState = listCtrl->GetItemState (itemIndex, wxLIST_STATE_SELECTED);

		vector <wstring> itemFields (listCtrl->GetColumnCount());
		for (size_t col = 0; col < itemFields.size(); ++col)
		{
			itemFields[col] = GetListCtrlSubItemText (listCtrl, itemIndex, col);
		}

		listCtrl->DeleteItem (itemIndex);

		if (newItemIndex > listCtrl->GetItemCount() - 1)
			AppendToListCtrl (listCtrl, itemFields, item.GetImage(), (void *) item.GetData());
		else
			InsertToListCtrl (listCtrl, newItemIndex, itemFields, item.GetImage(), (void *) item.GetData());

		item.SetId (newItemIndex);
		listCtrl->SetItemState (item, itemState, wxLIST_STATE_SELECTED);
	}

	VolumeInfoList GraphicUserInterface::MountAllDeviceHostedVolumes (MountOptions &options) const
	{
		MountOptionsDialog dialog (GetTopWindow(), options);
		while (true)
		{
			options.Path.reset();

			if (dialog.ShowModal() != wxID_OK)
				return VolumeInfoList();

			VolumeInfoList mountedVolumes = UserInterface::MountAllDeviceHostedVolumes (options);

			if (!mountedVolumes.empty())
				return mountedVolumes;
		}
	}

	shared_ptr <VolumeInfo> GraphicUserInterface::MountVolume (MountOptions &options) const
	{
		CheckRequirementsForMountingVolume();

		shared_ptr <VolumeInfo> volume;

		if (!options.Path || options.Path->IsEmpty())
			options.Path = make_shared <VolumePath> (SelectVolumeFile (GetActiveWindow()));

		if (options.Path->IsEmpty())
			throw UserAbort (SRC_POS);

		if (Core->IsVolumeMounted (*options.Path))
		{
			ShowInfo (StringFormatter (LangString["VOLUME_ALREADY_MOUNTED"], wstring (*options.Path)));
			return volume;
		}

		try
		{
			if ((!options.Password || options.Password->IsEmpty())
				&& (!options.Keyfiles || options.Keyfiles->empty())
				&& !Core->IsPasswordCacheEmpty())
			{
				// Cached password
				try
				{
					wxBusyCursor busy;
					return UserInterface::MountVolume (options);
				}
				catch (PasswordException&) { }
			}

			if (!options.Keyfiles && GetPreferences().UseKeyfiles && !GetPreferences().DefaultKeyfiles.empty())
				options.Keyfiles = make_shared <KeyfileList> (GetPreferences().DefaultKeyfiles);

			if ((options.Password && !options.Password->IsEmpty())
				|| (options.Keyfiles && !options.Keyfiles->empty() && (options.TrueCryptMode || options.Password)))
			{
				try
				{
					wxBusyCursor busy;
					return UserInterface::MountVolume (options);
				}
				catch (PasswordException&) { }
			}

			VolumePassword password;
			KeyfileList keyfiles;

			MountOptionsDialog dialog (GetTopWindow(), options);
			int incorrectPasswordCount = 0;

			while (!volume)
			{
				dialog.Hide();
				if (dialog.ShowModal() != wxID_OK)
					return volume;

				try
				{
					wxBusyCursor busy;
					volume = UserInterface::MountVolume (options);
				}
				catch (PasswordIncorrect &e)
				{
					if (++incorrectPasswordCount > 2 && !options.UseBackupHeaders)
					{
						// Try to mount the volume using the backup header
						options.UseBackupHeaders = true;

						try
						{
							volume = UserInterface::MountVolume (options);
							ShowWarning ("HEADER_DAMAGED_AUTO_USED_HEADER_BAK");
						}
						catch (...)
						{
							options.UseBackupHeaders = false;
							ShowWarning (e);
						}
					}
					else
						ShowWarning (e);
				}
				catch (PasswordException &e)
				{
					ShowWarning (e);
				}
			}
		}
		catch (exception &e)
		{
			ShowError (e);
		}

#ifdef TC_LINUX
		if (volume && !Preferences.NonInteractive && !Preferences.DisableKernelEncryptionModeWarning
			&& volume->EncryptionModeName != L"XTS"
			&& !AskYesNo (LangString["ENCRYPTION_MODE_NOT_SUPPORTED_BY_KERNEL"] + _("\n\nDo you want to show this message next time you mount such a volume?"), true, true))
		{
			UserPreferences prefs = GetPreferences();
			prefs.DisableKernelEncryptionModeWarning = true;
			Gui->SetPreferences (prefs);
		}
#endif
		return volume;
	}

	void GraphicUserInterface::OnAutoDismountAllEvent ()
	{
		VolumeInfoList mountedVolumes = Core->GetMountedVolumes();

		if (!mountedVolumes.empty())
		{
			wxBusyCursor busy;
			AutoDismountVolumes (mountedVolumes);
		}
	}

	bool GraphicUserInterface::OnInit ()
	{
		Gui = this;
		InterfaceType = UserInterfaceType::Graphic;
		try
		{
			FatalErrorHandler::Register();
			Init();

			if (ProcessCommandLine() && !CmdLine->StartBackgroundTask)
			{
				Yield();
				Application::SetExitCode (0);
				return false;
			}

			// Check if another instance is already running and bring its windows to foreground
#ifndef TC_MACOSX
#ifdef TC_WINDOWS
			const wxString serverName = Application::GetName() + L"-" + wxGetUserId();
			class Connection : public wxDDEConnection
			{
			public:
				Connection () { }

				bool OnExecute (const wxString& topic, wxChar *data, int size, wxIPCFormat format)
				{
					if (topic == L"raise")
					{
						if (Gui->IsInBackgroundMode())
							Gui->SetBackgroundMode (false);

						Gui->mMainFrame->Show (true);
						Gui->mMainFrame->Raise ();
						return true;
					}
					return false;
				}
			};
#endif

			wxLogLevel logLevel = wxLog::GetLogLevel();
			wxLog::SetLogLevel (wxLOG_Error);

			const wxString instanceCheckerName = wxString (L".") + Application::GetName() + L"-lock-" + wxGetUserId();
			SingleInstanceChecker.reset (new wxSingleInstanceChecker (instanceCheckerName));

			wxLog::SetLogLevel (logLevel);

			if (SingleInstanceChecker->IsAnotherRunning())
			{
#ifdef TC_WINDOWS
				class Client: public wxDDEClient
				{
				public:
					Client() {};
					wxConnectionBase *OnMakeConnection () { return new Connection; }
				};

				auto_ptr <wxDDEClient> client (new Client);
				auto_ptr <wxConnectionBase> connection (client->MakeConnection (L"localhost", serverName, L"raise"));

				if (connection.get() && connection->Execute (nullptr))
				{
					connection->Disconnect();
					Application::SetExitCode (0);
					return false;
				}
#endif

#if defined(TC_UNIX) && !defined(TC_MACOSX)
				try
				{
					int showFifo = open (string (MainFrame::GetShowRequestFifoPath()).c_str(), O_WRONLY | O_NONBLOCK);
					throw_sys_if (showFifo == -1);

					byte buf[1] = { 1 };
					if (write (showFifo, buf, 1) == 1)
					{
						close (showFifo);
						Gui->ShowInfo (_("VeraCrypt is already running."));
						Application::SetExitCode (0);
						return false;
					}

					close (showFifo);
				}
				catch (...)
				{
#ifdef DEBUG
					throw;
#endif
				}

				// This is a false positive as VeraCrypt is not running (pipe not available)
				// we continue running after cleaning the lock file
				// and creating a new instance of the checker
				wxString lockFileName = wxGetHomeDir();
				if ( lockFileName.Last() != wxT('/') )
				{
					lockFileName += wxT('/');
				}
				lockFileName << instanceCheckerName;

				if (wxRemoveFile (lockFileName))
				{
					SingleInstanceChecker.reset (new wxSingleInstanceChecker (instanceCheckerName));
				}
#else

				wxLog::FlushActive();
				Application::SetExitCode (1);
				Gui->ShowInfo (_("VeraCrypt is already running."));
				return false;
#endif
			}

#ifdef TC_WINDOWS
			class Server : public wxDDEServer
			{
			public:
				wxConnectionBase *OnAcceptConnection (const wxString &topic)
				{
					if (topic == L"raise")
						return new Connection;
					return nullptr;
				}
			};

			DDEServer.reset (new Server);
			if (!DDEServer->Create (serverName))
				wxLog::FlushActive();
#endif
#endif // !TC_MACOSX

			Connect (wxEVT_END_SESSION, wxCloseEventHandler (GraphicUserInterface::OnEndSession));
#ifdef wxHAS_POWER_EVENTS
			Gui->Connect (wxEVT_POWER_SUSPENDING, wxPowerEventHandler (GraphicUserInterface::OnPowerSuspending));
#endif

			mMainFrame = new MainFrame (nullptr);

			if (CmdLine->StartBackgroundTask)
			{
				UserPreferences prefs = GetPreferences ();
				prefs.BackgroundTaskEnabled = true;
				SetPreferences (prefs);
				mMainFrame->Close();
			}
			else
			{
				mMainFrame->Show (true);
			}

			SetTopWindow (mMainFrame);
		}
		catch (exception &e)
		{
			ShowError (e);
			return false;
		}

		return true;
	}

	void GraphicUserInterface::OnLogOff ()
	{
		VolumeInfoList mountedVolumes = Core->GetMountedVolumes();
		if (GetPreferences().BackgroundTaskEnabled && GetPreferences().DismountOnLogOff
			&& !mountedVolumes.empty())
		{
			wxLongLong startTime = wxGetLocalTimeMillis();
			bool timeOver = false;

			wxBusyCursor busy;
			while (!timeOver && !mountedVolumes.empty())
			{
				try
				{
					timeOver = (wxGetLocalTimeMillis() - startTime >= 4000);

					DismountVolumes (mountedVolumes, !timeOver ? false : GetPreferences().ForceAutoDismount, timeOver);
					OnVolumesAutoDismounted();

					break;
				}
				catch (UserAbort&)
				{
					return;
				}
				catch (...)
				{
					Thread::Sleep (500);
				}

				VolumeInfoList mountedVolumes = Core->GetMountedVolumes();
			}

		}
	}

#ifdef wxHAS_POWER_EVENTS
	void GraphicUserInterface::OnPowerSuspending (wxPowerEvent& event)
	{
		size_t volumeCount = Core->GetMountedVolumes().size();
		if (GetPreferences().BackgroundTaskEnabled && GetPreferences().DismountOnPowerSaving && volumeCount > 0)
		{
			OnAutoDismountAllEvent();

			if (Core->GetMountedVolumes().size() < volumeCount)
				ShowInfoTopMost (LangString["MOUNTED_VOLUMES_AUTO_DISMOUNTED"]);
		}
	}
#endif

	void GraphicUserInterface::OnSignal (int signal)
	{
#ifdef TC_UNIX
		Gui->SingleInstanceChecker.reset();
		_exit (1);
#endif
	}

	void GraphicUserInterface::OnVolumesAutoDismounted ()
	{
		if (GetPreferences().WipeCacheOnAutoDismount)
		{
			Core->WipePasswordCache();
			SecurityToken::CloseAllSessions();
		}
	}

	void GraphicUserInterface::OpenDocument (wxWindow *parent, const wxFileName &document)
	{
		if (!document.FileExists())
			throw ParameterIncorrect (SRC_POS);

#ifdef TC_WINDOWS

		if (int (ShellExecute (GetTopWindow() ? static_cast <HWND> (GetTopWindow()->GetHandle()) : nullptr, L"open",
		document.GetFullPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) >= 32)
		return;

#else
		wxMimeTypesManager mimeMgr;
		wxFileType *fileType = mimeMgr.GetFileTypeFromExtension (document.GetExt());
		if (fileType)
		{
			try
			{
				if (wxExecute (fileType->GetOpenCommand (document.GetFullPath())) != 0)
					return;
			}
			catch (TimeOut&) { }
		}
#endif
	}

	wxString GraphicUserInterface::GetHomepageLinkURL (const wxString &linkId, bool secure, const wxString &extraVars) const
	{
		wxString url = wxString (StringConverter::ToWide (secure ? TC_APPLINK_SECURE : TC_APPLINK));

		if (linkId == L"donate")
		{
			url = L"https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=5BCXVMTTNJDCY";
		}
		else if (linkId == L"main")
		{
			url = wxString (StringConverter::ToWide (TC_HOMEPAGE));
		}
		else if (linkId == L"localizations")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Language%20Packs";
		}
		else if (linkId == L"beginnerstutorial" || linkId == L"tutorial")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Beginner%27s%20Tutorial";
		}
		else if (linkId == L"releasenotes" || linkId == L"history")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Release%20Notes";
		}
		else if (linkId == L"hwacceleration")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Hardware%20Acceleration";
		}
		else if (linkId == L"parallelization")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Parallelization";
		}
		else if (linkId == L"help")
		{
			url = L"https://veracrypt.codeplex.com/documentation";
		}
		else if (linkId == L"keyfiles")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Keyfiles";
		}
		else if (linkId == L"introcontainer")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Creating%20New%20Volumes";
		}
		else if (linkId == L"introsysenc")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=System%20Encryption";
		}
		else if (linkId == L"hiddensysenc")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=VeraCrypt%20Hidden%20Operating%20System";
		}
		else if (linkId == L"sysencprogressinfo")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=System%20Encryption";
		}
		else if (linkId == L"hiddenvolume")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Hidden%20Volume";
		}
		else if (linkId == L"aes")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=AES";
		}
		else if (linkId == L"serpent")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Serpent";
		}
		else if (linkId == L"twofish")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Twofish";
		}
		else if (linkId == L"camellia")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Camellia";
		}
		else if (linkId == L"kuznyechik")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Kuznyechik";
		}
		else if (linkId == L"cascades")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Cascades";
		}
		else if (linkId == L"hashalgorithms")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Hash%20Algorithms";
		}
		else if (linkId == L"isoburning")
		{
			url = L"https://cdburnerxp.se/en/home";
		}
		else if (linkId == L"sysfavorites")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=System%20Favorite%20Volumes";
		}
		else if (linkId == L"favorites")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Favorite%20Volumes";
		}
		else if (linkId == L"hiddenvolprotection")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Protection%20of%20Hidden%20Volumes";
		}
		else if (linkId == L"faq")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=FAQ";
		}
		else if (linkId == L"downloads")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Downloads";
		}
		else if (linkId == L"news")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=News";
		}
		else if (linkId == L"contact")
		{
			url = L"https://veracrypt.codeplex.com/wikipage?title=Contact";
		}

		return url;
	}

	void GraphicUserInterface::OpenHomepageLink (wxWindow *parent, const wxString &linkId, const wxString &extraVars)
	{
		wxString url;

		BeginInteractiveBusyState (parent);
		wxLaunchDefaultBrowser (GetHomepageLinkURL (linkId, false, extraVars), wxBROWSER_NEW_WINDOW);
		Thread::Sleep (200);
		EndInteractiveBusyState (parent);
	}

	void GraphicUserInterface::OpenOnlineHelp (wxWindow *parent)
	{
		OpenHomepageLink (parent, L"help");
	}

	void GraphicUserInterface::OpenUserGuide (wxWindow *parent)
	{
		try
		{
			wxString docPath = wstring (Application::GetExecutableDirectory());

#ifdef TC_RESOURCE_DIR
			docPath = StringConverter::ToWide (string (TC_TO_STRING (TC_RESOURCE_DIR)) + "/doc/VeraCrypt User Guide.pdf");
#elif defined (TC_WINDOWS)
			docPath += L"\\VeraCrypt User Guide.pdf";
#elif defined (TC_MACOSX)
			docPath += L"/../Resources/VeraCrypt User Guide.pdf";
#elif defined (TC_UNIX)
			docPath = L"/usr/share/veracrypt/doc/VeraCrypt User Guide.pdf";
#else
#	error TC_RESOURCE_DIR undefined
#endif

			wxFileName docFile = docPath;
			docFile.Normalize();

			try
			{
				Gui->OpenDocument (parent, docFile);
			}
			catch (...)
			{
				if (Gui->AskYesNo (LangString ["HELP_READER_ERROR"], true))
					OpenOnlineHelp (parent);
			}
		}
		catch (Exception &e)
		{
			Gui->ShowError (e);
		}
	}

	void GraphicUserInterface::RestoreVolumeHeaders (shared_ptr <VolumePath> volumePath) const
	{
		wxWindow *parent = GetActiveWindow();

		if (!volumePath || volumePath->IsEmpty())
			volumePath = make_shared <VolumePath> (SelectVolumeFile (GetActiveWindow()));

		if (volumePath->IsEmpty())
			throw UserAbort (SRC_POS);

#ifdef TC_WINDOWS
		if (Core->IsVolumeMounted (*volumePath))
		{
			ShowInfo ("DISMOUNT_FIRST");
			return;
		}
#endif

#ifdef TC_UNIX
		// Temporarily take ownership of a device if the user is not an administrator
		UserId origDeviceOwner ((uid_t) -1);

		if (!Core->HasAdminPrivileges() && volumePath->IsDevice())
		{
			origDeviceOwner = FilesystemPath (wstring (*volumePath)).GetOwner();
			Core->SetFileOwner (*volumePath, UserId (getuid()));
		}

		finally_do_arg2 (FilesystemPath, *volumePath, UserId, origDeviceOwner,
		{
			if (finally_arg2.SystemId != (uid_t) -1)
				Core->SetFileOwner (finally_arg, finally_arg2);
		});
#endif

		// Ask whether to restore internal or external backup
		bool restoreInternalBackup;
		wxArrayString choices;
		choices.Add (LangString["HEADER_RESTORE_INTERNAL"]);
		choices.Add (LangString["HEADER_RESTORE_EXTERNAL"]);

		wxSingleChoiceDialog choiceDialog (parent, LangString["HEADER_RESTORE_EXTERNAL_INTERNAL"], Application::GetName(), choices);
		choiceDialog.SetSize (wxSize (Gui->GetCharWidth (&choiceDialog) * 80, -1));
		choiceDialog.SetSelection (-1);

		if (choiceDialog.ShowModal() != wxID_OK)
			return;

		switch (choiceDialog.GetSelection())
		{
		case 0:
			restoreInternalBackup = true;
			break;

		case 1:
			restoreInternalBackup = false;
			break;

		default:
			return;
		}

		/* force the display of the random enriching interface */
		RandomNumberGenerator::SetEnrichedByUserStatus (false);

		if (restoreInternalBackup)
		{
			// Restore header from the internal backup
			shared_ptr <Volume> volume;
			MountOptions options;
			options.Path = volumePath;

			MountOptionsDialog dialog (parent, options, wxEmptyString, true);

			while (!volume)
			{
				dialog.Hide();
				if (dialog.ShowModal() != wxID_OK)
					return;

				try
				{
					wxBusyCursor busy;
					OpenVolumeThreadRoutine routine(
						options.Path,
						options.PreserveTimestamps,
						options.Password,
						options.Pim,
						options.Kdf,
						options.TrueCryptMode,
						options.Keyfiles,
						options.Protection,
						options.ProtectionPassword,
						options.ProtectionPim,
						options.ProtectionKdf,
						options.ProtectionKeyfiles,
						options.SharedAccessAllowed,
						VolumeType::Unknown,
						true
						);

						ExecuteWaitThreadRoutine (parent, &routine);
						volume = routine.m_pVolume;
				}
				catch (PasswordException &e)
				{
					ShowWarning (e);
				}
			}

			shared_ptr <VolumeLayout> layout = volume->GetLayout();
			if (typeid (*layout) == typeid (VolumeLayoutV1Normal))
			{
				ShowError ("VOLUME_HAS_NO_BACKUP_HEADER");
				return;
			}

			RandomNumberGenerator::Start();
			UserEnrichRandomPool (nullptr);

			// Re-encrypt volume header
			wxBusyCursor busy;
			SecureBuffer newHeaderBuffer (volume->GetLayout()->GetHeaderSize());
			ReEncryptHeaderThreadRoutine routine(newHeaderBuffer, volume->GetHeader(), options.Password, options.Pim, options.Keyfiles);

			ExecuteWaitThreadRoutine (parent, &routine);

			// Write volume header
			int headerOffset = volume->GetLayout()->GetHeaderOffset();
			shared_ptr <File> volumeFile = volume->GetFile();

			if (headerOffset >= 0)
				volumeFile->SeekAt (headerOffset);
			else
				volumeFile->SeekEnd (headerOffset);

			volumeFile->Write (newHeaderBuffer);
		}
		else
		{
			// Restore header from an external backup

			wxString confirmMsg = LangString["CONFIRM_VOL_HEADER_RESTORE"];

			if (!AskYesNo (wxString::Format (confirmMsg, wstring (*volumePath).c_str()), true, true))
				return;

			FilePathList files = SelectFiles (parent, wxEmptyString, false, false);
			if (files.empty())
				return;

			File backupFile;
			backupFile.Open (*files.front(), File::OpenRead);

			bool legacyBackup;

			// Determine the format of the backup file
			switch (backupFile.Length())
			{
			case TC_VOLUME_HEADER_GROUP_SIZE:
				legacyBackup = false;
				break;

			case TC_VOLUME_HEADER_SIZE_LEGACY * 2:
				legacyBackup = true;
				break;

			default:
				ShowError ("HEADER_BACKUP_SIZE_INCORRECT");
				return;
			}

			// Open the volume header stored in the backup file
			MountOptions options;

			MountOptionsDialog dialog (parent, options, LangString["ENTER_HEADER_BACKUP_PASSWORD"], true);
			shared_ptr <VolumeLayout> decryptedLayout;

			while (!decryptedLayout)
			{
				dialog.Hide();
				if (dialog.ShowModal() != wxID_OK)
					return;

				try
				{
					wxBusyCursor busy;

					// Test volume layouts
					foreach (shared_ptr <VolumeLayout> layout, VolumeLayout::GetAvailableLayouts ())
					{
						if (layout->HasDriveHeader())
							continue;

						if (!legacyBackup && (typeid (*layout) == typeid (VolumeLayoutV1Normal)))
							continue;

						if (legacyBackup && (typeid (*layout) == typeid (VolumeLayoutV2Normal) || typeid (*layout) == typeid (VolumeLayoutV2Hidden)))
							continue;

						SecureBuffer headerBuffer (layout->GetHeaderSize());
						backupFile.ReadAt (headerBuffer, layout->GetType() == VolumeType::Hidden ? layout->GetHeaderSize() : 0);

						// Decrypt header
						shared_ptr <VolumePassword> passwordKey = Keyfile::ApplyListToPassword (options.Keyfiles, options.Password);
						Pkcs5KdfList keyDerivationFunctions = layout->GetSupportedKeyDerivationFunctions(options.TrueCryptMode);
						EncryptionAlgorithmList encryptionAlgorithms = layout->GetSupportedEncryptionAlgorithms();
						EncryptionModeList encryptionModes = layout->GetSupportedEncryptionModes();

						DecryptThreadRoutine decryptRoutine(layout->GetHeader(), headerBuffer, *passwordKey, options.Pim, options.Kdf, options.TrueCryptMode, keyDerivationFunctions, encryptionAlgorithms, encryptionModes);

						ExecuteWaitThreadRoutine (parent, &decryptRoutine);

						if (decryptRoutine.m_bResult)
						{
							decryptedLayout = layout;
							break;
						}
					}

					if (!decryptedLayout)
						throw PasswordIncorrect (SRC_POS);
				}
				catch (PasswordException &e)
				{
					ShowWarning (e);
				}
			}

			File volumeFile;
			volumeFile.Open (*volumePath, File::OpenReadWrite, File::ShareNone, File::PreserveTimestamps);

			RandomNumberGenerator::Start();
			UserEnrichRandomPool (nullptr);

			// Re-encrypt volume header
			wxBusyCursor busy;
			SecureBuffer newHeaderBuffer (decryptedLayout->GetHeaderSize());
			ReEncryptHeaderThreadRoutine routine(newHeaderBuffer, decryptedLayout->GetHeader(), options.Password, options.Pim, options.Keyfiles);

			ExecuteWaitThreadRoutine (parent, &routine);

			// Write volume header
			int headerOffset = decryptedLayout->GetHeaderOffset();
			if (headerOffset >= 0)
				volumeFile.SeekAt (headerOffset);
			else
				volumeFile.SeekEnd (headerOffset);

			volumeFile.Write (newHeaderBuffer);

			if (decryptedLayout->HasBackupHeader())
			{
				// Re-encrypt backup volume header
				ReEncryptHeaderThreadRoutine backupRoutine(newHeaderBuffer, decryptedLayout->GetHeader(), options.Password, options.Pim, options.Keyfiles);

				ExecuteWaitThreadRoutine (parent, &backupRoutine);

				// Write backup volume header
				headerOffset = decryptedLayout->GetBackupHeaderOffset();
				if (headerOffset >= 0)
					volumeFile.SeekAt (headerOffset);
				else
					volumeFile.SeekEnd (headerOffset);

				volumeFile.Write (newHeaderBuffer);
			}
		}

		ShowInfo ("VOL_HEADER_RESTORED");
	}

	DevicePath GraphicUserInterface::SelectDevice (wxWindow *parent) const
	{
		try
		{
			DeviceSelectionDialog dialog (parent);
			if (dialog.ShowModal() == wxID_OK)
			{
				return dialog.SelectedDevice.Path;
			}
		}
		catch (exception &e)
		{
			Gui->ShowError (e);
		}

		return DevicePath();
	}

	DirectoryPath GraphicUserInterface::SelectDirectory (wxWindow *parent, const wxString &message, bool existingOnly) const
	{
		return DirectoryPath (::wxDirSelector (!message.empty() ? message :
#ifdef __WXGTK__
			wxDirSelectorPromptStr,
#else
			L"",
#endif
			L"", wxDD_DEFAULT_STYLE | (existingOnly ? wxDD_DIR_MUST_EXIST : 0), wxDefaultPosition, parent).wc_str());
	}

	FilePathList GraphicUserInterface::SelectFiles (wxWindow *parent, const wxString &caption, bool saveMode, bool allowMultiple, const list < pair <wstring, wstring> > &fileExtensions, const DirectoryPath &directory) const
	{
		FilePathList files;

		long style;
		if (saveMode)
			style = wxFD_SAVE | wxFD_OVERWRITE_PROMPT;
		else
			style = wxFD_OPEN | wxFD_FILE_MUST_EXIST | (allowMultiple ? wxFD_MULTIPLE : 0);

		wxString wildcards = L"*.*";

#ifndef __WXGTK__
		if (!fileExtensions.empty())
#endif
		{
			wildcards = LangString["ALL_FILES"] +
#ifdef TC_WINDOWS
				L" (*.*)|*.*";
#else
				L"|*";
#endif
			typedef pair <wstring, wstring> StringPair;
			foreach (StringPair p, fileExtensions)
			{
				if (p.first == L"*" || p.first == L"*.*")
				{
					wildcards += L"|" + wildcards.substr (0, wildcards.find (L"*|") + 1);
					wildcards = wildcards.substr (wildcards.find (L"*|") + 2);
					continue;
				}

				wildcards += wxString (L"|") + p.second + L" (*." + p.first + L")|*." + p.first;
			}
		}

		wxFileDialog dialog (parent, !caption.empty() ? caption : LangString ["OPEN_TITLE"], wstring (directory), wxString(), wildcards, style);

		if (dialog.ShowModal() == wxID_OK)
		{
			if (!allowMultiple)
				files.push_back (make_shared <FilePath> (dialog.GetPath().wc_str()));
			else
			{
				wxArrayString paths;
				dialog.GetPaths (paths);

				foreach (const wxString &path, paths)
					files.push_back (make_shared <FilePath> (path.wc_str()));
			}
		}

		return files;
	}

	FilePath GraphicUserInterface::SelectVolumeFile (wxWindow *parent, bool saveMode, const DirectoryPath &directory) const
	{
		list < pair <wstring, wstring> > extensions;
		extensions.push_back (make_pair (L"tc", LangString["TC_VOLUMES"].ToStdWstring()));

		FilePathList selFiles = Gui->SelectFiles (parent, LangString[saveMode ? "OPEN_NEW_VOLUME" : "OPEN_VOL_TITLE"], saveMode, false, extensions, directory);

		if (!selFiles.empty())
			return *selFiles.front();
		else
			return FilePath();
	}

	void GraphicUserInterface::SetBackgroundMode (bool state)
	{
#ifdef TC_MACOSX
		// Hiding an iconized window on OS X apparently cannot be reversed
		if (state && mMainFrame->IsIconized())
			mMainFrame->Iconize (false);
#endif
		mMainFrame->Show (!state);
		if (!state)
		{
			if (mMainFrame->IsIconized())
				mMainFrame->Iconize (false);

			mMainFrame->Raise();
		}

		BackgroundMode = state;
	}

	void GraphicUserInterface::SetListCtrlColumnWidths (wxListCtrl *listCtrl, list <int> columnWidthPermilles, bool hasVerticalScrollbar) const
	{
#ifdef TC_MACOSX
		hasVerticalScrollbar = true;
#endif
		int listWidth = listCtrl->GetSize().GetWidth();
		int minListWidth = listCtrl->GetMinSize().GetWidth();
		if (minListWidth > listWidth)
			listWidth = minListWidth;

		listWidth -= GetScrollbarWidth (listCtrl, !hasVerticalScrollbar);

		int col = 0;
		int totalColWidth = 0;
		foreach (int colWidth, columnWidthPermilles)
		{
			int width = listWidth * colWidth / 1000;
			totalColWidth += width;

			if (col == listCtrl->GetColumnCount() - 1)
				width += listWidth - totalColWidth;

			listCtrl->SetColumnWidth (col++, width);
		}
	}

	void GraphicUserInterface::SetListCtrlHeight (wxListCtrl *listCtrl, size_t rowCount) const
	{
		wxRect itemRect;
		if (listCtrl->GetItemCount() == 0)
		{
			bool addedCols = false;
			if (listCtrl->GetColumnCount() == 0)
			{
				listCtrl->InsertColumn (0, L".", wxLIST_FORMAT_LEFT, 1);
				addedCols = true;
			}
			vector <wstring> f;
			f.push_back (L".");
			AppendToListCtrl (listCtrl, f);
			listCtrl->GetItemRect (0, itemRect);

			if (addedCols)
				listCtrl->ClearAll();
			else
				listCtrl->DeleteAllItems();
		}
		else
			listCtrl->GetItemRect (0, itemRect);

		int headerHeight = itemRect.y;
#ifdef TC_WINDOWS
		headerHeight += 4;
#elif defined (TC_MACOSX)
		headerHeight += 7;
#elif defined (__WXGTK__)
		headerHeight += 5;
#endif
		int rowHeight = itemRect.height;
#ifdef TC_MACOSX
		rowHeight += 1;
#endif
		listCtrl->SetMinSize (wxSize (listCtrl->GetMinSize().GetWidth(), rowHeight * rowCount + headerHeight));
	}

	void GraphicUserInterface::SetListCtrlWidth (wxListCtrl *listCtrl, size_t charCount, bool hasVerticalScrollbar) const
	{
		int width = GetCharWidth (listCtrl) * charCount;
#ifdef TC_MACOSX
		if (!hasVerticalScrollbar)
			width += GetScrollbarWidth (listCtrl);
#endif
		listCtrl->SetMinSize (wxSize (width, listCtrl->GetMinSize().GetHeight()));
	}

	void GraphicUserInterface::ShowErrorTopMost (const wxString &message) const
	{
		ShowMessage (message, wxOK | wxICON_ERROR, true);
	}

	void GraphicUserInterface::ShowInfoTopMost (const wxString &message) const
	{
		ShowMessage (message, wxOK | wxICON_INFORMATION, true);
	}

	int GraphicUserInterface::ShowMessage (const wxString &message, long style, bool topMost) const
	{
		wxString caption = Application::GetName();
		wxString subMessage = message;

#ifdef TC_MACOSX
		size_t p = message.find (L"\n");
		if (p != string::npos)
		{
			// Divide message to caption and info message
			caption = message.substr (0, p);

			p = message.find_first_not_of (L'\n', p);
			if (p != string::npos)
				subMessage = message.substr (p);
			else
				subMessage.clear();

			if (subMessage.EndsWith (L"?"))
			{
				// Move question to caption
				caption += wstring (L" ");
				p = subMessage.find_last_of (L".\n");
				if (p != string::npos)
				{
					if (caption.EndsWith (L": "))
						caption[caption.size() - 2] = L'.';

					caption += subMessage.substr (subMessage.find_first_not_of (L"\n ", p + 1));
					subMessage = subMessage.substr (0, p + 1);
				}
				else
				{
					caption += subMessage.substr (subMessage.find_first_not_of (L"\n"));
					subMessage.clear();
				}
			}
		}
		else if (message.size() < 160)
		{
			caption = message;
			subMessage.clear();
		}
		else
		{
			if (style & wxICON_EXCLAMATION)
				caption = wxString (_("Warning")) + L':';
			else if (style & wxICON_ERROR || style & wxICON_HAND)
				caption = wxString (_("Error")) + L':';
			else
				caption.clear();
		}
#endif
		if (mWaitDialog)
		{
			return mWaitDialog->RequestShowMessage(subMessage, caption, style, topMost);
		}
		else
		{
			if (topMost)
			{
				if (!IsActive())
					mMainFrame->RequestUserAttention (wxUSER_ATTENTION_ERROR);

				style |= wxSTAY_ON_TOP;
			}

			return wxMessageBox (subMessage, caption, style, GetActiveWindow());
		}
	}

	void GraphicUserInterface::ShowWarningTopMost (const wxString &message) const
	{
		ShowMessage (message, wxOK
#ifndef TC_MACOSX
			| wxICON_EXCLAMATION
#endif
			, true);
	}

	void GraphicUserInterface::ThrowTextModeRequired () const
	{
		Gui->ShowError (_("This feature is currently supported only in text mode."));
		throw UserAbort (SRC_POS);
	}

	bool GraphicUserInterface::UpdateListCtrlItem (wxListCtrl *listCtrl, long itemIndex, const vector <wstring> &itemFields) const
	{
		bool changed = false;
		wxListItem item;
		item.SetId (itemIndex);
		item.SetText (L"");

		int col = 0;
		foreach (wxString field, itemFields)
		{
			item.SetColumn (col++);

			if (!listCtrl->GetItem (item))
				throw ParameterIncorrect (SRC_POS);

			if (item.GetText() != field)
			{
				item.SetText (field);
				listCtrl->SetItem (item);
				if (item.GetColumn() == 3 || item.GetColumn() == 4)
					listCtrl->SetColumnWidth(item.GetColumn(), wxLIST_AUTOSIZE);
				changed = true;
			}
		}
		return changed;
	}

	void GraphicUserInterface::UserEnrichRandomPool (wxWindow *parent, shared_ptr <Hash> hash) const
	{
		RandomNumberGenerator::Start();

		if (hash)
			RandomNumberGenerator::SetHash (hash);

		if (!RandomNumberGenerator::IsEnrichedByUser())
		{
			RandomPoolEnrichmentDialog dialog (parent);
			RandomNumberGenerator::SetEnrichedByUserStatus (dialog.ShowModal() == wxID_OK);
		}
	}

	void GraphicUserInterface::Yield () const
	{
#ifndef TC_WINDOWS
		wxSafeYield (nullptr, true);
#endif
	}

	shared_ptr <VolumeInfo> GraphicUserInterface::MountVolumeThread (MountOptions &options) const
	{
		MountThreadRoutine routine(options);

		ExecuteWaitThreadRoutine(GetTopWindow(), &routine);
		return routine.m_pVolume;
	}

	void GraphicUserInterface::ExecuteWaitThreadRoutine (wxWindow *parent, WaitThreadRoutine *pRoutine) const
	{
		WaitDialog dlg(parent, LangString["IDT_STATIC_MODAL_WAIT_DLG_INFO"], pRoutine);
		mWaitDialog = &dlg;
		finally_do_arg (WaitDialog**, &mWaitDialog, { *finally_arg = nullptr; });
		dlg.Run();
	}

	DEFINE_EVENT_TYPE (TC_EVENT_THREAD_EXITING);

	GraphicUserInterface *Gui = nullptr;
}
