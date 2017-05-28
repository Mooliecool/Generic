/*++

Copyright (c) 1989  Microsoft Corporation

Module Name:

    LockCtrl.c

Abstract:

    This module implements the Lock Control routines for Udfs called
    by the Fsd/Fsp dispatch driver.

Author:

    Dan Lovinger    [DanLo]     20-Jan-1997

Revision History:

--*/

#include "UdfProcs.h"

//
//  The Bug check file id for this module
//

#define BugCheckFileId                   (UDFS_BUG_CHECK_LOCKCTRL)

//
//  The local debug trace level
//

#define Dbg                              (UDFS_DEBUG_LEVEL_LOCKCTRL)

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, UdfCommonLockControl)
#pragma alloc_text(PAGE, UdfFastLock)
#pragma alloc_text(PAGE, UdfFastUnlockAll)
#pragma alloc_text(PAGE, UdfFastUnlockAllByKey)
#pragma alloc_text(PAGE, UdfFastUnlockSingle)
#endif


NTSTATUS
UdfCommonLockControl (
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )

/*++

Routine Description:

    This is the common routine for Lock Control called by both the fsd and fsp
    threads.

Arguments:

    Irp - Supplies the Irp to process

Return Value:

    NTSTATUS - The return status for the operation

--*/

{
    NTSTATUS Status;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation( Irp );

    TYPE_OF_OPEN TypeOfOpen;
    PFCB Fcb;
    PCCB Ccb;

    PAGED_CODE();

    //
    //  Extract and decode the type of file object we're being asked to process
    //

    TypeOfOpen = UdfDecodeFileObject( IrpSp->FileObject, &Fcb, &Ccb );

    //
    //  If the file is not a user file open then we reject the request
    //  as an invalid parameter
    //

    if (TypeOfOpen != UserFileOpen) {

        UdfCompleteRequest( IrpContext, Irp, STATUS_INVALID_PARAMETER );
        return STATUS_INVALID_PARAMETER;
    }

    //
    //  We check whether we can proceed based on the state of the file oplocks.
    //  This call might post the irp for us.
    //

    Status = FsRtlCheckOplock( &Fcb->Oplock,
                               Irp,
                               IrpContext,
                               UdfOplockComplete,
                               NULL );

    //
    //  If we don't get success then the oplock package completed the request.
    //

    if (Status != STATUS_SUCCESS) {

        return Status;
    }

    //
    //  Verify the Fcb.
    //

    UdfVerifyFcbOperation( IrpContext, Fcb );

    //
    //  If we don't have a file lock, then get one now.
    //

    if (Fcb->FileLock == NULL) { UdfCreateFileLock( IrpContext, Fcb, TRUE ); }

    //
    //  Now call the FsRtl routine to do the actual processing of the
    //  Lock request
    //

    Status = FsRtlProcessFileLock( Fcb->FileLock, Irp, NULL );

    //
    //  Set the flag indicating if Fast I/O is possible
    //

    UdfLockFcb( IrpContext, Fcb );
    Fcb->IsFastIoPossible = UdfIsFastIoPossible( Fcb );
    UdfUnlockFcb( IrpContext, Fcb );

    //
    //  Complete the request.
    //

    UdfCompleteRequest( IrpContext, NULL, Status );
    return Status;
}


