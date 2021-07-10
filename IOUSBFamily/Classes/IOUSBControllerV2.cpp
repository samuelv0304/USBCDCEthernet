/*
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright � 1997-2009 Apple Inc.  All rights reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


//================================================================================================
//
//   Headers
//
//================================================================================================
//
#include <IOKit/IOBufferMemoryDescriptor.h>

#include <IOKit/usb/IOUSBController.h>
#include <IOKit/usb/IOUSBControllerV2.h>
#include <IOKit/usb/IOUSBControllerV3.h>
#include <IOKit/usb/IOUSBLog.h>


#include "USBTracepoints.h"

#define CONTROLLERV2_USE_KPRINTF 0

#if CONTROLLERV2_USE_KPRINTF
#undef USBLog
#undef USBError
void kprintf(const char *format, ...)
__attribute__((format(printf, 1, 2)));
#define USBLog( LEVEL, FORMAT, ARGS... )  if ((LEVEL) <= CONTROLLERV2_USE_KPRINTF) { kprintf( FORMAT "\n", ## ARGS ) ; }
#define USBError( LEVEL, FORMAT, ARGS... )  { kprintf( FORMAT "\n", ## ARGS ) ; }
#endif

extern IOReturn CheckForDisjointDescriptor(IOUSBCommand *command, UInt16 maxPacketSize);

//================================================================================================
//
//   Local Definitions
//
//================================================================================================
//
#define super IOUSBController

// Copied from IOUSBController
enum {
    kSetupSent  = 0x01,
    kDataSent   = 0x02,
    kStatusSent = 0x04,
    kSetupBack  = 0x10,
    kDataBack   = 0x20,
    kStatusBack = 0x40
};

//================================================================================================
//
//   IOKit Constructors and Destructors
//
//================================================================================================
//
OSDefineMetaClass( IOUSBControllerV2, IOUSBController )
OSDefineAbstractStructors(IOUSBControllerV2, IOUSBController)


//================================================================================================
//
//   IOUSBControllerV2 Methods
//
//================================================================================================
//

bool 
IOUSBControllerV2::init(OSDictionary * propTable)
{
    if (!super::init(propTable))  return false;
    
    // allocate our expansion data
    if (!_v2ExpansionData)
    {
		_v2ExpansionData = (V2ExpansionData *)IOMalloc(sizeof(V2ExpansionData));
		if (!_v2ExpansionData)
			return false;
		bzero(_v2ExpansionData, sizeof(V2ExpansionData));
    }
	
    return (true);
}


bool 
IOUSBControllerV2::start( IOService * provider )
{
    
    if ( !super::start(provider))
        return (false);

	// allocate a thread_call structure - code shared by EHCI and UHCI
	_returnIsochDoneQueueThread = thread_call_allocate((thread_call_func_t)ReturnIsochDoneQueueEntry, (thread_call_param_t)this);
	if ( !_returnIsochDoneQueueThread )
	{
		USBError(1, "IOUSBControllerV2[%p]::start - could not allocate thread callout function.",  this);
		return false;
	}
	
	return true;
}



void
IOUSBControllerV2::free()
{
	
    if (_v2ExpansionData && _returnIsochDoneQueueThread)
    {
        thread_call_cancel(_returnIsochDoneQueueThread);
        thread_call_free(_returnIsochDoneQueueThread);
    }
	
	//  This needs to be the LAST thing we do, as it disposes of our "fake" member
    //  variables.
    //
    if (_v2ExpansionData)
    {
		IOFree(_v2ExpansionData, sizeof(V2ExpansionData));
		_v2ExpansionData = NULL;
    }
	
    super::free();
}





void
IOUSBControllerV2::clearTTHandler(OSObject *target, void *parameter, IOReturn status, UInt32	bufferSizeRemaining)
{
#pragma unused (bufferSizeRemaining)
    IOUSBController *		me = (IOUSBController *)target;
    IOUSBCommand *			command = (IOUSBCommand *)parameter;
	IOMemoryDescriptor *	memDesc = NULL;
	IODMACommand *			dmaCommand = NULL;
    UInt8					sent, back, todo;
    UInt8					hubAddr = command->GetAddress();

	USBTrace_Start( kUSBTController, kTPControllerClearTTHandler, (uintptr_t)me, (uintptr_t)command, status,  bufferSizeRemaining );
    USBLog(5,"clearTTHandler: status (0x%x)", status);

	if (!me || !command)
	{
		USBError(1,"clearTTHandler: missing controller or command");
	}
	
    sent = (command->GetStage() & 0x0f) << 4;
    back = command->GetStage() & 0xf0;
    todo = sent ^ back;						// thats xor
	
    if ((todo & kSetupBack) != 0)
    {
		USBLog(5,"clearTTHandler: Setup comming back to us, check and forget");
        command->SetStage(command->GetStage() | kSetupBack);
    }
    else
    {
		dmaCommand = command->GetDMACommand();
		memDesc = command->GetRequestMemoryDescriptor();
        command->SetStage(0);
		if (dmaCommand)
		{
			if (dmaCommand->getMemoryDescriptor())
			{
				if (dmaCommand->getMemoryDescriptor() != memDesc)
				{
					USBLog(5, "clearTTHandler: DMA Command Memory Descriptor (%p) does not match Request MemoryDescriptor (%p)", dmaCommand->getMemoryDescriptor(), memDesc);
					USBTrace( kUSBTController, kTPControllerClearTTHandler, (uintptr_t)me, (uintptr_t)dmaCommand->getMemoryDescriptor(), (uintptr_t)memDesc, 1 );
				}
				USBLog(7, "clearTTHandler: clearing memory descriptor (%p) from dmaCommand (%p)", dmaCommand->getMemoryDescriptor(), dmaCommand);
				dmaCommand->clearMemoryDescriptor();
			}
			else
			{
				USBLog(2, "clearTTHandler - dmaCommand (%p) already cleared", dmaCommand);
				USBTrace( kUSBTController, kTPControllerClearTTHandler, (uintptr_t)me, (uintptr_t)dmaCommand, 0, 2 );
			}
		}
		if (memDesc)
		{
			USBLog(6, "clearTTHandler - completing and freeing memory descriptor (%p)", memDesc);
			USBTrace( kUSBTController, kTPControllerClearTTHandler, (uintptr_t)me, (uintptr_t)memDesc, 0, 3 );
			command->SetRequestMemoryDescriptor(NULL);
			memDesc->complete();
			memDesc->release();
		}
		else
		{
			USBLog(1, "clearTTHandler - missing memory descriptor");
			USBTrace( kUSBTController, kTPControllerClearTTHandler, (uintptr_t)me, 0, 0, 4 );
		}
		USBLog(5,"clearTTHandler: We've already seen the setup, deallocate command (%p)", command);
		me->_freeUSBCommandPool->returnCommand(command);   
    }
    if ((status != kIOReturnSuccess) && (status != kIOUSBTransactionReturned))
    {
		USBLog((status == kIOReturnNotResponding ? 5 : 1), "%s[%p]::clearTTHandler - error response from hub (0x%x), clearing hub endpoint stall", me->getName(), me, status);
		USBTrace( kUSBTController, kTPControllerClearTTHandler, (uintptr_t)me, status, 0, 5 );
		
		me->UIMClearEndpointStall(hubAddr, 0, kUSBAnyDirn);
    }

	USBTrace_End( kUSBTController, kTPControllerClearTTHandler, (uintptr_t)me, 0, 0, 0 );
}


OSMetaClassDefineReservedUsed(IOUSBControllerV2,  6);
void 
IOUSBControllerV2::ClearTT(USBDeviceAddress fnAddress, UInt8 endpt, Boolean IN)
{
    UInt16						wValue;
	IOBufferMemoryDescriptor	*memDesc = NULL;
    IOUSBDevRequest				*clearRequest = NULL;
    short						hubAddress;
    IOUSBCommand				*clearCommand = NULL;
    IOUSBCompletion				completion;
    int							i;
    IOReturn					err = kIOReturnSuccess;
	IODMACommand				*dmaCommand = NULL;
	
    USBLog(5,"+%s[%p]::ClearTT", getName(), this);
	USBTrace_Start( kUSBTController, kTPControllerClearTT, (uintptr_t)this, fnAddress, endpt, IN );
	
    hubAddress = _highSpeedHub[fnAddress];	// Address of its controlling hub.
    if (hubAddress == 0)	// Its not a high speed device, it doesn't need a clearTT
    {
		USBLog(1,"-%s[%p]::ClearTT high speed device, returning", getName(), this);
		USBTrace( kUSBTController, kTPControllerClearTT, (uintptr_t)this, 0, 0, 1 );
		return;
    }
	
	memDesc = IOBufferMemoryDescriptor::withOptions(kIOMemoryUnshared | kIODirectionInOut, sizeof(IOUSBDevRequest));
	
	do				// not really a loop - just a way to avoid gotos
	{
		if (!memDesc)
		{
			USBLog(1,"%s[%p]::ClearTT Could not get a memory descriptor",getName(),this);
			USBTrace( kUSBTController, kTPControllerClearTT, (uintptr_t)this, 0, 0, 2 );
			err = kIOReturnNoMemory;
			break;
		}
		
		err = memDesc->prepare();
		if (err != kIOReturnSuccess)
		{
			USBError(1,"%s[%p]::ClearTT - err (%p) trying to prepare memory descriptor", getName(), this, (void*)err);
			memDesc->release();
			memDesc = NULL;
			break;
		}
		
		clearRequest = (IOUSBDevRequest*)memDesc->getBytesNoCopy();
		if (!clearRequest)
		{
			USBLog(1,"%s[%p]::ClearTT Could not get a IOUSBDevRequest", getName(), this);
			USBTrace( kUSBTController, kTPControllerClearTT, (uintptr_t)this, 0, 0, 5 );
			err = kIOReturnNoMemory;
			break;
		}
		USBLog(5, "%s[%p]::ClearTT - got IOUSBDevRequest (%p)", getName(), this, clearRequest);
		
		clearCommand = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
		if ( clearCommand == NULL )
		{
			IncreaseCommandPool();
			
			clearCommand = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
			if ( clearCommand == NULL )
			{
				USBLog(1,"%s[%p]::ClearTT Could not get a IOUSBCommand",getName(),this);
				USBTrace( kUSBTController, kTPControllerClearTT, (uintptr_t)this, 0, 0, 3 );
				err = kIOReturnNoResources;
				break;
			}
		}
		USBLog(7, "%s[%p]::ClearTT V2 got command (%p)", getName(), this, clearCommand);
		if (clearCommand->GetBufferUSBCommand())
		{
			USBLog(1,"%s[%p]::ClearTT - unexpected BufferUSBCommand(%p) inside of new command(%p)", getName(), this, clearCommand->GetBufferUSBCommand(), clearCommand);
			clearCommand->SetBufferUSBCommand(NULL);
		}
		if (clearCommand->GetRequestMemoryDescriptor())
		{
			USBLog(1,"%s[%p]::ClearTT - unexpected RequestMemoryDescriptor(%p) inside of new command(%p)", getName(), this, clearCommand->GetRequestMemoryDescriptor(), clearCommand);
			clearCommand->SetRequestMemoryDescriptor(NULL);
		}
		
		dmaCommand = clearCommand->GetDMACommand();
		if (!dmaCommand)
		{
			USBError(1,"%s[%p]::ClearTT - No dmaCommand in the usb command", getName(), this);
			USBTrace( kUSBTController, kTPControllerClearTT, (uintptr_t)this, 0, 0, 4 );
			err = kIOReturnNoResources;
			break;
		}
		
		if (dmaCommand->getMemoryDescriptor())
		{
			IOMemoryDescriptor		*XmemDesc = (IOMemoryDescriptor *)dmaCommand->getMemoryDescriptor();
			USBError(1,"%s[%p]::ClearTT - dmaCommand (%p) already had memory descriptor (%p) - clearing", getName(), this, dmaCommand, XmemDesc);
			dmaCommand->clearMemoryDescriptor();
		}

		USBLog(7,"%s[%p]::ClearTT - setting memory descriptor (%p) into dmaCommand (%p)", getName(), this, memDesc, dmaCommand);
		err = dmaCommand->setMemoryDescriptor(memDesc);
		if (err)
		{
			USBError(1,"%s[%p]::ClearTT - err (%p) setting memory descriptor (%p) into dmaCommand (%p)", getName(), this, (void*)err, memDesc, dmaCommand);
			break;
		}
		
		clearCommand->SetRequestMemoryDescriptor(memDesc);
		clearCommand->SetReqCount(8);
		memDesc = NULL;						// to prevent a possibly double release now that it is in the command
		
		wValue = (endpt & 0xf) | ( (fnAddress & 0x7f) << 4);
		if (IN)
		{
			wValue  |= (1 << 15);
		}
		USBLog(5,"%s[%p]::ClearTT - V2 EP (%d) ADDR (%d) wValue (0x%x)", getName(), this, endpt, fnAddress, wValue);
		/*		
		 3..0 Endpoint Number
		 10..4 Device Address
		 12..11 Endpoint Type	- Always controll == zero.
		 14..13 Reserved, must be zero
		 15 Direction, 1 = IN, 0 = OUT
		 
		 Endpoint Type
		 00 Control
		 01 Isochronous
		 10 Bulk
		 11 Interrupt
		 
		 */
		
		/* Request details largely copied from AppleUSBHubPort::ClearTT */
		
		clearRequest->bmRequestType = 0x23;
		clearRequest->bRequest = 8;
		clearRequest->wValue = HostToUSBWord(wValue);
		if (_v2ExpansionData->_multiTT[hubAddress])
		{  // MultiTT hub needs port address here
			clearRequest->wIndex = HostToUSBWord(_highSpeedPort[fnAddress]);
		}
		else
		{  // Single TT hubs need 1 here
			clearRequest->wIndex = HostToUSBWord(1);
		}
		clearRequest->wLength = HostToUSBWord(0);
		clearRequest->pData = NULL;
		clearRequest->wLenDone = 0;
		
		/* This copies large parts of IOUSBController::DeviceRequest, its not using IOUSBController::DeviceRequest */
		/* Because we're already inside the lock and don't want to go through the gate again. */
		
		completion.target    = (void *)this;
		completion.action    = (IOUSBCompletionAction) &clearTTHandler;
		completion.parameter = clearCommand;
		clearCommand->SetUSLCompletion(completion);
		
		clearCommand->SetUseTimeStamp(false);
		clearCommand->SetSelector(DEVICE_REQUEST);
		clearCommand->SetRequest(clearRequest);
		clearCommand->SetAddress(hubAddress);
		clearCommand->SetEndpoint(0);
		clearCommand->SetType(kUSBControl);
		clearCommand->SetBuffer(0); 			// no buffer for device requests
		clearCommand->SetClientCompletion(completion);
		clearCommand->SetNoDataTimeout(5000);
		clearCommand->SetCompletionTimeout(0);
		clearCommand->SetStage(0);
		clearCommand->SetBufferUSBCommand(NULL);
		clearCommand->SetMultiTransferTransaction(false);
		clearCommand->SetFinalTransferInTransaction(false);
		
		for (i=0; i < 10; i++)
			clearCommand->SetUIMScratch(i, 0);
		
		err = ControlTransaction(clearCommand);					// Wait for completion? Or just fire and forget?
		
	} while (false);
	
	
    if (kIOReturnSuccess != err)
    {
		// The only way we get in here is if we never called ControlTransaction, or if it returned an immediate error
		// In all other circumstances, the cleanup is all done in the clearTTHandler
		USBLog(1, "%s[%p]::ClearTT - error 0x%x cleaning up", getName(), this, err);
		USBTrace( kUSBTController, kTPControllerClearTT, (uintptr_t)this, err, 0, 6 );
		if (memDesc)
		{
			// this indicates that the memory descriptor was never successfully placed into the command
			memDesc->complete();
			memDesc->release();
			memDesc = NULL;
		}
		if (clearCommand)
		{
			memDesc = (IOBufferMemoryDescriptor*)clearCommand->GetRequestMemoryDescriptor();
			if (memDesc)
				clearCommand->SetRequestMemoryDescriptor(NULL);
			dmaCommand = clearCommand->GetDMACommand();
		}
		if (dmaCommand && dmaCommand->getMemoryDescriptor())
		{
			USBLog(2, "%s[%p]::ClearTT - clearing dmaCommand", getName(), this);
			dmaCommand->clearMemoryDescriptor();
		}
		if (memDesc)
		{
			// we repeat this here on purpose, because it means that we got the MD out of the request, and that we enterred this part with it NULL
			memDesc->complete();
			memDesc->release();
			memDesc = NULL;
		}
		if (clearCommand)
		{
			USBLog(7, "%s[%p]::ClearTT - returning command", getName(), this);
			_freeUSBCommandPool->returnCommand(clearCommand);
		}
		
    }
	
	USBTrace_End( kUSBTController, kTPControllerClearTT, (uintptr_t)this, 0, 0, 0);
}



