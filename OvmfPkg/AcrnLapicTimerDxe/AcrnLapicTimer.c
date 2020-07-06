/** @file
  Timer Architectural Protocol module using Local Advanced Programmable Interrupt
  Controller (LAPIC) Timer.

  Copyright (c) 2011 - 2020, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <PiDxe.h>

#include <Protocol/Cpu.h>
#include <Protocol/Timer.h>

#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/LocalApicLib.h>

#include <Register/LocalApic.h>
#include <Register/Cpuid.h>
#include <Register/ArchitecturalMsr.h>

///
/// Timer Architectural Protocol function prototypes.
///

/**
  This function registers the handler NotifyFunction so it is called every time
  the timer interrupt fires.  It also passes the amount of time since the last
  handler call to the NotifyFunction.  If NotifyFunction is NULL, then the
  handler is unregistered.  If the handler is registered, then EFI_SUCCESS is
  returned.  If the CPU does not support registering a timer interrupt handler,
  then EFI_UNSUPPORTED is returned.  If an attempt is made to register a handler
  when a handler is already registered, then EFI_ALREADY_STARTED is returned.
  If an attempt is made to unregister a handler when a handler is not registered,
  then EFI_INVALID_PARAMETER is returned.  If an error occurs attempting to
  register the NotifyFunction with the timer interrupt, then EFI_DEVICE_ERROR
  is returned.

  @param  This            The EFI_TIMER_ARCH_PROTOCOL instance.
  @param  NotifyFunction  The function to call when a timer interrupt fires.
                          This function executes at TPL_HIGH_LEVEL.  The DXE
                          Core will register a handler for the timer interrupt,
                          so it can know how much time has passed.  This
                          information is used to signal timer based events.
                          NULL will unregister the handler.

  @retval  EFI_SUCCESS            The timer handler was registered.
  @retval  EFI_UNSUPPORTED        The platform does not support timer interrupts.
  @retval  EFI_ALREADY_STARTED    NotifyFunction is not NULL, and a handler is already
                                  registered.
  @retval  EFI_INVALID_PARAMETER  NotifyFunction is NULL, and a handler was not
                                  previously registered.
  @retval  EFI_DEVICE_ERROR       The timer handler could not be registered.

**/
EFI_STATUS
EFIAPI
TimerDriverRegisterHandler (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN EFI_TIMER_NOTIFY         NotifyFunction
  );

/**
  This function adjusts the period of timer interrupts to the value specified
  by TimerPeriod.  If the timer period is updated, then the selected timer
  period is stored in EFI_TIMER.TimerPeriod, and EFI_SUCCESS is returned.  If
  the timer hardware is not programmable, then EFI_UNSUPPORTED is returned.
  If an error occurs while attempting to update the timer period, then the
  timer hardware will be put back in its state prior to this call, and
  EFI_DEVICE_ERROR is returned.  If TimerPeriod is 0, then the timer interrupt
  is disabled.  This is not the same as disabling the CPU's interrupts.
  Instead, it must either turn off the timer hardware, or it must adjust the
  interrupt controller so that a CPU interrupt is not generated when the timer
  interrupt fires.

  @param  This         The EFI_TIMER_ARCH_PROTOCOL instance.
  @param  TimerPeriod  The rate to program the timer interrupt in 100 nS units.
                       If the timer hardware is not programmable, then
                       EFI_UNSUPPORTED is returned.  If the timer is programmable,
                       then the timer period will be rounded up to the nearest
                       timer period that is supported by the timer hardware.
                       If TimerPeriod is set to 0, then the timer interrupts
                       will be disabled.

  @retval  EFI_SUCCESS       The timer period was changed.
  @retval  EFI_UNSUPPORTED   The platform cannot change the period of the timer interrupt.
  @retval  EFI_DEVICE_ERROR  The timer period could not be changed due to a device error.

**/
EFI_STATUS
EFIAPI
TimerDriverSetTimerPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN UINT64                   TimerPeriod
  );

/**
  This function retrieves the period of timer interrupts in 100 ns units,
  returns that value in TimerPeriod, and returns EFI_SUCCESS.  If TimerPeriod
  is NULL, then EFI_INVALID_PARAMETER is returned.  If a TimerPeriod of 0 is
  returned, then the timer is currently disabled.

  @param  This         The EFI_TIMER_ARCH_PROTOCOL instance.
  @param  TimerPeriod  A pointer to the timer period to retrieve in 100 ns units.
                       If 0 is returned, then the timer is currently disabled.

  @retval  EFI_SUCCESS            The timer period was returned in TimerPeriod.
  @retval  EFI_INVALID_PARAMETER  TimerPeriod is NULL.

**/
EFI_STATUS
EFIAPI
TimerDriverGetTimerPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL   *This,
  OUT UINT64                   *TimerPeriod
  );

