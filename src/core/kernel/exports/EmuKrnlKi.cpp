// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// *  (c) 2016 Patrick van Logchem <pvanlogchem@gmail.com>
// *  (c) 2019 ergo720
// *
// *  All rights reserved
// *
// ******************************************************************

// Acknowledgment (timer functions): ReactOS (GPLv2)
// https://github.com/reactos/reactos

// KiCheckTimerTable, KiTimerExpiration, KiTimerListExpire
/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            ntoskrnl/ke/dpc.c
* PURPOSE:         Deferred Procedure Call (DPC) Support
* PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
*                  Philip Susi (phreak@iag.net)
*                  Eric Kohl
*/

// KiInsertTreeTimer, KiInsertTimerTable, KiSignalTimer, KiCompleteTimer
/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            ntoskrnl/ke/timerobj.c
* PURPOSE:         Handle Kernel Timers (Kernel-part of Executive Timers)
* PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
*/

// KiComputeDueTime, KiComputeTimerTableIndex, KxInsertTimer, KxRemoveTreeTimer, KiRemoveEntryTimer
/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            ntoskrnl/include/internal/ke_x.h
* PURPOSE:         Internal Inlined Functions for the Kernel
* PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
*/

// COPYING file:
/*
GNU GENERAL PUBLIC LICENSE
Version 2, June 1991

ReactOS may be used, runtime linked, and distributed with non-free software
(meaning that such software has no obligations to open-source, or render
free, their non-free code) such as commercial device drivers and commercial
applications. This exception does not alter any other responsibilities of
the licensee under the GPL (meaning that such software must still obey
the GPL for the free ("open-sourced") code that has been integrated into
the said software).
*/

// Changed from ReactOS: slight changes to the Hand parameter usage


#define LOG_PREFIX CXBXR_MODULE::KI


#include <core\kernel\exports\xboxkrnl.h> // For KeBugCheck, etc.
#include "core\kernel\support\EmuFS.h"
#include "Logging.h" // For LOG_FUNC()
#include "EmuKrnl.h" // for the list support functions
#include "EmuKrnlKi.h"

#define MAX_TIMER_DPCS   16

#define ASSERT_TIMER_LOCKED assert(KiTimerMtx.Acquired > 0)

xbox::KPROCESS KiUniqueProcess;
const xbox::ulong_xt CLOCK_TIME_INCREMENT = 0x2710;
xbox::KDPC KiTimerExpireDpc;
xbox::KI_TIMER_LOCK KiTimerMtx;
xbox::KTIMER_TABLE_ENTRY KiTimerTableListHead[TIMER_TABLE_SIZE];
xbox::LIST_ENTRY KiWaitInListHead;
std::mutex xbox::KiApcListMtx;


xbox::void_xt xbox::KiInitSystem()
{
	KiUniqueProcess.StackCount = 0;
	KiUniqueProcess.ThreadQuantum = X_THREAD_QUANTUM;
	InitializeListHead(&KiUniqueProcess.ThreadListHead);

	InitializeListHead(&KiWaitInListHead);

	KiTimerMtx.Acquired = 0;
	KeInitializeDpc(&KiTimerExpireDpc, KiTimerExpiration, NULL);
	for (unsigned i = 0; i < TIMER_TABLE_SIZE; i++) {
		InitializeListHead(&KiTimerTableListHead[i].Entry);
		KiTimerTableListHead[i].Time.u.HighPart = 0xFFFFFFFF;
		KiTimerTableListHead[i].Time.u.LowPart = 0;
	}

	InitializeListHead(&IdexChannelObject.DeviceQueue.DeviceListHead);
}

xbox::void_xt xbox::KiTimerLock()
{
	KiTimerMtx.Mtx.lock();
	KiTimerMtx.Acquired++;
}

xbox::void_xt xbox::KiTimerUnlock()
{
	KiTimerMtx.Acquired--;
	KiTimerMtx.Mtx.unlock();
}

