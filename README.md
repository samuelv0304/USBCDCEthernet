USBCDCEthernet
==============

This implements a driver for the USB-Ethernet adapter based on Davicom DM9601 on MacOS X, such as JP1081B/No:9700

Why does this project exist?
----------------------------

The official driver of JP1081B/No:9700 doesn't have a permanent MAC address. The MAC address is randomly defined. Also, it's impossible to define an IP address from MAC address.

How do I install it?
--------------------

- Download the source here from Github and compile it with XCode
- Determine the path of the built kext and load it with 'sudo' command. An example is shown below:
```
sudo chown -R root:wheel /Users/jay/Library/Developer/Xcode/DerivedData/USBCDCEthernet-dnnalxxjhiskovetybdyuisyohly/Build/Products/Debug/USBCDCEthernet.kext
sudo chmod -R 755 /Users/jay/Library/Developer/Xcode/DerivedData/USBCDCEthernet-dnnalxxjhiskovetybdyuisyohly/Build/Products/Debug/USBCDCEthernet.kext
sudo kextload /Users/jay/Library/Developer/Xcode/DerivedData/USBCDCEthernet-dnnalxxjhiskovetybdyuisyohly/Build/Products/Debug/USBCDCEthernet.kext
```


Thanks and Acknowledgements
---------------------------

Driver Davicom from Haiku project.
Based on USBCDCEthernet example from IOUSBFamily-560.4.2.

License
-------

This code is licensed under Apple Public Source License. See LICENSE for more details.

