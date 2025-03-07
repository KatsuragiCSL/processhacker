/*
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 *
 * Authors:
 *
 *     jxy-s   2022
 *
 */

#include <kph.h>
#include <comms.h>
#include <dyndata.h>
#include <kphmsgdyn.h>

#include <trace.h>

static PKPH_NPAGED_LOOKASIDE_OBJECT KphpProcesCreateApcLookaside = NULL;

PAGED_FILE();

typedef struct _KPH_PROCESS_CREATE_APC
{
    KSI_KAPC Apc;
    PKPH_THREAD_CONTEXT Actor;

} KPH_PROCESS_CREATE_APC, *PKPH_PROCESS_CREATE_APC;

/**
 * \brief Allocates the process create APC which is used to track when a thread
 * is actively in the kernel creating a process.
 *
 * \return Pointer to allocates process create APC, null on failure.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Return_allocatesMem_
PKPH_PROCESS_CREATE_APC KphpAllocateProcessCreateApc(
    VOID
    )
{
    PKPH_PROCESS_CREATE_APC apc;

    PAGED_PASSIVE();

    NT_ASSERT(KphpProcesCreateApcLookaside);

    apc = KphAllocateFromNPagedLookasideObject(KphpProcesCreateApcLookaside);
    if (apc)
    {
        KphReferenceObject(KphpProcesCreateApcLookaside);
    }

    return apc;
}

/**
 * \brief Frees a previously allocated process create APC.
 *
 * \param[in] Apc The process create APC to free.
 */
_IRQL_requires_max_(APC_LEVEL)
VOID KphpFreeProcessCreateApc(
    _In_freesMem_ PKPH_PROCESS_CREATE_APC Apc
    )
{
    PAGED_CODE();

    NT_ASSERT(KphpProcesCreateApcLookaside);

    KphFreeToNPagedLookasideObject(KphpProcesCreateApcLookaside, Apc);
    KphDereferenceObject(KphpProcesCreateApcLookaside);
}

/**
 * \brief Performing process tracking.
 *
 * \param[in] Process The process object from the notification callback.
 * \param[in] ProcessId Process ID from the notification callback.
 * \param[in] CreateInfo The create information from the callback.
 *
 * \return Pointer to the process context, may be null, the caller should
 * dereference this object if it is non-null.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
PKPH_PROCESS_CONTEXT KphpPerformProcessTracking(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    NTSTATUS status;
    PKPH_PROCESS_CONTEXT process;
    PKPH_PROCESS_CONTEXT creatorProcess;
    KPH_PROCESS_STATE processState;

    PAGED_PASSIVE();

    if (!CreateInfo)
    {
        process = KphUntrackProcessContext(ProcessId);
        if (!process)
        {
            return NULL;
        }

        KphTracePrint(TRACE_LEVEL_VERBOSE,
                      TRACKING,
                      "Stopped tracking process %lu",
                      HandleToULong(process->ProcessId));

        process->ExitNotification = TRUE;

        NT_ASSERT(process->NumberOfThreads == 0);
        NT_ASSERT(IsListEmpty(&process->ThreadListHead));

        return process;
    }

    NT_ASSERT(CreateInfo);

    process = KphTrackProcessContext(Process);
    if (!process)
    {
        KphTracePrint(TRACE_LEVEL_ERROR,
                      TRACKING,
                      "Failed to track process %lu",
                      HandleToULong(ProcessId));

        return NULL;
    }

    process->CreateNotification = TRUE;
    process->CreatorClientId.UniqueProcess = PsGetCurrentProcessId();
    process->CreatorClientId.UniqueThread = PsGetCurrentThreadId();

    KphTracePrint(TRACE_LEVEL_VERBOSE,
                  TRACKING,
                  "Tracking process %lu",
                  HandleToULong(process->ProcessId));

    processState = KphGetProcessState(process);
    if ((processState & KPH_PROCESS_STATE_LOW) == KPH_PROCESS_STATE_LOW)
    {
        status = KphStartProtectingProcess(process,
                                           KPH_PROTECED_PROCESS_MASK,
                                           KPH_PROTECED_THREAD_MASK);
        if (!NT_SUCCESS(status))
        {
            KphTracePrint(TRACE_LEVEL_ERROR,
                          PROTECTION,
                          "KphStartProtectingProcess failed: %!STATUS!",
                          status);

            NT_ASSERT(!process->Protected);
        }
    }

    creatorProcess = KphGetCurrentProcessContext();
    if (!creatorProcess)
    {
        return process;
    }

    processState = KphGetProcessState(creatorProcess);
    if ((processState & KPH_PROCESS_STATE_HIGH) == KPH_PROCESS_STATE_HIGH)
    {
        process->SecurelyCreated = TRUE;
    }

    KphDereferenceObject(creatorProcess);

    return process;
}

/**
 * \brief Informs any clients of process notify routine invocations.
 *
 * \param[in,out] Process The process being created.
 * \param[in] ProcessId ProcessID of the process being created.
 * \param[in,out] CreateInfo Information on the process being created, if the
 * process is being destroyed this is NULL.
 *
 */
