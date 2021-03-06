#include <unistd.h>
#include <signal.h>

#include <boost/program_options.hpp>

#include <fcgiapp.h>
#include <fcgio.h>

#include <json.hpp>

#include <roller/core/types.h>
#include <roller/core/log.h>
#include <roller/core/util.h>
#include <roller/core/thread.h>
#include <roller/core/serialization.h>

#include "device_manager.h"

#include "raspi_gpio_switch.h"
#include "owfs_sensors.h"

#include "temperature_manager.h"
#include "current_limiter.h"
#include "pid.h"
#include "server_controller.h"
#include "dummy_controller.h"
#include "valve_controller.h"

#define AB_SERVER_FASTCGI_SOCKET "/var/run/ab.socket"
#define AB_SERVER_FASTCGI_BACKLOG 8

// pin numbers
// TODO: move elsewhere, organize better (config file?)
#define AB_VALVE_PIN 22
#define AB_FLOAT_PIN 14


using namespace roller;
namespace po = boost::program_options;
using json = nlohmann::json;

// globals
std::atomic_bool g_appRunning(false);

std::atomic_bool g_hltPidEnabled(false);
std::atomic_bool g_bkPidEnabled(false);
std::string g_bkMode = "off";
std::string g_hltMode = "off";
volatile float g_hltSetpoint = -100.0f;
volatile float g_bkSetpoint = -100.0f;
StringId g_hltTempProbeId = StringId::intern("28.3AA87D040000");
StringId g_bkTempProbeId = StringId::intern("28.EE9B8B040000");

std::atomic<uint32_t> g_stateCounter = {0}; // changes each time there is a change in state

CurrentLimiter g_currentLimiter(700, 35000); // base is 0.7 amps, total allowed 35 amps
ValveController g_valveController(
		g_currentLimiter,
		AB_FLOAT_PIN,
		AB_VALVE_PIN);

// handleSignal
void handleSignal( i32 sig ) {

	// fuck it...
	abort();

	/*
	if (g_appRunning) {
		Log::i( "sig %d caught, flagging app to stop running", sig );
		g_appRunning = false;
		FCGX_ShutdownPending();

		// hack to make fcgx actually die
		system("curl http://localhost/ab?cmd=status");


	} else {
		Log::i( "sig %d caught, really dying this time...");
		exit(1);
	}
	*/
}

// handleRequest
void handleRequest( FCGX_Request& request );

// loop to update PID algorithms
void pidLoop();

// test Dummy controller
void handleStartDummy();
void handleStopDummy();
std::shared_ptr<DummyController> s_controller;

void configCurrentLimiter();

// main
i32 main( i32 argc, char** argv ) {

	try {
		printf( "ab server starting\n" );

		g_appRunning = true;

		setbuf( stdout, nullptr );

		signal( SIGINT, handleSignal );
		signal( SIGHUP, handleSignal );
		signal( SIGKILL, handleSignal );
		signal( SIGTERM, handleSignal );

		Log::setLogLevelMode( LOG_LEVEL_MODE_UNIX_TERMINAL );

		// initialize devman
		auto raspiGPIOManager = std::make_shared<RaspiGPIOSwitchManager>();
		StringId raspiSwitchManagerID = DeviceManager::registerSwitchManager( raspiGPIOManager );

		// initialize CurrentLimiter
		configCurrentLimiter();

		// initialize temperature manager
		auto owfsManager = std::make_shared<OWFSHardwareManager>( "--usb all" );
		StringId owfsManagerID = DeviceManager::registerTemperatureSensorManager( owfsManager );

		// start PID thread
		Thread pidThread(pidLoop);
		pidThread.run();

		// start valve thread
		g_valveController.start();

		// initialize fast cgi
		printf( "initializing fcgx\n" );
		int result = FCGX_Init();
		if ( result ) {
			fprintf( stderr, "FCGX_Init() failed: %d\n", result );
			return 1;
		}

		// TODO: make socket file configurable
		int listenSocket = FCGX_OpenSocket( AB_SERVER_FASTCGI_SOCKET, AB_SERVER_FASTCGI_BACKLOG );
		if ( listenSocket < 0 ) {
			fprintf( stderr, "FCGX_OpenSocket(%s) failed: %d\n", AB_SERVER_FASTCGI_SOCKET, listenSocket);
			return 1;
		}

		FCGX_Request request;
		result = FCGX_InitRequest( &request, listenSocket, FCGI_FAIL_ACCEPT_ON_INTR );
		if ( result ) {
			fprintf( stderr, "FCGX_InitRequest() failed: %d\n", result );
		}

		// TODO: hack to change server socket permissions
		system("chown root:www-data /var/run/ab.socket &");
		system("chmod g+w /var/run/ab.socket &");

		while ( g_appRunning ) {

			result = FCGX_Accept_r( &request );
			if ( result ) {
				fprintf( stderr, "FCGX_Request_r failed: %d\n", result );
				break;
			}

			try {

				handleRequest(request);

			} catch( exception& e ) {
				Log::w( "Caught exception while trying to handle request (ignoring): %s", e.what());
				continue;
			}

		}

		Log::f("Joining PID thread...");
		pidThread.join();

		return 0;
	} catch (const exception& e) {
		Log::w("Caught exception in main, exiting program: %s", e.what());
	} catch (...) {
		Log::w("Caught unknown exception in main, exiting program");
	}

	return 1;

}