/**
  This function generates a soft timer interrupt. If the platform does not support soft
  timer interrupts, then EFI_UNSUPPORTED is returned. Otherwise, EFI_SUCCESS is returned.
  If a handler has been registered through the EFI_TIMER_ARCH_PROTOCOL.RegisterHandler()
  service, then a soft timer interrupt will be generated. If the timer interrupt is
  enabled when this service is called, then the registered handler will be invoked. The
  registered handler should not be able to distinguish a hardware-generated timer
  interrupt from a software-generated timer interrupt.

  @param  This  The EFI_TIMER_ARCH_PROTOCOL instance.

  @retval  EFI_SUCCESS       The soft timer interrupt was generated.
  @retval  EFI_UNSUPPORTED   The platform does not support the generation of soft
                             timer interrupts.

**/
EFI_STATUS
EFIAPI
TimerDriverGenerateSoftInterrupt (
  IN EFI_TIMER_ARCH_PROTOCOL  *This
  );

///
/// The handle onto which the Timer Architectural Protocol will be installed.
///
EFI_HANDLE   mTimerHandle = NULL;

///
/// The Timer Architectural Protocol that this driver produces.
///
EFI_TIMER_ARCH_PROTOCOL  mTimer = {
  TimerDriverRegisterHandler,
  TimerDriverSetTimerPeriod,
  TimerDriverGetTimerPeriod,
  TimerDriverGenerateSoftInterrupt
};

///
/// Pointer to the CPU Architectural Protocol instance.
///
EFI_CPU_ARCH_PROTOCOL  *mCpu = NULL;

///
/// The notification function to call on every timer interrupt.
///
EFI_TIMER_NOTIFY  mTimerNotifyFunction = NULL;

///
/// The current period of the LAPIC timer interrupt in 100 ns units.
///
UINT64  mTimerPeriod = 0;

///
/// The number of TSC counts per second.
///
UINT64  mTscFrequency;

/**
  Read from an LAPIC register.

  This function reads from a LAPIC register either in xAPIC or x2APIC mode.
  It is required that in xAPIC mode wider registers (64-bit or 256-bit) must be
  accessed using multiple 32-bit loads or stores, so this function only performs
  32-bit read.

  @param  MmioOffset  The MMIO offset of the LAPIC register in xAPIC mode.
                      It must be 16-byte aligned.

  @return 32-bit      Value read from the register.
**/
STATIC
UINT32
EFIAPI
ReadLocalApicReg (
  IN UINTN  MmioOffset
  )
{
  UINT32 MsrIndex;

  ASSERT ((MmioOffset & 0xf) == 0);

  if (GetApicMode () == LOCAL_APIC_MODE_XAPIC) {
    return MmioRead32 (GetLocalApicBaseAddress() + MmioOffset);
  } else {
    //
    // DFR is not supported in x2APIC mode.
    //
    ASSERT (MmioOffset != XAPIC_ICR_DFR_OFFSET);
    //
    // Note that in x2APIC mode, ICR is a 64-bit MSR that needs special treatment. It
    // is not supported in this function for simplicity.
    //
    ASSERT (MmioOffset != XAPIC_ICR_HIGH_OFFSET);

    MsrIndex = (UINT32)(MmioOffset >> 4) + X2APIC_MSR_BASE_ADDRESS;
    return AsmReadMsr32 (MsrIndex);
  }
}

