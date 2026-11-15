// SPDX-License-Identifier: MIT
// BSD-only process selection; task ownership transfers to the Mach observer.
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/systm.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/file_internal.h>
#include <sys/vnode.h>
#include <kern/task.h>
#include <machine/machine_routines.h>
#include "SmThermalWaitBridge.h"
static int smtw_select(proc_t process,void *opaque) {
 struct smtw_selection *s=opaque;char name[MAXCOMLEN+1]={0};++s->scanned;
 proc_name(proc_pid(process),name,sizeof(name));
 if(!strcmp(name,"thermalmonitord") && proc_ppid(process)==1) {
  s->matched=1;s->pid=proc_pid(process);s->uid=proc_getuid(process);
  // Kernel-resident executable identity: no user Mach-O header is read. The
  // copy is not an atomic exec generation; the Mach side rechecks the task's
  // slide/base/cache UUID under task_lock and refuses on any change.
  uint8_t uuid[16]={0};
  proc_getexecutableuuid(process,uuid,sizeof(uuid));
  bool nonzero=false;
  for(unsigned i=0;i<sizeof(uuid);++i)if(uuid[i])nonzero=true;
  if(nonzero) {memcpy(s->exec_uuid,uuid,sizeof(uuid));s->uuid_source=1;}
  // proc_iterate holds the proc reference here. In this XNU proc/task are
  // coallocated (proc_get_task_raw); take a full task reference before return.
  task_t task=proc_task(process);
  if(task!=TASK_NULL) {task_reference(task);s->task=task;}
  return PROC_RETURNED_DONE;
 }
 return s->scanned>=512?PROC_RETURNED_DONE:PROC_RETURNED;
}
void native_sm8250_thermal_wait_select(struct smtw_selection *selected) {
 proc_iterate(PROC_ALLPROCLIST|PROC_NOWAITTRANS,smtw_select,selected,NULL,NULL);
}

// Temporary bring-up isolation requested by the user on the author's advice.
// Pattern: huajifruit/UniXNU-sirius ac0c900485f3c0cd18de9707579a1cdcd3365503
// package/BootWatchdogPolicy.c (MIT).
// Verify an opened object before reusing policy data from a previous boot.
static int smtw_policy_file(proc_t p,int fd) {
 struct fileproc *fp=NULL;vnode_t vp=NULLVP;
 int error=fp_getfvp(p,fd,&fp,&vp);
 if(error)return error;
 error=vnode_getwithref(vp);
 if(!error) {
  struct vnode_attr attr;
  VATTR_INIT(&attr);
  VATTR_WANTED(&attr,va_uid);VATTR_WANTED(&attr,va_mode);
  error=vnode_getattr(vp,&attr,vfs_context_current());
  if(!error && (vnode_vtype(vp)!=VREG || !VATTR_ALL_SUPPORTED(&attr) ||
      attr.va_uid!=0 || (attr.va_mode&0022)))error=EAUTH;
  vnode_put(vp);
 }
 int dropped=fp_drop(p,fd,fp,0);
 return error?error:dropped;
}

