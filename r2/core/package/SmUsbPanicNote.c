/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
extern const char *native_sm_usb_status_note(void);
extern const char *native_sm8250_thermal_wait_note(void);
const char *native_sm_usb_panic_note(void);
/* First panic owns this bounded RAM-only concatenation. Never wait on a writer,
 * call USB/MMIO, allocate, or format in this getter. Existing note is retained. */
__attribute__((used)) const char *native_sm_usb_panic_note(void) {
 static char note[1024];static unsigned claimed;
 if(__atomic_exchange_n(&claimed,1U,__ATOMIC_ACQ_REL))return "SMUSB21 panic note already claimed\n";
 const char *parts[2]={native_sm_usb_status_note(),native_sm8250_thermal_wait_note()};
 size_t used=0;
 for(unsigned p=0;p<2;++p)if(parts[p])
  for(size_t i=0;i<1023 && parts[p][i] && used<sizeof(note)-1;++i)note[used++]=parts[p][i];
 note[used]=0;return note;
}
