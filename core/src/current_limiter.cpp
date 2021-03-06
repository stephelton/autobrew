#include "current_limiter.h"

using json = nlohmann::json;

CurrentLimiter::CurrentLimiter(uint32_t baseMilliAmps, uint32_t maxMilliAmps)
		: _baseMilliAmps(baseMilliAmps)
		, _maxMilliAmps(maxMilliAmps)
{
}

CurrentLimiter::~CurrentLimiter() {
	Log::w("CurrentLimiter::~CurrentLimiter()");

	MutexLocker locker(_lock);

	// turn off all pins and PWM controllers
	for (const auto& entry : _pinConfigurations) {

		try {
			const PinConfiguration& config = entry.second;
			PinState& state = _pinStates[config._pinNumber];

			// if pwm, turn controller off and free it
			if (config._pwm) {
				state._pwmController->stop();
				state._pwmController->join();
				state._pwmController.reset();
			}

			Log::w("CurrentLimiter::~CurrentLimiter: disabling pin %d", config._pinNumber);
			state._ioSwitch->setState(false);
		} catch (const exception& e) {
			Log::w("Caught exception while destructing CurrentLimiter, ignoring: %s", e.what());
		} catch (...) {
			Log::w("Caught unknown exception while destructing CurrentLimiter, ignoring");
		}
	}
}

void CurrentLimiter::addPinConfiguration(const PinConfiguration& config, std::shared_ptr<Switch> gpio) {
	MutexLocker locker(_lock);

	uint32_t pin = config._pinNumber;
	
	// if we already have a pin for this pin number, reject
	if (_pinConfigurations.find(pin) != _pinConfigurations.end()) {
		throw RollerException("Cannot add a pin configuration for pin %u, already have one", pin);
	}

	_pinConfigurations[pin] = config;

	// also add state entry and create PWM controller if needed
	PinState state;
	state._pinNumber = pin;
	state._desiredState = false;
	state._overriden = false;
	state._enabled = false;
	state._pwmLoad = config._pwmLoad;
	state._ioSwitch = gpio;

	if (config._pwm) {
		state._pwmController = std::make_shared<PWMController>(gpio);
		state._pwmController->setLoadCycle(0.0f); // we won't set load until 
		state._pwmController->setFrequency(config._pwmFrequency);
	}

	_pinStates[pin] = state;
}

CurrentLimiter::PinConfiguration CurrentLimiter::getPinConfiguration(uint32_t pin) {
	MutexLocker locker(_lock);

	auto itr = _pinConfigurations.find(pin);
	if (itr == _pinConfigurations.end()) {
		throw RollerException("Cannot add a pin configuration for pin %u, already have one", pin);
	}

	return itr->second;
}

// getPinState
const CurrentLimiter::PinState& CurrentLimiter::getPinState(uint32_t pin) {
	MutexLocker locker(_lock);

	auto itr = _pinStates.find(pin);
	if (itr == _pinStates.end()) {
		throw RollerException("No pin state for pin %d", pin);
	}

	return itr->second;
}


void CurrentLimiter::updatePinConfiguration(const CurrentLimiter::PinConfiguration& config) {
	MutexLocker locker(_lock);

	uint32_t pin = config._pinNumber;

	auto itr = _pinConfigurations.find(pin);
	if (itr == _pinConfigurations.end()) {
		throw RollerException("Cannot update pin configuration for non-existent pin %u", pin);
	}

	CurrentLimiter::PinConfiguration& existingConfig = itr->second;
	// Log::f("Updating pin configuration for %s", config._name.c_str());

	if (existingConfig._pwm != config._pwm) {
		throw RollerException("Cannot convert a pin to PWW or vise versa after initialization");
	}

	aver(_pinStates.find(pin) != _pinStates.end());

	existingConfig = config;

	// TODO: only need to re-evaluateConfiguration() if certain things changed
	evaluateConfiguration();
}

