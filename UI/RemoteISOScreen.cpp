// Copyright (c) 2014- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <thread>
#include <mutex>

#include "base/timeutil.h"
#include "file/path.h"
#include "i18n/i18n.h"
#include "json/json_reader.h"
#include "net/http_client.h"
#include "net/resolve.h"
#include "net/url.h"
#include "Common/Common.h"
#include "Core/Config.h"
#include "Core/WebServer.h"
#include "UI/RemoteISOScreen.h"

using namespace UI;

static const char *REPORT_HOSTNAME = "report.ppsspp.org";
static const int REPORT_PORT = 80;

static bool scanCancelled = false;
static bool scanAborted = false;

static std::string RemoteSubdir() {
	if (g_Config.bRemoteISOManual) {
		return g_Config.sRemoteISOSubdir;
	}

	return "/";
}

static bool FindServer(std::string &resultHost, int &resultPort) {
	http::Client http;
	Buffer result;
	int code = 500;

	std::string subdir = RemoteSubdir();

	auto TryServer = [&](const std::string &host, int port) {
		// Don't wait as long for a connect - we need a good connection for smooth streaming anyway.
		// This way if it's down, we'll find the right one faster.
		if (http.Resolve(host.c_str(), port) && http.Connect(1, 10.0, &scanCancelled)) {
			code = http.GET(subdir.c_str(), &result);
			http.Disconnect();

			if (code != 200) {
				return false;
			}

			// Make sure this isn't just the debugger.  If so, move on.
			std::string listing;
			std::vector<std::string> items;
			result.TakeAll(&listing);
			SplitString(listing, '\n', items);

			bool supported = false;
			for (const std::string &item : items) {
				if (!RemoteISOFileSupported(item)) {
					continue;
				}
				supported = true;
				break;
			}

			if (supported) {
				resultHost = host;
				resultPort = port;
				NOTICE_LOG(HLE, "RemoteISO found: %s : %d", host.c_str(), port);
				return true;
			}
		}

		return false;
	};

	// Try last server first, if it is set
	if (g_Config.iLastRemoteISOPort && g_Config.sLastRemoteISOServer != "") {
		if (TryServer(g_Config.sLastRemoteISOServer.c_str(), g_Config.iLastRemoteISOPort)) {
			return true;
		}
	}

	// Don't scan if in manual mode.
	if (g_Config.bRemoteISOManual || scanCancelled) {
		return false;
	}

	// Start by requesting a list of recent local ips for this network.
	if (http.Resolve(REPORT_HOSTNAME, REPORT_PORT)) {
		if (http.Connect(2, 20.0, &scanCancelled)) {
			code = http.GET("/match/list", &result);
			http.Disconnect();
		}
	}

	if (code != 200 || scanCancelled) {
		return false;
	}

	std::string json;
	result.TakeAll(&json);

	using namespace json;

	JsonReader reader(json.c_str(), json.size());
	if (!reader.ok()) {
		return false;
	}

	const JsonValue entries = reader.rootArray();
	if (entries.getTag() != JSON_ARRAY) {
		return false;
	}

	for (const auto pentry : entries) {
		JsonGet entry = pentry->value;
		if (scanCancelled)
			return false;

		const char *host = entry.getString("ip", "");
		int port = entry.getInt("p", 0);

		if (TryServer(host, port)) {
			return true;
		}
	}

	// None of the local IPs were reachable.
	return false;
}

static bool LoadGameList(const std::string &url, std::vector<std::string> &games) {
	PathBrowser browser(url);
	std::vector<FileInfo> files;
	browser.GetListing(files, "iso:cso:pbp:elf:prx:ppdmp:", &scanCancelled);
	if (scanCancelled) {
		return false;
	}
	for (auto &file : files) {
		if (RemoteISOFileSupported(file.name)) {
			games.push_back(file.fullName);
		}
	}

	return !games.empty();
}

RemoteISOScreen::RemoteISOScreen() : serverRunning_(false), serverStopping_(false) {
}

void RemoteISOScreen::update() {
	UIScreenWithBackground::update();

	bool nowRunning = !WebServerStopped(WebServerFlags::DISCS);
	if (serverStopping_ && !nowRunning) {
		serverStopping_ = false;
	}

	if (serverRunning_ != nowRunning) {
		RecreateViews();
	}
	serverRunning_ = nowRunning;
}

