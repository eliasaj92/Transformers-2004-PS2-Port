#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
namespace ps2x::settings {
inline std::unordered_map<std::string,std::string> values;
inline std::unordered_set<std::string> wired;
inline std::string get(const char* key){return values[key];}
inline bool getBool(const char* key){return get(key)=="on";}
inline double getFloat(const char* key){const auto value=get(key);return value.empty()?1:std::stod(value);}
inline void declareWired(const char* key){wired.insert(key);}
}
