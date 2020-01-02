// HiddenDragon.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include <iostream>

#include <dplay.h>
#include <dpaddr.h>
#include <dxerr8.h>


IDirectPlay* _baseDirectPlay = nullptr;
IDirectPlay4A* _directPlay = nullptr;


static BOOL FAR PASCAL directPlayEnumHandler(LPCDPSESSIONDESC2   lpThisSD,
	LPDWORD             lpdwTimeOut,
	DWORD               dwFlags,
	LPVOID              lpContext)
{
	if (DPESC_TIMEDOUT == dwFlags || lpThisSD == nullptr)
	{
		return false;
	}

	std::cout << "Found session " << lpThisSD->lpszSessionNameA << std::endl;
	return false;
}

void onExit()
{
	_directPlay->Release();
	_baseDirectPlay->Release();
	CoUninitialize();
}

int main()
{
	atexit(onExit);
	CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::cout << "Welcome to Hidden Dragon, a bot for Close Combat 3: Cross of Iron\n";

	//{78d77218-76e4-cb41-8c06-7dce98bd2cb7}
	//{1872D778-E476-41CB-8C06-7DCE98BD2CB7}

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
		return 1;
	}

	if (FAILED(_baseDirectPlay->QueryInterface(IID_IDirectPlay4A, (LPVOID*)&_directPlay)))
	{
		std::cerr << "Failure to query DirectPlay 4 interface\n";
		return 1;
	}

	DPSESSIONDESC2 sessionDesc;
	std::memset(&sessionDesc, 0, sizeof(sessionDesc));
	sessionDesc.dwSize = sizeof(sessionDesc);
	sessionDesc.guidApplication = appGuid;
	if (FAILED(_directPlay->EnumSessions(&sessionDesc, 0, directPlayEnumHandler, nullptr, DPENUMSESSIONS_ALL)))
	{
		std::cerr << "Failure to enumerate DirectPlay sessions\n";
		return 1;
	}

	std::cout << "Successfully enumerated hosts\n";
}