_Function_class_(PCREATE_PROCESS_NOTIFY_ROUTINE_EX)
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID KphpCreateProcessNotifyInformer(
    _In_ PKPH_PROCESS_CONTEXT Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    NTSTATUS status;
    PKPH_MESSAGE msg;
    PKPH_MESSAGE reply;

    PAGED_PASSIVE();

    msg = NULL;
    reply = NULL;

    if (CreateInfo)
    {
        if (!KphInformerSettings.ProcessCreate)
        {
            goto Exit;
        }

        msg = KphAllocateMessage();
        if (!msg)
        {
            KphTracePrint(TRACE_LEVEL_ERROR,
                          INFORMER,
                          "Failed to allocate message");
            goto Exit;
        }

        reply = KphAllocateMessage();
        if (!reply)
        {
            KphTracePrint(TRACE_LEVEL_ERROR,
                          INFORMER,
                          "Failed to allocate message");
            goto Exit;
        }

        KphMsgInit(msg, KphMsgProcessCreate);
        msg->Kernel.ProcessCreate.CreatingClientId.UniqueProcess = PsGetCurrentProcessId();
        msg->Kernel.ProcessCreate.CreatingClientId.UniqueThread = PsGetCurrentProcessId();
        msg->Kernel.ProcessCreate.ParentProcessId = CreateInfo->ParentProcessId;
        msg->Kernel.ProcessCreate.TargetProcessId = ProcessId;
        msg->Kernel.ProcessCreate.IsSubsystemProcess = (CreateInfo->IsSubsystemProcess ? TRUE : FALSE);

        if (Process->ImageFileName)
        {
            status = KphMsgDynAddUnicodeString(msg,
                                               KphMsgFieldFileName,
                                               Process->ImageFileName);
            if (!NT_SUCCESS(status))
            {
                KphTracePrint(TRACE_LEVEL_WARNING,
                              INFORMER,
                              "KphMsgDynAddUnicodeString failed: %!STATUS!",
                              status);
            }
        }

        if (CreateInfo->CommandLine)
        {
            status = KphMsgDynAddUnicodeString(msg,
                                               KphMsgFieldCommandLine,
                                               CreateInfo->CommandLine);
            if (!NT_SUCCESS(status))
            {
                KphTracePrint(TRACE_LEVEL_WARNING,
                              INFORMER,
                              "KphMsgDynAddUnicodeString failed: %!STATUS!",
                              status);
            }
        }

        status = KphCommsSendMessage(msg, reply, KPH_COMMS_DEFAULT_TIMEOUT);
        if (!NT_SUCCESS(status))
        {
            KphTracePrint(TRACE_LEVEL_ERROR,
                          INFORMER,
                          "Failed to send message (%lu): %!STATUS!",
                          (ULONG)msg->Header.MessageId,
                          status);

            goto Exit;
        }

        if (NT_VERIFY(reply->Header.MessageId == KphMsgProcessCreate))
        {
            CreateInfo->CreationStatus = reply->Reply.ProcessCreate.CreationStatus;
        }
    }
    else
    {
        if (!KphInformerSettings.ProcessExit)
        {
            goto Exit;
        }

        msg = KphAllocateMessage();
        if (!msg)
        {
            KphTracePrint(TRACE_LEVEL_ERROR,
                          INFORMER,
                          "Failed to allocate message");
            goto Exit;
        }

        KphMsgInit(msg, KphMsgProcessExit);
        msg->Kernel.ProcessExit.ExitingClientId.UniqueProcess = PsGetCurrentProcessId();
        msg->Kernel.ProcessExit.ExitingClientId.UniqueThread = PsGetCurrentThreadId();
        msg->Kernel.ProcessExit.ExitStatus = PsGetProcessExitStatus(Process->EProcess);

        KphCommsSendMessageAsync(msg);
        msg = NULL;
    }

Exit:

    if (reply)
    {
        KphFreeMessage(reply);
    }

    if (msg)
    {
        KphFreeMessage(msg);
    }
}