xbox::void_xt xbox::KiClockIsr
(
	unsigned int ScalingFactor
)
{
	KIRQL OldIrql;
	LARGE_INTEGER InterruptTime;
	LARGE_INTEGER HostSystemTime;
	ULONG Hand;
	DWORD OldKeTickCount;

	OldIrql = KfRaiseIrql(CLOCK_LEVEL);

	// Update the interrupt time
	InterruptTime.u.LowPart = KeInterruptTime.LowPart;
	InterruptTime.u.HighPart = KeInterruptTime.High1Time;
	InterruptTime.QuadPart += (CLOCK_TIME_INCREMENT * ScalingFactor);
	KeInterruptTime.High2Time = InterruptTime.u.HighPart;
	KeInterruptTime.LowPart = InterruptTime.u.LowPart;
	KeInterruptTime.High1Time = InterruptTime.u.HighPart;

	// Update the system time
	// NOTE: I'm not sure if we should round down the host system time to the nearest multiple
	// of the Xbox clock increment...
	GetSystemTimeAsFileTime((LPFILETIME)&HostSystemTime);
	HostSystemTime.QuadPart += HostSystemTimeDelta.load();
	KeSystemTime.High2Time = HostSystemTime.u.HighPart;
	KeSystemTime.LowPart = HostSystemTime.u.LowPart;
	KeSystemTime.High1Time = HostSystemTime.u.HighPart;

	// Update the tick counter
	OldKeTickCount = KeTickCount;
	KeTickCount += ScalingFactor;

	// Because this function must be fast to continuously update the kernel clocks, if somebody else is currently
	// holding the lock, we won't wait and instead skip the check of the timers for this cycle
	if (KiTimerMtx.Mtx.try_lock()) {
		KiTimerMtx.Acquired++;
		// Check if a timer has expired
		Hand = OldKeTickCount & (TIMER_TABLE_SIZE - 1);
		if (KiTimerTableListHead[Hand].Entry.Flink != &KiTimerTableListHead[Hand].Entry &&
			(ULONGLONG)InterruptTime.QuadPart >= KiTimerTableListHead[Hand].Time.QuadPart) {
			KeInsertQueueDpc(&KiTimerExpireDpc, (PVOID)Hand, 0);
		}
		KiTimerMtx.Acquired--;
		KiTimerMtx.Mtx.unlock();
	}

	KfLowerIrql(OldIrql);
}

xbox::void_xt NTAPI xbox::KiCheckTimerTable
(
	IN xbox::ULARGE_INTEGER CurrentTime
)
{
	ULONG i = 0;
	PLIST_ENTRY ListHead, NextEntry;
	KIRQL OldIrql;
	PKTIMER Timer;

	ASSERT_TIMER_LOCKED;

	/* Raise IRQL to high and loop timers */
	OldIrql = KfRaiseIrql(HIGH_LEVEL);
	do
	{
		/* Loop the current list */
		ListHead = &KiTimerTableListHead[i].Entry;
		NextEntry = ListHead->Flink;
		while (NextEntry != ListHead)
		{
			/* Get the timer and move to the next one */
			Timer = CONTAINING_RECORD(NextEntry, KTIMER, TimerListEntry);
			NextEntry = NextEntry->Flink;

			/* Check if it expired */
			if (Timer->DueTime.QuadPart <= CurrentTime.QuadPart)
			{
				/* This is bad, breakpoint! */
				EmuLog(LOG_LEVEL::ERROR2, "Invalid timer state!");
				DbgBreakPoint();
			}
		}

		/* Move to the next timer */
		i++;
	} while (i < TIMER_TABLE_SIZE);

	/* Lower IRQL and return */
	KfLowerIrql(OldIrql);
}

