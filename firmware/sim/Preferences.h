#pragma once
#include <map>
#include <string>
#include <cstdint>

extern std::map<std::string, long> g_nvs;   // survives "reboots" within one test run

class Preferences {
 public:
  bool begin(const char*, bool) { return true; }
  void end() {}
  bool clear() { g_nvs.clear(); return true; }
  int32_t getInt(const char* k, int32_t d) { auto it = g_nvs.find(k); return it == g_nvs.end() ? d : (int32_t)it->second; }
  uint32_t getUInt(const char* k, uint32_t d) { auto it = g_nvs.find(k); return it == g_nvs.end() ? d : (uint32_t)it->second; }
  size_t putInt(const char* k, int32_t v) { g_nvs[k] = v; return 4; }
  size_t putUInt(const char* k, uint32_t v) { g_nvs[k] = v; return 4; }
};
