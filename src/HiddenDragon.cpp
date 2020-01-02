// HiddenDragon.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include <iostream>

#include <dplay.h>
#include <dpaddr.h>

#define TO_STRING(s) TO_STRING2(s)
#define TO_STRING2(s) #s

static IDirectPlay* _baseDirectPlay = nullptr;
static IDirectPlay4A* _directPlay = nullptr;
static DPSESSIONDESC2 _theSession;
static bool _sessionEnumTimeout;

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
	TEST_ERROR_CODE(DPERR_CANNOTCREATESERVER);
	TEST_ERROR_CODE(DPERR_CANTCREATEPLAYER);
	TEST_ERROR_CODE(DPERR_CANTLOADCAPI);
	TEST_ERROR_CODE(DPERR_CANTLOADSECURITYPACKAGE);
	TEST_ERROR_CODE(DPERR_CANTLOADSSPI);
	TEST_ERROR_CODE(DPERR_CONNECTING);
	TEST_ERROR_CODE(DPERR_CONNECTIONLOST);
	TEST_ERROR_CODE(DPERR_ENCRYPTIONFAILED);
	TEST_ERROR_CODE(DPERR_ENCRYPTIONNOTSUPPORTED);
	TEST_ERROR_CODE(DPERR_INVALIDFLAGS);
	TEST_ERROR_CODE(DPERR_INVALIDPARAMS);
	TEST_ERROR_CODE(DPERR_INVALIDPASSWORD);
	TEST_ERROR_CODE(DPERR_LOGONDENIED);
	TEST_ERROR_CODE(DPERR_NOCONNECTION);
	TEST_ERROR_CODE(DPERR_NONEWPLAYERS);
	TEST_ERROR_CODE(DPERR_NOSESSIONS);
	TEST_ERROR_CODE(DPERR_SIGNFAILED);
	TEST_ERROR_CODE(DPERR_TIMEOUT);
	TEST_ERROR_CODE(DPERR_UNINITIALIZED);
	TEST_ERROR_CODE(DPERR_USERCANCEL);
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
		//_sessionEnumTimeout = true;
		return true;
	}

	if (lpThisSD->dwCurrentPlayers == 1 && lpThisSD->dwMaxPlayers == 2 &&
		(lpThisSD->dwFlags & (DPSESSION_JOINDISABLED /*| DPSESSION_NEWPLAYERSDISABLED*/)) == 0)
	{
		_theSession.dwSize = sizeof(_theSession);
		//_theSession.guidApplication = lpThisSD->guidApplication;
		_theSession.guidInstance = lpThisSD->guidInstance;
		//_theSession.dwFlags = lpThisSD->dwFlags;

		//showing notice here because I don't want to save string
		std::cout << "Found session " << lpThisSD->lpszSessionNameA << std::endl;
		return false;
	}
	
	return true;
}

void OnProgramExit()
{
	_directPlay->Release();
	_baseDirectPlay->Release();
	CoUninitialize();
}

int main()
{
	atexit(OnProgramExit);
	CoInitializeEx(NULL, COINIT_MULTITHREADED);

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
}
