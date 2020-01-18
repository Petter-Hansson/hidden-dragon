#include "pch.h"

#include "HiddenDragon.hpp"
#include "MemoryScan.hpp"
#include "Util.hpp"

#define TO_STRING(s) TO_STRING2(s)
#define TO_STRING2(s) #s


namespace
{
	class TimedDump
	{
	public:
		explicit TimedDump(const char* prefix) :
			_Prefix(prefix)
		{
		}

		void Dump()
		{
			std::ofstream dumpFile(_Prefix + std::to_string(_Counter) + ".bin", std::ios::binary);
			DumpMemory(dumpFile, true);
			dumpFile.close();
			_Timer.Restart();
			_Counter += 1;
		}

		double GetElapsed() const
		{
			return _Timer.GetElapsed();
		}
	private:
		const char* const _Prefix;
		int _Counter = 0;
		Timer _Timer;
	};
}


static IDirectPlay* _baseDirectPlay = nullptr;
static IDirectPlay4A* _directPlay = nullptr;
static DPSESSIONDESC2 _theSession;
static DPID _localPlayer;
static bool _sessionEnumTimeout;
static bool _running = true;

std::ofstream _logFile("HiddenDragonLog.txt");
static std::ofstream _messageFile("HiddenDragonMessages.txt");
static std::ofstream _sendFile("HiddenDragonSent.txt");
static TimedDump _requisitionDump("req");
static TimedDump _deploymentDump("dep");

static const char* GetErrorString(HRESULT hr)
{
#define TEST_ERROR_CODE(x) else if (hr == x) return TO_STRING(#x)
	if (hr == -1)
	{
		return "probably not used for DirectPlay";
	}
	TEST_ERROR_CODE(DPERR_ACCESSDENIED);
	TEST_ERROR_CODE(DPERR_ALREADYINITIALIZED);
	TEST_ERROR_CODE(DPERR_AUTHENTICATIONFAILED);
	TEST_ERROR_CODE(DPERR_BUFFERTOOSMALL);
	TEST_ERROR_CODE(DPERR_CANNOTCREATESERVER);
	TEST_ERROR_CODE(DPERR_CANTADDPLAYER);
	TEST_ERROR_CODE(DPERR_CANTCREATEPLAYER);
	TEST_ERROR_CODE(DPERR_CANTLOADCAPI);
	TEST_ERROR_CODE(DPERR_CANTLOADSECURITYPACKAGE);
	TEST_ERROR_CODE(DPERR_CANTLOADSSPI);
	TEST_ERROR_CODE(DPERR_CONNECTING);
	TEST_ERROR_CODE(DPERR_CONNECTIONLOST);
	TEST_ERROR_CODE(DPERR_ENCRYPTIONFAILED);
	TEST_ERROR_CODE(DPERR_ENCRYPTIONNOTSUPPORTED);
	TEST_ERROR_CODE(DPERR_INVALIDFLAGS);
	TEST_ERROR_CODE(DPERR_INVALIDOBJECT);
	TEST_ERROR_CODE(DPERR_INVALIDPARAMS);
	TEST_ERROR_CODE(DPERR_INVALIDPASSWORD);
	TEST_ERROR_CODE(DPERR_LOGONDENIED);
	TEST_ERROR_CODE(DPERR_NOCONNECTION);
	TEST_ERROR_CODE(DPERR_NOMESSAGES);
	TEST_ERROR_CODE(DPERR_NONEWPLAYERS);
	TEST_ERROR_CODE(DPERR_NOSESSIONS);
	TEST_ERROR_CODE(DPERR_SIGNFAILED);
	TEST_ERROR_CODE(DPERR_TIMEOUT);
	TEST_ERROR_CODE(DPERR_UNINITIALIZED);
	TEST_ERROR_CODE(DPERR_USERCANCEL);
#undef TEST_ERROR_CODE

	return "Unknown";
}

