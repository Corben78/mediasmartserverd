/////////////////////////////////////////////////////////////////////////////
/// @file device_monitor.cpp
///
/// Device monitoring (disk add/removal etc)
///
/// -------------------------------------------------------------------------
///
/// Copyright (c) 2009-2010 Chris Byrne, Brian Teague
/// 
/// This software is provided 'as-is', without any express or implied
/// warranty. In no event will the authors be held liable for any damages
/// arising from the use of this software.
/// 
/// Permission is granted to anyone to use this software for any purpose,
/// including commercial applications, and to alter it and redistribute it
/// freely, subject to the following restrictions:
/// 
/// 1. The origin of this software must not be misrepresented; you must not
/// claim that you wrote the original software. If you use this software
/// in a product, an acknowledgment in the product documentation would be
/// appreciated but is not required.
/// 
/// 2. Altered source versions must be plainly marked as such, and must not
/// be misrepresented as being the original software.
/// 
/// 3. This notice may not be removed or altered from any source
/// distribution.
///
/////////////////////////////////////////////////////////////////////////////

//- includes
#include "device_monitor.h"
#include "errno_exception.h"
#include "mediasmartserverd.h"
#include <iostream>
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string>
#include <fstream>
extern "C" {
#include <libudev.h>
}

/////////////////////////////////////////////////////////////////////////////
/// constructor
DeviceMonitor::DeviceMonitor( )
        :       dev_context_( 0 )
	,	dev_monitor_( 0 )
	,	num_disks_( 0 )
{ 
    disk_led_map_["/dev/sda"] = 0;
    disk_led_map_["/dev/sdb"] = 1;
    disk_led_map_["/dev/sdc"] = 2;
    disk_led_map_["/dev/sdd"] = 3;
}
	
/////////////////////////////////////////////////////////////////////////////
/// destructor
DeviceMonitor::~DeviceMonitor( ) {
	if ( dev_context_ ) udev_unref( dev_context_ );
	if ( dev_monitor_ ) udev_monitor_unref( dev_monitor_ );
}

/////////////////////////////////////////////////////////////////////////////
/// intialise
void DeviceMonitor::Init( const LedControlPtr& leds ) {
	leds_ = leds;
	
	// get udev library context
	dev_context_ = udev_new();
	if ( !dev_context_ ) throw ErrnoException( "udev_new" );
	
	// set up udev monitor
	dev_monitor_ = udev_monitor_new_from_netlink( dev_context_, "udev" );
	if ( !dev_monitor_ ) throw ErrnoException( "udev_monitor_new_from_netlink" );
	
	// only interested in scsi devices
	if ( udev_monitor_filter_add_match_subsystem_devtype( dev_monitor_, "scsi", "disk" ) ) {
		throw ErrnoException( "udev_monitor_filter_add_match_subsystem_devtype" );
	}
	
	// enumerate existing devices
	enumDevices_( );
	
	// then start monitoring
	if ( udev_monitor_enable_receiving( dev_monitor_ ) ) {
		throw ErrnoException( "udev_monitor_enable_receiving" );
	}
}

/////////////////////////////////////////////////////////////////////////////
/// main looop
void DeviceMonitor::Main( ) {
	assert( dev_monitor_ );
	
	const int fd_mon = udev_monitor_get_fd( dev_monitor_ );
	const int nfds = fd_mon + 1;
        int queue_tok = 8;
	
	sigset_t sigempty;
	sigemptyset( &sigempty );

        struct timespec timeout;

        if( activity )
        {
            timeout.tv_sec = 0;
            //timeout.tv_sec = 1;
            timeout.tv_nsec = 100000000;
            //timeout.tv_nsec = 0;
        }
        else
        {
            timeout.tv_sec = 999;
            timeout.tv_nsec = 0;
        }

	while ( true ) {
		fd_set fds_read;
		FD_ZERO( &fds_read );
		FD_SET( fd_mon, &fds_read );
		
		// block for something interesting to happen
		int res = pselect( nfds, &fds_read, 0, 0, &timeout, &sigempty );
		if ( res < 0 ) {
			if ( EINTR != errno ) throw ErrnoException( "select" );
			std::cout << "Exiting on signal\n";
			return; // signalled
		}
		
		// udev monitor notification?
		if ( FD_ISSET( fd_mon, &fds_read ) ) {
			std::tr1::shared_ptr< udev_device > device( udev_monitor_receive_device( dev_monitor_ ), &udev_device_unref );
                        // make sure this is a device we want to monitor
                        udev_device* scsi_host = udev_device_get_parent_with_subsystem_devtype( device.get(), "scsi", "scsi_host" );
                        if ( !scsi_host ) return;
	
			const char* str = udev_device_get_action( device.get() );
			if ( !str ) {
			} else if ( 0 == strcasecmp( str, "add" ) ) {
				deviceAdded_( device.get() );
			} else if ( 0 == strcasecmp( str, "remove" ) ) {
				deviceRemove_( device.get() );
			} else {
				if ( debug ) {
					std::cout << "action: " << str << '\n';
					std::cout << ' ' << udev_device_get_syspath(device.get()) << "' (" << udev_device_get_subsystem(device.get()) << ")\n";
				}
			}
		}

                if( activity )
                {
                    for( int i = 0; i < num_disks_; ++i )
                    {
                        std::ifstream stats;
                        stats.open( statsFile(i).c_str() );

                        char buf[256];
                        stats.getline( &(buf[0]), 255 );
                        std::string s(&(buf[0]));

                        // tokenize the string
                        std::string::size_type last = s.find_first_not_of( " ", 0 );
                        std::string::size_type pos = s.find_first_of( " ", last );

                        int tok = 0;
                        int queue_length = 0;
                        while( std::string::npos != pos || std::string::npos != last )
                        {
                            last = s.find_first_not_of(" ", pos );
                            pos = s.find_first_of( " ", last );
                            ++tok;

                            if( tok == queue_tok )
                            {
                                queue_length = atoi(s.substr( last, pos - last ).c_str());
                                if( debug )
                                    std::cout << " " << i << " " << queue_length;
                                break;
                            }
                        }

                        int led_idx = ledIndex(i);

                        if( led_enabled_[led_idx] && leds_ )
                        {
                            if( queue_length > 0 )
                            {
                                leds_->Set( LED_BLUE, led_idx, true );
                                leds_->Set( LED_RED, led_idx, true );
                            }
                            else
                            {
                                leds_->Set( LED_BLUE, led_idx, true );
                                leds_->Set( LED_RED, led_idx, false );
                            }
                        }
                    }
                }
                if( debug )
                    std::cout << "\n";
	}
}

