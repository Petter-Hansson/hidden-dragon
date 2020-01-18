#include "pch.h"

#include "GameData.hpp"
#include "MemoryScan.hpp"
#include "Util.hpp"


constexpr bool AS_SERVER = false; //fake server for tricking client

#define TO_STRING(s) TO_STRING2(s)
#define TO_STRING2(s) #s

enum class GameState
{
	Waiting,
	Requisition,
	Deployment,
	Battle
};

enum class Faction
{
	Germans,
	Russians,
};

static IDirectPlay* _baseDirectPlay = nullptr;
static IDirectPlay4A* _directPlay = nullptr;
static DPSESSIONDESC2 _theSession;
static DPID _localPlayer;
static bool _sessionEnumTimeout;
static bool _running = true;
static GameState _gameState;
static std::ofstream _logFile("HiddenDragonLog.txt");
static std::ofstream _messageFile("HiddenDragonMessages.txt");
static std::ofstream _sendFile("HiddenDragonSent.txt");
static Timer _requisitionDumpTimer;
static int _requisitionDumpCounter;

//it's unfortunately necessary to reverse engineer requisition logic
static struct RequisitionState
{
	int NumSoldiers = 0;
	SoldierData Soldiers[MaxSoldiersPerSide];

	int NumVehicles = 0;
	VehicleData Vehicles[MaxVehiclesPerSide];

	int NumTeams = 0;
	TeamData Teams[MaxTeamsPerSide];

	void CountSoldiers() //until I find memory offset directly
	{
		NumSoldiers = 0;

		for (int i = 0; i < MaxSoldiersPerSide; ++i)
		{
			const SoldierData& soldier = Soldiers[i];

			if (std::strcmp(soldier.Name, "") == 0 ||
				std::strcmp(soldier.Name, "Unknown") == 0)
			{
				break;
			}

			NumSoldiers += 1;
		}
	}
	void CountVehicles() //until I find memory offset directly
	{
		NumVehicles = 0;

		for (int i = 0; i < MaxVehiclesPerSide; ++i)
		{
			const VehicleData& vehicle = Vehicles[i];

			if (std::strcmp(vehicle.Name, "") == 0 ||
				std::strcmp(vehicle.Name, "Unknown") == 0)
			{
				break;
			}

			NumVehicles += 1;
		}
	}
	void CountTeams() //until I find memory offset directly
	{
		NumTeams = 0;

		for (int i = 0; i < MaxTeamsPerSide; ++i)
		{
			const TeamData& team = Teams[i];

			if (std::strcmp(team.Name, "") == 0 ||
				std::strcmp(team.Name, "Unknown") == 0)
			{
				break;
			}

			NumTeams += 1;
		}
	}
} _requisitionState;

#define LOG(x) std::cout << x; _logFile << x

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

static const char* GetGameStateString(GameState state)
{
	switch (state)
	{
	case GameState::Battle:
		return "Battle";
	case GameState::Deployment:
		return "Deployment";
	case GameState::Requisition:
		return "Requisition";
	case GameState::Waiting:
		return "Waiting";
	default:
		return "Unknown";
	}
}

static bool IsServer()
{
	return _localPlayer == DPID_SERVERPLAYER;
}

static bool IsClient()
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

static void LogMessageContent(std::ostream& stream, const uint8_t* data, std::size_t len)
{
	for (std::size_t i = 0; i < len; ++i)
	{
		const uint8_t value = data[i];
		stream << (int)value << ' ';
	}
}

static void LogMessageContent(std::ostream& stream, const std::vector<uint8_t>& messageBuffer)
{
	LogMessageContent(stream, messageBuffer.data(), messageBuffer.size());
}

static void SendDirectPlayMessage(const uint8_t* data, std::size_t len)
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

static void SendDirectPlayMessage(const std::vector<uint8_t>& data)
{
	SendDirectPlayMessage(data.data(), data.size());
}

static void SendDirectPlayMessage(const char* byteStream)
{
	std::vector<uint8_t> fields;
	InformalByteWriter writer(fields);
	writer.WriteDescription(byteStream);

	SendDirectPlayMessage(fields.data(), fields.size());
}

static void SendDirectPlayMessage(const std::string& str)
{
	SendDirectPlayMessage(str.c_str());
}

static void LogDirectPlayMessage(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer)
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

