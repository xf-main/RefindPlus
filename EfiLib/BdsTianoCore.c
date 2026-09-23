/** @file
  BDS Lib functions which relate with create or process the boot option.

Copyright (c) 2004 - 2011, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/
/**
 * Modified for RefindPlus
 * Copyright (c) 2020-2025 Dayo Akanji (sf.net/u/dakanji/profile)
 *
 * Modifications distributed under the preceding terms.
**/

#ifdef __MAKEWITH_TIANO
#include "../include/tiano_includes.h"
#else
#include "BdsHelper.h"
#include "gnuefi-helper.h"
#endif

#include "../Main/rp_funcs.h"
#include "../include/refit_call_wrapper.h"

EFI_GUID EfiDevicePathProtocolGuid = { 0x09576E91, 0x6D3F, 0x11D2, \
    { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }};

/**
  This function will create all handles associate with every device
  path node. If the handle associate with one device path node can not
  be created success, then still give one chance to do the dispatch,
  which load the missing drivers if possible.

  @param  DevicePathToConnect   The device path which will be connected, it can be
                                a multi-instance device path

  @retval EFI_SUCCESS           All handles associate with every device path  node
                                have been created
  @retval EFI_OUT_OF_RESOURCES  There is no resource to create new handles
  @retval EFI_NOT_FOUND         Create the handle associate with one device  path
                                node failed

**/
EFI_STATUS BdsLibConnectDevicePath (
    IN EFI_DEVICE_PATH_PROTOCOL  *DevicePathToConnect
) {
    EFI_STATUS                 Status;
    UINTN                      Size;
    EFI_DEVICE_PATH_PROTOCOL  *Instance;
    EFI_DEVICE_PATH_PROTOCOL  *RemainingDevicePath;
    EFI_DEVICE_PATH_PROTOCOL  *CopyOfDevicePath;
    EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
    EFI_DEVICE_PATH_PROTOCOL  *Next;
    EFI_HANDLE                 Handle;
    EFI_HANDLE                 PreviousHandle;


    if (DevicePathToConnect == NULL) {
        return EFI_SUCCESS;
    }

    // Freed later via CopyOfDevicePath if not NULL
    DevicePath = DuplicateDevicePath (DevicePathToConnect);
    CopyOfDevicePath = DevicePath;
    if (DevicePath == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    do {
        // The outer loop handles multi-instance device paths.
        // Only console variables contain multi-instance paths.
        //
        // After this call, DevicePath points to the next instance
        Instance = GetNextDevicePathInstance (&DevicePath, &Size);
        if (Instance == NULL) {
            Status = EFI_OUT_OF_RESOURCES;

            break;
        }

        Next = Instance;
        while (!IsDevicePathEndType (Next)) {
            Next = NextDevicePathNode (Next);
        }
        SetDevicePathEndNode (Next);

        PreviousHandle = NULL;
        do {
            // Loops until RemainingDevicePath is an empty device path
            //
            // Find the handle that best matches the device path.
            // If only partial match, the remaining part of the
            // device path is returned in RemainingDevicePath.
            RemainingDevicePath = Instance;

            Status = REFIT_CALL_3_WRAPPER(
                gBS->LocateDevicePath, &EfiDevicePathProtocolGuid,
                &RemainingDevicePath, &Handle
            );
            if (EFI_ERROR(Status)) continue;

            if (Handle == PreviousHandle) {
                // If no forward progress is made, try invoking the Dispatcher.
                // A new FV may have been added and new drivers may be found.
                // EFI_SUCCESS status if drivers were dispatched.
                // EFI_NOT_FOUND status if drivers were not dispatched.
                Status = (gDS != NULL) ? gDS->Dispatch() : EFI_NOT_FOUND;
                if (EFI_ERROR(Status)) continue;
            }

            // Connect all drivers that apply to Handle and RemainingDevicePath,
            // the "Recursive" flag is FALSE so only one level will be expanded.
            //
            // Do not check the connect status here.
            // 1. If connection fails, "RemainingDevicepath" and "Handle" will
            //    not change. Run Dispatcher and adopt dispatch status.
            // 2. If connection works, "RemainingDevicepath" and "Handle" will
            //    change. Avoid dispatch and continue to next connection.
            PreviousHandle = Handle;
            REFIT_CALL_4_WRAPPER(
                gBS->ConnectController, Handle,
                NULL, RemainingDevicePath, FALSE
            );
        } while (
            RemainingDevicePath != NULL &&
            !IsDevicePathEnd (RemainingDevicePath)
        );
    } while (DevicePath != NULL);

    MY_FREE_POOL(CopyOfDevicePath);

    return Status;
} // EFI_STATUS BdsLibConnectDevicePath()

/**
  Build the boot#### or driver#### option from the VariableName, the
  build boot#### or driver#### will also be linked to BdsCommonOptionList.

  @param  BdsCommonOptionList   The header of the boot#### or driver#### option
                                link list
  @param  VariableName          UEFI Variable name indicate if it is boot#### or
                                driver####

  @retval BDS_COMMON_OPTION     Get the option just been created
  @retval NULL                  Failed to get the new option

**/
BDS_COMMON_OPTION * BdsLibVariableToOption (
    IN OUT LIST_ENTRY *BdsCommonOptionList,
    IN     CHAR16     *VariableName
) {
    UINT32                     Attribute;
    UINT16                     FilePathSize;
    UINT8                     *Variable;
    UINT8                     *TempPtr;
    UINTN                      VariableSize;
    INTN                       i;
    EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
    BDS_COMMON_OPTION         *Option;
    VOID                      *LoadOptions;
    UINT32                     LoadOptionsSize;
    CHAR16                    *Description;

    // DA-TAG: Investigate This
    //         From upstream 'Read the variable. We will never free this data'
    //         Yet data is freed ... oversight?
    Variable = BdsLibGetVariableAndSize (
        VariableName,
        &gEfiGlobalVariableGuid,
        &VariableSize
    );
    if (Variable == NULL) {
        return NULL;
    }

    // Notes: careful defined the variable of Boot#### or
    // Driver####, consider use some macro to abstract the code
    //
    // Get the option attribute
    TempPtr    =  Variable;
    Attribute  =  *(UINT32 *) Variable;
    TempPtr   += sizeof (UINT32);

    // Get the option's device path size
    FilePathSize  =  *(UINT16 *) TempPtr;
    TempPtr      += sizeof (UINT16);

    // Get the option's description string
    Description = (CHAR16 *) TempPtr;

    // Get the option's description string size
    TempPtr += StrSize ((CHAR16 *) TempPtr);

    // Get the option's device path
    DevicePath       = (EFI_DEVICE_PATH_PROTOCOL *) TempPtr;
    TempPtr         += FilePathSize;
    LoadOptions      = TempPtr;
    LoadOptionsSize  = (UINT32) (VariableSize - (UINTN) (TempPtr - Variable));

    // The Console variables may have multiple device paths,
    // so make an Entry for each one.
    Option = AllocateZeroPool (sizeof (BDS_COMMON_OPTION));
    if (Option == NULL) {
        MY_FREE_POOL(Variable);

        return NULL;
    }

    Option->Signature  = BDS_LOAD_OPTION_SIGNATURE;
    Option->DevicePath = AllocateZeroPool (GetDevicePathSize (DevicePath));
    if (Option->DevicePath == NULL) {
        MY_FREE_POOL(Option);
        MY_FREE_POOL(Variable);

        return NULL;
    }

    REFIT_CALL_3_WRAPPER(
        gBS->CopyMem, Option->DevicePath,
        DevicePath, GetDevicePathSize (DevicePath)
    );
    Option->Attribute   = Attribute;
    Option->Description = AllocateZeroPool (StrSize (Description));
    if (Option->Description == NULL) {
        MY_FREE_POOL(Option->DevicePath);
        MY_FREE_POOL(Option);
        MY_FREE_POOL(Variable);

        return NULL;
    }

    REFIT_CALL_3_WRAPPER(
        gBS->CopyMem, Option->Description,
        Description, StrSize (Description)
    );
    Option->LoadOptions = AllocateZeroPool (LoadOptionsSize);
    if (Option->LoadOptions == NULL) {
        MY_FREE_POOL(Option->DevicePath);
        MY_FREE_POOL(Option->Description);
        MY_FREE_POOL(Option);
        MY_FREE_POOL(Variable);

        return NULL;
    }

    REFIT_CALL_3_WRAPPER(
        gBS->CopyMem, Option->LoadOptions,
        LoadOptions, LoadOptionsSize
    );
    Option->LoadOptionsSize = LoadOptionsSize;

    // Get the value from VariableName Unicode string since the ISO
    // standard assumes ASCII equivalent abbreviations, we can be safe
    // in converting this Unicode stream to ASCII without any loss in meaning.
    i = 0;

    #define is(x) (VariableName[i++] == x)
    #define ishex ({CHAR16 c; c = VariableName[i++]; (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');})
    #define hex ({CHAR16 c; c = VariableName[4 + i]; (c - (c <= '9' ? '0' : 'A' - 10)) << ((3 - i++) * 4);})

    if (is('B') && is('o') && is('o') && is('t') && ishex && ishex && ishex && ishex && is(0)) {
        i = 0;
        Option->BootCurrent = hex + hex + hex + hex;
    }

    #undef is
    #undef ishex

    // Insert active entry to BdsDeviceList
    if ((Option->Attribute & LOAD_OPTION_ACTIVE) == LOAD_OPTION_ACTIVE) {
        InsertTailList (BdsCommonOptionList, &Option->Link);
        MY_FREE_POOL(Variable);

        return Option;
    }

    MY_FREE_POOL(Option->DevicePath);
    MY_FREE_POOL(Option->Description);
    MY_FREE_POOL(Option->LoadOptions);
    MY_FREE_POOL(Option);
    MY_FREE_POOL(Variable);

    return NULL;
} // BDS_COMMON_OPTION * BdsLibVariableToOption()


/**
  Read the UEFI variable (VendorGuid/Name) and return a dynamically allocated
  buffer, and the size of the buffer. If failure return NULL.

  @param  Name                  String part of UEFI variable name
  @param  VendorGuid            GUID part of UEFI variable name
  @param  VariableSize          Returns the size of the UEFI variable that was read

  @return                       Dynamically allocated memory that contains a copy of the UEFI variable
                                Caller is responsible freeing the buffer.
  @retval NULL                  Variable was not read

**/
VOID * BdsLibGetVariableAndSize (
    IN  CHAR16   *Name,
    IN  EFI_GUID *VendorGuid,
    OUT UINTN    *VariableSize
) {
    EFI_STATUS  Status;
    UINTN       BufferSize;
    VOID       *Buffer;

    Buffer = NULL;

    // Pass in a zero size buffer to find the required buffer size.
    BufferSize = 0;
    Status = REFIT_CALL_5_WRAPPER(
        gRT->GetVariable, Name,
        VendorGuid, NULL,
        &BufferSize, Buffer
    );
    if (Status == EFI_BUFFER_TOO_SMALL) {
        // Allocate the buffer to return
        Buffer = AllocateZeroPool (BufferSize);

        if (Buffer == NULL) {
            return NULL;
        }

        // Read variable into the allocated buffer.
        Status = REFIT_CALL_5_WRAPPER(
            gRT->GetVariable, Name,
            VendorGuid, NULL,
            &BufferSize, Buffer
        );
        if (EFI_ERROR(Status)) {
            BufferSize = 0;
        }
    }

    *VariableSize = BufferSize;

    return Buffer;
} // VOID * BdsLibGetVariableAndSize()