static const char* GetSysMessageString(DWORD id)
{
#define TEST_TYPE_CODE(x) else if (id == x) return TO_STRING(#x)
	if (id == -1)
	{
		return "probably not used for DirectPlay";
	}
	TEST_TYPE_CODE(DPSYS_ADDGROUPTOGROUP);
	TEST_TYPE_CODE(DPSYS_ADDPLAYERTOGROUP);
	TEST_TYPE_CODE(DPSYS_CHAT);
	TEST_TYPE_CODE(DPSYS_CREATEPLAYERORGROUP);
	TEST_TYPE_CODE(DPSYS_DELETEGROUPFROMGROUP);
	TEST_TYPE_CODE(DPSYS_DELETEPLAYERFROMGROUP);
	TEST_TYPE_CODE(DPSYS_DESTROYPLAYERORGROUP);
	TEST_TYPE_CODE(DPSYS_HOST);
	TEST_TYPE_CODE(DPSYS_SECUREMESSAGE);
	TEST_TYPE_CODE(DPSYS_SENDCOMPLETE);
	TEST_TYPE_CODE(DPSYS_SESSIONLOST);
	TEST_TYPE_CODE(DPSYS_SETGROUPOWNER);
	TEST_TYPE_CODE(DPSYS_SETPLAYERORGROUPDATA);
	TEST_TYPE_CODE(DPSYS_SETPLAYERORGROUPNAME);
	TEST_TYPE_CODE(DPSYS_SETSESSIONDESC);
	TEST_TYPE_CODE(DPSYS_STARTSESSION);
#undef TEST_ERROR_CODE

	return "Unknown";
}

bool IsServer()
{
	return _localPlayer == DPID_SERVERPLAYER;
}

bool IsClient()
{
	return !IsServer();
}

static BOOL FAR PASCAL DirectPlayEnumHandler(LPCDPSESSIONDESC2 lpThisSD,
	LPDWORD lpdwTimeOut,
	DWORD dwFlags,
	LPVOID lpContext)
{
	if (DPESC_TIMEDOUT == dwFlags || lpThisSD == nullptr)
	{
		//keep enumerating until we find something since CC3: CoI crashes if you tab out
		//_sessionEnumTimeout = true;
		return true;
	}

	if (lpThisSD->dwCurrentPlayers == 1 && lpThisSD->dwMaxPlayers == 2 &&
		(lpThisSD->dwFlags & (DPSESSION_JOINDISABLED | DPSESSION_NEWPLAYERSDISABLED)) == 0)
	{
		_theSession.dwSize = sizeof(_theSession);
		_theSession.guidInstance = lpThisSD->guidInstance;

		//showing notice here because I don't want to save string
		std::cout << "Found session " << lpThisSD->lpszSessionNameA << std::endl;
		return false;
	}
	
	return true;
}

void LogMessageContent(std::ostream& stream, const uint8_t* data, std::size_t len)
{
	for (std::size_t i = 0; i < len; ++i)
	{
		const uint8_t value = data[i];
		stream << (int)value << ' ';
	}
}

void LogMessageContent(std::ostream& stream, const std::vector<uint8_t>& messageBuffer)
{
	LogMessageContent(stream, messageBuffer.data(), messageBuffer.size());
}

void SendDirectPlayMessage(const uint8_t* data, std::size_t len)
{
	DPID toPlayer = DPID_SERVERPLAYER;
	if (IsServer())
	{
		toPlayer = DPID_ALLPLAYERS;
	}

	_directPlay->Send(_localPlayer, toPlayer, DPSEND_GUARANTEED, (LPVOID)data, len);

	_sendFile << "SendDirectPlayMessage(\"";
	LogMessageContent(_sendFile, data, len);
	_sendFile << "\");" << std::endl;
	_sendFile.flush();
}

void SendDirectPlayMessage(const std::vector<uint8_t>& data)
{
	SendDirectPlayMessage(data.data(), data.size());
}

void SendDirectPlayMessage(const char* byteStream)
{
	std::vector<uint8_t> fields;
	InformalByteWriter writer(fields);
	writer.WriteDescription(byteStream);

	SendDirectPlayMessage(fields.data(), fields.size());
}