xbox::void_xt xbox::KxInsertTimer
(
	IN xbox::PKTIMER Timer,
	IN xbox::ulong_xt Hand
)
{
	ASSERT_TIMER_LOCKED;

	/* Try to insert the timer */
	if (KiInsertTimerTable(Timer, Hand))
	{
		/* Complete it */
		KiCompleteTimer(Timer, Hand);
	}
}

xbox::void_xt FASTCALL xbox::KiCompleteTimer
(
	IN xbox::PKTIMER Timer,
	IN xbox::ulong_xt Hand
)
{
	LIST_ENTRY ListHead;
	BOOLEAN RequestInterrupt = FALSE;

	ASSERT_TIMER_LOCKED;

	/* Remove it from the timer list */
	KiRemoveEntryTimer(Timer, Hand);

	/* Link the timer list to our stack */
	ListHead.Flink = &Timer->TimerListEntry;
	ListHead.Blink = &Timer->TimerListEntry;
	Timer->TimerListEntry.Flink = &ListHead;
	Timer->TimerListEntry.Blink = &ListHead;

	/* Signal the timer if it's still on our list */
	if (!IsListEmpty(&ListHead)) {
		RequestInterrupt = KiSignalTimer(Timer);
	}

	/* Request a DPC if needed */
	if (RequestInterrupt) {
		HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
	}
}

xbox::void_xt xbox::KiRemoveEntryTimer
(
	IN xbox::PKTIMER Timer,
	IN xbox::ulong_xt Hand
)
{
	PKTIMER_TABLE_ENTRY TableEntry;

	ASSERT_TIMER_LOCKED;

	/* Remove the timer from the timer list and check if it's empty */
	if (RemoveEntryList(&Timer->TimerListEntry))
	{
		/* Get the respective timer table entry */
		TableEntry = &KiTimerTableListHead[Hand];
		if (&TableEntry->Entry == TableEntry->Entry.Flink)
		{
			/* Set the entry to an infinite absolute time */
			TableEntry->Time.u.HighPart = 0xFFFFFFFF;
		}
	}

	/* Clear the list entries so we can tell the timer is gone */
	Timer->TimerListEntry.Flink = NULL;
	Timer->TimerListEntry.Blink = NULL;
}

xbox::void_xt xbox::KxRemoveTreeTimer
(
	IN xbox::PKTIMER Timer
)
{
	ULONG Hand = KiComputeTimerTableIndex(Timer->DueTime.QuadPart);
	PKTIMER_TABLE_ENTRY TimerEntry;

	ASSERT_TIMER_LOCKED;

	/* Set the timer as non-inserted */
	Timer->Header.Inserted = FALSE;

	/* Remove it from the timer list */
	if (RemoveEntryList(&Timer->TimerListEntry))
	{
		/* Get the entry and check if it's empty */
		TimerEntry = &KiTimerTableListHead[Hand];
		if (IsListEmpty(&TimerEntry->Entry))
		{
			/* Clear the time then */
			TimerEntry->Time.u.HighPart = 0xFFFFFFFF;
		}
	}
}

xbox::boolean_xt FASTCALL xbox::KiInsertTimerTable
(
	IN xbox::PKTIMER Timer,
	IN xbox::ulong_xt Hand
)
{
	LARGE_INTEGER InterruptTime;
	LONGLONG DueTime = Timer->DueTime.QuadPart;
	BOOLEAN Expired = FALSE;
	PLIST_ENTRY ListHead, NextEntry;
	PKTIMER CurrentTimer;

	EmuLog(LOG_LEVEL::DEBUG, "%s: inserting Timer %p, Hand: %lu", __func__, Timer, Hand);

	ASSERT_TIMER_LOCKED;

	/* Check if the period is zero */
	if (!Timer->Period) {
		Timer->Header.SignalState = FALSE;
	}

	/* Loop the timer list backwards */
	ListHead = &KiTimerTableListHead[Hand].Entry;
	NextEntry = ListHead->Blink;
	while (NextEntry != ListHead)
	{
		/* Get the timer */
		CurrentTimer = CONTAINING_RECORD(NextEntry, KTIMER, TimerListEntry);

		/* Now check if we can fit it before */
		if ((ULONGLONG)DueTime >= CurrentTimer->DueTime.QuadPart) break;

		/* Keep looping */
		NextEntry = NextEntry->Blink;
	}

	/* Looped all the list, insert it here and get the interrupt time again */
	InsertHeadList(NextEntry, &Timer->TimerListEntry);

	/* Check if we didn't find it in the list */
	if (NextEntry == ListHead)
	{
		/* Set the time */
		KiTimerTableListHead[Hand].Time.QuadPart = DueTime;

		/* Make sure it hasn't expired already */
		InterruptTime.QuadPart = KeQueryInterruptTime();
		if (DueTime <= InterruptTime.QuadPart) {
			EmuLog(LOG_LEVEL::DEBUG, "Timer %p already expired", Timer);
			Expired = TRUE;
		}
	}

	/* Return expired state */
	return Expired;
}