static void SendFlee()
{
	LOG("Fleeing!\n");
	if (IsClient())
	{
		//SendDirectPlayMessage("8 0 0 0 1 0 0 0 ");
	}
	else
	{
		SendDirectPlayMessage("24 0 0 0 8 205 107 2 ");
		SendDirectPlayMessage("24 0 0 0 247 205 107 2 ");
		SendDirectPlayMessage("14 0 0 0 12 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 1 0 255 255 255 0 0 0 0 5 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 255 0 0 0 0 0 2 0 3 0 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
		SendDirectPlayMessage("25 0 0 0 ");
		SendDirectPlayMessage("16 0 0 0 5 0 0 0 0 0 1 36 0 0 1 151 ");
	}
}

static void SetGameState(GameState newState)
{
	if (newState == _gameState)
		return;

	LOG("Switching game state from " << GetGameStateString(_gameState) << " to " << GetGameStateString(newState) << std::endl);

	_gameState = newState;
}

static void SendClientUnitData()
{
	LOG("Sending client data\n");

	std::vector<uint8_t> message;
	InformalByteWriter writer(message);

	const int numSoldiers = _requisitionState.NumSoldiers;
	const int numTeams = _requisitionState.NumTeams;
	const int numVehicles = _requisitionState.NumVehicles;

	message.clear();
	writer.WriteDescription("18 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	writer.WriteBytes((uint16_t)numTeams);
	writer.WriteBytes((uint16_t)numSoldiers);
	writer.WriteDescription(" 1 231 9 0 ");
	SendDirectPlayMessage(message);

	for (int i = 0; i < numSoldiers; ++i)
	{
		const SoldierData& soldier = _requisitionState.Soldiers[i];

		auto teamEnd = _requisitionState.Teams + _requisitionState.NumTeams;
		auto teamIt = std::find_if(_requisitionState.Teams, teamEnd,
			[=](const TeamData& team)
		{
			return Contains(team.Soldiers, team.Soldiers + MaxSoldiersPerTeam, i);
		});
		if (teamIt == teamEnd)
		{
			std::cerr << "No team link for soldier " << i << " (" << soldier.Name << ")\n";
			continue;
		}
		const auto teamIndex = std::distance(_requisitionState.Teams, teamIt);

		message.clear();
		writer.WriteBytes((uint32_t)22);
		writer.WriteBytes((uint32_t)i);
		writer.WriteBytes((uint16_t)i);
		writer.WriteBytes((uint8_t)0);
		writer.WriteString(soldier.Name, sizeof(soldier.Name));
		writer.WriteDescription("2 1 2 236 0 0 0 23 1 0 0 26 1 0 0 0 1 0 0 22 1 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
		writer.WriteBytes((uint32_t)teamIndex); //TODO: this is NOT team index, might be rank but must be verified
		SendDirectPlayMessage(message);
	}

	for (int i = 0; i < numVehicles; ++i)
	{
		const VehicleData& vehicle = _requisitionState.Vehicles[i];
		message.clear();
		writer.WriteBytes((uint32_t)23);
		writer.WriteBytes((uint32_t)i);
		writer.WriteBytes((uint16_t)i);
		writer.WriteString(vehicle.Name, sizeof(vehicle.Name));
		writer.WriteDescription("68 0 0 0 0 0 0 0 0 231 9 0 ");
		SendDirectPlayMessage(message);
	}

	for (int i = 0; i < numTeams; ++i)
	{
		const TeamData& team = _requisitionState.Teams[i];
		message.clear();
		writer.WriteBytes((uint32_t)24);
		writer.WriteBytes((uint32_t)i);
		writer.WriteBytes((uint16_t)i);
		writer.WriteBytes((uint8_t)0);
		writer.WriteString(team.Name, 12); //TODO: see why 13 != 26
		writer.WriteBytes((uint8_t)0); //null terminator of above
		writer.WriteDescription("255 255 255 255 0 0 0 0 157 14 73 0 ");
		writer.WriteBytes((uint8_t)team.Type);
		writer.WriteBytes((uint8_t)0);
		for (int i = 0; i < MaxSoldiersPerTeam; ++i)
			writer.WriteBytes((uint16_t)team.Soldiers[i]);
		writer.WriteBytes((uint8_t)team.VehicleIndex);
		writer.WriteDescription("105 27 1 0 0 3 1 0 0 8 0 111 98 27 1 0 0 0 51 0 0 0 0 0 0 ");
		SendDirectPlayMessage(message);
	}

	//these messages don't appear to change on adding a team
	SendDirectPlayMessage("17 0 0 0 12 0 73 0 ");
	SendDirectPlayMessage("21 0 0 0 ");
	SendDirectPlayMessage("17 0 0 0 10 0 73 0 ");
	SendDirectPlayMessage("6 0 0 0 254 67 73 0 ");
}

//unfortunately a lot of data we must figure out and send
static void SendClientUnitDataExample()
{
	//last fields of message 18 here are "10 0 64 0 1 231 9 0"
	//where 10 = requisition points remaining
	//64 added by 1 if adding sniper
	//9 added by 1 if adding sniper
	SendDirectPlayMessage("18 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 10 0 64 0 1 231 9 0 ");
	
	//message 22 specifies a soldier
	//first soldier is the commander (helps debugging since name is always the same)
	//potentially random fields are 75 to 91 ("75 1 0 0 94 1 0 0 41 1 0 0 91" below)
	SendDirectPlayMessage("22 0 0 0 0 0 0 0 0 0 0 80 101 116 116 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 4 97 1 0 0 75 1 0 0 94 1 0 0 41 1 0 0 91 1 0 0 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 0 0 0 ");
	
	//other soldiers, additional random field is the name field which is necessarily fixed size 
	//TODO: find out what is morale, experience, fatigue, strength, leadership ability, etc.
	//also we have kills and medals here probably which are necessarily zero for a new battle/campaign
	//there are (for this data) 105 rows in Solidiers.txt and I at least expect to find an index row for that
	//potentially first big endian DWORD is a general index into soldier arra, and it may be duplicated a second time...
	SendDirectPlayMessage("22 0 0 0 1 0 0 0 1 0 0 83 111 108 111 109 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 1 2 236 0 0 0 23 1 0 0 26 1 0 0 0 1 0 0 22 1 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 2 0 0 0 2 0 0 80 108 97 115 116 111 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 2 0 152 0 0 0 64 1 0 0 2 1 0 0 229 0 0 0 229 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 3 0 0 0 3 0 0 83 104 101 114 115 116 105 117 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 3 31 1 0 0 227 0 0 0 238 0 0 0 2 1 0 0 41 1 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 4 0 0 0 4 0 0 71 111 110 116 97 114 0 117 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 1 2 231 0 0 0 3 1 0 0 240 0 0 0 247 0 0 0 14 1 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 5 0 0 0 5 0 0 83 104 107 108 105 97 114 0 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 2 1 167 0 0 0 243 0 0 0 206 0 0 0 228 0 0 0 233 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 6 0 0 0 6 0 0 83 104 105 114 107 117 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 6 0 118 0 0 0 27 1 0 0 251 0 0 0 177 0 0 0 177 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 7 0 0 0 7 0 0 67 104 101 108 105 97 100 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 7 0 136 0 0 0 245 0 0 0 253 0 0 0 204 0 0 0 204 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 8 0 0 0 8 0 0 77 101 108 110 105 107 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 8 0 129 0 0 0 238 0 0 0 183 0 0 0 194 0 0 0 194 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 9 0 0 0 9 0 0 75 104 111 108 111 100 111 118 105 99 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 3 1 147 0 0 0 48 1 0 0 228 0 0 0 197 0 0 0 203 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 10 0 0 0 10 0 0 80 114 111 107 111 102 121 101 118 0 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 4 1 161 0 0 0 10 1 0 0 254 0 0 0 218 0 0 0 223 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 11 0 0 0 11 0 0 77 111 110 97 115 116 105 114 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 59 5 1 161 0 0 0 15 1 0 0 10 1 0 0 218 0 0 0 223 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 12 0 0 0 12 0 0 82 117 118 105 110 115 107 105 121 0 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 38 9 0 128 0 0 0 41 1 0 0 233 0 0 0 192 0 0 0 192 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 13 0 0 0 13 0 0 77 111 115 101 105 99 104 117 107 0 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 3 27 1 0 0 36 1 0 0 10 1 0 0 248 0 0 0 33 1 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 14 0 0 0 14 0 0 66 114 111 100 111 118 0 117 107 0 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 1 2 220 0 0 0 56 1 0 0 226 0 0 0 225 0 0 0 250 0 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 15 0 0 0 15 0 0 78 105 122 97 109 111 118 0 107 0 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 38 9 0 150 0 0 0 38 1 0 0 202 0 0 0 226 0 0 0 226 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 16 0 0 0 16 0 0 66 111 114 105 115 111 118 0 107 0 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 2 1 157 0 0 0 27 1 0 0 204 0 0 0 212 0 0 0 217 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 17 0 0 0 17 0 0 84 117 114 99 104 97 110 105 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 3 1 161 0 0 0 4 1 0 0 223 0 0 0 219 0 0 0 224 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 18 0 0 0 18 0 0 66 101 115 115 111 110 111 118 0 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 4 1 165 0 0 0 228 0 0 0 212 0 0 0 224 0 0 0 229 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 19 0 0 0 19 0 0 70 105 108 105 112 111 118 105 99 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 59 5 1 143 0 0 0 243 0 0 0 250 0 0 0 191 0 0 0 197 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 20 0 0 0 20 0 0 75 111 107 117 115 104 107 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 7 0 131 0 0 0 235 0 0 0 222 0 0 0 197 0 0 0 197 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 21 0 0 0 21 0 0 83 104 97 114 97 101 118 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 8 0 142 0 0 0 224 0 0 0 208 0 0 0 214 0 0 0 214 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 22 0 0 0 22 0 0 90 97 105 107 105 110 0 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 6 1 155 0 0 0 24 1 0 0 218 0 0 0 210 0 0 0 215 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 23 0 0 0 23 0 0 75 97 108 109 97 107 111 118 0 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 3 35 1 0 0 38 1 0 0 4 1 0 0 12 1 0 0 50 1 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 24 0 0 0 24 0 0 77 97 116 118 101 121 101 118 0 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 59 1 2 211 0 0 0 20 1 0 0 249 0 0 0 208 0 0 0 234 0 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 25 0 0 0 25 0 0 68 121 97 100 101 110 99 104 117 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 2 1 155 0 0 0 18 1 0 0 238 0 0 0 210 0 0 0 215 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 26 0 0 0 26 0 0 75 111 114 110 101 101 118 0 117 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 8 0 126 0 0 0 26 1 0 0 251 0 0 0 190 0 0 0 190 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 27 0 0 0 27 0 0 75 111 107 111 114 105 110 0 117 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 7 0 126 0 0 0 228 0 0 0 198 0 0 0 190 0 0 0 190 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 28 0 0 0 28 0 0 90 118 101 114 101 118 0 0 117 107 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 3 1 169 0 0 0 244 0 0 0 219 0 0 0 232 0 0 0 237 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 29 0 0 0 29 0 0 67 104 101 114 110 105 97 118 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 6 0 132 0 0 0 241 0 0 0 13 1 0 0 198 0 0 0 198 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 30 0 0 0 30 0 0 84 97 114 97 115 111 118 0 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 4 1 153 0 0 0 41 1 0 0 236 0 0 0 207 0 0 0 212 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 31 0 0 0 31 0 0 71 114 101 103 117 108 0 0 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 5 1 142 0 0 0 28 1 0 0 201 0 0 0 188 0 0 0 194 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 32 0 0 0 32 0 0 80 111 108 105 101 118 115 107 105 121 0 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 9 0 123 0 0 0 8 1 0 0 228 0 0 0 185 0 0 0 185 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 33 0 0 0 33 0 0 86 97 114 102 97 108 111 109 101 121 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 3 27 1 0 0 12 1 0 0 22 1 0 0 248 0 0 0 33 1 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 34 0 0 0 34 0 0 70 111 109 105 110 0 111 109 101 121 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 59 1 2 217 0 0 0 3 1 0 0 230 0 0 0 221 0 0 0 246 0 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 35 0 0 0 35 0 0 83 109 111 108 110 105 107 111 118 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 9 0 144 0 0 0 36 1 0 0 193 0 0 0 217 0 0 0 217 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 36 0 0 0 36 0 0 75 111 115 116 105 110 0 111 118 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 2 1 153 0 0 0 14 1 0 0 17 1 0 0 206 0 0 0 211 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 37 0 0 0 37 0 0 66 117 107 97 110 111 118 0 118 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 3 1 155 0 0 0 5 1 0 0 204 0 0 0 210 0 0 0 215 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 38 0 0 0 38 0 0 80 114 111 115 101 107 111 118 0 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 4 1 157 0 0 0 42 1 0 0 191 0 0 0 213 0 0 0 218 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 39 0 0 0 39 0 0 80 114 111 114 111 99 104 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 5 1 151 0 0 0 52 1 0 0 202 0 0 0 203 0 0 0 209 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 40 0 0 0 40 0 0 86 97 114 97 107 115 105 110 0 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 7 0 119 0 0 0 27 1 0 0 188 0 0 0 179 0 0 0 179 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 41 0 0 0 41 0 0 82 111 103 111 122 104 107 105 110 0 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 8 0 149 0 0 0 31 1 0 0 239 0 0 0 224 0 0 0 224 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 42 0 0 0 42 0 0 66 111 99 104 97 114 110 105 107 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 6 1 149 0 0 0 15 1 0 0 213 0 0 0 200 0 0 0 206 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 43 0 0 0 43 0 0 75 105 114 101 121 101 118 0 107 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 3 28 1 0 0 241 0 0 0 49 1 0 0 252 0 0 0 36 1 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 44 0 0 0 44 0 0 76 111 109 116 101 118 0 0 107 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 59 1 2 211 0 0 0 33 1 0 0 230 0 0 0 209 0 0 0 235 0 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 45 0 0 0 45 0 0 75 111 108 116 121 115 104 101 118 0 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 2 1 150 0 0 0 26 1 0 0 201 0 0 0 201 0 0 0 207 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 46 0 0 0 46 0 0 89 101 118 115 116 105 103 110 101 121 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 7 0 138 0 0 0 4 1 0 0 4 1 0 0 207 0 0 0 207 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 47 0 0 0 47 0 0 73 115 99 104 101 110 107 111 0 121 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 3 1 144 0 0 0 245 0 0 0 217 0 0 0 193 0 0 0 199 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 48 0 0 0 48 0 0 86 105 115 111 116 115 107 105 121 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 8 0 128 0 0 0 228 0 0 0 253 0 0 0 192 0 0 0 192 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 49 0 0 0 49 0 0 82 105 117 99 104 105 110 0 121 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 6 0 120 0 0 0 231 0 0 0 14 1 0 0 180 0 0 0 180 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 50 0 0 0 50 0 0 80 97 110 107 114 97 116 111 118 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 4 1 163 0 0 0 250 0 0 0 235 0 0 0 222 0 0 0 227 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 51 0 0 0 51 0 0 89 101 115 105 110 0 116 111 118 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 16 5 1 157 0 0 0 224 0 0 0 205 0 0 0 213 0 0 0 218 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 52 0 0 0 52 0 0 77 101 122 101 110 116 115 101 118 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 9 0 132 0 0 0 249 0 0 0 181 0 0 0 198 0 0 0 198 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 53 0 0 0 53 0 0 80 97 110 102 105 108 111 118 0 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 0 2 249 0 0 0 0 1 0 0 32 1 0 0 24 1 0 0 44 1 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 54 0 0 0 54 0 0 80 101 116 114 105 99 104 0 0 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 71 2 0 175 0 0 0 255 0 0 0 215 0 0 0 7 1 0 0 7 1 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 55 0 0 0 55 0 0 71 117 112 107 111 0 104 0 0 0 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 8 1 1 167 0 0 0 44 1 0 0 38 1 0 0 228 0 0 0 233 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 56 0 0 0 56 0 0 90 97 98 111 108 111 116 110 121 107 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 0 2 230 0 0 0 18 1 0 0 52 1 0 0 245 0 0 0 12 1 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 57 0 0 0 57 0 0 80 97 116 114 105 110 0 110 121 107 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 1 1 177 0 0 0 248 0 0 0 1 1 0 0 244 0 0 0 249 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 8 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 58 0 0 0 58 0 0 83 104 117 118 97 108 111 118 0 107 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 3 0 146 0 0 0 72 1 0 0 220 0 0 0 219 0 0 0 219 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 59 0 0 0 59 0 0 90 97 114 101 116 115 107 105 107 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 2 1 169 0 0 0 68 1 0 0 231 0 0 0 232 0 0 0 237 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 60 0 0 0 60 0 0 90 97 110 105 110 0 107 105 107 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 13 0 2 236 0 0 0 9 1 0 0 14 1 0 0 1 1 0 0 23 1 0 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 61 0 0 0 61 0 0 71 111 109 111 118 0 107 105 107 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 1 0 143 0 0 0 250 0 0 0 24 1 0 0 215 0 0 0 215 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 62 0 0 0 62 0 0 76 105 115 99 104 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 2 0 146 0 0 0 62 1 0 0 219 0 0 0 220 0 0 0 220 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 8 0 0 0 ");
	SendDirectPlayMessage("22 0 0 0 63 0 0 0 63 0 0 65 104 109 97 100 111 118 0 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 70 3 0 142 0 0 0 62 1 0 0 231 0 0 0 213 0 0 0 213 0 0 0 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 9 0 0 0 ");
	
	//message 23 might be vehicle team list, unfortunately a bit hard to confirm on default scenario
	//second last field of message 23 is 9, which is added to by 1 when sniper is added
	SendDirectPlayMessage("23 0 0 0 0 0 0 0 0 0 52 53 109 109 32 80 114 111 116 105 118 46 32 79 114 117 100 105 101 32 111 98 114 46 51 50 47 51 56 0 68 0 0 0 0 0 0 0 0 231 9 0 ");
	
	//messages 24 specifies a team
	//first two big endian DWORDS seem to be duplicated team indices like for soldiers
	//teams seem to be inserted into the order they are displayed in the game
	//expecting there to be a row index into RUTeams.txt, but a bit unsure if GETeams.txt and RUTeams.txt are somehow appended into one
	//suspect e.g. substring "33 0 34 0 36 0 37 0 38 0 39 0 42 0 40 0 41 0 35 0" below are soldier indices
	SendDirectPlayMessage("24 0 0 0 0 0 0 0 0 0 0 71 114 111 117 112 32 76 101 97 100 101 114 0 255 255 255 255 0 0 0 0 157 14 73 0 79 0 0 0 1 0 2 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 105 27 1 0 0 3 1 0 0 8 0 111 98 27 1 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 1 0 0 0 1 0 0 76 77 71 32 73 110 102 97 110 116 114 121 0 255 255 255 255 0 0 0 0 157 14 73 0 22 0 3 0 4 0 5 0 9 0 10 0 11 0 6 0 7 0 8 0 12 0 255 105 221 0 0 0 213 0 0 0 11 0 111 98 221 0 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 2 0 0 0 2 0 0 76 77 71 32 73 110 102 97 110 116 114 121 0 255 255 255 255 0 0 0 0 157 14 73 0 22 0 13 0 14 0 16 0 17 0 18 0 19 0 22 0 20 0 21 0 15 0 255 105 225 0 0 0 216 0 0 0 11 0 111 98 225 0 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 3 0 0 0 3 0 0 76 105 103 104 116 32 73 110 102 97 110 116 114 121 0 255 255 0 0 0 0 157 14 73 0 7 0 23 0 24 0 25 0 28 0 30 0 31 0 29 0 27 0 26 0 32 0 255 105 216 0 0 0 207 0 0 0 9 0 111 98 216 0 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 4 0 0 0 4 0 0 76 105 103 104 116 32 73 110 102 97 110 116 114 121 0 255 255 0 0 0 0 157 14 73 0 7 0 33 0 34 0 36 0 37 0 38 0 39 0 42 0 40 0 41 0 35 0 255 105 221 0 0 0 212 0 0 0 9 0 111 98 221 0 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 5 0 0 0 5 0 0 76 105 103 104 116 32 73 110 102 97 110 116 114 121 0 255 255 0 0 0 0 157 14 73 0 7 0 43 0 44 0 45 0 47 0 50 0 51 0 49 0 46 0 48 0 52 0 255 105 215 0 0 0 206 0 0 0 9 0 111 98 215 0 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 6 0 0 0 6 0 0 77 77 71 32 84 101 97 109 0 97 110 116 114 121 0 255 255 0 0 0 0 157 14 73 0 4 0 53 0 55 0 54 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 105 9 1 0 0 1 1 0 0 7 0 111 98 9 1 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 7 0 0 0 7 0 0 52 53 109 109 32 65 84 32 71 117 110 0 114 121 0 255 255 0 0 0 0 157 14 73 0 33 0 56 0 57 0 59 0 58 0 255 255 255 255 255 255 255 255 255 255 255 255 0 105 243 0 0 0 235 0 0 0 17 0 111 98 243 0 0 0 0 51 0 0 0 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 8 0 0 0 8 0 0 56 50 109 109 32 77 111 114 116 97 114 0 114 121 0 255 255 0 0 0 0 157 14 73 0 6 0 60 0 61 0 62 0 63 0 255 255 255 255 255 255 255 255 255 255 255 255 255 105 231 0 0 0 226 0 0 0 10 0 111 98 231 0 0 0 0 51 0 0 0 0 0 0 ");

	//these messages don't appear to change on adding a team
	SendDirectPlayMessage("17 0 0 0 12 0 73 0 ");
	SendDirectPlayMessage("21 0 0 0 ");
	SendDirectPlayMessage("17 0 0 0 10 0 73 0 ");
	SendDirectPlayMessage("6 0 0 0 254 67 73 0 ");
}

static void SendServerUnitData()
{
	SendDirectPlayMessage("3 0 0 0 0 64 0 42 0 9 0 9 0 65 0 42 0 1 0 0 1 71 25 0 ");
	SendDirectPlayMessage("2 0 0 0 0 0 0 16 4 0 0 0 0 0 80 101 116 116 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 149 131 102 44 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 3 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 133 3 5 6 255 133 25 0 1 0 0 0 58 0 0 0 133 3 5 0 0 0 0 0 1 0 0 0 54 0 0 0 133 3 5 0 0 0 0 0 1 0 0 0 0 0 6 2 28 1 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 1 0 0 16 3 0 0 0 0 0 80 105 115 116 114 105 99 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 21 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 4 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 133 3 3 6 255 133 25 0 1 0 0 0 66 0 0 0 133 3 3 0 0 0 0 0 1 0 0 0 58 0 0 0 133 3 3 0 0 0 0 0 1 0 0 0 0 0 6 2 28 1 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 2 0 0 16 0 0 0 0 0 0 82 121 97 98 117 115 104 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 225 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 5 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 133 3 11 6 255 133 25 0 1 0 0 0 71 0 0 0 133 3 11 0 0 0 0 0 1 0 0 0 64 0 0 0 133 3 11 0 0 0 0 0 1 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 3 0 0 16 1 0 0 0 1 0 75 101 114 101 115 116 101 115 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 234 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 6 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 77 4 3 6 255 133 25 0 1 0 0 0 59 0 0 0 77 4 3 0 0 0 0 0 1 0 0 0 72 0 0 0 77 4 3 0 0 0 0 0 1 0 0 0 0 0 6 2 28 1 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 4 0 0 16 3 0 0 0 1 0 80 111 108 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 248 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 7 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 78 4 5 6 255 133 25 0 1 0 0 0 57 0 0 0 78 4 5 0 0 0 0 0 1 0 0 0 73 0 0 0 78 4 5 0 0 0 0 0 1 0 0 0 0 0 6 2 28 1 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 5 0 0 16 2 0 0 0 1 0 89 97 115 101 110 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 249 0 0 0 0 0 0 0 0 0 1 0 0 0 13 0 255 255 14 0 1 0 0 0 1 0 0 0 1 4 0 0 78 4 1 6 255 133 25 0 1 0 0 0 71 0 0 0 78 4 1 0 0 0 0 0 1 0 0 0 75 0 0 0 78 4 1 0 0 0 0 0 0 0 0 0 0 0 6 2 26 1 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 6 0 0 16 2 0 0 0 1 0 66 97 122 117 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 216 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 9 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 78 4 0 6 255 133 25 0 1 0 0 0 71 0 0 0 78 4 0 0 0 0 0 0 1 0 0 0 66 0 0 0 78 4 0 0 0 0 0 0 1 0 0 0 0 0 6 2 50 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 7 0 0 16 0 0 0 0 1 0 80 111 110 111 109 97 114 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 220 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 10 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 12 4 12 6 255 133 25 0 1 0 0 0 54 0 0 0 12 4 12 0 0 0 0 0 1 0 0 0 51 0 0 0 12 4 12 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 8 0 0 16 0 0 0 0 1 0 80 111 108 105 118 101 122 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 183 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 11 0 12 0 1 0 0 0 1 0 0 0 1 4 0 0 12 4 14 6 255 133 25 0 1 0 0 0 60 0 0 0 12 4 14 0 0 0 0 0 1 0 0 0 64 0 0 0 12 4 14 0 0 0 0 0 1 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 9 0 0 16 0 0 0 0 1 0 75 104 97 115 97 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 191 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 8 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 78 4 12 6 255 133 25 0 1 0 0 0 71 0 0 0 78 4 12 0 0 0 0 0 1 0 0 0 55 0 0 0 78 4 12 0 0 0 0 0 1 0 0 0 0 0 6 2 35 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 10 0 0 16 0 0 0 0 1 0 83 104 107 108 105 97 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 204 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 15 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 78 4 14 6 255 133 25 0 1 0 0 0 64 0 0 0 78 4 14 0 0 0 0 0 1 0 0 0 66 0 0 0 78 4 14 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 11 0 0 16 0 0 0 0 1 0 66 101 103 111 121 97 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 197 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 16 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 77 4 11 6 255 133 25 0 1 0 0 0 62 0 0 0 77 4 11 0 0 0 0 0 1 0 0 0 72 0 0 0 77 4 11 0 0 0 0 0 1 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 12 0 0 16 0 0 0 0 1 0 68 117 108 101 98 105 110 101 116 115 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 209 0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 255 255 18 0 1 0 0 0 1 0 0 0 1 4 0 0 11 4 15 6 255 133 25 0 1 0 0 0 61 0 0 0 11 4 15 0 0 0 0 0 1 0 0 0 74 0 0 0 11 4 15 0 0 0 0 0 0 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 13 0 0 16 1 0 0 0 2 0 80 101 116 114 111 99 104 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 252 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 19 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 113 2 13 6 255 133 25 0 1 0 0 0 70 0 0 0 113 2 13 0 0 0 0 0 1 0 0 0 75 0 0 0 113 2 13 0 0 0 0 0 1 0 0 0 0 0 6 2 213 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 14 0 0 16 3 0 0 0 2 0 66 97 108 97 115 104 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 229 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 20 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 179 2 5 6 255 133 25 0 1 0 0 0 65 0 0 0 179 2 5 0 0 0 0 0 1 0 0 0 69 0 0 0 179 2 5 0 0 0 0 0 1 0 0 0 0 0 6 2 213 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 15 0 0 16 2 0 0 0 2 0 80 117 116 105 108 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 0 1 0 0 0 0 0 0 0 0 0 0 0 0 21 0 255 255 22 0 1 0 0 0 1 0 0 0 1 4 0 0 179 2 1 6 255 133 25 0 1 0 0 0 63 0 0 0 179 2 1 0 0 0 0 0 1 0 0 0 57 0 0 0 179 2 1 0 0 0 0 0 0 0 0 0 0 0 6 2 94 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 16 0 0 16 2 0 0 0 2 0 84 114 117 115 104 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 182 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 23 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 179 2 0 6 255 133 25 0 1 0 0 0 60 0 0 0 179 2 0 0 0 0 0 0 1 0 0 0 67 0 0 0 179 2 0 0 0 0 0 0 1 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 17 0 0 16 0 0 0 0 2 0 67 104 105 114 107 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 188 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 24 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 178 2 3 6 255 133 25 0 1 0 0 0 53 0 0 0 178 2 3 0 0 0 0 0 1 0 0 0 60 0 0 0 178 2 3 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 18 0 0 16 0 0 0 0 2 0 71 111 110 116 97 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 223 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 25 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 179 2 3 6 255 133 25 0 1 0 0 0 65 0 0 0 179 2 3 0 0 0 0 0 1 0 0 0 57 0 0 0 179 2 3 0 0 0 0 0 1 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 19 0 0 16 0 0 0 0 2 0 66 101 114 101 122 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 221 0 0 0 0 0 0 0 0 0 0 0 0 0 26 0 255 255 27 0 1 0 0 0 1 0 0 0 1 4 0 0 113 2 15 6 255 133 25 0 1 0 0 0 65 0 0 0 113 2 15 0 0 0 0 0 1 0 0 0 60 0 0 0 113 2 15 0 0 0 0 0 0 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 20 0 0 16 0 0 0 0 2 0 83 104 117 109 105 108 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 227 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 28 0 29 0 1 0 0 0 1 0 0 0 1 4 0 0 179 2 11 6 255 133 25 0 1 0 0 0 59 0 0 0 179 2 11 0 0 0 0 0 1 0 0 0 70 0 0 0 179 2 11 0 0 0 0 0 1 0 0 0 0 0 6 2 55 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 21 0 0 16 0 0 0 0 2 0 80 114 111 114 111 99 104 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 209 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 30 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 178 2 11 6 255 133 25 0 1 0 0 0 72 0 0 0 178 2 11 0 0 0 0 0 1 0 0 0 55 0 0 0 178 2 11 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 22 0 0 16 0 0 0 0 2 0 83 104 101 109 101 116 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 189 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 31 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 179 2 12 6 255 133 25 0 1 0 0 0 62 0 0 0 179 2 12 0 0 0 0 0 1 0 0 0 64 0 0 0 179 2 12 0 0 0 0 0 1 0 0 0 0 0 6 2 35 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 23 0 0 16 1 0 0 0 3 0 71 97 108 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 236 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 32 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 191 2 5 6 255 133 25 0 1 0 0 0 61 0 0 0 191 2 5 0 0 0 0 0 1 0 0 0 59 0 0 0 191 2 5 0 0 0 0 0 1 0 0 0 0 0 6 2 213 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 24 0 0 16 3 0 0 0 3 0 71 114 101 118 110 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 246 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 33 0 34 0 1 0 0 0 1 0 0 0 1 4 0 0 191 2 3 6 255 133 25 0 1 0 0 0 75 0 0 0 191 2 3 0 0 0 0 0 1 0 0 0 69 0 0 0 191 2 3 0 0 0 0 0 1 0 0 0 0 0 6 2 55 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 25 0 0 16 0 0 0 0 3 0 77 101 100 118 101 100 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 197 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 35 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 192 2 0 6 255 133 25 0 1 0 0 0 67 0 0 0 192 2 0 0 0 0 0 0 1 0 0 0 61 0 0 0 192 2 0 0 0 0 0 0 1 0 0 0 0 0 6 2 50 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 26 0 0 16 0 0 0 0 3 0 83 117 108 116 97 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 211 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 36 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 191 2 11 6 255 133 25 0 1 0 0 0 56 0 0 0 191 2 11 0 0 0 0 0 1 0 0 0 67 0 0 0 191 2 11 0 0 0 0 0 1 0 0 0 0 0 6 2 60 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 27 0 0 16 0 0 0 0 3 0 83 116 97 114 105 110 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 184 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 37 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 125 2 12 6 255 133 25 0 1 0 0 0 61 0 0 0 125 2 12 0 0 0 0 0 1 0 0 0 60 0 0 0 125 2 12 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 28 0 0 16 0 0 0 0 3 0 72 97 114 107 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 181 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 38 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 125 2 14 6 255 133 25 0 1 0 0 0 61 0 0 0 125 2 14 0 0 0 0 0 1 0 0 0 73 0 0 0 125 2 14 0 0 0 0 0 1 0 0 0 0 0 6 2 55 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 29 0 0 16 0 0 0 0 3 0 67 104 101 114 110 105 97 118 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 188 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 39 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 190 2 3 6 255 133 25 0 1 0 0 0 55 0 0 0 190 2 3 0 0 0 0 0 1 0 0 0 64 0 0 0 190 2 3 0 0 0 0 0 1 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 30 0 0 16 0 0 0 0 3 0 68 101 109 107 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 200 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 40 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 190 2 11 6 255 133 25 0 1 0 0 0 59 0 0 0 190 2 11 0 0 0 0 0 1 0 0 0 73 0 0 0 190 2 11 0 0 0 0 0 1 0 0 0 0 0 6 2 35 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 31 0 0 16 0 0 0 0 3 0 75 105 114 101 121 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 226 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 41 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 191 2 12 6 255 133 25 0 1 0 0 0 53 0 0 0 191 2 12 0 0 0 0 0 1 0 0 0 64 0 0 0 191 2 12 0 0 0 0 0 1 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 32 0 0 16 0 0 0 0 3 0 73 108 117 99 104 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 213 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 42 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 192 2 8 6 255 133 25 0 1 0 0 0 69 0 0 0 192 2 8 0 0 0 0 0 1 0 0 0 55 0 0 0 192 2 8 0 0 0 0 0 1 0 0 0 0 0 6 2 55 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 33 0 0 16 1 0 0 0 4 0 67 104 109 117 116 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 247 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 43 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 246 1 5 6 255 133 25 0 1 0 0 0 71 0 0 0 246 1 5 0 0 0 0 0 1 0 0 0 59 0 0 0 246 1 5 0 0 0 0 0 1 0 0 0 0 0 6 2 213 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 34 0 0 16 3 0 0 0 4 0 75 104 111 108 107 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 217 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 44 0 45 0 1 0 0 0 1 0 0 0 1 4 0 0 246 1 12 6 255 133 25 0 1 0 0 0 65 0 0 0 246 1 12 0 0 0 0 0 1 0 0 0 63 0 0 0 246 1 12 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 35 0 0 16 0 0 0 0 4 0 69 116 116 115 101 108 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 200 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 46 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 246 1 14 6 255 133 25 0 1 0 0 0 69 0 0 0 246 1 14 0 0 0 0 0 1 0 0 0 68 0 0 0 246 1 14 0 0 0 0 0 1 0 0 0 0 0 6 2 80 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 36 0 0 16 0 0 0 0 4 0 69 108 105 115 116 114 97 116 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 198 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 47 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 180 1 12 6 255 133 25 0 1 0 0 0 61 0 0 0 180 1 12 0 0 0 0 0 1 0 0 0 75 0 0 0 180 1 12 0 0 0 0 0 1 0 0 0 0 0 6 2 80 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 37 0 0 16 0 0 0 0 4 0 83 97 116 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 187 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 48 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 245 1 3 6 255 133 25 0 1 0 0 0 70 0 0 0 245 1 3 0 0 0 0 0 1 0 0 0 73 0 0 0 245 1 3 0 0 0 0 0 1 0 0 0 0 0 6 2 60 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 38 0 0 16 0 0 0 0 4 0 77 97 116 118 101 121 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 193 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 49 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 179 1 15 6 255 133 25 0 1 0 0 0 57 0 0 0 179 1 15 0 0 0 0 0 1 0 0 0 65 0 0 0 179 1 15 0 0 0 0 0 1 0 0 0 0 0 6 2 35 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 39 0 0 16 0 0 0 0 4 0 67 104 101 114 101 112 97 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 196 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 50 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 56 2 0 6 255 133 25 0 1 0 0 0 52 0 0 0 56 2 0 0 0 0 0 0 1 0 0 0 56 0 0 0 56 2 0 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 40 0 0 16 0 0 0 0 4 0 80 114 111 107 111 102 121 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 186 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 51 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 56 2 2 6 255 133 25 0 1 0 0 0 60 0 0 0 56 2 2 0 0 0 0 0 1 0 0 0 65 0 0 0 56 2 2 0 0 0 0 0 1 0 0 0 0 0 6 2 25 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 41 0 0 16 0 0 0 0 4 0 83 97 122 111 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 225 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 52 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 245 1 11 6 255 133 25 0 1 0 0 0 54 0 0 0 245 1 11 0 0 0 0 0 1 0 0 0 64 0 0 0 245 1 11 0 0 0 0 0 1 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 42 0 0 16 0 0 0 0 4 0 83 108 97 118 103 111 114 111 100 115 107 105 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 184 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 53 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 55 2 3 6 255 133 25 0 1 0 0 0 65 0 0 0 55 2 3 0 0 0 0 0 1 0 0 0 65 0 0 0 55 2 3 0 0 0 0 0 1 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 43 0 0 16 1 0 0 0 5 0 66 97 122 104 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 8 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 54 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 136 3 5 6 255 133 25 0 1 0 0 0 75 0 0 0 136 3 5 0 0 0 0 0 1 0 0 0 63 0 0 0 136 3 5 0 0 0 0 0 1 0 0 0 0 0 6 2 28 1 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 44 0 0 16 3 0 0 0 5 0 69 114 101 109 101 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 228 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 55 0 56 0 1 0 0 0 1 0 0 0 1 4 0 0 136 3 3 6 255 133 25 0 1 0 0 0 72 0 0 0 136 3 3 0 0 0 0 0 1 0 0 0 74 0 0 0 136 3 3 0 0 0 0 0 1 0 0 0 0 0 6 2 50 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 45 0 0 16 0 0 0 0 5 0 70 105 108 105 112 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 199 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 57 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 202 3 0 6 255 133 25 0 1 0 0 0 69 0 0 0 202 3 0 0 0 0 0 0 1 0 0 0 72 0 0 0 202 3 0 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 46 0 0 16 0 0 0 0 5 0 75 114 101 109 110 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 182 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 58 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 202 3 2 6 255 133 25 0 1 0 0 0 62 0 0 0 202 3 2 0 0 0 0 0 1 0 0 0 75 0 0 0 202 3 2 0 0 0 0 0 1 0 0 0 0 0 6 2 35 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 47 0 0 16 0 0 0 0 5 0 80 111 108 117 107 104 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 231 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 59 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 136 3 11 6 255 133 25 0 1 0 0 0 65 0 0 0 136 3 11 0 0 0 0 0 1 0 0 0 52 0 0 0 136 3 11 0 0 0 0 0 1 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 48 0 0 16 0 0 0 0 5 0 70 101 100 111 114 99 104 101 110 107 111 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 214 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 60 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 136 3 12 6 255 133 25 0 1 0 0 0 67 0 0 0 136 3 12 0 0 0 0 0 1 0 0 0 75 0 0 0 136 3 12 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 49 0 0 16 0 0 0 0 5 0 80 111 110 111 109 97 114 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 200 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 61 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 70 3 12 6 255 133 25 0 1 0 0 0 74 0 0 0 70 3 12 0 0 0 0 0 1 0 0 0 74 0 0 0 70 3 12 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 50 0 0 16 0 0 0 0 5 0 77 97 105 100 97 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 214 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 62 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 70 3 14 6 255 133 25 0 1 0 0 0 61 0 0 0 70 3 14 0 0 0 0 0 1 0 0 0 70 0 0 0 70 3 14 0 0 0 0 0 1 0 0 0 0 0 6 2 55 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 51 0 0 16 0 0 0 0 5 0 65 118 101 114 107 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 193 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 63 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 135 3 3 6 255 133 25 0 1 0 0 0 66 0 0 0 135 3 3 0 0 0 0 0 1 0 0 0 73 0 0 0 135 3 3 0 0 0 0 0 1 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 52 0 0 16 0 0 0 0 5 0 86 97 116 111 108 107 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 206 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 64 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 135 3 11 6 255 133 25 0 1 0 0 0 60 0 0 0 135 3 11 0 0 0 0 0 1 0 0 0 54 0 0 0 135 3 11 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 53 0 0 16 3 0 0 0 6 0 77 117 122 97 108 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 252 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 65 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 188 2 3 6 255 133 25 0 1 0 0 0 62 0 0 0 188 2 3 0 0 0 0 0 1 0 0 0 53 0 0 0 188 2 3 0 0 0 0 0 1 0 0 0 0 0 6 2 25 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 54 0 0 16 2 0 0 0 6 0 80 101 114 99 104 101 110 107 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 253 0 0 0 0 0 0 0 0 0 0 0 0 0 67 0 255 255 68 0 1 0 0 0 1 0 0 0 1 4 0 0 188 2 5 6 255 133 25 0 1 0 0 0 59 0 0 0 188 2 5 0 0 0 0 0 1 0 0 0 68 0 0 0 188 2 5 0 0 0 0 0 0 0 0 0 0 0 6 2 208 7 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 55 0 0 16 2 0 0 0 6 0 77 97 107 115 117 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 230 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 66 0 1 0 0 0 1 0 0 0 1 4 0 0 188 2 1 6 255 133 25 0 1 0 0 0 55 0 0 0 188 2 1 0 0 0 0 0 1 0 0 0 66 0 0 0 188 2 1 0 0 0 0 0 2 0 0 0 0 0 6 2 24 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 56 0 0 16 3 0 0 0 7 0 84 114 97 105 102 108 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 30 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 69 0 255 255 1 0 0 0 1 0 0 0 1 4 1 0 250 1 0 6 255 133 25 0 1 0 0 0 54 0 0 0 250 1 0 0 0 0 0 0 1 0 0 0 66 0 0 0 250 1 0 0 0 0 0 0 1 0 0 0 0 0 6 2 50 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 57 0 0 16 2 0 0 0 7 0 84 101 108 110 105 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 220 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 70 0 255 255 1 0 0 0 1 0 0 0 1 4 1 0 250 1 4 6 255 133 25 0 1 0 0 0 67 0 0 0 250 1 4 0 0 0 0 0 1 0 0 0 57 0 0 0 250 1 4 0 0 0 0 0 1 0 0 0 0 0 6 2 45 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 58 0 0 16 2 0 0 0 7 0 71 117 115 97 114 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 218 0 0 0 0 0 0 0 0 0 0 0 0 0 78 0 72 0 255 255 1 0 0 0 1 0 0 0 1 4 1 0 184 1 12 6 255 133 25 0 1 0 0 0 68 0 0 0 184 1 12 0 0 0 0 0 1 0 0 0 60 0 0 0 184 1 12 0 0 0 0 0 0 0 0 0 0 0 6 2 36 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 59 0 0 16 2 0 0 0 7 0 84 114 105 102 111 110 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 250 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 71 0 255 255 1 0 0 0 1 0 0 0 1 4 1 0 249 1 7 6 255 133 25 0 1 0 0 0 59 0 0 0 249 1 7 0 0 0 0 0 1 0 0 0 61 0 0 0 249 1 7 0 0 0 0 0 1 0 0 0 0 0 6 2 35 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 60 0 0 16 3 0 0 0 8 0 67 104 101 114 107 97 115 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 17 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 75 0 255 255 1 0 0 0 1 0 0 0 1 4 1 0 87 4 1 6 255 133 25 0 1 0 0 0 73 0 0 0 87 4 1 0 0 0 0 0 1 0 0 0 54 0 0 0 87 4 1 0 0 0 0 0 1 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 61 0 0 16 2 0 0 0 8 0 77 97 116 114 111 115 111 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 246 0 0 0 4 0 0 0 0 0 0 0 0 0 73 0 255 255 74 0 1 0 0 0 1 0 0 0 1 4 1 0 21 4 13 6 255 133 25 0 1 0 0 0 67 0 0 0 21 4 13 0 0 0 0 0 1 0 0 0 59 0 0 0 21 4 13 0 0 0 0 0 0 0 0 0 1 0 6 2 28 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 62 0 0 16 2 0 0 0 8 0 77 107 114 116 105 99 104 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 240 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 76 0 255 255 1 0 0 0 1 0 0 0 1 4 1 0 210 3 3 6 255 133 25 0 1 0 0 0 68 0 0 0 210 3 3 0 0 0 0 0 1 0 0 0 76 0 0 0 210 3 3 0 0 0 0 0 1 0 0 0 0 0 6 2 35 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 63 0 0 16 2 0 0 0 8 0 69 115 105 101 118 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 217 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 77 0 1 0 0 0 1 0 0 0 1 4 1 0 21 4 9 6 255 133 25 0 1 0 0 0 59 0 0 0 21 4 9 0 0 0 0 0 1 0 0 0 53 0 0 0 21 4 9 0 0 0 0 0 2 0 0 0 0 0 6 2 142 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 0 0 1 16 4 0 0 0 0 0 80 101 116 116 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 149 131 102 71 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 79 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 118 6 5 2 255 133 25 0 1 0 0 0 55 0 0 0 118 6 5 0 0 0 0 0 1 0 0 0 66 0 0 0 118 6 5 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 1 0 1 16 0 0 0 0 0 0 76 97 110 103 98 101 105 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 9 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 80 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 118 6 3 2 255 133 25 0 1 0 0 0 69 0 0 0 118 6 3 0 0 0 0 0 1 0 0 0 57 0 0 0 118 6 3 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 2 0 1 16 0 0 0 0 0 0 70 101 105 116 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 4 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 81 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 118 6 11 2 255 133 25 0 1 0 0 0 54 0 0 0 118 6 11 0 0 0 0 0 1 0 0 0 64 0 0 0 118 6 11 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 3 0 1 16 3 0 0 0 1 0 75 111 112 102 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 30 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 82 0 83 0 1 0 0 0 1 0 0 0 1 4 0 0 115 6 5 2 255 133 25 0 1 0 0 0 72 0 0 0 115 6 5 0 0 0 0 0 1 0 0 0 71 0 0 0 115 6 5 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 4 0 1 16 2 0 0 0 1 0 74 117 110 103 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 34 1 0 0 0 0 0 0 0 0 0 0 0 0 84 0 255 255 85 0 1 0 0 0 1 0 0 0 1 4 0 0 115 6 1 2 255 133 25 0 1 0 0 0 67 0 0 0 115 6 1 0 0 0 0 0 1 0 0 0 63 0 0 0 115 6 1 0 0 0 0 0 0 0 0 0 0 0 6 2 196 9 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 5 0 1 16 2 0 0 0 1 0 75 111 110 114 97 100 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 246 0 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 86 0 1 0 0 0 1 0 0 0 1 4 0 0 115 6 2 2 255 133 25 0 1 0 0 0 54 0 0 0 115 6 2 0 0 0 0 0 1 0 0 0 57 0 0 0 115 6 2 0 0 0 0 0 2 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 6 0 1 16 2 0 0 0 1 0 75 117 114 116 122 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 15 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 255 255 87 0 1 0 0 0 1 0 0 0 1 4 0 0 115 6 0 2 255 133 25 0 1 0 0 0 63 0 0 0 115 6 0 0 0 0 0 0 1 0 0 0 55 0 0 0 115 6 0 0 0 0 0 0 2 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 7 0 1 16 3 0 0 0 2 0 66 117 99 104 104 111 108 122 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 58 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 88 0 89 0 1 0 0 0 1 0 0 0 1 4 0 0 176 5 12 2 255 133 25 0 1 0 0 0 58 0 0 0 176 5 12 0 0 0 0 0 1 0 0 0 70 0 0 0 176 5 12 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 8 0 1 16 2 0 0 0 2 0 72 101 108 100 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 62 1 0 0 0 0 0 0 0 0 0 0 0 0 90 0 255 255 91 0 1 0 0 0 1 0 0 0 1 4 0 0 176 5 5 2 255 133 25 0 1 0 0 0 65 0 0 0 176 5 5 0 0 0 0 0 1 0 0 0 59 0 0 0 176 5 5 0 0 0 0 0 0 0 0 0 0 0 6 2 226 4 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 9 0 1 16 2 0 0 0 2 0 83 116 101 98 108 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 0 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 92 0 1 0 0 0 1 0 0 0 1 4 0 0 176 5 1 2 255 133 25 0 1 0 0 0 49 0 0 0 176 5 1 0 0 0 0 0 1 0 0 0 56 0 0 0 176 5 1 0 0 0 0 0 2 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 10 0 1 16 3 0 0 0 3 0 70 114 105 116 115 99 104 101 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 39 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 93 0 94 0 1 0 0 0 1 0 0 0 1 4 1 0 39 4 3 2 255 133 25 0 1 0 0 0 56 0 0 0 39 4 3 0 0 0 0 0 1 0 0 0 60 0 0 0 39 4 3 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 11 0 1 16 2 0 0 0 3 0 76 111 104 98 101 114 103 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 32 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 95 0 1 0 0 0 1 0 0 0 1 4 1 0 39 4 7 2 255 133 25 0 1 0 0 0 55 0 0 0 39 4 7 0 0 0 0 0 1 0 0 0 57 0 0 0 39 4 7 0 0 0 0 0 2 0 0 0 0 0 6 2 40 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 12 0 1 16 2 0 0 0 3 0 76 101 104 109 97 110 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 36 1 0 0 0 0 0 0 0 0 0 0 0 0 96 0 255 255 97 0 1 0 0 0 1 0 0 0 1 4 1 0 39 4 11 2 255 133 25 0 1 0 0 0 65 0 0 0 39 4 11 0 0 0 0 0 1 0 0 0 67 0 0 0 39 4 11 0 0 0 0 0 0 0 0 0 0 0 6 2 226 4 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 13 0 1 16 1 0 0 0 4 0 72 111 108 122 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 49 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 98 0 99 0 1 0 0 0 1 0 0 0 1 4 0 0 107 5 13 2 255 133 25 0 1 0 0 0 51 0 0 0 107 5 13 0 0 0 0 0 1 0 0 0 67 0 0 0 107 5 13 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 14 0 1 16 3 0 0 0 4 0 70 101 114 223 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 47 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 100 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 173 5 5 2 255 133 25 0 1 0 0 0 59 0 0 0 173 5 5 0 0 0 0 0 1 0 0 0 50 0 0 0 173 5 5 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 15 0 1 16 2 0 0 0 4 0 82 111 104 114 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 60 1 0 0 0 0 0 0 0 0 1 0 0 0 101 0 255 255 102 0 1 0 0 0 1 0 0 0 1 4 0 0 173 5 1 2 255 133 25 0 1 0 0 0 69 0 0 0 173 5 1 0 0 0 0 0 1 0 0 0 64 0 0 0 173 5 1 0 0 0 0 0 0 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 16 0 1 16 0 0 0 0 4 0 83 99 104 117 108 116 122 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 26 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 103 0 104 0 1 0 0 0 1 0 0 0 1 4 0 0 172 5 3 2 255 133 25 0 1 0 0 0 58 0 0 0 172 5 3 0 0 0 0 0 1 0 0 0 51 0 0 0 172 5 3 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 17 0 1 16 0 0 0 0 4 0 72 117 116 122 108 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 24 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 105 0 106 0 1 0 0 0 1 0 0 0 1 4 0 0 173 5 11 2 255 133 25 0 1 0 0 0 64 0 0 0 173 5 11 0 0 0 0 0 1 0 0 0 53 0 0 0 173 5 11 0 0 0 0 0 1 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 18 0 1 16 0 0 0 0 4 0 76 246 98 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 2 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 107 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 173 5 12 2 255 133 25 0 1 0 0 0 67 0 0 0 173 5 12 0 0 0 0 0 1 0 0 0 67 0 0 0 173 5 12 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 19 0 1 16 0 0 0 0 4 0 68 111 114 110 98 101 114 103 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 35 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 108 0 109 0 1 0 0 0 1 0 0 0 1 4 0 0 173 5 3 2 255 133 25 0 1 0 0 0 54 0 0 0 173 5 3 0 0 0 0 0 1 0 0 0 52 0 0 0 173 5 3 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 20 0 1 16 1 0 0 0 5 0 84 104 101 105 223 101 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 46 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 110 0 111 0 1 0 0 0 1 0 0 0 1 4 0 0 233 4 15 2 255 133 25 0 1 0 0 0 58 0 0 0 233 4 15 0 0 0 0 0 1 0 0 0 53 0 0 0 233 4 15 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 21 0 1 16 3 0 0 0 5 0 76 97 110 103 101 110 101 99 107 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 28 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 112 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 234 4 9 2 255 133 25 0 1 0 0 0 64 0 0 0 234 4 9 0 0 0 0 0 1 0 0 0 55 0 0 0 234 4 9 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 22 0 1 16 2 0 0 0 5 0 75 110 101 98 108 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 51 1 0 0 0 0 0 0 0 0 1 0 0 0 113 0 255 255 114 0 1 0 0 0 1 0 0 0 1 4 0 0 234 4 8 2 255 133 25 0 1 0 0 0 56 0 0 0 234 4 8 0 0 0 0 0 1 0 0 0 51 0 0 0 234 4 8 0 0 0 0 0 0 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 23 0 1 16 0 0 0 0 5 0 76 246 102 102 108 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 40 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 115 0 116 0 1 0 0 0 1 0 0 0 1 4 0 0 44 5 0 2 255 133 25 0 1 0 0 0 51 0 0 0 44 5 0 0 0 0 0 0 1 0 0 0 51 0 0 0 44 5 0 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 24 0 1 16 0 0 0 0 5 0 66 101 99 107 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 39 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 117 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 43 5 3 2 255 133 25 0 1 0 0 0 66 0 0 0 43 5 3 0 0 0 0 0 1 0 0 0 56 0 0 0 43 5 3 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 25 0 1 16 0 0 0 0 5 0 66 101 99 107 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 29 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 118 0 119 0 1 0 0 0 1 0 0 0 1 4 0 0 44 5 2 2 255 133 25 0 1 0 0 0 70 0 0 0 44 5 2 0 0 0 0 0 1 0 0 0 65 0 0 0 44 5 2 0 0 0 0 0 1 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 26 0 1 16 0 0 0 0 5 0 71 114 111 115 115 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 35 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 120 0 121 0 1 0 0 0 1 0 0 0 1 4 0 0 44 5 8 2 255 133 25 0 1 0 0 0 68 0 0 0 44 5 8 0 0 0 0 0 1 0 0 0 58 0 0 0 44 5 8 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 27 0 1 16 1 0 0 0 6 0 70 105 108 105 97 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 55 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 122 0 123 0 1 0 0 0 1 0 0 0 1 4 0 0 33 4 0 2 255 133 25 0 1 0 0 0 55 0 0 0 33 4 0 0 0 0 0 0 1 0 0 0 51 0 0 0 33 4 0 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 28 0 1 16 3 0 0 0 6 0 75 97 110 100 108 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 63 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 124 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 222 3 6 2 255 133 25 0 1 0 0 0 63 0 0 0 222 3 6 0 0 0 0 0 1 0 0 0 67 0 0 0 222 3 6 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 29 0 1 16 2 0 0 0 6 0 83 99 104 105 108 108 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 71 1 0 0 0 0 0 0 0 0 1 0 0 0 125 0 255 255 126 0 1 0 0 0 1 0 0 0 1 4 0 0 222 3 5 2 255 133 25 0 1 0 0 0 62 0 0 0 222 3 5 0 0 0 0 0 1 0 0 0 57 0 0 0 222 3 5 0 0 0 0 0 0 0 0 0 0 0 6 2 30 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 30 0 1 16 0 0 0 0 6 0 73 110 103 108 104 111 102 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 0 1 0 0 0 0 0 0 0 0 1 0 0 0 255 255 127 0 255 255 1 0 0 0 1 0 0 0 1 4 0 0 33 4 14 2 255 133 25 0 1 0 0 0 67 0 0 0 33 4 14 0 0 0 0 0 1 0 0 0 65 0 0 0 33 4 14 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 31 0 1 16 0 0 0 0 6 0 83 99 104 114 101 105 98 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 50 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 128 0 129 0 1 0 0 0 1 0 0 0 1 4 0 0 33 4 6 2 255 133 25 0 1 0 0 0 59 0 0 0 33 4 6 0 0 0 0 0 1 0 0 0 58 0 0 0 33 4 6 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 32 0 1 16 0 0 0 0 6 0 68 252 115 115 101 108 107 97 109 112 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 31 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 130 0 131 0 1 0 0 0 1 0 0 0 1 4 0 0 33 4 8 2 255 133 25 0 1 0 0 0 68 0 0 0 33 4 8 0 0 0 0 0 1 0 0 0 54 0 0 0 33 4 8 0 0 0 0 0 1 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 33 0 1 16 0 0 0 0 6 0 71 114 105 110 100 108 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 47 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 132 0 133 0 1 0 0 0 1 0 0 0 1 4 0 0 223 3 14 2 255 133 25 0 1 0 0 0 61 0 0 0 223 3 14 0 0 0 0 0 1 0 0 0 51 0 0 0 223 3 14 0 0 0 0 0 1 0 0 0 0 0 6 2 90 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 34 0 1 16 5 0 0 0 7 0 80 228 99 104 116 108 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 149 131 102 61 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 134 0 135 0 1 0 0 0 1 0 0 0 1 4 0 0 11 1 6 2 255 133 25 0 1 0 0 0 61 0 0 0 11 1 6 0 0 0 0 0 1 0 0 0 61 0 0 0 11 1 6 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 35 0 1 16 2 0 0 0 7 0 87 101 103 101 114 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 38 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 136 0 1 0 0 0 1 0 0 0 1 4 0 0 12 1 0 2 255 133 25 0 1 0 0 0 66 0 0 0 12 1 0 0 0 0 0 0 1 0 0 0 67 0 0 0 12 1 0 0 0 0 0 0 2 0 0 0 0 0 6 2 24 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 36 0 1 16 2 0 0 0 7 0 66 97 121 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 32 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 137 0 1 0 0 0 1 0 0 0 1 4 0 0 11 1 3 2 255 133 25 0 1 0 0 0 71 0 0 0 11 1 3 0 0 0 0 0 1 0 0 0 57 0 0 0 11 1 3 0 0 0 0 0 2 0 0 0 0 0 6 2 24 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 37 0 1 16 2 0 0 0 7 0 70 101 107 116 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 18 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 138 0 1 0 0 0 1 0 0 0 1 4 0 0 12 1 5 2 255 133 25 0 1 0 0 0 51 0 0 0 12 1 5 0 0 0 0 0 1 0 0 0 57 0 0 0 12 1 5 0 0 0 0 0 2 0 0 0 0 0 6 2 24 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 38 0 1 16 3 0 0 0 8 0 70 114 246 104 108 105 99 104 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 149 131 102 44 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 139 0 140 0 1 0 0 0 1 0 0 0 1 4 0 0 68 0 12 2 255 133 25 0 1 0 0 0 67 0 0 0 68 0 12 0 0 0 0 0 1 0 0 0 63 0 0 0 68 0 12 0 0 0 0 0 1 0 0 0 0 0 6 2 224 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 39 0 1 16 2 0 0 0 8 0 72 111 108 115 116 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 57 1 0 0 4 0 0 0 0 0 1 0 0 0 143 0 255 255 144 0 1 0 0 0 1 0 0 0 1 4 0 0 67 0 1 2 255 133 25 0 1 0 0 0 59 0 0 0 67 0 1 0 0 0 0 0 1 0 0 0 60 0 0 0 67 0 1 0 0 0 0 0 0 0 0 0 1 0 6 2 63 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 40 0 1 16 2 0 0 0 8 0 69 103 108 101 115 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 149 131 102 249 0 0 0 0 0 0 0 0 0 1 0 0 0 255 255 255 255 142 0 1 0 0 0 1 0 0 0 1 4 0 0 2 0 4 2 255 133 25 0 1 0 0 0 61 0 0 0 2 0 4 0 0 0 0 0 1 0 0 0 54 0 0 0 2 0 4 0 0 0 0 0 2 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("2 0 0 0 41 0 1 16 2 0 0 0 8 0 71 117 116 102 114 101 117 110 100 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 149 131 102 10 1 0 0 0 0 0 0 0 0 0 0 0 0 255 255 255 255 141 0 1 0 0 0 1 0 0 0 1 4 0 0 1 0 15 2 255 133 25 0 1 0 0 0 55 0 0 0 1 0 15 0 0 0 0 0 1 0 0 0 72 0 0 0 1 0 15 0 0 0 0 0 2 0 0 0 0 0 6 2 65 0 88 0 1 0 0 0 0 0 21 16 ");
	SendDirectPlayMessage("7 0 0 0 0 0 0 71 114 111 117 112 32 76 101 97 100 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 79 0 0 0 1 0 2 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 0 0 0 0 0 0 0 0 3 0 255 0 2 0 0 0 255 255 6 0 133 3 5 0 11 1 0 0 60 1 0 0 0 255 141 0 125 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 7 0 7 0 5 0 1 0 1 0 0 0 4 0 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 1 0 0 76 77 71 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 0 0 22 0 3 0 4 0 9 0 10 0 7 0 8 0 5 0 6 0 11 0 12 0 255 0 8 0 0 0 0 0 0 0 10 0 4 0 0 0 3 0 5 0 6 0 78 4 0 0 215 0 0 0 0 1 0 0 0 255 141 0 99 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 7 0 5 0 5 0 4 0 6 0 5 0 2 0 2 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 2 0 0 76 77 71 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 0 0 22 0 13 0 14 0 15 0 16 0 18 0 20 0 22 0 17 0 21 0 19 0 255 0 8 0 0 0 0 0 0 0 10 0 255 0 0 0 13 0 15 0 6 0 179 2 1 0 217 0 0 0 226 0 0 0 0 255 141 0 70 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 7 0 5 0 5 0 4 0 6 0 5 0 2 0 2 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 3 0 0 76 105 103 104 116 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 7 0 23 0 24 0 26 0 31 0 27 0 28 0 29 0 30 0 25 0 32 0 255 0 7 0 0 0 0 0 0 0 10 0 4 0 0 0 23 0 255 255 6 0 191 2 5 0 208 0 0 0 249 0 0 0 0 255 141 0 96 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 5 0 4 0 3 0 1 0 6 0 5 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 4 0 0 76 105 103 104 116 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 7 0 33 0 34 0 35 0 36 0 37 0 41 0 39 0 40 0 38 0 42 0 255 0 7 0 0 0 0 0 0 0 10 0 255 0 0 0 33 0 255 255 6 0 246 1 5 0 203 0 0 0 212 0 0 0 0 255 141 0 65 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 5 0 4 0 3 0 1 0 6 0 5 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 5 0 0 76 105 103 104 116 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 7 0 43 0 44 0 47 0 48 0 49 0 50 0 51 0 52 0 45 0 46 0 255 0 7 0 0 0 0 0 0 0 10 0 4 0 0 0 43 0 255 255 6 0 136 3 5 0 213 0 0 0 255 0 0 0 0 255 141 0 99 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 5 0 4 0 3 0 1 0 6 0 5 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 6 0 0 77 77 71 32 84 101 97 109 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4 0 53 0 55 0 54 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 11 0 0 0 0 0 0 0 3 0 4 0 0 0 53 0 54 0 6 0 188 2 1 0 245 0 0 0 27 1 0 0 0 255 141 0 108 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 8 0 8 0 5 0 5 0 5 0 4 0 4 0 4 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 7 0 0 52 53 109 109 32 65 84 32 71 117 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 33 0 56 0 57 0 58 0 59 0 255 255 255 255 255 255 255 255 255 255 255 255 0 0 19 0 0 0 0 0 0 0 4 0 255 0 0 0 56 0 58 0 6 0 249 1 3 0 243 0 0 0 252 0 0 0 1 255 141 0 82 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 8 0 8 0 4 0 3 0 3 0 2 0 5 0 5 0 4 0 4 0 4 0 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 8 0 0 56 50 109 109 32 77 111 114 116 97 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 0 60 0 61 0 62 0 63 0 255 255 255 255 255 255 255 255 255 255 255 255 255 0 12 0 0 0 0 0 0 0 4 0 255 0 0 0 60 0 61 0 6 0 21 4 9 0 244 0 0 0 251 0 0 0 1 255 141 0 78 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 8 0 8 0 4 0 2 0 4 0 4 0 4 0 4 0 1 0 0 0 3 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 0 0 1 71 114 111 117 112 32 76 101 97 100 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 93 0 0 0 1 0 2 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 0 0 0 0 0 0 0 0 3 0 255 0 2 0 0 0 255 255 2 0 118 6 5 0 28 1 0 0 66 1 0 0 0 255 141 0 134 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 8 0 8 0 4 0 1 0 1 0 0 0 4 0 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 1 0 1 72 77 71 32 84 101 97 109 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 75 0 3 0 4 0 5 0 6 0 255 255 255 255 255 255 255 255 255 255 255 255 255 0 11 0 0 0 0 0 0 0 4 0 4 0 0 0 3 0 4 0 2 0 115 6 1 0 17 1 0 0 49 1 0 0 0 255 141 0 124 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 9 0 8 0 7 0 6 0 4 0 4 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 2 0 1 77 71 32 84 101 97 109 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 74 0 7 0 8 0 9 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 11 0 0 0 0 0 0 0 3 0 4 0 0 0 7 0 8 0 2 0 176 5 5 0 40 1 0 0 71 1 0 0 0 255 141 0 132 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 7 0 6 0 5 0 5 0 4 0 4 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 3 0 1 77 71 32 84 101 97 109 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 74 0 10 0 12 0 11 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 0 11 0 0 0 0 0 0 0 3 0 255 0 0 0 10 0 12 0 2 0 39 4 7 0 35 1 0 0 43 1 0 0 1 255 141 0 110 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 9 0 9 0 7 0 6 0 5 0 5 0 4 0 4 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 4 0 1 76 116 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 84 0 13 0 14 0 15 0 19 0 17 0 18 0 16 0 255 255 255 255 255 255 255 0 7 0 0 0 0 0 0 0 7 0 4 0 0 0 13 0 15 0 2 0 173 5 1 0 34 1 0 0 67 1 0 0 0 255 141 0 133 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 5 0 3 0 2 0 0 0 8 0 5 0 2 0 2 0 2 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 40 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 5 0 1 76 116 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 84 0 20 0 21 0 22 0 23 0 25 0 24 0 26 0 255 255 255 255 255 255 255 0 7 0 0 0 0 0 0 0 7 0 255 0 0 0 20 0 22 0 2 0 233 4 15 0 38 1 0 0 47 1 0 0 0 255 141 0 114 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 5 0 3 0 2 0 0 0 8 0 5 0 2 0 2 0 1 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 6 0 1 76 116 32 73 110 102 97 110 116 114 121 0 0 0 0 0 0 0 0 0 0 0 0 0 0 84 0 27 0 28 0 29 0 31 0 32 0 30 0 33 0 255 255 255 255 255 255 255 0 7 0 0 0 0 0 0 0 7 0 255 0 0 0 27 0 29 0 2 0 33 4 0 0 45 1 0 0 53 1 0 0 0 255 141 0 116 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 10 0 10 0 5 0 3 0 2 0 0 0 8 0 5 0 2 0 2 0 2 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 7 0 1 83 116 117 71 32 73 73 73 32 67 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 233 0 34 0 35 0 37 0 36 0 255 255 255 255 255 255 255 255 255 255 255 255 255 0 27 0 0 0 0 0 0 0 4 0 255 0 0 0 34 0 255 255 2 0 11 1 3 0 37 1 0 0 46 1 0 0 0 255 141 0 114 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 9 0 9 0 4 0 0 0 0 0 0 0 4 0 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("7 0 0 0 8 0 1 56 99 109 32 77 111 114 116 97 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 7 0 38 0 41 0 40 0 39 0 255 255 255 255 255 255 255 255 255 255 255 255 255 0 12 0 0 0 0 0 0 0 4 0 255 0 0 0 38 0 39 0 2 0 67 0 1 0 26 1 0 0 31 1 0 0 0 255 141 0 105 1 0 0 0 0 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 255 9 0 9 0 4 0 2 0 5 0 5 0 4 0 4 0 0 0 0 0 3 0 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 223 1 1 0 255 0 1 16 ");
	SendDirectPlayMessage("9 0 0 0 3 0 7 0 0 0 68 0 56 0 57 0 58 0 59 0 255 255 255 255 255 255 78 0 255 255 255 255 35 0 6 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 96 0 96 0 255 255 255 255 255 255 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 96 0 96 0 0 0 2 0 0 0 3 0 249 1 249 1 0 0 0 0 0 0 5 5 5 5 0 0 0 0 0 0 0 0 0 0 1 16 ");

	//have no idea whether the following ("10 ...") belong to unit data or whatever, makes most sense to keep context until I know better
	SendDirectPlayMessage("10 0 0 0 0 0 0 255 255 255 121 9 8 134 25 0 255 198 1 0 ");
	SendDirectPlayMessage("10 0 0 0 1 0 1 255 255 255 121 9 8 134 25 0 255 198 1 0 ");
	SendDirectPlayMessage("10 0 0 0 2 0 2 255 255 255 121 9 8 134 25 0 255 198 1 0 ");
	SendDirectPlayMessage("10 0 0 0 3 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 4 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 5 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 6 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 7 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 8 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 9 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 10 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 11 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 12 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 13 0 8 255 255 255 121 9 12 134 25 0 0 198 46 0 ");
	SendDirectPlayMessage("10 0 0 0 14 0 51 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 15 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 16 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 17 0 78 255 255 255 121 9 12 134 25 0 0 198 29 0 ");
	SendDirectPlayMessage("10 0 0 0 18 0 51 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 19 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 20 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 21 0 8 255 255 255 121 9 12 134 25 0 0 198 46 0 ");
	SendDirectPlayMessage("10 0 0 0 22 0 51 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 23 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 24 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 25 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 26 0 78 255 255 255 121 9 12 134 25 0 0 198 29 0 ");
	SendDirectPlayMessage("10 0 0 0 27 0 51 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 28 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 29 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 30 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 31 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 32 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 33 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 34 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 35 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 36 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 37 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 38 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 39 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 40 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 41 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 42 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 43 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 44 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 45 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 46 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 47 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 48 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 49 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 50 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 51 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 52 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 53 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 54 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 55 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 56 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 57 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 58 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 59 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 60 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 61 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 62 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 63 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 64 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 65 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 66 0 51 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 67 0 10 255 255 255 121 9 12 134 25 0 0 198 249 0 ");
	SendDirectPlayMessage("10 0 0 0 68 0 51 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 69 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 70 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 71 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 72 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 73 0 19 255 255 255 121 9 12 134 25 0 1 198 6 0 ");
	SendDirectPlayMessage("10 0 0 0 74 0 30 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 75 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 76 0 6 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 77 0 5 255 255 255 121 9 12 134 25 0 0 198 70 0 ");
	SendDirectPlayMessage("10 0 0 0 78 0 48 0 0 0 121 9 12 134 25 0 0 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 79 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 80 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 81 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 82 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 83 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 84 0 84 255 255 255 121 9 12 134 25 0 0 198 249 0 ");
	SendDirectPlayMessage("10 0 0 0 85 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 86 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 87 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 88 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 89 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 90 0 29 255 255 255 121 9 12 134 25 0 0 198 249 0 ");
	SendDirectPlayMessage("10 0 0 0 91 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 92 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 93 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 94 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 95 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 96 0 29 255 255 255 121 9 12 134 25 0 0 198 249 0 ");
	SendDirectPlayMessage("10 0 0 0 97 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 98 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 99 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 100 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 101 0 77 255 255 255 121 9 12 134 25 0 0 198 29 0 ");
	SendDirectPlayMessage("10 0 0 0 102 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 103 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 104 0 103 255 255 255 121 9 12 134 25 0 1 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 105 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 106 0 64 255 255 255 121 9 12 134 25 0 1 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 107 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 108 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 109 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 110 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 111 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 112 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 113 0 77 255 255 255 121 9 12 134 25 0 0 198 29 0 ");
	SendDirectPlayMessage("10 0 0 0 114 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 115 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 116 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 117 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 118 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 119 0 64 255 255 255 121 9 12 134 25 0 1 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 120 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 121 0 103 255 255 255 121 9 12 134 25 0 1 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 122 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 123 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 124 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 125 0 77 255 255 255 121 9 12 134 25 0 0 198 29 0 ");
	SendDirectPlayMessage("10 0 0 0 126 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 127 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 128 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 129 0 104 255 255 255 121 9 12 134 25 0 4 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 130 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 131 0 64 255 255 255 121 9 12 134 25 0 1 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 132 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 133 0 103 255 255 255 121 9 12 134 25 0 1 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 134 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 135 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 136 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 137 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 138 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 139 0 7 255 255 255 121 9 12 134 25 0 0 198 31 0 ");
	SendDirectPlayMessage("10 0 0 0 140 0 43 255 255 255 121 9 12 134 25 0 0 198 7 0 ");
	SendDirectPlayMessage("10 0 0 0 141 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 142 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 143 0 20 255 255 255 121 9 12 134 25 0 1 198 6 0 ");
	SendDirectPlayMessage("10 0 0 0 144 0 3 255 255 255 121 9 12 134 25 0 0 198 4 0 ");
	SendDirectPlayMessage("10 0 0 0 145 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 146 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 147 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 148 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 149 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 150 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 151 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 152 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 153 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 154 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 155 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 156 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 157 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 158 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 159 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 160 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 161 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 162 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 163 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 164 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 165 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 166 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 167 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 168 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 169 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 170 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 171 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 172 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 173 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 174 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 175 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 176 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 177 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 178 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 179 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 180 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 181 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 182 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 183 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 184 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 185 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 186 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 187 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 188 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 189 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 190 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 191 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 192 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 193 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 194 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 195 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 196 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 197 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 198 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 199 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 200 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 201 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 202 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 203 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 204 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 205 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 206 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 207 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 208 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 209 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 210 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 211 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 212 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 213 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 214 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 215 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 216 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 217 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 218 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 219 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 220 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 221 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 222 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 223 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 224 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 225 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 226 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 227 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 228 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 229 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 230 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 231 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 232 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 233 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 234 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 235 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 236 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 237 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 238 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 239 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 240 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 241 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 242 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 243 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 244 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 245 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 246 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 247 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 248 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 249 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 250 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 251 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 252 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 253 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 254 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 255 0 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 0 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 1 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 2 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 3 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 4 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 5 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 6 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 7 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 8 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 9 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 10 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 11 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 12 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 13 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 14 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 15 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 16 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 17 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 18 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 19 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 20 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 21 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 22 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 23 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 24 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 25 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 26 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 27 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 28 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 29 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 30 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 31 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 32 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 33 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 34 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 35 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 36 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 37 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 38 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 39 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 40 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 41 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 42 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 43 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 44 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 45 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 46 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 47 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 48 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 49 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 50 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 51 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 52 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 53 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 54 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 55 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 56 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 57 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 58 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 59 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 60 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 61 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 62 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 63 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 64 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 65 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 66 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 67 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 68 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 69 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");
	SendDirectPlayMessage("10 0 0 0 70 1 255 0 255 255 121 9 8 134 25 0 255 198 0 0 ");

	SendDirectPlayMessage("16 0 0 0 1 134 25 0 0 0 1 9 0 0 1 23 ");
	SendDirectPlayMessage("4 0 0 0 ");
	SendDirectPlayMessage("61 0 0 0 0 0 0 0 ");
	SendDirectPlayMessage("49 0 0 0 10 0 25 0 ");
}

static void SendTickMessage(DWORD header, DWORD counter)
{
	uint8_t buffer[8];
	DWORD* const fields = reinterpret_cast<DWORD*>(buffer);
	fields[0] = header;
	fields[1] = counter;
	SendDirectPlayMessage(buffer, 8);
}

static void SendServerTick()
{
	static DWORD counter = 1;
	SendTickMessage(5, counter);

	counter += 1;
}

static void SendClientTick()
{
	static DWORD counter = 1;
	SendTickMessage(8, counter);

	counter += 1;
}

static void SendServerDeploymentData()
{
	LOG("Sending deployment data\n");
	SendDirectPlayMessage("24 0 0 0 254 205 96 2 ");
	SendDirectPlayMessage("37 0 0 0 ");
	SendDirectPlayMessage("24 0 0 0 0 205 96 2 ");
	SendServerTick();
}

static void SendServerFleeData()
{
	LOG("Sending flee data\n");
	SendDirectPlayMessage("60 0 0 0 161 0 0 0 59 0 0 0 8 0 0 0 0 0 0 0 2 0 0 0 0 0 0 0 4 0 0 0 213 0 0 0 0 0 0 0 59 0 0 0 8 0 1 0 0 0 0 0 2 0 0 32 0 0 0 0 6 0 0 0 171 1 0 0 33 2 0 0 0 0 59 0 0 0 9 0 1 0 0 0 0 0 2 0 0 0 0 0 0 0 4 0 0 0 167 1 0 0 0 0 0 0 58 0 0 0 7 0 1 0 0 0 0 133 4 0 0 0 10 0 0 0 1 66 0 0 0 68 0 0 0 2 0 0 0 0 59 0 0 0 9 0 1 0 0 0 0 4 0 0 0 0 0 0 0 0 1 0 0 0 3 0 0 0 0 0 0 0 0 ");
	//SendDirectPlayMessage("55 0 0 0 15 0 25 0 ");
	//SendDirectPlayMessage("16 0 0 0 1 0 0 0 0 0 1 48 0 0 1 166 ");
	//SendDirectPlayMessage("49 0 0 0 6 0 25 0 ");
}

static void ReadConfigMessage(const uint8_t* content)
{
	_requisitionState = RequisitionState();

	const char* const battle = reinterpret_cast<const char*>(content + 36);

	BattleFileData battleData;
	const std::string filename = GetAttachedPathPrefix() + "Data\\BATTLES\\" + battle;
	std::ifstream is(filename, std::ios::binary);
	if (is.fail())
		std::cerr << "Failed to open " << filename << " for reading\n";
	is.read(reinterpret_cast<char*>(&battleData), sizeof(battleData));
	if (is.fail())
		std::cerr << "Error reading battle data struct!!!\n";
	//TODO: would be best to quit here on error perhaps
	is.close();
	battleData.CheckAssumptions();

	//TODO: figure out how to detect who we are playing, for now assume bot is Russians

	std::memcpy(_requisitionState.Soldiers, battleData.RussianSoldiers, sizeof(_requisitionState.Soldiers));
	_requisitionState.CountSoldiers();
	LOG("Loaded " << _requisitionState.NumSoldiers << " soldiers from battle file " << battle << std::endl);
	LOG("The first soldier is " << _requisitionState.Soldiers[0].Name << std::endl);

	std::memcpy(_requisitionState.Vehicles, battleData.RussianVehicles, sizeof(_requisitionState.Vehicles));
	_requisitionState.CountVehicles();
	LOG("Loaded " << _requisitionState.NumVehicles << " vehicles from battle file " << battle << std::endl);
	LOG("The first vehicle is " << _requisitionState.Vehicles[0].Name << std::endl);

	std::memcpy(_requisitionState.Teams, battleData.RussianTeams, sizeof(_requisitionState.Teams));
	_requisitionState.CountTeams();
	LOG("Loaded " << _requisitionState.NumTeams << " teams from battle file " << battle << std::endl);
	LOG("The first team is " << _requisitionState.Teams[0].Name << std::endl);
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
			//greeting
			SendDirectPlayMessage("48 0 0 0 67 79 73 51 46 54 32 50 48 49 49 48 49 50 52 48 49 0 0 0 0 0 0 0 0 0 0 0 32 0 204 0 203 15 12 0 243 223 2 0 25 240 11 0 182 168 14 0 71 199 57 0 79 170 48 0 ");

			//config
			SendDirectPlayMessage("19 0 0 0 0 1 0 1 0 3 0 0 3 3 0 0 0 0 0 0 0 0 1 1 1 0 1 1 80 70 0 0 0 0 0 0 0 0 0 0 68 45 68 97 121 32 82 117 115 115 105 97 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 68 45 68 97 121 32 82 117 115 115 105 97 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 3 3 4 4 0 0 95 0 0 0 95 0 0 0 0 0 0 0 0 0 0 0 68 114 97 103 111 110 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 80 101 116 116 101 114 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2 0 0 0 0 0 0 0 76 118 111 118 49 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 10 0 0 0 50 0 0 0 50 0 0 0 10 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");

			//ready up!
			SendDirectPlayMessage("55 0 0 0 5 0 25 0");

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

static void TimedDump(const std::string& prefix, int& counter, Timer& timer)
{
	std::ofstream dumpFile(prefix + std::to_string(counter) + ".bin", std::ios::binary);
	DumpMemory(dumpFile, true);
	dumpFile.close();
	timer.Restart();
	counter += 1;
}

static void OnGameMessageReceived(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer)
{
	if (messageBuffer.size() < 4)
	{
		std::cout << "This message is too short to have a type!\n";
		LogDirectPlayMessage(fromPlayer, toPlayer, messageBuffer);
		return;
	}

	const DWORD userMessageType = *(const DWORD*)messageBuffer.data();
	const uint8_t* const messageContent = messageBuffer.data() + 4;
	bool printToLog = true;
	if (userMessageType == 10 && IsClient())
		printToLog = true;
	if (printToLog)
		LogDirectPlayMessage(fromPlayer, toPlayer, messageBuffer);

	switch (userMessageType)
	{
	case 55:
	{
		if (IsClient())
		{
			LOG("Readying up!\n");
			SendDirectPlayMessage("3 0 0 0 0 5 73 0 ");
		}
		break;
	}
	case 3:
	{
		if (IsServer() && _gameState == GameState::Waiting)
		{
			SetGameState(GameState::Requisition);

			SendDirectPlayMessage("35 0 0 0"); //game starting

			SendDirectPlayMessage("9 0 0 0"); //done with requisition???
			SendDirectPlayMessage("6 0 0 0 2 67 73 0 "); //???
		}
		if (IsServer() && _gameState == GameState::Deployment)
		{
			SendServerDeploymentData();

			SetGameState(GameState::Battle);
		}
		break;
	}
	case 35:
	{
		if (IsClient() && _gameState == GameState::Waiting)
		{
			SetGameState(GameState::Requisition);

			//TODO: this was "extra" and was outright hanging host. something else is evidently "done with req"
			//SendDirectPlayMessage("9 0 0 0"); //done with requisition

			TimedDump("req", _requisitionDumpCounter, _requisitionDumpTimer);
		}
		break;
	}
	case 9:
	{
		if (IsServer() && _gameState == GameState::Requisition)
		{
			SendDirectPlayMessage("49 0 0 0 14 0 25 0 "); //??? comment on own requisition?
			SendDirectPlayMessage("26 0 0 0 "); //request req from client???
			SendDirectPlayMessage("49 0 0 0 12 0 25 0"); //???
			SendServerUnitData();

			SetGameState(GameState::Deployment);
		}
		break;
	}
	case 26:
	{
		if (IsClient() && _gameState == GameState::Requisition)
		{
			SendDirectPlayMessage("17 0 0 0 14 0 73 0 ");
			SendDirectPlayMessage("9 0 0 0 ");

			SendClientUnitData();

			SetGameState(GameState::Deployment);
		}
		break;
	}
	case 5:
	{
		if (IsClient() && _gameState == GameState::Battle)
		{
			//server tick
			SendClientTick();
		}
		break;
	}
	case 8:
	{
		if (IsServer() && _gameState == GameState::Battle)
		{
			static int cntr = 0;
			if (cntr == 40)
			{
				LOG("SENDING DEBUG FLEE\n");
				SendFlee();
				break;
			}
			else if (cntr > 40)
				break;

			//server tick
			SendServerTick();
		}
		break;
	}
	case 10:
	{
		if (IsClient() && _gameState == GameState::Deployment)
		{
			SendDirectPlayMessage("3 0 0 0 0 2 73 0 ");
			SendDirectPlayMessage("6 0 0 0 0 67 73 0 ");
			SendDirectPlayMessage("6 0 0 0 2 67 73 0 ");

			SetGameState(GameState::Battle);
		}
		break;
	}
	case 19:
		if (IsClient())
		{
			ReadConfigMessage(messageContent);
		}
		break;
	case 60:
		if (IsClient())
		{
			//respond to server flee data
			//SendDirectPlayMessage("3 0 0 0 0 13 0 0 ");
			//SendDirectPlayMessage("17 0 0 0 6 0 73 0 ");
		}
		break;
	case 25:
		if (IsClient())
		{
			//respond to server flee initiation
			LOG("Server is fleeing\n");
			SendDirectPlayMessage("3 0 0 0 0 9 97 0");
			SendDirectPlayMessage("3 0 0 0 0 10 0 1");
		}
		break;
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
		std::cout << "You can leave the dialog box field blank to search the local network.";
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
		SendDirectPlayMessage("16 0 0 0 67 79 73 51 46 54 32 50 48 49 49 48 49 50 52 48 49 0 64 0 0 0 0 0 216 17 0 0 190 0 119 136 203 15 12 0 243 223 2 0 25 240 11 0 182 168 14 0 71 199 57 0 79 170 48 0 ");
	}

	return 0;
}

static void OnMainLoop()
{
	if (IsClient() && _gameState == GameState::Requisition)
	{
		if (_requisitionDumpTimer.GetElapsed() > 15)
		{
			TimedDump("req", _requisitionDumpCounter, _requisitionDumpTimer);
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