void SendDirectPlayMessage(const std::string& str)
{
	SendDirectPlayMessage(str.c_str());
}

void LogDirectPlayMessage(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer)
{
	LOG("Received " << messageBuffer.size() << " byte message from " << fromPlayer << " to " << toPlayer << std::endl);

	LogMessageContent(_logFile, messageBuffer);
	_logFile << std::endl;

	_messageFile << "SendDirectPlayMessage(\"";

	LogMessageContent(_messageFile, messageBuffer);
	_messageFile << "\");";
	_messageFile << std::endl;

	//print ASCII
	for (uint8_t value : messageBuffer)
	{
		if (value < 32)
			value = '_';
		_logFile << (char)value;
	}

	_logFile << std::endl;

	_logFile.flush();
	_messageFile.flush();
}

static void OnSystemMessageReceived(DPID toPlayer, const DPMSG_GENERIC* sysMessage)
{
	_logFile << "Sys message " << GetSysMessageString(sysMessage->dwType) << " to player " << toPlayer << std::endl;
	
	switch (sysMessage->dwType)
	{
	case DPSYS_SESSIONLOST:
	{
		_running = false;
		break;
	}
	case DPSYS_CREATEPLAYERORGROUP:
	{
		const DPMSG_CREATEPLAYERORGROUP* const createPlayerOrGroup = (const DPMSG_CREATEPLAYERORGROUP*)sysMessage;

		assert(createPlayerOrGroup->dwPlayerType == DPPLAYERTYPE_PLAYER); //don't think CC3 uses groups
		assert(createPlayerOrGroup->dwCurrentPlayers == 2); //think this only happens when remote player is created

		if (IsServer())
		{
			SendFakeServerSetup();

			AttachToCloseCombat();
		}
		break;
	}
	default:
	{
		std::cout << "Unhandled sys message " << GetSysMessageString(sysMessage->dwType) << std::endl;
		break;
	}
	}
}

static void OnDirectPlayMessageReceived(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer)
{
	if (fromPlayer == DPID_SYSMSG)
	{
		const DPMSG_GENERIC* const sysMessage = (const DPMSG_GENERIC*)messageBuffer.data();
		OnSystemMessageReceived(toPlayer, sysMessage);
	}
	else
	{
		OnGameMessageReceived(toPlayer, fromPlayer, messageBuffer);
	}

	_logFile.flush();
}

#ifndef NDEBUG //to avoid needlessly scaring AV software
//need to get keyboard presses when CC3 is open since we can't tab out of game during debug
//TODO: the following didn't appear to work, might need different method
//could be DirectInput exclusive access being a bastard
static HHOOK _keyboardHook;
static LRESULT CALLBACK OnGlobalKeyboardEvent(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0 && wParam == WM_KEYDOWN)
	{
		const PKBDLLHOOKSTRUCT p = (PKBDLLHOOKSTRUCT)lParam;
		const int vkCode = p->vkCode;
		std::cout << "Pressed key " << vkCode << std::endl;
	}

	return CallNextHookEx(_keyboardHook, code, wParam, lParam);
}

static void HookKeyboard()
{
	//_keyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, OnGlobalKeyboardEvent, nullptr, 0);
}
#endif

