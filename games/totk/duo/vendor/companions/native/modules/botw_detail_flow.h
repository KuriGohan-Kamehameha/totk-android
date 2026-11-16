// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
namespace botw::flow {
// Iteration 13 items 2-4: one box model for every native attribute well (stat, modifier,
// hearts, food/armor effect). A well is 41 px tall with an 8 px inset on every side (text and
// 25 px icons sit 8 px from the top and bottom), wells in a row are 8 px apart and rows are
// 49 px apart (41 + 8), so boxes on adjacent rows can never overlap.
constexpr int kHeight=41,kPad=8,kGap=8,kPitch=kHeight+kGap;
// Manifest wells exist in 16 px width steps (32..448); the module publishes the exact
// quantised width so the drawn well and the content-centring math agree.
constexpr int kStep=16,kMin=32,kMax=448;
inline int quantise(int natural){return std::clamp((natural+kStep-1)/kStep*kStep,kMin,kMax);}
// Content of natural width `natural` (including both pads) centred in a quantised well: the
// offset added to the left pad, so the left and right margins differ by at most 1 px.
inline int centre_offset(int natural,int width){return std::max(0,(width-natural)/2);}
struct Box{int x{},row{},width{};int right()const{return x+width;}};
// Left-to-right flow with wrapping. A box that does not fit after another box starts the next
// row at `left`; the first box on a row is never wrapped (it is clipped by its own width cap).
class Flow {
 int left_,right_,x_,row_{};bool empty_{true};
 public:
 Flow(int left,int right,int start):left_(left),right_(right),x_(start){}
 Box place(int width){
  if(!empty_&&x_+width>right_){++row_;x_=left_;}
  const Box b{x_,row_,width};x_+=width+kGap;empty_=false;return b;
 }
 void newline(){++row_;x_=left_;empty_=true;}
 int rows()const{return row_+1;}
};
} // namespace botw::flow
