#include "native/core.hpp"
#include <iostream>
#include <sstream>
using namespace rosh;
int wmain(int argc,wchar_t**argv){try{
 if(argc!=4)throw std::runtime_error("Usage: StagePublicContent SOURCE_SERVER NEW_SERVER PRIVATE_KEY_HEADER");
 const auto source=fs::weakly_canonical(argv[1]),destination=fs::weakly_canonical(argv[2]);
 if(source==destination||fs::exists(destination/"Data/catalog.dat"))throw std::runtime_error("Content destination must be new and separate");
 Content current(source);auto manifest=current.status();
 const auto oldKey=unprotect_key(source/"Config/Keys/content.key.dpapi"),releaseKey=random_bytes(32);
 std::map<std::string,std::string> categories;
 for(auto& [name,row]:manifest.at("resources").items()){
  if(name.find(".private.")!=std::string::npos)throw std::runtime_error("Private resource is not permitted in the release catalog");
  const auto file=row.at("file").get<std::string>();
  if(fs::path(file).filename()!=fs::path(file)||file.find(':')!=std::string::npos)throw std::runtime_error("Invalid resource filename");
  const auto aad=row.value("category",name);
  if(categories.count(file)&&categories.at(file)!=aad)throw std::runtime_error("Conflicting content categories");
  categories[file]=aad;
 }
 for(const auto& [file,aad]:categories){
  const auto input=read_file(source/"Data"/file);
  for(const auto& row:manifest.at("resources"))if(row.at("file")==file&&row.at("sha256")!=hex(sha256(input)))throw std::runtime_error("Source content hash mismatch");
  auto plain=decrypt_content(input,oldKey,aad);auto encrypted=encrypt_content(plain,releaseKey,aad);
  if(decrypt_content(encrypted,releaseKey,aad)!=plain)throw std::runtime_error("Release content roundtrip mismatch");
  write_file(destination/"Data"/file,encrypted);
  for(auto& row:manifest.at("resources"))if(row.at("file")==file)row["sha256"]=hex(sha256(encrypted));
 }
 auto text=manifest.dump();auto catalog=encrypt_content(Bytes(text.begin(),text.end()),releaseKey,"catalog");write_file(destination/"Data/catalog.dat",catalog);
 std::ostringstream header;header<<"#pragma once\ninline constexpr unsigned char releaseContentKey[32]={";
 for(std::size_t i=0;i<releaseKey.size();++i){if(i)header<<',';header<<unsigned(releaseKey[i]);}header<<"};\ninline constexpr const char* releaseCatalogHash=\""<<hex(sha256(catalog))<<"\";\n";
 auto generated=header.str();write_file(argv[3],Bytes(generated.begin(),generated.end()));
 std::cout<<"PASS: "<<categories.size()<<" content files re-encrypted with a release-only key; production key not copied.\n";
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
