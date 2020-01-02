// HiddenDragon.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"


#define TO_STRING(s) TO_STRING2(s)
#define TO_STRING2(s) #s

static IDirectPlay* _baseDirectPlay = nullptr;
static IDirectPlay4A* _directPlay = nullptr;
static DPSESSIONDESC2 _theSession;
static DPID _localPlayer;
static bool _sessionEnumTimeout;
static bool _running = true;
static std::ofstream _logFile("HiddenDragonLog.txt");

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

static void OnProgramExit()
{
	if (_directPlay)
		_directPlay->Release();
	if (_baseDirectPlay)
		_baseDirectPlay->Release();
	CoUninitialize();
}

static void OnDirectPlayMessageReceived(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer)
{
	std::cout << "Received " << messageBuffer.size() << " byte message from " << fromPlayer << " to " << toPlayer << std::endl;
	_logFile << "Received " << messageBuffer.size() << " byte message from " << fromPlayer << " to " << toPlayer << std::endl;
	for (const uint8_t value : messageBuffer)
	{
		_logFile << (int)value << ' ';
	}
	_logFile << std::endl;
	_logFile.flush();

	if (toPlayer == DPID_SYSMSG)
	{
		const DPMSG_GENERIC* const sysMessage = (const DPMSG_GENERIC*)messageBuffer.data();
		_logFile << "Sys message " << GetSysMessageString(sysMessage->dwType) << std::endl;
		switch (sysMessage->dwType)
		{
		case DPSYS_SESSIONLOST:
			_running = false;
			break;
		default:
			std::cout << "Unhandled sys message " << GetSysMessageString(sysMessage->dwType) << std::endl;
			break;
		}
	}
}

constexpr bool AS_SERVER = true; //fake server for tricking client
int main()
{
	atexit(OnProgramExit);
	CoInitializeEx(NULL, COINIT_MULTITHREADED);

	_logFile.rdbuf()->pubsetbuf(0, 0);

	std::cout << "Welcome to Hidden Dragon, a bot for Close Combat 3: Cross of Iron\n";

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
			return 6;
		}

		std::cout << "Successfully started server!\n";

		DPNAME playerName;
		std::memset(&playerName, 0, sizeof(playerName));
		playerName.dwSize = sizeof(playerName);
		playerName.lpszLongNameA = (char*)"Fake Server";
		playerName.lpszShortNameA = (char*)"Fake";
		const HRESULT createPlayerResult = _directPlay->CreatePlayer(&_localPlayer, &playerName, nullptr, nullptr, 0, DPPLAYER_SERVERPLAYER);
		if (FAILED(createPlayerResult))
		{
			std::cerr << "Failed to create local player: " << GetErrorString(createPlayerResult);
			return 7;
		}

		std::cout << "Created server player\n";

		std::cout << "Running bot as fake server\n";
		_logFile << "Running bot as fake server\n";
	}
	else
	{
		std::cout << "The bot will join your session ASAP once you host it\n";
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
		playerName.lpszShortNameA = (char*)"Dragon";
		const HRESULT createPlayerResult = _directPlay->CreatePlayer(&_localPlayer, &playerName, nullptr, nullptr, 0, 0);
		if (FAILED(createPlayerResult))
		{
			std::cerr << "Failed to create local player: " << GetErrorString(createPlayerResult);
			return 7;
		}

		std::cout << "Created local player\n";

		std::cout << "Running bot as client\n";
		_logFile << "Running bot as client\n";
	}

	for (std::vector<uint8_t> messageBuffer; _running; messageBuffer.clear())
	{
		DPID fromPlayer, toPlayer;
		DWORD dataLength = 0;
		const HRESULT peekResult =_directPlay->Receive(&fromPlayer, &toPlayer, DPRECEIVE_PEEK, nullptr, &dataLength);
		if (peekResult != DPERR_NOMESSAGES && peekResult != DPERR_BUFFERTOOSMALL
			&& FAILED(peekResult))
		{
			std::cerr << "Failed to peek message: " << GetErrorString(peekResult);
			return 8;
		}
		else if (peekResult != DPERR_NOMESSAGES)
		{
			messageBuffer.resize(dataLength);
			const HRESULT receiveResult = _directPlay->Receive(&fromPlayer, &toPlayer, DPRECEIVE_ALL, &*messageBuffer.begin(), &dataLength);
			if (FAILED(receiveResult))
			{
				std::cerr << "Failed to receive message: " << GetErrorString(receiveResult);
				return 8;
			}

			OnDirectPlayMessageReceived(fromPlayer, toPlayer, messageBuffer);
		}

		Sleep(1);
	}
}
