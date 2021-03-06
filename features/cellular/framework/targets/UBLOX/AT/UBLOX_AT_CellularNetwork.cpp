/*
 * Copyright (c) 2018, Arm Limited and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "UBLOX_AT_CellularNetwork.h"
#include "UBLOX_AT_CellularStack.h"

using namespace mbed;

UBLOX_AT_CellularNetwork::UBLOX_AT_CellularNetwork(ATHandler &atHandler) : AT_CellularNetwork(atHandler)
{
    _op_act = RAT_UNKNOWN;
    // The authentication to use
    _auth = NSAPI_SECURITY_UNKNOWN;
}

UBLOX_AT_CellularNetwork::~UBLOX_AT_CellularNetwork()
{
    if (_connection_status_cb) {
        _connection_status_cb(NSAPI_EVENT_CONNECTION_STATUS_CHANGE, NSAPI_ERROR_CONNECTION_LOST);
    }
}

NetworkStack *UBLOX_AT_CellularNetwork::get_stack()
{
    if (!_stack) {
        _stack = new UBLOX_AT_CellularStack(_at, _cid, _ip_stack_type);
    }
    return _stack;
}

bool UBLOX_AT_CellularNetwork::get_modem_stack_type(nsapi_ip_stack_t requested_stack)
{
    return requested_stack == IPV4_STACK ? true : false;
}

AT_CellularNetwork::RegistrationMode UBLOX_AT_CellularNetwork::has_registration(RegistrationType reg_type)
{
    return (reg_type == C_REG || reg_type == C_GREG) ? RegistrationModeLAC : RegistrationModeDisable;
}

nsapi_error_t UBLOX_AT_CellularNetwork::set_access_technology_impl(RadioAccessTechnology opRat)
{
    switch(opRat) {
#if defined(TARGET_UBLOX_C030_U201) || defined(TARGET_UBLOX_C027)
        case RAT_GSM:
        case RAT_GSM_COMPACT:
            break;
        case RAT_EGPRS:
            break;
#elif defined(TARGET_UBLOX_C030_U201)
        case RAT_UTRAN:
            break;
        case RAT_HSDPA:
            break;
        case RAT_HSUPA:
            break;
        case RAT_HSDPA_HSUPA:
            break;
#elif defined(TARGET_UBLOX_C030_R410M)
        case RAT_CATM1:
            break;
#elif defined(TARGET_UBLOX_C030_R410M) || defined(TARGET_UBLOX_C030_N211)
        case RAT_NB1:
            break;
#endif
        default: {
            _op_act = RAT_UNKNOWN;
            return NSAPI_ERROR_UNSUPPORTED;
        }
    }

    return NSAPI_ERROR_OK;
}

nsapi_error_t UBLOX_AT_CellularNetwork::connect()
{
    _at.lock();
    nsapi_error_t err = NSAPI_ERROR_NO_CONNECTION;

    // Attempt to establish a connection
#ifdef TARGET_UBLOX_C030_R410M
    err = NSAPI_ERROR_OK;
#else
    err = open_data_channel();
#endif
    if (err != NSAPI_ERROR_OK) {
        // If new PSD context was created and failed to activate, delete it
        if (_new_context_set) {
            disconnect_modem_stack();
        }
        _connect_status = NSAPI_STATUS_DISCONNECTED;
    } else {
        _connect_status = NSAPI_STATUS_GLOBAL_UP;
    }
    _at.unlock();

    if (_connection_status_cb) {
        _connection_status_cb(NSAPI_EVENT_CONNECTION_STATUS_CHANGE, _connect_status);
    }

    return err;
}

nsapi_error_t UBLOX_AT_CellularNetwork::open_data_channel()
{
    bool success = false;
    int active = 0;
    char * config = NULL;
    nsapi_error_t err = NSAPI_ERROR_NO_CONNECTION;
    char imsi[MAX_IMSI_LENGTH+1];

    // do check for stack to validate that we have support for stack
    _stack = get_stack();
    if (!_stack) {
        return err;
    }

    _at.cmd_start("AT+UPSND=" PROFILE ",8");
    _at.cmd_stop();
    _at.resp_start("+UPSND:");
    _at.read_int();
    _at.read_int();
    active = _at.read_int();
    _at.resp_stop();

    if (active == 0) {
        // If the caller hasn't entered an APN, try to find it
        if (_apn == NULL) {
            err = get_imsi(imsi);
            if (err == NSAPI_ERROR_OK) {
                config = (char*)apnconfig(imsi);
            }
        }

        // Attempt to connect
        do {
            get_next_credentials(&config);
            if(_uname && _pwd) {
                _auth = (*_uname && *_pwd) ? _auth : NSAPI_SECURITY_NONE;
            } else {
                _auth = NSAPI_SECURITY_NONE;
            }
            success = activate_profile(_apn, _uname, _pwd);
        } while (!success && config && *config);
    } else {
        // If the profile is already active, we're good
        success = true;
    }

    err = (_at.get_last_error() == NSAPI_ERROR_OK) ? NSAPI_ERROR_OK : NSAPI_ERROR_NO_CONNECTION;

    return err;
}

bool UBLOX_AT_CellularNetwork::activate_profile(const char* apn,
        const char* username,
        const char* password)
{
    bool activated = false;
    bool success = false;

    // Set up the APN
    if (apn) {
        success = false;
        _at.cmd_start("AT+UPSD=0,1,");
        _at.write_string(apn);
        _at.cmd_stop_read_resp();

        if (_at.get_last_error() == NSAPI_ERROR_OK) {
            success = true;
        }
    }
    // Set up the UserName
    if (success && username) {
        success = false;
        _at.cmd_start("AT+UPSD=" PROFILE ",2,");
        _at.write_string(username);
        _at.cmd_stop_read_resp();

        if (_at.get_last_error() == NSAPI_ERROR_OK) {
            success = true;
        }
    }
    // Set up the Password
    if (success && password) {
        success = false;
        _at.cmd_start("AT+UPSD=" PROFILE ",3,");
        _at.write_string(password);
        _at.cmd_stop_read_resp();

        if (_at.get_last_error() == NSAPI_ERROR_OK) {
            success = true;
        }
    }

    if (success) {
        _at.cmd_start("AT+UPSD=" PROFILE ",7,\"0.0.0.0\"");
        _at.cmd_stop_read_resp();

        // Set up the authentication protocol
        // 0 = none
        // 1 = PAP (Password Authentication Protocol)
        // 2 = CHAP (Challenge Handshake Authentication Protocol)
        for (int protocol = nsapi_security_to_modem_security(NSAPI_SECURITY_NONE);
                success && (protocol <= nsapi_security_to_modem_security(NSAPI_SECURITY_CHAP)); protocol++) {
            if ((_auth == NSAPI_SECURITY_UNKNOWN) || (nsapi_security_to_modem_security(_auth) == protocol)) {
                _at.cmd_start("AT+UPSD=0,6,");
                _at.write_int(protocol);
                _at.cmd_stop_read_resp();

                if (_at.get_last_error() == NSAPI_ERROR_OK) {
                    // Activate, wait upto 30 seconds for the connection to be made
                    _at.set_at_timeout(30000);
                    _at.cmd_start("AT+UPSDA=0,3");
                    _at.cmd_stop_read_resp();
                    _at.restore_at_timeout();

                    if (_at.get_last_error() == NSAPI_ERROR_OK) {
                        activated = true;
                    }
                }
            }
        }
    }

    return activated;
}

// Convert nsapi_security_t to the modem security numbers
int UBLOX_AT_CellularNetwork::nsapi_security_to_modem_security(nsapi_security_t nsapi_security)
{
    int modem_security = 3;

    switch (nsapi_security) {
        case NSAPI_SECURITY_NONE:
            modem_security = 0;
            break;
        case NSAPI_SECURITY_PAP:
            modem_security = 1;
            break;
        case NSAPI_SECURITY_CHAP:
            modem_security = 2;
            break;
        case NSAPI_SECURITY_UNKNOWN:
            modem_security = 3;
            break;
        default:
            modem_security = 3;
            break;
    }

    return modem_security;
}

// Disconnect the on board IP stack of the modem.
bool UBLOX_AT_CellularNetwork::disconnect_modem_stack()
{
    bool success = false;

    if (get_ip_address() != NULL) {
        _at.cmd_start("AT+UPSDA=" PROFILE ",4");
        _at.cmd_stop_read_resp();

        if (_at.get_last_error() == NSAPI_ERROR_OK) {
            success = true;
        }
    }

    return success;
}

nsapi_error_t UBLOX_AT_CellularNetwork::get_imsi(char* imsi)
{
    _at.lock();
    _at.cmd_start("AT+CIMI");
    _at.cmd_stop();
    _at.resp_start();
    int len = _at.read_string(imsi, MAX_IMSI_LENGTH);
    if (len > 0) {
        imsi[len] = '\0';
    }
    _at.resp_stop();

    return _at.unlock_return_error();
}

// Get the next set of credentials, based on IMSI.
void UBLOX_AT_CellularNetwork::get_next_credentials(char ** config)
{
    if (*config) {
        _apn    = _APN_GET(*config);
        _uname  = _APN_GET(*config);
        _pwd    = _APN_GET(*config);
    }
}

const char *UBLOX_AT_CellularNetwork::get_gateway()
{
    return get_ip_address();
}
