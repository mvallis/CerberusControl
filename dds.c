/***********************************************************************
 * dds.c  -  Modified for runtime COM port and clock configuration
 *
 * Changes from original:
 *   - DDS_COM_PORT replaced with static char array + dds_set_com_port()
 *   - DDS_CLOCK replaced with static double + dds_set_clock()
 *   - Added dds_get_clock() accessor
 *   - All other logic unchanged
 ***********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "windows.h"
#include "dds.h"

/////////////////////////////
// Runtime DDS configuration
/////////////////////////////

/* Defaults - can be overridden by dds_set_com_port / dds_set_clock */
static char   dds_com_port[64] = "\\\\.\\COM4";
static double dds_clock        = 3530000000.0;

//#define DDS_ENABLE_VERBOSE

#define DDS_BAUDRATE 125000

#define MAX_DATA_BYTES 8
#define POWER_TWO_THIRTYTWO 4294967296.0

static char to_hex[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

static HANDLE sh = NULL;

/*-----------------------------------------------------------------------
 * Runtime configuration
 *-----------------------------------------------------------------------*/
void dds_set_com_port (const char *port)
{
    if (port != NULL)
        strncpy (dds_com_port, port, sizeof(dds_com_port) - 1);
}

void dds_set_clock (double clock_hz)
{
    if (clock_hz > 0.0)
        dds_clock = clock_hz;
}

double dds_get_clock (void)
{
    return dds_clock;
}

/*-----------------------------------------------------------------------
 * Internal helpers
 *-----------------------------------------------------------------------*/
static int dds_check_response(void) {
    int i;
    DWORD read;
    char buffer[6];

    buffer[0] = 0;

    for (i = 0; i != 6; i++) {
        if ((!ReadFile(sh, buffer + i, 1, &read, NULL)) || (read != 1)) return 0;

        if (buffer[i] == '\n') {
            buffer[i] = 0;
            break;
        }

        if (i == 5) return 0;
    }

#ifdef DDS_ENABLE_VERBOSE
    printf("<== %s\n", buffer);
#endif

    if (strcmp(buffer, "okay") == 0) return 1;

    return 0;
}

/*-----------------------------------------------------------------------
 * Write functions
 *-----------------------------------------------------------------------*/
int dds_write_bytes(uint8_t address, int count, uint8_t* data) {
    int i;
    DWORD written;
    char command[13 + 2 * MAX_DATA_BYTES];

    if ((count < 1) || (count > MAX_DATA_BYTES)) return 0;

    sprintf(command, "write,%02x,%x,", address, count);

    for (i = 0; i != count; i++) {
        command[2 * i + 11] = to_hex[data[i] >> 4];
        command[2 * i + 12] = to_hex[data[i] & 15];
    }

    command[2 * i + 11] = '\n';
    command[2 * i + 12] = 0;

    WriteFile(sh, command, 20, &written, NULL);
    FlushFileBuffers(sh);

#ifdef DDS_ENABLE_VERBOSE
    printf("==> %s", command);
#endif

    return dds_check_response();
}

int dds_write_uint8(uint8_t address, uint8_t param) {
    return dds_write_bytes(address, 1, &param);
}

int dds_write_uint16(uint8_t address, uint16_t param) {
    uint8_t data[2];
    data[1] = param & 255;
    data[0] = param >> 8;
    return dds_write_bytes(address, 2, data);
}

int dds_write_uint16_pair(uint8_t address, uint16_t upper, uint16_t lower) {
    uint8_t data[4];
    data[3] = lower & 255;
    data[2] = lower >> 8;
    data[1] = upper & 255;
    data[0] = upper >> 8;
    return dds_write_bytes(address, 4, data);
}

int dds_write_uint32(uint8_t address, uint32_t param) {
    int i;
    uint8_t data[4];
    for (i = 3; i != -1; i--) {
        data[i] = param & 255;
        param >>= 8;
    }
    return dds_write_bytes(address, 4, data);
}

int dds_write_uint32_pair(uint8_t address, uint32_t upper, uint32_t lower) {
    int i;
    uint8_t data[8];
    for (i = 7; i != 3; i--) {
        data[i] = lower & 255;
        lower >>= 8;
    }
    for (; i != -1; i--) {
        data[i] = upper & 255;
        upper >>= 8;
    }
    return dds_write_bytes(address, 8, data);
}

int dds_write_uint64(uint8_t address, uint64_t param) {
    int i;
    uint8_t data[8];
    for (i = 7; i != -1; i--) {
        data[i] = param & 255;
        param >>= 8;
    }
    return dds_write_bytes(address, 8, data);
}

/*-----------------------------------------------------------------------
 * Read functions
 *-----------------------------------------------------------------------*/
int dds_read_bytes(uint8_t address, int count, uint8_t* data) {
    int i;
    DWORD written;
    DWORD read;
    uint8_t value;
    char buffer[2 * MAX_DATA_BYTES + 1] = {0};
    char command[11];

    if ((count < 1) || (count > MAX_DATA_BYTES)) return 0;

    sprintf(command, "read,%02x,%x\n", address, count);

    WriteFile(sh, command, 10, &written, NULL);
    FlushFileBuffers(sh);

#ifdef DDS_ENABLE_VERBOSE
    printf("==> %s", command);
#endif

    for (i = 0; i != 2 * MAX_DATA_BYTES + 1; i++) {
        char in_char;
        if ((!ReadFile(sh, &in_char, 1, &read, NULL)) || (read != 1)) return 0;
        buffer[i] = in_char;

        if (buffer[i] == '\n') {
            buffer[i] = 0;
            break;
        }

        if (i == 2 * MAX_DATA_BYTES) return 0;
    }

#ifdef DDS_ENABLE_VERBOSE
    printf("<== %s\n", buffer);
#endif

    if (strcmp(buffer, "error") == 0) return 0;

    for (i = 0; i != count; i++) {
        char in_char = buffer[2 * i];

        if ((in_char >= '0') && (in_char <= '9')) value = (uint8_t)(in_char - '0');
        else if ((in_char >= 'a') && (in_char <= 'f')) value = (uint8_t)(in_char - 'a' + 10);
        else return 0;

        in_char = buffer[2 * i + 1];

        if ((in_char >= '0') && (in_char <= '9')) data[i] = (uint8_t)((value << 4) + in_char - '0');
        else if ((in_char >= 'a') && (in_char <= 'f')) data[i] = (uint8_t)((value << 4) + in_char - 'a' + 10);
        else return 0;
    }

    return 1;
}

int dds_read_uint8(uint8_t address, uint8_t* param) {
    return dds_read_bytes(address, 1, param);
}

int dds_read_uint16(uint8_t address, uint16_t* param) {
    uint8_t data[2];
    if (dds_read_bytes(address, 2, data)) {
        *param = (uint16_t)((data[0] << 8) + data[1]);
        return 1;
    }
    return 0;
}

int dds_read_uint16_pair(uint8_t address, uint16_t* upper, uint16_t* lower) {
    uint8_t data[4];
    if (dds_read_bytes(address, 4, data)) {
        *upper = (uint16_t)((data[0] << 8) + data[1]);
        *lower = (uint16_t)((data[2] << 8) + data[3]);
        return 1;
    }
    return 0;
}

int dds_read_uint32(uint8_t address, uint32_t* param) {
    uint8_t data[4];
    if (dds_read_bytes(address, 4, data)) {
        *param = (data[0] << 24) + (data[1] << 16) + (data[2] << 8) + data[3];
        return 1;
    }
    return 0;
}

int dds_read_uint32_pair(uint8_t address, uint32_t* upper, uint32_t* lower) {
    uint8_t data[8];
    if (dds_read_bytes(address, 8, data)) {
        *upper = (data[0] << 24) + (data[1] << 16) + (data[2] << 8) + data[3];
        *lower = (data[4] << 24) + (data[5] << 16) + (data[6] << 8) + data[7];
        return 1;
    }
    return 0;
}

int dds_read_uint64(uint8_t address, uint64_t* param) {
    uint8_t data[8];
    if (dds_read_bytes(address, 8, data)) {
        *param = ((uint64_t)data[0] << 56) + ((uint64_t)data[1] << 48) +
                 ((uint64_t)data[2] << 40) + ((uint64_t)data[3] << 32) +
                 ((uint64_t)data[4] << 24) + ((uint64_t)data[5] << 16) +
                 ((uint64_t)data[6] << 8)  +  (uint64_t)data[7];
        return 1;
    }
    return 0;
}

/*-----------------------------------------------------------------------
 * Control functions
 *-----------------------------------------------------------------------*/
int dds_reset(void) {
    DWORD written;
    WriteFile(sh, "reset\n", 6, &written, NULL);
    FlushFileBuffers(sh);
#ifdef DDS_ENABLE_VERBOSE
    printf("==> reset\n");
#endif
    return dds_check_response();
}

int dds_update(void) {
    DWORD written;
    WriteFile(sh, "update\n", 7, &written, NULL);
    FlushFileBuffers(sh);
#ifdef DDS_ENABLE_VERBOSE
    printf("==> update\n");
#endif
    return dds_check_response();
}

int dds_powerup(void) {
    DWORD written;
    WriteFile(sh, "powerup\n", 8, &written, NULL);
    FlushFileBuffers(sh);
#ifdef DDS_ENABLE_VERBOSE
    printf("==> powerup\n");
#endif
    return dds_check_response();
}

int dds_powerdown(void) {
    DWORD written;
    WriteFile(sh, "powerdown\n", 10, &written, NULL);
    FlushFileBuffers(sh);
#ifdef DDS_ENABLE_VERBOSE
    printf("==> powerdown\n");
#endif
    return dds_check_response();
}

int dds_hello(void) {
    DWORD written;
    WriteFile(sh, "hello\n", 6, &written, NULL);
    FlushFileBuffers(sh);
#ifdef DDS_ENABLE_VERBOSE
    printf("==> hello\n");
#endif
    return dds_check_response();
}

int dds_drctrl(int invert) {
    DWORD written;
    char command[12];
    sprintf(command, "drctrl,%d\n", invert ? 1 : 0);
    WriteFile(sh, command, (DWORD)strlen(command), &written, NULL);
    FlushFileBuffers(sh);
#ifdef DDS_ENABLE_VERBOSE
    printf("==> %s", command);
#endif
    return dds_check_response();
}

int dds_drover(int invert) {
    DWORD written;
    char command[12];
    sprintf(command, "drover,%d\n", invert ? 1 : 0);
    WriteFile(sh, command, (DWORD)strlen(command), &written, NULL);
    FlushFileBuffers(sh);
#ifdef DDS_ENABLE_VERBOSE
    printf("==> %s", command);
#endif
    return dds_check_response();
}

/*-----------------------------------------------------------------------
 * AD9914 high-level functions
 *-----------------------------------------------------------------------*/
int ad9914_calibrate_dac(void) {
    if (!dds_write_uint32(0x03, 0x01052120)) return 0;
    if (!dds_update()) return 0;
    Sleep(1000);
    if (!dds_write_uint32(0x03, 0x00052120)) return 0;
    if (!dds_update()) return 0;
    return 1;
}

int ad9914_single_tone(double frequency, double* actual_frequency) {
    uint32_t word;

    if (frequency > dds_clock) return 0;

    word = (uint32_t)(0.5 + 1000000.0 * frequency * POWER_TWO_THIRTYTWO / dds_clock);

    if (!dds_write_uint32(0x01, 0x00800900)) return 0;
    if (!dds_write_uint32(0x0b, word)) return 0;
    if (!dds_write_uint32(0x0c, 0x00000000)) return 0;

    if (actual_frequency != NULL)
        *actual_frequency = ((double)word) * dds_clock / (1000000.0 * POWER_TWO_THIRTYTWO);

    return 1;
}

int ad9914_ramp_generator(double start_freq, double stop_freq, double period,
                          uint32_t flags, double* actual_start,
                          double* actual_stop, double* actual_period)
{
    double steps, round_steps, bandwidth;
    uint32_t start_word, step_size, stop_word, cfr2;

    if ((stop_freq < start_freq) || (start_freq < 0) ||
        (stop_freq > dds_clock) || (period <= 0))
        return 0;

    steps       = period * dds_clock / (1000000.0 * 24.0);
    round_steps = ceil(steps);
    bandwidth   = stop_freq * round_steps / steps - start_freq;
    start_word  = (uint32_t)(0.5 + 1000000.0 * start_freq *
                  POWER_TWO_THIRTYTWO / dds_clock);
    step_size   = (uint32_t)(0.5 + (1000000.0 * bandwidth *
                  POWER_TWO_THIRTYTWO / dds_clock) / round_steps);
    stop_word   = start_word + step_size * ((uint32_t)round_steps);

    cfr2 = 0x00082900;
    if (flags & DRG_NO_DWELL_LOW)  cfr2 |= (1 << 17);
    if (flags & DRG_NO_DWELL_HIGH) cfr2 |= (1 << 18);

    if (!dds_write_uint32(0x01, cfr2))           return 0;
    if (!dds_write_uint32(0x04, start_word))     return 0;
    if (!dds_write_uint32(0x05, stop_word))      return 0;
    if (!dds_write_uint32(0x06, step_size))      return 0;
    if (!dds_write_uint32(0x07, step_size))      return 0;
    if (!dds_write_uint16_pair(0x08, 1, 1))      return 0;
    if (!dds_write_uint32(0x09, 0))              return 0;
    if (!dds_write_uint32(0x0a, 0))              return 0;

    if (actual_start  != NULL)
        *actual_start  = ((double)start_word) * dds_clock /
                         (1000000.0 * POWER_TWO_THIRTYTWO);
    if (actual_stop   != NULL)
        *actual_stop   = ((double)stop_word)  * dds_clock /
                         (1000000.0 * POWER_TWO_THIRTYTWO);
    if (actual_period != NULL)
        *actual_period = 1000000.0 * round_steps * 24.0 / dds_clock;

    return 1;
}

/*-----------------------------------------------------------------------
 * Init / Deinit  -  now uses runtime dds_com_port
 *-----------------------------------------------------------------------*/
int init_dds(void) {
    DCB sparms;
    COMMTIMEOUTS timeouts;

    sh = CreateFile(dds_com_port, GENERIC_READ | GENERIC_WRITE,
                    0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (sh == INVALID_HANDLE_VALUE) return 0;

    sparms.DCBlength = sizeof(sparms);
    GetCommState(sh, &sparms);
    sparms.BaudRate         = DDS_BAUDRATE;
    sparms.StopBits         = ONESTOPBIT;
    sparms.Parity           = NOPARITY;
    sparms.fBinary          = TRUE;
    sparms.fParity          = FALSE;
    sparms.fOutxCtsFlow     = FALSE;
    sparms.fOutxDsrFlow     = FALSE;
    sparms.fDtrControl      = DTR_CONTROL_DISABLE;
    sparms.fDsrSensitivity  = FALSE;
    sparms.fOutX            = FALSE;
    sparms.fInX             = FALSE;
    sparms.fErrorChar       = FALSE;
    sparms.fNull            = FALSE;
    sparms.fRtsControl      = RTS_CONTROL_DISABLE;
    sparms.fAbortOnError    = FALSE;
    sparms.ByteSize         = 8;
    if (!SetCommState(sh, &sparms)) goto error;

    timeouts.ReadIntervalTimeout         = 1000;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.ReadTotalTimeoutConstant    = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = 1000;
    if (!SetCommTimeouts(sh, &timeouts)) goto error;

    if (dds_hello()) return 1;
    else if (dds_hello()) return 1;

error:
    CloseHandle(sh);
    sh = NULL;
    return 0;
}

int deinit_dds(void) {
    if (sh == NULL) return 0;
    CloseHandle(sh);
    sh = NULL;
    return 1;
}
