/*

Nintendont (Kernel) - Playing Gamecubes in Wii mode on a Wii U

Copyright (C) 2013  crediar

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/
#include "string.h"
#include "syscalls.h"
#include "global.h"
#include "ipc.h"
#include "common.h"
#include "alloc.h"
#include "DI.h"
#include "ES.h"
#include "SI.h"
#include "BT.h"
#include "lwbt/bte.h"
#include "Stream.h"
#include "HID.h"
#include "EXI.h"
#include "debug.h"
#include "GCAM.h"
#include "Patch.h"
#ifdef NINTENDONT_USB
#include "usb.h"
#else
#include "SDI.h"
#endif

int verbose = 0;
u32 base_offset=0;
void *queuespace=NULL;
int queueid = 0;
int heapid=0;
int FFSHandle=0;
u32 FSUSB=0;
void *reset = (void*)0x1300300C;
//#undef DEBUG

extern u32 s_size;
extern u32 s_cnt;

FATFS *fatfs;

static u32 HID_ThreadStack[0x400] __attribute__((aligned(32)));
static u32 DI_ThreadStack[0x400] __attribute__((aligned(32)));

extern u32 SI_IRQ;
extern bool DI_IRQ, EXI_IRQ;
extern struct ipcmessage DI_CallbackMsg;

extern char __bss_start, __bss_end;
int _main( int argc, char *argv[] )
{
	//BSS is in DATA section so IOS doesnt touch it, we need to manually clear it
	//dbgprintf("memset32(%08x, 0, %08x)\n", &__bss_start, &__bss_end - &__bss_start);
	memset32(&__bss_start, 0, &__bss_end - &__bss_start);
	sync_after_write(&__bss_start, &__bss_end - &__bss_start);

	s32 ret = 0;
	u32 HID_Thread = 0, DI_Thread = 0;
	
	u8 MessageHeap[0x10];
	//u32 MessageQueue=0xFFFFFFFF;

	BootStatus(0, 0, 0);

	thread_set_priority( 0, 0x79 );	// do not remove this, this waits for FS to be ready!
	thread_set_priority( 0, 0x50 );
	thread_set_priority( 0, 0x79 );

	//MessageQueue = ES_Init( MessageHeap );
	ES_Init( MessageHeap );

	BootStatus(1, 0, 0);

#ifndef NINTENDONT_USB
	BootStatus(2, 0, 0);
	ret = SDHCInit();
	if(!ret)
	{
		dbgprintf("SD:SDHCInit() failed:%d\r\n", ret );
		BootStatusError(-2, ret);
		mdelay(2000);
		Shutdown();
	}
#endif
	BootStatus(3, 0, 0);
	fatfs = (FATFS*)malloca( sizeof(FATFS), 32 );

	s32 res = f_mount( 0, fatfs );
	if( res != FR_OK )
	{
		dbgprintf("ES:f_mount() failed:%d\r\n", res );
		BootStatusError(-3, res);
		mdelay(2000);
		Shutdown();
	}
	
	BootStatus(4, 0, 0);

	BootStatus(5, 0, 0);
	
	int MountFail = 0;
	s32 fres = -1;
	FIL fp;
	while(fres != FR_OK)
	{
		fres = f_open(&fp, "/bladie", FA_READ|FA_OPEN_EXISTING);
		switch(fres)
		{
			case FR_OK:
				f_close(&fp);
			case FR_NO_PATH:
			case FR_NO_FILE:
			{
				fres = FR_OK;
			} break;
			default:
			case FR_DISK_ERR:
			{
				f_mount(0, NULL);		//unmount drive todo: retry could never work
				MountFail++;
				if(MountFail == 10)
				{
					BootStatusError(-5, fres);
					mdelay(2000);
					Shutdown();
				}
				mdelay(5);
			} break;
		}
	}
#ifndef NINTENDONT_USB
	s_size = 512;
	s_cnt = fatfs->n_fatent * fatfs->csize;
#endif

	BootStatus(6, s_size, s_cnt);

#ifdef NINTENDONT_USB
	s32 r = LoadModules(55);
	//dbgprintf("ES:ES_LoadModules(%d):%d\r\n", 55, r );
	if( r < 0 )
	{
		BootStatusError(-6, r);
		mdelay(2000);
		Shutdown();
	}
#endif

	BootStatus(7, s_size, s_cnt);
	ConfigInit();
	
	if (ConfigGetConfig(NIN_CFG_LOG))
		SDisInit = 1;  // Looks okay after threading fix
	dbgprintf("Game path: %s\r\n", ConfigGetGamePath());

	BootStatus(8, s_size, s_cnt);

	memset32((void*)0x13002800, 0, 0x30);
	sync_after_write((void*)0x13002800, 0x30);
	memset32((void*)0x13160000, 0, 0x20);
	sync_after_write((void*)0x13160000, 0x20);

	memset32((void*)0x13026500, 0, 0x100);
	sync_after_write((void*)0x13026500, 0x100);

	bool UseHID = ConfigGetConfig(NIN_CFG_HID);
	if( UseHID )
	{
		ret = HIDInit();
		if(ret < 0 )
		{
			dbgprintf("ES:HIDInit() failed\r\n" );
			BootStatusError(-8, ret);
			mdelay(2000);
			Shutdown();
		}
		write32(0x13003004, 0);
		sync_after_write((void*)0x13003004, 0x20);

		HID_Thread = thread_create(HID_Run, NULL, HID_ThreadStack, 0x400, 0x78, 1);
		thread_continue(HID_Thread);
	}
	BootStatus(9, s_size, s_cnt);

	DIRegister();
	DI_Thread = thread_create(DIReadThread, NULL, DI_ThreadStack, 0x400, 0x78, 1);
	thread_continue(DI_Thread);

	DIinit(true);

	BootStatus(10, s_size, s_cnt);

	GCAMInit();

	EXIInit();

	BootStatus(11, s_size, s_cnt);
#ifdef PATCHSI
	SIInit();
#endif
	StreamInit();

	PatchInit();

//This bit seems to be different on japanese consoles
	u32 ori_ppcspeed = read32(HW_PPCSPEED);
	if((ConfigGetGameID() & 0xFF) == 'J')
		set32(HW_PPCSPEED, (1<<17));
	else
		clear32(HW_PPCSPEED, (1<<17));

	//write32( 0x1860, 0xdeadbeef );	// Clear OSReport area

//Tell PPC side we are ready!
	cc_ahbMemFlush(1);
	mdelay(1000);
	BootStatus(0xdeadbeef, s_size, s_cnt);

	u32 Now = read32(HW_TIMER);
	#ifdef PATCHSI
	u32 PADTimer = Now;
	#endif
	u32 DiscChangeTimer = Now;
	u32 ResetTimer = Now;
	u32 Reset = 0;
	bool SaveCard = false;
	if( ConfigGetConfig(NIN_CFG_LED) )
	{
		set32(HW_GPIO_ENABLE, GPIO_SLOT_LED);
		clear32(HW_GPIO_DIR, GPIO_SLOT_LED);
		clear32(HW_GPIO_OWNER, GPIO_SLOT_LED);
	}
	EnableAHBProt(-1); //disable AHBPROT
	write32(0xd8006a0, 0x30000004), mask32(0xd8006a8, 0, 2); //widescreen fix
	while (1)
	{
		_ahbMemFlush(0);

		//Check this.  Purpose is to send another interrupt if wasn't processed
		/*if (((read32(0x14) != 0) || (read32(0x13026514) != 0))
			&& (read32(HW_ARMIRQFLAG) & (1 << 30)) == 0)
		{
			write32(HW_IPC_ARMCTRL, (1 << 0) | (1 << 4)); //throw irq
		}*/
		#ifdef PATCHALL
		if (EXI_IRQ == true)
		{
			if(EXICheckTimer())
				EXIInterrupt();
		}
		#endif
		#ifdef PATCHSI
		if(SI_IRQ != 0)
		{
			if (((read32(HW_TIMER) - PADTimer) > 7910) || (SI_IRQ & 0x2))	// about 240 times a second
			{
				SIInterrupt();
				PADTimer = read32(HW_TIMER);
			}
		}
		#endif
		if(DI_IRQ == true)
		{
			if(DI_CallbackMsg.result == 0)
				DIInterrupt();
		}
		else if(SaveCard == true) /* DI IRQ indicates we might read async, so dont write at the same time */
		{
			if((read32(HW_TIMER) - Now) / 1898437 > 2) /* after 3 second earliest */
			{
				EXISaveCard();
				SaveCard = false;
			}
		}
		udelay(10); //wait for other threads

		//Baten Kaitos save hax
		/*if( read32(0) == 0x474B4245 )
		{
			if( read32( 0x0073E640 ) == 0xFFFFFFFF )
			{
				write32( 0x0073E640, 0 );
			}
		}*/

		if ( DiscChangeIRQ == 1 )
		{
			DiscChangeTimer = read32(HW_TIMER);
			DiscChangeIRQ = 2;
		}
		else if ( DiscChangeIRQ == 2 )
		{
			if ( (read32(HW_TIMER) - DiscChangeTimer ) >  2 * 243000000 / 128)
			{
				//dbgprintf("DIP:IRQ mon!\r\n");
				set32( DI_SSTATUS, 0x3A );
				sync_after_write((void*)DI_SSTATUS, 4);
				DIInterrupt();
				DiscChangeIRQ = 0;
			}
		}
		_ahbMemFlush(1);
		DIUpdateRegisters();
		#ifdef PATCHALL
		EXIUpdateRegistersNEW();
		GCAMUpdateRegisters();
		BTUpdateRegisters();
		#endif
		StreamUpdateRegisters();
		CheckOSReport();
		if(EXICheckCard())
		{
			Now = read32(HW_TIMER);
			SaveCard = true;
		}
		#ifdef PATCHSI
		SIUpdateRegisters();
		if(read32(DIP_IMM) == 0x1DEA)
		{
			DIFinishAsync();
			break;
		}
		if(read32(DIP_IMM) == 0x3DEA)
		{
			if(Reset == 0)
			{
				dbgprintf("Fake Reset IRQ\n");
				write32( EXI2DATA, 0x2 ); // Reset irq
				write32( HW_IPC_ARMCTRL, (1<<0) | (1<<4) ); //throw irq
				Reset = 1;
			}
		}
		else if(Reset == 1)
		{
			write32( EXI2DATA, 0x10000 ); // send pressed
			ResetTimer = read32(HW_TIMER);
			Reset = 2;
		}
		/* The cleanup is not connected to the button press */
		if(Reset == 2)
		{
			if((read32(HW_TIMER) - ResetTimer) / 949219 > 0) //free after half a second
			{
				write32( EXI2DATA, 0 ); // done, clear
				write32(DIP_IMM, 0);
				Reset = 0;
			}
		}
		#endif
		if(read32(DIP_IMM) == 0x4DEA)
			PatchGame();
		if(read32(HW_GPIO_IN) & GPIO_POWER)
		{
			DIFinishAsync();
			#ifdef PATCHALL
			BTE_Shutdown();
			#endif
			Shutdown();
		}
		//sync_before_read( (void*)0x1860, 0x20 );
		//if( read32(0x1860) != 0xdeadbeef )
		//{
		//	if( read32(0x1860) != 0 )
		//	{
		//		dbgprintf(	(char*)(P2C(read32(0x1860))),
		//					(char*)(P2C(read32(0x1864))),
		//					(char*)(P2C(read32(0x1868))),
		//					(char*)(P2C(read32(0x186C))),
		//					(char*)(P2C(read32(0x1870))),
		//					(char*)(P2C(read32(0x1874)))
		//				);
		//	}
		//	write32(0x1860, 0xdeadbeef);
		//	sync_after_write( (void*)0x1860, 0x20 );
		//}
		cc_ahbMemFlush(1);
	}
	if( UseHID )
	{
		/* we're done reading inputs */
		thread_cancel(HID_Thread, 0);
	}

	IOS_Close(DI_Handle); //close game
	thread_cancel(DI_Thread, 0);
	DIUnregister();

	write32( DIP_IMM, 0 );
	/* reset time */
	while(1)
	{
		if(read32(DIP_IMM) == 0x2DEA)
			break;
		wait_for_ppc(1);
	}

	if( ConfigGetConfig(NIN_CFG_LED) )
		clear32(HW_GPIO_OUT, GPIO_SLOT_LED);

	if( ConfigGetConfig(NIN_CFG_MEMCARDEMU) )
		EXIShutdown();

	if (ConfigGetConfig(NIN_CFG_LOG))
		closeLog();

#ifdef PATCHALL
	BTE_Shutdown();
#endif

//unmount FAT device
	f_mount(0, NULL);

#ifndef NINTENDONT_USB
	SDHCShutdown();
#endif

//make sure we set that back to the original
	write32(HW_PPCSPEED, ori_ppcspeed);

	IOSBoot((char*)0x13003020, 0, read32(0x13003000));
	return 0;
}