void RemoteISOScreen::CreateViews() {
	I18NCategory *di = GetI18NCategory("Dialog");
	I18NCategory *ri = GetI18NCategory("RemoteISO");

	Margins actionMenuMargins(0, 20, 15, 0);
	Margins contentMargins(0, 20, 5, 5);
	ViewGroup *leftColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 0.4f, contentMargins));
	LinearLayout *leftColumnItems = new LinearLayout(ORIENT_VERTICAL, new LayoutParams(WRAP_CONTENT, FILL_PARENT));
	ViewGroup *rightColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(300, FILL_PARENT, actionMenuMargins));
	LinearLayout *rightColumnItems = new LinearLayout(ORIENT_VERTICAL);

	leftColumnItems->Add(new TextView(ri->T("RemoteISODesc", "Games in your recent list will be shared"), new LinearLayoutParams(Margins(12, 5, 0, 5))));
	leftColumnItems->Add(new TextView(ri->T("RemoteISOWifi", "Note: Connect both devices to the same wifi"), new LinearLayoutParams(Margins(12, 5, 0, 5))));

	rightColumnItems->SetSpacing(0.0f);
	Choice *browseChoice = new Choice(ri->T("Browse Games"));
	rightColumnItems->Add(browseChoice)->OnClick.Handle(this, &RemoteISOScreen::HandleBrowse);
	if (WebServerStopping(WebServerFlags::DISCS)) {
		rightColumnItems->Add(new Choice(ri->T("Stopping..")))->SetDisabledPtr(&serverStopping_);
		browseChoice->SetEnabled(false);
	} else if (!WebServerStopped(WebServerFlags::DISCS)) {
		rightColumnItems->Add(new Choice(ri->T("Stop Sharing")))->OnClick.Handle(this, &RemoteISOScreen::HandleStopServer);
		browseChoice->SetEnabled(false);
	} else {
		rightColumnItems->Add(new Choice(ri->T("Share Games (Server)")))->OnClick.Handle(this, &RemoteISOScreen::HandleStartServer);
		browseChoice->SetEnabled(true);
	}
	Choice *settingsChoice = new Choice(ri->T("Settings"));
	rightColumnItems->Add(settingsChoice)->OnClick.Handle(this, &RemoteISOScreen::HandleSettings);

	LinearLayout *beforeBack = new LinearLayout(ORIENT_HORIZONTAL, new AnchorLayoutParams(FILL_PARENT, FILL_PARENT));
	beforeBack->Add(leftColumn);
	beforeBack->Add(rightColumn);
	root_ = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));
	root_->Add(beforeBack);
	root_->Add(new Choice(di->T("Back"), "", false, new AnchorLayoutParams(150, WRAP_CONTENT, 10, NONE, NONE, 10)))->OnClick.Handle<UIScreen>(this, &UIScreen::OnBack);

	leftColumn->Add(leftColumnItems);
	rightColumn->Add(rightColumnItems);
}

UI::EventReturn RemoteISOScreen::HandleStartServer(UI::EventParams &e) {
	if (!StartWebServer(WebServerFlags::DISCS)) {
		return EVENT_SKIPPED;
	}

	return EVENT_DONE;
}

UI::EventReturn RemoteISOScreen::HandleStopServer(UI::EventParams &e) {
	if (!StopWebServer(WebServerFlags::DISCS)) {
		return EVENT_SKIPPED;
	}

	serverStopping_ = true;
	RecreateViews();

	return EVENT_DONE;
}

UI::EventReturn RemoteISOScreen::HandleBrowse(UI::EventParams &e) {
	screenManager()->push(new RemoteISOConnectScreen());
	return EVENT_DONE;
}

UI::EventReturn RemoteISOScreen::HandleSettings(UI::EventParams &e) {
	screenManager()->push(new RemoteISOSettingsScreen());
	return EVENT_DONE;
}

RemoteISOConnectScreen::RemoteISOConnectScreen() : status_(ScanStatus::SCANNING), nextRetry_(0.0) {
	scanCancelled = false;
	scanAborted = false;

	scanThread_ = new std::thread([](RemoteISOConnectScreen *thiz) {
		thiz->ExecuteScan();
	}, this);
}

RemoteISOConnectScreen::~RemoteISOConnectScreen() {
	int maxWait = 5000;
	scanCancelled = true;
	while (GetStatus() == ScanStatus::SCANNING || GetStatus() == ScanStatus::LOADING) {
		sleep_ms(1);
		if (--maxWait < 0) {
			// If it does ever wake up, it may crash... but better than hanging?
			scanAborted = true;
			break;
		}
	}
	if (scanThread_->joinable())
		scanThread_->join();
	delete scanThread_;
}

