/*
 *	Davicom DM9601 USB 1.1 Ethernet Driver.
 *	Copyright (c) 2013 Samuel Vinson <samuelv0304@gmail.com>
 *	Copyright (c) 2008, 2011 Siarzhuk Zharski <imker@gmx.li>
 *	Copyright (c) 2009 Adrien Destugues <pulkomandy@gmail.com>
 *	Distributed under the terms of the MIT license.
 */

#ifndef USBCDCEthernet_DM9601_h
#define USBCDCEthernet_DM9601_h

// Vendor Requests

enum {
    kVenReqReadRegister			= 0,
    kVenReqWriteRegister		= 1,
    kVenReqWriteRegisterByte	= 3,
};

enum DM9601Registers {
    RegNCR	= 0x00,	// Network Control Register
    NCRExtPHY	= 0x80,	// Select External PHY
    NCRFullDX	= 0x08,	// Full duplex
    NCRLoopback	= 0x06,	// Internal PHY analog loopback
    
    RegNSR	= 0x01,	// Network Status Register
    NSRSpeed10	= 0x80,	// 0 = 100MBps, 1 = 10MBps (internal PHY)
    NSRLinkUp	= 0x40,	// 1 = link up (internal PHY)
    NSRTXFull	= 0x10,	// TX FIFO full
    NSRRXOver	= 0x08,	// RX FIFO overflow
    
    RegRCR	= 0x05,	// RX Control Register
    RCRDiscardLong	= 0x20,	// Discard long packet (over 1522 bytes)
    RCRDiscardCRC	= 0x10,	// Discard CRC error packet
    RCRAllMulticast	= 0x08,	// Pass all multicast
    RCRPromiscuous	= 0x02,	// Promiscuous
    RCRRXEnable		= 0x01,	// RX enable
    
    RegEPCR	= 0x0b,	// EEPROM & PHY Control Register
    EPCROpSelect	= 0x08,	// EEPROM or PHY Operation Select
    EPCRRegRead		= 0x04,	// EEPROM or PHY Register Read Command
    EPCRRegWrite	= 0x02,	// EEPROM or PHY Register Write Command
    
    RegEPAR	= 0x0c,	// EEPROM & PHY Address Register
    EPARIntPHY		= 0x40,	// [7:6] force to 01 if Internal PHY is selected
    EPARMask		= 0x1f,	// mask [0:5]
    
    RegEPDRL = 0x0d, // EEPROM & PHY Low Byte Data Register
    
    RegEPDRH = 0x0e, // EEPROM & PHY Low Byte Data Register
    
    RegPAR	= 0x10,	// [0x10 - 0x15] Physical Address Register
    
    RegMAR	= 0x16,	// [0x16 - 0x1d] Multicast Address Register
    
    RegGPCR	= 0x1E,	// General Purpose Control Register
    GPCRPowerDown	= 0x01,	// [0:6] Define in/out direction of GPCR
    // GPIO0 - is output for Power Down function
    
    RegGPR	= 0x1F,	// General Purpose Register
    GPRPowerDownInPHY = 0x01,	// Power down Internal PHY
    
    RegUSBC	= 0xf4, // USB Control Register
    USBCIntAck		= 0x20,	// ACK with 8-bytes of data on interrupt EP
    USBCIntNAck		= 0x10,	// Supress ACK on interrupt EP
    
};

#endif