void CurrentLimiter::enablePin(uint32_t pin) {
	MutexLocker locker(_lock);

	auto itr = _pinConfigurations.find(pin);
	if (itr == _pinConfigurations.end()) {
		throw RollerException("Cannot enable non-existent pin %u", pin);
	}

	PinState& state = _pinStates[pin];

	if (! state._desiredState) {
		state._desiredState = true;
		evaluateConfiguration();
	}
}

void CurrentLimiter::disablePin(uint32_t pin) {
	MutexLocker locker(_lock);

	auto itr = _pinConfigurations.find(pin);
	if (itr == _pinConfigurations.end()) {
		throw RollerException("Cannot disable non-existent pin %u", pin);
	}

	PinState& state = _pinStates[pin];

	if (state._desiredState) {
		state._desiredState = false;
		evaluateConfiguration();
	}
}

bool CurrentLimiter::isEnabled(uint32_t pin) {
	MutexLocker locker(_lock);

	auto itr = _pinConfigurations.find(pin);
	if (itr == _pinConfigurations.end()) {
		throw RollerException("Cannot enable non-existent pin %u", pin);
	}

	PinState& state = _pinStates[pin];

	return state._desiredState;
}

void CurrentLimiter::evaluateConfiguration() {

	// TODO: the implementation here should really pre-calculate all pin states, and then
	//		make 2 passes: one to disable any pins that should be off and then finally
	//		a second to turn on any pins that should be on

	/*
	Log::f("CurrentLimiter::evaluateConfiguration()");

	Log::f("  Current configuration:");
	for (const auto& entry : _pinConfigurations) {
		const PinConfiguration& config = entry.second;
		Log::f("  - Name:       %s", config._name.c_str());
		Log::f("    Pin Number: %u", config._pinNumber);
		Log::f("    Milliamps:  %u", config._milliAmps);
		Log::f("    Critical:   %s", (config._critical ? "T" : "F"));
		Log::f("    PWM:        %s", (config._pwm ? "T" : "F"));
		if (config._pwm) {
			Log::f("    PWM load:   %.3f", config._pwmLoad);
		}
	}
	*/

	uint32_t available = _maxMilliAmps;
	// Log::f("  Max available current: %u mA", _maxMilliAmps);

	available -= _baseMilliAmps;
	// Log::f("  Base current: %u mA", _baseMilliAmps);
	// Log::f("  Available: %u mA", available);

	// first, turn on all critical non-pwm pins
	// Log::f("  Turning on critical non-PWM pins...");
	for (const auto& entry : _pinConfigurations) {
		const PinConfiguration& config = entry.second;
		PinState& state = _pinStates[config._pinNumber];
		
		if (config._critical && ! config._pwm) {

			if (state._desiredState) {

				int32_t remainder = (available - config._milliAmps);
				// Log::f("    %s: %u - %u = %d", config._name.c_str(), available, config._milliAmps, remainder);

				if (remainder > 0) {
					// update set state
					state._overriden = false;
					state._enabled = true;

					// update available
					available = (uint32_t)remainder;

					// we don't turn the pin on here; we do that after
					// we've pins off and have calculated what is to be
					// turned on

				} else {
					Log::w("    %s won't fit (critical / non-PWM)");

					// update set state
					state._overriden = true;
					state._enabled = false;

					// turn pin off
					state._ioSwitch->setState(false);
				}

			} else {

				// pin wasn't desired anyway, turn off
				state._overriden = false;
				state._enabled = false;
				state._ioSwitch->setState(false);
			}
		}
	}

	// TODO:
	// Log::f("  TODO:   critical PWM pins");
	// Log::f("  TODO:   non-critical non-PWM pins");


	// Log::f("  Turning on non-critical PWM pins...");

	// make a first pass to tally the desired mA
	double totalDesiredMilliAmps = 0.0f;
	for (const auto& entry : _pinConfigurations) {
		const PinConfiguration& config = entry.second;

		if (! config._critical && config._pwm) {
			double loadMA = ((float)config._milliAmps * config._pwmLoad);
			// Log::f("    %s wants %.3f mA (%.3f * %u)", config._name.c_str(), (float)loadMA, config._pwmLoad, config._milliAmps);
			totalDesiredMilliAmps += loadMA;
		}
	}

	double availableRatio = ((double)available / totalDesiredMilliAmps);

	// Log::f("    Total desired mA: %.1f", (float)totalDesiredMilliAmps);
	// Log::f("    Available mA:     %u", available);
	// Log::f("    Available ratio:  %.3f", availableRatio);

	// if there is enough to go around, give everyone what they want
	if (totalDesiredMilliAmps > 0.001f) { // TODO: this precludes very small current demands
		if (totalDesiredMilliAmps < (double)available) {
			for (const auto& entry : _pinConfigurations) {
				const PinConfiguration& config = entry.second;

				if (! config._critical && config._pwm) {
					PinState& state = _pinStates[config._pinNumber];
					state._pwmLoad = config._pwmLoad;
					// Log::f("    %s gets what he wants: %.3f", config._name.c_str(), (float)state._pwmLoad);
				}
			}
		} else {
			// not enough current left, divide up amongst the requestors
			for (const auto& entry : _pinConfigurations) {
				const PinConfiguration& config = entry.second;

				if (! config._critical && config._pwm) {
					// TODO: configure as desired
					double loadMA = (double)config._milliAmps * (double)config._pwmLoad;
					double load = (loadMA * ((double)available / totalDesiredMilliAmps));
					// Log::f("    %s's portion: %.3f * %.3f = %.3f", config._name.c_str(), (float)loadMA, (float)availableRatio, (float)load);
					
					PinState& state = _pinStates[config._pinNumber];
					state._pwmLoad = availableRatio * config._pwmLoad;
				}
			}
			
		}
	} else {

		// turn all off
		for (auto& entry : _pinStates) {
			PinState& state = entry.second;
			if (state._pwmLoad > 0.0f) {
				state._pwmLoad = 0.0f;
			}
		}
	}

	// finally, turn on pins that we calculated should be on
	for (const auto& entry : _pinConfigurations) {
		const PinConfiguration& config = entry.second;
		PinState& state = _pinStates[config._pinNumber];

		if (config._pwm) {
			// Log::i("Setting pin %d to %.3f (PWM), %u (freq)", config._pinNumber, state._pwmLoad, config._pwmFrequency);
			state._pwmController->setLoadCycle(state._pwmLoad);
			state._pwmController->setFrequency(config._pwmFrequency);
			state._pwmController->unpause();
		} else if (state._enabled) {
			state._ioSwitch->setState(true);
			// Log::i("Setting pin %d to on", config._pinNumber);
		}
	}


}