/**
  Write to an LAPIC register.

  This function writes to a LAPIC register either in xAPIC or x2APIC mode.
  It is required that in xAPIC mode wider registers (64-bit or 256-bit) must be
  accessed using multiple 32-bit loads or stores, so this function only performs
  32-bit write.

  if the register index is invalid or unsupported in current APIC mode, then ASSERT.

  @param  MmioOffset  The MMIO offset of the LAPIC register in xAPIC mode.
                      It must be 16-byte aligned.
  @param  Value       Value to be written to the register.
**/
STATIC
VOID
EFIAPI
WriteLocalApicReg (
  IN UINTN  MmioOffset,
  IN UINT32 Value
  )
{
  UINT32 MsrIndex;

  ASSERT ((MmioOffset & 0xf) == 0);

  if (GetApicMode () == LOCAL_APIC_MODE_XAPIC) {
    MmioWrite32 (GetLocalApicBaseAddress() + MmioOffset, Value);
  } else {
    //
    // DFR is not supported in x2APIC mode.
    //
    ASSERT (MmioOffset != XAPIC_ICR_DFR_OFFSET);
    //
    // Note that in x2APIC mode, ICR is a 64-bit MSR that needs special treatment. It
    // is not supported in this function for simplicity.
    //
    ASSERT (MmioOffset != XAPIC_ICR_HIGH_OFFSET);
    ASSERT (MmioOffset != XAPIC_ICR_LOW_OFFSET);

    MsrIndex =  (UINT32)(MmioOffset >> 4) + X2APIC_MSR_BASE_ADDRESS;
    //
    // The serializing semantics of WRMSR are relaxed when writing to the APIC registers.
    // Use memory fence here to force the serializing semantics to be consistent with xAPIC mode.
    //
    MemoryFence ();
    AsmWriteMsr32 (MsrIndex, Value);
  }
}

/**
  Write to MSR_IA32_TSC_DEADLINE register.

  @param  Value       The 64-bit value to write to the MSR.
**/
STATIC
VOID
EFIAPI
WriteTscDeadlineReg (
  IN UINT64 Value
  )
{
  MemoryFence ();
  AsmWriteMsr64 (MSR_IA32_TSC_DEADLINE, Value);
}

/**
  Initialize the LAPIC timer in TSC-deadline mode.

  The LAPIC timer is initialized and disabled.
**/
VOID
EFIAPI
InitializeApicTimerDeadlineMode (
  VOID
  )
{
  LOCAL_APIC_LVT_TIMER LvtTimer;
  //
  // Ensure LAPIC is in software-enabled state.
  //
  InitializeLocalApicSoftwareEnable (TRUE);

  //
  // Disable APIC timer interrupt in deadline mode.
  //
  LvtTimer.Uint32 = ReadLocalApicReg (XAPIC_LVT_TIMER_OFFSET);
  LvtTimer.Bits.TimerMode = 2;
  LvtTimer.Bits.Mask = 1;
  LvtTimer.Bits.Vector = 64;
  WriteLocalApicReg (XAPIC_LVT_TIMER_OFFSET, LvtTimer.Uint32);
}

/**
  The interrupt handler for an LAPIC timer.

  @param  InterruptType  The type of interrupt that occurred.
  @param  SystemContext  A pointer to the system context when the interrupt occurred.
**/
VOID
EFIAPI
TimerInterruptHandler (
  IN EFI_EXCEPTION_TYPE   InterruptType,
  IN EFI_SYSTEM_CONTEXT   SystemContext
  )
{
  EFI_TPL Tpl;
  UINT64  TimerCount;

  //
  // DXE core uses this callback for the EFI timer tick. The DXE core uses locks
  // that raise to TPL_HIGH and then restore back to current level. Thus we need
  // to make sure TPL level is set to TPL_HIGH while we are handling the timer tick.
  //
  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  //
  // Send EOI
  //
  SendApicEoi ();

  if (mTimerPeriod != 0) {
    // TimerCount = TimerPeriod in 100ns unit x Frequency
    //            = TimerPeriod.10^-7 x Frequency
    //            = (TimerPeriod x Frequency) x 10^-7
    TimerCount = (UINT32)DivU64x32 (
              MultU64x64 (
                mTscFrequency,
                mTimerPeriod
                ), 10000000U
              ) + AsmReadTsc();
  } else {
    TimerCount = 0;
  }

  WriteTscDeadlineReg (TimerCount);

  if (mTimerNotifyFunction != NULL) {
    mTimerNotifyFunction (mTimerPeriod);
  }

  gBS->RestoreTPL (Tpl);
}