// handleRequest
void handleRequest( FCGX_Request& request ) {

	std::string scriptName = FCGX_GetParam("SCRIPT_NAME", request.envp);
	std::string pathInfo = FCGX_GetParam("PATH_INFO", request.envp);
	std::string requestMethod = FCGX_GetParam("REQUEST_METHOD", request.envp);
	std::string requestUri = FCGX_GetParam("REQUEST_URI", request.envp);

	/*
	Log::i( "handling request uri:    %s", requestUri.c_str());
	Log::i( "         script name:    %s", scriptName.c_str());
	Log::i( "         path info:      %s", pathInfo.c_str());
	Log::i( "         request method: %s", requestMethod.c_str());
	*/

	// parse URI
	std::map<std::string, std::string> params;
	std::string baseUri;

	std::string handlerName;

	size_t paramsStart = requestUri.find_first_of('?');
	if (paramsStart != std::string::npos) {

		baseUri = requestUri.substr(0, paramsStart);
		std::string paramsString = requestUri.substr(paramsStart + 1);

		std::vector<std::string> parts = split(paramsString, "&");
		for (const std::string& part : parts) {
			std::vector<std::string> paramParts = split(part, "=");
			if (paramParts.size() != 2) {
				throw RollerException("Illegal param (%s) in URI %s", part.c_str(), requestUri.c_str());
			}

			params[paramParts[0]] = paramParts[1];
		}
	} else {
		baseUri = requestUri;
	}

	handlerName = params["cmd"];

	
	// get last part of URI
	// TODO: clean this up

	std::string jsonResponse = "{}";
	i32 responseCode = 200;
	if (handlerName == "start_dummy") {

		try {
			handleStartDummy();

			jsonResponse = "{ \"response\": \"OK\" }";
			responseCode = 200;

		} catch ( exception& e ) {
			Log::w( "caught exception in handleStartDummy: %s", e.what() );

			jsonResponse = roller::makeString( "{ \"response\": \"Failed to start Dummy controller\", \"reason\": \"%s\"}",
					e.what());
			responseCode = 500;
		}

	} else if (handlerName == "stop_dummy") {

		try {
			handleStopDummy();

			jsonResponse = "{ \"response\": \"OK\" }";
			responseCode = 200;

		} catch ( exception& e ) {
			Log::w( "caught exception in handleStopDummy: %s", e.what() );

			jsonResponse = roller::makeString( "{ \"response\": \"Failed to stop Dummy controller\", \"reason\": \"%s\"}",
					e.what());
			responseCode = 500;
		}

	} else if (handlerName == "status") {

		json jsonObj = {
			{"status", "OK"},
			{"stateCounter", g_stateCounter.load()}
		};

		jsonResponse = jsonObj.dump(4);
		responseCode = 200;

	} else if (handlerName == "getState") {

		json jsonObj = {
			{"pins",  g_currentLimiter}
		};

		// controls

		json controlsJsonObj = {
			{"valve", g_valveController.getMode()},
			{"pump1", g_currentLimiter.getPinState(18)._desiredState},
			{"pump2", g_currentLimiter.getPinState(27)._desiredState},
			{"bk", g_bkMode},
			{"hlt", g_hltMode}
		};
		jsonObj["controls"] = controlsJsonObj;

		// pid controllers
		json pidJsonObj = {
			{"bk", {
				{"pid", g_bkPidEnabled.load()},
				{"setpoint", g_bkSetpoint}
			}},
			{"hlt", {
				{"pid", g_hltPidEnabled.load()},
				{"setpoint", g_hltSetpoint}
			}}
		};
		jsonObj["pid"] = pidJsonObj;

		jsonResponse = jsonObj.dump(4);
		responseCode = 200;

	// TODO: use wiring pi library here and track pin state?
	} else if (handlerName == "p1_on") {
		g_currentLimiter.enablePin(18);
		g_stateCounter++;

	} else if (handlerName == "p1_off") {
		g_currentLimiter.disablePin(18);
		g_stateCounter++;

	} else if (handlerName == "p2_on") {
		g_currentLimiter.enablePin(27);
		g_stateCounter++;

	} else if (handlerName == "p2_off") {
		g_currentLimiter.disablePin(27);
		g_stateCounter++;

	} else if (handlerName == "valve_on") {
		g_valveController.setMode(ValveController::Mode::ON);
		g_stateCounter++;

	} else if (handlerName == "valve_off") {
		g_valveController.setMode(ValveController::Mode::OFF);
		g_stateCounter++;

	} else if (handlerName == "valve_float") {
		g_valveController.setMode(ValveController::Mode::FLOAT);
		g_stateCounter++;

	} else if (handlerName == "configure_bk") {

		bool enabled = Serialization::toBool(params["enabled"]);
		if (enabled) {

			// get optional "critical" field
			bool critical = Serialization::toBool(params["critical"]);
			// TODO: use critical field (requires mods to CurrentLimiter)

			if (params["type"] == "") {
				throw RollerException("configure_bk requires type when enabled=true");
			} else if (params["type"] == "pid") {
				if (params["setpoint"] == "") {
					throw RollerException("configure_bk requires setpoint when type=pid");
				} else {
					f32 setpoint = Serialization::toF32(params["setpoint"]);
					g_bkSetpoint = setpoint;
					g_bkPidEnabled = true;
					g_bkMode = "pid";
					g_currentLimiter.enablePin(10);
				}
			} else if (params["type"] == "pwm") {
				if (params["load"] == "") {
					throw RollerException("configure_bk requires load when type=pwm");
				} else {
					f32 load = Serialization::toF32(params["load"]);
					CurrentLimiter::PinConfiguration pinConfiguration = g_currentLimiter.getPinConfiguration(17);
					pinConfiguration._pwmLoad = load;
					g_currentLimiter.updatePinConfiguration(pinConfiguration);
					g_currentLimiter.enablePin(10);
					g_bkPidEnabled = false;
					g_bkMode = "pwm";
				}
			} else {
				throw RollerException("illegal type parameter (%s) for configure_bk", params["type"].c_str());
			}
		} else {

			Log::i("Turning off BK");

			// set pwm load to 0
			CurrentLimiter::PinConfiguration pinConfiguration = g_currentLimiter.getPinConfiguration(17);
			pinConfiguration._pwmLoad = 0.0f;
			g_currentLimiter.updatePinConfiguration(pinConfiguration);

			// disable safety
			g_currentLimiter.disablePin(10);

			// flag bk pid to stop
			g_bkPidEnabled = false;
			g_bkMode = "off";
		}
		g_stateCounter++;
	} else if (handlerName == "configure_hlt") {

		bool enabled = Serialization::toBool(params["enabled"]);
		if (enabled) {
			if (params["type"] == "") {
				throw RollerException("configure_hlt requires type when enabled=true");
			} else if (params["type"] == "pid") {
				if (params["setpoint"] == "") {
					throw RollerException("configure_hlt requires setpoint when type=pid");
				} else {
					f32 setpoint = Serialization::toF32(params["setpoint"]);
					g_hltSetpoint = setpoint;
					g_hltPidEnabled = true;
					g_hltMode = "pid";
					g_currentLimiter.enablePin(24);
				}
			} else if (params["type"] == "pwm") {
				if (params["load"] == "") {
					throw RollerException("configure_hlt requires load when type=pwm");
				} else {
					f32 load = Serialization::toF32(params["load"]);
					CurrentLimiter::PinConfiguration pinConfiguration = g_currentLimiter.getPinConfiguration(4);
					pinConfiguration._pwmLoad = load;
					g_currentLimiter.updatePinConfiguration(pinConfiguration);
					g_currentLimiter.enablePin(24);
					g_hltPidEnabled = false;
					g_hltMode = "pwm";
				}
			} else {
				throw RollerException("illegal type parameter (%s) for configure_hlt", params["type"].c_str());
			}
		} else {
			Log::i("Turning off HLT");

			// set pwm load to 0
			CurrentLimiter::PinConfiguration pinConfiguration = g_currentLimiter.getPinConfiguration(4);
			pinConfiguration._pwmLoad = 0.0f;
			g_currentLimiter.updatePinConfiguration(pinConfiguration);

			// disable safety
			g_currentLimiter.disablePin(24);

			// flag hlt pid to stop
			g_hltPidEnabled = false;
			g_hltMode = "off";
		}
		g_stateCounter++;

	} else {

		jsonResponse = "{ \"response\": \"Unrecognized Handler\" }";
		responseCode = 400;
	}

	// assemble HTTP response
	std::string statusResponse;
	switch (responseCode)
	{
		case 200:
			statusResponse = "200 OK";
			break;

		case 400:
			statusResponse = "400 Bad Request";
			break;

		default:
			Log::w( "HTTP response code not set, assuming 500" );
			// intentionally fall through

		case 500:
			statusResponse = "500 Internal Server Error";
			break;
	}

	FCGX_FPrintF( request.out, "Status: %s\r\n", statusResponse.c_str() );
	FCGX_FPrintF( request.out, "Content-Type: application/json; charset=utf-8\r\n" );
	FCGX_FPrintF( request.out, "Content-Length: %d\r\n", jsonResponse.size() );
	FCGX_FPrintF( request.out, "\r\n" );
	FCGX_PutStr( jsonResponse.c_str(), jsonResponse.size(), request.out );

	FCGX_Finish_r( &request );
}

