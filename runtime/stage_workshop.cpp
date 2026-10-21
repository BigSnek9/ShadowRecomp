// Stage Workshop's optional, bounded event runtime. No sidecar means no gameplay changes.
// Native visual/collision/SET data stays in mods/files; this sidecar adds player-triggered actions.
#include "object_distance.h"
namespace boss { void on_set_loaded(); }
namespace set_capacity { uint32_t cap_of(uint32_t manager); }   // set_capacity.cpp (#158)
#include "stage_workshop.h"
#include "sys.h"
#include "game.h"
#include "input.h"
#include "lighting_environment.h"
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
void enemy_ai_stand(uint32_t record, bool on);   // enemy_ai.cpp
bool audio_play_file(const char* path);   // audio.cpp: one-shot WAV (VM play sound)
uint32_t custom_stats_route_mission(int route);   // custom_stats.cpp
void hle_MissionComplete(Cpu&);
void hle_MissionFail(Cpu&);
namespace settings { extern std::string g_hud_values[3]; }   // settings_ui.cpp draws them
void hud_text_show(const char* text, double seconds);   // settings_ui.cpp

namespace stage_workshop {
namespace {
struct V { float x,y,z; };
bool ram(uint32_t a);   // guest RAM pointer check (defined with the capture code below)
uint32_t class_id(uint32_t entry);   // SET class id list<<8|type of a SetManager entry (capture code below)

#pragma pack(push,1)
struct Header { char magic[8]; uint32_t stage,version,ntriggers,nactions,has_spawn; V spawn; };
struct Volume { char name[64]; uint32_t event,flags; V position,size,rotation; float cooldown; uint32_t first,count; };
struct Condition { char name[64]; float value; uint32_t compare; };
struct Action { uint32_t type; V vector; float value,delay; char name[64]; int32_t target; uint32_t nconditions; Condition conditions[4]; };
#pragma pack(pop)
static_assert(sizeof(Header)==40 && sizeof(Volume)==120 && sizeof(Action)==384);
struct State { bool inside=false,fired=false,enabled=true; double next=0; };
struct Pending { Action action; double due; };
Header g_header{};
std::vector<Volume> g_volumes;
std::vector<Action> g_actions;
std::vector<State> g_state;
std::vector<Pending> g_pending;
std::unordered_map<std::string,float> g_variables;
int g_stage=-1;
uint32_t g_owner=0;
uint64_t g_frame=0,g_step_frame=~0ull;
double g_time=0;
bool g_spawn_pending=false;
uint16_t g_buttons=0;
float g_last_stage_time=-1;
bool finite(V p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
bool player(uint32_t p) { return p>=0x80600000 && p<RT_RAM_END-0x400 && ld32(p+0x38)==0x80521C50; }
V position(uint32_t p) { return {float(ldf32(p)),float(ldf32(p+4)),float(ldf32(p+8))}; }
void put(uint32_t p,V v) { stf32(p,v.x);stf32(p+4,v.y);stf32(p+8,v.z); }
void teleport(uint32_t p,V v) {
  // Position and the collision movement point used by8009728C. No guessed orientation writes.
  put(p,v);put(p+0xD0,v);put(p+0x18,{0,0,0});put(p+0x24,{0,0,0});
  st32(p+0xA8,ld32(p+0xA8)&~4u);
  logf("[workshop] teleport (%g,%g,%g)\n",v.x,v.y,v.z);
}
void vm_load(int stage);void vm_fire(uint32_t event,uint32_t kind,int32_t owner,const char* custom=nullptr);void vm_step();bool vm_on();
void load_v3(int stage) {
  for(const auto& a:g_actions)if(a.type==6)lighting::set_environment_trigger(a.name,false);
  g_header={};g_volumes.clear();g_actions.clear();g_state.clear();g_pending.clear();g_variables.clear();
  g_owner=0;g_time=0;g_spawn_pending=false;g_buttons=0;g_stage=stage;
  g_last_stage_time=-1;
  char exe[MAX_PATH];GetModuleFileNameA(nullptr,exe,MAX_PATH);std::string path=exe;
  path=path.substr(0,path.find_last_of("\\/"))+"\\mods\\stages\\stg";
  char name[32];snprintf(name,sizeof name,"%04d.bin",stage);path+=name;
  std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return;
  const auto size=f.tellg();if(size<sizeof(Header)||size>2*1024*1024)return;
  f.seekg(0);Header h{};f.read(reinterpret_cast<char*>(&h),sizeof h);
  if(memcmp(h.magic,"SHSTAGE2",8)||(h.version!=2&&h.version!=3)||h.stage!=uint32_t(stage)||h.ntriggers>512||h.nactions>4096||h.has_spawn>1||!finite(h.spawn))return;
  const uint64_t expected=sizeof(Header)+uint64_t(h.ntriggers)*sizeof(Volume)+uint64_t(h.nactions)*sizeof(Action);
  if(uint64_t(size)!=expected)return;
  std::vector<Volume> volumes(h.ntriggers);std::vector<Action> actions(h.nactions);
  f.read(reinterpret_cast<char*>(volumes.data()),volumes.size()*sizeof(Volume));
  f.read(reinterpret_cast<char*>(actions.data()),actions.size()*sizeof(Action));if(!f)return;
  for(const auto& v:volumes)if(v.name[63]||v.event>(h.version>=3?7u:3u)||!finite(v.position)||!finite(v.size)||!finite(v.rotation)||
    v.size.x<=0||v.size.y<=0||v.size.z<=0||!std::isfinite(v.cooldown)||v.cooldown<0||uint64_t(v.first)+v.count>h.nactions)return;
  for(const auto& a:actions) {
    if(a.name[63]||!finite(a.vector)||!std::isfinite(a.value)||!std::isfinite(a.delay)||a.nconditions>4||
      a.delay<0||a.delay>3600||a.type<1||a.type>8||a.target<-1||a.target>=int32_t(h.ntriggers))return;
    for(uint32_t i=0;i<a.nconditions;++i)if(!a.conditions[i].name[0]||a.conditions[i].name[63]||!std::isfinite(a.conditions[i].value)||a.conditions[i].compare>5)return;
  }
  g_header=h;g_volumes=std::move(volumes);g_actions=std::move(actions);g_state.resize(h.ntriggers);g_spawn_pending=h.has_spawn;
  logf("[workshop] stage%d loaded %u volumes, %u actions, spawn%d\n",stage,h.ntriggers,h.nactions,h.has_spawn);
}
void load(int stage){load_v3(stage);vm_load(stage);}   // v4 (if present) takes over all logic; v3 keeps geometry
bool contains(const Volume& v,V world) {
  V p{world.x-v.position.x,world.y-v.position.y,world.z-v.position.z};
  constexpr float d=3.14159265359f/180;
  // Invert editor Y*X*Z transform in reverse order.
  const float ay=-v.rotation.y*d,ax=-v.rotation.x*d,az=-v.rotation.z*d;
  p={p.x*std::cos(ay)+p.z*std::sin(ay),p.y,-p.x*std::sin(ay)+p.z*std::cos(ay)};
  p={p.x,p.y*std::cos(ax)-p.z*std::sin(ax),p.y*std::sin(ax)+p.z*std::cos(ax)};
  p={p.x*std::cos(az)-p.y*std::sin(az),p.x*std::sin(az)+p.y*std::cos(az),p.z};
  return std::fabs(p.x)<=v.size.x*.5f&&std::fabs(p.y)<=v.size.y*.5f&&std::fabs(p.z)<=v.size.z*.5f;
}
float condition_value(const char* name) {
  // Read-only live values share the same condition representation as project variables.
  if(!strcmp(name,"@rings"))return float(ld32(0x8057670C));
  if(!strcmp(name,"@score"))return float(ld32(0x80576710));
  if(!strcmp(name,"@lives"))return float(ld32(0x80576704));
  if(!strcmp(name,"@hero_score"))return float(ld32(0x80576718));
  if(!strcmp(name,"@dark_score"))return float(ld32(0x80576714));
  if(!strcmp(name,"@stage_time"))return float(ldf32(0x8057D734));
  if(!strcmp(name,"@player_y"))return float(ldf32(g_owner+4));
  if(!strcmp(name,"@speed")) {
    const V v=position(g_owner+0x18);return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
  }
  auto it=g_variables.find(name);return it==g_variables.end()?0:it->second;
}
void execute(const Action& a) {
  for(uint32_t i=0;i<a.nconditions;++i) {
    const auto& c=a.conditions[i];const float v=condition_value(c.name);
    const bool eq=std::fabs(v-c.value)<=0.0001f;
    const bool pass=c.compare==0?eq:c.compare==1?!eq:c.compare==2?v<c.value:c.compare==3?v<=c.value:c.compare==4?v>c.value:v>=c.value;
    if(!pass)return;
  }
  switch(a.type) {
  case 1:teleport(g_owner,a.vector);break;
  case 2:put(g_owner+0x18,a.vector);if(a.vector.y>0)st32(g_owner+0xA8,ld32(g_owner+0xA8)&~4u);break;   // upward: leave the floor (grounded flag), else the step cancels it
  case 3:st32(0x8057670C,uint32_t(std::clamp(a.value,0.f,9999.f)));break;
  case 4:st32(0x80576710,uint32_t(std::clamp(double(ld32(0x80576710))+a.value,0.,99999999.)));break;
  case 5:if(g_variables.size()<128||g_variables.count(a.name))g_variables[a.name]=a.value;break;
  case 6:lighting::set_environment_trigger(a.name,a.value!=0);break;
  case 7:case 8:if(a.target>=0)g_state[a.target].enabled=a.type==7;break;
  }
  logf("[workshop] action%u %s value%g\n",a.type,a.name,a.value);
}
// ---- native SET object events (sidecar v3; volumes with event 4 spawn, 5 killed, 6 removed/collected, 7 despawned).
// Such a volume names its object by position.x = layer (0 cmn, 1 nrm, 2 hrd, 3 ds1) and position.y = record index in that
// layer's file. The game's SetManager (holder 0x805EE6D4: +0x18 entry array of 0x20-byte entries, entry i <-> record i,
// entry+0xC = live instance) loads cmn, then nrm OR hrd, then ds1 into one record array (fn_800CADBC appends a file's
// records; its count at manager+0). Research and citations: agents/claude/research/objhooks (fn addresses below).
struct LayerRange { int layer; uint32_t first, end; };
std::vector<LayerRange> g_layers;
int g_cause=0;            // set while a kill / remove wrapper runs: 5 killed, 6 removed
bool g_in_remove=false;
uint32_t manager() { const uint32_t m=ld32(0x805EE6D4); return m>=0x80000000&&m<RT_RAM_END?m:0; }
bool locate(uint32_t entry,int& layer,uint32_t& index) {
  const uint32_t m=manager();if(!m)return false;const uint32_t base=ld32(m+0x18);
  if(entry<base||entry>=base+set_capacity::cap_of(m)*0x20||(entry-base)&0x1F)return false;   // #158: 2,048 or the raised size
  const uint32_t idx=(entry-base)>>5;
  for(const auto& r:g_layers)if(idx>=r.first&&idx<r.end){layer=r.layer;index=idx-r.first;return true;}
  return false;
}
void object_event(uint32_t entry,uint32_t event) {
  if(vm_on()){int layer;uint32_t index;if(locate(entry,layer,index))vm_fire(event,2,int32_t((uint32_t(layer)<<16)|index));return;}
  if(!g_header.version||g_header.version<3)return;
  int layer;uint32_t index;if(!locate(entry,layer,index))return;
  for(size_t i=0;i<g_volumes.size();++i) {
    const auto& v=g_volumes[i];auto& s=g_state[i];
    if(v.event!=event||int(v.position.x)!=layer||uint32_t(v.position.y)!=index)continue;
    if(!s.enabled||((v.flags&1)&&s.fired)||g_time<s.next)continue;
    s.fired=true;s.next=g_time+std::max(v.cooldown,.05f);
    logf("[workshop] object %s (layer%d #%u) event%u\n",v.name,layer,index,event);
    for(uint32_t j=0;j<v.count&&g_pending.size()<2048;++j){const auto& a=g_actions[v.first+j];g_pending.push_back({a,g_time+a.delay});}
  }
}
// ---- sidecar v4: the node-graph VM (roadmap 5b; format docs/stage-workshop/SIDECAR-V4.md, compiler
// tools/stage_editor/events.py compile_graph_v4, reference interpreter events.V4Machine). mods/stages/stgNNNN.v4.bin
// next to the v3 file: v3 then only supplies volume geometry / the spawn point and every event runs in the VM.
// Stack machine over floats, coroutines that WAIT in elapsed stage seconds (g_time: pause excluded, frame-rate
// independent), per-coroutine / per-frame instruction budgets, bounded coroutine count and stack depth.
struct VmObj { uint32_t kind; int32_t a,b; float x,y,z; };
struct VmVar { uint16_t name,flags; float init; };
struct VmEntry { uint32_t event,owner_kind; int32_t owner; uint32_t flags; float period; uint32_t addr,name; };
struct VmIns { uint8_t op,sub; uint16_t node; int32_t arg; };
struct VmCo { uint32_t pc; double wake; std::vector<float> st; uint32_t last_node=0xFFFFFFFF; };
// 5d live highlight: <game folder>/live-highlight.jsonl while a v4 program runs (UI 2 shows the running nodes);
// one 'nodes' line per frame with the nodes that executed, 'vars' when a named variable changed (<= 4 per s),
// 'error' lines, capped at 2 MB (then 'capped' and silence)
FILE* g_live=nullptr;size_t g_live_bytes=0;std::vector<uint16_t> g_live_nodes;double g_live_vars_t=-1;std::vector<float> g_live_vals;
void live_write(const std::string& line) {
  if(!g_live)return;
  if(g_live_bytes+line.size()>2u*1024*1024){fputs("{\"ev\":\"capped\"}\n",g_live);fclose(g_live);g_live=nullptr;return;}
  fputs(line.c_str(),g_live);fflush(g_live);g_live_bytes+=line.size();
}
std::string jesc(const std::string& s){std::string o;for(char ch:s){if(ch=='"'||ch=='\\')o+='\\';if(uint8_t(ch)>=0x20)o+=ch;}return o;}
// 5c object operations, queued by the VM and applied at the start of the next frame (frame_begin: outside every object
// update, with a Cpu to call the game's own functions)
struct VmOp { uint32_t type,obj; V to; float secs; };
std::vector<VmOp> g_ops;
// logic VM HUD values (ACT 23 / 24): "label: variable" on HUD line 1-3 until hidden; formatted every frame (hud_values_tick)
struct HudVal { std::string label; int var = -1; bool on = false; } g_hud_vals[3];
std::string g_vm_dir;   // <game>\mods (play sound looks in mods\sounds)
struct Glide { uint32_t entry; V from,to; double t0,secs; };
std::vector<Glide> g_glides;
std::unordered_map<uint32_t,uint32_t> g_pos_off;   // instance -> offset of its position copy (found at create)
std::unordered_map<uint32_t,std::vector<uint32_t>> g_roots;   // SET entry -> RenderWare root frames of its model
struct Find { uint32_t entry; V old; int frames; };
std::vector<Find> g_find;
uint32_t frame_root(uint32_t fr){for(int i=0;i<16&&ram(fr)&&ram(ld32(fr+4))&&ld8(ld32(fr+4))==0;++i)fr=ld32(fr+4);return fr;}   // RwFrame: type 0, parent +4
void shift_tree(uint32_t root,V d) {   // modelling position of the root, world (LTM) position of it and every child
  if(!ram(root))return;
  stf32(root+0x40,float(ldf32(root+0x40))+d.x);stf32(root+0x44,float(ldf32(root+0x44))+d.y);stf32(root+0x48,float(ldf32(root+0x48))+d.z);
  std::function<void(uint32_t,int)> ltm=[&](uint32_t fr,int depth){
    for(int n=0;n<256&&ram(fr)&&depth<16;++n,fr=ld32(fr+0x9C)){
      stf32(fr+0x80,float(ldf32(fr+0x80))+d.x);stf32(fr+0x84,float(ldf32(fr+0x84))+d.y);stf32(fr+0x88,float(ldf32(fr+0x88))+d.z);
      ltm(ld32(fr+0x98),depth+1);if(depth==0)break;}};
  ltm(root,0);
}
void find_atomic(uint32_t at) {   // draw hooks, while a Move looks for its model
  if(!ram(at)||ld8(at)!=1)return;const uint32_t root=frame_root(ld32(at+4));if(!ram(root))return;
  const float x=float(ldf32(root+0x80)),y=float(ldf32(root+0x84)),z=float(ldf32(root+0x88));
  for(auto& f:g_find) {
    if(std::fabs(x-f.old.x)>1||std::fabs(z-f.old.z)>1||std::fabs(y-f.old.y)>15)continue;
    auto& r=g_roots[f.entry];if(std::find(r.begin(),r.end(),root)!=r.end())continue;
    const uint32_t rec=ld32(f.entry);if(!ram(rec))continue;const V now=position(rec);
    shift_tree(root,{now.x-f.old.x,now.y-f.old.y,now.z-f.old.z});r.push_back(root);
    logf("[workshop] vm move: model frame %08X attached (atomic %08X)\n",root,at);
  }
}
struct Vm {
  bool ok=false; uint32_t per_co=256,per_frame=4096,max_co=64,max_stack=64,v3_triggers=0;
  std::vector<float> consts; std::vector<std::string> strings; std::vector<VmObj> objs; std::vector<VmVar> vars;
  std::vector<VmEntry> entries; std::vector<VmIns> code; std::vector<std::string> nodes;
  std::vector<float> val; std::vector<VmCo> cos; std::vector<double> last; std::vector<uint8_t> done;
  uint32_t dropped=0; bool started=false; uint32_t rng=0x12345678u;
} g_vm;
bool vm_on(){return g_vm.ok;}
uint32_t vm_entry_of(uint32_t o) {   // SetManager entry of a v4 native object (kind 2: layer a, record b), or 0
  if(o>=g_vm.objs.size()||g_vm.objs[o].kind!=2)return 0;const uint32_t m=manager();if(!m)return 0;
  for(const auto& r:g_layers)if(r.layer==g_vm.objs[o].a&&uint32_t(g_vm.objs[o].b)<r.end-r.first)return ld32(m+0x18)+(r.first+uint32_t(g_vm.objs[o].b))*0x20;
  return 0;
}
// #43 class table for ACT 14 set parameter - EMPTY for now. Research (WS_WATCHCLS, homing-attack hits): the Black
// Arms soldier 008D shows its health as a fraction at +0x2A8 (1 -> 0.5 per homing attack, two kill) and the GUN
// soldier 0064 at +0x2E8, but both are display copies the game rebuilds every frame (a write of 0.5 is back to 1 the
// next frame); the stored health is elsewhere (not in the first 0x1000 bytes). GUN Beetle 0065 / Egg Pawn 0079 die
// from one hit, Bk Worm 0090 is not hurt by a homing attack. Speed fields not identified.
// #43 FOUND (2026-10-17, WS_HPLOG hooks on the HP bar update fn_801A102C): every enemy keeps its health in its status
// object as floats, +0x78 = max, +0x7C = current (getters vt+0xB0 fn_8017B84C / vt+0xAC fn_8017B844, shared by all
// enemy classes). Measured max: GUN Soldier 0064 = 2, GUN Beetle 0065 = 2, Egg Pawn 0079 = 4, Black Arms soldier 008D
// = 4, Bk Worm 0090 = 12; a homing attack takes 2. The status object lies a class-specific distance BELOW the SET
// instance (0x258..0x328), so it is found by that vtable signature. 0 = not found.
uint32_t enemy_status(uint32_t inst) {
  if(!ram(inst))return 0;
  for(uint32_t off=4;off<=0x800;off+=4){const uint32_t x=inst-off,vt=ld32(x);
    if(vt>=0x80400000&&vt<0x80600000&&ld32(vt+0xAC)==0x8017B844&&ld32(vt+0xB0)==0x8017B84C)return x;}
  return 0;
}
// field 1 = health as a fraction of max, 2 = health in points, 3 = max health in points (a full enemy stays full)
bool enemy_set_health(uint32_t inst,uint32_t field,float v) {
  const uint32_t x=enemy_status(inst);if(!x||!std::isfinite(v))return false;
  const float mx=float(ldf32(x+0x78)),cur=float(ldf32(x+0x7C));
  if(field==1)stf32(x+0x7C,std::clamp(v,0.f,1.f)*mx);
  else if(field==2)stf32(x+0x7C,std::clamp(v,0.f,mx));
  else if(field==3){const float m=std::clamp(v,1.f,10000.f);stf32(x+0x78,m);stf32(x+0x7C,cur>=mx?m:std::min(cur,m));}
  else return false;
  logf("[workshop] enemy %08X status %08X health field %u = %g (max %g, now %g)\n",inst,x,field,double(v),double(ldf32(x+0x78)),double(ldf32(x+0x7C)));
  return true;
}
const char* vm_node(uint32_t pc){return pc<g_vm.code.size()&&g_vm.code[pc].node<g_vm.nodes.size()?g_vm.nodes[g_vm.code[pc].node].c_str():"?";}
void vm_load(int stage) {
  g_ops.clear();g_glides.clear();g_pos_off.clear();g_roots.clear();g_find.clear();
  g_vm=Vm{};for(auto& h:g_hud_vals)h=HudVal{};
  char exe[MAX_PATH];GetModuleFileNameA(nullptr,exe,MAX_PATH);std::string path=exe;
  g_vm_dir=path.substr(0,path.find_last_of("\\/"))+"\\mods";
  char name[40];snprintf(name,sizeof name,"\\mods\\stages\\stg%04d.v4.bin",stage);
  path=path.substr(0,path.find_last_of("\\/"))+name;
  std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return;
  const size_t size=size_t(f.tellg());if(size<64||size>4*1024*1024){logf("[workshop] vm: %s bad size\n",path.c_str());return;}
  std::vector<uint8_t> d(size);f.seekg(0);f.read(reinterpret_cast<char*>(d.data()),size);if(!f)return;
  auto fail=[&](const char* why){logf("[workshop] vm: stg%04d.v4.bin rejected (%s) - v3 logic stays in use\n",stage,why);g_vm=Vm{};};
  if(memcmp(d.data(),"SWVM0004",8))return fail("magic");
  uint32_t h[14];memcpy(h,d.data()+8,sizeof h);
  const uint32_t nc=h[2],ns=h[3],no=h[4],nv=h[5],ne=h[6],ncode=h[7],nn=h[8];
  if(h[0]!=uint32_t(stage))return fail("stage");
  if(nc>65536||ns>65536||no>65536||nv>65536||ne>4096||ncode>262144||nn>65536)return fail("counts");
  size_t p=64;auto need=[&](size_t n){return p+n<=size;};
  if(!need(4ull*nc))return fail("constants");g_vm.consts.resize(nc);memcpy(g_vm.consts.data(),d.data()+p,4ull*nc);p+=4ull*nc;
  auto str=[&](std::vector<std::string>& out,uint32_t n){for(uint32_t i=0;i<n;++i){if(!need(2))return false;uint16_t l;memcpy(&l,d.data()+p,2);p+=2;
    if(!need(l))return false;out.emplace_back(reinterpret_cast<const char*>(d.data()+p),l);p+=l;}return true;};
  if(!str(g_vm.strings,ns))return fail("strings");p+=(4-p%4)%4;
  if(!need(32ull*no))return fail("objects");
  for(uint32_t i=0;i<no;++i){VmObj o;memcpy(&o,d.data()+p,sizeof o);p+=32;g_vm.objs.push_back(o);}
  if(!need(8ull*nv))return fail("variables");
  for(uint32_t i=0;i<nv;++i){VmVar v;memcpy(&v,d.data()+p,8);p+=8;g_vm.vars.push_back(v);}
  if(!need(28ull*ne))return fail("entries");
  for(uint32_t i=0;i<ne;++i){VmEntry e;memcpy(&e,d.data()+p,28);p+=28;g_vm.entries.push_back(e);}
  if(!need(8ull*ncode))return fail("code");
  for(uint32_t i=0;i<ncode;++i){VmIns c;memcpy(&c,d.data()+p,8);p+=8;g_vm.code.push_back(c);}
  if(!str(g_vm.nodes,nn))return fail("nodes");
  if(p!=size)return fail("trailing bytes");
  // every reference checked once here, so the interpreter needs no per-instruction bounds checks
  for(const auto& c:g_vm.code) {
    const uint32_t a=uint32_t(c.arg);
    if(c.op>20||c.node>=std::max<size_t>(1,g_vm.nodes.size())&&!g_vm.nodes.empty())return fail("opcode / node");
    if(c.op==2&&a>=nc)return fail("PUSH index");
    if((c.op==3||c.op==4)&&a>=nv)return fail("variable index");
    if(c.op==5&&a>7&&(a<100||a-100>=no))return fail("LIVE index");   // 100+i = distance from the player to object i
    if((c.op==6&&c.sub>5)||(c.op==7&&c.sub>5))return fail("MATH / CMP sub");
    if((c.op==9||c.op==10||c.op==11||c.op==14)&&a>=ncode)return fail("jump target");
    if(c.op==15&&(c.sub<1||c.sub>25||c.sub==5))return fail("ACT sub");
  }
  for(const auto& e:g_vm.entries)if(e.addr>=ncode||e.event>11||!std::isfinite(e.period)||e.period<0||(e.name!=0xFFFF&&e.name>=ns))return fail("entry");
  for(const auto& v:g_vm.vars)if(v.name!=0xFFFF&&v.name>=ns)return fail("variable name");
  for(const auto& o:g_vm.objs)if(!std::isfinite(o.x)||!std::isfinite(o.y)||!std::isfinite(o.z))return fail("object position");
  g_vm.per_co=std::clamp<uint32_t>(h[9],1,100000);g_vm.per_frame=std::clamp<uint32_t>(h[10],1,1000000);
  g_vm.max_co=std::clamp<uint32_t>(h[11],1,1024);g_vm.max_stack=std::clamp<uint32_t>(h[12],4,1024);g_vm.v3_triggers=h[13];
  for(const auto& v:g_vm.vars)g_vm.val.push_back(v.init);
  g_vm.last.assign(ne,-1e9);g_vm.done.assign(ne,0);
  for(size_t i=0;i<ne;++i)if(g_vm.entries[i].event==9)g_vm.last[i]=0;   // every N s: N, 2N, ... of stage time
  g_vm.ok=true;
  if(g_live){fclose(g_live);g_live=nullptr;}
  {const std::string lp=path.substr(0,path.find("\\mods\\"))+"\\live-highlight.jsonl";g_live=fopen(lp.c_str(),"wb");
   g_live_bytes=0;g_live_nodes.clear();g_live_vars_t=-1;g_live_vals.clear();
   char b[160];snprintf(b,sizeof b,"{\"ev\":\"start\",\"stage\":%d,\"instructions\":%u,\"nodes\":%zu}\n",stage,ncode,g_vm.nodes.size());live_write(b);}
  logf("[workshop] vm stg%04d.v4.bin: %u entries, %u instructions, %u objects, %u variables (v3 triggers %u)\n",stage,ne,ncode,no,nv,g_vm.v3_triggers);
}
void vm_start(uint32_t addr) {
  if(g_vm.cos.size()>=g_vm.max_co){if(g_vm.dropped++<16)logf("[workshop] vm: coroutine limit (%u), start at %s dropped\n",g_vm.max_co,vm_node(addr));return;}
  g_vm.cos.push_back({addr,g_time,{}});
}
void vm_fire(uint32_t event,uint32_t kind,int32_t owner,const char* custom) {
  if(!g_vm.ok)return;
  for(size_t i=0;i<g_vm.entries.size();++i) {
    const auto& e=g_vm.entries[i];
    if(e.event!=event||e.owner_kind!=kind||e.owner!=owner)continue;
    if(e.name!=0xFFFF&&(!custom||g_vm.strings[e.name]!=custom))continue;
    if((e.flags&1)&&g_vm.done[i])continue;
    if(event!=9&&e.period>0&&g_time-g_vm.last[i]<e.period)continue;
    g_vm.done[i]=1;g_vm.last[i]=g_time;vm_start(e.addr);
  }
}
bool vm_obj_pos(uint32_t o,V& out,bool& alive) {
  alive=true;if(o>=g_vm.objs.size())return false;const auto& ob=g_vm.objs[o];
  auto vol=[&](uint32_t i){if(i>=g_volumes.size())return false;out=g_volumes[i].position;return true;};
  if(ob.kind==1)return vol(uint32_t(ob.a));
  if(ob.kind==3)return vol(g_vm.v3_triggers+uint32_t(ob.a));
  if(ob.kind==2) {   // native SET record: its live record position
    const uint32_t m=manager();if(!m)return false;
    for(const auto& r:g_layers)if(r.layer==ob.a&&uint32_t(ob.b)<r.end-r.first) {
      const uint32_t entry=ld32(m+0x18)+(r.first+uint32_t(ob.b))*0x20,rec=ld32(entry);
      alive=ld32(entry+0xC)!=0;if(rec<0x80000000||rec>=RT_RAM_END)return false;out=position(rec);return true;
    }
    return false;
  }
  out={ob.x,ob.y,ob.z};return true;
}
void vm_volume_enable(uint32_t o,bool on) {
  if(o>=g_vm.objs.size())return;const auto& ob=g_vm.objs[o];
  const uint32_t i=ob.kind==1?uint32_t(ob.a):ob.kind==3?g_vm.v3_triggers+uint32_t(ob.a):~0u;
  if(i<g_state.size())g_state[i].enabled=on;
}
// one coroutine until it ends / waits / yields / runs out of budget; returns {instructions, still alive}
std::pair<uint32_t,bool> vm_run(VmCo& c,uint32_t budget) {
  auto& st=c.st;uint32_t n=0;
  auto pop=[&]()->float{if(st.empty())return 0.f;const float v=st.back();st.pop_back();return v;};
  auto err=[&](const char* what){logf("[workshop] vm error: %s at node %s\n",what,vm_node(c.pc?c.pc-1:0));
    live_write("{\"ev\":\"error\",\"t\":"+std::to_string(g_time)+",\"node\":\""+jesc(vm_node(c.pc?c.pc-1:0))+"\",\"what\":\""+what+"\"}\n");
    return std::pair<uint32_t,bool>{n,false};};
  while(n<budget) {
    if(c.pc>=g_vm.code.size())return err("pc out of range");
    const VmIns in=g_vm.code[c.pc++];++n;
    if(g_live&&in.node!=c.last_node){c.last_node=in.node;
      if(g_live_nodes.size()<256&&std::find(g_live_nodes.begin(),g_live_nodes.end(),in.node)==g_live_nodes.end())g_live_nodes.push_back(in.node);}
    if(st.size()>g_vm.max_stack)return err("stack overflow");
    switch(in.op) {
    case 0:break;
    case 1:return {n,false};
    case 2:st.push_back(g_vm.consts[in.arg]);break;
    case 3:st.push_back(g_vm.val[in.arg]);break;
    case 4:g_vm.val[in.arg]=pop();break;
    case 5:{
      if(in.arg>=100){   // player near: the distance from the player to object (arg - 100); 1e9 when it is not alive / known
        V p{0,0,0};bool al=false;const bool okp=vm_obj_pos(uint32_t(in.arg-100),p,al);
        if(!okp||!al||!player(g_owner)){st.push_back(1e9f);break;}
        const V q=position(g_owner);st.push_back(std::sqrt((p.x-q.x)*(p.x-q.x)+(p.y-q.y)*(p.y-q.y)+(p.z-q.z)*(p.z-q.z)));break;}
      static const char* live[]={"@rings","@score","@lives","@hero_score","@dark_score","@stage_time","@player_y","@speed"};st.push_back(condition_value(live[in.arg]));break;}
    case 6:{const float b=pop(),a=pop();const float r[]={a+b,a-b,a*b,b!=0?a/b:0.f,std::min(a,b),std::max(a,b)};st.push_back(r[in.sub]);break;}
    case 7:{const float b=pop(),a=pop();const bool r[]={a==b,a!=b,a<b,a<=b,a>b,a>=b};st.push_back(r[in.sub]?1.f:0.f);break;}
    case 8:st.push_back(pop()!=0?0.f:1.f);break;
    case 9:c.pc=uint32_t(in.arg);break;
    case 10:if(pop()==0)c.pc=uint32_t(in.arg);break;
    case 11:if(pop()!=0)c.pc=uint32_t(in.arg);break;
    case 12:{const float s=pop();c.wake=g_time+std::clamp(std::isfinite(s)?double(s):0.,0.,3600.);return {n,true};}
    case 13:c.wake=g_time+1e-6;return {n,true};
    case 14:vm_start(uint32_t(in.arg));break;
    case 15:{
      const char* nd=vm_node(c.pc-1);
      switch(in.sub) {
      case 1:{const float o=pop();V p;bool al;if(vm_obj_pos(uint32_t(o),p,al)&&player(g_owner))teleport(g_owner,p);break;}
      case 2:{const float z=pop(),y=pop(),x=pop();if(player(g_owner)){put(g_owner+0x18,{x,y,z});if(y>0)st32(g_owner+0xA8,ld32(g_owner+0xA8)&~4u);}break;}
      case 3:{const float v=pop();st32(0x8057670C,uint32_t(std::clamp(v,0.f,9999.f)));break;}   // SET, like v3
      case 4:{const float v=pop();st32(0x80576710,uint32_t(std::clamp(double(ld32(0x80576710))+v,0.,99999999.)));break;}
      case 6:{const float on=pop(),s=pop();if(uint32_t(s)<g_vm.strings.size())lighting::set_environment_trigger(g_vm.strings[uint32_t(s)].c_str(),on!=0);break;}
      case 7:case 8:vm_volume_enable(uint32_t(pop()),in.sub==7);break;
      case 9:{const float s=pop();if(uint32_t(s)<g_vm.strings.size())vm_fire(10,0,0,g_vm.strings[uint32_t(s)].c_str());break;}
      case 10:{const float z=pop(),y=pop(),x=pop(),o=pop();g_ops.push_back({10,uint32_t(o),{x,y,z},0});st.push_back(o);break;}   // spawn: the pool record itself
      case 14:{   // set parameter (object, field, value): enemy health, fields 1 fraction / 2 points / 3 max (enemy_set_health)
        const float v=pop(),fld=pop(),o=pop();const uint32_t entry=vm_entry_of(uint32_t(o)),inst=entry?ld32(entry+0xC):0;
        if(!(inst&&enemy_set_health(inst,uint32_t(fld),v)))logf("[workshop] vm set parameter: field %u of object %u not available (class %04X, alive %d)\n",uint32_t(fld),uint32_t(o),entry?class_id(entry):0,inst!=0);
        break;}
      case 11:g_ops.push_back({11,uint32_t(pop()),{0,0,0},0});break;
      case 15:case 16:{   // link flag set / clear: the game's switch flags (singleton *(r13 + 0x32B8) = 0x805EE6F8 -> table of
        // 256 x 8 bytes, value at +0; fn_8016904C reads, fn_801690D0 writes) - opens a Link ID group (Digital Breakable Tile ...)
        const float id=pop();const uint32_t mgr=ld32(0x805EE6F8),tab=ram(mgr)?ld32(mgr):0;const int i=int(id);
        if(ram(tab)&&i>=0&&i<256){st32(tab+uint32_t(i)*8,in.sub==15?1u:0u);logf("[workshop] vm link flag %d -> %d\n",i,in.sub==15?1:0);}
        else logf("[workshop] vm link flag %d: no switch-flag table in this stage\n",i);
        break;}
      case 17:case 18:g_ops.push_back({uint32_t(in.sub),uint32_t(pop()),{0,0,0},0});break;   // hide / show (run_ops)
      case 19:case 20:{const float o=pop();const uint32_t entry=vm_entry_of(uint32_t(o));int layer;uint32_t index;
        if(entry){const uint32_t m=manager();const uint32_t rec=m?(entry-ld32(m+0x18))/0x20:0;enemy_ai_stand(rec,in.sub==20);
          logf("[workshop] vm object %u (record %u) AI %s\n",uint32_t(o),rec,in.sub==20?"off (stands)":"on");}
        (void)layer;(void)index;break;}
      case 12:{const float s=pop(),z=pop(),y=pop(),x=pop(),o=pop();g_ops.push_back({12,uint32_t(o),{x,y,z},std::isfinite(s)?std::clamp(s,0.f,3600.f):0.f});break;}
      case 21:case 22:{const float r=pop();g_ops.push_back({uint32_t(in.sub),uint32_t(std::clamp(r,0.f,2.f)),{0,0,0},0});break;}   // mission complete / fail (route 0 dark 1 neutral 2 hero): run_ops
      case 23:{const float slot=pop(),var=pop(),lbl=pop();const int k=std::clamp(int(slot)-1,0,2);   // HUD value: "label: variable" stays on HUD line k
        g_hud_vals[k]={uint32_t(lbl)<g_vm.strings.size()?g_vm.strings[uint32_t(lbl)]:std::string(),int(var),true};
        logf("[workshop] vm HUD value line %d: %s = variable %d\n",k+1,g_hud_vals[k].label.c_str(),int(var));break;}
      case 24:{const int k=std::clamp(int(pop())-1,0,2);g_hud_vals[k].on=false;logf("[workshop] vm HUD value line %d hidden\n",k+1);break;}
      case 25:{const float s=pop();const std::string a=uint32_t(s)<g_vm.strings.size()?g_vm.strings[uint32_t(s)]:std::string();   // play sound: mods\sounds\<asset>[.wav]
        const std::string dir=g_vm_dir+"\\sounds\\";std::string f=dir+a;
        if(GetFileAttributesA(f.c_str())==INVALID_FILE_ATTRIBUTES)f+=".wav";
        if(a.empty()||!audio_play_file(f.c_str()))logf("[workshop] vm play sound \"%s\": no such sound in mods\\sounds (game cues are not supported yet)\n",a.c_str());
        break;}
      default:{   // 13 message: no on-screen text yet - logged only
        const float sec=pop(),s=pop();
        const char* txt=uint32_t(s)<g_vm.strings.size()?g_vm.strings[uint32_t(s)].c_str():"?";
        hud_text_show(txt,std::isfinite(sec)?std::clamp(double(sec),0.5,60.):3.);   // on screen (settings_ui draws it)
        logf("[workshop] vm message \"%s\" %.1f s (on screen)\n",txt,double(sec));break;}
      }
      logf("[workshop] vm act%u node %s t=%.3f rings=%u score=%u\n",in.sub,nd,g_time,ld32(0x8057670C),ld32(0x80576710));break;
    }
    case 16:g_vm.rng=g_vm.rng*1664525u+1013904223u;st.push_back(float(g_vm.rng>>8)/16777216.f);break;
    case 17:{const V p=player(g_owner)?position(g_owner):V{0,0,0};st.push_back(p.x);st.push_back(p.y);st.push_back(p.z);break;}
    case 18:{V p{0,0,0};bool al=false;const bool okp=vm_obj_pos(uint32_t(pop()),p,al);
      if(in.sub==0){st.push_back(p.x);st.push_back(p.y);st.push_back(p.z);}else st.push_back(okp&&al?1.f:0.f);break;}
    case 19:st.push_back(st.empty()?0.f:st.back());break;
    case 20:pop();break;
    }
  }
  return {n,true};   // over budget: suspended to the next frame
}
void vm_step() {
  if(!g_vm.ok)return;
  if(!g_vm.started){g_vm.started=true;vm_fire(8,0,0);}
  for(size_t i=0;i<g_vm.entries.size();++i) {
    const auto& e=g_vm.entries[i];
    if(e.event==9&&e.period>0)while(g_time+1e-9>=g_vm.last[i]+e.period){g_vm.last[i]+=e.period;vm_start(e.addr);}
  }
  uint32_t total=0;
  for(size_t k=0;k<g_vm.cos.size()&&total<g_vm.per_frame;) {
    if(g_vm.cos[k].wake>g_time+1e-9){++k;continue;}
    VmCo c=std::move(g_vm.cos[k]);   // a SPAWN may grow the vector while this one runs
    const auto [n,alive]=vm_run(c,std::min(g_vm.per_co,g_vm.per_frame-total));total+=n;
    if(alive){g_vm.cos[k]=std::move(c);++k;}else g_vm.cos.erase(g_vm.cos.begin()+k);
  }
  if(g_live) {   // 5d: what ran this frame, and changed variables
    char t[32];snprintf(t,sizeof t,"%.3f",g_time);
    if(!g_live_nodes.empty()) {
      std::string l=std::string("{\"ev\":\"nodes\",\"t\":")+t+",\"nodes\":[";
      for(size_t i=0;i<g_live_nodes.size();++i)l+=(i?",\"":"\"")+jesc(g_vm.nodes.size()>g_live_nodes[i]?g_vm.nodes[g_live_nodes[i]]:"?")+"\"";
      live_write(l+"]}\n");g_live_nodes.clear();
    }
    if(g_time-g_live_vars_t>=0.25&&g_live_vals!=g_vm.val) {
      std::string l=std::string("{\"ev\":\"vars\",\"t\":")+t+",\"vars\":{";bool first=true;
      for(size_t i=0;i<g_vm.vars.size();++i) {
        const auto& v=g_vm.vars[i];char b[96];
        const std::string name=v.name!=0xFFFF?g_vm.strings[v.name]:"#slot"+std::to_string(i);   // hidden node-state slots by index
        snprintf(b,sizeof b,"%g",std::isfinite(g_vm.val[i])?double(g_vm.val[i]):0.);
        l+=(first?"\"":",\"")+jesc(name)+"\":"+b;first=false;
      }
      live_write(l+"}}\n");g_live_vals=g_vm.val;g_live_vars_t=g_time;
    }
  }
}
}
}  // namespace stage_workshop

// ---- #35 model capture (WS_CAPTURE=<file>: JSON lines, format in tools/stage_editor/capture/SCHEMA.md).
// Every resource the game takes out of a loaded .one goes through one ResourceOneFile (RTTI, vtable 8051C6BC):
// +0x10 archive path (char*), +0x14 entry table (0x3C-byte rows: name[0x2C] at +0, type word at +0x2C, objects after),
// +0x18 row count. Its virtual getters take a name (strcmp fn_803AC91C over the rows) or an index (row = index - 2).
// Callers reach them through ResourceManager wrappers fn_80012510..fn_8001277C (manager fn_800122D4, keyed by slot).
// While hle_SetCreate runs, each lookup is tagged with the SET record being created.
namespace boss_slots { void enter(uint32_t obj); void leave(); }   // boss_slots.cpp (#104 two bosses)
namespace stage_workshop {
namespace {
FILE* g_cap=nullptr;
bool g_cap_init=false;
struct Creating { int layer; uint32_t index, entry, cls; uint32_t lookups; uint32_t rec; };
std::vector<Creating> g_creating;                 // SET records whose create() is running (hle_SetCreate)
std::unordered_map<uint32_t,Creating> g_instances; // live instance (entry+0xC) -> its record, from create
std::vector<uint32_t> g_nodes;                    // task-tree nodes being run (fn_8004EC68 per-node call, node_enter)
std::unordered_map<std::string,int> g_cap_seen;   // each (record or none, archive, name, caller) is logged once
std::unordered_map<uint32_t,std::string> g_res_name;   // object a by-name lookup returned -> its name
std::unordered_map<std::string,std::vector<Creating>> g_rec_models;   // DFF name -> records that looked it up
FILE* capture() {
  if(!g_cap_init){g_cap_init=true;if(const char* p=getenv("WS_CAPTURE"))if(*p){g_cap=fopen(p,"wb");if(g_cap)logf("[workshop] capture -> %s\n",p);}}
  return g_cap;
}
std::string guest_str(uint32_t a,uint32_t max=64) {
  std::string s;if(a<0x80000000||a>=RT_RAM_END)return s;
  for(uint32_t i=0;i<max;++i){const uint8_t ch=ld8(a+i);if(!ch)break;s+=(ch>=0x20&&ch<0x7f&&ch!='"'&&ch!='\\')?char(ch):'?';}
  return s;
}
uint32_t class_id(uint32_t entry) {   // SetManager class descriptor (manager+0x14 table, entry+0x10 index) +0x18 = class id
  const uint32_t m=manager();if(!m)return 0xFFFF;const uint32_t tab=ld32(m+0x14),i=ld32(entry+0x10);
  if(tab<0x80000000||tab>=RT_RAM_END||i>4096)return 0xFFFF;const uint32_t d=ld32(tab+i*4);
  return d>=0x80000000&&d<RT_RAM_END?ld16(d+0x18):0xFFFF;
}
void emit_lookup(const std::string& name,const std::string& archive,const char* how,uint32_t slot,int row,uint32_t kind,bool found,uint32_t caller,uint32_t sp,const char* query=nullptr);
// RenderWare 3.7 layouts (no RW symbols in the project; checked against the game's RpClumpRender fn_80456BC4):
// RwObject {u8 type (1 atomic, 2 clump), subType, flags, privFlags; parent (frame)}; RpAtomic +0x18 geometry,
// +0x3C clump, +0x40 link in the clump's atomic list, +0x48 render callback; RpClump +4 frame, +8 atomic list head;
// RwFrame +0x50 LTM (right, up, at, pos; 4 words each); RpGeometry +0x10 triangles, +0x14 vertices, +0x18 morph
// targets, +0x20 material array, +0x24 count; RpMaterial +0 texture; RwTexture +0x10 name[32].
bool ram(uint32_t a){return a>=0x80000000&&a<RT_RAM_END;}
std::unordered_map<uint32_t,std::string> g_geo_name;   // geometry -> the DFF it came from (clones share geometry)
void register_model(uint32_t obj,const std::string& name) {
  if(!ram(obj)||g_geo_name.size()>200000)return;
  if(ld8(obj)==1){const uint32_t g=ld32(obj+0x18);if(ram(g))g_geo_name[g]=name;return;}
  if(ld8(obj)!=2)return;
  const uint32_t head=obj+8;uint32_t link=ld32(head);
  for(int i=0;i<512&&ram(link)&&link!=head;++i,link=ld32(link)){
    const uint32_t at=link-0x40;if(ld32(at+0x3C)!=obj)break;
    const uint32_t g=ld32(at+0x18);if(ram(g))g_geo_name[g]=name;
  }
}
// Which SET record is asking: the one whose create() runs, else the innermost running task-tree node that is a
// record's instance (entry+0xC at create). nullptr = none (stage / player / effects / class load).
const Creating* asker(const char*& during,uint32_t& node) {
  during="none";node=g_nodes.empty()?0:g_nodes.back();
  if(!g_creating.empty()){during="create";return &g_creating.back();}
  for(size_t i=g_nodes.size();i-->0;){auto it=g_instances.find(g_nodes[i]);if(it!=g_instances.end()){during="update";node=g_nodes[i];return &it->second;}}
  return nullptr;
}
void capture_lookup(const char* how,uint32_t slot,uint32_t self,uint32_t arg,uint32_t result,uint32_t caller,uint32_t sp) {
  if(!capture())return;
  const uint32_t rows=ld32(self+0x14),n=ld32(self+0x18);
  std::string name;uint32_t kind=0;int row=-1;
  if(how[0]=='n'){name=guest_str(arg);
    if(rows>=0x80000000&&n<100000)for(uint32_t i=0;i<n;++i)if(guest_str(rows+i*0x3C,0x2C)==name){row=int(i);break;}}
  else if(rows>=0x80000000&&arg>=2&&arg-2<n){row=int(arg-2);name=guest_str(rows+row*0x3C,0x2C);}
  if(row>=0)kind=ld32(rows+row*0x3C+0x2C);
  if(result&&result!=0xFFFFFFFFu&&how[0]=='n'&&g_res_name.size()<100000)g_res_name[result]=name;   // e.g. the ARTISTGADGET.BIN table
  if(result&&result!=0xFFFFFFFFu&&name.size()>4&&name.compare(name.size()-4,4,".DFF")==0)register_model(result,name);
  const char* res=ld32(self)==0x8051C6BC?"one":ld32(self)==0x8051FA3C?"req":"res";   // which resource class answered
  char by[24];snprintf(by,sizeof by,"%s-%s",res,how);
  emit_lookup(name,guest_str(ld32(self+0x10),96),by,slot,row,kind,result&&result!=0xFFFFFFFFu,caller,sp);
}
// The raw one-file reader (TOneFile*, object starts with its path; +0x34 -> data, entry i at data+0x34+i*0x38, i >= 2)
// resolves names itself: fn_8004C910(file, name) -> index (upper-cased strcmp), fn_8004C850(file, part, start) -> index.
void capture_raw(const char* how,uint32_t file,uint32_t query,int32_t index,uint32_t caller,uint32_t sp) {
  if(!capture())return;
  const uint32_t data=ld32(file+0x34);std::string name;
  if(index>=2&&data>=0x80000000&&data<RT_RAM_END)name=guest_str(data+0x34+uint32_t(index)*0x38,0x2C);
  const std::string q=guest_str(query);
  emit_lookup(name.empty()?q:name,guest_str(file,96),how,0,index>=2?index-2:-1,0,index>=2,caller,sp,q.c_str());
}
void emit_lookup(const std::string& name,const std::string& archive,const char* how,uint32_t slot,int row,uint32_t kind,bool found,uint32_t caller,uint32_t sp,const char* query) {
  FILE* f=g_cap;
  const char* during;uint32_t node;const Creating* who=asker(during,node);
  char head[200],key[48];
  snprintf(key,sizeof key,"|%08X|%d|%u",caller,who?who->layer:-9,who?who->index:0);
  if(g_cap_seen.size()>200000||!g_cap_seen.emplace(archive+"|"+name+key,1).second)return;
  if(who&&name.size()>4&&name.compare(name.size()-4,4,".DFF")==0&&g_rec_models.size()<100000)g_rec_models[name].push_back(*who);
  if(who)snprintf(head,sizeof head,"{\"ev\":\"lookup\",\"layer\":%d,\"index\":%u,\"cls\":\"%04X\",\"during\":\"%s\"",who->layer,who->index,who->cls,during);
  else snprintf(head,sizeof head,"{\"ev\":\"lookup\",\"layer\":null,\"index\":null,\"cls\":null,\"during\":\"none\"");
  char stack[128];int sn=snprintf(stack,sizeof stack,"\"%08X\"",caller);
  for(int i=0;i<6&&sp>=0x80000000u&&sp<RT_RAM_END;++i){const uint32_t back=ld32(sp),lr=ld32(sp+4);if(i)sn+=snprintf(stack+sn,sizeof stack-sn,",\"%08X\"",lr);if(back<=sp)break;sp=back;}
  std::string extra;if(query&&name!=query)extra=std::string(",\"query\":\"")+query+"\"";
  fprintf(f,"%s,\"name\":\"%s\"%s,\"archive\":\"%s\",\"by\":\"%s\",\"slot\":\"%02X\",\"row\":%d,\"kind\":%u,\"found\":%s,\"node\":\"%08X\",\"node_vt\":\"%08X\",\"stack\":[%s],\"t\":%.3f}\n",
    head,name.c_str(),extra.c_str(),archive.c_str(),how,slot,row,kind,found?"true":"false",node,node?ld32(node):0,stack,double(ldf32(0x8057D734)));
  fflush(f);
}
}
// Free MEM1 at "player active" (--stage runs; memory budget for foreign content). The game allocates from
// Sonicteam::System::Heap objects (RTTI, vtable 80569E6C): +4 -> free-list head, nodes {+0 next, +4 size}; its own
// "total free" (vtable fn_8040AC40) and "largest free block" (fn_8040AD38) just walk that list, as done here.
bool os_heap0(uint64_t& free,uint32_t& largest,uint32_t& size) {   // Dolphin OSAlloc heap 0 (see memory_report)
  const uint32_t arr=ld32(0x805F0EB8);free=0;largest=0;size=0;
  if(arr<0x80000000||arr>=RT_RAM_END||!ld32(0x805F0EBC))return false;
  size=ld32(arr);uint32_t n=0;
  for(uint32_t p=ld32(arr+4);p;p=ld32(p+4)){if(p<0x80000000||p>=RT_RAM_END||++n>500000)return false;const uint32_t s=ld32(p+8);free+=s;largest=std::max(largest,s);}
  return true;
}
void memory_report(int stage) {
  logf("[mem] stage %d OS arena lo %08X hi %08X\n",stage,ld32(0x805EDB38),ld32(0x805F0EC8));
  // Dolphin OSAlloc (OSInitAlloc fn_803712F0): HeapArray *0x805F0EB8, NumHeaps *0x805F0EBC, current heap *0x805EDB30;
  // HeapDesc {size, free cells, allocated cells}, Cell {prev, next, size (header included)}.
  const uint32_t arr=ld32(0x805F0EB8),num=ld32(0x805F0EBC);
  for(uint32_t i=0;arr>=0x80000000&&arr<RT_RAM_END&&i<num&&i<16;++i) {
    const uint32_t hd=arr+i*12;const int32_t size=int32_t(ld32(hd));if(size<0)continue;
    auto walk=[](uint32_t p,uint64_t& total,uint32_t& largest,uint32_t& cells){
      for(;p;p=ld32(p+4)){if(p<0x80000000||p>=RT_RAM_END||++cells>500000)return false;const uint32_t s=ld32(p+8);total+=s;largest=std::max(largest,s);}return true;};
    uint64_t ft=0,at=0;uint32_t fl=0,al=0,fc=0,ac=0;
    const bool ok=walk(ld32(hd+4),ft,fl,fc)&walk(ld32(hd+8),at,al,ac);
    logf("[mem] stage %d OS heap %u%s size %d free %llu largest %u free_cells %u used %llu used_cells %u%s\n",stage,i,
      int32_t(ld32(0x805EDB30))==int32_t(i)?" (current)":"",size,(unsigned long long)ft,fl,fc,(unsigned long long)at,ac,ok?"":" (list walk stopped)");
  }
  int n=0;
  for(uint32_t a=0x80000000;a<0x80000000u+g_mem1_size&&n<64;a+=4) {
    if(ld32(a)!=0x80569E6C)continue;
    const uint32_t head=ld32(a+4);if(head<0x80000000||head>=RT_RAM_END)continue;
    uint64_t total=0;uint32_t largest=0,blocks=0,lo=0xFFFFFFFF,hi=0;bool ok=true;
    for(uint32_t p=ld32(head);p;p=ld32(p)) {
      if(p<0x80000000||p>=RT_RAM_END||++blocks>200000){ok=false;break;}
      const uint32_t s=ld32(p+4);total+=s;largest=std::max(largest,s);lo=std::min(lo,p);hi=std::max(hi,p+s);
    }
    if(!ok)continue;++n;
    logf("[mem] stage %d heap %08X free %llu largest %u blocks %u span %08X-%08X words %08X %08X %08X %08X\n",stage,a,
      (unsigned long long)total,largest,blocks,blocks?lo:0,blocks?hi:0,ld32(a+8),ld32(a+12),ld32(a+16),ld32(a+20));
  }
  if(!n)logf("[mem] stage %d: no Sonicteam::System::Heap found\n",stage);
  // WS_FSTCHECK=dir/name: look the path up in the file table the GAME has in RAM (*0x80000038, what DVDOpen walks) and
  // read the first bytes through the disc layer (proof for NEW mod files, runtime/disc.cpp)
  if(const char* want=getenv("WS_FSTCHECK")) {
    const uint32_t fst=ld32(0x80000038),n=ld32(fst+8),names=fst+n*12;std::string w=want;
    for(auto& ch:w)ch=ch=='\\'?'/':(char)tolower((unsigned char)ch);
    std::vector<std::string> stack;std::vector<uint32_t> ends;uint32_t hit=0;
    for(uint32_t i=1;i<n&&i<100000&&!hit;++i) {
      while(!ends.empty()&&i>=ends.back()){ends.pop_back();stack.pop_back();}
      const uint32_t e=fst+i*12;std::string nm;
      for(uint32_t a=names+(ld32(e)&0xFFFFFF);ld8(a)&&nm.size()<256;++a)nm+=(char)tolower(ld8(a));
      std::string path;for(auto& d:stack)path+=d+"/";path+=nm;
      if(ld8(e)){stack.push_back(nm);ends.push_back(ld32(e+8));}else if(path==w)hit=i;
    }
    if(!hit){logf("[fstcheck] %s not in the game's file table (%u entries)\n",want,n);return;}
    uint8_t b[8]={};const uint32_t e=fst+hit*12;disc_read(ld32(e+4),b,8);
    logf("[fstcheck] %s entry %u of %u offset %08X size %u first bytes %02X%02X%02X%02X%02X%02X%02X%02X\n",want,hit,n,ld32(e+4),ld32(e+8),
      b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
  }
}
void node_enter(uint32_t obj){if(g_cap)g_nodes.push_back(obj);boss_slots::enter(obj);}   // boss_slots: which boss runs (#104)
void node_leave(){if(g_cap&&!g_nodes.empty())g_nodes.pop_back();boss_slots::leave();}
}
void f_800CADBC(Cpu&);   // SetManager: load one layer file (r3 manager, r4 path)
void f_800C9B04(Cpu&);   // kill (stays dead; r3 entry)
void f_800C9AD4(Cpu&);   // remove, respawns when the player comes back in range
void f_800C9B34(Cpu&);   // remove (collected / used / destroyed)
void f_800C9BE8(Cpu&);   // detach instance (destructors; out-of-range despawn)
using namespace stage_workshop;
void hle_SetLoadFile(Cpu& c) {
  const uint32_t m=c.r[3];char path[96]={};
  for(int i=0;i<95;++i){const uint8_t ch=ld8(c.r[4]+i);if(!ch)break;path[i]=char(ch);}
  const uint32_t before=ld32(m);
  f_800CADBC(c);
  const uint32_t after=ld32(m);
  std::string lower(path);for(auto& ch:lower)ch=char(tolower(ch));
  const int layer=lower.find("_cmn")!=std::string::npos?0:lower.find("_nrm")!=std::string::npos?1:lower.find("_hrd")!=std::string::npos?2:lower.find("_ds1")!=std::string::npos?3:-1;
  if(layer==0||before==0)g_layers.clear();
  if(layer>=0&&after>=before)g_layers.push_back({layer,before,after});
  logf("[workshop] SET %s: records %u..%u (layer %d)\n",path,before,after,layer); object_distance::apply();   // new records get the object draw distance before the first activation scan
  boss::on_set_loaded();   // a placed boss gets its HP bar (boss.cpp)
  if(FILE* f=capture()){fprintf(f,"{\"ev\":\"set\",\"stage\":%d,\"file\":\"%s\",\"layer\":%d,\"first\":%u,\"end\":%u}\n",int(ld32(0x80584720)),path,layer,before,after);fflush(f);}
}
namespace stage_workshop { void watch_add(uint32_t entry,uint32_t inst,uint32_t cls); void track_add(uint32_t entry); }
void hle_SetCreate(Cpu& c,uint32_t target) {   // callhooks.txt 800CA564: class create(entry) of the activation scan
  const uint32_t entry=c.r[3];
  FILE* f=capture();
  if(f) {
    int layer=-1;uint32_t index=0;if(!locate(entry,layer,index)){layer=-1;index=0;}
    g_creating.push_back({layer,index,entry,class_id(entry),0,ld32(entry)});
  }
  call_addr(c,target);
  stage_workshop::track_add(entry);
  if(f) {
    const auto k=g_creating.back();g_creating.pop_back();
    const uint32_t rec=ld32(entry);
    if(ld32(entry+0xC))g_instances[ld32(entry+0xC)]=k;
    fprintf(f,"{\"ev\":\"create\",\"layer\":%d,\"index\":%u,\"cls\":\"%04X\",\"list\":%u,\"type\":%u,\"pos\":[%.2f,%.2f,%.2f],\"rot\":[%.4f,%.4f,%.4f],\"instance\":\"%08X\",\"lookups\":%u,\"t\":%.3f}\n",
      k.layer,k.index,k.cls,rec>=0x80000000&&rec<RT_RAM_END?ld8(rec+0x20):999,rec>=0x80000000&&rec<RT_RAM_END?ld8(rec+0x21):999,
      rec?double(ldf32(rec)):0.,rec?double(ldf32(rec+4)):0.,rec?double(ldf32(rec+8)):0.,rec?double(ldf32(rec+12)):0.,rec?double(ldf32(rec+16)):0.,rec?double(ldf32(rec+20)):0.,ld32(entry+0xC),k.lookups,double(ldf32(0x8057D734)));
    // 5c research (roadmap 3.4): where the new instance keeps the record's position / rotation. The first 3 creates
    // of each class: every offset in the instance's first 0x400 bytes holding the position floats (exact bits), and
    // the rotation as radians (float, 1e-4), as 16-bit angle units in a 32-bit word (deg * 65536 / 360) or as raw floats.
    static std::unordered_map<uint32_t,int> seen;
    const uint32_t inst=ld32(entry+0xC);
    if(ram(inst)&&ram(rec)&&seen[k.cls]++<3) {
      std::string pos,rad,bam,raw;char b[24];
      const uint32_t px=ld32(rec),py=ld32(rec+4),pz=ld32(rec+8);
      const float r[3]={float(ldf32(rec+12)),float(ldf32(rec+16)),float(ldf32(rec+20))};
      const bool rot=r[0]!=0||r[1]!=0||r[2]!=0;
      for(uint32_t o=0;o+12<=0x400;o+=4) {
        if(ld32(inst+o)==px&&ld32(inst+o+4)==py&&ld32(inst+o+8)==pz){snprintf(b,sizeof b,"%s%u",pos.empty()?"":",",o);pos+=b;}
        if(!rot)continue;
        bool mr=true,mb=true,mw=true;
        for(int i=0;i<3;++i){const float v=float(ldf32(inst+o+4*i));const int32_t w=int32_t(ld32(inst+o+4*i));
          mr&=std::fabs(v-r[i]*3.14159265f/180)<1e-4f;mb&=(w&0xFFFF)==(int32_t(std::lround(r[i]*65536./360))&0xFFFF);mw&=v==r[i];}
        if(mr){snprintf(b,sizeof b,"%s%u",rad.empty()?"":",",o);rad+=b;}
        if(mb){snprintf(b,sizeof b,"%s%u",bam.empty()?"":",",o);bam+=b;}
        if(mw){snprintf(b,sizeof b,"%s%u",raw.empty()?"":",",o);raw+=b;}
      }
      // nearby copies (x and z within 0.5, any y): in the instance, and one pointer level down (first 0x200 bytes)
      std::string nearby;
      const float fx=float(ldf32(rec)),fz=float(ldf32(rec+8));
      auto scan=[&](uint32_t base,uint32_t len,const char* via,uint32_t from){
        for(uint32_t o=0;o+12<=len;o+=4){const float x=float(ldf32(base+o)),y=float(ldf32(base+o+4)),z=float(ldf32(base+o+8));
          if(std::fabs(x-fx)<0.5f&&std::fabs(z-fz)<0.5f&&std::isfinite(y)&&nearby.size()<900){
            snprintf(b,sizeof b,"%s\"%s%X+%X\"",nearby.empty()?"":",",via,from,o);nearby+=b;snprintf(b,sizeof b,",%.1f",double(y));nearby+=b;}}};
      scan(inst,0x400,"",0);
      for(uint32_t w=0;w<0x100&&nearby.size()<900;w+=4){const uint32_t q=ld32(inst+w);if(ram(q)&&q!=inst&&q!=rec)scan(q,0x200,"*",w);}
      fprintf(f,"{\"ev\":\"layout\",\"cls\":\"%04X\",\"layer\":%d,\"index\":%u,\"vtable\":\"%08X\",\"pos_off\":[%s],\"rot_rad_off\":[%s],\"rot_bams_off\":[%s],\"rot_deg_off\":[%s],\"rot\":%s,\"near\":[%s],\"rec_bytes\":\"%08X%08X\"}\n",
        k.cls,k.layer,k.index,ld32(inst),pos.c_str(),rad.c_str(),bam.c_str(),raw.c_str(),rot?"true":"false",nearby.c_str(),ld32(rec+0x20),ld32(rec+0x24));
    }
    fflush(f);
  }
  watch_add(entry,ld32(entry+0xC),class_id(entry));
  if(const uint32_t inst=ld32(entry+0xC);ram(inst)&&ram(ld32(entry))) {   // where this instance keeps its position (Move)
    const uint32_t rec=ld32(entry);
    for(uint32_t o=0;o+12<=0x400;o+=4)if(ld32(inst+o)==ld32(rec)&&ld32(inst+o+4)==ld32(rec+4)&&ld32(inst+o+8)==ld32(rec+8)){g_pos_off[inst]=o;break;}
  }
  if(ld32(entry+0xC))object_event(entry,4);
}
// ResourceOneFile getters (vtable 8051C6BC; overrides.txt): wrap, then log what was resolved.
void f_80014980(Cpu&);void f_800148E0(Cpu&);void f_8001483C(Cpu&);void f_800147C4(Cpu&);void f_80014790(Cpu&);void f_80014718(Cpu&);
void f_800146C8(Cpu&);void f_80014640(Cpu&);void f_80014870(Cpu&);void f_8001461C(Cpu&);void f_800145A4(Cpu&);
static int g_getter_depth=0;   // a by-name getter that calls the by-index one logs once
#define WS_RES_HOOK(NAME,FN,HOW,SLOT) void NAME(Cpu& c) { \
  if(!capture()||g_getter_depth){FN(c);return;} const uint32_t self=c.r[3],arg=c.r[4],lr=c.lr,sp=c.r[1]; \
  ++g_getter_depth; FN(c); --g_getter_depth; capture_lookup(HOW,SLOT,self,arg,c.r[3],lr,sp); }
WS_RES_HOOK(hle_ResByIndex18,f_80014980,"index",0x18)   // object (clump / dict / other by row type)
WS_RES_HOOK(hle_ResByName1C,f_800148E0,"name",0x1C)
WS_RES_HOOK(hle_ResByIndex20,f_8001483C,"index",0x20)   // row type 1 -> +0x30
WS_RES_HOOK(hle_ResByName24,f_800147C4,"name",0x24)
WS_RES_HOOK(hle_ResByIndex28,f_80014790,"index",0x28)   // row type 2 -> +0x34
WS_RES_HOOK(hle_ResByName2C,f_80014718,"name",0x2C)
WS_RES_HOOK(hle_ResByIndex30,f_800146C8,"index",0x30)
WS_RES_HOOK(hle_ResByName34,f_80014640,"name",0x34)
WS_RES_HOOK(hle_ResIndexOf38,f_80014870,"name",0x38)    // name -> index (returns -1 when absent)
WS_RES_HOOK(hle_ResByIndex3C,f_8001461C,"index",0x3C)
WS_RES_HOOK(hle_ResByName40,f_800145A4,"name",0x40)
// ResourceOneFileReq (RTTI, vtable 8051FA3C): the stage's own data (gdt / ARTISTGADGET.BIN ...), ResourceManager slot 1,
// same row table (+0x14 rows of 0x3C, +0x18 count, index = row + 2).
void f_8006AFEC(Cpu&);void f_8006AF68(Cpu&);void f_8006AEC8(Cpu&);void f_8006AE50(Cpu&);void f_8006AE20(Cpu&);void f_8006ADA8(Cpu&);
void f_8006AEF8(Cpu&);void f_8006AD74(Cpu&);void f_8006ACF0(Cpu&);
WS_RES_HOOK(hle_ReqByIndex18,f_8006AFEC,"index",0x18)
WS_RES_HOOK(hle_ReqByName1C,f_8006AF68,"name",0x1C)
WS_RES_HOOK(hle_ReqByIndex20,f_8006AEC8,"index",0x20)
WS_RES_HOOK(hle_ReqByName24,f_8006AE50,"name",0x24)
WS_RES_HOOK(hle_ReqByIndex28,f_8006AE20,"index",0x28)
WS_RES_HOOK(hle_ReqByName2C,f_8006ADA8,"name",0x2C)
WS_RES_HOOK(hle_ReqIndexOf38,f_8006AEF8,"name",0x38)
WS_RES_HOOK(hle_ReqByIndex3C,f_8006AD74,"index",0x3C)
WS_RES_HOOK(hle_ReqByName40,f_8006ACF0,"name",0x40)
// ARTISTGADGET / ARTISTWORK type rows: fn_80208978(out, table, type) takes the LAST row of the parsed table
// (+0x28 rows of 0x48 bytes, +0x2C count) whose id (+4) == type. Row words that point at strings are logged as
// strings (row name, model names, broken model, crash effect), the rest as numbers. Callers: the ART GADGET / ART WORK
// ctors (fn_800E554C ...) and a few other classes.
// #35 step 2: every RenderWare atomic the game renders goes through atomic->renderCallBack(atomic) at three call
// sites (callhooks.txt): 80456C14 in RpClumpRender fn_80456BC4, 8045F418 in the world-sector atomic loop, 80433AE4.
// While a SET record's node runs in the task-tree walker (node stack, see asker) the draw belongs to that record.
// Logged once per (record, atomic): the atomic frame's world matrix (LTM), its clump frame's LTM, geometry counts,
// texture names and the DFF the geometry came from (register_model). Draws with no record: once per geometry.
namespace stage_workshop { namespace {
std::unordered_map<std::string,int> g_draw_seen;
std::unordered_map<uint32_t,int> g_drawn_atomics;   // atomics that went through a draw hook (their LTM is current)
uint32_t g_draw_lines=0,g_draw_none=0;
const Creating* draw_owner(const char*& how,uint32_t& node) {
  if(const Creating* w=asker(how,node)){if(!strcmp(how,"update"))how="node";return w;}
  // (a "node holds the instance pointer" fallback was tried and dropped: shared atomics such as the ring went to the
  // wrong record; match_owner by model name + position handles them)
  how="none";return nullptr;
}
// Gadgets are added to the RpWorld and drawn by the world-sector loop under the world's node, not their own. Such a
// draw is matched by name + place: among the records that looked up this DFF, the one nearest to the clump's position.
const Creating* match_owner(const std::string& model,uint32_t cframe,float& dist) {
  dist=-1;if(model.empty()||!ram(cframe))return nullptr;auto it=g_rec_models.find(model);if(it==g_rec_models.end())return nullptr;
  const float x=float(ldf32(cframe+0x80)),y=float(ldf32(cframe+0x84)),z=float(ldf32(cframe+0x88));
  const Creating* best=nullptr;float bd=1e30f;
  for(const auto& r:it->second){if(!ram(r.rec))continue;const float dx=float(ldf32(r.rec))-x,dy=float(ldf32(r.rec+4))-y,dz=float(ldf32(r.rec+8))-z;
    const float d=std::sqrt(dx*dx+dy*dy+dz*dz);if(d<bd){bd=d;best=&r;}}
  dist=bd;return best;
}
void put_ltm(std::string& o,uint32_t frame) {
  char b[64];o+='[';
  for(int r=0;r<4;++r)for(int k=0;k<3;++k){const double v=ram(frame)?double(ldf32(frame+0x50+r*16+k*4)):0.;
    if(std::isfinite(v))snprintf(b,sizeof b,"%s%.5g",(r||k)?",":"",v);else snprintf(b,sizeof b,"%snull",(r||k)?",":"");o+=b;}   // JSON has no nan
  o+=']';
}
void capture_draw(uint32_t at,uint32_t site) {
  if(!ram(at)||ld8(at)!=1||g_draw_lines>400000)return;
  const char* how;uint32_t node;const Creating* who=draw_owner(how,node);
  const uint32_t geo=ld32(at+0x18),clump=ld32(at+0x3C),frame=ld32(at+4),cframe=ram(clump)?ld32(clump+4):0;
  if(site!=0x8045F390&&g_drawn_atomics.size()<500000)g_drawn_atomics[at]=1;
  auto gn=g_geo_name.find(geo);const std::string model=gn==g_geo_name.end()?"":gn->second;
  float dist=-1;if(!who&&(who=match_owner(model,cframe,dist)))how="match";
  char key[64];
  if(who)snprintf(key,sizeof key,"%d|%u|%08X",who->layer,who->index,at);
  else{if(g_draw_none>30000)return;snprintf(key,sizeof key,"none|%08X",at);}
  if(!g_draw_seen.emplace(key,1).second)return;
  if(!who)++g_draw_none;++g_draw_lines;
  std::string o;char b[256];
  if(who)snprintf(b,sizeof b,"{\"ev\":\"draw\",\"layer\":%d,\"index\":%u,\"cls\":\"%04X\",\"during\":\"%s\"",who->layer,who->index,who->cls,how);
  else snprintf(b,sizeof b,"{\"ev\":\"draw\",\"layer\":null,\"index\":null,\"cls\":null,\"during\":\"none\"");
  o+=b;
  if(dist>=0){snprintf(b,sizeof b,",\"match_dist\":%.2f",double(dist));o+=b;}
  snprintf(b,sizeof b,",\"model\":\"%s\",\"atomic\":\"%08X\",\"geometry\":\"%08X\",\"clump\":\"%08X\"",model.c_str(),at,geo,clump);o+=b;
  if(ram(geo)){snprintf(b,sizeof b,",\"verts\":%u,\"tris\":%u,\"morphs\":%u",ld32(geo+0x14),ld32(geo+0x10),ld32(geo+0x18));o+=b;}
  o+=",\"textures\":[";
  if(ram(geo)){const uint32_t mats=ld32(geo+0x20),nm=ld32(geo+0x24);
    for(uint32_t i=0;ram(mats)&&i<nm&&i<32;++i){const uint32_t m=ld32(mats+i*4),t=ram(m)?ld32(m):0;
      o+=(i?",":"");o+='"';if(ram(t))o+=guest_str(t+0x10,32);o+='"';}}
  o+="],\"ltm\":";put_ltm(o,frame);
  o+=",\"clump_ltm\":";put_ltm(o,cframe);
  if(who&&ram(who->rec)){snprintf(b,sizeof b,",\"rec_pos\":[%.3f,%.3f,%.3f],\"rec_rot\":[%.4f,%.4f,%.4f]",double(ldf32(who->rec)),double(ldf32(who->rec+4)),
    double(ldf32(who->rec+8)),double(ldf32(who->rec+12)),double(ldf32(who->rec+16)),double(ldf32(who->rec+20)));o+=b;}
  snprintf(b,sizeof b,",\"node\":\"%08X\",\"node_vt\":\"%08X\",\"site\":\"%08X\",\"t\":%.3f}\n",node,ram(node)?ld32(node):0,site,double(ldf32(0x8057D734)));o+=b;
  fputs(o.c_str(),g_cap);fflush(g_cap);
}
} }
// callhooks.txt: the atomic->renderCallBack(atomic) call sites
void hle_AtomicRenderClump(Cpu& c,uint32_t target){if(g_cap)capture_draw(c.r[3],0x80456C14);if(!g_find.empty())find_atomic(c.r[3]);call_addr(c,target);}
void hle_AtomicRenderWorld(Cpu& c,uint32_t target){if(g_cap)capture_draw(c.r[3],0x8045F418);if(!g_find.empty())find_atomic(c.r[3]);call_addr(c,target);}
void hle_AtomicRenderOther(Cpu& c,uint32_t target){if(g_cap)capture_draw(c.r[3],0x80433AE4);if(!g_find.empty())find_atomic(c.r[3]);call_addr(c,target);}
// Appear Types tests (WS_STATELOG=1): every enemy AI state change (fn_8019F944(sm, new): sm = {owner, current, previous,
// locked}, enemy-ai-59.md) with the RTTI names of the old / new state (state vtable[0] = typeinfo, typeinfo[0] = name).
void f_8019F944(Cpu&);
static std::string rtti_name(uint32_t obj){
  if(obj<0x80000000u||obj>=RT_RAM_END)return "?";const uint32_t vt=ld32(obj);if(vt<0x80000000u||vt>=RT_RAM_END)return "?";
  const uint32_t ti=ld32(vt);if(ti<0x80000000u||ti>=RT_RAM_END)return "?";const uint32_t np=ld32(ti);if(np<0x80000000u||np>=RT_RAM_END)return "?";
  std::string n;for(uint32_t a=np;n.size()<96;++a){const uint8_t ch=ld8(a);if(!ch)break;n+=char(ch);}return n;
}
void hle_EnemyStateChange(Cpu& c){
  static const bool on=getenv("WS_STATELOG")!=nullptr;static int lines=0;
  if(on&&lines<4000&&ld8(c.r[3]+0xC)==0){++lines;
    logf("[state] owner %08X %s -> %s t=%.2f\n",ld32(c.r[3]),rtti_name(ld32(c.r[3]+4)).c_str(),rtti_name(c.r[4]).c_str(),double(ldf32(0x8057D734)));}
  extern int enemy_ai_state_filter(Cpu&);   // enemy_ai.cpp: #58 / #59 AI override menu (ignore / chase)
  if(enemy_ai_state_filter(c)==1)return;
  f_8019F944(c);
}
// #43 research (WS_HPLOG=1): the enemy HP bar update fn_801A102C asks its status object for max (vt[0xB0]) and current
// (vt[0xAC]) HP; log status object, getter and value whenever the value changes (per status object).
static void hp_hook(Cpu& c,uint32_t target,const char* what){
  static const bool on=getenv("WS_HPLOG")!=nullptr;const uint32_t x=c.r[3];call_addr(c,target);if(!on)return;
  static std::unordered_map<uint64_t,double> last;const double v=c.f[1];auto& l=last[(uint64_t(x)<<1)|(what[0]=='m')];
  if(l==v)return;l=v;logf("[hp] %s status %08X vt %08X getter %08X -> %g\n",what,x,ld32(x),target,v);
}
void hle_EnemyHpMax(Cpu& c,uint32_t target){hp_hook(c,target,"max");}
void hle_EnemyHpCur(Cpu& c,uint32_t target){hp_hook(c,target,"cur");}
// overrides.txt fn_8045F390: the world-sector render callback (RpWorld renders the sectors the camera sees). Its loop
// frustum-tests each atomic of the sector (ties at sector+0x38, atomic at tie+8); recording them all first also keeps
// the atomics the test culls (objects beside / behind the camera).
void f_8045F390(Cpu&);
void hle_SectorAtomics(Cpu& c) {
  if(g_cap){const uint32_t head=c.r[3]+0x38;uint32_t link=ram(c.r[3])?ld32(head):0;
    for(int i=0;i<4096&&ram(link)&&link!=head;++i,link=ld32(link))capture_draw(ld32(link+8),0x8045F390);}
  if(!g_find.empty()){const uint32_t head=c.r[3]+0x38;uint32_t link=ram(c.r[3])?ld32(head):0;   // a Move looking for its model
    for(int i=0;i<4096&&ram(link)&&link!=head;++i,link=ld32(link))find_atomic(ld32(link+8));}
  f_8045F390(c);
}
void f_80208978(Cpu&);
void hle_ArtistType(Cpu& c) {
  if(!capture()){f_80208978(c);return;}
  const uint32_t table=c.r[4],type=c.r[5],lr=c.lr;
  f_80208978(c);
  const uint32_t rows=ld32(table+0x28),n=ld32(table+0x2C);int row=-1;
  if(rows>=0x80000000&&rows<RT_RAM_END&&n<4096)for(uint32_t i=0;i<n;++i)if(ld32(rows+i*0x48+4)==type)row=int(i);
  const char* during;uint32_t node;const Creating* who=asker(during,node);
  auto it=g_res_name.find(table);const std::string tname=it==g_res_name.end()?"":it->second;
  char key[64];snprintf(key,sizeof key,"artist|%08X|%u|%d|%u",table,type,who?who->layer:-9,who?who->index:0);
  if(!g_cap_seen.emplace(key,1).second)return;
  std::string fields;
  for(uint32_t w=0;row>=0&&w<18;++w) {
    const uint32_t v=ld32(rows+uint32_t(row)*0x48+w*4);const std::string s=guest_str(v,48);
    char b[80];if(v>=0x80000000&&v<RT_RAM_END&&s.size()>=2)snprintf(b,sizeof b,"\"%s\"",s.c_str());else snprintf(b,sizeof b,"%u",v);
    fields+=(w?",":"")+std::string(b);
  }
  char head[200];
  if(who)snprintf(head,sizeof head,"{\"ev\":\"artist\",\"layer\":%d,\"index\":%u,\"cls\":\"%04X\",\"during\":\"%s\"",who->layer,who->index,who->cls,during);
  else snprintf(head,sizeof head,"{\"ev\":\"artist\",\"layer\":null,\"index\":null,\"cls\":null,\"during\":\"none\"");
  fprintf(g_cap,"%s,\"table\":\"%s\",\"type\":%u,\"row\":%d,\"fields\":[%s],\"caller\":\"%08X\",\"t\":%.3f}\n",
    head,tname.c_str(),type,row,fields.c_str(),lr,double(ldf32(0x8057D734)));
  fflush(g_cap);
}
#undef WS_RES_HOOK
void f_8004C910(Cpu&);void f_8004C850(Cpu&);
void hle_OneFileIndexOf(Cpu& c) {   // fn_8004C910(file, name) -> index or -1
  if(!capture()){f_8004C910(c);return;} const uint32_t file=c.r[3],q=c.r[4],lr=c.lr,sp=c.r[1];
  f_8004C910(c);capture_raw("raw-name",file,q,int32_t(c.r[3]),lr,sp);
}
void hle_OneFileFind(Cpu& c) {      // fn_8004C850(file, part, start) -> index or -1
  if(!capture()){f_8004C850(c);return;} const uint32_t file=c.r[3],q=c.r[4],lr=c.lr,sp=c.r[1];
  f_8004C850(c);capture_raw("raw-part",file,q,int32_t(c.r[3]),lr,sp);
}
void hle_SetKill(Cpu& c) { const int old=g_cause;g_cause=5;f_800C9B04(c);g_cause=old; }
void hle_SetRemoveRespawn(Cpu& c) { const int old=g_cause;g_cause=6;f_800C9AD4(c);g_cause=old; }
void hle_SetRemove(Cpu& c) {
  const uint32_t entry=c.r[3];
  if(ld32(entry+0xC))object_event(entry,g_cause?uint32_t(g_cause):6u);
  const bool old=g_in_remove;g_in_remove=true;f_800C9B34(c);g_in_remove=old;
}
void hle_SetDetach(Cpu& c) {
  const uint32_t entry=c.r[3];
  if(!g_in_remove&&ld32(entry+0xC))object_event(entry,7);
  f_800C9BE8(c);
}
namespace stage_workshop {
namespace {
// #35 step 2, objects that were created but never drawn (off camera / far away): from each live instance follow
// pointers two levels deep (instance words 0..0x1FF, then 0x100 bytes of each pointee) to RenderWare atomics /
// clumps whose geometry is a DFF this record looked up. The world matrix is composed from the frames' MODELLING
// matrices (frame+0x10) up the parent chain (RwObject parent at +4), so it does not depend on the frame having been
// rendered (the LTM at +0x50 is only refreshed when drawn).
void world_matrix(uint32_t frame,double m[12]) {
  double r[12]={1,0,0, 0,1,0, 0,0,1, 0,0,0};
  for(int d=0;d<16&&ram(frame);++d,frame=ld32(frame+4)) {
    double l[12];for(int i=0;i<4;++i)for(int k=0;k<3;++k)l[i*3+k]=ldf32(frame+0x10+i*16+k*4);
    double o[12];   // o = r * l  (row vectors: v' = v * M; position row last)
    for(int i=0;i<4;++i)for(int k=0;k<3;++k){double s=0;for(int j=0;j<3;++j)s+=r[i*3+j]*l[j*3+k];o[i*3+k]=s+(i==3?l[9+k]:0);}
    memcpy(r,o,sizeof r);
  }
  memcpy(m,r,sizeof r);
}
uint32_t scan_models() {
  std::unordered_map<std::string,std::vector<std::string>> names;   // "layer|index" -> DFFs it looked up
  for(const auto& [n,recs]:g_rec_models)for(const auto& r:recs)names[std::to_string(r.layer)+"|"+std::to_string(r.index)].push_back(n);
  uint32_t lines=0;
  struct Found { const Creating* k; uint32_t at; int depth; std::string model; };
  std::vector<Found> found;std::unordered_map<uint32_t,int> owners;   // an atomic under several records = shared
  for(const auto& [inst,k]:g_instances) {
    if(!ram(inst)||ld32(k.entry+0xC)!=inst)continue;   // gone (collected / destroyed / despawned)
    auto nm=names.find(std::to_string(k.layer)+"|"+std::to_string(k.index));if(nm==names.end())continue;
    std::unordered_map<uint32_t,int> atoms;
    auto take=[&](uint32_t p,int depth){
      if(!ram(p))return false;const uint8_t t=ld8(p);
      if(t==1&&ram(ld32(p+0x18))){atoms.emplace(p,depth);return true;}
      if(t==2){const uint32_t head=p+8;uint32_t link=ld32(head);bool any=false;
        for(int i=0;i<64&&ram(link)&&link!=head;++i,link=ld32(link)){const uint32_t at=link-0x40;if(ld32(at+0x3C)!=p)break;atoms.emplace(at,depth);any=true;}
        return any;}
      return false;};
    for(uint32_t w=0;w<0x80;++w){const uint32_t p=ld32(inst+w*4);if(!ram(p)||p==inst)continue;
      if(take(p,1))continue;for(uint32_t v=0;v<0x40;++v)take(ld32(p+v*4),2);}
    for(const auto& [at,depth]:atoms) {
      auto gn=g_geo_name.find(ld32(at+0x18));if(gn==g_geo_name.end())continue;
      if(std::find(nm->second.begin(),nm->second.end(),gn->second)==nm->second.end())continue;   // not this record's model
      found.push_back({&k,at,depth,gn->second});++owners[at];
    }
  }
  for(const auto& f:found) {
    const Creating& k=*f.k;const uint32_t at=f.at;const int depth=f.depth;
    double m[12];world_matrix(ld32(at+4),m);
    double l[12];const uint32_t fr=ld32(at+4);for(int i=0;i<4;++i)for(int j=0;j<3;++j)l[i*3+j]=ram(fr)?ldf32(fr+0x50+i*16+j*4):0;
    const bool drawn=g_drawn_atomics.count(at)>0,origin=std::fabs(m[9])+std::fabs(m[10])+std::fabs(m[11])<0.001;
    const double* best=drawn||origin?l:m;
    {
      const uint32_t geo=ld32(at+0x18);
      std::string o;char b[256];
      snprintf(b,sizeof b,"{\"ev\":\"model\",\"layer\":%d,\"index\":%u,\"cls\":\"%04X\",\"model\":\"%s\",\"atomic\":\"%08X\",\"depth\":%d,\"shared\":%s,\"drawn\":%s,\"verts\":%u,\"tris\":%u,\"textures\":[",
        k.layer,k.index,k.cls,f.model.c_str(),at,depth,owners[at]>1?"true":"false",drawn?"true":"false",ld32(geo+0x14),ld32(geo+0x10));o+=b;
      const uint32_t mats=ld32(geo+0x20),nmats=ld32(geo+0x24);
      for(uint32_t i=0;ram(mats)&&i<nmats&&i<32;++i){const uint32_t mt=ld32(mats+i*4),t=ram(mt)?ld32(mt):0;o+=(i?",":"");o+='"';if(ram(t))o+=guest_str(t+0x10,32);o+='"';}
      o+="],\"best\":[";for(int i=0;i<12;++i){if(std::isfinite(best[i]))snprintf(b,sizeof b,"%s%.5g",i?",":"",best[i]);else snprintf(b,sizeof b,"%snull",i?",":"");o+=b;}
      o+="],\"world\":[";for(int i=0;i<12;++i){if(std::isfinite(m[i]))snprintf(b,sizeof b,"%s%.5g",i?",":"",m[i]);else snprintf(b,sizeof b,"%snull",i?",":"");o+=b;}
      o+="],\"ltm\":[";for(int i=0;i<12;++i){if(std::isfinite(l[i]))snprintf(b,sizeof b,"%s%.5g",i?",":"",l[i]);else snprintf(b,sizeof b,"%snull",i?",":"");o+=b;}
      snprintf(b,sizeof b,"],\"visible\":%s,\"rec_pos\":[%.3f,%.3f,%.3f],\"rec_rot\":[%.4f,%.4f,%.4f]}\n",(ld8(at+2)&4)?"true":"false",
        double(ldf32(k.rec)),double(ldf32(k.rec+4)),double(ldf32(k.rec+8)),double(ldf32(k.rec+12)),double(ldf32(k.rec+16)),double(ldf32(k.rec+20)));o+=b;
      fputs(o.c_str(),g_cap);++lines;
    }
  }
  fflush(g_cap);return lines;
}
}
// WS_CAPTURE_SECONDS=N: at stage time N the capture gets the free heap at that moment (after everything activated
// so far: the near-peak number) and an end line; the harness stops the game after it.
void capture_end_check() {
  static const double secs=getenv("WS_CAPTURE_SECONDS")?atof(getenv("WS_CAPTURE_SECONDS")):0;
  static bool done=false;
  static uint64_t t0=0;
  if(!g_cap||done||secs<=0)return;
  if(!t0){if(!game::in_stage())return;t0=sys_time_ns();}   // counts from the first in-stage frame; later frames may be
                                                         // a fall, a cutscene or a respawn (in_stage false) - still count
  const double st=(sys_time_ns()-t0)/1e9;if(st<secs)return;   // wall seconds since the stage first ran
  done=true;uint64_t fr;uint32_t lg,sz;const bool ok=os_heap0(fr,lg,sz);
  fprintf(g_cap,"{\"ev\":\"mem\",\"stage\":%d,\"heap_size\":%u,\"free\":%llu,\"largest_free\":%u,\"ok\":%s,\"t\":%.3f}\n",g_stage,sz,(unsigned long long)fr,lg,ok?"true":"false",double(st));
  const uint32_t models=scan_models();
  fprintf(g_cap,"{\"ev\":\"end\",\"stage\":%d,\"records_created\":%zu,\"draw_lines\":%u,\"model_lines\":%u,\"t\":%.3f}\n",g_stage,g_instances.size(),g_draw_lines,models,double(st));
  fflush(g_cap);logf("[workshop] capture end at stage time %.1f\n",double(st));
}
// #43 research (debug): WS_WATCHCLS=008D logs, per frame, the words of up to 4 instances of that class that change
// (first 0x800 bytes); a word that keeps changing (timers, positions) is muted after 30 changes. HP = an integer that
// drops when the object is hit.
struct Watch { uint32_t entry,inst; std::vector<uint32_t> snap; std::vector<uint16_t> changes; uint32_t base=0; int mod=-1; int frames=0; };
std::vector<Watch> g_watch;
uint32_t watch_cls() { static const uint32_t c=getenv("WS_WATCHCLS")?uint32_t(strtoul(getenv("WS_WATCHCLS"),nullptr,16)):0xFFFFFFFF; return c; }
void watch_add(uint32_t entry,uint32_t inst,uint32_t cls) {
  if(cls!=watch_cls()||g_watch.size()>=4||!ram(inst))return;
  Watch w{entry,inst,std::vector<uint32_t>(0x400),std::vector<uint16_t>(0x400)};for(uint32_t i=0;i<0x400;++i)w.snap[i]=ld32(inst+i*4);
  w.base=inst;g_watch.push_back(std::move(w));logf("[watch] cls %04X instance %08X vt %08X\n",cls,inst,ld32(inst));
  // WS_WATCHMOD=1: also the enemy's module objects (enemy-ai-59.md: +0x4 control, +0x8 move, +0xC display, +0x10 status)
  if(getenv("WS_WATCHMOD"))for(uint32_t m=1;m<=4;++m){const uint32_t t=ld32(inst+m*4);if(t<0x80600000||!ram(t+0x400))continue;
    Watch s{entry,inst,std::vector<uint32_t>(0x100),std::vector<uint16_t>(0x100)};s.base=t;s.mod=int(m);for(uint32_t i=0;i<0x100;++i)s.snap[i]=ld32(t+i*4);
    logf("[watch] instance %08X module +%X -> %08X vt %08X\n",inst,m*4,t,ld32(t));g_watch.push_back(std::move(s));}
  // the status object fn_801A102C reads HP from: X = *(*(*(e+0x10))+4), current = X->vt[0xAC], max = X->vt[0xB0]
  if(getenv("WS_WATCHMOD")){const uint32_t a=ld32(inst+0x10),b=ram(a)?ld32(a):0,x=ram(b)?ld32(b+4):0;
    if(ram(x)&&ram(x+0x400)){const uint32_t vt=ld32(x);
      logf("[watch] instance %08X status object %08X vt %08X cur fn %08X max fn %08X\n",inst,x,vt,ram(vt)?ld32(vt+0xAC):0,ram(vt)?ld32(vt+0xB0):0);
      Watch s{entry,inst,std::vector<uint32_t>(0x100),std::vector<uint16_t>(0x100)};s.base=x;s.mod=9;for(uint32_t i=0;i<0x100;++i)s.snap[i]=ld32(x+i*4);g_watch.push_back(std::move(s));}}
}
void watch_step() {
  for(auto& w:g_watch) {
    if(ld32(w.entry+0xC)!=w.inst)continue;
    // WS_HPSET=<field>:<value> (#43 test): one health write on the watched enemy 30 frames after it was created
    if(w.mod<0&&++w.frames==30)if(const char* hs=getenv("WS_HPSET"))enemy_set_health(w.inst,uint32_t(atoi(hs)),strchr(hs,':')?float(atof(strchr(hs,':')+1)):0.f);
    for(uint32_t i=0;i<w.snap.size();++i){const uint32_t v=ld32(w.base+i*4);if(v==w.snap[i])continue;
      if(++w.changes[i]<=30){float fo,fn;memcpy(&fo,&w.snap[i],4);memcpy(&fn,&v,4);
        logf("[watch] %08X%s +%03X %08X -> %08X (int %d -> %d, float %g -> %g) t=%.3f\n",w.base,w.mod<0?"":" (module)",i*4,w.snap[i],v,int32_t(w.snap[i]),int32_t(v),double(fo),double(fn),g_time);}
      w.snap[i]=v;}
  }
}
// Appear Types tests (research/appear-types.md): WS_TRACKCLS=LLTT logs, 4 times a second, where each created instance
// of that class is (up to 3 instances): every offset of its first 0x400 bytes that held the record position at create
// (one of them follows the enemy), plus the record's own position (spawn point).
struct Track { uint32_t entry,inst,offs[4],n; };
std::vector<Track> g_track;
uint64_t g_track_last=0;
void track_add(uint32_t entry) {
  static const uint32_t cls=getenv("WS_TRACKCLS")?uint32_t(strtoul(getenv("WS_TRACKCLS"),nullptr,16)):0xFFFFFFFF;
  if(cls==0xFFFFFFFF||g_track.size()>=3||class_id(entry)!=cls)return;
  const uint32_t inst=ld32(entry+0xC),rec=ld32(entry);if(!ram(inst)||!ram(rec))return;
  Track t{entry,inst,{},0};
  // every enemy class measured keeps its live position at +0x40 (GUN / Bk soldier, robot, pawn, wing, bigfoot): log that
  t.offs[t.n++]=0x40;
  // WS_TRACKOFF=a,b,c (hex): more instance offsets to log (flyers keep the flight position elsewhere)
  if(const char* e=getenv("WS_TRACKOFF"))for(const char* q=e;*q&&t.n<4;){t.offs[t.n++]=uint32_t(strtoul(q,nullptr,16))&0x3FC;const char* c2=strchr(q,',');if(!c2)break;q=c2+1;}
  g_track.push_back(t);
  std::string offs;for(uint32_t i=0;i<t.n;++i)offs+=(i?",":"")+std::to_string(t.offs[i]);
  logf("[track] cls %04X instance %08X record (%.1f,%.1f,%.1f) rot y %.1f position copies at +[%s]\n",cls,inst,double(ldf32(rec)),double(ldf32(rec+4)),
    double(ldf32(rec+8)),double(ldf32(rec+16)),offs.c_str());
}
void track_step() {
  if(g_track.empty())return;const uint64_t now=sys_time_ns();if(now-g_track_last<250000000ull)return;g_track_last=now;
  for(auto& t:g_track){if(ld32(t.entry+0xC)!=t.inst){continue;}
    if(t.n==0){   // some classes fill their position after create: look again (within 1 unit of the record position)
      const uint32_t rec=ld32(t.entry);if(!ram(rec))continue;const V r=position(rec);
      for(uint32_t o=0;o+12<=0x400&&t.n<4;o+=4){const V v=position(t.inst+o);if(std::fabs(v.x-r.x)<1&&std::fabs(v.y-r.y)<1&&std::fabs(v.z-r.z)<1)t.offs[t.n++]=o;}
      if(t.n){std::string offs;for(uint32_t i=0;i<t.n;++i)offs+=(i?",":"")+std::to_string(t.offs[i]);logf("[track] %08X position copies at +[%s] (found later)\n",t.inst,offs.c_str());}
    }
    std::string s;char b[80];
    for(uint32_t i=0;i<t.n;++i){snprintf(b,sizeof b," +%X (%.1f,%.1f,%.1f)",t.offs[i],double(ldf32(t.inst+t.offs[i])),double(ldf32(t.inst+t.offs[i]+4)),double(ldf32(t.inst+t.offs[i]+8)));s+=b;}
    logf("[track] %08X t=%.2f%s\n",t.inst,double(ldf32(0x8057D734)),s.c_str());}
}
uint32_t vm_entry(uint32_t o) {   // SetManager entry of a v4 native object (kind 2: layer a, record b), or 0
  if(o>=g_vm.objs.size()||g_vm.objs[o].kind!=2)return 0;const uint32_t m=manager();if(!m)return 0;
  for(const auto& r:g_layers)if(r.layer==g_vm.objs[o].a&&uint32_t(g_vm.objs[o].b)<r.end-r.first)return ld32(m+0x18)+(r.first+uint32_t(g_vm.objs[o].b))*0x20;
  return 0;
}
// Instance layout (5c research, capture 'layout' lines over many classes): each class keeps an exact copy of the record
// position at its own offset (ring +0x70, dash panel +0x58, jump board +0x60, art gadget +0x90, light +0xB0, ...;
// rotation in radians right after it, +12), found per instance at create (g_pos_off); and almost every class holds at
// instance+0x30 a pointer to a block that starts with the position. Record: activation byte at +0x23 (link +0x22).
void place(uint32_t entry,V p) {
  const uint32_t rec=ld32(entry),inst=ld32(entry+0xC);if(!ram(rec))return;
  const V old=position(rec);put(rec,p);
  if(!ram(inst))return;
  auto near_old=[&](uint32_t a){return ram(a)&&std::fabs(float(ldf32(a))-old.x)<1&&std::fabs(float(ldf32(a+8))-old.z)<1;};
  auto it=g_pos_off.find(inst);if(it!=g_pos_off.end()&&near_old(inst+it->second))put(inst+it->second,p);
  const uint32_t blk=ld32(inst+0x30);if(near_old(blk))put(blk,{p.x,float(ldf32(blk+4))+(p.y-old.y),p.z});   // keeps its own y offset
  // the drawn model: RenderWare frames are only rebuilt when the object itself moves them, so shift the instance's
  // model frame trees (root frame near the old position) by the same delta: root modelling + every frame's LTM
  const V d{p.x-old.x,p.y-old.y,p.z-old.z};
  // every other position copy the object keeps (e.g. the weapon box rebuilds its frame each frame from *(inst+0x70)):
  // x and z within 0.5 of the old place, in the instance and one pointer level down, shifted by the same delta
  {
    std::vector<uint32_t> done;
    auto shift_copies=[&](uint32_t base,uint32_t len){
      for(uint32_t o=0;o+12<=len;o+=4){const uint32_t a=base+o;
        if(std::fabs(float(ldf32(a))-old.x)<0.5f&&std::fabs(float(ldf32(a+8))-old.z)<0.5f&&std::isfinite(float(ldf32(a+4)))&&
           std::find(done.begin(),done.end(),a)==done.end()){
          done.push_back(a);stf32(a,float(ldf32(a))+(p.x-old.x));stf32(a+4,float(ldf32(a+4))+(p.y-old.y));stf32(a+8,float(ldf32(a+8))+(p.z-old.z));}}};
    shift_copies(inst,0x400);
    for(uint32_t w=0;w<0x100;w+=4){const uint32_t q=ld32(inst+w);if(ram(q)&&q!=inst&&q!=rec&&q!=blk)shift_copies(q,0x200);}
  }
  auto& roots=g_roots[entry];
  auto take=[&](uint32_t a){
    if(!ram(a))return;const uint8_t t=ld8(a);uint32_t fr=0;
    if(t==1&&ram(ld32(a+0x18)))fr=ld32(a+4);else if(t==2)fr=ld32(a+4);else return;
    fr=frame_root(fr);if(!ram(fr))return;
    if(std::fabs(float(ldf32(fr+0x80))-old.x)<60&&std::fabs(float(ldf32(fr+0x88))-old.z)<60&&std::find(roots.begin(),roots.end(),fr)==roots.end())roots.push_back(fr);};
  if(roots.empty())for(uint32_t w=0;w<0x80;++w){const uint32_t q=ld32(inst+w*4);if(!ram(q)||q==inst)continue;take(q);for(uint32_t v=0;v<0x40;++v)take(ld32(q+v*4));}
  for(uint32_t r:roots)shift_tree(r,d);
  // not reachable from the instance (world atomics such as boxes): the draw hooks find atomics standing at the old
  // place and attach them (find_atomic), shifted to wherever the record is by then
  if(roots.empty()&&std::none_of(g_find.begin(),g_find.end(),[&](const Find& f){return f.entry==entry;}))g_find.push_back({entry,old,0});
}
void run_ops(Cpu& c) {
  if(!g_vm.ok){g_ops.clear();g_glides.clear();return;}
  for(auto& f:g_find)++f.frames;
  g_find.erase(std::remove_if(g_find.begin(),g_find.end(),[](const Find& f){return f.frames>120||!g_roots[f.entry].empty();}),g_find.end());
  for(size_t i=0;i<g_glides.size();) {   // Move over N seconds: linear in elapsed stage time
    auto& g=g_glides[i];const double k=g.secs>0?std::clamp((g_time-g.t0)/g.secs,0.,1.):1.;
    place(g.entry,{float(g.from.x+(g.to.x-g.from.x)*k),float(g.from.y+(g.to.y-g.from.y)*k),float(g.from.z+(g.to.z-g.from.z)*k)});
    if(k>=1)g_glides.erase(g_glides.begin()+i);else ++i;
  }
  std::vector<VmOp> ops;ops.swap(g_ops);
  for(const auto& op:ops) {
    if(op.type==21||op.type==22) {   // mission complete / fail: the game's own fn_8016B2A4 / fn_8016B270 on the route's mission object
      const uint32_t m=custom_stats_route_mission(int(op.obj));
      if(!m){logf("[workshop] vm mission %s: no mission object in this stage\n",op.type==21?"complete":"fail");continue;}
      const Cpu saved=c;c.r[3]=m;if(op.type==21)hle_MissionComplete(c);else hle_MissionFail(c);c=saved;
      logf("[workshop] vm mission %s (route %u, mission %08X)\n",op.type==21?"complete":"fail",op.obj,m);continue;
    }
    const uint32_t entry=vm_entry(op.obj);
    if(!entry){logf("[workshop] vm: object op %u on object %u: not a placed SET record\n",op.type,op.obj);continue;}
    const uint32_t rec=ld32(entry),inst=ld32(entry+0xC);
    if(op.type==17||op.type==18) {   // hide: moved 100000 below (record, instance, models) - show: back where it was
      static std::unordered_map<uint32_t,V> hidden;
      if(!ram(rec))continue;
      if(op.type==17&&!hidden.count(entry)){const V p=position(rec);hidden[entry]=p;place(entry,{p.x,p.y-100000.f,p.z});}
      else if(op.type==18&&hidden.count(entry)){place(entry,hidden[entry]);hidden.erase(entry);}
      logf("[workshop] vm %s layer%d #%d\n",op.type==17?"hide":"show",g_vm.objs[op.obj].a,g_vm.objs[op.obj].b);
      continue;
    }
    if(op.type==11) {   // destroy = the game's own kill (stays dead): fn_800C9B04(entry), events fire as for a kill
      if(!inst)continue;const Cpu saved=c;c.r[3]=entry;hle_SetKill(c);c=saved;
      logf("[workshop] vm destroy layer%d #%d\n",g_vm.objs[op.obj].a,g_vm.objs[op.obj].b);
    } else if(op.type==10) {   // spawn: move the (pool) record, then create(entry) through its class descriptor
      if(!ram(rec))continue;put(rec,op.to);
      if(inst){place(entry,op.to);continue;}
      const uint32_t m=manager(),tab=ld32(m+0x14),desc=ld32(tab+ld32(entry+0x10)*4),create=ram(desc)?ld32(desc+0xC):0;
      if(!create){logf("[workshop] vm spawn: no create() for this class\n");continue;}
      st32(rec+0x18,ld32(rec+0x18)&0xFFF7FFBFu);   // the flags the activation scan clears before create()
      st8(rec+0x23,255);   // activation distance: max, or the next scan despawns a pool record (activation 0) again
      const Cpu saved=c;c.r[3]=entry;hle_SetCreate(c,create);c=saved;
      logf("[workshop] vm spawn layer%d #%d at (%g,%g,%g) -> instance %08X\n",g_vm.objs[op.obj].a,g_vm.objs[op.obj].b,op.to.x,op.to.y,op.to.z,ld32(entry+0xC));
    } else if(op.type==12) {
      if(!ram(rec))continue;g_glides.erase(std::remove_if(g_glides.begin(),g_glides.end(),[&](const Glide& g){return g.entry==entry;}),g_glides.end());
      g_glides.push_back({entry,position(rec),op.to,g_time,op.secs});
      logf("[workshop] vm move layer%d #%d to (%g,%g,%g) in %.2f s\n",g_vm.objs[op.obj].a,g_vm.objs[op.obj].b,op.to.x,op.to.y,op.to.z,double(op.secs));
    }
  }
}
void live_frame();void live_player(uint32_t owner);
void frame_begin(Cpu& c) {
  capture_end_check();live_frame();
  run_ops(c);
  for(int k=0;k<3;++k) {   // HUD values (ACT 23): the variable's current value, every frame
    const auto& h=g_hud_vals[k];
    if(!h.on||h.var<0||size_t(h.var)>=g_vm.val.size()){settings::g_hud_values[k].clear();continue;}
    char b[128];snprintf(b,sizeof b,"%s: %g",h.label.c_str(),double(g_vm.val[size_t(h.var)]));settings::g_hud_values[k]=b;
  }
  if(!g_watch.empty())watch_step();
  track_step();
  ++g_frame;const int stage=int(ld32(0x80584720));if(stage!=g_stage)load(stage);
  if(!g_header.version&&!vm_on())return;
  const float elapsed=float(ldf32(0x8057D734));
  if(std::isfinite(elapsed)&&elapsed>=0) {
    if(g_last_stage_time>=0 && elapsed+.25f<g_last_stage_time)load(stage);
    g_last_stage_time=elapsed;
  }
  if(!game::in_stage()||!player(g_owner)||game::pause_menu_active()||game::cutscene_active())return;
  // Apply mutations before next behaviour/collision step, not halfway through native output-copy logic.
  unsigned budget=256;
  for(size_t i=0;i<g_pending.size()&&budget;) {
    if(g_pending[i].due>g_time){++i;continue;}
    const auto a=g_pending[i].action;g_pending.erase(g_pending.begin()+i);execute(a);--budget;
  }
}
uint32_t g_player=0;   // the player object the movement step last ran for (lighting L1 regions)
bool player_position(float out[3]){if(!player(g_player)||game::cutscene_active())return false;const V v=position(g_player);out[0]=v.x;out[1]=v.y;out[2]=v.z;return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
void before_player(uint32_t owner) {
  if(player(owner))g_player=owner;
  live_player(owner);
  if(!g_header.version||!player(owner))return;g_owner=owner;
  if(g_spawn_pending&&!game::cutscene_active()){teleport(owner,g_header.spawn);g_spawn_pending=false;}
}
void after_player(uint32_t owner,float dt) {
  live_player(owner);
  if((!g_header.version&&!vm_on())||g_step_frame==g_frame||!player(owner)||game::pause_menu_active()||game::cutscene_active()||!std::isfinite(dt)||dt<=0)return;
  g_step_frame=g_frame;g_owner=owner;g_time+=std::min(dt,.25f);
  const V pos=position(owner);if(!finite(pos))return;
  const bool interact=(g_user_buttons&PAD_A)&&!(g_buttons&PAD_A);g_buttons=g_user_buttons;
  for(size_t i=0;i<g_volumes.size();++i) {
    if(g_volumes[i].event>=4)continue;   // native object events fire from the SET manager hooks below
    const auto& v=g_volumes[i];auto& s=g_state[i];const bool inside=contains(v,pos);
    const bool fire=v.event==0?inside&&!s.inside:v.event==1?!inside&&s.inside:v.event==2?inside:inside&&interact;
    s.inside=inside;if(!s.enabled||!fire||((v.flags&1)&&s.fired)||g_time<s.next)continue;
    s.fired=true;s.next=g_time+std::max(v.cooldown,.05f);
    logf("[workshop] trigger %s\n",v.name);
    if(vm_on()) {   // v4: the volume is only a sensor; its event runs in the VM (pads: the v3 volumes after the triggers)
      if(i<g_vm.v3_triggers)vm_fire(v.event,1,int32_t(i));else vm_fire(11,3,int32_t(i-g_vm.v3_triggers));
      continue;
    }
    for(uint32_t j=0;j<v.count&&g_pending.size()<2048;++j){const auto& a=g_actions[v.first+j];g_pending.push_back({a,g_time+a.delay});}
  }
  vm_step();
}
}

// ---- Live game view (docs/stage-workshop/LIVE-VIEW.md) ----------------------------------------------------------------
// The editor (server.py /api/live-view/camera) writes <game folder>/live-view-camera.json atomically:
//   {seq, mode 'camera'|'release', position [x,y,z], lookAt [x,y,z] | yaw + pitch (deg; yaw 0 = +z, 90 = +x), fov (deg), t}
// Polled every 50 ms (file time only); every newer write wins. While a camera is held the screen camera's frame is replaced at
// RwCameraBeginUpdate (widescreen.cpp), smoothed toward the target each frame, and the player is carried along behind the
// camera (frozen, out of view) so chunks and objects around the view are active. Release puts the player back.
namespace stage_workshop { namespace {
struct LiveView {
  bool on=false,have=false,carry=true;uint32_t seq=0;float fov=0;
  V tpos{},tat{},pos{},at{};          // target / smoothed eye and look direction
  bool saved=false;V player{};uint32_t owner=0;
  FILETIME mtime{};uint64_t polled=0,last=0;std::string path;bool log=false;uint32_t logged=0;
} g_lv;
const char* jfind(const std::string& s,const char* key){const std::string k=std::string("\"")+key+"\"";size_t p=s.find(k);if(p==std::string::npos)return nullptr;p=s.find(':',p+k.size());return p==std::string::npos?nullptr:s.c_str()+p+1;}
bool jnum(const std::string& s,const char* key,float& out){const char* p=jfind(s,key);if(!p)return false;char* e;const double v=strtod(p,&e);if(e==p||!std::isfinite(v))return false;out=float(v);return true;}
bool jvec(const std::string& s,const char* key,V& out){const char* p=jfind(s,key);if(!p)return false;while(*p==' ')++p;if(*p!='[')return false;++p;float r[3];
  for(int i=0;i<3;++i){char* e;const double v=strtod(p,&e);if(e==p||!std::isfinite(v))return false;r[i]=float(v);p=e;while(*p==' '||*p==',')++p;}out={r[0],r[1],r[2]};return true;}
V vsub(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
V vcross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float vlen(V a){return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);}
V vnorm(V a){const float l=vlen(a);return l>1e-6f?V{a.x/l,a.y/l,a.z/l}:V{0,0,1};}
void live_poll() {
  const uint64_t now=sys_time_ns();if(now-g_lv.polled<50000000ull)return;g_lv.polled=now;
  if(g_lv.path.empty()){char exe[MAX_PATH];GetModuleFileNameA(nullptr,exe,MAX_PATH);g_lv.path=exe;g_lv.path=g_lv.path.substr(0,g_lv.path.find_last_of("\/")+1)+"live-view-camera.json";
    g_lv.log=getenv("LIVE_VIEW_LOG")!=nullptr;
    // a file left over from an earlier session is not a request: only files written after this process started count
    FILETIME ft;GetSystemTimeAsFileTime(&ft);g_lv.mtime=ft;}
  WIN32_FILE_ATTRIBUTE_DATA fa;if(!GetFileAttributesExA(g_lv.path.c_str(),GetFileExInfoStandard,&fa))return;
  if(CompareFileTime(&fa.ftLastWriteTime,&g_lv.mtime)<=0)return;
  std::ifstream f(g_lv.path,std::ios::binary);if(!f)return;   // being replaced: next poll
  const std::string s((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());g_lv.mtime=fa.ftLastWriteTime;
  float seq=0;jnum(s,"seq",seq);   // every newer write counts (the server's seq restarts with the server)
  const char* m=jfind(s,"mode");const bool release=m&&strstr(m,"release")&&(!strchr(m,',')||strstr(m,"release")<strchr(m,','));
  g_lv.seq=uint32_t(seq);g_lv.have=true;
  if(release){if(g_lv.on)logf("[liveview] seq %u release\n",g_lv.seq);g_lv.on=false;g_lv.fov=0;return;}
  V pos,look;if(!jvec(s,"position",pos)){logf("[liveview] seq %u: no position, ignored\n",g_lv.seq);return;}
  V dir;
  if(jvec(s,"lookAt",look)&&vlen(vsub(look,pos))>1e-3f)dir=vnorm(vsub(look,pos));
  else{float yaw=0,pitch=0;jnum(s,"yaw",yaw);jnum(s,"pitch",pitch);const float y=yaw*0.0174532925f,p=pitch*0.0174532925f;dir={std::sin(y)*std::cos(p),std::sin(p),std::cos(y)*std::cos(p)};}
  float fov=0;g_lv.fov=jnum(s,"fov",fov)&&fov>=10&&fov<=150?fov:0;
  const char* pl=jfind(s,"player");g_lv.carry=!(pl&&strstr(pl,"stay")&&(!strchr(pl,',')||strstr(pl,"stay")<strchr(pl,',')));
  if(!g_lv.on){g_lv.pos=pos;g_lv.at=dir;}   // first pose: no smoothing from the game camera
  g_lv.tpos=pos;g_lv.tat=dir;
  if(!g_lv.on)logf("[liveview] seq %u camera (%.1f,%.1f,%.1f) dir (%.3f,%.3f,%.3f) fov %.0f\n",g_lv.seq,double(pos.x),double(pos.y),double(pos.z),double(dir.x),double(dir.y),double(dir.z),double(g_lv.fov));
  g_lv.on=true;
}
void live_hold_player(uint32_t owner) {
  if(!player(owner))return;g_lv.owner=owner;
  if(g_lv.on&&g_lv.carry&&game::in_stage()&&!game::cutscene_active()) {
    if(!g_lv.saved){g_lv.player=position(owner);g_lv.saved=true;logf("[liveview] player held (was %.1f,%.1f,%.1f)\n",double(g_lv.player.x),double(g_lv.player.y),double(g_lv.player.z));}
    const V p={g_lv.pos.x-g_lv.at.x*60,g_lv.pos.y-g_lv.at.y*60,g_lv.pos.z-g_lv.at.z*60};   // behind the camera: out of view
    put(owner,p);put(owner+0xD0,p);put(owner+0x18,{0,0,0});put(owner+0x24,{0,0,0});
  } else if(g_lv.saved&&!g_lv.on) {
    put(owner,g_lv.player);put(owner+0xD0,g_lv.player);put(owner+0x18,{0,0,0});put(owner+0x24,{0,0,0});g_lv.saved=false;
    logf("[liveview] player back at (%.1f,%.1f,%.1f)\n",double(g_lv.player.x),double(g_lv.player.y),double(g_lv.player.z));
  }
}
}  // namespace

float live_fov(){return g_lv.on&&!game::cutscene_active()?g_lv.fov:0.f;}
// widescreen.cpp hle_RwCameraBeginUpdate, screen cameras only, BEFORE the original: replace the camera frame's matrix
// (modelling + LTM) so the frame sync, the frustum and the lighting engine's camera all see the live view.
void live_camera(uint32_t cam,uint32_t frm) {
  if(!g_lv.on||game::cutscene_active()||!ram(frm))return;
  // smoothing: ~80 ms time constant (the editor streams ~10 Hz)
  const uint64_t now=sys_time_ns();const float dt=g_lv.last?std::min(float(now-g_lv.last)*1e-9f,.1f):0.f;g_lv.last=now;
  const float a=dt>0?1.f-std::exp(-dt/.08f):1.f;
  g_lv.pos={g_lv.pos.x+(g_lv.tpos.x-g_lv.pos.x)*a,g_lv.pos.y+(g_lv.tpos.y-g_lv.pos.y)*a,g_lv.pos.z+(g_lv.tpos.z-g_lv.pos.z)*a};
  g_lv.at=vnorm({g_lv.at.x+(g_lv.tat.x-g_lv.at.x)*a,g_lv.at.y+(g_lv.tat.y-g_lv.at.y)*a,g_lv.at.z+(g_lv.tat.z-g_lv.at.z)*a});
  // handedness from the game's own camera matrix (right vs up x at), so the image is never mirrored
  auto rd=[&](uint32_t o){return V{float(ldf32(frm+o)),float(ldf32(frm+o+4)),float(ldf32(frm+o+8))};};
  const V gr=rd(0x50),gu=rd(0x60),ga=rd(0x70);const V c=vcross(gu,ga);const float hand=gr.x*c.x+gr.y*c.y+gr.z*c.z<0?-1.f:1.f;
  const V at=g_lv.at;V r0=vcross({0,1,0},at);if(vlen(r0)<1e-4f)r0={1,0,0};r0=vnorm(r0);const V up=vcross(at,r0);
  const V right={r0.x*hand,r0.y*hand,r0.z*hand};
  for(uint32_t base:{0x10u,0x50u}){put(frm+base,right);put(frm+base+0x10,up);put(frm+base+0x20,at);put(frm+base+0x30,g_lv.pos);}
  if(g_lv.log&&g_lv.logged<100000){++g_lv.logged;logf("[liveview] frame cam %08X frm %08X flags %02X parent %08X pos (%.2f,%.2f,%.2f) at (%.3f,%.3f,%.3f) hand %.0f\n",
    cam,frm,ld8(frm+3),ld32(frm+4),double(g_lv.pos.x),double(g_lv.pos.y),double(g_lv.pos.z),double(at.x),double(at.y),double(at.z),double(hand));}
}
// after RwCameraBeginUpdate: what the frame actually holds (proof that nothing overwrote the live pose)
void live_check(uint32_t frm) {
  if(!g_lv.on||!g_lv.log||!ram(frm))return;
  logf("[liveview] rendered pos (%.2f,%.2f,%.2f) at (%.3f,%.3f,%.3f)\n",double(ldf32(frm+0x80)),double(ldf32(frm+0x84)),double(ldf32(frm+0x88)),
    double(ldf32(frm+0x70)),double(ldf32(frm+0x74)),double(ldf32(frm+0x78)));
}
void live_frame(){live_poll();}
void live_player(uint32_t owner){live_hold_player(owner);}
}
