#include "native/core.hpp"
#include <iostream>
#include <cstring>
using namespace rosh;
static void(*system_free)(void*);
static void* released{};
static int duplicate_free{};
// Simulate SQLite's documented FREEONCLOSE ownership on a failed deserialize.
extern "C" int sqlite3_deserialize(sqlite3*,const char*,unsigned char*buffer,sqlite3_int64,sqlite3_int64,unsigned flags){
 if(!(flags&SQLITE_DESERIALIZE_FREEONCLOSE))return SQLITE_ERROR;
 system_free(buffer);released=buffer;return SQLITE_BUSY;
}
extern "C" void sqlite3_free(void*buffer){
 if(buffer&&buffer==released){++duplicate_free;return;}
 system_free(buffer);
}
int wmain(){try{
 const auto module=LoadLibraryW(L"winsqlite3.dll");if(!module)throw std::runtime_error("Windows SQLite unavailable");
 system_free=reinterpret_cast<void(*)(void*)>(GetProcAddress(module,"sqlite3_free"));if(!system_free)throw std::runtime_error("SQLite allocator missing");
 Bytes image(4096);std::memcpy(image.data(),"SQLite format 3\0",16);bool rejected=false;
 try{Sqlite database(image);}catch(const std::runtime_error&){rejected=true;}
 if(!rejected)throw std::runtime_error("Failed deserialize was accepted");
 if(duplicate_free)throw std::runtime_error("Deserializer buffer was freed twice after SQLite released it");
 std::cout<<"PASS: failed readonly SQLite deserialization throws and never frees the released buffer a second time\n";
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
