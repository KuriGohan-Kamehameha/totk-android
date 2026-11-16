// SPDX-License-Identifier: GPL-3.0-or-later
// i8-layout item 10: a module-side port of the offline renderer's word-wrap/measure (tools/
// render-pages.py Font.measure()/Font.lines()), against the SAME Normal_00.bffnt advance-width
// table (botw_font_metrics.h) the real runtime's game-font text path uses -- this is NOT a
// guess at line count, it is the actual font's own per-glyph advance data. Used so a manifest
// label's header/row geometry can follow the REAL wrapped height of a selection-dependent
// string (recipe title, reflowed description) instead of a fixed worst-case constant.
//
// Known gap: multi-byte UTF-8 beyond the covered codepoints (see botw_font_metrics.h) falls back
// to a half-cap-width estimate per codepoint, same fallback the Python renderer uses for a glyph
// miss -- fine for English recipe names/descriptions (plain ASCII + a few punctuation marks),
// not a claim of full Unicode coverage.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "botw_font_metrics.h"
namespace botw {

/// Next UTF-8 code point starting at `i` (advances `i`); a malformed byte is taken on its own,
/// same rule as Core::Mods::Msbt::NextUtf8 (read-only runtime code, not duplicated here beyond
/// this minimal decode -- this header owns no text, only measures what it's given).
inline char32_t TextMeasureNextUtf8(std::string_view s, size_t& i) {
 if(i>=s.size())return 0;
 const unsigned char c0=static_cast<unsigned char>(s[i]);
 auto cont=[&](size_t n)->bool{return i+n<s.size();};
 if(c0<0x80){++i;return c0;}
 if((c0&0xE0)==0xC0&&cont(1)){char32_t cp=((c0&0x1Fu)<<6)|(static_cast<unsigned char>(s[i+1])&0x3Fu);i+=2;return cp;}
 if((c0&0xF0)==0xE0&&cont(2)){char32_t cp=((c0&0x0Fu)<<12)|((static_cast<unsigned char>(s[i+1])&0x3Fu)<<6)|(static_cast<unsigned char>(s[i+2])&0x3Fu);i+=3;return cp;}
 if((c0&0xF8)==0xF0&&cont(3)){char32_t cp=((c0&0x07u)<<18)|((static_cast<unsigned char>(s[i+1])&0x3Fu)<<12)|((static_cast<unsigned char>(s[i+2])&0x3Fu)<<6)|(static_cast<unsigned char>(s[i+3])&0x3Fu);i+=4;return cp;}
 ++i;return c0;
}

/// Advance width (px) of one codepoint at `scale`, folding to the renderer's half-cap-width
/// fallback for anything outside the table (matches Font.measure()'s `g is None` branch).
inline double GlyphAdvancePx(char32_t cp, int scale) {
 const double ratio=double(scale)*5.0/double(kFontMetricsCapHeight);
 int idx=-1;
 if(cp>=char32_t(kFontMetricsFirstCp)&&cp<char32_t(kFontMetricsFirstCp)+95)idx=int(cp)-kFontMetricsFirstCp;
 else idx=FontMetricsExtraIndex(int(cp));
 if(idx<0)return double(scale)*5.0*0.5;
 return double(kFontAdvanceTable[idx])*ratio;
}

/// Rendered pixel width of `text` at `scale` (no wrapping) -- mirrors Font.measure().
inline int MeasureTextPx(std::string_view text, int scale) {
 double pen=0.0;size_t i=0;
 while(i<text.size()){const char32_t cp=TextMeasureNextUtf8(text,i);if(cp=='\n')continue;pen+=GlyphAdvancePx(cp,scale);}
 return int(pen);
}

/// A string split on ASCII spaces (word-wrap unit boundary), '\n' kept as its own empty-word
/// marker is NOT needed here since callers pre-split on '\n' like Font.lines() does.
inline std::vector<std::string> SplitSpaces(std::string_view s) {
 std::vector<std::string> out;size_t start=0;
 for(size_t i=0;i<=s.size();++i){
  if(i==s.size()||s[i]==' '){out.emplace_back(s.substr(start,i-start));start=i+1;}
 }
 return out;
}

/// Greedy word-wrap line count of `text` at `scale`/`wrap` px, each '\n' a hard paragraph break
/// -- mirrors Font.lines() exactly (word-by-word accumulation, break when the next word would
/// exceed `wrap`). Returns at least 1 for non-empty text, 0 for empty.
inline int WrappedLineCount(std::string_view text, int scale, int wrap) {
 if(text.empty())return 0;
 int total=0;size_t pos=0;
 while(true){
  const size_t nl=text.find('\n',pos);
  const std::string_view para=text.substr(pos,nl==std::string_view::npos?std::string_view::npos:nl-pos);
  if(wrap<=0){++total;}
  else{
   const auto words=SplitSpaces(para);
   std::string cur;int lines_here=0;
   for(const auto& word:words){
    std::string t=cur.empty()?word:cur+" "+word;
    if(MeasureTextPx(t,scale)<=wrap||cur.empty())cur=t;
    else{++lines_here;cur=word;}
   }
   ++lines_here; // the trailing `cur`
   total+=lines_here;
  }
  if(nl==std::string_view::npos)break;
  pos=nl+1;
 }
 return total;
}

/// Runtime's `fit_text` behaviour (render-pages.py text()): shrink `scale` down to `floor` while
/// the WHOLE (unwrapped) text is wider than `wrap`, then stop -- wrapping still happens
/// afterward at whatever scale this returns. No-op (returns `scale`) when `floor>=scale` or
/// `wrap<=0`, matching the renderer's guard.
inline int FitTextScale(std::string_view text, int scale, int wrap, int floor) {
 if(wrap<=0||floor>=scale)return scale;
 int s=scale;
 while(s>floor&&MeasureTextPx(text,s)>wrap)--s;
 return s;
}

/// Effective (post-fit_text, post-max_lines-cap) line count for a manifest label-style widget:
/// shrink to fit one line down to `min_scale` (fit_text), wrap at the resulting scale, cap at
/// `max_lines` (0 = no cap). Returns 0 for empty text so callers can special-case "no content".
inline int LabelEffectiveLines(std::string_view text, int scale, int wrap, int min_scale, int max_lines) {
 if(text.empty())return 0;
 const int s=FitTextScale(text,scale,wrap,min_scale);
 const int n=WrappedLineCount(text,s,wrap);
 return (max_lines>0&&n>max_lines)?max_lines:n;
}

/// Pixel height of an `n`-line block at `scale`, default line gap (renderer's `scale*3`) --
/// mirrors `pitch = scale*5 + scale*3` in render-pages.py's text(). 0 for n<=0.
inline int TextBlockHeightPx(int scale, int n) {
 if(n<=0)return 0;
 const int pitch=scale*5+scale*3;
 return scale*5+(n-1)*pitch;
}

/// Layout batch 2 fix round 1 (2026-10-19, critic finding): a manifest label's `max_lines` is a
/// STATIC per-widget cap -- it cannot follow a module-computed, selection-dependent line budget
/// (e.g. Recipes' description, whose available height shrinks as the ingredient list grows). A
/// widget whose static cap is wider than the actual available space will draw text past its own
/// pane into whatever sits below it (found live: a 3-line description overflowing onto the
/// "Hold ingredients" button's top edge). Fix at the TEXT level instead of the widget level:
/// truncate the string itself, word-by-word, to fit within `max_lines` at `scale`/`wrap`,
/// appending an ellipsis to the last kept line (trimming further words first if the ellipsis
/// itself would not fit) when something was cut. Re-wrapping this returned string at the SAME
/// scale/wrap the module already computed from is then guaranteed to reproduce these exact line
/// breaks (every kept line, by construction, already fits within `wrap`) -- the widget's own
/// `max_lines` stays a generous static ceiling (never the active constraint) and the renderer's
/// wrap pass becomes a no-op over this pre-truncated text.
inline std::string TruncateToLines(std::string_view text, int scale, int wrap, int max_lines) {
 if(text.empty()||max_lines<=0)return std::string();
 // Mirror WrappedLineCount's greedy word-wrap exactly, but collect the actual line text (every
 // line, no early cutoff -- descriptions are short, so this is cheap) instead of only counting.
 std::vector<std::string> lines;
 size_t pos=0;
 while(true) {
  const size_t nl=text.find('\n',pos);
  const std::string_view para=text.substr(pos,nl==std::string_view::npos?std::string_view::npos:nl-pos);
  const auto words=SplitSpaces(para);
  std::string cur;
  for(const auto& word:words) {
   std::string t=cur.empty()?word:cur+" "+word;
   if(MeasureTextPx(t,scale)<=wrap||cur.empty())cur=t;
   else { lines.push_back(cur); cur=word; }
  }
  lines.push_back(cur);
  if(nl==std::string_view::npos)break;
  pos=nl+1;
 }
 const bool truncated=int(lines.size())>max_lines;
 if(truncated)lines.resize(size_t(max_lines));
 if(truncated) {
  std::string& last=lines.back();
  // Trim trailing words until "<line>..." fits `wrap`, or nothing is left.
  while(!last.empty() && MeasureTextPx(last+"...",scale)>wrap) {
   const auto sp=last.find_last_of(' ');
   if(sp==std::string::npos) { last.clear(); break; }
   last=last.substr(0,sp);
  }
  last+="...";
 }
 std::string out;
 for(size_t i=0;i<lines.size();++i) { if(i)out+='\n'; out+=lines[i]; }
 return out;
}

} // namespace botw