BOOLEAN
UdfFastLock (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    BOOLEAN FailImmediately,
    BOOLEAN ExclusiveLock,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This is a call back routine for doing the fast lock call.

Arguments:

    FileObject - Supplies the file object used in this operation

    FileOffset - Supplies the file offset used in this operation

    Length - Supplies the length used in this operation

    ProcessId - Supplies the process ID used in this operation

    Key - Supplies the key used in this operation

    FailImmediately - Indicates if the request should fail immediately
        if the lock cannot be granted.

    ExclusiveLock - Indicates if this is a request for an exclusive or
        shared lock

    IoStatus - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.

--*/

{
    BOOLEAN Results = FALSE;

    PFCB Fcb;
    TYPE_OF_OPEN TypeOfOpen;

    PAGED_CODE();

    ASSERT_FILE_OBJECT( FileObject );

    IoStatus->Information = 0;

    //
    //  Decode the type of file object we're being asked to process and
    //  make sure that is is only a user file open.
    //

    TypeOfOpen = UdfFastDecodeFileObject( FileObject, &Fcb );

    if (TypeOfOpen != UserFileOpen) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //
    //  Only deal with 'good' Fcb's.
    //

    if (!UdfVerifyFcbOperation( NULL, Fcb )) {

        return FALSE;
    }

    FsRtlEnterFileSystem();

    //
    //  Use a try-finally to facilitate cleanup.
    //

    try {

        //
        //  We check whether we can proceed based on the state of the file oplocks.
        //

        if ((Fcb->Oplock != NULL) && !FsRtlOplockIsFastIoPossible( &Fcb->Oplock )) {

            try_leave( NOTHING );
        }

        //
        //  If we don't have a file lock, then get one now.
        //

        if ((Fcb->FileLock == NULL) && !UdfCreateFileLock( NULL, Fcb, FALSE )) {

            try_leave( NOTHING );
        }

        //
        //  Now call the FsRtl routine to perform the lock request.
        //

        if (Results = FsRtlFastLock( Fcb->FileLock,
                                     FileObject,
                                     FileOffset,
                                     Length,
                                     ProcessId,
                                     Key,
                                     FailImmediately,
                                     ExclusiveLock,
                                     IoStatus,
                                     NULL,
                                     FALSE )) {

            //
            //  Set the flag indicating if Fast I/O is questionable.  We
            //  only change this flag if the current state is possible.
            //  Retest again after synchronizing on the header.
            //

            if (Fcb->IsFastIoPossible == FastIoIsPossible) {

                UdfLockFcb( NULL, Fcb );
                Fcb->IsFastIoPossible = UdfIsFastIoPossible( Fcb );
                UdfUnlockFcb( NULL, Fcb );
            }
        }

    } finally {

        FsRtlExitFileSystem();
    }

    return Results;
}


BOOLEAN
UdfFastUnlockSingle (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This is a call back routine for doing the fast unlock single call.

Arguments:

    FileObject - Supplies the file object used in this operation

    FileOffset - Supplies the file offset used in this operation

    Length - Supplies the length used in this operation

    ProcessId - Supplies the process ID used in this operation

    Key - Supplies the key used in this operation

    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.

--*/

{
    BOOLEAN Results = FALSE;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB Fcb;

    PAGED_CODE();

    IoStatus->Information = 0;

    //
    //  Decode the type of file object we're being asked to process and
    //  make sure that is is only a user file open.
    //

    TypeOfOpen = UdfFastDecodeFileObject( FileObject, &Fcb );

    if (TypeOfOpen != UserFileOpen) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //
    //  Only deal with 'good' Fcb's.
    //

    if (!UdfVerifyFcbOperation( NULL, Fcb )) {

        return FALSE;
    }

    //
    //  If there is no lock then return immediately.
    //

    if (Fcb->FileLock == NULL) {

        IoStatus->Status = STATUS_RANGE_NOT_LOCKED;
        return TRUE;
    }

    FsRtlEnterFileSystem();

    try {

        //
        //  We check whether we can proceed based on the state of the file oplocks.
        //

        if ((Fcb->Oplock != NULL) && !FsRtlOplockIsFastIoPossible( &Fcb->Oplock )) {

            try_leave( NOTHING );
        }

        //
        //  If we don't have a file lock, then get one now.
        //

        if ((Fcb->FileLock == NULL) && !UdfCreateFileLock( NULL, Fcb, FALSE )) {

            try_leave( NOTHING );
        }

        //
        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request.  The call will always succeed.
        //

        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockSingle( Fcb->FileLock,
                                                  FileObject,
                                                  FileOffset,
                                                  Length,
                                                  ProcessId,
                                                  Key,
                                                  NULL,
                                                  FALSE );

        //
        //  Set the flag indicating if Fast I/O is possible.  We are
        //  only concerned if there are no longer any filelocks on this
        //  file.
        //

        if (!FsRtlAreThereCurrentFileLocks( Fcb->FileLock ) &&
            (Fcb->IsFastIoPossible != FastIoIsPossible)) {

            UdfLockFcb( IrpContext, Fcb );
            Fcb->IsFastIoPossible = UdfIsFastIoPossible( Fcb );
            UdfUnlockFcb( IrpContext, Fcb );
        }

    } finally {

        FsRtlExitFileSystem();
    }

    return Results;
}


BOOLEAN
UdfFastUnlockAll (
    IN PFILE_OBJECT FileObject,
    PEPROCESS ProcessId,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This is a call back routine for doing the fast unlock all call.

Arguments:

    FileObject - Supplies the file object used in this operation

    ProcessId - Supplies the process ID used in this operation

    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.

--*/

{
    BOOLEAN Results = FALSE;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB Fcb;

    PAGED_CODE();

    IoStatus->Information = 0;

    //
    //  Decode the type of file object we're being asked to process and
    //  make sure that is is only a user file open.
    //

    TypeOfOpen = UdfFastDecodeFileObject( FileObject, &Fcb );

    if (TypeOfOpen != UserFileOpen) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //
    //  Only deal with 'good' Fcb's.
    //

    if (!UdfVerifyFcbOperation( NULL, Fcb )) {

        return FALSE;
    }

    //
    //  If there is no lock then return immediately.
    //

    if (Fcb->FileLock == NULL) {

        IoStatus->Status = STATUS_RANGE_NOT_LOCKED;
        return TRUE;
    }

    FsRtlEnterFileSystem();

    try {

        //
        //  We check whether we can proceed based on the state of the file oplocks.
        //

        if ((Fcb->Oplock != NULL) && !FsRtlOplockIsFastIoPossible( &Fcb->Oplock )) {

            try_leave( NOTHING );
        }

        //
        //  If we don't have a file lock, then get one now.
        //

        if ((Fcb->FileLock == NULL) && !UdfCreateFileLock( NULL, Fcb, FALSE )) {

            try_leave( NOTHING );
        }

        //
        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request.  The call will always succeed.
        //

        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockAll( Fcb->FileLock,
                                               FileObject,
                                               ProcessId,
                                               NULL );


        //
        //  Set the flag indicating if Fast I/O is possible
        //

        UdfLockFcb( IrpContext, Fcb );
        Fcb->IsFastIoPossible = UdfIsFastIoPossible( Fcb );
        UdfUnlockFcb( IrpContext, Fcb );

    } finally {

        FsRtlExitFileSystem();
    }

    return Results;
}


BOOLEAN
UdfFastUnlockAllByKey (
    IN PFILE_OBJECT FileObject,
    PVOID ProcessId,
    ULONG Key,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    This is a call back routine for doing the fast unlock all by key call.

Arguments:

    FileObject - Supplies the file object used in this operation

    ProcessId - Supplies the process ID used in this operation

    Key - Supplies the key used in this operation

    Status - Receives the Status if this operation is successful

Return Value:

    BOOLEAN - TRUE if this operation completed and FALSE if caller
        needs to take the long route.

--*/

{
    BOOLEAN Results = FALSE;
    TYPE_OF_OPEN TypeOfOpen;
    PFCB Fcb;

    PAGED_CODE();

    IoStatus->Information = 0;

    //
    //  Decode the type of file object we're being asked to process and
    //  make sure that is is only a user file open.
    //

    TypeOfOpen = UdfFastDecodeFileObject( FileObject, &Fcb );

    if (TypeOfOpen != UserFileOpen) {

        IoStatus->Status = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    //
    //  Only deal with 'good' Fcb's.
    //

    if (!UdfVerifyFcbOperation( NULL, Fcb )) {

        return FALSE;
    }

    //
    //  If there is no lock then return immediately.
    //

    if (Fcb->FileLock == NULL) {

        IoStatus->Status = STATUS_RANGE_NOT_LOCKED;
        return TRUE;
    }

    FsRtlEnterFileSystem();

    try {

        //
        //  We check whether we can proceed based on the state of the file oplocks.
        //

        if ((Fcb->Oplock != NULL) && !FsRtlOplockIsFastIoPossible( &Fcb->Oplock )) {

            try_leave( NOTHING );
        }

        //
        //  If we don't have a file lock, then get one now.
        //

        if ((Fcb->FileLock == NULL) && !UdfCreateFileLock( NULL, Fcb, FALSE )) {

            try_leave( NOTHING );
        }

        //
        //  Now call the FsRtl routine to do the actual processing of the
        //  Lock request.  The call will always succeed.
        //

        Results = TRUE;
        IoStatus->Status = FsRtlFastUnlockAllByKey( Fcb->FileLock,
                                                    FileObject,
                                                    ProcessId,
                                                    Key,
                                                    NULL );


        //
        //  Set the flag indicating if Fast I/O is possible
        //

        UdfLockFcb( IrpContext, Fcb );
        Fcb->IsFastIoPossible = UdfIsFastIoPossible( Fcb );
        UdfUnlockFcb( IrpContext, Fcb );

    } finally {

        FsRtlExitFileSystem();
    }

    return Results;
}