void handleStartDummy() {

	if ( s_controller ) {
		throw RollerException( "Can't start dummy controller; already running" );
	}

	s_controller.reset( new DummyController());
	s_controller->start();
}

void handleStopDummy() {

	if ( ! s_controller ) {
		throw RollerException( "Can't stop dummy controller; none running" );
	}

	s_controller->stop();
	s_controller->join();
	s_controller.reset();
}

void configCurrentLimiter() {

	// TODO: pull this info from config file (etc.)

	// pump 1
	CurrentLimiter::PinConfiguration config;
	config._name = "Pump 1";
	config._id = "p1";
	config._pinNumber = 18;
	config._milliAmps = 1400;
	config._critical = true;
	config._pwm = false;
	config._pwmFrequency = 0;
	config._pwmLoad = 0.0f;
	g_currentLimiter.addPinConfiguration(
			config,
			DeviceManager::getSwitch(RaspiGPIOSwitchManager::s_id, StringId::format("%d", config._pinNumber)));

	// pump 2
	config._name = "Pump 2";
	config._id = "p2";
	config._pinNumber = 27;
	config._milliAmps = 1400;
	config._critical = true;
	config._pwm = false;
	config._pwmFrequency = 0;
	config._pwmLoad = 0.0f;
	g_currentLimiter.addPinConfiguration(
			config,
			DeviceManager::getSwitch(RaspiGPIOSwitchManager::s_id, StringId::format("%d", config._pinNumber)));

	// valve 1
	config._name = "Valve 1";
	config._id = "valve1";
	config._pinNumber = AB_VALVE_PIN;
	config._milliAmps = 200;
	config._critical = true;
	config._pwm = false;
	config._pwmFrequency = 0;
	config._pwmLoad = 0.0f;
	g_currentLimiter.addPinConfiguration(
			config,
			DeviceManager::getSwitch(RaspiGPIOSwitchManager::s_id, StringId::format("%d", config._pinNumber)));

	// BK element safety
	config._name = "BK Element Safety";
	config._id = "bk_safety";
	config._pinNumber = 10;
	config._milliAmps = 34;
	config._critical = true;
	config._pwm = false;
	config._pwmFrequency = 0;
	config._pwmLoad = 0.0f;
	g_currentLimiter.addPinConfiguration(
			config,
			DeviceManager::getSwitch(RaspiGPIOSwitchManager::s_id, StringId::format("%d", config._pinNumber)));

	// HLT element safety
	config._name = "HLT Element Safety";
	config._id = "hlt_safety";
	config._pinNumber = 24;
	config._milliAmps = 34;
	config._critical = true;
	config._pwm = false;
	config._pwmFrequency = 0;
	config._pwmLoad = 0.0f;
	g_currentLimiter.addPinConfiguration(
			config,
			DeviceManager::getSwitch(RaspiGPIOSwitchManager::s_id, StringId::format("%d", config._pinNumber)));

	// BK element 
	config._name = "BK Element";
	config._id = "bk";
	config._pinNumber = 17;
	config._milliAmps = 23000;
	config._critical = false;
	config._pwm = true;
	config._pwmFrequency = 20;
	config._pwmLoad = 0.0f;
	g_currentLimiter.addPinConfiguration(
			config,
			DeviceManager::getSwitch(RaspiGPIOSwitchManager::s_id, StringId::format("%d", config._pinNumber)));

	// HLT element 
	config._name = "HLT Element";
	config._id = "hlt";
	config._pinNumber = 4;
	config._milliAmps = 23000;
	config._critical = false;
	config._pwm = true;
	config._pwmFrequency = 20;
	config._pwmLoad = 0.0f;
	g_currentLimiter.addPinConfiguration(
			config,
			DeviceManager::getSwitch(RaspiGPIOSwitchManager::s_id, StringId::format("%d", config._pinNumber)));
}