IOReturn IOUSBControllerV2::OpenPipe(USBDeviceAddress address, UInt8 speed,
									 Endpoint *endpoint)
{
    return _commandGate->runAction(DoCreateEP, (void *)(UInt32)address,
								   (void *)(UInt32)speed, endpoint);
}


#ifdef SUPPORTS_SS_USB
OSMetaClassDefineReservedUsed(IOUSBControllerV2,  25);
IOReturn IOUSBControllerV2::OpenSSPipe(USBDeviceAddress address, UInt8 speed,
									 Endpoint *endpoint, UInt32 maxStreams, UInt32 maxBurst)
{
    return _commandGate->runAction(DoCreateEP, (void *)(UInt32)address,
								   (void *)(UInt32)speed, endpoint, (void *)(maxStreams+(maxBurst << 16)));
}
#endif

IOReturn 
IOUSBControllerV2::DoCreateEP(OSObject *owner,
							  void *arg0, void *arg1,
							  void *arg2, void *arg3)
{	
    IOUSBControllerV2 *me = (IOUSBControllerV2 *)owner;
    UInt8 address = (UInt8)(uintptr_t)arg0;
    UInt8 speed = (UInt8)(uintptr_t)arg1;
    Endpoint *endpoint = (Endpoint *)arg2;
#ifdef SUPPORTS_SS_USB
	UInt32 maxStreams = ((UInt32)(uintptr_t)arg3) & 0xffff;
	UInt32 maxBurst = ((UInt32)(uintptr_t)arg3) >> 16;
#else
#pragma unused(arg3)
#endif
    IOReturn err;
	
    USBLog(5,"%s[%p]::DoCreateEP, high speed ancestor hub:%d, port:%d", me->getName(), me, me->_highSpeedHub[address], me->_highSpeedPort[address]);

	USBTrace_Start( kUSBTController, kTPControllerDoCreateEP, (uintptr_t)me, me->_highSpeedHub[address], me->_highSpeedPort[address], endpoint->transferType );

#ifdef SUPPORTS_SS_USB
	IOUSBControllerV3		*me3 = OSDynamicCast(IOUSBControllerV3, owner);
    if(me3 == NULL)
    {
        if( (maxBurst != 0) || (maxStreams != 0) )
        {
            USBLog(1,"%s[%p]::DoCreateEP, SuperSpeed create EP, but controller doesn't support it. maxStreams: %d, maxBurst: %d", me->getName(), me, (int)maxStreams, (int)maxBurst);
        }
		return kIOReturnUnsupported;
    }
#endif
	
	switch (endpoint->transferType)
    {
        case kUSBInterrupt:
#ifdef SUPPORTS_SS_USB
            if(maxBurst == 0)
            {
#endif
                err = me->UIMCreateInterruptEndpoint(address,
                                                     endpoint->number,
                                                     endpoint->direction,
                                                     speed,
                                                     endpoint->maxPacketSize,
                                                     endpoint->interval,
                                                     me->_highSpeedHub[address],
                                                     me->_highSpeedPort[address]);
 #ifdef SUPPORTS_SS_USB
          }
            else
            {
                err = me3->UIMCreateSSInterruptEndpoint(address,
                                                     endpoint->number,
                                                     endpoint->direction,
                                                     speed,
                                                     endpoint->maxPacketSize,
                                                     endpoint->interval,
                                                     maxBurst);
            }
#endif
            break;
			
        case kUSBBulk:
#ifdef SUPPORTS_SS_USB
			if( (maxStreams == 0) && (maxBurst == 0) )
			{
#endif
				err = me->UIMCreateBulkEndpoint(address,
												endpoint->number,
												endpoint->direction,
												speed,
												endpoint->maxPacketSize,
												me->_highSpeedHub[address],
												me->_highSpeedPort[address]);
#ifdef SUPPORTS_SS_USB
			}
			else
			{
				err = me3->UIMCreateSSBulkEndpoint(address,
												endpoint->number,
												endpoint->direction,
												speed,
												endpoint->maxPacketSize,
												maxStreams,
                                                maxBurst);	
			}
#endif
            break;
			
        case kUSBControl:
            err = me->UIMCreateControlEndpoint(address,
											   endpoint->number,
											   endpoint->maxPacketSize,
											   speed,
											   me->_highSpeedHub[address],
											   me->_highSpeedPort[address]);
            break;
			
        case kUSBIsoc:
		{			
			if (speed == kUSBDeviceSpeedHigh)
			{
				UInt32		interval;
				
				// Filter out cases that violate the USB spec:
				if ((endpoint->interval < 1) || (endpoint->interval > 16))
				{
					USBLog(1, "%s[%p]::DoCreateEP - The USB 2.0 spec only allows Isoch EP with bInterval values of 1 through 16 "
						   "(see Table 9-13), but the illegal interval %d [0x%x] was requested, returning kIOReturnBadArgument", 
						   me->getName(), me, endpoint->interval, endpoint->interval);
					err = kIOReturnBadArgument;
					USBTrace( kUSBTController, kTPControllerDoCreateEP, (uintptr_t)me, err, endpoint->interval, 0);
					break;
				}

				interval = (1 << (endpoint->interval - 1));

				USBLog(4, "%s[%p]::DoCreateEP - Creating a High-Speed Isoch EP with interval %u [raw %u]", me->getName(), me, 
					   (unsigned int )interval, (unsigned int )endpoint->interval);

			}
			else
			{	
				// Full speed devices may have an invalid bInterval, thinking it doesn't
				// matter. To protect ourselves, assign an interval of 1 - our code will
				// do the right thing for full-speed devices then.
				endpoint->interval = 1;

				USBLog(4, "%s[%p]::DoCreateEP - Creating a Full-Speed Isoch EP with interval %u", me->getName(), me, (unsigned int )endpoint->interval);

			}

#ifdef SUPPORTS_SS_USB
            if(maxBurst == 0)
            {
#endif
                err = me->UIMCreateIsochEndpoint(address,
                                                 endpoint->number,
                                                 endpoint->maxPacketSize,
                                                 endpoint->direction,
                                                 me->_highSpeedHub[address],
                                                 me->_highSpeedPort[address],
                                                 endpoint->interval);
#ifdef SUPPORTS_SS_USB
            }
            else
            {
                err = me3->UIMCreateSSIsochEndpoint(address,
                                                 endpoint->number,
                                                 endpoint->maxPacketSize,
                                                 endpoint->direction,
                                                 endpoint->interval,
                                                 maxBurst);
            }
#endif
			break;
		  }

			
        default:
            err = kIOReturnBadArgument;
            break;
    }

	USBTrace_End( kUSBTController, kTPControllerDoCreateEP, (uintptr_t)me, err, 0, 0 );	
	
#ifdef SUPPORTS_SS_USB
	if ( err == kIOUSBEndpointCountExceeded )
	{
		USBLog(1, "%s[%p]::DoCreateEP - Received a kIOUSBEndpointCountExceeded, Address: %d, Speed: %d:  Endpoint: (0x%x, 0x%x, 0x%x, 0x%x)", me->getName(), me, address, speed,
			   (uint32_t) endpoint->number,
			   (uint32_t) endpoint->direction,
			   (uint32_t) endpoint->maxPacketSize,
			   (uint32_t) endpoint->interval);
		IOLog("The USB device at USB Address %d might not work correctly because the controller driver has reached a hardware limit on the number of endpoints", address);
	}
#endif
	
    return (err);
}



