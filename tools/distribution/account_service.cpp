#include "account_api.hpp"
#include "client_update_source.hpp"
#include <iostream>
#include <sstream>
#include <ws2tcpip.h>
using namespace rosh;
namespace {
std::string trim(std::string value){auto first=value.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);}
std::string lower(std::string value){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return char(std::tolower(c));});return value;}
void send_all(SOCKET socket,const char*data,std::size_t size){while(size){const auto count=send(socket,data,static_cast<int>(std::min<std::size_t>(size,65536)),0);if(count<=0)throw std::runtime_error("Connection closed");data+=count;size-=count;}}
void reply(SOCKET socket,const AccountReply&result){
 auto body=result.body.dump();std::string headers="HTTP/1.1 "+std::to_string(result.status)+" Response\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n";
 if(!result.cookie.empty())headers+="Set-Cookie: "+result.cookie+"\r\n";headers+="\r\n";send_all(socket,headers.data(),headers.size());send_all(socket,body.data(),body.size());
}
void file_reply(SOCKET socket,RepairFile&file){
 std::string header="HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\nContent-Length: "+std::to_string(file.size)+"\r\n\r\n";send_all(socket,header.data(),header.size());
 char buffer[65536];std::uint64_t sent=0;DWORD count;while((count=file.read(buffer,sizeof(buffer)))!=0){send_all(socket,buffer,count);sent+=count;}if(sent!=file.size)throw std::runtime_error("Update file read failed");
}
std::string title_name(const std::string&encoded){
 std::string result;
 auto digit=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
 for(std::size_t i=0;i<encoded.size();++i){char c=encoded[i];if(c=='%'){if(i+2>=encoded.size()||digit(encoded[i+1])<0||digit(encoded[i+2])<0)throw std::runtime_error("Invalid encoded name");c=static_cast<char>(digit(encoded[i+1])*16+digit(encoded[i+2]));i+=2;}else if(c=='+')c=' ';result+=c;}
 if(result.size()>140||encoded.find('&')!=std::string::npos)throw std::runtime_error("Invalid title query");return result;
}
void serve(SOCKET socket,PublicAccountApi&api,const fs::path&root,int port){
 std::string wire;char buffer[4096];std::size_t end;
 while((end=wire.find("\r\n\r\n"))==std::string::npos){if(wire.size()>=16384)throw std::runtime_error("Request headers too large");auto count=recv(socket,buffer,sizeof(buffer),0);if(count<=0)throw std::runtime_error("Incomplete request");wire.append(buffer,count);}
 std::istringstream lines(wire.substr(0,end));std::string first,method,path,version;std::getline(lines,first);std::istringstream request(first);request>>method>>path>>version;
 const std::string titlePrefix="/api/game/title?name=";
 const bool titleRequest=method=="GET"&&path.rfind(titlePrefix,0)==0;
 if((method!="GET"&&method!="POST")||version!="HTTP/1.1"||path.find_first_of("#\r\n")!=std::string::npos||(!titleRequest&&path.find('?')!=std::string::npos)){reply(socket,{400,{{"error","Invalid HTTP request."}}});return;}
 std::map<std::string,std::string> headers;std::string line;
 while(std::getline(lines,line)){const auto colon=line.find(':');if(colon==std::string::npos)throw std::runtime_error("Invalid header");auto key=lower(trim(line.substr(0,colon)));if(headers.count(key))throw std::runtime_error("Duplicate header");headers[key]=trim(line.substr(colon+1));}
 const auto host=headers["host"];const auto endpoint="127.0.0.1:"+std::to_string(port),localhost="localhost:"+std::to_string(port);
 if((host!=endpoint&&host!=localhost)||headers.count("transfer-encoding")){reply(socket,{403,{{"error","Invalid local endpoint."}}});return;}
 Json data=Json::object();
 if(method=="POST"){
  if(headers["origin"]!="http://"+host||headers["content-type"]!="application/json"||!headers.count("content-length")){reply(socket,{403,{{"error","Invalid request origin or content type."}}});return;}
  const auto length=headers["content-length"];if(length.empty()||length.find_first_not_of("0123456789")!=std::string::npos||length.size()>5)throw std::runtime_error("Invalid body length");auto size=std::stoul(length);if(size>4096)throw std::runtime_error("Request body too large");
  auto body=wire.substr(end+4);while(body.size()<size){auto count=recv(socket,buffer,static_cast<int>(std::min<std::size_t>(sizeof(buffer),size-body.size())),0);if(count<=0)throw std::runtime_error("Incomplete JSON body");body.append(buffer,count);}
  if(body.size()!=size)throw std::runtime_error("Trailing request data");data=Json::parse(body);if(!data.is_object())throw std::runtime_error("Expected JSON object");
 }
 if(titleRequest){reply(socket,api.appearance(title_name(path.substr(titlePrefix.size()))));return;}
 if(method=="GET"&&path=="/api/client-update"){
  const auto manifest=root/"Updates/manifest.json";if(!fs::is_regular_file(manifest)){reply(socket,{503,{{"error","The signed client release has not been installed."}}});return;}
  reply(socket,{200,Json::parse(read_file(manifest,8*1024*1024))});return;
 }
 const std::string prefix="/api/client-update/";
 if(method=="GET"&&path.rfind(prefix,0)==0){
  const auto hash=path.substr(prefix.size());if(!std::regex_match(hash,std::regex("[0-9a-f]{64}"))){reply(socket,{404,{{"error","Invalid update identifier."}}});return;}
  auto approved=open_client_repair(root,hash);if(approved){file_reply(socket,*approved);return;}
  reply(socket,{404,{{"error","Approved repair file is unavailable. Configure Config/client-updates.json to point to the matching client folder."}}});return;
 }
 std::string cookie;std::istringstream cookies(headers["cookie"]);while(std::getline(cookies,line,';')){line=trim(line);if(line.rfind("karos_session=",0)==0){if(!cookie.empty())throw std::runtime_error("Duplicate session cookie");cookie=line.substr(14);}}
 try{reply(socket,api.handle(method,path,data,cookie,"127.0.0.1"));}
 catch(const std::exception&e){reply(socket,{400,{{"error",e.what()}}});}
}
}
int wmain(int argc,wchar_t**argv){try{
 fs::path root=fs::absolute(argv[0]).parent_path().parent_path();int duration=0;
 for(int i=1;i<argc;++i){std::wstring option=argv[i];if(option==L"--root"&&i+1<argc)root=fs::absolute(argv[++i]);else if(option==L"--duration"&&i+1<argc)duration=std::stoi(argv[++i]);else throw std::runtime_error("Invalid accounts service option");}
 auto config=Json::parse(read_file(root/"Config/server.json"));int port=config.value("account_port",18290);if(port<1024||port>65535)throw std::runtime_error("Invalid accounts service port");
 WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("Winsock initialization failed");
 PublicAccountApi api(root);auto listener=socket(AF_INET,SOCK_STREAM,0);BOOL exclusive=TRUE;setsockopt(listener,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
 if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener,32))throw std::runtime_error("Accounts service port is already in use");
 u_long nonblocking=1;ioctlsocket(listener,FIONBIO,&nonblocking);
 auto stop=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\RoshAccountsStop_"+std::to_wstring(GetCurrentProcessId())).c_str());if(!stop)throw std::runtime_error("Cannot create shutdown event");
 const auto deadline=duration>0?now()+duration:0;std::cout<<"Native accounts API ready on 127.0.0.1:"<<port<<"\n"<<std::flush;
 while(WaitForSingleObject(stop,25)!=WAIT_OBJECT_0&&(!deadline||now()<deadline)){
  const auto client=accept(listener,nullptr,nullptr);if(client==INVALID_SOCKET)continue;nonblocking=0;ioctlsocket(client,FIONBIO,&nonblocking);DWORD timeout=5000;
  setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
  try{serve(client,api,root,port);}catch(...){try{reply(client,{400,{{"error","Invalid account service request."}}});}catch(...){}}
  shutdown(client,SD_BOTH);closesocket(client);
 }
 CloseHandle(stop);closesocket(listener);WSACleanup();return 0;
}catch(const std::exception&e){std::cerr<<"Accounts: "<<e.what()<<'\n';return 1;}}