/**
  This function registers the handler NotifyFunction so it is called every time
  the timer interrupt fires.  It also passes the amount of time since the last
  handler call to the NotifyFunction.  If NotifyFunction is NULL, then the
  handler is unregistered.  If the handler is registered, then EFI_SUCCESS is
  returned.  If the CPU does not support registering a timer interrupt handler,
  then EFI_UNSUPPORTED is returned.  If an attempt is made to register a handler
  when a handler is already registered, then EFI_ALREADY_STARTED is returned.
  If an attempt is made to unregister a handler when a handler is not registered,
  then EFI_INVALID_PARAMETER is returned.  If an error occurs attempting to
  register the NotifyFunction with the timer interrupt, then EFI_DEVICE_ERROR
  is returned.

  @param  This            The EFI_TIMER_ARCH_PROTOCOL instance.
  @param  NotifyFunction  The function to call when a timer interrupt fires.
                          This function executes at TPL_HIGH_LEVEL.  The DXE
                          Core will register a handler for the timer interrupt,
                          so it can know how much time has passed.  This
                          information is used to signal timer based events.
                          NULL will unregister the handler.

  @retval  EFI_SUCCESS            The timer handler was registered.
  @retval  EFI_UNSUPPORTED        The platform does not support timer interrupts.
  @retval  EFI_ALREADY_STARTED    NotifyFunction is not NULL, and a handler is already
                                  registered.
  @retval  EFI_INVALID_PARAMETER  NotifyFunction is NULL, and a handler was not
                                  previously registered.
  @retval  EFI_DEVICE_ERROR       The timer handler could not be registered.

**/
EFI_STATUS
EFIAPI
TimerDriverRegisterHandler (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN EFI_TIMER_NOTIFY         NotifyFunction
  )
{
  //
  // Check for invalid parameters
  //
  if (NotifyFunction == NULL && mTimerNotifyFunction == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (NotifyFunction != NULL && mTimerNotifyFunction != NULL) {
    return EFI_ALREADY_STARTED;
  }

  //
  // Cache the registered notification function
  //
  mTimerNotifyFunction = NotifyFunction;

  return EFI_SUCCESS;
}

/**
  This function adjusts the period of timer interrupts to the value specified
  by TimerPeriod.  If the timer period is updated, then the selected timer
  period is stored in EFI_TIMER.TimerPeriod, and EFI_SUCCESS is returned.  If
  the timer hardware is not programmable, then EFI_UNSUPPORTED is returned.
  If an error occurs while attempting to update the timer period, then the
  timer hardware will be put back in its state prior to this call, and
  EFI_DEVICE_ERROR is returned.  If TimerPeriod is 0, then the timer interrupt
  is disabled.  This is not the same as disabling the CPU's interrupts.
  Instead, it must either turn off the timer hardware, or it must adjust the
  interrupt controller so that a CPU interrupt is not generated when the timer
  interrupt fires.

  @param  This         The EFI_TIMER_ARCH_PROTOCOL instance.
  @param  TimerPeriod  The rate to program the timer interrupt in 100 nS units.
                       If the timer hardware is not programmable, then
                       EFI_UNSUPPORTED is returned.  If the timer is programmable,
                       then the timer period will be rounded up to the nearest
                       timer period that is supported by the timer hardware.
                       If TimerPeriod is set to 0, then the timer interrupts
                       will be disabled.

  @retval  EFI_SUCCESS       The timer period was changed.
  @retval  EFI_UNSUPPORTED   The platform cannot change the period of the timer interrupt.
  @retval  EFI_DEVICE_ERROR  The timer period could not be changed due to a device error.

**/
EFI_STATUS
EFIAPI
TimerDriverSetTimerPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN UINT64                   TimerPeriod
  )
{
  EFI_TPL Tpl;
  UINT64  TimerCount;

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  //
  // Disable LAPIC timer when adjusting the timer period
  //
  DisableApicTimerInterrupt ();
  WriteTscDeadlineReg (0);

  if (TimerPeriod != 0) {
    // TimerCount = TimerPeriod in 100ns unit x Frequency
    //            = TimerPeriod.10^-7 x Frequency
    //            = (TimerPeriod x Frequency) x 10^-7
    TimerCount = (UINT32)DivU64x32 (
              MultU64x64 (
                mTscFrequency,
                TimerPeriod
                ), 10000000U
              ) + AsmReadTsc();

    EnableApicTimerInterrupt ();
    WriteTscDeadlineReg (TimerCount);
  }

  //
  // Save the new timer period
  //
  mTimerPeriod = TimerPeriod;

  gBS->RestoreTPL (Tpl);

  return EFI_SUCCESS;
}