IOReturn 
IOUSBControllerV2::CreateDevice(	IOUSBDevice 		*newDevice,
									USBDeviceAddress	deviceAddress,
									UInt8				maxPacketSize,
									UInt8				speed,
									UInt32				powerAvailable,
									USBDeviceAddress	hub,
									int					port)
{
    USBLog(5,"%s[%p]::CreateDevice, new method called with speed : %d hub:%d, port:%d", getName(), this, speed, hub, port);
    
#ifdef SUPPORTS_SS_USB
	if( deviceAddress > kUSBMaxDevices )
	{
		USBLog(5,"%s[%p]::CreateDevice, returning kIOReturnInvalid with address: %d speed : %d hub:%d, port:%d", getName(), this, deviceAddress, speed, hub, port);
		return kIOReturnInvalid;
	}
    
    if ( speed < kUSBDeviceSpeedHigh )
    {
		if ( (hub == kXHCISSRootHubAddress) or (hub == kXHCIUSB2RootHubAddress) )
		{
            _highSpeedHub[deviceAddress] = hub;
            _highSpeedPort[deviceAddress] = port;
		}
		else
		{
			if (_highSpeedHub[hub] == 0)	// this is the first non high speed device in this chain
			{
				_highSpeedHub[deviceAddress] = hub;
				_highSpeedPort[deviceAddress] = port;
			}
			else
			{
				_highSpeedHub[deviceAddress] = _highSpeedHub[hub];
				_highSpeedPort[deviceAddress] = _highSpeedPort[hub];
			}
		}
    }
    else
    {
        _highSpeedHub[deviceAddress] = 0;
        _highSpeedPort[deviceAddress] = 0;
    }
	
#else
	if (speed != kUSBDeviceSpeedHigh)
    {
        if (_highSpeedHub[hub] == 0)	// this is the first non high speed device in this chain
        {
            _highSpeedHub[deviceAddress] = hub;
            _highSpeedPort[deviceAddress] = port;
        }
        else
        {
            _highSpeedHub[deviceAddress] = _highSpeedHub[hub];
            _highSpeedPort[deviceAddress] = _highSpeedPort[hub];
        }
    }
    else
    {
        _highSpeedHub[deviceAddress] = 0;
        _highSpeedPort[deviceAddress] = 0;
    }
#endif
	
    USBLog(5,"%s[%p]::CreateDevice, high speed ancestor hub:%d, port:%d",getName(), this, _highSpeedHub[deviceAddress], _highSpeedPort[deviceAddress]);
    
    return (super::CreateDevice(newDevice, deviceAddress, maxPacketSize, speed, powerAvailable));
}



