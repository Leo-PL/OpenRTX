/***************************************************************************
 *   Copyright (C) 2021 - 2022 by Federico Amedeo Izzo IU2NUO,             *
 *                                Niccolò Izzo IU2KIN                      *
 *                                Wojciech Kaczmarski SP5WWP               *
 *                                Frederik Saraci IU2NRO                   *
 *                                Silvano Seva IU2KWO                      *
 *                                                                         *
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

#include <M17/M17Demodulator.h>
#include <M17/M17DSP.h>
#include <M17/M17Utils.h>
#include <interfaces/audio_stream.h>
#include <interfaces/gpio.h>
#include <math.h>
#include <cstring>
#include <stdio.h>
#include <interfaces/graphics.h>
#include <deque>

#ifdef PLATFORM_LINUX
#include <emulator.h>
#endif

using namespace M17;

M17Demodulator::M17Demodulator()
{

}

M17Demodulator::~M17Demodulator()
{
    terminate();
}

void M17Demodulator::init()
{
    /*
     * Allocate a chunk of memory to contain two complete buffers for baseband
     * audio. Split this chunk in two separate blocks for double buffering using
     * placement new.
     */

    baseband_buffer = new int16_t[2 * M17_SAMPLE_BUF_SIZE];
    baseband        = { nullptr, 0 };
    activeFrame     = new frame_t;
    rawFrame        = new uint16_t[M17_FRAME_SYMBOLS];
    idleFrame       = new frame_t;
    frameIndex      = 0;
    phase           = 0;
    locked          = false;
    newFrame        = false;

    #ifdef PLATFORM_LINUX
    FILE *csv_log = fopen("demod_log_1.csv", "w");
    fprintf(csv_log, "Signal,Convolution,Threshold,Offset\n");
    fclose(csv_log);
    csv_log = fopen("demod_log_2.csv", "w");
    fprintf(csv_log, "Sample,Max,Min,Symbol,I\n");
    fclose(csv_log);
    #endif // PLATFORM_MOD17
}

void M17Demodulator::terminate()
{
    // Delete the buffers and deallocate memory.
    delete[] baseband_buffer;
    delete activeFrame;
    delete[] rawFrame;
    delete idleFrame;
}

void M17Demodulator::startBasebandSampling()
{
    basebandId = inputStream_start(SOURCE_RTX, PRIO_RX,
                                   baseband_buffer,
                                   2 * M17_SAMPLE_BUF_SIZE,
                                   BUF_CIRC_DOUBLE,
                                   M17_RX_SAMPLE_RATE);
    // Clean start of the demodulation statistics
    resetCorrelationStats();
}

void M17Demodulator::stopBasebandSampling()
{
     inputStream_stop(basebandId);
}

void M17Demodulator::resetCorrelationStats()
{
    conv_ema   = 0.0f;
    conv_emvar = 800000000.0f;
}

/**
 * Algorithms taken from
 * https://fanf2.user.srcf.net/hermes/doc/antiforgery/stats.pdf
 */
void M17Demodulator::updateCorrelationStats(int32_t value)
{
    float delta = (float) value - conv_ema;
    float incr  = CONV_STATS_ALPHA * delta;
    conv_ema   += incr;
    conv_emvar  = (1.0f - CONV_STATS_ALPHA) * (conv_emvar + delta * incr);
}

float M17Demodulator::getCorrelationStddev()
{
    return sqrt(conv_emvar);
}

void M17Demodulator::resetQuantizationStats()
{
    qnt_ema = 0.0f;
    qnt_max = 0.0f;
    qnt_min = 0.0f;
}

void M17Demodulator::updateQuantizationStats(int32_t offset)
{
    // If offset is negative use bridge buffer
    int16_t sample = 0;
    if (offset < 0) // When we are at negative offsets use bridge buffer
        sample = basebandBridge[M17_BRIDGE_SIZE + offset];
    else            // Otherwise use regular data buffer
        sample = baseband.data[offset];
    // Compute symbols exponential moving average
    float delta = (float) sample - qnt_ema;
    qnt_ema += CONV_STATS_ALPHA * delta;
    // Remove DC offset
    int16_t s = sample - (int16_t) qnt_ema;
    if (s > qnt_max)
        qnt_max = s;
    else
        qnt_max *= QNT_STATS_ALPHA;
    if (s < qnt_min)
        qnt_min = s;
    else
        qnt_min *= QNT_STATS_ALPHA;
}

