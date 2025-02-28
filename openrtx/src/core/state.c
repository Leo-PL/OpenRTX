/***************************************************************************
 *   Copyright (C) 2020 by Federico Amedeo Izzo IU2NUO,                    *
 *                         Niccolò Izzo IU2KIN,                            *
 *                         Frederik Saraci IU2NRO,                         *
 *                         Silvano Seva IU2KWO                             *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include <stdio.h>
#include <string.h>
#include <state.h>
#include <battery.h>
#include <hwconfig.h>
#include <interfaces/platform.h>
#include <interfaces/nvmem.h>

#include <cps.h>

state_t state;

void defaultSettingsAndVfo()
{

    //don't need to lock state mutex because this is called from a section
    //that already does that - ui_updatefsm runs in a critical section in
    //the ui thread
    channel_t default_vfo = get_default_channel();
    nvm_writeSettingsAndVfo( &default_settings, &default_vfo );
    state_init();
}

void state_init()
{
    /*
     * Try loading settings from nonvolatile memory and default to sane values
     * in case of failure.
     */
    if(nvm_readSettings(&state.settings) < 0)
    {
        state.settings = default_settings;
        strncpy(state.settings.callsign, "OPNRTX", 10);
    }

    /*
     * Try loading VFO configuration from nonvolatile memory and default to sane
     * values in case of failure.
     */
    if(nvm_readVFOChannelData(&state.channel) < 0)
    {
        state.channel = get_default_channel();
    }

    /*
     * Initialise remaining fields
     */
    #ifdef HAS_RTC
    state.time = rtc_getTime();
    #endif
    state.v_bat  = platform_getVbat();
    state.charge = battery_getCharge(state.v_bat);
    state.rssi   = rtx_getRssi();

    state.channel_index = 1;    // Set default channel index (it is 1-based)
    state.zone_enabled  = false;
    state.rtxStatus     = RTX_OFF;
    state.rtxShutdown   = false;
    state.emergency     = false;

    // Force brightness field to be in range 0 - 100
    if(state.settings.brightness > 100) state.settings.brightness = 100;
}

void state_terminate()
{
    /*
     * Never store a brightness of 0 to avoid booting with a black screen
     */
    if(state.settings.brightness == 0)
    {
        state.settings.brightness = 5;
    }

    nvm_writeSettingsAndVfo(&state.settings, &state.channel);
}

void state_update()
{
    /*
     * Low-pass filtering with a time constant of 10s when updated at 1Hz
     * Original computation: state.v_bat = 0.02*vbat + 0.98*state.v_bat
     * Peak error is 18mV when input voltage is 49mV.
     */
    uint16_t vbat = platform_getVbat();
    state.v_bat  -= (state.v_bat * 2) / 100;
    state.v_bat  += (vbat * 2) / 100;

    state.charge = battery_getCharge(state.v_bat);
    state.rssi = rtx_getRssi();

    #ifdef HAS_RTC
    state.time = rtc_getTime();
    #endif
}

curTime_t state_getLocalTime(curTime_t utc_time)
{
    curTime_t local_time = utc_time;
    if(local_time.hour + state.settings.utc_timezone >= 24)
    {
        local_time.hour = local_time.hour - 24 + state.settings.utc_timezone;
        local_time.date += 1;
    }
    else if(local_time.hour + state.settings.utc_timezone < 0)
    {
        local_time.hour = local_time.hour + 24 + state.settings.utc_timezone;
        local_time.date -= 1;
    }
    else
        local_time.hour += state.settings.utc_timezone;
    return local_time;
}

curTime_t state_getUTCTime(curTime_t local_time)
{
    curTime_t utc_time = local_time;
    if(utc_time.hour - state.settings.utc_timezone >= 24)
    {
        utc_time.hour = utc_time.hour - 24 - state.settings.utc_timezone;
        utc_time.date += 1;
    }
    else if(utc_time.hour - state.settings.utc_timezone < 0)
    {
        utc_time.hour = utc_time.hour + 24 - state.settings.utc_timezone;
        local_time.date -= 1;
    }
    else
        utc_time.hour -= state.settings.utc_timezone;
    return utc_time;
}