IOReturn 
IOUSBControllerV2::ConfigureDeviceZero(UInt8 maxPacketSize, UInt8 speed, USBDeviceAddress hub, int port)
{
    USBLog(5,"%s[%p]::ConfigureDeviceZero, new method called with speed : %d, hub:%d, port:%d", getName(), this, speed, hub, port);
	
#ifdef SUPPORTS_SS_USB
	if( hub > kXHCIUSB2RootHubAddress )
	{
		USBLog(5,"%s[%p]::ConfigureDeviceZero, returning kIOReturnInvalid with speed : %d hub:%d, port:%d", getName(), this, speed, hub, port);
		return kIOReturnInvalid;
	}
	
    if ( speed < kUSBDeviceSpeedHigh )
    {
		if ( (hub == kXHCISSRootHubAddress) or (hub == kXHCIUSB2RootHubAddress) )
		{
            _highSpeedHub[0] = hub;
            _highSpeedPort[0] = port;
		}
		else
		{
			if (_highSpeedHub[hub] == 0)	// this is the first non high speed device in this chain
			{
				_highSpeedHub[0] = hub;
				_highSpeedPort[0] = port;
			}
			else
			{
				_highSpeedHub[0] = _highSpeedHub[hub];
				_highSpeedPort[0] = _highSpeedPort[hub];
			}
		}
    }
    else
    {
        _highSpeedHub[0] = 0;
        _highSpeedPort[0] = 0;
    }
#else
	if (speed != kUSBDeviceSpeedHigh)
    {
        if (_highSpeedHub[hub] == 0)	// this is the first non high speed device in this chain
        {
            _highSpeedHub[0] = hub;
            _highSpeedPort[0] = port;
        }
        else
        {
            _highSpeedHub[0] = _highSpeedHub[hub];
            _highSpeedPort[0] = _highSpeedPort[hub];
        }
    }
    else
    {
        _highSpeedHub[0] = 0;
        _highSpeedPort[0] = 0;
    }
#endif
    USBLog(5, "%s[%p]::CreateDevice, high speed ancestor hub:%d, port:%d", getName(), this, _highSpeedHub[0], _highSpeedPort[0]);
    
    return (super::ConfigureDeviceZero(maxPacketSize, speed));
}




IOReturn
IOUSBControllerV2::DOHSHubMaintenance(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
#pragma unused (arg3)
   IOUSBControllerV2 *me = (IOUSBControllerV2 *)owner;
    USBDeviceAddress highSpeedHub = (USBDeviceAddress)(uintptr_t)arg0;
    UInt32 command = (uintptr_t)arg1;
    UInt32 flags = (uintptr_t)arg2;
    UInt8 multi;
	
    USBLog(5,"%s[%p]::DOHSHubMaintenance, command: %d, flags: %d", me->getName(), me, (uint32_t)command, (uint32_t)flags);
	
    multi = ((flags & kUSBHSHubFlagsMultiTTMask) != 0);
    me->_v2ExpansionData->_multiTT[highSpeedHub] = multi;
    USBLog(3,"%s[%p]::DOHSHubMaintenance hub at %d is multiTT:%d", me->getName(), me, highSpeedHub, (uint32_t)me->_v2ExpansionData->_multiTT[highSpeedHub]);
	
    return me->UIMHubMaintenance(highSpeedHub, 0, command, flags);
}



IOReturn
IOUSBControllerV2::DOSetTestMode(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
#pragma unused (arg2, arg3)
    IOUSBControllerV2 *me = (IOUSBControllerV2 *)owner;
    UInt32 mode = (uintptr_t)arg0;
    UInt32 port = (uintptr_t)arg1;
	
    USBLog(5,"%s[%p]::DOSetTestMode, mode: %d, port: %d", me->getName(), me, (uint32_t)mode, (uint32_t)port);
	
    return me->UIMSetTestMode(mode, port);
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  0);
IOReturn		
IOUSBControllerV2::AddHSHub(USBDeviceAddress highSpeedHub, UInt32 flags)
{
    return _commandGate->runAction(DOHSHubMaintenance, (void *)(UInt32)highSpeedHub,
								   (void *)(UInt32)kUSBHSHubCommandAddHub, (void *)flags);
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  1);
IOReturn 		
IOUSBControllerV2::UIMHubMaintenance(USBDeviceAddress highSpeedHub, UInt32 highSpeedPort, UInt32 command, UInt32 flags)
{
#pragma unused (highSpeedHub, highSpeedPort, command, flags)
    return kIOReturnUnsupported;			// not implemented
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  2);
IOReturn		
IOUSBControllerV2::RemoveHSHub(USBDeviceAddress highSpeedHub)
{
    return _commandGate->runAction(DOHSHubMaintenance, (void *)(UInt32)highSpeedHub,
								   (void *)(UInt32)kUSBHSHubCommandRemoveHub, NULL);
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  3);
IOReturn		
IOUSBControllerV2::SetTestMode(UInt32 mode, UInt32 port)
{
    return _commandGate->runAction(DOSetTestMode, (void *)mode, (void *)port);
}

OSMetaClassDefineReservedUsed(IOUSBControllerV2,  4);
IOReturn 		
IOUSBControllerV2::UIMSetTestMode(UInt32 mode, UInt32 port)
{
#pragma unused (mode, port)
    return kIOReturnUnsupported;			// not implemented
}

OSMetaClassDefineReservedUsed(IOUSBControllerV2,  5);
UInt64
IOUSBControllerV2::GetMicroFrameNumber(void)
{
    return 0;			// not implemented
}