/**
 * \brief Cleanup routine for the create process APC.
 *
 * \param[in] Apc The APC to clean up.
 * \param[in] Reason Unused.
 */
_Function_class_(KSI_KCLEANUP_ROUTINE)
_IRQL_requires_min_(PASSIVE_LEVEL)
_IRQL_requires_max_(APC_LEVEL)
_IRQL_requires_same_
VOID
KSIAPI
KphpProcessCreateCleanupRoutine(
    _In_ PKSI_KAPC Apc,
    _In_ KSI_KAPC_CLEANUP_REASON Reason
    )
{
    PKPH_PROCESS_CREATE_APC apc;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Reason);

    apc = CONTAINING_RECORD(Apc, KPH_PROCESS_CREATE_APC, Apc);

    KphDereferenceObject(apc->Actor);
    KphpFreeProcessCreateApc(apc);
}

/**
 * \brief Performs process creation actions at return from system.
 *
 * \details We use an APC to do state tracking in the current thread context.
 * This enables us to identify when the thread is currently in the kernel
 * creating a process. In this kernel routine we clear state from the acting
 * thread before it returns from the system.
 *
 * \param[in] Apc The APC that is executing.
 * \param[in] NormalRoutine Set to NULL to cancel the faked routine.
 * \param[in,out] NormalContext Unused.
 * \param[in,out] SystemArgument1 Unused.
 * \param[in,out] SystemArgument2 Unused.
 */