xbox::boolean_xt FASTCALL xbox::KiInsertTreeTimer
(
	IN xbox::PKTIMER Timer,
	IN xbox::LARGE_INTEGER Interval
)
{
	BOOLEAN Inserted = FALSE;
	ULONG Hand = 0;

	ASSERT_TIMER_LOCKED;

	/* Setup the timer's due time */
	if (KiComputeDueTime(Timer, Interval, (PULONG)&Hand))
	{
		/* Insert the timer */
		if (KiInsertTimerTable(Timer, Hand))
		{
			/* It was already there, remove it */
			KiRemoveEntryTimer(Timer, Hand);
			Timer->Header.Inserted = FALSE;
		}
		else
		{
			/* Otherwise, we're now inserted */
			Inserted = TRUE;
		}
	}

	return Inserted;
}

xbox::ulong_xt xbox::KiComputeTimerTableIndex
(
	IN xbox::ulonglong_xt Interval
)
{
	return (Interval / CLOCK_TIME_INCREMENT) & (TIMER_TABLE_SIZE - 1);
}

xbox::boolean_xt xbox::KiComputeDueTime
(
	IN  xbox::PKTIMER Timer,
	IN  xbox::LARGE_INTEGER DueTime,
	OUT xbox::PULONG Hand)
{
	LARGE_INTEGER InterruptTime, SystemTime, DifferenceTime;

	ASSERT_TIMER_LOCKED;

	/* Convert to relative time if needed */
	Timer->Header.Absolute = FALSE;
	if (DueTime.u.HighPart >= 0)
	{
		/* Get System Time */
		KeQuerySystemTime(&SystemTime);

		/* Do the conversion */
		DifferenceTime.QuadPart = SystemTime.QuadPart - DueTime.QuadPart;

		/* Make sure it hasn't already expired */
		Timer->Header.Absolute = TRUE;
		if (DifferenceTime.u.HighPart >= 0)
		{
			/* Cancel everything */
			EmuLog(LOG_LEVEL::DEBUG, "Timer %p already expired", Timer);
			Timer->Header.SignalState = TRUE;
			Timer->DueTime.QuadPart = 0;
			*Hand = 0;
			return FALSE;
		}

		/* Set the time as Absolute */
		DueTime = DifferenceTime;
	}

	/* Get the Interrupt Time */
	InterruptTime.QuadPart = KeQueryInterruptTime();

	/* Recalculate due time */
	Timer->DueTime.QuadPart = InterruptTime.QuadPart - DueTime.QuadPart;

	/* Get the handle */
	*Hand = KiComputeTimerTableIndex(Timer->DueTime.QuadPart);
	Timer->Header.Inserted = TRUE;

	return TRUE;
}