OSMetaClassDefineReservedUsed(IOUSBControllerV2,  7);
IOReturn
IOUSBControllerV2::ReadV2(IOMemoryDescriptor *buffer, USBDeviceAddress address, Endpoint *endpoint, IOUSBCompletionWithTimeStamp *completion, UInt32 noDataTimeout, UInt32 completionTimeout, IOByteCount reqCount)
{
    IOReturn				err = kIOReturnSuccess;
    IOUSBCommand			*command;
    IOUSBCompletion			nullCompletion;
    IOUSBCompletion			theCompletion;
	IODMACommand			*dmaCommand = NULL;
    int						i;
	bool					isSyncTransfer = false;
	
    USBLog(7, "%s[%p]::ReadV2 - reqCount = %d", getName(), this, (uint32_t)reqCount);
	
	// USBTrace_Start( kUSBTController, kTPControllerReadV2, (uintptr_t)this, address, endpoint->direction, reqCount );
	
    // Validate its a inny pipe and that there is a buffer
    if ((endpoint->direction != kUSBIn) || !buffer || (buffer->getLength() < reqCount))
    {
        USBLog(5, "%s[%p]::ReadV2 - direction is not kUSBIn (%d), No Buffer, or buffer length < reqCount (%qd < %qd). Returning kIOReturnBadArgument(0x%x)", getName(), this, endpoint->direction,  (uint64_t)buffer->getLength(), (uint64_t)reqCount, kIOReturnBadArgument);
        return kIOReturnBadArgument;
    }
	
    if ((endpoint->transferType != kUSBBulk) && (noDataTimeout || completionTimeout))
    {
        USBLog(5, "%s[%p]::ReadV2 - Pipe is NOT kUSBBulk (%d) AND specified a timeout (%d, %d).  Returning kIOReturnBadArgument(0x%x)", getName(), this, endpoint->transferType, (uint32_t)noDataTimeout, (uint32_t)completionTimeout, kIOReturnBadArgument);
        return kIOReturnBadArgument;							// timeouts only on bulk pipes
    }
	
    // Validate the completion
    if (!completion)
    {
        USBLog(5, "%s[%p]::ReadV2 - No Completion routine.  Returning kIOReturnNoCompletion(0x%x)", getName(), this, kIOReturnNoCompletion);
        return kIOReturnNoCompletion;
    }
	
    // Validate the command gate
    if (!_commandGate)
    {
        USBLog(5, "%s[%p]::ReadV2 - Could not get _commandGate.  Returning kIOReturnInternalError(0x%x)", getName(), this, kIOReturnInternalError);
        return kIOReturnInternalError;
    }
	
    if (  (uintptr_t) completion->action == (uintptr_t) &IOUSBSyncCompletion )
	{
		isSyncTransfer = true;
		// 7889995 - check to see if we are on the workloop thread before setting up the IOUSBCommand
		if ( _workLoop->onThread() )
		{
            USBError(1,"IOUSBControllerV2(%s)[%p]::DoIOTransfer sync request on workloop thread.  Use async!", getName(), this);
            return kIOUSBSyncRequestOnWLThread;
		}
	}
	
	
	// allocate the command
    command = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
	
    // If we couldn't get a command, increase the allocation and try again
    //
    if ( command == NULL )
    {
        IncreaseCommandPool();
		
        command = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
        if ( command == NULL )
        {
            USBLog(3,"%s[%p]::ReadV2 Could not get a IOUSBCommand",getName(),this);
            return kIOReturnNoResources;
        }
    }
	
	if (reqCount)
	{
		dmaCommand = command->GetDMACommand();
		
		if (!dmaCommand)
		{
			USBError(1, "%s[%p]::ReadV2 - no DMA COMMAND", getName(), this);
            return kIOReturnNoResources;
		}
		if (dmaCommand->getMemoryDescriptor())
		{
			IOMemoryDescriptor		*memDesc = (IOMemoryDescriptor *)dmaCommand->getMemoryDescriptor();
			USBError(1, "%s[%p]::ReadV2 - dma command (%p) already contains memory descriptor (%p) - clearing", getName(), this, dmaCommand, memDesc);
			dmaCommand->clearMemoryDescriptor();
		}

		USBLog(7, "%s[%p]::ReadV2 - setting memory descriptor (%p) into dmaCommand (%p)", getName(), this, buffer, dmaCommand);
		err = dmaCommand->setMemoryDescriptor(buffer);
		if (err)
		{
			USBError(1, "%s[%p]::ReadV2 - err(%p) attempting to set the memory descriptor to the dmaCommand", getName(), this, (void*)err);
			_freeUSBCommandPool->returnCommand(command);
			return err;
		}
	}
	
    theCompletion.target = completion->target;
    theCompletion.action = (IOUSBCompletionAction)completion->action;
    theCompletion.parameter = completion->parameter;
	
	// Set up a flag indicating that we have a synchronous request in this command
	//
	command->SetIsSyncTransfer(isSyncTransfer);
	
    command->SetUseTimeStamp(true);
    command->SetSelector(READ);
    command->SetRequest(0);            	// Not a device request
    command->SetAddress(address);
    command->SetEndpoint(endpoint->number);
#ifdef SUPPORTS_SS_USB
    command->SetStreamID(0);
#endif
    command->SetDirection(kUSBIn);
    command->SetType(endpoint->transferType);
    command->SetBuffer(buffer);
    command->SetReqCount(reqCount);
    command->SetClientCompletion(theCompletion);
    command->SetNoDataTimeout(noDataTimeout);
    command->SetCompletionTimeout(completionTimeout);
 	command->SetMultiTransferTransaction(false);
	command->SetFinalTransferInTransaction(false);
	for (i=0; i < 10; i++)
        command->SetUIMScratch(i, 0);
	
    nullCompletion.target = (void *) NULL;
    nullCompletion.action = (IOUSBCompletionAction) NULL;
    nullCompletion.parameter = (void *) NULL;
    command->SetDisjointCompletion(nullCompletion);
	
    err = CheckForDisjointDescriptor(command, endpoint->maxPacketSize);
    if (kIOReturnSuccess == err)
	{
		
        err = _commandGate->runAction(DoIOTransfer, command);
		
		// If we have a sync request, then we always return the command after the DoIOTransfer.  If it's an async request, we only return it if 
		// we get an immediate error
		//
		if ( isSyncTransfer || (kIOReturnSuccess != err) )
		{
			IODMACommand		*dmaCommand = command->GetDMACommand();
			IOMemoryDescriptor	*memDesc = dmaCommand ? (IOMemoryDescriptor	*)dmaCommand->getMemoryDescriptor() : NULL;
			
			nullCompletion = command->GetDisjointCompletion();
			if (nullCompletion.action)
			{
				USBLog(1, "%s[%p]::ReadV2 - SYNC xfer or immediate error with Disjoint Completion", getName(), this);
				USBTrace( kUSBTController, kTPControllerReadV2, (uintptr_t)this, err, 0, 1 );
			}
			if (memDesc)
			{
				USBLog(7, "%s[%p]::ReadV2 - SYNC xfer or immediate error - clearing memDesc (%p) from dmaCommand (%p)", getName(), this, memDesc, dmaCommand);
				dmaCommand->clearMemoryDescriptor();
			}
			_freeUSBCommandPool->returnCommand(command);
		}
	}
	else
	{
		IODMACommand		*dmaCommand = command->GetDMACommand();
		IOMemoryDescriptor	*memDesc = dmaCommand ? (IOMemoryDescriptor	*)dmaCommand->getMemoryDescriptor() : NULL;

		// CheckFordDisjoint returned an error, so free up the comand
		//
		if (memDesc)
		{
			USBLog(7, "%s[%p]::ReadV2 - CheckForDisjointDescriptor error (%p) - clearing memDesc (%p) from dmaCommand (%p)", getName(), this, (void*)err, memDesc, dmaCommand);
			dmaCommand->clearMemoryDescriptor();
		}
		_freeUSBCommandPool->returnCommand(command);
	}
	
	// USBTrace_End( kUSBTController, kTPControllerReadV2, (uintptr_t)this, err);
	
    return err;
}


OSMetaClassDefineReservedUsed(IOUSBControllerV2,  8);
IOReturn
IOUSBControllerV2::UIMCreateIsochEndpoint(short functionAddress, short endpointNumber, UInt32 maxPacketSize, UInt8 direction, USBDeviceAddress	highSpeedHub, int highSpeedPort, UInt8 interval)
{
#pragma unused (interval)
	// this is the "default implementation of UIMCreateIsochEndpoint for UIMs which don't implement it.
	// In those cases the interval parameter is ignored
	return UIMCreateIsochEndpoint(functionAddress, endpointNumber, maxPacketSize, direction, highSpeedHub, highSpeedPort);
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  9);
IOUSBControllerIsochEndpoint*
IOUSBControllerV2::AllocateIsochEP()
{
	USBError(1, "IOUSBControllerV2[%p]::AllocateIsochEP - should be overriden in a subclass", this);
	return NULL;
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  10);
IOReturn
IOUSBControllerV2::DeallocateIsochEP(IOUSBControllerIsochEndpoint* pEP)
{
	USBLog(4, "%s[%p]::DeallocateIsochEP (%p)",getName(), this, pEP);
    pEP->nextEP = _freeIsochEPList;
    _freeIsochEPList = pEP;
    return kIOReturnSuccess;
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  11);
IOUSBControllerIsochEndpoint*
IOUSBControllerV2::FindIsochronousEndpoint(short								functionAddress,
										   short								endpointNumber,
										   short								direction,
										   IOUSBControllerIsochEndpoint			**ppEPBack)
{
    IOUSBControllerIsochEndpoint		*pEP, *pBack;
    
    pEP = _isochEPList;
    pBack = NULL;
    while (pEP)
    {
		if ((pEP->functionAddress == functionAddress)
			&& (pEP->endpointNumber == endpointNumber)
			&& (pEP->direction == direction))
			break;
		pBack = pEP;
		pEP = pEP->nextEP;
    }
    if (pEP && ppEPBack)
		*ppEPBack = pBack;
    return pEP;
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  12);
IOUSBControllerIsochEndpoint*
IOUSBControllerV2::CreateIsochronousEndpoint(short					functionAddress,
											 short					endpointNumber,
											 short 					direction)
{
    IOUSBControllerIsochEndpoint*			pEP;
    
    USBLog(4, "+%s[%p]::CreateIsochronousEndpoint (%d:%d:%d)", getName(), this, functionAddress, endpointNumber, direction);
	pEP = _freeIsochEPList;
	if (!pEP)
	{
		pEP = AllocateIsochEP();
		USBLog(4, "%s[%p]::CreateIsochronousEndpoint  called AllocateIsochEP (%p)",getName(), this, pEP);
	}
	if (pEP)
	{
		_freeIsochEPList = pEP->nextEP;									// unlink from free list
		pEP->init();													// make sure to reinitialize it
		pEP->nextEP = _isochEPList;
		_isochEPList = pEP;
		pEP->functionAddress = functionAddress;
		pEP->endpointNumber = endpointNumber;
		pEP->direction = direction;
	}
    return pEP;
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  13);
void
IOUSBControllerV2::PutTDonToDoList(IOUSBControllerIsochEndpoint* pED, IOUSBControllerIsochListElement *pTD)
{
    USBLog(7, "AppleUSBEHCI[%p]::PutTDonToDoList - pED (%p) pTD (%p) frameNumber(%qx)", this, pED, pTD, pTD->_frameNumber);
    // Link TD into todo list
    if (pED->toDoList == NULL)
    {
		// as the head of a new list
		pED->toDoList = pTD;
    }
    else
    {
		// at the tail of the old list
		pED->toDoEnd->_logicalNext = pTD;
    }
    // no matter what we are the new tail
    pED->toDoEnd = pTD;
	pED->onToDoList++;
    pED->activeTDs++;
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  14);
IOUSBControllerIsochListElement *
IOUSBControllerV2::GetTDfromToDoList(IOUSBControllerIsochEndpoint* pED)
{
    IOUSBControllerIsochListElement	*pTD;
    
	// Do not call USBLog here, as this can be called from AddIsocFramesToSchedule, which holds off preemption
    pTD = pED->toDoList;
    if (pTD)
    {
		if (pTD == pED->toDoEnd)
			pED->toDoList = pED->toDoEnd = NULL;
		else
			pED->toDoList = OSDynamicCast(IOUSBControllerIsochListElement, pTD->_logicalNext);
		// USBLog(7, "AppleUSBEHCI[%p]::GetTDfromToDoList - pED (%p) pTD (%p) frameNumber(%qx)", this, pED, pTD, pTD->_frameNumber);
		pED->onToDoList--;
    }
    return pTD;
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  15);
void
IOUSBControllerV2::PutTDonDoneQueue(IOUSBControllerIsochEndpoint* pED, IOUSBControllerIsochListElement *pTD, bool checkDeferred)
{
    IOUSBControllerIsochListElement	*deferredTD;
	
	if ((pED->doneQueue != NULL) && (pED->doneEnd == NULL))
	{
		IOUSBControllerIsochListElement		*lastTD;
		
		// I cannot log a message here because we are running with pre-emption turned off
		// Instead, for rdar://6488628, I will attempt to patch up the pED. If the event detected here was present before we 
		// disabled pre-emption, there should be an error message in the system log.
		lastTD = pED->doneQueue;
		while (lastTD->_logicalNext != NULL)
			lastTD = (IOUSBControllerIsochListElement*)lastTD->_logicalNext;
		pED->doneEnd = lastTD;
	}
	
	// Do not call USBLog here, as this can be called from AddIsocFramesToSchedule, which holds off preemption
    if (checkDeferred)
    {
		while (pED->deferredQueue && (pED->deferredQueue->_frameNumber < pTD->_frameNumber))
		{
			deferredTD = GetTDfromDeferredQueue(pED);
			PutTDonDoneQueue(pED, deferredTD, false);
		}
    }
    
    if (pED->doneQueue == NULL)
    {
		// as the head of a new list
		pED->doneQueue = pTD;
    }
    else
    {
		// at the tail of the old list
		pED->doneEnd->_logicalNext = pTD;
    }
    // and not matter what we are now the new tail
    pED->doneEnd = pTD;
	pED->onDoneQueue++;
	
	// if there are no TDs on the schedule, and no TDs on the toDO list, then we should clear out the deferred queue
	if (checkDeferred && (pED->scheduledTDs == 0) && !pED->toDoList)
	{
		deferredTD = GetTDfromDeferredQueue(pED);
		while (deferredTD)
		{
			PutTDonDoneQueue(pED, deferredTD, false);
			deferredTD = GetTDfromDeferredQueue(pED);
		}
	}
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  16);
IOUSBControllerIsochListElement *
IOUSBControllerV2::GetTDfromDoneQueue(IOUSBControllerIsochEndpoint* pED)
{
    IOUSBControllerIsochListElement	*pTD;
    
    pTD = pED->doneQueue;
    if (pTD)
    {
		if (pTD == pED->doneEnd)
			pED->doneQueue = pED->doneEnd = NULL;
		else
			pED->doneQueue = OSDynamicCast(IOUSBControllerIsochListElement, pTD->_logicalNext);
		pED->onDoneQueue--;
		pED->activeTDs--;
    }
    return pTD;
}



