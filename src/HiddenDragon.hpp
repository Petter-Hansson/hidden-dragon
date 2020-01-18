#pragma once

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

void LogDirectPlayMessage(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer);

void DumpRequisition();

//BotCommunication.cpp ==============================================

GameState GetGameState();
const char* GetGameStateString(GameState state);

void OnGameMessageReceived(DPID fromPlayer, DPID toPlayer, const std::vector<uint8_t>& messageBuffer);