// Called after Data mounts, before launchd exec. Install a starting policy
// once, then leave the existing overrides and migration state to launchd.
int native_sm8250_thermal_job_disable(uint64_t scratch) {
 static const char directory[]="/private/var/db/com.apple.xpc.launchd";
 static const char path[]="/private/var/db/com.apple.xpc.launchd/disabled.plist";
 static const char marker[]="/private/var/db/com.apple.xpc.launchd/disabled.migrated";
 static const char policy[]="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<plist version=\"1.0\"><dict><key>com.apple.thermalmonitord</key><true/>"
  "<key>com.apple.CommCenter</key><true/></dict></plist>\n";
 proc_t p=current_proc();int error=0,fd=-1;int32_t result=0;
 if(proc_pid(p)!=1 || !scratch || scratch>UINT64_MAX-0x400)return EINVAL;
 if((error=copyout(directory,scratch,sizeof(directory))))return error;
 struct mkdir_args md={.path=scratch,.mode=0755};
 error=mkdir(p,&md,&result);if(error && error!=EEXIST)return error;
 if((error=copyout(path,scratch,sizeof(path))))return error;
 struct open_args op={.path=scratch,.flags=O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
 error=open(p,&op,&result);
 struct close_args cl={.fd=-1};int closed=0;
 if(error==EEXIST) {
  op.flags=O_RDONLY|O_NOFOLLOW|O_NONBLOCK;
  if((error=open(p,&op,&result))) {
   printf("SMUFS jobs-off19 existing-open errno%d\n",error);
   return error;
  }
  fd=result;error=smtw_policy_file(p,fd);
  cl.fd=fd;closed=sys_close(p,&cl,&result);if(!error)error=closed;
  printf("SMUFS jobs-off19 existing-policy launchd1 errno%d\n",error);
  return error;
 }
 if(error)return error;

 fd=result;
 if(!(error=copyout(policy,scratch+0x200,sizeof(policy)-1))) {
  struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=sizeof(policy)-1};user_ssize_t n=0;
  error=write(p,&wr,&n);if(!error && n!=(user_ssize_t)(sizeof(policy)-1))error=EIO;
 }
 if(!error){struct fchown_args a={.fd=fd,.uid=0,.gid=0};error=fchown(p,&a,&result);}
 if(!error){struct fchmod_args a={.fd=fd,.mode=0644};error=fchmod(p,&a,&result);}
 if(!error){struct fsync_args a={.fd=fd};error=fsync(p,&a,&result);}
 cl.fd=fd;closed=sys_close(p,&cl,&result);if(!error)error=closed;
 if(error)return error;

 // Check the policy just created, including EOF, before publishing the marker.
 op.flags=O_RDONLY|O_NOFOLLOW|O_NONBLOCK;
 if((error=open(p,&op,&result)))return error;
 fd=result;
 error=smtw_policy_file(p,fd);
 char bytes[sizeof(policy)]={0};user_ssize_t n=0;
 struct read_args rd={.fd=fd,.cbuf=scratch+0x200,.nbyte=sizeof(bytes)};
 if(!error)error=read(p,&rd,&n);
 if(!error && n!=(user_ssize_t)(sizeof(policy)-1))error=EIO;
 if(!error)error=copyin(scratch+0x200,bytes,(size_t)n);
 if(!error && memcmp(bytes,policy,sizeof(policy)-1))error=EAUTH;
 cl.fd=fd;closed=sys_close(p,&cl,&result);if(!error)error=closed;
 if(error)return error;

 if((error=copyout(marker,scratch,sizeof(marker))))return error;
 op.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW;
 if((error=open(p,&op,&result)))return error;
 fd=result;
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 struct fchmod_args mode={.fd=fd,.mode=0644};
 struct fsync_args sync={.fd=fd};
 error=fchown(p,&owner,&result);
 if(!error)error=fchmod(p,&mode,&result);
 if(!error)error=fsync(p,&sync,&result);
 cl.fd=fd;closed=sys_close(p,&cl,&result);if(!error)error=closed;
 printf("SMUFS jobs-off19 thermal1 comm1 file-verified1 marker1 errno%d\n",error);
 return error;
}

struct smui_selector {
 struct smtw_selection *selection;
 const char *name;
 const uint8_t *uuid;
};
static int smui_select(proc_t process,void *opaque) {
 const struct smui_selector *request=opaque;
 struct smtw_selection *result=request->selection;
 char name[MAXCOMLEN+1]={0};
 ++result->scanned;
 proc_name(proc_pid(process),name,sizeof(name));
 if(!strcmp(name,request->name)) {
  result->matched++;
  result->pid=proc_pid(process);
  if(proc_ppid(process)!=1)return result->scanned>=512?PROC_RETURNED_DONE:PROC_RETURNED;
  uint8_t uuid[16]={0};
  proc_getexecutableuuid(process,uuid,sizeof(uuid));
  if(!memcmp(uuid,request->uuid,sizeof(uuid))) {
   result->uuid_source=1;
   task_t task=proc_task(process);
   if(task!=TASK_NULL) {
    task_reference(task);
    result->task=task;
    result->pid=proc_pid(process);
    memcpy(result->exec_uuid,uuid,sizeof(uuid));
    return PROC_RETURNED_DONE;
   }
  }
 }
 return result->scanned>=512?PROC_RETURNED_DONE:PROC_RETURNED;
}
void native_sm8250_gui_wait_select(struct smtw_selection *result,const char *name,const uint8_t uuid[16]) {
 struct smui_selector request={result,name,uuid};
 proc_iterate(PROC_ALLPROCLIST|PROC_NOWAITTRANS,smui_select,&request,NULL,NULL);
}