// this is a static method that is inside the command gate (called through runAction)
IOReturn		
IOUSBControllerV2::GatedGetTDfromDoneQueue(OSObject *target, void *arg0, void *arg1, void*, void*)
{
	IOUSBControllerV2				*me = (IOUSBControllerV2 *)target;
	IOUSBControllerIsochEndpoint	*pED = (IOUSBControllerIsochEndpoint*)arg0;
	IOUSBControllerIsochListElement *pTD;
	
	if (!me || !pED)
		return kIOReturnInternalError;
	
	pTD = me->GetTDfromDoneQueue(pED);
	*(IOUSBControllerIsochListElement**)arg1 = pTD;
	
	return kIOReturnSuccess;
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  17);
void
IOUSBControllerV2::PutTDonDeferredQueue(IOUSBControllerIsochEndpoint* pED, IOUSBControllerIsochListElement *pTD)
{
	// Do not call USBLog here, as this can be called from AddIsocFramesToSchedule, which holds off preemption
	// USBLog(7, "AppleUSBEHCI[%p]::PutTDonDeferredQueue(%p, %p)", this, pED, pTD);
	
    if (pED->deferredQueue == NULL)
    {
		// as the head of a new list
		pED->deferredQueue = pTD;
    }
    else
    {
		// at the tail of the old list
		pED->deferredEnd->_logicalNext = pTD;
    }
    // and not matter what we are now the new tail
    pED->deferredEnd = pTD;
	pED->deferredTDs++;
	
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  18);
IOUSBControllerIsochListElement *
IOUSBControllerV2::GetTDfromDeferredQueue(IOUSBControllerIsochEndpoint* pED)
{
    IOUSBControllerIsochListElement	*pTD;
    
    pTD = pED->deferredQueue;
    if (pTD)
    {
		if (pTD == pED->deferredEnd)
			pED->deferredQueue = pED->deferredEnd = NULL;
		else
			pED->deferredQueue = OSDynamicCast(IOUSBControllerIsochListElement, pTD->_logicalNext);
		pED->deferredTDs--;
    }
	USBLog(7, "AppleUSBEHCI[%p]::GetTDfromDeferredQueue(%p) returning %p", this, pED, pTD);
    return pTD;
}


// this is a static method - hence no slot
void
IOUSBControllerV2::ReturnIsochDoneQueueEntry(OSObject *target, thread_call_param_t endpointPtr)
{
    IOUSBControllerV2 *					me = OSDynamicCast(IOUSBControllerV2, target);
    IOUSBControllerIsochEndpoint*		pEP = (IOUSBControllerIsochEndpoint*) endpointPtr;
	
    if (!me || !pEP)
        return;
	
    me->retain();
	me->ReturnIsochDoneQueue(pEP);
    me->release();
}