static int SetupDirectPlay()
{
	GUID appGuid;
	const HRESULT guidResult = CLSIDFromString(L"{1872D778-E476-41CB-8C06-7DCE98BD2CB7}", (LPCLSID)&appGuid);
	if (guidResult != S_OK)
	{
		std::cerr << "Invalid DirectPlay application GUID\n";
		return 1;
	}

	if (FAILED(DirectPlayCreate((LPGUID)&DPSPGUID_TCPIP, &_baseDirectPlay, nullptr)))
	{
		std::cerr << "Failure to get base DirectPlay interface\n";
		return 2;
	}

	if (FAILED(_baseDirectPlay->QueryInterface(IID_IDirectPlay4A, (LPVOID*)&_directPlay)))
	{
		std::cerr << "Failure to query DirectPlay 4 interface\n";
		return 3;
	}

	if (AS_SERVER)
	{
		_theSession.dwSize = sizeof(_theSession);
		_theSession.guidApplication = appGuid;
		_theSession.dwMaxPlayers = 2;
		_theSession.lpszSessionNameA = (char*)"Fake Server";
		_theSession.dwFlags = 12384 & ~DPSESSION_JOINDISABLED;

		const HRESULT createServerResult = _directPlay->Open(&_theSession, DPOPEN_CREATESESSION);
		if (FAILED(createServerResult))
		{
			std::cerr << "Failed to create server: " << GetErrorString(createServerResult);
			return 8;
		}

		std::cout << "Successfully started server!\n";

		DPNAME playerName;
		std::memset(&playerName, 0, sizeof(playerName));
		playerName.dwSize = sizeof(playerName);
		playerName.lpszLongNameA = (char*)"Fake Dragon";
		playerName.lpszShortNameA = (char*)"Fake Dragon";
		const HRESULT createPlayerResult = _directPlay->CreatePlayer(&_localPlayer, &playerName, nullptr, nullptr, 0, DPPLAYER_SERVERPLAYER);
		if (FAILED(createPlayerResult))
		{
			std::cerr << "Failed to create local player: " << GetErrorString(createPlayerResult);
			return 9;
		}

		std::cout << "Created server player " << _localPlayer << std::endl;

		std::cout << "Running bot as fake server\n";
		_logFile << "Running bot as fake server\n";
	}
	else
	{
		std::cout << "The bot will automatically join your game after a few seconds once hosted.\n";
		std::cout << "Cross of Iron tends to crash when I tab out so I don't recommend doing that.\n";
		std::cout << "You can leave the dialog box field blank to search the local network.\n";
		DPSESSIONDESC2 sessionDesc;
		std::memset(&sessionDesc, 0, sizeof(sessionDesc));
		sessionDesc.dwSize = sizeof(sessionDesc);
		sessionDesc.guidApplication = appGuid;
		if (FAILED(_directPlay->EnumSessions(&sessionDesc, 0, DirectPlayEnumHandler, nullptr, DPENUMSESSIONS_ALL)))
		{
			std::cerr << "Failure to enumerate DirectPlay sessions\n";
			return 4;
		}

		if (_sessionEnumTimeout)
		{
			std::cerr << "Timed out searching for Close Combat 3 sessions\n";
			return 5;
		}

		std::cout << "Will now try to connect to the session\n";

		const HRESULT connectResult = _directPlay->Open(&_theSession, DPOPEN_JOIN);
		if (FAILED(connectResult))
		{
			std::cerr << "Failed to connect: " << GetErrorString(connectResult);
			return 6;
		}

		std::cout << "Successfully connected!\n";

		DPNAME playerName;
		std::memset(&playerName, 0, sizeof(playerName));
		playerName.dwSize = sizeof(playerName);
		playerName.lpszLongNameA = (char*)"Hidden Dragon";
		playerName.lpszShortNameA = (char*)"Hidden Dragon";
		const HRESULT createPlayerResult = _directPlay->CreatePlayer(&_localPlayer, &playerName, nullptr, nullptr, 0, 0);
		if (FAILED(createPlayerResult))
		{
			std::cerr << "Failed to create local player: " << GetErrorString(createPlayerResult);
			return 7;
		}

		std::cout << "Created local player " << _localPlayer << std::endl;

		std::cout << "Running bot as client\n";
		_logFile << "Running bot as client\n";

		SendDirectPlayMessage("20 0 0 0 6 0 0 0 4 0 0 0 1 0 0 0 ");

		//version message
		SendDirectPlayMessage("16 0 0 0 67 79 73 51 46 54 32 50 48 49 49 48 49 50 52 48 49 0 64 0 0 0 0 0 216 17 0 0 190 0 119 136 203 15 12 0 243 223 2 0 25 240 11 0 182 168 14 0 71 199 57 0 79 170 48 0 ");
	}

	return 0;
}