void RemoteISOConnectScreen::CreateViews() {
	I18NCategory *di = GetI18NCategory("Dialog");
	I18NCategory *ri = GetI18NCategory("RemoteISO");

	Margins actionMenuMargins(0, 20, 15, 0);
	Margins contentMargins(0, 20, 5, 5);
	ViewGroup *leftColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(WRAP_CONTENT, FILL_PARENT, 0.4f, contentMargins));
	LinearLayout *leftColumnItems = new LinearLayout(ORIENT_VERTICAL, new LayoutParams(WRAP_CONTENT, FILL_PARENT));
	ViewGroup *rightColumn = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(300, FILL_PARENT, actionMenuMargins));
	LinearLayout *rightColumnItems = new LinearLayout(ORIENT_VERTICAL);

	statusView_ = leftColumnItems->Add(new TextView(ri->T("RemoteISOScanning", "Scanning... click Share Games on your desktop"), new LinearLayoutParams(Margins(12, 5, 0, 5))));

	rightColumnItems->SetSpacing(0.0f);
	rightColumnItems->Add(new Choice(di->T("Cancel"), "", false, new AnchorLayoutParams(150, WRAP_CONTENT, 10, NONE, NONE, 10)))->OnClick.Handle<UIScreen>(this, &UIScreen::OnBack);

	root_ = new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, FILL_PARENT, 1.0f));
	root_->Add(leftColumn);
	root_->Add(rightColumn);

	leftColumn->Add(leftColumnItems);
	rightColumn->Add(rightColumnItems);
}

void RemoteISOConnectScreen::update() {
	I18NCategory *ri = GetI18NCategory("RemoteISO");

	UIScreenWithBackground::update();

	ScanStatus s = GetStatus();
	switch (s) {
	case ScanStatus::SCANNING:
	case ScanStatus::LOADING:
		break;

	case ScanStatus::FOUND:
		statusView_->SetText(ri->T("RemoteISOLoading", "Connected - loading game list"));
		status_ = ScanStatus::LOADING;

		// Let's reuse scanThread_.
		if (scanThread_->joinable())
			scanThread_->join();
		delete scanThread_;
		scanThread_ = new std::thread([](RemoteISOConnectScreen *thiz) {
			thiz->ExecuteLoad();
		}, this);
		break;

	case ScanStatus::FAILED:
		nextRetry_ = real_time_now() + 30.0;
		status_ = ScanStatus::RETRY_SCAN;
		break;

	case ScanStatus::RETRY_SCAN:
		if (nextRetry_ < real_time_now()) {
			status_ = ScanStatus::SCANNING;
			nextRetry_ = 0.0;

			if (scanThread_->joinable())
				scanThread_->join();
			delete scanThread_;
			scanThread_ = new std::thread([](RemoteISOConnectScreen *thiz) {
				thiz->ExecuteScan();
			}, this);
		}
		break;

	case ScanStatus::LOADED:
		TriggerFinish(DR_OK);
		screenManager()->push(new RemoteISOBrowseScreen(url_, games_));
		break;
	}
}

void RemoteISOConnectScreen::ExecuteScan() {
	FindServer(host_, port_);
	if (scanAborted) {
		return;
	}

	std::lock_guard<std::mutex> guard(statusLock_);
	status_ = host_.empty() ? ScanStatus::FAILED : ScanStatus::FOUND;
}

ScanStatus RemoteISOConnectScreen::GetStatus() {
	std::lock_guard<std::mutex> guard(statusLock_);
	return status_;
}

void RemoteISOConnectScreen::ExecuteLoad() {
	std::string subdir = RemoteSubdir();
	url_ = StringFromFormat("http://%s:%d%s", host_.c_str(), port_, subdir.c_str());
	bool result = LoadGameList(url_, games_);
	if (scanAborted) {
		return;
	}

	if (result && !games_.empty() && !g_Config.bRemoteISOManual) {
		g_Config.sLastRemoteISOServer = host_;
		g_Config.iLastRemoteISOPort = port_;
	}

	std::lock_guard<std::mutex> guard(statusLock_);
	status_ = result ? ScanStatus::LOADED : ScanStatus::FAILED;
}