_Function_class_(KSI_KKERNEL_ROUTINE)
_IRQL_requires_(APC_LEVEL)
_IRQL_requires_same_
VOID KSIAPI KphpProcessCreateKernelRoutine(
    _In_ PKSI_KAPC Apc,
    _Inout_ _Deref_pre_maybenull_ PKNORMAL_ROUTINE* NormalRoutine,
    _Inout_ _Deref_pre_maybenull_ PVOID* NormalContext,
    _Inout_ _Deref_pre_maybenull_ PVOID* SystemArgument1,
    _Inout_ _Deref_pre_maybenull_ PVOID* SystemArgument2
    )
{
    PKPH_PROCESS_CREATE_APC apc;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(NormalContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    apc = CONTAINING_RECORD(Apc, KPH_PROCESS_CREATE_APC, Apc);

    //
    // Cancel the faked routine.
    //
    *NormalRoutine = NULL;

    NT_ASSERT(apc->Actor->IsCreatingProcess);

    apc->Actor->IsCreatingProcess = FALSE;
    apc->Actor->IsCreatingProcessId = NULL;
}

/**
 * \brief Performs tracking of the actor thread creating a process.
 *
 * \details We use an APC to do state tracking in the current thread context.
 * This enables us to identify when the thread is currently in the kernel
 * creating a process. This will be cleared when the kernel routine fires
 * before returning from the system.
 *
 * \param[in] ProcessId ProcessID of the process being created.
 * \param[in,out] CreateInfo Information on the process being created, if the
 * process is being destroyed this is NULL.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID KphpPerformProcessCreationTracking(
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    PKPH_THREAD_CONTEXT actor;
    PKPH_PROCESS_CREATE_APC apc;

    PAGED_PASSIVE();

    if (!CreateInfo)
    {
        return;
    }

    NT_ASSERT(PsGetCurrentProcessId() == CreateInfo->CreatingThreadId.UniqueProcess);
    NT_ASSERT(PsGetCurrentThreadId() == CreateInfo->CreatingThreadId.UniqueThread);

    actor = KphGetCurrentThreadContext();
    if (!actor)
    {
        KphTracePrint(TRACE_LEVEL_ERROR,
                      TRACKING,
                      "KphGetCurrentThreadContext returned null");

        return;
    }

    apc = KphpAllocateProcessCreateApc();
    if (!apc)
    {
        KphTracePrint(TRACE_LEVEL_ERROR,
                      TRACKING,
                      "Failed to allocate create process APC");

        KphDereferenceObject(actor);
        return;
    }

    apc->Actor = actor;
    actor->IsCreatingProcess = TRUE;
    actor->IsCreatingProcessId = ProcessId;

    //
    // Fake a normal routine do the APC mode is honored, we'll cancel it later.
    //
    KsiInitializeApc(&apc->Apc,
                     KphDriverObject,
                     KeGetCurrentThread(),
                     OriginalApcEnvironment,
                     KphpProcessCreateKernelRoutine,
                     KphpProcessCreateCleanupRoutine,
                     (PKNORMAL_ROUTINE)(PVOID)1,
                     UserMode,
                     NULL);

    if (KsiInsertQueueApc(&apc->Apc, NULL, NULL, IO_NO_INCREMENT))
    {
        //
        // Stage the thread for user mode APC delivery.
        //
        KeTestAlertThread(UserMode);
        return;
    }

    KphTracePrint(TRACE_LEVEL_ERROR, TRACKING, "KsiInsertQueueApc failed");

    //
    // No choice other than to reset the actor state immediately.
    //
    actor->IsCreatingProcess = FALSE;
    actor->IsCreatingProcessId = NULL;

    KphDereferenceObject(actor);
    KphpFreeProcessCreateApc(apc);
}

/**
 * \brief Process create notify routine.
 *
 * \param[in,out] Process The process object being created.
 * \param[in] ProcessId ProcessID of the process being created.
 * \param[in,out] CreateInfo Information on the process being created, if the
 * process is being destroyed this is NULL.
 */
_Function_class_(PCREATE_PROCESS_NOTIFY_ROUTINE_EX)
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID KphpCreateProcessNotifyRoutine(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    PKPH_PROCESS_CONTEXT process;

    PAGED_PASSIVE();

    KphpPerformProcessCreationTracking(ProcessId, CreateInfo);

    process = KphpPerformProcessTracking(Process, ProcessId, CreateInfo);
    if (process)
    {
        KphpCreateProcessNotifyInformer(process, ProcessId, CreateInfo);
        KphDereferenceObject(process);
    }
}

/**
 * \brief Starts the process informer.
 *
 * \return Successful or errant status.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
_Must_inspect_result_
NTSTATUS KphProcessInformerStart(
    VOID
    )
{
    NTSTATUS status;

    PAGED_PASSIVE();

    status = KphCreateNPagedLookasideObject(&KphpProcesCreateApcLookaside,
                                            sizeof(KPH_PROCESS_CREATE_APC),
                                            KPH_TAG_PROCESS_CREATE_APC);
    if (!NT_SUCCESS(status))
    {
        KphTracePrint(TRACE_LEVEL_ERROR,
                      INFORMER,
                      "KphCreateNPagedLookasideObject failed: %!STATUS!",
                      status);

        KphpProcesCreateApcLookaside = NULL;
        goto Exit;
    }

    if (KphDynPsSetCreateProcessNotifyRoutineEx2)
    {
        status = KphDynPsSetCreateProcessNotifyRoutineEx2(PsCreateProcessNotifySubsystems,
                                                          (PVOID)KphpCreateProcessNotifyRoutine,
                                                          FALSE);
        if (!NT_SUCCESS(status))
        {
            KphTracePrint(TRACE_LEVEL_ERROR,
                          INFORMER,
                          "Failed to register process notify routine: %!STATUS!",
                          status);

            goto Exit;
        }
    }
    else
    {
        status = PsSetCreateProcessNotifyRoutineEx(KphpCreateProcessNotifyRoutine,
                                                   FALSE);
        if (!NT_SUCCESS(status))
        {
            KphTracePrint(TRACE_LEVEL_ERROR,
                          INFORMER,
                          "Failed to register process notify routine: %!STATUS!",
                          status);

            goto Exit;
        }
    }

Exit:

    if (!NT_SUCCESS(status))
    {
        if (KphpProcesCreateApcLookaside)
        {
            KphDereferenceObject(KphpProcesCreateApcLookaside);
            KphpProcesCreateApcLookaside = NULL;
        }
    }

    return status;
}

/**
 * \brief Stops the process informer.
 *
 * \return Successful or errant status.
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID KphProcessInformerStop(
    VOID
    )
{
    PAGED_PASSIVE();

    if (!KphpProcesCreateApcLookaside)
    {
        return;
    }

    if (KphDynPsSetCreateProcessNotifyRoutineEx2)
    {
        KphDynPsSetCreateProcessNotifyRoutineEx2(PsCreateProcessNotifySubsystems,
                                                 (PVOID)KphpCreateProcessNotifyRoutine,
                                                 TRUE);
    }
    else
    {
        PsSetCreateProcessNotifyRoutineEx(KphpCreateProcessNotifyRoutine, TRUE);
    }

    KphDereferenceObject(KphpProcesCreateApcLookaside);
}