// to_json
void to_json(json& j, const CurrentLimiter::PinConfiguration& pinConfig) {
	j = json {
		{"name", pinConfig._name},
		{"id", pinConfig._id},
		{"pinNumber", pinConfig._pinNumber},
		{"milliAmps", pinConfig._milliAmps},
		{"critical", pinConfig._critical},
		{"pwm", pinConfig._pwm},
		{"pwmFrequency", pinConfig._pwmFrequency},
		{"pwmLoad", pinConfig._pwmLoad}
	};
}

// to_json
void to_json(json& j, const CurrentLimiter::PinState& pinState) {
	j = json {
		{"pinNumber", pinState._pinNumber},
		{"desiredState", pinState._desiredState},
		{"overriden", pinState._overriden},
		{"enabled", pinState._enabled},
		{"pwmLoad", pinState._pwmLoad}
	};
}

// to_json
void to_json(json& j, const CurrentLimiter& currentLimiter) {
	currentLimiter.to_json(j);
}
void CurrentLimiter::to_json(nlohmann::json& j) const {
	j = json {
		{"baseMilliAmps", _baseMilliAmps},
		{"maxMilliAmps", _maxMilliAmps}
	};

	for (const auto itr : _pinConfigurations) {

		const PinConfiguration pinConfig = itr.second;
		const PinState pinState = _pinStates.at(pinConfig._pinNumber);

		json pinJsonObj = {
			{"config", pinConfig},
			{"state", pinState}
		};

		j[pinConfig._id] = pinJsonObj;

	}

}
