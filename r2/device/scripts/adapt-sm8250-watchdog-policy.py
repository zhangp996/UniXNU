#!/usr/bin/env python3
"""Derive explicit 21H461 owner policy for the real board watchdog class.

This changes one read-only policy condition, not any policy execution code.
It is a board owner policy, not an unmodified or Apple-authenticated policy.
"""
import argparse,hashlib,json,struct,sys
from pathlib import Path

ORIGINAL_SHA='f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
TEXT_SHA='c6982990cbb77c43ddad60a45bbd3e5936334c166aadf819125aaa851a4ee223'
EXEC_SHA={'com.apple.security.sandbox':'2f79896751bf9eb1f78b0a13ffeb50acd0c27acdb19bbdf15908a4f6c13e2788','com.apple.driver.AppleARMWatchdogTimer':'fe9b7ca78df85b0a8292f3e8130e658b53ecb987da76f8e3bae7722f37df5173'}
OLD=b'AppleARMWatchdogTimer';NEW=b'SmUfsPlatformWatchdog'
CONTEXT=bytes.fromhex('6f6755736572436c69656e740f000f0a00000000001a00544170706c6541524d5761746368646f6754696d65720f000f0a00000000210048494f504d55426f6f740f44537461676582000f0a464170704e616d650f000f0a')
VA=0xfffffff00583881b;SLIDE=-0x1000000
sha=lambda b:hashlib.sha256(b).hexdigest()
def need(ok,message):
 if not ok:raise ValueError(message)

def derive(raw,native,plan,api):
 macho,backed_offset,unserialize_xml,exports=api
 need(sha(raw)==ORIGINAL_SHA,'Wrong original21H461 cache')
 need(len(OLD)==len(NEW)==21,'Fixed equal-length real class names')
 need(plan['original_sha256']==ORIGINAL_SHA and plan['slide']==SLIDE,'Wrong original plan or slide')
 need(plan['candidate_sha256']==sha(native) and plan['candidate_bytes']==len(native),'Candidate/plan identity')
 need('board_owner_watchdog_policy' not in plan,'Already adapted policy')
 original=macho(raw);candidate=macho(native)
 read=lambda data,img,a,n:data[backed_offset(img['segments'],a,n,len(data)):][:n]
 info=next(s for s in original['segments'] if s['name']=='__PRELINK_INFO')
 entries=unserialize_xml(read(raw,original,info['va'],info['filesize']))['_PrelinkInfoDictionary']
 mods={}
 for module in EXEC_SHA:
  entry=next(e for e in entries if e['CFBundleIdentifier']==module)
  mods[module]=macho(raw,backed_offset(original['segments'],entry['_PrelinkExecutableLoadAddr'],32,len(raw)))
  for seg in mods[module]['segments']:
   if not seg['filesize']:continue
   rows=[e for e in plan['extents'] if e['module']==module and e['segment']==seg['name']]
   need(len(rows)==1 and rows[0]['va']==seg['va']+SLIDE and rows[0]['size']==seg['filesize'],'Exact module mapping '+module+seg['name'])
  code=next(s for s in mods[module]['segments'] if s['name']=='__TEXT_EXEC')
  need(sha(read(raw,original,code['va'],code['filesize']))==EXEC_SHA[module],'Pinned original executable '+module)
  need(sha(read(native,candidate,code['va']+SLIDE,code['filesize']))==EXEC_SHA[module]==plan['original_executable_sha256'][module],'Unchanged candidate executable '+module)
 seg=next(s for s in mods['com.apple.security.sandbox']['segments'] if s['name']=='__TEXT')
 need(seg['initprot']==1 and seg['filesize']==1538612,'Pinned read-only policy segment')
 text=read(raw,original,seg['va'],seg['filesize'])
 need(sha(text)==TEXT_SHA,'Exact original policy segment')
 # Prelink rebases the embedded Mach-O header's segment/section VA fields.
 # Derive exactly those fixed-width header changes; policy bytes stay pristine.
 header_at=backed_offset(original['segments'],seg['va'],32,len(raw))
 expected_text=bytearray(text)
 for part in mods['com.apple.security.sandbox']['segments']:
  struct.pack_into('<Q',expected_text,part['command_offset']-header_at+24,part['va']+SLIDE)
  for section in part['sections']:
   struct.pack_into('<Q',expected_text,section['command_offset']-header_at+32,section['va']+SLIDE)
 need(sha(expected_text)=='08aaf5b105296d455099eb383d5b26de36ada567ed0356afd830474915c5d856','Exact normal prelink header rebasing')
 need(read(native,candidate,seg['va']+SLIDE,seg['filesize'])==expected_text,'Candidate policy must be pristine except derived header rebasing')
 hits=[]
 for s in mods['com.apple.security.sandbox']['segments']:
  if not s['filesize']:continue
  b=read(raw,original,s['va'],s['filesize']);start=0
  while (start:=b.find(OLD,start))>=0:hits.append((s['name'],s['va']+start));start+=1
 need(hits==[('__TEXT',VA)],'Unique pinned module condition')
 need(read(raw,original,VA-24,len(CONTEXT))==CONTEXT,'Pinned serialized policy context')
 need(read(native,candidate,VA+SLIDE-24,len(CONTEXT))==CONTEXT,'Candidate context')
 names={name:va for va,ns in exports(native,candidate).items() for name in ns}
 need('__ZTV21SmUfsPlatformWatchdog' in names and '_native_smufs_watchdog_prepare' in names,'Real linked board watchdog required')
 offset=backed_offset(candidate['segments'],VA+SLIDE,len(NEW),len(native))
 result=native[:offset]+NEW+native[offset+21:]
 need(len(result)==len(native) and result[:offset]==native[:offset] and result[offset+21:]==native[offset+21:],'Only fixed data span may change')
 changed=[offset+i for i,(a,b) in enumerate(zip(OLD,NEW)) if a!=b]
 receipt={'schema':'sm8250-owner-watchdog-policy-v1','original_kernelcache_sha256':ORIGINAL_SHA,'parent_kernel_sha256':sha(native),'adapted_kernel_sha256':sha(result),'module':'com.apple.security.sandbox','segment':'__TEXT','original_va':hex(VA),'candidate_va':hex(VA+SLIDE),'file_offset':offset,'span_bytes':21,'changed_file_offsets':changed,'old_class':OLD.decode(),'new_real_class':NEW.decode(),'context_hex':CONTEXT.hex(),'original_policy_segment_sha256':TEXT_SHA,'adapted_policy_segment_sha256':sha(read(result,candidate,seg['va']+SLIDE,seg['filesize'])),'unchanged_executable_sha256':EXEC_SHA,'only_class_condition_changed':True,'apple_authenticated_policy':False,'native_policy_results_pending':True}
 derived=dict(plan);derived['candidate_sha256']=sha(result);derived['board_owner_watchdog_policy']=receipt
 return result,derived,receipt

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kernel',type=Path,required=True);p.add_argument('--plan',type=Path,required=True);p.add_argument('--kernelcache',type=Path,required=True);p.add_argument('--prelink',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 sys.path.insert(0,str(a.prelink));from native_prelink import macho,backed_offset,unserialize_xml,exports
 need(not a.output.exists() and not a.output.with_suffix('.json').exists(),'Never overwrite candidate or receipt')
 data,plan,receipt=derive(a.kernelcache.read_bytes(),a.kernel.read_bytes(),json.loads(a.plan.read_text()),(macho,backed_offset,unserialize_xml,exports))
 a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(data);a.output.with_suffix('.json').write_text(json.dumps(plan,indent=2)+'\n');a.output.with_suffix('.policy.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
