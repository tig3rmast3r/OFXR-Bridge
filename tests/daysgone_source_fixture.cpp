#include <cstdint>
using Setup=void(*)(std::uintptr_t,std::uintptr_t);
static Setup original;
extern "C" __declspec(dllexport) void DaysGoneTestOriginal(Setup fn){original=fn;}
extern "C" __declspec(dllexport) void DaysGoneTestDetour(std::uintptr_t renderer,std::uintptr_t list){original(renderer,list);}
