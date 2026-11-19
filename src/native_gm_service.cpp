#include "native/gm.hpp"
#include <algorithm>
#include <sstream>
#include <iostream>
using namespace rosh;
static std::string ascii_lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
static void serve(SOCKET fd,GmController& gm,int port){
 std::string input;char buffer[4096];std::size_t boundary=std::string::npos;
 while((boundary=input.find("\r\n\r\n"))==std::string::npos){int count=recv(fd,buffer,sizeof(buffer),0);if(count<=0)return;input.append(buffer,count);if(input.size()>16384)throw std::runtime_error("Cabecalho excede o limite");}
 std::istringstream headers(input.substr(0,boundary));std::string method,path,version,line;headers>>method>>path>>version;std::getline(headers,line);
 std::map<std::string,std::string> fields;
 while(std::getline(headers,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();auto colon=line.find(':');if(colon==std::string::npos)continue;auto name=ascii_lower(line.substr(0,colon));auto value=line.substr(colon+1);while(!value.empty()&&value.front()==' ')value.erase(value.begin());if(fields.count(name))throw std::runtime_error("Cabecalho duplicado");fields[name]=value;}
 if(version!="HTTP/1.1"||fields["host"]!="127.0.0.1:"+std::to_string(port)||fields.count("origin")||fields.count("transfer-encoding"))throw std::runtime_error("Origem HTTP recusada");
 if(method!="POST"&&!(method=="GET"&&path=="/health"))throw std::runtime_error("Metodo HTTP recusado");
 std::size_t size=0;
 if(fields.count("content-length")){auto value=fields.at("content-length");if(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Tamanho invalido");size=std::stoul(value);}
 if(size>65536)throw std::runtime_error("Requisicao excede o limite");
 auto body=input.substr(boundary+4);while(body.size()<size){int count=recv(fd,buffer,std::min(sizeof(buffer),size-body.size()),0);if(count<=0)throw std::runtime_error("Corpo incompleto");body.append(buffer,count);}
 if(body.size()!=size)throw std::runtime_error("Corpo invalido");
 if(method=="POST"&&fields["content-type"].find("application/json")!=0)throw std::runtime_error("Formato JSON obrigatorio");
 std::string secret;auto authorization=fields["authorization"];if(authorization.rfind("Bearer ",0)==0)secret=authorization.substr(7);
 Json response;int code=200;
 try{response=gm.request(method,path,body.empty()?Json::object():Json::parse(body),secret);}
 catch(const std::exception& e){code=400;response={{"error",e.what()}};}
 auto payload=response.dump();auto wire="HTTP/1.1 "+std::to_string(code)+(code==200?" OK":" Bad Request")+"\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\nContent-Length: "+std::to_string(payload.size())+"\r\n\r\n"+payload;
 std::size_t offset=0;while(offset<wire.size()){int count=send(fd,wire.data()+offset,static_cast<int>(wire.size()-offset),0);if(count<=0)break;offset+=count;}
}
int wmain(int argc,wchar_t**argv){
 try{
  fs::path root=fs::absolute(fs::path(argv[0])).parent_path().parent_path();int port=18192;
  for(int i=1;i<argc;i++){std::wstring a=argv[i];if(a==L"--root"&&i+1<argc)root=argv[++i];else if(a==L"--port"&&i+1<argc)port=std::stoi(argv[++i]);else throw std::runtime_error("Argumento invalido");}
  if(port<1024||port>65535)throw std::runtime_error("Porta invalida");
  for(auto& name:{"karos_login.sqlite","karos_gameplay.sqlite","karos_roles.sqlite","staff_second_login.sqlite","moderation.sqlite"})if(!fs::is_regular_file(root/"Database"/name))throw std::runtime_error("Banco necessario ausente");
  WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))throw std::runtime_error("Winsock indisponivel");
  GmController gm(root);auto fd=socket(AF_INET,SOCK_STREAM,0);BOOL exclusive=TRUE;setsockopt(fd,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char*)&exclusive,sizeof(exclusive));
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
  if(bind(fd,(sockaddr*)&address,sizeof(address))||listen(fd,16))throw std::runtime_error("Porta GM ocupada");
  u_long nonblock=1;ioctlsocket(fd,FIONBIO,&nonblock);
  auto event=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\RoshGmStop_"+std::to_wstring(GetCurrentProcessId())).c_str());if(!event)throw std::runtime_error("Evento de encerramento indisponivel");
  std::cout<<"ROSH GM native service ready: 127.0.0.1:"<<port<<"\n"<<std::flush;
  while(WaitForSingleObject(event,50)!=WAIT_OBJECT_0){auto client=accept(fd,nullptr,nullptr);if(client==INVALID_SOCKET)continue;nonblock=0;ioctlsocket(client,FIONBIO,&nonblock);DWORD timeout=3000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeout,sizeof(timeout));try{serve(client,gm,port);}catch(const std::exception&){}shutdown(client,SD_BOTH);closesocket(client);}
  CloseHandle(event);closesocket(fd);WSACleanup();return 0;
 }catch(const std::exception&e){std::cerr<<"GM service: "<<e.what()<<"\n";return 1;}
}