void pidLoop() {

	// TODO: find a better home for this
	TemperatureManager temperatureManager;
	temperatureManager.run();

	bool hltSetup = false;
	bool bkSetup = false;

	shared_ptr<PID> hltPID;
	shared_ptr<PID> bkPID;

	int64_t lastHLTPIDUpdateTime = getTime();
	int64_t lastBKPIDUpdateTime = getTime();


	while (g_appRunning) {

		// update HLT PID if needed
		if (g_hltPidEnabled) {

			// initialize HLT PID if needed
			if (! hltSetup) {
				hltPID.reset(new PID(15.0f, 1.0f, 3.0f, g_hltSetpoint, -100.0f, 100.0f));
				hltPID->setErrorAccumulationCap(1.5f);
				hltSetup = true;

				// TODO: enable safety pin
			}

			hltPID->setSetpoint(g_hltSetpoint);

			ProbeStats stats = temperatureManager.getProbeStats(g_hltTempProbeId);
			int32_t temp = stats._lastTemp;

			int64_t now = getTime();
			hltPID->update( ((float)temp / 1000.f), ((float)(now - lastHLTPIDUpdateTime) / 1000.0f) );
			lastHLTPIDUpdateTime = now;

			// TODO: review / optimize -- this triggers a lot of work
			CurrentLimiter::PinConfiguration pinConfiguration = g_currentLimiter.getPinConfiguration(4);
			pinConfiguration._pwmLoad = std::max(0.0f, (hltPID->getOutput() / 100.0f ));
			g_currentLimiter.updatePinConfiguration(pinConfiguration);

		} else if (hltSetup) {
			Log::i("killing HLT pid...");
			hltPID.reset();
			hltSetup = false;
		}

		// update BK PID if needed
		if (g_bkPidEnabled) {

			// initialize BK PID if needed
			if (! bkSetup) {
				bkPID.reset(new PID(15.0f, 1.0f, 3.0f, g_bkSetpoint, -100.0f, 100.0f));
				bkPID->setErrorAccumulationCap(1.5f);
				bkSetup = true;

				// TODO: enable safety pin
			}

			bkPID->setSetpoint(g_bkSetpoint);

			ProbeStats stats = temperatureManager.getProbeStats(g_bkTempProbeId);
			int32_t temp = stats._lastTemp;

			int64_t now = getTime();
			bkPID->update( ((float)temp / 1000.f), ((float)(now - lastBKPIDUpdateTime) / 1000.0f) );
			lastBKPIDUpdateTime = now;

			// TODO: review / optimize -- this triggers a lot of work
			CurrentLimiter::PinConfiguration pinConfiguration = g_currentLimiter.getPinConfiguration(17);
			pinConfiguration._pwmLoad = std::max(0.0f, (bkPID->getOutput() / 100.0f ));
			g_currentLimiter.updatePinConfiguration(pinConfiguration);

		} else if (bkSetup) {
			Log::i("killing BK pid...");
			bkPID.reset();
			bkSetup = false;
		}
		
		usleep(1 * 1000 * 1000);
	}
}
