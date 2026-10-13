#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Root's only async-origin-interface-v1.json. No P0 schema changes or alternate canonical.
#include <nlohmann/json.hpp>
#include <vector>
#include <stdexcept>
#include <set>
#include <string>
#include <cmath>
#include <algorithm>
#include "native_pre_sr_spec.h"
#include "native_pre_sr_hot120_policy.h"
#include <cstdio>
namespace Vulkan::NativePreSrWire {
using J=nlohmann::json;
inline void need(bool v,const char*m){if(!v)throw std::invalid_argument(m);}
inline J parse(const std::string&t){need(!t.empty()&&t.size()<=65536,"control64KiB bound");std::vector<std::set<std::string>>s;auto cb=[&](int,J::parse_event_t e,J&v){if(e==J::parse_event_t::object_start)s.emplace_back();else if(e==J::parse_event_t::key)need(!s.empty()&&s.back().insert(v.get<std::string>()).second,"duplicate key");else if(e==J::parse_event_t::object_end)s.pop_back();if(e==J::parse_event_t::value&&v.is_number_float())need(std::isfinite(v.get<double>()),"nonfinite");return true;};auto j=J::parse(t,cb);need(j.is_object(),"JSON object");return j;}
inline const J&spec(){static J j=parse(NativePreSrSpecJson);return j;}
inline uint64_t u(const J&j,const char*k){need(j.contains(k)&&j.at(k).is_number_unsigned(),k);auto v=j.at(k).get<uint64_t>();need(v<=9007199254740991ull,"uint53 bound");return v;}
inline bool b(const J&j,const char*k){need(j.contains(k)&&j.at(k).is_boolean(),k);return j.at(k).get<bool>();}
inline std::string s(const J&j,const char*k){need(j.contains(k)&&j.at(k).is_string(),k);return j.at(k).get<std::string>();}
inline void exact(const J&j,const std::set<std::string>&keys){need(j.is_object()&&j.size()==keys.size(),"exact stage keys");for(auto&k:keys)need(j.contains(k),"missing stage key");}
inline std::set<std::string>keys(const J&j){std::set<std::string>k;for(auto i=j.begin();i!=j.end();++i)k.insert(i.key());return k;}
inline bool same_typed(const J&j,const J&e){if(j.is_object()&&e.is_object()){if(j.size()!=e.size())return false;for(auto i=e.begin();i!=e.end();++i)if(!j.contains(i.key())||!same_typed(j.at(i.key()),i.value()))return false;return true;}if(j.is_array()&&e.is_array()){if(j.size()!=e.size())return false;for(size_t i=0;i<e.size();i++)if(!same_typed(j[i],e[i]))return false;return true;}if(e.is_boolean())return j.is_boolean()&&j==e;if(e.is_number_unsigned())return j.is_number_unsigned()&&j==e;if(e.is_number())return j.is_number()&&!j.is_boolean()&&std::isfinite(j.get<double>())&&j==e;return j.type()==e.type()&&j==e;}
inline bool hex32(const std::string&s){return s.size()==32&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
inline J config_expected(const std::string&session,const std::string&mode,const std::string&device,const std::string&driver){auto e=spec()["bootstrap_exact"]["async-config.json"]["exact_sample"];e["session"]=session;e["mode"]=mode;e["resource_set"]=session+"-g0";e["device_uuid"]=device;e["driver_uuid"]=driver;return e;}
inline void config(const J&j,const std::string&session,const std::string&mode){need(mode=="resident"||mode=="skipEvaluate","mode");auto device=s(j,"device_uuid"),driver=s(j,"driver_uuid");need(hex32(device)&&hex32(driver),"actual UUID syntax");need(same_typed(j,config_expected(session,mode,device,driver)),"exact config types/literals");}
inline J common(const std::string&session,const std::string&mode){return J{{"protocol_version",1u},{"transport_layout",spec()["transport_layout"]},{"execution_mode","hot"},{"mode",mode},{"session",session},{"generation",0u},{"resource_set",session+"-g0"},{"profile_id",spec()["profile_id"]},{"quality_profile",spec()["quality_profile"]},{"params_version",1u},{"parameter_application",spec()["parameter_application"]}};}
inline void common_check(const J&j,const J&c){for(auto i=c.begin();i!=c.end();++i)need(j.contains(i.key())&&same_typed(j.at(i.key()),i.value()),"common exact identity/profile");}
inline std::set<std::string>stage_keys(const J&common,const J&fields){auto k=keys(common);for(auto i=fields.begin();i!=fields.end();++i)k.insert(i.key());return k;}
inline void description_types(const J&j,const J&fields){for(auto i=fields.begin();i!=fields.end();++i){auto d=i.value().get<std::string>();const auto&v=j.at(i.key());if(d.rfind("uint",0)==0)u(j,i.key().c_str());else if(d.rfind("bool",0)==0)b(j,i.key().c_str());else if(d.rfind("string",0)==0)s(j,i.key().c_str());else if(d.rfind("array",0)==0)need(v.is_array(),"array field");else if(d.rfind("object",0)==0)need(v.is_object(),"object field");else if(d.rfind("number",0)==0)need(v.is_number()&&!v.is_boolean()&&std::isfinite(v.get<double>()),"finite number field");}}
inline void bootstrap(const J&j,const J&c,const char*stage){auto f=spec()["bootstrap_exact"][stage]["required_stage_fields"];exact(j,stage_keys(c,f));common_check(j,c);description_types(j,f);}
inline void jitter(const J&j){exact(j,keys(spec()["source_jitter_metadata_exact"]));for(auto i=j.begin();i!=j.end();++i)need(i.value().is_number()&&!i.value().is_boolean()&&std::isfinite(i.value().get<double>()),"jitter scalar finite actual");}
inline J job_fields(const J&j){J e;for(const char*k:{"job","origin_source","origin_epoch","origin_history"}){auto v=u(j,k);need(k==std::string("job")?v<BbPreSr::Hot120::MaxJobs:v>0,"job origin identity");e[k]=v;}return e;}
inline void input(const J&j,const J&c,uint64_t expected){auto k=keys(c);for(const char*p:{"job","origin_source","origin_epoch","origin_history","op","status","native_input_tick","native_input_GPU_complete","source_reset","source_jitter_metadata","model_reset"})k.insert(p);exact(j,k);common_check(j,c);job_fields(j);need(u(j,"job")==expected&&s(j,"op")=="input"&&s(j,"status")=="ORIGIN_INPUT_READY"&&u(j,"native_input_tick")>0&&b(j,"native_input_GPU_complete")&&u(j,"model_reset")==1,"exact origin input actual GPU release");b(j,"source_reset");jitter(j.at("source_jitter_metadata"));}
struct Counts {uint64_t finalized=0,captured=0,obsolete=0,skip=0;J last{{"origin_source",0u},{"origin_epoch",0u},{"origin_history",1u}};J fields()const{J j=last;j.update(J{{"finalized_jobs",finalized},{"captured_jobs",captured},{"obsolete_jobs",obsolete},{"skip_jobs",skip},{"job",finalized},{"next_job",finalized},{"source_frame_valid",finalized>0}});return j;}
 void check(const J&j)const{auto f=fields();for(auto i=f.begin();i!=f.end();++i)need(j.contains(i.key())&&same_typed(j.at(i.key()),i.value()),"terminal actual N/counts/origin");need(finalized==captured+obsolete+skip&&finalized<=BbPreSr::Hot120::MaxJobs,"count invariant");}
 void finalize(const J&input,const std::string&disposition){need(u(input,"job")==finalized&&finalized<BbPreSr::Hot120::MaxJobs,"one exact finalized job");if(disposition=="captured")++captured;else if(disposition=="obsolete_discard")++obsolete;else if(disposition=="expected_skip")++skip;else need(false,"disposition");++finalized;for(const char*k:{"origin_source","origin_epoch","origin_history"})last[k]=input.at(k);}
};
inline void terminal(const J&j,const J&c,const Counts&counts,const char*stage){auto f=spec()["terminal_exact"][stage];auto k=stage_keys(c,f);for(auto&key:keys(counts.fields()))k.insert(key);exact(j,k);common_check(j,c);counts.check(j);description_types(j,f);}
inline void native_last_use(const J&j){const auto proof=s(j,"native_last_use_proof");const auto t=u(j,"native_last_use_tick"),o=u(j,"native_last_use_observed_tick");if(proof=="ACTUAL_NATIVE_GPU_LAST_USE")need(t>0&&o>=t&&b(j,"completed_GPU"),"actual Native max last-use proof");else need(proof=="NOT_REQUIRED_NO_NATIVE_USE"&&t==0&&o==0&&!b(j,"completed_GPU"),"N0 is not a no-use exemption");}
inline std::string base(const std::string&kind,const std::string&session){return "async-"+kind+"-"+session+"-g0.json";}
inline std::string job_path(const std::string&root,const std::string&kind,const std::string&session,uint64_t job){need(job<BbPreSr::Hot120::MaxJobs,"finite120 job ordinal");char leaf[32];std::snprintf(leaf,sizeof(leaf),"bucket-%06llu",(unsigned long long)(job/64));return root+"/frames/"+leaf+"/async-"+kind+"-"+session+"-g0-f"+std::to_string(job)+".json";}
inline void resource(const J&j,unsigned role){const auto f=spec()["bootstrap_exact"]["async-meta-g0.json"]["resource_required"];exact(j,keys(f));description_types(j,f);const char* names[]={"color","depth","mv","output"};need(role<4&&s(j,"role")==names[role],"unique role order");
 for(auto [k,value]:std::initializer_list<std::pair<const char*,uint64_t>>{{"format",role==1?100u:role==2?103u:109u},{"width",1280u},{"height",720u},{"usage",15},{"tiling",0},{"image_type",1},{"samples",1},{"mip_levels",1},{"array_layers",1},{"flags",0},{"sharing_mode",0},{"offset",0},{"runtime_layout",1}})need(u(j,k)==value,"exact shared CI field");
 need(u(j,"fd")>0&&u(j,"fd")<=INT32_MAX&&u(j,"allocation_size")>0&&u(j,"alignment")>0&&u(j,"memory_type_index")<32&&u(j,"req_memory_type_bits")>0&&u(j,"req_memory_type_bits")<=UINT32_MAX,"actual memory/fd bounds");need(b(j,"dedicated")&&u(j,"prefers_dedicated")<=1&&u(j,"requires_dedicated")<=1,"dedicated VkBool32 strictly uint0or1");need(u(j,"external_features")<=UINT32_MAX&&(u(j,"external_features")&6)==6&&u(j,"compatible_handle_types")<=UINT32_MAX&&(u(j,"compatible_handle_types")&512),"DMA_BUF import/export features");}
}