xbox::boolean_xt FASTCALL xbox::KiSignalTimer
(
	IN xbox::PKTIMER Timer
)
{
	BOOLEAN RequestInterrupt = FALSE;
	PKDPC Dpc = Timer->Dpc;
	ULONG Period = Timer->Period;
	LARGE_INTEGER Interval, SystemTime;

	ASSERT_TIMER_LOCKED;

	/* Set default values */
	Timer->Header.Inserted = FALSE;
	Timer->Header.SignalState = TRUE;

	/* Check if the timer has waiters */
	if (!IsListEmpty(&Timer->Header.WaitListHead))
	{
		/* Check the type of event */
		if (Timer->Header.Type == TimerNotificationObject)
		{
			/* Unwait the thread */
			// KxUnwaitThread(&Timer->Header, IO_NO_INCREMENT);
		}
		else
		{
			/* Otherwise unwait the thread and signal the timer */
			// KxUnwaitThreadForEvent((PKEVENT)Timer, IO_NO_INCREMENT);
		}
	}

	/* Check if we have a period */
	if (Period)
	{
		/* Calculate the interval and insert the timer */
		Interval.QuadPart = Int32x32To64(Period, -10000);
		while (!KiInsertTreeTimer(Timer, Interval));
	}

	/* Check if we have a DPC */
	if (Dpc)
	{
		/* Insert it in the queue */
		KeQuerySystemTime(&SystemTime);
		KeInsertQueueDpc(Dpc,
			ULongToPtr(SystemTime.u.LowPart),
			ULongToPtr(SystemTime.u.HighPart));
		RequestInterrupt = TRUE;
	}

	/* Return whether we need to request a DPC interrupt or not */
	return RequestInterrupt;
}