void DumpRequisition()
{
	_requisitionDump.Dump();
}

void DumpDeployment()
{
	_deploymentDump.Dump();
}

static void OnMainLoop()
{
	if (IsClient() && GetGameState() == GameState::Requisition)
	{
		if (_requisitionDump.GetElapsed() > 15)
		{
			_requisitionDump.Dump();
		}
	}
}

static int RunMainLoop()
{
	for (std::vector<uint8_t> messageBuffer; _running; messageBuffer.clear())
	{
		/*MSG msg;
		while (GetMessageA(&msg, nullptr, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}*/

		DPID fromPlayer, toPlayer;
		DWORD dataLength = 0;
		const HRESULT peekResult = _directPlay->Receive(&fromPlayer, &toPlayer, DPRECEIVE_PEEK, nullptr, &dataLength);
		if (peekResult != DPERR_NOMESSAGES && peekResult != DPERR_BUFFERTOOSMALL
			&& FAILED(peekResult))
		{
			std::cerr << "Failed to peek message: " << GetErrorString(peekResult);
			return 10;
		}
		else if (peekResult != DPERR_NOMESSAGES)
		{
			messageBuffer.resize(dataLength);
			const HRESULT receiveResult = _directPlay->Receive(&fromPlayer, &toPlayer, DPRECEIVE_ALL, &*messageBuffer.begin(), &dataLength);
			if (FAILED(receiveResult))
			{
				std::cerr << "Failed to receive message: " << GetErrorString(receiveResult);
				return 11;
			}

			OnDirectPlayMessageReceived(fromPlayer, toPlayer, messageBuffer);
		}

		OnMainLoop();

		Sleep(1);
	}

	return 0;
}

static void OnProgramExit()
{
	DetachFromCloseCombat();

	if (_directPlay)
		_directPlay->Release();
	if (_baseDirectPlay)
		_baseDirectPlay->Release();
#ifndef NDEBUG
	if (_keyboardHook)
		UnhookWindowsHookEx(_keyboardHook);
#endif
	CoUninitialize();
}

static BOOL EnableTokenPrivilege(LPCWSTR privilege)
{
	HANDLE hToken;
	TOKEN_PRIVILEGES token_privileges;
	DWORD dwSize;
	ZeroMemory(&token_privileges, sizeof(token_privileges));
	token_privileges.PrivilegeCount = 1;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &hToken))
		return FALSE;
	if (!LookupPrivilegeValue(NULL, privilege, &token_privileges.Privileges[0].Luid))
	{
		CloseHandle(hToken);
		return FALSE;
	}

	token_privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!AdjustTokenPrivileges(hToken, FALSE, &token_privileges, 0, NULL, &dwSize))
	{
		CloseHandle(hToken);
		return FALSE;
	}
	CloseHandle(hToken);
	return TRUE;
}

int main()
{
	atexit(OnProgramExit);
	CoInitializeEx(NULL, COINIT_MULTITHREADED);

	_logFile.rdbuf()->pubsetbuf(0, 0);
	_messageFile.rdbuf()->pubsetbuf(0, 0);
	_sendFile.rdbuf()->pubsetbuf(0, 0);

#ifndef NDEBUG
	HookKeyboard();
#endif

	std::cout << "Welcome to Hidden Dragon, a bot for Close Combat 3: Cross of Iron\n";

	if (!EnableTokenPrivilege(SE_DEBUG_NAME))
	{
		std::cerr << "Cannot get required privilege: " << GetLastError() << std::endl;
		return 15;
	}

	const int result = SetupDirectPlay();
	if (result != 0)
	{
		return result;
	}

	if (IsClient() && !AttachToCloseCombat())
	{
		std::cerr << "This is a critical error, bot won't be able to function without reading CC3.exe process memory.\n";
		return 12;
	}

	return RunMainLoop();
}