int32_t M17Demodulator::convolution(int32_t offset,
                                    int8_t *target,
                                    size_t target_size)
{
    // Compute convolution
    int32_t conv = 0;
    for(uint32_t i = 0; i < target_size; i++)
    {
        int32_t sample_index = offset + i * M17_SAMPLES_PER_SYMBOL;
        int16_t sample = 0;
        // When we are at negative indices use bridge buffer
        if (sample_index < 0)
            sample = basebandBridge[M17_BRIDGE_SIZE + sample_index];
        else
            sample = baseband.data[sample_index];
        conv += (int32_t) target[i] * (int32_t) sample;
    }
    return conv;
}

sync_t M17Demodulator::nextFrameSync(int32_t offset)
{
    #ifdef PLATFORM_LINUX
    FILE *csv_log = fopen("demod_log_1.csv", "a");
    #endif

    sync_t syncword = { -1, false };
    // Find peaks in the correlation between the baseband and the frame syncword
    // Leverage the fact LSF syncword is the opposite of the frame syncword
    // to detect both syncwords at once. Stop early because convolution needs
    // access samples ahead of the starting offset.
    int32_t maxLen = static_cast < int32_t >(baseband.len - M17_BRIDGE_SIZE);
    for(int32_t i = offset; (syncword.index == -1) && (i < maxLen); i++)
    {
        // If we are not locked search for a syncword
        int32_t conv = convolution(i, stream_syncword, M17_SYNCWORD_SYMBOLS);
        updateCorrelationStats(conv);
        updateQuantizationStats(i);

        #ifdef PLATFORM_LINUX
        int16_t sample = 0;
        if (i < 0) // When we are at negative offsets use bridge buffer
            sample = basebandBridge[M17_BRIDGE_SIZE + i];
        else            // Otherwise use regular data buffer
            sample = baseband.data[i];

        fprintf(csv_log, "%" PRId16 ",%d,%f,%d\n",
                sample,
                conv - static_cast< int32_t >(conv_ema),
                CONV_THRESHOLD_FACTOR * getCorrelationStddev(),
                i);
        #endif

        // Positive correlation peak -> frame syncword
        if ((conv - static_cast< int32_t >(conv_ema)) >
            (getCorrelationStddev() * CONV_THRESHOLD_FACTOR))
        {
            syncword.lsf = false;
            syncword.index = i;
        }
        // Negative correlation peak -> LSF syncword
        else if ((conv - static_cast< int32_t >(conv_ema)) <
                 -(getCorrelationStddev() * CONV_THRESHOLD_FACTOR))
        {
            syncword.lsf = true;
            syncword.index = i;
        }
    }

    #ifdef PLATFORM_LINUX
    fclose(csv_log);
    #endif
    return syncword;
}

int8_t M17Demodulator::quantize(int32_t offset)
{
    int16_t sample = 0;
    if (offset < 0) // When we are at negative offsets use bridge buffer
        sample = basebandBridge[M17_BRIDGE_SIZE + offset];
    else            // Otherwise use regular data buffer
        sample = baseband.data[offset];
    // DC offset removal
    int16_t s = sample - static_cast< int16_t >(qnt_ema);
    if (s > static_cast< int16_t >(qnt_max) / 2)
        return +3;
    else if (s < static_cast< int16_t >(qnt_min) / 2)
        return -3;
    else if (s > 0)
        return +1;
    else
        return -1;
}

const frame_t& M17Demodulator::getFrame()
{
    // When a frame is read is not new anymore
    newFrame = false;
    return *activeFrame;
}

bool M17Demodulator::isFrameLSF()
{
    return isLSF;
}

bool M17::M17Demodulator::isLocked()
{
    return locked;
}

uint8_t M17Demodulator::hammingDistance(uint8_t x, uint8_t y)
{
    return __builtin_popcount(x ^ y);
}

