/** @file
  OVMF ACPI ACRN support

  Copyright (c) 2018, Intel Corporation. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/ 

#include "AcpiPlatform.h"

/**
  Get ACRN Acpi tables from the RSDP structure. And installs ACRN ACPI tables
  into the RSDT/XSDT using InstallAcpiTable. Some signature of the installed
  ACPI tables are: FACP, APIC, HPET, WAET, SSDT, FACS, DSDT.

  @param  AcpiProtocol           Protocol instance pointer.

  @return EFI_SUCCESS            The table was successfully inserted.
  @return EFI_INVALID_PARAMETER  Either AcpiTableBuffer is NULL, TableHandle is
                                 NULL, or AcpiTableBufferSize and the size
                                 field embedded in the ACPI table pointed to
                                 by AcpiTableBuffer are not in sync.
  @return EFI_OUT_OF_RESOURCES   Insufficient resources exist to complete the request.

**/
EFI_STATUS
EFIAPI
InstallAcrnTables (
  IN   EFI_ACPI_TABLE_PROTOCOL       *AcpiProtocol
  )
{
  /* reuse Xen's implementation */
  return InstallXenTables (AcpiProtocol);
}

