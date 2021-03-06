#include <unistd.h>
#include <signal.h>

#include <boost/program_options.hpp>

#include <roller/core/types.h>
#include <roller/core/log.h>
#include <roller/core/util.h>

#include "device_manager.h"

#include "raspi_gpio_switch.h"

#include "pwm.h"

using namespace roller;
namespace po = boost::program_options;

// globals
bool g_appRunning = false;

// handleSignal
void handleSignal( i32 sig ) {
	Log::i( "sig %d caught, flagging app to stop running", sig );
	g_appRunning = false;
}

// mainNULL
i32 main( i32 argc, char** argv ) {

	i32 safetyPinId;
	i32 ssrPinId;
	f32 load;
	i32 freq;

	po::options_description mainOptions( "Main options" );
	mainOptions.add_options()
		("help,h",																"produce this help message")
		("pin-id",			po::value<i32>(&ssrPinId)->required(),				"id of the pin to be controlled (required)")
		("safety-id",		po::value<i32>(&safetyPinId)->default_value(-1),	"id of the pin used for safety circuit (-1 for none)")
		("load,l",			po::value<f32>(&load)->required(),					"load to be applied (0-1)")
		("frequency,f",		po::value<i32>(&freq)->default_value(20),			"frequency of pwm, in Hz")
		;

	po::variables_map mainOptionsMap;
	po::store( po::parse_command_line( argc, argv, mainOptions ), mainOptionsMap );

	if ( mainOptionsMap.count( "help" )) {
		std::cout << mainOptions << std::endl;
		return 0;
	}

	// will handle required, etc.
	po::notify( mainOptionsMap );

	Log::f( "pin id: %d", ssrPinId );
	Log::f( "safety pin id: %d", safetyPinId );
	Log::f( "load: %.2f", load );
	Log::f( "frequency: %df", freq );

	g_appRunning = true;

	setbuf( stdout, nullptr );

	signal( SIGINT, handleSignal );
	signal( SIGHUP, handleSignal );
	signal( SIGKILL, handleSignal );
	signal( SIGTERM, handleSignal );

	Log::setLogLevelMode( LOG_LEVEL_MODE_UNIX_TERMINAL );

	// set up devman

	auto raspiGPIOManager = std::make_shared<RaspiGPIOSwitchManager>();
	StringId raspiSwitchManagerID = DeviceManager::registerSwitchManager( raspiGPIOManager );

	auto ssrPin = DeviceManager::getSwitch(
			raspiSwitchManagerID,
			StringId::format( "%d", ssrPinId ));

	// turn safety pin on if it was supplied
	if ( safetyPinId >= 0 ) {
		auto safetyPin = DeviceManager::getSwitch(
				raspiSwitchManagerID,
				StringId::format( "%d", safetyPinId ));

		safetyPin->setState( true );
	}

	PWMController pwm( ssrPin );
	pwm.setFrequency( freq );
	pwm.unpause();

	i64 time = -1;

	pwm.setLoadCycle( load );

	while ( g_appRunning ) {

		usleep( 100 * 1000 );
	}

	pwm.pause();
	pwm.stop();
	pwm.join();

	ssrPin->setState( false );

	// turn safety pin off if it was supplied
	if ( safetyPinId >= 0 ) {
		auto safetyPin = DeviceManager::getSwitch(
				raspiSwitchManagerID,
				StringId::format( "%d", safetyPinId ));

		safetyPin->setState( false );
	}

	DeviceManager::unregisterSwitchManager( raspiSwitchManagerID );

	return 0;
}
