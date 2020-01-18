#pragma once

constexpr bool AS_SERVER = false; //fake server for tricking client

//HiddenDragon.cpp =================================================

enum class GameState
{
	Waiting,
	Requisition,
	Deployment,
	Battle
};

#define LOG(x) std::cout << x; _logFile << x
extern std::ofstream _logFile;

bool IsServer();
bool IsClient();

void SendDirectPlayMessage(const uint8_t* data, std::size_t len);
void SendDirectPlayMessage(const std::vector<uint8_t>& data);
void SendDirectPlayMessage(const char* byteStream);
void SendDirectPlayMessage(const std::string& str);

void LogMessageContent(std::ostream& stream, const uint8_t* data, std::size_t len);
void LogMessageContent(std::ostream& stream, const std::vector<uint8_t>& messageBuffer);
void LogDirectPlayMessage(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer);

void DumpRequisition();
void DumpDeployment();

//BotCommunication.cpp ==============================================

GameState GetGameState();
const char* GetGameStateString(GameState state);

void OnGameMessageReceived(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer);

void SendFakeServerSetup();