xbox::void_xt NTAPI xbox::KiTimerExpiration
(
	IN xbox::PKDPC Dpc,
	IN xbox::PVOID DeferredContext,
	IN xbox::PVOID SystemArgument1,
	IN xbox::PVOID SystemArgument2
)
{
	ULARGE_INTEGER SystemTime, InterruptTime;
	LARGE_INTEGER Interval;
	LONG Limit, Index, i;
	ULONG Timers, ActiveTimers, DpcCalls;
	PLIST_ENTRY ListHead, NextEntry;
	KIRQL OldIrql;
	PKTIMER Timer;
	PKDPC TimerDpc;
	ULONG Period;
	DPC_QUEUE_ENTRY DpcEntry[MAX_TIMER_DPCS];

	/* Query system and interrupt time */
	KeQuerySystemTime((PLARGE_INTEGER)&SystemTime);
	InterruptTime.QuadPart = KeQueryInterruptTime();
	Limit = KeTickCount;

	/* Get the index of the timer and normalize it */
	Index = PtrToLong(SystemArgument1);
	if ((Limit - Index) >= TIMER_TABLE_SIZE)
	{
		/* Normalize it */
		Limit = Index + TIMER_TABLE_SIZE - 1;
	}

	/* Setup index and actual limit */
	Index--;
	Limit &= (TIMER_TABLE_SIZE - 1);

	/* Setup accounting data */
	DpcCalls = 0;
	Timers = 24;
	ActiveTimers = 4;

	/* Lock the Database and Raise IRQL */
	KiTimerLock();
	KiLockDispatcherDatabase(&OldIrql);

	/* Start expiration loop */
	do
	{
		/* Get the current index */
		Index = (Index + 1) & (TIMER_TABLE_SIZE - 1);

		/* Get list pointers and loop the list */
		ListHead = &KiTimerTableListHead[Index].Entry;
		while (ListHead != ListHead->Flink)
		{
			/* Go to the next entry */
			NextEntry = ListHead->Flink;

			/* Get the current timer and check its due time */
			Timers--;
			Timer = CONTAINING_RECORD(NextEntry, KTIMER, TimerListEntry);
			if ((NextEntry != ListHead) &&
				(Timer->DueTime.QuadPart <= InterruptTime.QuadPart))
			{
				/* It's expired, remove it */
				ActiveTimers--;
				KiRemoveEntryTimer(Timer, Index);

				/* Make it non-inserted and signal it */
				Timer->Header.Inserted = FALSE;
				Timer->Header.SignalState = 1;

				/* Get the DPC and period */
				TimerDpc = Timer->Dpc;
				Period = Timer->Period;

				/* Check if there are any waiters */
				if (!IsListEmpty(&Timer->Header.WaitListHead))
				{
					/* Check the type of event */
					if (Timer->Header.Type == TimerNotificationObject)
					{
						/* Unwait the thread */
						// KxUnwaitThread(&Timer->Header, IO_NO_INCREMENT);
					}
					else
					{
						/* Otherwise unwait the thread and signal the timer */
						// KxUnwaitThreadForEvent((PKEVENT)Timer, IO_NO_INCREMENT);
					}
				}

				/* Check if we have a period */
				if (Period)
				{
					/* Calculate the interval and insert the timer */
					Interval.QuadPart = Int32x32To64(Period, -10000);
					while (!KiInsertTreeTimer(Timer, Interval));
				}

				/* Check if we have a DPC */
				if (TimerDpc)
				{
					/* Setup the DPC Entry */
					DpcEntry[DpcCalls].Dpc = TimerDpc;
					DpcEntry[DpcCalls].Routine = TimerDpc->DeferredRoutine;
					DpcEntry[DpcCalls].Context = TimerDpc->DeferredContext;
					DpcCalls++;
					assert(DpcCalls < MAX_TIMER_DPCS);
				}

				/* Check if we're done processing */
				if (!(ActiveTimers) || !(Timers))
				{
					/* Release the dispatcher while doing DPCs */
					KiUnlockDispatcherDatabase(DISPATCH_LEVEL);

					/* Start looping all DPC Entries */
					for (i = 0; DpcCalls; DpcCalls--, i++)
					{
						/* Call the DPC */
						EmuLog(LOG_LEVEL::DEBUG, "%s, calling DPC at 0x%.8X", __func__, DpcEntry[i].Routine);

						// Call the Deferred Procedure  :
						DpcEntry[i].Routine(
							DpcEntry[i].Dpc,
							DpcEntry[i].Context,
							UlongToPtr(SystemTime.u.LowPart),
							UlongToPtr(SystemTime.u.HighPart)
						);
					}

					/* Reset accounting */
					Timers = 24;
					ActiveTimers = 4;

					/* Lock the dispatcher database */
					KiLockDispatcherDatabaseAtDpcLevel();
				}
			}
			else
			{
				/* Check if the timer list is empty */
				if (NextEntry != ListHead)
				{
					/* Sanity check */
					assert(KiTimerTableListHead[Index].Time.QuadPart <=
						Timer->DueTime.QuadPart);

					/* Update the time */
					KiTimerTableListHead[Index].Time.QuadPart =
						Timer->DueTime.QuadPart;
				}

				/* Check if we've scanned all the timers we could */
				if (!Timers)
				{
					/* Release the dispatcher while doing DPCs */
					KiUnlockDispatcherDatabase(DISPATCH_LEVEL);

					/* Start looping all DPC Entries */
					for (i = 0; DpcCalls; DpcCalls--, i++)
					{
						/* Call the DPC */
						EmuLog(LOG_LEVEL::DEBUG, "%s, calling DPC at 0x%.8X", __func__, DpcEntry[i].Routine);

						// Call the Deferred Procedure  :
						DpcEntry[i].Routine(
							DpcEntry[i].Dpc,
							DpcEntry[i].Context,
							UlongToPtr(SystemTime.u.LowPart),
							UlongToPtr(SystemTime.u.HighPart)
						);
					}

					/* Reset accounting */
					Timers = 24;
					ActiveTimers = 4;

					/* Lock the dispatcher database */
					KiLockDispatcherDatabaseAtDpcLevel();
				}

				/* Done looping */
				break;
			}
		}
	} while (Index != Limit);

	/* Verify the timer table, on a debug kernel only */
	if (g_bIsDebugKernel) {
		KiCheckTimerTable(InterruptTime);
	}

	/* Check if we still have DPC entries */
	if (DpcCalls)
	{
		/* Release the dispatcher while doing DPCs */
		KiUnlockDispatcherDatabase(DISPATCH_LEVEL);

		/* Start looping all DPC Entries */
		for (i = 0; DpcCalls; DpcCalls--, i++)
		{
			/* Call the DPC */
			EmuLog(LOG_LEVEL::DEBUG, "%s, calling DPC at 0x%.8X", __func__, DpcEntry[i].Routine);

			// Call the Deferred Procedure  :
			DpcEntry[i].Routine(
				DpcEntry[i].Dpc,
				DpcEntry[i].Context,
				UlongToPtr(SystemTime.u.LowPart),
				UlongToPtr(SystemTime.u.HighPart)
			);
		}

		/* Lower IRQL if we need to */
		if (OldIrql != DISPATCH_LEVEL) {
			KfLowerIrql(OldIrql);
		}
		KiTimerUnlock();
	}
	else
	{
		/* Unlock the dispatcher */
		KiUnlockDispatcherDatabase(OldIrql);
		KiTimerUnlock();
	}
}