bool M17Demodulator::update()
{
    M17::sync_t syncword = { 0, false };
    int32_t offset = locked ? 0 : -(int32_t) M17_BRIDGE_SIZE;
    uint16_t decoded_syms = 0;

    // Read samples from the ADC
    baseband = inputStream_getData(basebandId);

    #ifdef PLATFORM_LINUX
    FILE *csv_log = fopen("demod_log_2.csv", "a");
    #endif

    if(baseband.data != NULL)
    {
        // Apply RRC on the baseband buffer
        for(size_t i = 0; i < baseband.len; i++)
        {
            float elem = static_cast< float >(baseband.data[i]);
            baseband.data[i] = static_cast< int16_t >(M17::rrc(elem));
        }

        // Process the buffer
        while((syncword.index != -1) &&
              ((static_cast< int32_t >(M17_SAMPLES_PER_SYMBOL * decoded_syms) +
                offset + phase) < static_cast < int32_t >(baseband.len)))
        {

            // If we are not locked search for a syncword
            if (!locked)
            {
                syncword = nextFrameSync(offset);
                if (syncword.index != -1) // Lock was just acquired
                {
                    locked = true;
                    isLSF  = syncword.lsf;
                    offset = syncword.index + 1;
                    frameIndex = 0;
                }
            }
            // While we are locked, demodulate available samples
            else
            {
                // Slice the input buffer to extract a frame and quantize
                int32_t symbol_index = offset
                                     + phase
                                     + (M17_SAMPLES_PER_SYMBOL * decoded_syms);

                updateQuantizationStats(symbol_index);
                int8_t symbol = quantize(symbol_index);

                #ifdef PLATFORM_LINUX
                fprintf(csv_log, "%" PRId16 ",%f,%f,%d,%d\n",
                        baseband.data[symbol_index] - (int16_t) qnt_ema,
                        qnt_max / 2,
                        qnt_min / 2,
                        symbol * 666,
                        frameIndex == 0 ? 2300 : symbol_index);
                #endif

                setSymbol<M17_FRAME_BYTES>(*activeFrame, frameIndex, symbol);
                decoded_syms++;
                frameIndex++;

                // If the frame buffer is full switch active and idle frame
                if (frameIndex == M17_FRAME_SYMBOLS)
                {
                    std::swap(activeFrame, idleFrame);
                    frameIndex = 0;
                    newFrame   = true;
                }

                // If syncword is not valid, lock is lost, accept 2 bit errors
                uint8_t hammingSync = hammingDistance((*activeFrame)[0],
                                                      stream_syncword_bytes[0])
                                    + hammingDistance((*activeFrame)[1],
                                                      stream_syncword_bytes[1]);

                uint8_t hammingLsf = hammingDistance((*activeFrame)[0],
                                                     lsf_syncword_bytes[0])
                                   + hammingDistance((*activeFrame)[1],
                                                     lsf_syncword_bytes[1]);

                if ((frameIndex == M17_SYNCWORD_SYMBOLS) &&
                    (hammingSync > 2) && (hammingLsf > 2))
                {
                    locked = false;
                    std::swap(activeFrame, idleFrame);
                    frameIndex = 0;
                    newFrame   = true;

                    #ifdef PLATFORM_MOD17
                    gpio_clearPin(SYNC_LED);
                    #endif // PLATFORM_MOD17
                }
                else if (frameIndex == M17_SYNCWORD_SYMBOLS)
                {
                    #ifdef PLATFORM_MOD17
                    gpio_setPin(SYNC_LED);
                    #endif // PLATFORM_MOD17
                }
            }
        }

        // We are at the end of the buffer
        if (locked)
        {
            // Compute phase of next buffer
            phase = (offset % M17_SAMPLES_PER_SYMBOL) +
                    (baseband.len % M17_SAMPLES_PER_SYMBOL);
        }
        else
        {
            // Copy last N samples to bridge buffer
            memcpy(basebandBridge,
                   baseband.data + (baseband.len - M17_BRIDGE_SIZE),
                   sizeof(int16_t) * M17_BRIDGE_SIZE);
        }
    }

    #ifdef PLATFORM_LINUX
    fclose(csv_log);
    #endif

    return newFrame;
}
