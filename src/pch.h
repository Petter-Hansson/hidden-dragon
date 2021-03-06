// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file

#ifndef PCH_H
#define PCH_H

#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <dplay.h>
#include <dpaddr.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <experimental/filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <iphlpapi.h>
#include <memory>
#include <mutex>
#include <objbase.h>
#include <psapi.h>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <wchar.h>

#endif //PCH_H
