// Demonstrate the TEA core: update(Msg) -> Cmds, asserted directly.
#include <cstdio>
#include <string>
#include "gvte/term/update.hpp"
using namespace gvte;
static int fails=0;
void ok(bool c,const char*n){ printf("%s %s\n",c?"ok  ":"FAIL",n); if(!c)fails++; }
std::string writes(const Cmds&cs){ std::string s; for(auto&c:cs) if(auto*w=std::get_if<WriteChild>(&c)) s+=w->bytes; return s; }
int main(){
  Config cfg; term::Model m{cfg, Extent{80,24}};
  // ChildOutput with a DA1 query -> a WriteChild reply Cmd (pure).
  ok(writes(term::feed_output(m,"\x1b[c"))=="\x1b[?62;1;6;22c", "feed_output DA1 -> WriteChild reply");
  // OSC 2 title -> SetTitle Cmd.
  { Cmds c=term::feed_output(m,"\x1b]2;hello\x07"); bool t=false;
    for(auto&x:c) if(auto*s=std::get_if<SetTitle>(&x)) t=(s->title=="hello");
    ok(t,"feed_output OSC2 -> SetTitle(hello)"); }
  // OSC 52 clipboard -> SetClipboard Cmd (base64 "hi"=aGk=).
  { Cmds c=term::feed_output(m,"\x1b]52;c;aGk=\x07"); bool t=false;
    for(auto&x:c) if(auto*s=std::get_if<SetClipboard>(&x)) t=(s->text=="hi");
    ok(t,"feed_output OSC52 -> SetClipboard(hi)"); }
  // BEL -> RingBell Cmd.
  { Cmds c=term::feed_output(m,"\x07"); bool t=false;
    for(auto&x:c) if(std::holds_alternative<RingBell>(x)) t=true;
    ok(t,"feed_output BEL -> RingBell"); }
  printf(fails?"%d TEA test(s) failed\n":"all TEA tests passed\n",fails);
  return fails?1:0;
}