class RemoteGameBrowser : public GameBrowser {
public:
	RemoteGameBrowser(const std::string &url, const std::vector<std::string> &games, BrowseFlags browseFlags, bool *gridStyle_, std::string lastText, std::string lastLink, UI::LayoutParams *layoutParams = nullptr)
	: GameBrowser(url, browseFlags, gridStyle_, lastText, lastLink, layoutParams) {
		games_ = games;
		Refresh();
	}

protected:
	bool DisplayTopBar() override {
		return false;
	}

	bool HasSpecialFiles(std::vector<std::string> &filenames) override;

	std::string url_;
	std::vector<std::string> games_;
};

bool RemoteGameBrowser::HasSpecialFiles(std::vector<std::string> &filenames) {
	filenames = games_;
	return true;
}

RemoteISOBrowseScreen::RemoteISOBrowseScreen(const std::string &url, const std::vector<std::string> &games)
	: url_(url), games_(games) {
}

void RemoteISOBrowseScreen::CreateViews() {
	bool vertical = UseVerticalLayout();

	I18NCategory *di = GetI18NCategory("Dialog");
	I18NCategory *ri = GetI18NCategory("RemoteISO");

	Margins actionMenuMargins(0, 10, 10, 0);

	TabHolder *leftColumn = new TabHolder(ORIENT_HORIZONTAL, 64, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	tabHolder_ = leftColumn;
	tabHolder_->SetTag("RemoteGames");
	gameBrowsers_.clear();

	leftColumn->SetClip(true);

	ScrollView *scrollRecentGames = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	scrollRecentGames->SetTag("RemoteGamesTab");
	RemoteGameBrowser *tabRemoteGames = new RemoteGameBrowser(
		url_, games_, BrowseFlags::PIN, &g_Config.bGridView1, "", "",
		new LinearLayoutParams(FILL_PARENT, FILL_PARENT));
	scrollRecentGames->Add(tabRemoteGames);
	gameBrowsers_.push_back(tabRemoteGames);

	leftColumn->AddTab(ri->T("Remote Server"), scrollRecentGames);
	tabRemoteGames->OnChoice.Handle<MainScreen>(this, &MainScreen::OnGameSelectedInstant);
	tabRemoteGames->OnHoldChoice.Handle<MainScreen>(this, &MainScreen::OnGameSelected);
	tabRemoteGames->OnHighlight.Handle<MainScreen>(this, &MainScreen::OnGameHighlight);

	ViewGroup *rightColumn = new ScrollView(ORIENT_VERTICAL);
	LinearLayout *rightColumnItems = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	rightColumnItems->SetSpacing(0.0f);
	rightColumn->Add(rightColumnItems);

	rightColumnItems->Add(new Choice(di->T("Back"), "", false, new AnchorLayoutParams(150, WRAP_CONTENT, 10, NONE, NONE, 10)))->OnClick.Handle<UIScreen>(this, &UIScreen::OnBack);

	if (vertical) {
		root_ = new LinearLayout(ORIENT_VERTICAL);
		rightColumn->ReplaceLayoutParams(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
		leftColumn->ReplaceLayoutParams(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0));
		root_->Add(rightColumn);
		root_->Add(leftColumn);
	} else {
		root_ = new LinearLayout(ORIENT_HORIZONTAL);
		leftColumn->ReplaceLayoutParams(new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, 1.0));
		rightColumn->ReplaceLayoutParams(new LinearLayoutParams(300, FILL_PARENT, actionMenuMargins));
		root_->Add(leftColumn);
		root_->Add(rightColumn);
	}

	root_->SetDefaultFocusView(tabHolder_);

	upgradeBar_ = 0;
}

RemoteISOSettingsScreen::RemoteISOSettingsScreen() {
	serverRunning_ = !WebServerStopped(WebServerFlags::DISCS);
}

void RemoteISOSettingsScreen::update() {
	UIDialogScreenWithBackground::update();

	bool nowRunning = !WebServerStopped(WebServerFlags::DISCS);
	if (serverRunning_ != nowRunning) {
		RecreateViews();
	}
	serverRunning_ = nowRunning;
}

