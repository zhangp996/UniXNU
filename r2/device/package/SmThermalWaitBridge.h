// SPDX-License-Identifier: MIT
#pragma once
#include <mach/mach_types.h>
struct smtw_selection {
 task_t task;
 unsigned scanned;
 int pid;
 unsigned uid;
 unsigned matched;
 unsigned uuid_source;      // 1 = proc_getexecutableuuid copied a non-zero UUID
 uint8_t exec_uuid[16];
};
void native_sm8250_thermal_wait_select(struct smtw_selection *);
void native_sm8250_gui_wait_select(struct smtw_selection *,const char *,const uint8_t [16]);
