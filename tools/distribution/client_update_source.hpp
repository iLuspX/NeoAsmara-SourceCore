#pragma once
#include "native/core.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace rosh {
// Hold the approved file open without write/delete sharing through hashing and transmission.
class RepairFile {
 HANDLE handle_=INVALID_HANDLE_VALUE;
public:
 std::uint64_t size=0;
 explicit RepairFile(const fs::path&path){
  handle_=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
  if(handle_==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open approved repair source");
  LARGE_INTEGER length{};if(!GetFileSizeEx(handle_,&length)||length.QuadPart<0){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;throw std::runtime_error("Cannot size approved repair source");}size=std::uint64_t(length.QuadPart);
 }
 RepairFile(RepairFile&&other)noexcept:handle_(other.handle_),size(other.size){other.handle_=INVALID_HANDLE_VALUE;}
 RepairFile(const RepairFile&)=delete;RepairFile&operator=(const RepairFile&)=delete;
 ~RepairFile(){if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);}
 DWORD read(char*buffer,DWORD capacity){DWORD count=0;if(!ReadFile(handle_,buffer,capacity,&count,nullptr))throw std::runtime_error("Cannot read approved repair source");return count;}
 void rewind(){LARGE_INTEGER offset{};if(!SetFilePointerEx(handle_,offset,nullptr,FILE_BEGIN))throw std::runtime_error("Cannot rewind approved repair source");}
 std::string sha256(){
  BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};Bytes object;
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot initialize repair hashing");
  try{
   DWORD size=0,received=0;if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&received,0)<0||!size)throw std::runtime_error("Cannot size repair hash");
   object.resize(size);Bytes digest(32);if(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)<0)throw std::runtime_error("Cannot create repair hash");
   rewind();std::array<char,65536> buffer{};DWORD count;while((count=read(buffer.data(),static_cast<DWORD>(buffer.size())))!=0)if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),count,0)<0)throw std::runtime_error("Cannot hash repair source");
   if(BCryptFinishHash(hash,digest.data(),32,0)<0)throw std::runtime_error("Cannot finish repair hash");BCryptDestroyHash(hash);hash=nullptr;BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;rewind();return hex(digest);
  }catch(...){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);throw;}
 }
};
inline bool repair_path_allowed(const std::string&path){
 if(path.empty()||path.front()=='/'||path.find_first_of("\\:\0",0,3)!=std::string::npos||path.find("..")!=std::string::npos)return false;
 for(auto prefix:{"Data/","ResUnpak/","overlay/","LauncherAssets/","reshade-shaders/","reshade/","catalogues/","EasyAntiCheat/"})if(path.rfind(prefix,0)==0)return true;
 for(auto file:{"AMo.exe","RoshClientUi.dll","karos_aa.dll","WeberForClntV2.dll","WeberForClntV2_real.dll","WeberForClientNHN.dll","CLIENT_PROTECTED_FILES.json","CheckFileListW.ten","ServerList.ten","KarosGuard.exe","bdvid32.dll","CREDITS.txt","D3DX9_40.dll","EOSSDK-Win32-Shipping.dll","Shader.dll","shop_api.dll","shop_api_v2.dll"})if(path==file)return true;
 return false;
}
inline bool repair_no_redirects(const fs::path&path){
 fs::path walked;for(const auto&part:fs::absolute(path).lexically_normal()){
  walked/=part;DWORD attributes=GetFileAttributesW(walked.c_str());if(attributes==INVALID_FILE_ATTRIBUTES)return false;if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)return false;
 }return true;
}
inline Json repair_payload(const fs::path&root){
 const auto envelope=Json::parse(read_file(root/L"Updates/manifest.json",8*1024*1024));const auto encoded=envelope.at("payload").get<std::string>();
 DWORD size=0;if(encoded.size()>8*1024*1024||!CryptStringToBinaryA(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,nullptr,&size,nullptr,nullptr))throw std::runtime_error("Invalid signed repair payload");
 Bytes decoded(size);if(!CryptStringToBinaryA(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,decoded.data(),&size,nullptr,nullptr))throw std::runtime_error("Invalid signed repair payload");
 const auto payload=Json::parse(decoded);if(payload.at("schema")!=1||!payload.at("files").is_array()||payload.at("files").size()>10000)throw std::runtime_error("Unsupported repair payload");return payload;
}
inline std::optional<RepairFile> open_client_repair(const fs::path&root,const std::string&hash){
 if(hash.size()!=64||!std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}))return std::nullopt;
 const auto payload=repair_payload(root);std::vector<Json> approved;
 for(const auto&row:payload.at("files"))if(row.at("sha256").get<std::string>()==hash){
  if(!repair_path_allowed(row.at("path").get<std::string>())||!row.at("size").is_number_unsigned())throw std::runtime_error("Invalid approved repair path or size");approved.push_back(row);
 }
 if(approved.empty())return std::nullopt;
 const auto settings=root/L"Config/client-updates.json";const auto config=fs::is_regular_file(settings)?Json::parse(read_file(settings,65536)):Json::object();
 std::vector<fs::path> sources;
 const auto configured=config.value("source",std::string("../Client And Launcher"));if(!configured.empty()){
  auto source=fs::absolute(root/fs::u8path(configured)).lexically_normal();if(fs::is_directory(source))sources.push_back(source);
 }
 // A renamed adjacent client remains discoverable by its exact signed protection-list hash.
 if(sources.empty()&&config.value("discover_adjacent",true)){
  for(const auto&entry:fs::directory_iterator(root.parent_path()))if(entry.is_directory()){
   const auto manifest=entry.path()/L"CLIENT_PROTECTED_FILES.json";
   if(fs::is_regular_file(manifest)&&repair_no_redirects(manifest)){
    RepairFile file(manifest);if(file.size<=8*1024*1024&&file.sha256()==payload.at("integrity_manifest_sha256").get<std::string>())sources.push_back(entry.path());
   }
  }
 }
 for(const auto&source:sources)for(const auto&row:approved){
  auto path=source/fs::u8path(row.at("path").get<std::string>());if(!fs::is_regular_file(path)||!repair_no_redirects(path))continue;
  RepairFile file(path);if(file.size==row.at("size").get<std::uint64_t>()&&file.sha256()==hash)return std::optional<RepairFile>(std::move(file));
 }
 // Optional small, hash-addressed patches remain supported; full mirrors are not required.
 const auto patch=root/L"Updates/blobs"/hash;if(fs::is_regular_file(patch)&&repair_no_redirects(patch)){
  RepairFile file(patch);if(file.size==approved.front().at("size").get<std::uint64_t>()&&file.sha256()==hash)return std::optional<RepairFile>(std::move(file));
 }
 return std::nullopt;
}
}
