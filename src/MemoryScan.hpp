#pragma once

bool AttachToCloseCombat();
const std::string& GetAttachedFilename();
std::string GetAttachedPathPrefix();
void DetachFromCloseCombat();
void DumpMemory(std::ostream& binaryStream, bool segmented);