/////////////////////////////////////////////////////////////////////////////
/// device added
void DeviceMonitor::deviceAdded_( udev_device* device ) {
	std::cout << "ADDED: '" << udev_device_get_syspath(device) << "' (" << udev_device_get_subsystem(device) << ")\n";
	deviceChanged_( device, true );
}

/////////////////////////////////////////////////////////////////////////////
/// device removed
void DeviceMonitor::deviceRemove_( udev_device* device ) {
	std::cout << "REMOVED: '" << udev_device_get_syspath(device) << "' (" << udev_device_get_subsystem(device) << ")\n";
	deviceChanged_( device, false );
}

/////////////////////////////////////////////////////////////////////////////
/// device changed
void DeviceMonitor::deviceChanged_( udev_device* device, bool state ) {
    udev_device* scsi_host = udev_device_get_parent_with_subsystem_devtype( device, "scsi", "scsi_host" );
    if ( !scsi_host ) return;
	// ensure that scsi_host is attached to PCI (and not say USB)
	udev_device* scsi_host_parent = udev_device_get_parent( scsi_host );
	if ( !scsi_host_parent ) return;
	
	if ( debug ) std::cout << " scsi_host_parent: '" << udev_device_get_syspath(scsi_host_parent) << "' (" << udev_device_get_subsystem(scsi_host_parent) << ")\n";
	if ( 0 != strcmp("pci", udev_device_get_subsystem(scsi_host_parent)) ) return;
	
        // dev node gives us the LED number
        std::string devnode = udev_device_get_devnode(device);
        if ( debug ) std::cout << " dev node: " << devnode;
        if( disk_led_map_.count(devnode) == 0 )
            // this dev wasn't defined in the map
            return;

        int led_idx = disk_led_map_[devnode];
	if ( debug ) std::cout << " led: " << led_idx << '\n';
	
	// finally we can play with the appopriate LED
	if ( leds_ ) leds_->Set( LED_BLUE, led_idx, state );
        led_enabled_[led_idx] = state;
}

/////////////////////////////////////////////////////////////////////////////
/// enumerate existing devices
void DeviceMonitor::enumDevices_( ) {
	assert( dev_context_ );
	
	// create udev enumeration interface
	std::tr1::shared_ptr< udev_enumerate > dev_enum( udev_enumerate_new( dev_context_ ), &udev_enumerate_unref );
	
	// only interested in scsi_device's
	udev_enumerate_add_match_property( dev_enum.get(), "DEVTYPE", "disk" );
	udev_enumerate_scan_devices( dev_enum.get() ); // start
	
	//- enumerate list
	udev_list_entry* list_entry = udev_enumerate_get_list_entry( dev_enum.get() );
	for ( ; list_entry; list_entry = udev_list_entry_get_next( list_entry ) ) {
		// retrieve device
		std::tr1::shared_ptr< udev_device > device(
			udev_device_new_from_syspath(
				udev_enumerate_get_udev( dev_enum.get() ),
				udev_list_entry_get_name( list_entry )
			), &udev_device_unref
		);
                if ( !device ) continue;
		
                udev_device* scsi_host = udev_device_get_parent_with_subsystem_devtype( device.get(), "scsi", "scsi_host" );
                if ( !scsi_host ) return;

                // dev node gives us the LED number
                std::string devnode = udev_device_get_devnode(device.get());
                if ( debug || verbose > 1 ) std::cout << " dev node: " << devnode;
                if( disk_led_map_.count(devnode) == 0 )
                    // this dev wasn't defined in the map
                    continue;

                leds_idx_[num_disks_] = disk_led_map_[devnode];
                if ( debug || verbose > 1 ) std::cout << " led: " << leds_idx_[num_disks_] << "\n";

                stats_files_[num_disks_] = std::string( udev_device_get_syspath(device.get()) );
                stats_files_[num_disks_].append( "/stat" );

                // make sure it's there and we can open it
                std::ifstream stats;
                stats.open( stats_files_[num_disks_].c_str() );
                if( !stats )
                {
                    std::cout << " Couldn't open stats " << stats_files_[num_disks_] << "\n";
                    continue;
                }
                else
                    stats.close();

                ++num_disks_;

		deviceAdded_( device.get() );
	}
}
