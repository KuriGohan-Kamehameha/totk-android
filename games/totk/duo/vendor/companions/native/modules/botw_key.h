// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace botw {
// Output names such as "botw.map.koroks.17.kind" are rebuilt for every published
// value on every sample. Composing them in a fixed buffer avoids several thousand
// short-lived heap strings per sample; the produced names are byte-identical to the
// former std::string concatenation ("<prefix><decimal index><suffix>").
class Key {
 public:
  explicit Key(std::string_view prefix) {append(prefix);base_=size_;terminate();}
  Key(std::string_view prefix,uint64_t index) {append(prefix);append(index);base_=size_;terminate();}
  // Prefix plus index, e.g. "botw.map.names.12".
  const char* c_str() {size_=base_;terminate();return buffer_;}
  // Prefix plus index plus suffix, e.g. Key("botw.map.places.",3)(".kind").
  const char* operator()(std::string_view suffix) {size_=base_;append(suffix);terminate();return buffer_;}
  // Prefix plus index plus two suffix fragments, e.g. Key("botw.map.poi.",3)(".",part).
  const char* operator()(std::string_view first,std::string_view second) {
    size_=base_;append(first);append(second);terminate();return buffer_;
  }
  // Prefix plus a second index between two fragments, e.g. Key("botw.map.ui.poi.")("",7,".kind").
  const char* operator()(std::string_view middle,uint64_t index,std::string_view suffix) {
    size_=base_;append(middle);append(index);append(suffix);terminate();return buffer_;
  }
 private:
  // The longest current name is under 50 bytes; anything longer is cut, never overrun.
  static constexpr std::size_t Capacity=96;
  char buffer_[Capacity]{};
  std::size_t size_{},base_{};
  void terminate() {buffer_[size_]=0;}
  void append(std::string_view text) {
    for(const char c:text){if(size_+1>=Capacity)return;buffer_[size_++]=c;}
  }
  void append(uint64_t value) {
    char digits[20];std::size_t n=0;
    do{digits[n++]=char('0'+value%10);value/=10;}while(value);
    while(n){if(size_+1>=Capacity)return;buffer_[size_++]=digits[--n];}
  }
};
} // namespace botw