void RemoteISOSettingsScreen::CreateViews() {
	I18NCategory *ri = GetI18NCategory("RemoteISO");
	
	ViewGroup *remoteisoSettingsScroll = new ScrollView(ORIENT_VERTICAL, new LayoutParams(FILL_PARENT, FILL_PARENT));
	remoteisoSettingsScroll->SetTag("RemoteISOSettings");
	LinearLayout *remoteisoSettings = new LinearLayout(ORIENT_VERTICAL);
	remoteisoSettings->SetSpacing(0);
	remoteisoSettingsScroll->Add(remoteisoSettings);

	remoteisoSettings->Add(new ItemHeader(ri->T("Remote disc streaming")));
	remoteisoSettings->Add(new CheckBox(&g_Config.bRemoteShareOnStartup, ri->T("Share on PPSSPP startup")));
	remoteisoSettings->Add(new CheckBox(&g_Config.bRemoteISOManual, ri->T("Manual Mode Client", "Manually configure client")));
#if !defined(MOBILE_DEVICE)
	PopupTextInputChoice *remoteServer = remoteisoSettings->Add(new PopupTextInputChoice(&g_Config.sLastRemoteISOServer, ri->T("Remote Server"), "", 255, screenManager()));
#else
	ChoiceWithValueDisplay *remoteServer = new ChoiceWithValueDisplay(&g_Config.sLastRemoteISOServer, ri->T("Remote Server"), (const char *)nullptr);
	remoteisoSettings->Add(remoteServer);
	remoteServer->OnClick.Handle(this, &RemoteISOSettingsScreen::OnClickRemoteServer);
#endif
	remoteServer->SetEnabledPtr(&g_Config.bRemoteISOManual);
	PopupSliderChoice *remotePort = remoteisoSettings->Add(new PopupSliderChoice(&g_Config.iLastRemoteISOPort, 0, 65535, ri->T("Remote Port", "Remote Port"), 100, screenManager()));
	remotePort->SetEnabledPtr(&g_Config.bRemoteISOManual);
#if !defined(MOBILE_DEVICE)
	PopupTextInputChoice *remoteSubdir = remoteisoSettings->Add(new PopupTextInputChoice(&g_Config.sRemoteISOSubdir, ri->T("Remote Subdirectory"), "", 255, screenManager()));
	remoteSubdir->OnChange.Handle(this, &RemoteISOSettingsScreen::OnChangeRemoteISOSubdir);
#else
	ChoiceWithValueDisplay *remoteSubdir = remoteisoSettings->Add(
			new ChoiceWithValueDisplay(&g_Config.sRemoteISOSubdir, ri->T("Remote Subdirectory"), (const char *)nullptr));
	remoteSubdir->OnClick.Handle(this, &RemoteISOSettingsScreen::OnClickRemoteISOSubdir);
#endif
	remoteSubdir->SetEnabledPtr(&g_Config.bRemoteISOManual);

	PopupSliderChoice *portChoice = new PopupSliderChoice(&g_Config.iRemoteISOPort, 0, 65535, ri->T("Local Server Port", "Local Server Port"), 100, screenManager());
	remoteisoSettings->Add(portChoice);
	portChoice->SetDisabledPtr(&serverRunning_);
	remoteisoSettings->Add(new Spacer(25.0));
	
	root_ = new AnchorLayout(new LayoutParams(FILL_PARENT, FILL_PARENT));
	root_->Add(remoteisoSettingsScroll);
	AddStandardBack(root_);
}

UI::EventReturn RemoteISOSettingsScreen::OnClickRemoteServer(UI::EventParams &e) {
	System_SendMessage("inputbox", ("remoteiso_server:" + g_Config.sLastRemoteISOServer).c_str());
	return UI::EVENT_DONE;
}

UI::EventReturn RemoteISOSettingsScreen::OnClickRemoteISOSubdir(UI::EventParams &e) {
	System_SendMessage("inputbox", ("remoteiso_subdir:" + g_Config.sRemoteISOSubdir).c_str());
	return UI::EVENT_DONE;
}

UI::EventReturn RemoteISOSettingsScreen::OnChangeRemoteISOSubdir(UI::EventParams &e) {
	//Conform to HTTP standards
	ReplaceAll(g_Config.sRemoteISOSubdir, " ", "%20");
	ReplaceAll(g_Config.sRemoteISOSubdir, "\\", "/");
	//Make sure it begins with /
	if (g_Config.sRemoteISOSubdir.empty() || g_Config.sRemoteISOSubdir[0] != '/')
		g_Config.sRemoteISOSubdir = "/" + g_Config.sRemoteISOSubdir;
	
	return UI::EVENT_DONE;
}