xbox::void_xt FASTCALL xbox::KiTimerListExpire
(
	IN xbox::PLIST_ENTRY ExpiredListHead,
	IN xbox::KIRQL OldIrql
)
{
	ULARGE_INTEGER SystemTime;
	LARGE_INTEGER Interval;
	LONG i;
	ULONG DpcCalls = 0;
	PKTIMER Timer;
	PKDPC TimerDpc;
	ULONG Period;
	DPC_QUEUE_ENTRY DpcEntry[MAX_TIMER_DPCS];

	ASSERT_TIMER_LOCKED;

	/* Query system */
	KeQuerySystemTime((PLARGE_INTEGER)&SystemTime);

	/* Loop expired list */
	while (ExpiredListHead->Flink != ExpiredListHead)
	{
		/* Get the current timer */
		Timer = CONTAINING_RECORD(ExpiredListHead->Flink, KTIMER, TimerListEntry);

		/* Remove it */
		RemoveEntryList(&Timer->TimerListEntry);

		/* Not inserted */
		Timer->Header.Inserted = FALSE;

		/* Signal it */
		Timer->Header.SignalState = 1;

		/* Get the DPC and period */
		TimerDpc = Timer->Dpc;
		Period = Timer->Period;

		/* Check if there's any waiters */
		if (!IsListEmpty(&Timer->Header.WaitListHead))
		{
			/* Check the type of event */
			if (Timer->Header.Type == TimerNotificationObject)
			{
				/* Unwait the thread */
				// KxUnwaitThread(&Timer->Header, IO_NO_INCREMENT);
			}
			else
			{
				/* Otherwise unwait the thread and signal the timer */
				// KxUnwaitThreadForEvent((PKEVENT)Timer, IO_NO_INCREMENT);
			}
		}

		/* Check if we have a period */
		if (Period)
		{
			/* Calculate the interval and insert the timer */
			Interval.QuadPart = Int32x32To64(Period, -10000);
			while (!KiInsertTreeTimer(Timer, Interval));
		}

		/* Check if we have a DPC */
		if (TimerDpc)
		{
			/* Setup the DPC Entry */
			DpcEntry[DpcCalls].Dpc = TimerDpc;
			DpcEntry[DpcCalls].Routine = TimerDpc->DeferredRoutine;
			DpcEntry[DpcCalls].Context = TimerDpc->DeferredContext;
			DpcCalls++;
			assert(DpcCalls < MAX_TIMER_DPCS);
		}
	}

	/* Check if we still have DPC entries */
	if (DpcCalls)
	{
		/* Release the dispatcher while doing DPCs */
		KiUnlockDispatcherDatabase(DISPATCH_LEVEL);

		/* Start looping all DPC Entries */
		for (i = 0; DpcCalls; DpcCalls--, i++)
		{
			/* Call the DPC */
			EmuLog(LOG_LEVEL::DEBUG, "%s, calling DPC at 0x%.8X", __func__, DpcEntry[i].Routine);

			// Call the Deferred Procedure  :
			DpcEntry[i].Routine(
				DpcEntry[i].Dpc,
				DpcEntry[i].Context,
				UlongToPtr(SystemTime.u.LowPart),
				UlongToPtr(SystemTime.u.HighPart)
			);
		}

		/* Lower IRQL */
		KfLowerIrql(OldIrql);
		KiTimerUnlock();
	}
	else
	{
		/* Unlock the dispatcher */
		KiUnlockDispatcherDatabase(OldIrql);
		KiTimerUnlock();
	}
}