OSMetaClassDefineReservedUsed(IOUSBControllerV2,  19);
void 
IOUSBControllerV2::ReturnIsochDoneQueue(IOUSBControllerIsochEndpoint* pEP)
{
    IOUSBControllerIsochListElement		*pTD = NULL;
    IOUSBIsocFrame						*pFrames = NULL;
	IOUSBIsocCompletionAction			pHandler;
	uint32_t							busFunctEP;
	
    USBLog(7, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue (%p)", this, pEP);
	_commandGate->runAction(GatedGetTDfromDoneQueue, pEP, &pTD);
	
	USBTrace_Start( kUSBTController, kTPControllerReturnIsochDoneQueue, (uintptr_t)this, (uintptr_t)pEP, (uintptr_t)pTD, 0 );
    
    if (pTD)
    {
		pFrames = pTD->_pFrames;
    }
	else
	{
		USBLog(7, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue - no TDs to return", this);
	}
    while(pTD)
    {
		USBLog(7, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue: TD %p", this, pTD); 
		if ( pTD->_completion.action != NULL)
		{
			busFunctEP = ((_busNumber << 16 ) | ( pEP->functionAddress << 8) | (pEP->endpointNumber) );
			pHandler = pTD->_completion.action;
			pTD->_completion.action = NULL;
				
			if (pEP->accumulatedStatus == kIOUSBBufferUnderrunErr)
			{
				USBLog(1, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue - kIOReturnBufferUnderrunErr (PCI issue perhaps)  Bus: %x, Address: %d, Endpoint: %d", this, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber);
				USBTrace( kUSBTController, kTPControllerReturnIsochDoneQueue, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber, 1);
			}
			if (pEP->accumulatedStatus == kIOUSBBufferOverrunErr)
			{
				USBLog(1, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue - kIOReturnBufferOverrunErr (PCI issue perhaps)  Bus: %x, Address: %d, Endpoint: %d", this, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber);
				USBTrace( kUSBTController, kTPControllerReturnIsochDoneQueue, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber, 2);
			}
			if ((pEP->accumulatedStatus == kIOReturnOverrun) && (pEP->direction == kUSBIn))
			{
				USBLog(1, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue - kIOReturnOverrun on IN - device babbling?  Bus: %x, Address: %d, Endpoint: %d", this, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber);
				USBTrace( kUSBTController, kTPControllerReturnIsochDoneQueue, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber, 3);
			}

			USBLog(7, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue- TD (%p) calling handler[%p](target: %p, comp.param: %p, status: %p (%s), pFrames: %p)  Bus: %x, Address: %d, Endpoint: %d", this, pTD,
																pHandler, pTD->_completion.target, pTD->_completion.parameter, (void*)pEP->accumulatedStatus, USBStringFromReturn(pEP->accumulatedStatus), pFrames, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber);
			
			USBTrace( kUSBTController, kTPControllerReturnIsochDoneQueue, (uint32_t)busFunctEP, (uintptr_t)pHandler, (uint32_t)pEP->accumulatedStatus, 5);
                        
			(*pHandler) (pTD->_completion.target,  pTD->_completion.parameter, pEP->accumulatedStatus, pFrames);
			
			_activeIsochTransfers--;
			if ( _activeIsochTransfers < 0 )
			{
				USBLog(1, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue - _activeIsochTransfers went negative (%d).  We lost one somewhere  Bus: %x, Address: %d, Endpoint: %d", this, (uint32_t)_activeIsochTransfers, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber);
				USBTrace( kUSBTController, kTPControllerReturnIsochDoneQueue, (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber,  4);
			}
			else if (!_activeIsochTransfers && (_expansionData->_isochMaxBusStall != 0))
            {
				requireMaxBusStall(0);										// remove maximum stall restraint on the PCI bus
                if (metaCast("AppleUSBUHCI"))
                    requireMaxInterruptDelay(0);
            }
			
			// if the accumulated status is aborted, then we need to keep that status until we are done
			// otherwise the status will be in the endpoint when we get to the callback case and will
			// be reset afterwards
			if (pEP->accumulatedStatus != kIOReturnAborted)
			{
				if (pEP->accumulatedStatus != kIOReturnSuccess && (pEP->accumulatedStatus != kIOReturnUnderrun) )
				{
					USBLog(6, "IOUSBControllerV2[%p]::ReturnIsocDoneQueue - resetting status from 0x%x (%s)  Bus: %x, Address: %d, Endpoint: %d", this,  pEP->accumulatedStatus, USBStringFromReturn(pEP->accumulatedStatus), (uint32_t)_busNumber, pEP->functionAddress,  pEP->endpointNumber);
				}
				pEP->accumulatedStatus = kIOReturnSuccess;
			}
			pTD->Deallocate(this);
			pTD = NULL;
			_commandGate->runAction(GatedGetTDfromDoneQueue, pEP, &pTD);
			if (pTD)
				pFrames = pTD->_pFrames;
		}
		else
		{
			pTD->Deallocate(this);
			pTD = NULL;
			_commandGate->runAction(GatedGetTDfromDoneQueue, pEP, &pTD);
		}
    }
	
	USBTrace_End( kUSBTController, kTPControllerReturnIsochDoneQueue, (uintptr_t)this, 0, 0, 0);
}



// This method should be overridden by any controller which can support >4GB physical addresses (32 bits addresses)
OSMetaClassDefineReservedUsed(IOUSBControllerV2,  20);
IODMACommand*
IOUSBControllerV2::GetNewDMACommand()
{
	return IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, 0);
}



#define defaultOptionBits						0							// by default we don't need contiguous memory
#define defaultPhysicalMask						0x00000000FFFFF000ULL		// by default we require memory 4K aligned memory below 4GB
OSMetaClassDefineReservedUsed(IOUSBControllerV2,  21);
IOReturn
IOUSBControllerV2::GetLowLatencyOptionsAndPhysicalMask(IOOptionBits *pOptionBits, mach_vm_address_t *pPhysicalMask)
{
	*pOptionBits = defaultOptionBits;
	*pPhysicalMask = defaultPhysicalMask;
	return kIOReturnSuccess;
}


OSMetaClassDefineReservedUsed(IOUSBControllerV2,  22);
IOReturn
IOUSBControllerV2::GetFrameNumberWithTime(UInt64* frameNumber, AbsoluteTime *theTime)
{
#pragma unused (frameNumber, theTime)
	return kIOReturnUnsupported;
}


#ifdef SUPPORTS_SS_USB
OSMetaClassDefineReservedUsed(IOUSBControllerV2,  23);
IOReturn 
IOUSBControllerV2::ReadStream(UInt32 streamID, IOMemoryDescriptor *buffer, USBDeviceAddress address, Endpoint *endpoint, IOUSBCompletion *completion, UInt32 noDataTimeout, UInt32 completionTimeout, IOByteCount reqCount)
{
    IOReturn			err = kIOReturnSuccess;
    IOUSBCommand *		command = NULL;
	IODMACommand *		dmaCommand = NULL;
    IOUSBCompletion 	nullCompletion;
    int					i;
	bool				isSyncTransfer = false;
    
    USBLog(7, "%s[%p]::Read - reqCount = %qd", getName(), this, (uint64_t)reqCount);
    
    // Validate its a inny pipe and that there is a buffer
    if ((endpoint->direction != kUSBIn) || !buffer || (buffer->getLength() < reqCount))
    {
        USBLog(2, "%s[%p]::Read - direction is not kUSBIn (%d), No Buffer, or buffer length < reqCount (%qd < %qd). Returning kIOReturnBadArgument(0x%x)", getName(), this, endpoint->direction,  (uint64_t)buffer->getLength(), (uint64_t)reqCount, kIOReturnBadArgument);
		return kIOReturnBadArgument;
    }
    
    if ((endpoint->transferType != kUSBBulk) && (noDataTimeout || completionTimeout))
    {
        USBLog(2, "%s[%p]::Read - Pipe is NOT kUSBBulk (%d) AND specified a timeout (%d, %d).  Returning kIOReturnBadArgument(0x%x)", getName(), this, endpoint->transferType, (uint32_t)noDataTimeout, (uint32_t)completionTimeout, kIOReturnBadArgument);
		return kIOReturnBadArgument; // timeouts only on bulk pipes
    }
    
    // Validate the completion
    if (!completion)
    {
        USBLog(2, "%s[%p]::Read - No Completion routine.  Returning kIOReturnNoCompletion(0x%x)", getName(), this, kIOReturnNoCompletion);
		return kIOReturnNoCompletion;
    }
    
    // Validate the command gate
    if (!_commandGate)
    {
        USBLog(1, "%s[%p]::Read - Could not get _commandGate.  Returning kIOReturnInternalError(0x%x)", getName(), this, kIOReturnInternalError);
		USBTrace( kUSBTController, kTPControllerRead, (uintptr_t)this, kIOReturnInternalError, 0, 1 );
		return kIOReturnInternalError;
    }
    
    if (  (uintptr_t) completion->action == (uintptr_t) &IOUSBSyncCompletion )
	{
		isSyncTransfer = true;
		// 7889995 - check to see if we are on the workloop thread before setting up the IOUSBCommand
		if ( _workLoop->onThread() )
		{
            USBError(1,"IOUSBController(%s)[%p]::Read sync request on workloop thread.  Use async!", getName(), this);
            return kIOUSBSyncRequestOnWLThread;
		}
	}
	
	
    // allocate the command
    command = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
	
    // If we couldn't get a command, increase the allocation and try again
    //
    if ( command == NULL )
    {
        IncreaseCommandPool();
        
        command = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
        if ( command == NULL )
        {
            USBLog(1,"%s[%p]::Read Could not get a IOUSBCommand",getName(),this);
            return kIOReturnNoResources;
        }
    }
	
	if (reqCount)
	{
		IOMemoryDescriptor		*memDesc;
        
		dmaCommand = command->GetDMACommand();
		
		if (!dmaCommand)
		{
			USBLog(1, "%s[%p]::Read - no DMA COMMAND", getName(), this);
			USBTrace( kUSBTController, kTPControllerRead, (uintptr_t)this, kIOReturnNoResources, 0, 2 );
            err = kIOReturnNoResources;
		}
		else
		{
            
			memDesc = (IOMemoryDescriptor*)dmaCommand->getMemoryDescriptor();
			if (memDesc)
			{
				USBLog(1, "%s[%p]::Read - dmaCommand (%p) already contains memory descriptor (%p) - clearing", getName(), this, dmaCommand, memDesc);
				USBTrace( kUSBTController, kTPControllerRead, (uintptr_t)this, (uintptr_t)dmaCommand, (uintptr_t)memDesc, 3 );
				dmaCommand->clearMemoryDescriptor();
			}
			USBLog(7, "%s[%p]::Read - setting memory descriptor (%p) into dmaCommand (%p)", getName(), this, buffer, dmaCommand);
			err = dmaCommand->setMemoryDescriptor(buffer);
			if (err)
			{
				USBLog(1, "%s[%p]::Read - err(%p) attempting to set the memory descriptor to the dmaCommand", getName(), this, (void*)err);
				USBTrace( kUSBTController, kTPControllerRead, (uintptr_t)this, err, 0, 5 );
			}
		}
	}
    
	if (!err)
	{
        
		command->SetIsSyncTransfer(isSyncTransfer);
		command->SetUseTimeStamp(false);
		command->SetSelector(READ);
		command->SetRequest(0);            	// Not a device request
		command->SetAddress(address);
		command->SetEndpoint(endpoint->number);
		command->SetDirection(kUSBIn);
		command->SetType(endpoint->transferType);
		command->SetBuffer(buffer);
		command->SetReqCount(reqCount);
		command->SetClientCompletion(*completion);
		command->SetNoDataTimeout(noDataTimeout);
		command->SetCompletionTimeout(completionTimeout);
		command->SetMultiTransferTransaction(false);
		command->SetFinalTransferInTransaction(false);
        command->SetStreamID(streamID);
		for (i=0; i < 10; i++)
			command->SetUIMScratch(i, 0);
		
		nullCompletion.target = (void *) NULL;
		nullCompletion.action = (IOUSBCompletionAction) NULL;
		nullCompletion.parameter = (void *) NULL;
		command->SetDisjointCompletion(nullCompletion);
		
		err = CheckForDisjointDescriptor(command, endpoint->maxPacketSize);
		if (!err)
		{			
			err = _commandGate->runAction(DoIOTransfer, command);
		}
	}
    
	// 7455477: handle and and all errors which may have occured above
	// If we have a sync request, then we always return the command after the DoIOTransfer.  If it's an async request, we only return it if 
	// we get an immediate error
	//
	if ( isSyncTransfer || (kIOReturnSuccess != err) )
	{
		IOMemoryDescriptor	*memDesc = dmaCommand ? (IOMemoryDescriptor	*)dmaCommand->getMemoryDescriptor() : NULL;
		
		if (!isSyncTransfer)
		{
			USBLog(2, "%s[%p]::Read - General error (%p) - cleaning up - command(%p) dmaCommand(%p)", getName(), this, (void*)err, command, dmaCommand);
		}
		
		if (memDesc)
		{
			USBLog(7, "%s[%p]::Read - sync xfer or err return - clearing memory descriptor (%p) from dmaCommand (%p)", getName(), this, memDesc, dmaCommand);
			dmaCommand->clearMemoryDescriptor();
		}
		nullCompletion = command->GetDisjointCompletion();
		if (nullCompletion.action)
		{
			USBLog(2, "%s[%p]::Read - SYNC xfer or immediate error with Disjoint Completion", getName(), this);
		}
		_freeUSBCommandPool->returnCommand(command);
	}
	
	return err;
}


OSMetaClassDefineReservedUsed(IOUSBControllerV2,  24);
IOReturn 
IOUSBControllerV2::WriteStream(UInt32 streamID, IOMemoryDescriptor *buffer, USBDeviceAddress address, Endpoint *endpoint, IOUSBCompletion *completion, UInt32 noDataTimeout, UInt32 completionTimeout, IOByteCount reqCount)
{
    IOReturn				err = kIOReturnSuccess;
    IOUSBCommand *			command = NULL;
	IODMACommand *			dmaCommand = NULL;
    IOUSBCompletion			nullCompletion;
    int						i;
	bool					isSyncTransfer = false;
	
    USBLog(7, "%s[%p]::Write - reqCount = %qd", getName(), this, (uint64_t)reqCount);
    
    // Validate its a outty pipe and that we have a buffer
    if ((endpoint->direction != kUSBOut) || !buffer || (buffer->getLength() < reqCount))
    {
        USBLog(5, "%s[%p]::Write - direction is not kUSBOut (%d), No Buffer, or buffer length < reqCount (%qd < %qd). Returning kIOReturnBadArgument(0x%x)", getName(), this, endpoint->direction,  (uint64_t)buffer->getLength(), (uint64_t)reqCount, kIOReturnBadArgument);
		return kIOReturnBadArgument;
    }
	
    if ((endpoint->transferType != kUSBBulk) && (noDataTimeout || completionTimeout))
    {
        USBLog(5, "%s[%p]::Write - Pipe is NOT kUSBBulk (%d) AND specified a timeout (%d, %d).  Returning kIOReturnBadArgument(0x%x)", getName(), this, endpoint->transferType, (uint32_t)noDataTimeout, (uint32_t)completionTimeout, kIOReturnBadArgument);
		return kIOReturnBadArgument;							// timeouts only on bulk pipes
    }
	
    // Validate the command gate
    if (!_commandGate)
    {
        USBLog(5, "%s[%p]::Write - Could not get _commandGate.  Returning kIOReturnInternalError(0x%x)", getName(), this, kIOReturnInternalError);
		return kIOReturnInternalError;
    }
	
    if (  (uintptr_t) completion->action == (uintptr_t) &IOUSBSyncCompletion )
	{
		isSyncTransfer = true;
		// 7889995 - check to see if we are on the workloop thread before setting up the IOUSBCommand
		if ( _workLoop->onThread() )
		{
            USBError(1,"IOUSBController(%s)[%p]::Write sync request on workloop thread.  Use async!", getName(), this);
            return kIOUSBSyncRequestOnWLThread;
		}
	}
	
	
    // allocate the command
    command = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
    
    // If we couldn't get a command, increase the allocation and try again
    //
    if ( command == NULL )
    {
        IncreaseCommandPool();
        
        command = (IOUSBCommand *)_freeUSBCommandPool->getCommand(false);
        if ( command == NULL )
        {
            USBLog(3,"%s[%p]::Write Could not get a IOUSBCommand",getName(),this);
            return kIOReturnNoResources;
        }
    }
	
	// 7455477: from this point forward, we have the command object, and we need to be careful to put it back if there is an error..
	if (reqCount)
	{
		IOMemoryDescriptor	*memDesc;
		
		dmaCommand = command->GetDMACommand();
		
		if (!dmaCommand)
		{
			USBLog(1, "%s[%p]::Write - no DMA COMMAND", getName(), this);
			USBTrace( kUSBTController, kTPControllerWrite, (uintptr_t)this, kIOReturnNoResources, 0, 1 );
            err = kIOReturnNoResources;
		}
		else
		{
			memDesc = (IOMemoryDescriptor *)dmaCommand->getMemoryDescriptor();
			if (memDesc)
			{
				USBLog(1, "%s[%p]::Write - dmaCommand (%p) already contains memory descriptor (%p) - clearing", getName(), this, dmaCommand, memDesc);
				USBTrace( kUSBTController, kTPControllerWrite, (uintptr_t)this, (uintptr_t)dmaCommand, (uintptr_t)memDesc, 2 );
				dmaCommand->clearMemoryDescriptor();
			}
			USBLog(7, "%s[%p]::Write - setting memory descriptor (%p) into dmaCommand (%p)", getName(), this, buffer, dmaCommand);
			err = dmaCommand->setMemoryDescriptor(buffer);
			if (err)
			{
				USBTrace( kUSBTController, kTPControllerWrite, (uintptr_t)this, err, 0, 4 );
				USBLog(1, "%s[%p]::Write - err(%p) attempting to set the memory descriptor to the dmaCommand", getName(), this, (void*)err);
			}
		}
		
	}
	
	if (!err)
	{
		command->SetIsSyncTransfer(isSyncTransfer);
		command->SetUseTimeStamp(false);
		command->SetSelector(WRITE);
		command->SetRequest(0);            // Not a device request
		command->SetAddress(address);
		command->SetEndpoint(endpoint->number);
		command->SetDirection(kUSBOut);
		command->SetType(endpoint->transferType);
		command->SetBuffer(buffer);
		command->SetReqCount(reqCount);
		command->SetClientCompletion(*completion);
		command->SetNoDataTimeout(noDataTimeout); 
		command->SetCompletionTimeout(completionTimeout);
		command->SetMultiTransferTransaction(false);
		command->SetFinalTransferInTransaction(false);
		command->SetStreamID(streamID);
		for (i=0; i < 10; i++)
			command->SetUIMScratch(i, 0);
		
		nullCompletion.target = (void *) NULL;
		nullCompletion.action = (IOUSBCompletionAction) NULL;
		nullCompletion.parameter = (void *) NULL;
		command->SetDisjointCompletion(nullCompletion);
		
		err = CheckForDisjointDescriptor(command, endpoint->maxPacketSize);
		if (!err)
		{			
			err = _commandGate->runAction(DoIOTransfer, command);
		}
	}
	
	// 7455477: handle and and all errors which may have occured above
	// If we have a sync request, then we always return the command after the DoIOTransfer.  If it's an async request, we only return it if 
	// we get an immediate error
	//
	if ( isSyncTransfer || (kIOReturnSuccess != err) )
	{
		IOMemoryDescriptor	*memDesc = dmaCommand ? (IOMemoryDescriptor	*)dmaCommand->getMemoryDescriptor() : NULL;
		
		if (!isSyncTransfer)
		{
			USBLog(2, "%s[%p]::Write - General error (%p) - cleaning up - command(%p) dmaCommand(%p)", getName(), this, (void*)err, command, dmaCommand);
		}
		
		if (memDesc)
		{
			USBLog(7, "%s[%p]::Write - General error (%p) - clearing memory descriptor (%p) from dmaCommand (%p)", getName(), this, (void*)err, memDesc, dmaCommand);
			dmaCommand->clearMemoryDescriptor();
		}
		nullCompletion = command->GetDisjointCompletion();
		if (nullCompletion.action)
		{
			USBLog(2, "%s[%p]::Write - SYNC xfer or immediate error with Disjoint Completion", getName(), this);
		}
		_freeUSBCommandPool->returnCommand(command);
	}
	
    return err;
}
#else
OSMetaClassDefineReservedUnused(IOUSBControllerV2,  23);
OSMetaClassDefineReservedUnused(IOUSBControllerV2,  24);
OSMetaClassDefineReservedUnused(IOUSBControllerV2,  25);
#endif

OSMetaClassDefineReservedUnused(IOUSBControllerV2,  26);
OSMetaClassDefineReservedUnused(IOUSBControllerV2,  27);
OSMetaClassDefineReservedUnused(IOUSBControllerV2,  28);
OSMetaClassDefineReservedUnused(IOUSBControllerV2,  29);

