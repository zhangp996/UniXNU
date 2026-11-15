/** @file
  Minimal volume-key console input for instantnoodlep.

  Volume up is PM8150 SPMI GPIO6 at peripheral 0xC500 and volume down is GPIO7 at
  0xC600 (docs/evidence/device-fdt.dts gpio_keys; the stock kernel's /proc/interrupts
  decodes volume_up/down to per 0xC5/0xC6 and APID 25/26). The power key is the PM8150
  PON at peripheral 0x0800, RT_STS bit0 (KPDPWR_N, active low), and is reported as
  Enter so the menu can boot the selection. All keys are active low.
  The driver polls the GPIO real-time status byte (offset 0x10, bit 0) every 10 ms,
  requires two identical samples so the 15 ms debounce in the device tree is covered,
  and publishes an EFI_SIMPLE_TEXT_INPUT_PROTOCOL so ConSplitter folds the keys into
  ConIn without a serial console.
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/ConsoleInDevice.h>

#include <SpmiLib.h>

#define GPIO_UP          0xC500
#define GPIO_DOWN        0xC600
#define GPIO_RT_STS      0x10
#define PON_ADDRESS      0x0800
#define PON_RT_STS       0x10
#define POLL_MS          10
#define DEBOUNCE_SAMPLES 2
#define QUEUE_SIZE       8

typedef struct {
  BOOLEAN  Raw;
  BOOLEAN  Stable;
  UINTN    Count;
} KEY_STATE;

STATIC EFI_EVENT                      mTimer;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mInput;
STATIC EFI_INPUT_KEY                  mQueue[QUEUE_SIZE];
STATIC UINTN                          mHead;
STATIC UINTN                          mTail;
STATIC KEY_STATE                      mUp;
STATIC KEY_STATE                      mDown;
STATIC KEY_STATE                      mPower;

STATIC
BOOLEAN
Pressed (
  IN UINT16  Address
  )
{
  UINT32  Value = 0;

  if (EFI_ERROR (SpmiRead (0, Address, GPIO_RT_STS, 1, &Value))) {
    return FALSE;
  }
  /* Active low: the key pulls the pin to ground. */
  return (Value & 1) == 0;
}

STATIC
BOOLEAN
PowerPressed (
  VOID
  )
{
  UINT32  Value = 0;

  if (EFI_ERROR (SpmiRead (0, PON_ADDRESS, PON_RT_STS, 1, &Value))) {
    return FALSE;
  }
  /* KPDPWR_N is bit0 and active low (mainline pm8941-pwrkey on pm8150). */
  return (Value & 0x1) == 0;
}

//
// Return TRUE only on a stable released-to-pressed transition. A held key is
// reported once; the release is tracked silently.
//
STATIC
BOOLEAN
Debounce (
  IN OUT KEY_STATE *State,
  IN     BOOLEAN    Sample
  )
{
  if (Sample == State->Raw) {
    if (State->Count < DEBOUNCE_SAMPLES) {
      State->Count++;
    }
  } else {
    State->Raw   = Sample;
    State->Count = 1;
  }

  if ((State->Count >= DEBOUNCE_SAMPLES) && (State->Stable != State->Raw)) {
    State->Stable = State->Raw;
    return State->Stable;
  }
  return FALSE;
}

STATIC
VOID
QueueKey (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  if (((mTail + 1) % QUEUE_SIZE) == mHead) {
    return;
  }
  mQueue[mTail].ScanCode    = ScanCode;
  mQueue[mTail].UnicodeChar = UnicodeChar;
  mTail = (mTail + 1) % QUEUE_SIZE;
}

//
// WaitForKey is an EVT_NOTIFY_WAIT event: the timer queues the key and signals it,
// and the no-op notify only has to exist because CheckEvent queues it while idle.
//
STATIC
VOID
EFIAPI
WaitForKeyNotify (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
}

STATIC
VOID
EFIAPI
Poll (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  BOOLEAN  Added = FALSE;

  if (Debounce (&mUp, Pressed (GPIO_UP))) {
    QueueKey (SCAN_UP, CHAR_NULL);
    Added = TRUE;
  }
  if (Debounce (&mDown, Pressed (GPIO_DOWN))) {
    QueueKey (SCAN_DOWN, CHAR_NULL);
    Added = TRUE;
  }
  if (Debounce (&mPower, PowerPressed ())) {
    QueueKey (SCAN_NULL, CHAR_CARRIAGE_RETURN);
    Added = TRUE;
  }
  if (Added) {
    gBS->SignalEvent (mInput.WaitForKey);
  }
}

STATIC
EFI_STATUS
EFIAPI
InputReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mHead = 0;
  mTail = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  OUT EFI_INPUT_KEY                  *Key
  )
{
  if (mHead == mTail) {
    return EFI_NOT_READY;
  }
  *Key  = mQueue[mHead];
  mHead = (mHead + 1) % QUEUE_SIZE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KonaKeysEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  Status = SpmiInitialize ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Seed the debounce state so a key already held at boot is not a new press. */
  mUp.Raw      = Pressed (GPIO_UP);
  mUp.Stable   = mUp.Raw;
  mUp.Count    = DEBOUNCE_SAMPLES;
  mDown.Raw    = Pressed (GPIO_DOWN);
  mDown.Stable = mDown.Raw;
  mDown.Count  = DEBOUNCE_SAMPLES;
  mPower.Raw   = PowerPressed ();
  mPower.Stable = mPower.Raw;
  mPower.Count = DEBOUNCE_SAMPLES;

  mInput.Reset         = InputReset;
  mInput.ReadKeyStroke = ReadKeyStroke;
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  WaitForKeyNotify,
                  NULL,
                  &mInput.WaitForKey
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Poll, NULL, &mTimer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->SetTimer (mTimer, TimerPeriodic, POLL_MS * 10000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* ConSplitterConInDriverBindingSupported only binds a handle that carries the
     ConsoleInDevice tag, so publish it next to the protocol itself. */
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEfiSimpleTextInProtocolGuid, &mInput,
                &gEfiConsoleInDeviceGuid, NULL,
                NULL
                );
}