xbox::void_xt FASTCALL xbox::KiWaitSatisfyAll
(
	IN xbox::PKWAIT_BLOCK WaitBlock
)
{
	PKMUTANT Object;
	PRKTHREAD Thread;
	PKWAIT_BLOCK WaitBlock1;

	WaitBlock1 = WaitBlock;
	Thread = WaitBlock1->Thread;
	do {
		if (WaitBlock1->WaitKey != (cshort_xt)STATUS_TIMEOUT) {
			Object = (PKMUTANT)WaitBlock1->Object;
			KiWaitSatisfyAny(Object, Thread);
		}

		WaitBlock1 = WaitBlock1->NextWaitBlock;
	} while (WaitBlock1 != WaitBlock);

	return;
}

template<xbox::MODE ApcMode>
static xbox::void_xt KiExecuteApc()
{
	xbox::PKTHREAD kThread = xbox::KeGetCurrentThread();

	if constexpr (ApcMode == xbox::KernelMode) {
		kThread->ApcState.KernelApcPending = FALSE;
	}
	else {
		kThread->ApcState.UserApcPending = FALSE;
	}

	// Even though the apc list is per-thread, it's still possible that another thread will access it while we are processing it below
	xbox::KiApcListMtx.lock();
	while (!IsListEmpty(&kThread->ApcState.ApcListHead[ApcMode])) {
		if ((ApcMode == xbox::KernelMode) && (kThread->KernelApcDisable != 0)) {
			xbox::KiApcListMtx.unlock();
			return;
		}
		xbox::PLIST_ENTRY Entry = kThread->ApcState.ApcListHead[ApcMode].Flink;
		xbox::PKAPC Apc = CONTAINING_RECORD(Entry, xbox::KAPC, ApcListEntry);
		RemoveEntryList(Entry);
		Apc->Inserted = FALSE;
		xbox::KiApcListMtx.unlock();

		// NOTE: we never use KernelRoutine because that is only used for kernel APCs, which we currently don't use
		if (Apc->NormalRoutine != xbox::zeroptr) {
			(Apc->NormalRoutine)(Apc->NormalContext, Apc->SystemArgument1, Apc->SystemArgument2);
		}

		xbox::ExFreePool(Apc);
		xbox::KiApcListMtx.lock();
	}

	xbox::KiApcListMtx.unlock();
}

xbox::void_xt xbox::KiExecuteKernelApc()
{
	KiExecuteApc<KernelMode>();
}

xbox::void_xt xbox::KiExecuteUserApc()
{
	KiExecuteApc<UserMode>();
}

xbox::PLARGE_INTEGER FASTCALL xbox::KiComputeWaitInterval
(
	IN xbox::PLARGE_INTEGER OriginalTime,
	IN xbox::PLARGE_INTEGER DueTime,
	IN OUT xbox::PLARGE_INTEGER NewTime
)
{
	if (OriginalTime->QuadPart >= 0) {
		return OriginalTime;
	}
	else {
		NewTime->QuadPart = xbox::KeQueryInterruptTime();
		NewTime->QuadPart -= DueTime->QuadPart;
		return NewTime;
	}
}