/**
  This function retrieves the period of timer interrupts in 100 ns units,
  returns that value in TimerPeriod, and returns EFI_SUCCESS.  If TimerPeriod
  is NULL, then EFI_INVALID_PARAMETER is returned.  If a TimerPeriod of 0 is
  returned, then the timer is currently disabled.

  @param  This         The EFI_TIMER_ARCH_PROTOCOL instance.
  @param  TimerPeriod  A pointer to the timer period to retrieve in 100 ns units.
                       If 0 is returned, then the timer is currently disabled.

  @retval  EFI_SUCCESS            The timer period was returned in TimerPeriod.
  @retval  EFI_INVALID_PARAMETER  TimerPeriod is NULL.

**/
EFI_STATUS
EFIAPI
TimerDriverGetTimerPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL   *This,
  OUT UINT64                   *TimerPeriod
  )
{
  if (TimerPeriod == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *TimerPeriod = mTimerPeriod;

  return EFI_SUCCESS;
}

/**
  This function generates a soft timer interrupt. If the platform does not support soft
  timer interrupts, then EFI_UNSUPPORTED is returned. Otherwise, EFI_SUCCESS is returned.
  If a handler has been registered through the EFI_TIMER_ARCH_PROTOCOL.RegisterHandler()
  service, then a soft timer interrupt will be generated. If the timer interrupt is
  enabled when this service is called, then the registered handler will be invoked. The
  registered handler should not be able to distinguish a hardware-generated timer
  interrupt from a software-generated timer interrupt.

  @param  This  The EFI_TIMER_ARCH_PROTOCOL instance.

  @retval  EFI_SUCCESS       The soft timer interrupt was generated.
  @retval  EFI_UNSUPPORTED   The platform does not support the generation of soft
                             timer interrupts.

**/
EFI_STATUS
EFIAPI
TimerDriverGenerateSoftInterrupt (
  IN EFI_TIMER_ARCH_PROTOCOL  *This
  )
{
  EFI_TPL    Tpl;
  EFI_STATUS Status = EFI_SUCCESS;

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  //
  // If the timer interrupt is enabled, then the registered handler will be invoked.
  //
  if (GetApicTimerInterruptState ()) {
    if (mTimerNotifyFunction != NULL) {
      mTimerNotifyFunction (mTimerPeriod);
    }
  } else {
    Status = EFI_UNSUPPORTED;
  }

  gBS->RestoreTPL (Tpl);

  return Status;
}

/**
  Initialize the Timer Architectural Protocol driver

  @param  ImageHandle  ImageHandle of the loaded driver
  @param  SystemTable  Pointer to the System Table

  @retval  EFI_SUCCESS           Timer Architectural Protocol created
  @retval  EFI_OUT_OF_RESOURCES  Not enough resources available to initialize driver.
  @retval  EFI_DEVICE_ERROR      A device error occurred attempting to initialize the driver.

**/
EFI_STATUS
EFIAPI
TimerDriverInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                             Status;
  CPUID_VERSION_INFO_ECX                 Ecx;

  DEBUG ((DEBUG_INFO, "Initializing LAPIC Timer Driver\n"));

  //
  // Make sure the Timer Architectural Protocol is not already installed in the system
  //
  ASSERT_PROTOCOL_ALREADY_INSTALLED (NULL, &gEfiTimerArchProtocolGuid);

  //
  // Find the CPU architectural protocol.
  //
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **) &mCpu);
  ASSERT_EFI_ERROR (Status);

  //
  // Check if LAPIC supports TSC-deadine mode
  //
  AsmCpuid (CPUID_VERSION_INFO, NULL, NULL, &Ecx.Uint32, NULL);
  if (Ecx.Bits.TSC_Deadline == 0) {
    DEBUG ((DEBUG_ERROR, "LAPIC TSC-deadine mode is not supported. Unload LAPIC timer driver.\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Get TSC frequency
  //
  mTscFrequency = (UINT64) PcdGet32 (PcdFSBClock);

  //
  // Disable LAPIC timer during initialization
  //
  InitializeApicTimerDeadlineMode ();

  //
  // Install interrupt handler
  //
  Status = mCpu->RegisterInterruptHandler (mCpu, 64, TimerInterruptHandler);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Unable to register LAPIC interrupt with CPU Arch Protocol. Unload LAPIC timer driver.\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Force the LAPIC Timer to be enabled
  //
  Status = TimerDriverSetTimerPeriod (&mTimer, 1000000);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Unable to set LAPIC default timer period. Unload LAPIC timer driver.\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Install the Timer Architectural Protocol onto a new handle
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mTimerHandle,
                  &gEfiTimerArchProtocolGuid, &mTimer,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}