/*
 Copyright (C) AC SOFTWARE SP. Z O.O.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#define SUPLADEVICE_CPP

#include <Arduino.h>

#include "SuplaDevice.h"
#include "SuplaImpulseCounter.h"
#include "supla-common/IEEE754tools.h"
#include "supla-common/log.h"
#include "supla-common/srpc.h"
#include "supla/channel.h"
#include "supla/element.h"
#include "supla/io.h"
#include "supla/timer.h"

void SuplaDeviceClass::status(int status, const char *msg) {
  if (impl_arduino_status != NULL) {
    impl_arduino_status(status, msg);
  } else {
    if (currentStatus != status) {
      currentStatus = status;
      supla_log(LOG_DEBUG, "Current status: [%d] %s", status, msg);
    }
  }
}

SuplaDeviceClass::SuplaDeviceClass()
    : port(-1),
      connectionFailCounter(0),
      networkIsNotReadyCounter(0),
      currentStatus(STATUS_UNKNOWN) {
  srpc = NULL;
  registered = 0;
  last_iterate_time = 0;
  wait_for_iterate = 0;
  channel_pin = NULL;
  channel_pin_count = 0;
}

SuplaDeviceClass::~SuplaDeviceClass() {
  if (channel_pin != NULL) {
    free(channel_pin);
    channel_pin = NULL;
  }
}

void SuplaDeviceClass::setStatusFuncImpl(
    _impl_arduino_status impl_arduino_status) {
  this->impl_arduino_status = impl_arduino_status;
}

bool SuplaDeviceClass::isInitialized(bool msg) {
  if (srpc != NULL) {
    if (msg)
      status(STATUS_ALREADY_INITIALIZED, "SuplaDevice is already initialized");

    return true;
  }

  return false;
}

bool SuplaDeviceClass::begin(char GUID[SUPLA_GUID_SIZE],
                             const char *Server,
                             const char *email,
                             char authkey[SUPLA_AUTHKEY_SIZE],
                             unsigned char version) {
  if (isInitialized(true)) return false;

  if (Supla::Network::Instance() == NULL) {
    status(STATUS_MISSING_NETWORK_INTERFACE, "Network Interface not defined!");
    return false;
  }

  memcpy(Supla::Channel::reg_dev.GUID, GUID, SUPLA_GUID_SIZE);
  memcpy(Supla::Channel::reg_dev.AuthKey, authkey, SUPLA_AUTHKEY_SIZE);

  setString(Supla::Channel::reg_dev.Email, email, SUPLA_EMAIL_MAXSIZE);
  setString(
      Supla::Channel::reg_dev.ServerName, Server, SUPLA_SERVER_NAME_MAXSIZE);

  bool emptyGuidDetected = true;
  for (int i = 0; i < SUPLA_GUID_SIZE; i++) {
    if (Supla::Channel::reg_dev.GUID[i] != 0) {
      emptyGuidDetected = false;
    }
  }
  if (emptyGuidDetected) {
    status(STATUS_INVALID_GUID, "Invalid GUID");
    return false;
  }

  if (Supla::Channel::reg_dev.ServerName[0] == NULL) {
    status(STATUS_UNKNOWN_SERVER_ADDRESS, "Unknown server address");
    return false;
  }

  if (Supla::Channel::reg_dev.Email[0] == NULL) {
    status(STATUS_MISSING_CREDENTIALS, "Unknown email address");
    return false;
  }

  bool emptyAuthKeyDetected = true;
  for (int i = 0; i < SUPLA_AUTHKEY_SIZE; i++) {
    if (Supla::Channel::reg_dev.AuthKey[i] != 0) {
      emptyAuthKeyDetected = false;
      break;
    }
  }
  if (emptyAuthKeyDetected) {
    status(STATUS_MISSING_CREDENTIALS, "Unknown AuthKey");
    return false;
  }

  if (strnlen(Supla::Channel::reg_dev.Name, SUPLA_DEVICE_NAME_MAXSIZE) == 0) {
#if defined(ARDUINO_ARCH_ESP8266)
    setString(
        Supla::Channel::reg_dev.Name, "ESP8266", SUPLA_DEVICE_NAME_MAXSIZE);
#elif defined(ARDUINO_ARCH_ESP32)
    setString(Supla::Channel::reg_dev.Name, "ESP32", SUPLA_DEVICE_NAME_MAXSIZE);
#else
    setString(
        Supla::Channel::reg_dev.Name, "ARDUINO", SUPLA_DEVICE_NAME_MAXSIZE);
#endif
  }

  if (strnlen(Supla::Channel::reg_dev.SoftVer, SUPLA_SOFTVER_MAXSIZE) == 0) {
    setString(Supla::Channel::reg_dev.SoftVer,
              "User SW, lib 2.3.2",
              SUPLA_SOFTVER_MAXSIZE);
  }

  Supla::Network::Setup();

  TsrpcParams srpc_params;
  srpc_params_init(&srpc_params);
  srpc_params.data_read = &Supla::data_read;
  srpc_params.data_write = &Supla::data_write;
  srpc_params.on_remote_call_received = &Supla::message_received;
  srpc_params.user_params = this;

  srpc = srpc_init(&srpc_params);
  Supla::Network::SetSrpc(srpc);

  // Set Supla protocol interface version
  srpc_set_proto_version(srpc, version);

  supla_log(LOG_DEBUG, "Using Supla protocol version %d", version);

  // Iterate all elements and load configuration (TODO)
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onLoadConfig();
  }

  // Load counters values from EEPROM storage
  SuplaImpulseCounter::loadStorage();

  // Enable timers
  Supla::initTimers();

  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onInit();
  }

  for (int a = 0; a < channel_pin_count; a++) {
    SuplaImpulseCounter *ptr = SuplaImpulseCounter::getCounterByChannel(a);
    if (ptr) {
      _supla_int64_t value = ptr->getCounter();
      ptr->clearIsChanged();
      memcpy(Supla::Channel::reg_dev.channels[a].value, &value, 8);
    }
  }

  status(STATUS_INITIALIZED, "SuplaDevice initialized");
  return true;
}

void SuplaDeviceClass::setName(const char *Name) {
  if (isInitialized(true)) return;
  setString(Supla::Channel::reg_dev.Name, Name, SUPLA_DEVICE_NAME_MAXSIZE);
}

int SuplaDeviceClass::addChannel(int pin1,
                                 int pin2,
                                 bool hiIsLo,
                                 bool bistable) {
  if (isInitialized(true)) return -1;

  if (Supla::Channel::reg_dev.channel_count >= SUPLA_CHANNELMAXCOUNT) {
    status(STATUS_CHANNEL_LIMIT_EXCEEDED, "Channel limit exceeded");
    return -1;
  }

  channel_pin_count++;

  if (bistable && (pin1 == 0 || pin2 == 0)) bistable = false;

  // !!! Channel number is always equal to channel array idx
  // Supla::Channel::reg_dev.channels[idx]
  Supla::Channel::reg_dev.channels[Supla::Channel::reg_dev.channel_count]
      .Number = Supla::Channel::reg_dev.channel_count;
  channel_pin = (SuplaChannelPin *)realloc(
      channel_pin,
      sizeof(SuplaChannelPin) * (Supla::Channel::reg_dev.channel_count + 1));
  channel_pin[Supla::Channel::reg_dev.channel_count].pin1 = pin1;
  channel_pin[Supla::Channel::reg_dev.channel_count].pin2 = pin2;
  channel_pin[Supla::Channel::reg_dev.channel_count].hiIsLo = hiIsLo;
  channel_pin[Supla::Channel::reg_dev.channel_count].bistable = bistable;
  channel_pin[Supla::Channel::reg_dev.channel_count].time_left =
      0;  // 100*Supla::Channel::reg_dev.channel_count;
  channel_pin[Supla::Channel::reg_dev.channel_count].vc_time = 0;
  channel_pin[Supla::Channel::reg_dev.channel_count].bi_time_left = 0;
  channel_pin[Supla::Channel::reg_dev.channel_count].last_val =
      Supla::Io::digitalRead(Supla::Channel::reg_dev.channel_count,
                             bistable ? pin2 : pin1);

  Supla::Channel::reg_dev.channel_count++;

  return Supla::Channel::reg_dev.channel_count - 1;
}

bool SuplaDeviceClass::addImpulseCounter(int impulsePin,
                                         int statusLedPin,
                                         bool detectLowToHigh,
                                         bool inputPullup,
                                         unsigned long debounceDelay) {
  int c = addChannel(0, 0, false, false);
  if (c == -1) return false;

  Supla::Channel::reg_dev.channels[c].Type = SUPLA_CHANNELTYPE_IMPULSE_COUNTER;

  // Init channel value with "0"
  memset(Supla::Channel::reg_dev.channels[c].value, 0, 8);

  SuplaImpulseCounter::create(
      c, impulsePin, statusLedPin, detectLowToHigh, inputPullup, debounceDelay);
}

void SuplaDeviceClass::setString(char *dst, const char *src, int max_size) {
  if (src == NULL) {
    dst[0] = 0;
    return;
  }

  int size = strlen(src);

  if (size + 1 > max_size) size = max_size - 1;

  memcpy(dst, src, size);
}

void SuplaDeviceClass::iterate_impulse_counter(
    SuplaChannelPin *pin,
    TDS_SuplaDeviceChannel_C *channel,
    unsigned long time_diff,
    int channel_number) {
  if (channel->Type == SUPLA_CHANNELTYPE_IMPULSE_COUNTER &&
      pin->time_left <= 0) {
    pin->time_left = 5000;
    SuplaImpulseCounter *ptr =
        SuplaImpulseCounter::getCounterByChannel(channel_number);
    if (ptr && ptr->isChanged()) {
      _supla_int64_t value = ptr->getCounter();
      ptr->clearIsChanged();
      memcpy(channel->value, &value, 8);
      srpc_ds_async_channel_value_changed(srpc, channel_number, channel->value);
    }
  }
}

void SuplaDeviceClass::onTimer(void) {
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onTimer();
  }
}

void SuplaDeviceClass::onFastTimer(void) {
  // Iteration over all impulse counters will count incomming impulses. It is
  // after SuplaDevice initialization (because we have to read stored counter
  // values) and before any other operation like connection to Supla cloud
  // (because we want to count impulses even when we have connection issues.
  SuplaImpulseCounter::iterateAll();
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->onFastTimer();
  }
}

void SuplaDeviceClass::iterate(void) {
  if (!isInitialized(false)) return;

  unsigned long _millis = millis();
  unsigned long time_diff = abs(_millis - last_iterate_time);

  uptime.iterate(_millis);

  // Iterate all elements
  for (auto element = Supla::Element::begin(); element != nullptr;
       element = element->next()) {
    element->iterateAlways();
  }

  SuplaImpulseCounter::updateStorageOccasionally();

  if (wait_for_iterate != 0 && _millis < wait_for_iterate) {
    return;

  } else {
    wait_for_iterate = 0;
  }

  // Restart network after >1 min of failed connection attempts
  if (connectionFailCounter > 30) {
    connectionFailCounter = 0;
    supla_log(LOG_DEBUG,
              "Connection fail counter overflow. Trying to setup network "
              "interface again");
    Supla::Network::Setup();
    return;
  }

  if (!Supla::Network::IsReady()) {
    uptime.setConnectionLostCause(SUPLA_LASTCONNECTIONRESETCAUSE_WIFI_CONNECTION_LOST);
    wait_for_iterate = millis() + 100;
    status(STATUS_NETWORK_DISCONNECTED, "No connection to network");
    networkIsNotReadyCounter++;
    if (networkIsNotReadyCounter > 20) {
      networkIsNotReadyCounter = 0;
      connectionFailCounter++;
    }
    return;
  }
  networkIsNotReadyCounter = 0;

  if (!Supla::Network::Connected()) {
    status(STATUS_SERVER_DISCONNECTED, "Not connected to Supla server");

    uptime.setConnectionLostCause(SUPLA_LASTCONNECTIONRESETCAUSE_SERVER_CONNECTION_LOST);

    registered = 0;

    int result =
        Supla::Network::Connect(Supla::Channel::reg_dev.ServerName, port);
    if (1 == result) {
      uptime.resetConnectionUptime();
      connectionFailCounter = 0;
      supla_log(LOG_DEBUG, "Connected to Supla Server");
    } else {
      supla_log(LOG_DEBUG,
                "Connection fail (%d). Server: %s",
                result,
                Supla::Channel::reg_dev.ServerName);

      Supla::Network::Disconnect();
      wait_for_iterate = millis() + 2000;
      connectionFailCounter++;
      return;
    }
  }

  Supla::Network::Iterate();

  if (srpc_iterate(srpc) == SUPLA_RESULT_FALSE) {
    status(STATUS_ITERATE_FAIL, "Iterate fail");
    Supla::Network::Disconnect();

    wait_for_iterate = millis() + 5000;
    return;
  }

  if (registered == 0) {
    registered = -1;
    status(STATUS_REGISTER_IN_PROGRESS, "Register in progress");
    if (!srpc_ds_async_registerdevice_e(srpc, &Supla::Channel::reg_dev)) {
      supla_log(LOG_DEBUG, "Fatal SRPC failure!");
    }
    Supla::Channel::clearAllUpdateReady();

  } else if (registered == 1) {
    if (Supla::Network::Ping() == false) {
      uptime.setConnectionLostCause(SUPLA_LASTCONNECTIONRESETCAUSE_ACTIVITY_TIMEOUT);
      supla_log(LOG_DEBUG, "TIMEOUT - lost connection with server");
      Supla::Network::Disconnect();
    }

    if (time_diff > 0) {
      // Iterate all elements
      for (auto element = Supla::Element::begin(); element != nullptr;
           element = element->next()) {
        if (!element->iterateConnected(srpc)) {
          break;
        }
      }

      for (int a = 0; a < channel_pin_count; a++) {
        iterate_impulse_counter(&channel_pin[a],
                                &Supla::Channel::reg_dev.channels[a],
                                time_diff,
                                a);
      }

      last_iterate_time = millis();
    }
  }
}

void SuplaDeviceClass::onVersionError(TSDC_SuplaVersionError *version_error) {
  status(STATUS_PROTOCOL_VERSION_ERROR, "Protocol version error");
  Supla::Network::Disconnect();

  wait_for_iterate = millis() + 5000;
}

void SuplaDeviceClass::onRegisterResult(
    TSD_SuplaRegisterDeviceResult *register_device_result) {
  _supla_int_t activity_timeout = 0;

  switch (register_device_result->result_code) {
    // OK scenario
    case SUPLA_RESULTCODE_TRUE:
      activity_timeout = register_device_result->activity_timeout;
      Supla::Network::Instance()->setActivityTimeout(activity_timeout);
      registered = 1;
      supla_log(LOG_DEBUG,
                "Device registered (activity timeout %d s, server version: %d, "
                "server min version: %d)",
                register_device_result->activity_timeout,
                register_device_result->version,
                register_device_result->version_min);
      last_iterate_time = millis();
      status(STATUS_REGISTERED_AND_READY, "Registered and ready.");

      if (activity_timeout != ACTIVITY_TIMEOUT) {
        supla_log(
            LOG_DEBUG, "Changing activity timeout to %d", ACTIVITY_TIMEOUT);
        TDCS_SuplaSetActivityTimeout at;
        at.activity_timeout = ACTIVITY_TIMEOUT;
        srpc_dcs_async_set_activity_timeout(srpc, &at);
      }

      return;

      // NOK scenarios
    case SUPLA_RESULTCODE_BAD_CREDENTIALS:
      status(STATUS_BAD_CREDENTIALS, "Bad credentials!");
      break;

    case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
      status(STATUS_TEMPORARILY_UNAVAILABLE, "Temporarily unavailable!");
      break;

    case SUPLA_RESULTCODE_LOCATION_CONFLICT:
      status(STATUS_LOCATION_CONFLICT, "Location conflict!");
      break;

    case SUPLA_RESULTCODE_CHANNEL_CONFLICT:
      status(STATUS_CHANNEL_CONFLICT, "Channel conflict!");
      break;
    case SUPLA_RESULTCODE_DEVICE_DISABLED:
      status(STATUS_DEVICE_IS_DISABLED, "Device is disabled!");
      break;

    case SUPLA_RESULTCODE_LOCATION_DISABLED:
      status(STATUS_LOCATION_IS_DISABLED, "Location is disabled!");
      break;

    case SUPLA_RESULTCODE_DEVICE_LIMITEXCEEDED:
      status(STATUS_DEVICE_LIMIT_EXCEEDED, "Device limit exceeded!");
      break;

    case SUPLA_RESULTCODE_GUID_ERROR:
      status(STATUS_INVALID_GUID, "Incorrect device GUID!");
      break;

    case SUPLA_RESULTCODE_AUTHKEY_ERROR:
      status(STATUS_INVALID_GUID, "Incorrect AuthKey!");
      break;

    case SUPLA_RESULTCODE_REGISTRATION_DISABLED:
      status(STATUS_INVALID_GUID, "Registration disabled!");
      break;

    case SUPLA_RESULTCODE_NO_LOCATION_AVAILABLE:
      status(STATUS_INVALID_GUID, "No location available!");
      break;

    case SUPLA_RESULTCODE_USER_CONFLICT:
      status(STATUS_INVALID_GUID, "User conflict!");
      break;

    default:
      supla_log(LOG_ERR,
                "Register result code %i",
                register_device_result->result_code);
      break;
  }

  Supla::Network::Disconnect();
  wait_for_iterate = millis() + 5000;
}

void SuplaDeviceClass::channelSetActivityTimeoutResult(
    TSDC_SuplaSetActivityTimeoutResult *result) {
  Supla::Network::Instance()->setActivityTimeout(result->activity_timeout);
  supla_log(
      LOG_DEBUG, "Activity timeout set to %d s", result->activity_timeout);
}

void SuplaDeviceClass::setServerPort(int value) {
  port = value;
}

void SuplaDeviceClass::setSwVersion(const char *swVersion) {
  setString(Supla::Channel::reg_dev.SoftVer, swVersion, SUPLA_SOFTVER_MAXSIZE);
}

int SuplaDeviceClass::getCurrentStatus() {
  return currentStatus;
}

void SuplaDeviceClass::fillStateData(TDSC_ChannelState &channelState) {
  channelState.Fields |= SUPLA_CHANNELSTATE_FIELD_UPTIME |
                  SUPLA_CHANNELSTATE_FIELD_CONNECTIONUPTIME;

  channelState.Uptime = uptime.getUptime();
  channelState.ConnectionUptime = uptime.getConnectionUptime();
  if (uptime.getLastResetCause() > 0) {
    channelState.Fields |= SUPLA_CHANNELSTATE_FIELD_LASTCONNECTIONRESETCAUSE;
    channelState.LastConnectionResetCause = uptime.getLastResetCause();
  }
}

SuplaDeviceClass SuplaDevice;
