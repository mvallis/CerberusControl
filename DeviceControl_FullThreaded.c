/*===========================================================================
 * DeviceControl_Full.c
 *
 * CVI/LabWindows application for controlling:
 *   1. KMTronic 2-Channel USB Relay  (serial / VISA)
 *   2. Siglent SPD3303X DC Power Supply (USBTMC / VISA)
 *   3. AD9914 DDS via custom USB interface board (COM port)
 *   4. ADLINK PCI-9846H ADC digitiser (WD-DASK)
 *
 * Loads GUI from DeviceControl_Full.uir
 *
 * Build:
 *   Add this .c, dds.c, and the .uir to your CVI project.
 *   Link:  visa32.lib, WD-Dask.lib (or WD-Dask64.lib for x64)
 *===========================================================================*/

#include <windows.h>
#include <utility.h>
#include <userint.h>
#include <ansi_c.h>
#include <stddef.h>
#include <visa.h>
#include <formatio.h>
#include <math.h>

#include "dds.h"
#include "Wd-dask.h"        /* WD-DASK header  */
#include "DeviceControl_FullThreaded.h"



/*---------------------------------------------------------------------------
 * Constants
 *---------------------------------------------------------------------------*/
#define MAX_RESOURCES       32
#define RESOURCE_STR_LEN    256
#define READ_BUF_LEN        256
#define UIR_FILE            "DeviceControl_FullThreaded.uir"

#define ADC_NUM_CHANNELS    2       /* CH0 and CH1 */
#define FFT_MAX_SIZE        65536
#define PI                  3.14159265358979323846

/* Radar binary file header magic */
#define RADAR_FILE_MAGIC    0x52444152  /* "RADR" */

#pragma pack(push, 1)
typedef struct {
    unsigned int   magic;                   /* 0x52444152 */
    unsigned int   num_channels;
    unsigned int   samples_per_trigger;
    unsigned int   num_triggers;
    double         sample_rate_hz;
    double         dds_start_freq_hz;
    double         dds_stop_freq_hz;
    double         dds_sweep_period_us;
    unsigned char  reserved[80];            /* pad to 128 bytes */
} RadarFileHeader;
#pragma pack(pop)

/*---------------------------------------------------------------------------
 * Global Variables
 *---------------------------------------------------------------------------*/

/* ---- VISA (relay + PSU) ---- */
static ViSession defaultRM      = VI_NULL;
static ViSession relaySession   = VI_NULL;
static ViSession psuSession     = VI_NULL;

static char resourceList[MAX_RESOURCES][RESOURCE_STR_LEN];
static int  numResources = 0;

/* ---- Panel handles ---- */
static int mainPanel;
static int relayTabHandle, psuTabHandle, ddsTabHandle, adcTabHandle;

/* ---- Relay state ---- */
static int relay1State = 0, relay2State = 0;

/* ---- PSU state ---- */
static int ch1OutState = 0, ch2OutState = 0, ch3OutState = 0;

/* ---- DDS state ---- */
static int ddsConnected = 0;
static int ddsSweepActive = 0;
static double lastActualPeriod_us = 0.0;   /* remember last sweep period */

/* ---- ADC state ---- */
static I16    adcCard       = -1;
static int    adcRegistered = 0;
static int    adcRunning    = 0;

/* ---- Continuous Acquisition State ---- */
static U16   *dmaBuffer1      = NULL;  /* Replaces adcBuffer */
static U16   *dmaBuffer2      = NULL;  /* Dual buffer architecture */
static U16    adcBufId        = 0;
static U32    halfBufferSize  = 0;    
static U32    samplesPerChirp = 0;
static U32    absoluteSampleCount = 0; 
static int    currentProgDiv      = 1; 

/* ---- Multithreading State ---- */
static CmtTSQHandle dmaQueue            = 0;
static CmtThreadFunctionID consumerThreadID = 0;
static CmtThreadFunctionID producerThreadID = 0;
static volatile int isAcquiring         = 0; /* Replaces consumerRunning */

/* ---- FFT scratch ---- */
static double fftReal[FFT_MAX_SIZE];
static double fftImag[FFT_MAX_SIZE];
static double fftMagCH0[FFT_MAX_SIZE];
static double fftMagCH1[FFT_MAX_SIZE];

/* Ensure these are declared BEFORE any functions like DBEvent_Callback or AdcStartCB */
static int    recording       = 0;
static FILE  *recordFile      = NULL;
static U32    recordedTrigs   = 0;
static char   recordPath[512] = "";

/* ---- UI Plotting & Diagnostic State ---- */
static U16          plotBuffer[FFT_MAX_SIZE * 2];
static int          plotSamples      = 0;
static volatile int plotBusy         = 0;
static int          heartbeatCounter = 0;  /* Diagnostic: counts processed buffers */
static double       timeDataCH0[FFT_MAX_SIZE];
static double       timeDataCH1[FFT_MAX_SIZE];
static double       currentAdcVoltageLimit = 1.0; /* 1.0V or 5.0V based on UI */
static double       currentAdcSampleRateHz = 40000000.0; /* Updated by timing calculator */

/* ---- FMCW Range Axis ---- */
#define FREQ_MULTIPLIER   3       /* Tx chain frequency tripler */
#define SPEED_OF_LIGHT    3.0e8   /* m/s */
static double       rangeAxis[FFT_MAX_SIZE]; /* Range in metres per FFT bin */


/*---------------------------------------------------------------------------
 * Forward Declarations
 *---------------------------------------------------------------------------*/
/* VISA */
static void ScanVISAResources (void);

/* Relay */
static int  Relay_Connect    (const char *resource);
static void Relay_Disconnect (void);
static int  Relay_Set        (int relayNum, int state);

/* PSU */
static int  PSU_Connect      (const char *resource);
static void PSU_Disconnect   (void);
static int  PSU_SendCmd      (const char *cmd);
static int  PSU_Query        (const char *cmd, char *resp, int maxLen);
static int  PSU_SetVoltage   (int ch, double volts);
static int  PSU_SetCurrent   (int ch, double amps);
static int  PSU_OutputOnOff  (int ch, int on);
static void PSU_ReadMeasurements (void);
static void SetPSUDimmed        (int dimmed);

/* ADC helpers */
static void ADC_ComputeFFT   (U16 *data, int nSamples, int chOffset, int nCh, double *outMag);
static void CVICALLBACK ADC_PlotFFT_Deferred (void *callbackData);
static int  ADC_SaveRawFile  (const char *path);
static void ADC_RecordWriteHeader (void);
static void ADC_RecordAppendData  (void);
static void ADC_RecordFinish      (void);
static int  ADC_RestartAcquisition (void);

/* Simple FFT */
static void FFT_Radix2       (double *re, double *im, int n);
static int  NextPow2         (int v);

/* Forward declarations for new functions */
int  CVICALLBACK DiskWriterThreadFunction (void *functionData);
int  CVICALLBACK HardwarePollThreadFunction (void *functionData);
void CVICALLBACK ADC_PlotFFT_Deferred (void *callbackData);
void CVICALLBACK Overrun_Deferred (void *callbackData);
/*===========================================================================
 *  MAIN
 *===========================================================================*/
int main (int argc, char *argv[])
{
    if (InitCVIRTE (0, argv, 0) == 0) return -1;

    if (viOpenDefaultRM (&defaultRM) < VI_SUCCESS)
    {
        MessagePopup ("Error", "Cannot open VISA resource manager.\n"
                      "Make sure NI-VISA is installed.");
        return -1;
    }

    mainPanel = LoadPanel (0, UIR_FILE, MAIN_PANEL);
    if (mainPanel < 0)
    {
        MessagePopup ("Error", "Cannot load .uir file");
        viClose (defaultRM);
        return -1;
    }

    /* Tab page handles - order: 0=PSU, 1=Relay, 2=DDS, 3=ADC */
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 0, &psuTabHandle);
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 1, &relayTabHandle);
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 2, &ddsTabHandle);
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 3, &adcTabHandle);

    /* Initial dimming */
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,       ATTR_DIMMED, 1);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,       ATTR_DIMMED, 1);

    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_TIMER_READBACK, ATTR_ENABLED, 0);
    SetPSUDimmed (1);

    ScanVISAResources ();

    DisplayPanel (mainPanel);
    RunUserInterface ();

    /* Cleanup */
    Relay_Disconnect ();
    PSU_Disconnect ();
    if (ddsConnected) { deinit_dds (); ddsConnected = 0; }
    if (recording) { ADC_RecordFinish (); recording = 0; }
    if (adcRunning)
    {
        U32 dummy1, dummy2;
        WD_AI_AsyncClear (adcCard, &dummy1, &dummy2);
    }
    
    /* REPLACED: Free dual DMA buffers instead of deprecated single buffers */
    if (dmaBuffer1) { WD_Buffer_Free (adcCard, dmaBuffer1); dmaBuffer1 = NULL; }
    if (dmaBuffer2) { WD_Buffer_Free (adcCard, dmaBuffer2); dmaBuffer2 = NULL; }
    
    if (adcRegistered) { WD_AI_ContBufferReset (adcCard); WD_Release_Card (adcCard); }
    if (defaultRM != VI_NULL) viClose (defaultRM);

    DiscardPanel (mainPanel);
    CloseCVIRTE ();
    return 0;
}

/*===========================================================================
 * VISA RESOURCE SCAN (With Logical Aliasing)
 *===========================================================================*/
static void ScanVISAResources (void)
{
    ViFindList findList;
    ViUInt32   numFound;
    ViChar     desc[RESOURCE_STR_LEN];
    char       displayStr[RESOURCE_STR_LEN + 64]; /* Extended buffer for labels */
    ViStatus   status;

    numResources = 0;
    ClearListCtrl (relayTabHandle, RELAY_TAB_RELAY_RING_RESOURCE);
    ClearListCtrl (psuTabHandle,   PSU_TAB_PSU_RING_RESOURCE);

    if (defaultRM == VI_NULL) return;

    /* Query VISA for all instrument resources */
    status = viFindRsrc (defaultRM, "?*INSTR", &findList, &numFound, desc);
    if (status < VI_SUCCESS || numFound == 0) return;

    do {
        /* 1. Store the raw VISA string for the backend connection */
        strncpy (resourceList[numResources], desc, RESOURCE_STR_LEN - 1);

        /* 2. Intercept specific addresses to create logical UI labels */
        if (strstr(desc, "ASRL18::INSTR") != NULL) 
        {
            sprintf(displayStr, "%s (KMTronic USB Relay)", desc);
        } 
        else if (strstr(desc, "USB") != NULL && strstr(desc, "::INSTR") != NULL) 
        {
            /* Heuristic: USBTMC devices are typically the Siglent PSU */
            sprintf(displayStr, "%s (Siglent SPD3303X)", desc);
        }
        else 
        {
            /* Default: Display raw string for unknown devices */
            strcpy(displayStr, desc);
        }

        /* 3. Insert into the UI rings using the friendly label but keeping the index value */
        InsertListItem (relayTabHandle, RELAY_TAB_RELAY_RING_RESOURCE, -1, displayStr, numResources);
        InsertListItem (psuTabHandle,   PSU_TAB_PSU_RING_RESOURCE,   -1, displayStr, numResources);

        numResources++;

    } while (numResources < (int)numFound && numResources < MAX_RESOURCES && 
             viFindNext (findList, desc) == VI_SUCCESS);

    viClose (findList);
}

/*===========================================================================
 *  RELAY DEVICE FUNCTIONS  (unchanged)
 *===========================================================================*/
static int Relay_Connect (const char *resource)
{
    ViStatus s = viOpen (defaultRM, (ViRsrc)resource, VI_NULL, 2000, &relaySession);
    if (s < VI_SUCCESS) return -1;
    viSetAttribute (relaySession, VI_ATTR_ASRL_BAUD,       9600);
    viSetAttribute (relaySession, VI_ATTR_ASRL_DATA_BITS,  8);
    viSetAttribute (relaySession, VI_ATTR_ASRL_PARITY,     VI_ASRL_PAR_NONE);
    viSetAttribute (relaySession, VI_ATTR_ASRL_STOP_BITS,  VI_ASRL_STOP_ONE);
    viSetAttribute (relaySession, VI_ATTR_ASRL_FLOW_CNTRL, VI_ASRL_FLOW_NONE);
    viSetAttribute (relaySession, VI_ATTR_TMO_VALUE,       2000);
    return 0;
}

static void Relay_Disconnect (void)
{
    if (relaySession != VI_NULL) {
        Relay_Set (1, 0); Relay_Set (2, 0);
        viClose (relaySession); relaySession = VI_NULL;
    }
    relay1State = relay2State = 0;
}

static int Relay_Set (int relayNum, int state)
{
    unsigned char cmd[3]; ViUInt32 retCnt;
    if (relaySession == VI_NULL) return -1;
    cmd[0] = 0xFF; cmd[1] = (unsigned char)relayNum;
    cmd[2] = (unsigned char)(state ? 0x01 : 0x00);
    return (viWrite (relaySession, cmd, 3, &retCnt) < VI_SUCCESS) ? -1 : 0;
}

/*===========================================================================
 *  PSU DEVICE FUNCTIONS  (unchanged)
 *===========================================================================*/
static int PSU_Connect (const char *resource)
{
    ViStatus s = viOpen (defaultRM, (ViRsrc)resource, VI_NULL, 5000, &psuSession);
    if (s < VI_SUCCESS) return -1;
    viSetAttribute (psuSession, VI_ATTR_TMO_VALUE,   3000);
    viSetAttribute (psuSession, VI_ATTR_TERMCHAR,    0x0A);
    viSetAttribute (psuSession, VI_ATTR_TERMCHAR_EN, VI_TRUE);
    viSetAttribute (psuSession, VI_ATTR_SEND_END_EN, VI_TRUE);
    return 0;
}

static void PSU_Disconnect (void)
{
    if (psuSession != VI_NULL) {
        PSU_OutputOnOff(1,0); PSU_OutputOnOff(2,0); PSU_OutputOnOff(3,0);
        viClose (psuSession); psuSession = VI_NULL;
    }
    ch1OutState = ch2OutState = ch3OutState = 0;
}

static int PSU_SendCmd (const char *cmd)
{
    char buf[READ_BUF_LEN]; ViUInt32 retCnt;
    if (psuSession == VI_NULL) return -1;
    sprintf (buf, "%s\n", cmd);
    return (viWrite (psuSession, (ViBuf)buf, (ViUInt32)strlen(buf), &retCnt) < VI_SUCCESS) ? -1 : 0;
}

static int PSU_Query (const char *cmd, char *resp, int maxLen)
{
    char buf[READ_BUF_LEN]; ViUInt32 retCnt; ViStatus s;
    if (psuSession == VI_NULL) return -1;
    sprintf (buf, "%s\n", cmd);
    s = viWrite (psuSession, (ViBuf)buf, (ViUInt32)strlen(buf), &retCnt);
    if (s < VI_SUCCESS) return -1;
    memset (resp, 0, maxLen);
    s = viRead (psuSession, (ViBuf)resp, (ViUInt32)(maxLen-1), &retCnt);
    if (s < VI_SUCCESS && s != VI_SUCCESS_TERM_CHAR && s != VI_SUCCESS_MAX_CNT) return -1;
    while (retCnt > 0 && (resp[retCnt-1]=='\n'||resp[retCnt-1]=='\r'||resp[retCnt-1]==' '))
        resp[--retCnt] = '\0';
    return 0;
}

static int PSU_SetVoltage (int ch, double v) { char c[64]; sprintf(c,"CH%d:VOLTage %.3f",ch,v); return PSU_SendCmd(c); }
static int PSU_SetCurrent (int ch, double a) { char c[64]; sprintf(c,"CH%d:CURRent %.3f",ch,a); return PSU_SendCmd(c); }
static int PSU_OutputOnOff(int ch, int on)   { char c[64]; sprintf(c,"OUTPut CH%d,%s",ch,on?"ON":"OFF"); return PSU_SendCmd(c); }

static void PSU_ReadMeasurements (void)
{
    char r[READ_BUF_LEN]; double v;
    if (PSU_Query("MEASure:VOLTage? CH1",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_VOLT_READ,v); }
    if (PSU_Query("MEASure:CURRent? CH1",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_CURR_READ,v); }
    if (PSU_Query("MEASure:POWEr? CH1",  r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_PWR_READ,v);  }
    if (PSU_Query("MEASure:VOLTage? CH2",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_VOLT_READ,v); }
    if (PSU_Query("MEASure:CURRent? CH2",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_CURR_READ,v); }
    if (PSU_Query("MEASure:POWEr? CH2",  r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_PWR_READ,v);  }
}

/*===========================================================================
 *  FFT UTILITY  -  in-place radix-2 Cooley-Tukey
 *===========================================================================*/
static int NextPow2 (int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

static void FFT_Radix2 (double *re, double *im, int n)
{
    int i, j, k, m, step;
    double tr, ti, wr, wi, wpr, wpi, theta;

    /* Bit-reversal permutation */
    j = 0;
    for (i = 0; i < n - 1; i++) {
        if (i < j) {
            tr = re[j]; re[j] = re[i]; re[i] = tr;
            ti = im[j]; im[j] = im[i]; im[i] = ti;
        }
        m = n >> 1;
        while (m >= 1 && j >= m) { j -= m; m >>= 1; }
        j += m;
    }

    /* FFT butterfly */
    for (step = 2; step <= n; step <<= 1) {
        theta = -2.0 * PI / (double)step;
        wpr = cos(theta);
        wpi = sin(theta);
        wr = 1.0; wi = 0.0;
        for (k = 0; k < step / 2; k++) {
            for (i = k; i < n; i += step) {
                j = i + step / 2;
                tr = wr * re[j] - wi * im[j];
                ti = wr * im[j] + wi * re[j];
                re[j] = re[i] - tr;
                im[j] = im[i] - ti;
                re[i] += tr;
                im[i] += ti;
            }
            tr = wr;
            wr = tr * wpr - wi * wpi;
            wi = tr * wpi + wi * wpr;
        }
    }
}

/*===========================================================================
 *  ADC HELPERS
 *===========================================================================*/
/*===========================================================================
 * THREAD 1: Background Disk Writer (Consumer)
 *===========================================================================*/
int CVICALLBACK DiskWriterThreadFunction (void *functionData)
{
    U16 *localBuf = malloc(halfBufferSize * sizeof(U16));
    static int writeCounter = 0;
    if (!localBuf) return -1;

    while (isAcquiring) {
        int itemsRead = CmtReadTSQData(dmaQueue, localBuf, 1, 100, 0);
        
        if (itemsRead > 0) {
            if (recordFile != NULL) {
                fwrite(localBuf, sizeof(U16), halfBufferSize, recordFile);
                recordedTrigs += (halfBufferSize / ADC_NUM_CHANNELS) / samplesPerChirp;
                writeCounter++;
                if (writeCounter % 10 == 0) {
                    char msg[64];
                    sprintf(msg, "Disk Write: %d blocks", writeCounter);
                    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
                }
            }

            /* Pass data to UI plotting routine if idle */
            if (!plotBusy && samplesPerChirp > 0 && currentProgDiv > 0) {
                U32 halfSamplesPerCh = halfBufferSize / ADC_NUM_CHANNELS;
                U32 startPhase = absoluteSampleCount % currentProgDiv;
                U32 offsetToFirstChirp = (startPhase == 0) ? 0 : (currentProgDiv - startPhase);
                
                if (halfSamplesPerCh > (offsetToFirstChirp + samplesPerChirp)) {
                    U32 n_max = (halfSamplesPerCh - samplesPerChirp - offsetToFirstChirp) / currentProgDiv;
                    U32 lastChirpStart = offsetToFirstChirp + (n_max * currentProgDiv);
                    U32 bufferOffset = lastChirpStart * ADC_NUM_CHANNELS;
                    
                    plotBusy = 1;
                    memcpy(plotBuffer, localBuf + bufferOffset, samplesPerChirp * ADC_NUM_CHANNELS * sizeof(U16));
                    plotSamples = samplesPerChirp;
                    PostDeferredCall((DeferredCallbackPtr)ADC_PlotFFT_Deferred, NULL);
                }
            }
            absoluteSampleCount += (halfBufferSize / ADC_NUM_CHANNELS);
        }
    }

    /* Flush queue on termination */
    while (CmtReadTSQData(dmaQueue, localBuf, 1, 0, 0) > 0) {
        if (recordFile) fwrite(localBuf, sizeof(U16), halfBufferSize, recordFile);
    }

    if (recordFile) fflush(recordFile);
    free(localBuf);
    return 0;
}

/*===========================================================================
 * DEFERRED CALLBACK: Overrun Trap
 *===========================================================================*/
void CVICALLBACK Overrun_Deferred (void *callbackData) {
    int overrunType = (int)(intptr_t)callbackData;
    
    if (overrunType == 1) {
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "HALTED: DMA Queue Full (Disk IO too slow)");
    } else {
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "HALTED: Hardware DMA Overrun");
    }
    AdcStopCB(0, 0, EVENT_COMMIT, NULL, 0, 0);
}

/*===========================================================================
 * THREAD 2: Hardware Polling (Producer)
 *===========================================================================*/

int CVICALLBACK HardwarePollThreadFunction (void *functionData)
{
    BOOLEAN halfReady, fStop;
    int activeBuf = 0;
    int queueStatus;

    while (isAcquiring) {
        WD_AI_AsyncDblBufferHalfReady(adcCard, &halfReady, &fStop);
        if (halfReady) {
            U16 *src = (activeBuf == 0) ? dmaBuffer1 : dmaBuffer2;
            queueStatus = CmtWriteTSQData(dmaQueue, src, 1, 0, NULL);
            if (queueStatus < 0 && isAcquiring) {
                PostDeferredCall((DeferredCallbackPtr)Overrun_Deferred, (void*)1);
                break;
            }
            activeBuf = 1 - activeBuf;
            heartbeatCounter++;
            
            // CRITICAL FIX: Tell the driver the buffer is free to be overwritten
            WD_AI_AsyncDblBufferHandled(adcCard); 
        }
        
        if (fStop && isAcquiring) {
            PostDeferredCall((DeferredCallbackPtr)Overrun_Deferred, (void*)0);
            break;
        }
        /* Yielding cpu slice. 1ms sleep is fine once buffer is ~50ms wide */
        Sleep(1);
    }
	
	
    return 0;
}

void CVICALLBACK ADC_PlotFFT_Deferred (void *callbackData)
{
    U32 fftN;
    int plotN;
    int i;
    double startF, stopF, bwDDS_Hz, bwTx_Hz, chirpPeriod_s;
    double rangePerBin;

    if (plotSamples == 0) {
        plotBusy = 0;
        return; 
    }

    /* 1. Time-Domain: Extract, voltage-scale, plot */
    for (i = 0; i < plotSamples; i++) {
        double raw0 = (double)((int)plotBuffer[i * ADC_NUM_CHANNELS] - 32768);
        double raw1 = (double)((int)plotBuffer[i * ADC_NUM_CHANNELS + 1] - 32768);
        timeDataCH0[i] = (raw0 / 32768.0) * currentAdcVoltageLimit;
        timeDataCH1[i] = (raw1 / 32768.0) * currentAdcVoltageLimit;
    }

    DeleteGraphPlot (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, -1, VAL_IMMEDIATE_DRAW);
    {
        int hT0, hT1;
        hT0 = PlotY (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, timeDataCH0, plotSamples, 
               VAL_DOUBLE, VAL_THIN_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_YELLOW);
        hT1 = PlotY (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, timeDataCH1, plotSamples, 
               VAL_DOUBLE, VAL_THIN_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_CYAN);
        SetPlotAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, hT0, ATTR_PLOT_LG_TEXT, "LRX");
        SetPlotAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, hT1, ATTR_PLOT_LG_TEXT, "URX");
        SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, ATTR_LEGEND_VISIBLE, 1);
    }

    /* 2. FFT Computation (Hanning window applied inside ADC_ComputeFFT) */
    ADC_ComputeFFT (plotBuffer, plotSamples, 0, ADC_NUM_CHANNELS, fftMagCH0);
    ADC_ComputeFFT (plotBuffer, plotSamples, 1, ADC_NUM_CHANNELS, fftMagCH1);

    fftN  = (U32)NextPow2 (plotSamples);
    plotN = (int)(fftN / 2);
    if (plotN > FFT_MAX_SIZE / 2) plotN = FFT_MAX_SIZE / 2;

    /* 3. Compute FMCW range axis ----
       Tx bandwidth = FREQ_MULTIPLIER × DDS bandwidth (frequency tripler chain).
       Beat frequency per bin: fb = bin × fs / Nfft
       Range per bin: R = c × fb / (2 × chirp_rate)
                        = c × fs × T_chirp / (2 × Nfft × BW_tx)  */
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_START_FREQ, &startF);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_STOP_FREQ,  &stopF);
    bwDDS_Hz = fabs(stopF - startF) * 1e6;              /* MHz → Hz */
    bwTx_Hz  = (double)FREQ_MULTIPLIER * bwDDS_Hz;      /* After tripler */
    chirpPeriod_s = (lastActualPeriod_us > 0.0) ? lastActualPeriod_us * 1e-6 : 1.0;

    if (bwTx_Hz > 0.0 && currentAdcSampleRateHz > 0.0) {
        rangePerBin = (SPEED_OF_LIGHT * currentAdcSampleRateHz * chirpPeriod_s)
                      / (2.0 * (double)fftN * bwTx_Hz);
    } else {
        rangePerBin = 1.0; /* fallback: bin index */
    }

    for (i = 0; i < plotN; i++) {
        rangeAxis[i] = (double)i * rangePerBin;
    }

    /* 4. Plot FFT with range X-axis (PlotXY) */
    DeleteGraphPlot (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, -1, VAL_IMMEDIATE_DRAW);
    {
        int hF0, hF1;
        hF0 = PlotXY (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, rangeAxis, fftMagCH0, plotN,
                VAL_DOUBLE, VAL_DOUBLE, VAL_THIN_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_YELLOW);
        hF1 = PlotXY (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, rangeAxis, fftMagCH1, plotN,
                VAL_DOUBLE, VAL_DOUBLE, VAL_THIN_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_CYAN);
        SetPlotAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, hF0, ATTR_PLOT_LG_TEXT, "LRX");
        SetPlotAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, hF1, ATTR_PLOT_LG_TEXT, "URX");
        SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, ATTR_LEGEND_VISIBLE, 1);
        SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, ATTR_XNAME, "Range (m)");
        SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_GRAPH_FFT, ATTR_YNAME, "Magnitude (dB)");
    }
    plotBusy = 0;
}

/* Compute FFT of one channel from interleaved data, store magnitude in outMag */
static void ADC_ComputeFFT (U16 *data, int nSamples, int chOffset, int nCh,
                            double *outMag)
{
    int i, fftN;

    fftN = NextPow2 (nSamples);
    if (fftN > FFT_MAX_SIZE) fftN = FFT_MAX_SIZE;

    /* Zero-fill */
    memset (fftReal, 0, sizeof(double) * fftN);
    memset (fftImag, 0, sizeof(double) * fftN);

    /* Copy channel data with Hanning window, convert 16-bit unsigned to signed */
    for (i = 0; i < nSamples && i < fftN; i++)
    {
        double raw = (double)((int)data[i * nCh + chOffset] - 32768);
        double win = 0.5 * (1.0 - cos(2.0 * PI * (double)i / (double)(nSamples - 1)));
        fftReal[i] = raw * win;
    }

    FFT_Radix2 (fftReal, fftImag, fftN);

    /* Magnitude in dB (first half only) */
    for (i = 0; i < fftN / 2; i++)
    {
        double mag = sqrt (fftReal[i] * fftReal[i] + fftImag[i] * fftImag[i]);
        outMag[i] = (mag > 0.0) ? 20.0 * log10(mag) : -200.0;
    }
}


static int ADC_SaveRawFile (const char *path)
{
    FILE *fp;
    RadarFileHeader hdr;
    U32 sampPerTrig;
    double startF, stopF, period, sampleRate;

    /* Use plotBuffer for static snapshots instead of deprecated adcBuffer */
    if (plotSamples == 0) return -1;

    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_NUM_SAMP_PER_TRIG, &sampPerTrig);

    sampleRate = currentAdcSampleRateHz;

    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_START_FREQ, &startF);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_STOP_FREQ,  &stopF);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_PERIOD,     &period);

    memset (&hdr, 0, sizeof(hdr));
    hdr.magic               = RADAR_FILE_MAGIC;
    hdr.num_channels        = ADC_NUM_CHANNELS;
    hdr.samples_per_trigger = plotSamples;
    hdr.num_triggers        = 1;  /* A static snapshot is a single trigger burst */
    hdr.sample_rate_hz      = sampleRate;
    hdr.dds_start_freq_hz   = startF * 1e6;   /* MHz to Hz for file header */
    hdr.dds_stop_freq_hz    = stopF  * 1e6;
    hdr.dds_sweep_period_us = period;

    fp = fopen (path, "wb");
    if (fp == NULL) return -1;

    fwrite (&hdr, sizeof(hdr), 1, fp);
    /* Write the most recent frame extracted by the plotting routine */
    fwrite (plotBuffer, sizeof(U16), plotSamples * ADC_NUM_CHANNELS, fp);
    fclose (fp);

    return 0;
}

/*--- Recording helpers: write header, append per-acquisition, finalise ---*/

static void ADC_RecordWriteHeader (void)
{
    RadarFileHeader hdr;
    U32 sampPerTrig;
    double startF, stopF, period;

    if (recordFile == NULL) return;

    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_NUM_SAMP_PER_TRIG, &sampPerTrig);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_START_FREQ, &startF);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_STOP_FREQ,  &stopF);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_PERIOD,      &period);

    memset (&hdr, 0, sizeof(hdr));
    hdr.magic               = RADAR_FILE_MAGIC;
    hdr.num_channels        = ADC_NUM_CHANNELS;
    hdr.samples_per_trigger = sampPerTrig;
    hdr.num_triggers        = 0;   /* updated on finish */
    hdr.sample_rate_hz      = currentAdcSampleRateHz;
    hdr.dds_start_freq_hz   = startF * 1e6;   /* MHz -> Hz for file header */
    hdr.dds_stop_freq_hz    = stopF  * 1e6;
    hdr.dds_sweep_period_us = period;

    fwrite (&hdr, sizeof(hdr), 1, recordFile);
    fflush (recordFile);
}


/* Rewrite header with final trigger count and close file */
static void ADC_RecordFinish (void)
{
    if (recordFile == NULL) return;

    /* Seek back to the num_triggers field and update it */
    fseek (recordFile, offsetof(RadarFileHeader, num_triggers), SEEK_SET);
    fwrite (&recordedTrigs, sizeof(U32), 1, recordFile);

    fclose (recordFile);
    recordFile = NULL;
}

///* Helper: restart acquisition with current settings (for continuous recording) */
//static int ADC_RestartAcquisition (void)
//{
//    U32 sampPerTrig, numTrigs, totalScans;
//    I16 err;

//    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_NUM_SAMP_PER_TRIG, &sampPerTrig);
//    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_NUM_TRIGGERS,       &numTrigs);
//    totalScans = sampPerTrig * numTrigs;

//    err = WD_AI_ContScanChannels (adcCard, 1, adcBufId,
//                                  totalScans, 1, 1, ASYNCH_OP);
//    if (err != NoError) return -1;

//    adcRunning = 1;
//    return 0;
//}

/*===========================================================================
 *  CALLBACKS  -  MAIN PANEL
 *===========================================================================*/
int CVICALLBACK MainPanelCB (int panel, int event, void *cbd, int ed1, int ed2)
{
    if (event == EVENT_CLOSE) QuitUserInterface (0);
    return 0;
}

/*===========================================================================
 *  CALLBACKS  -  RELAY TAB
 *===========================================================================*/
int CVICALLBACK RlConnectCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int idx;
    if (ev != EVENT_COMMIT) return 0;
    GetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_RING_RESOURCE, &idx);
    if (idx < 0 || idx >= numResources) { MessagePopup("Relay","Select a resource."); return 0; }
    if (Relay_Connect (resourceList[idx]) < 0)
    { SetCtrlVal(relayTabHandle,RELAY_TAB_RELAY_MSG_STATUS,"Status: FAILED"); return 0; }
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_MSG_STATUS, "Status: Connected");
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_CONNECT,    ATTR_DIMMED, 1);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_DISCONNECT, ATTR_DIMMED, 0);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,       ATTR_DIMMED, 0);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,       ATTR_DIMMED, 0);
    relay1State = relay2State = 0;
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, 0);
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY2, 0);
    return 0;
}

int CVICALLBACK RlDisconnect (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    Relay_Disconnect ();
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_MSG_STATUS, "Status: Disconnected");
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_CONNECT,    ATTR_DIMMED, 0);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,       ATTR_DIMMED, 1);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,       ATTR_DIMMED, 1);
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, 0);
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY2, 0);
    return 0;
}

int CVICALLBACK Rl1CB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    relay1State = !relay1State;
    if (Relay_Set(1,relay1State)<0) { relay1State=!relay1State; return 0; }
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, relay1State);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1, ATTR_LABEL_TEXT,
                      relay1State ? "Relay 1: ON" : "Relay 1: OFF");
    return 0;
}

int CVICALLBACK Rl2CB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    relay2State = !relay2State;
    if (Relay_Set(2,relay2State)<0) { relay2State=!relay2State; return 0; }
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY2, relay2State);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2, ATTR_LABEL_TEXT,
                      relay2State ? "Relay 2: ON" : "Relay 2: OFF");
    return 0;
}

int CVICALLBACK RefreshResCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    ScanVISAResources ();
    return 0;
}

/*===========================================================================
 *  CALLBACKS  -  PSU TAB  (compact — same logic as before)
 *===========================================================================*/
static void SetPSUDimmed (int d)
{
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_NUM_CH1_VOLT_SET, ATTR_DIMMED, d);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_NUM_CH1_CURR_SET, ATTR_DIMMED, d);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH1_APPLY,    ATTR_DIMMED, d);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH1_OUTPUT,   ATTR_DIMMED, d);

    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_NUM_CH2_VOLT_SET, ATTR_DIMMED, d);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_NUM_CH2_CURR_SET, ATTR_DIMMED, d);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH2_APPLY,    ATTR_DIMMED, d);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH2_OUTPUT,   ATTR_DIMMED, d);

    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH3_OUTPUT,   ATTR_DIMMED, d);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_ALL_OUTPUT,   ATTR_DIMMED, d);
}

int CVICALLBACK PsuConnectCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int idx; char resp[READ_BUF_LEN];
    if (ev != EVENT_COMMIT) return 0;
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_RING_RESOURCE, &idx);
    if (idx<0||idx>=numResources) { MessagePopup("PSU","Select a resource."); return 0; }
    if (PSU_Connect(resourceList[idx])<0) { SetCtrlVal(psuTabHandle,PSU_TAB_PSU_MSG_STATUS,"Status: FAILED"); return 0; }
    if (PSU_Query("*IDN?",resp,sizeof(resp))==0) SetCtrlVal(psuTabHandle,PSU_TAB_PSU_MSG_IDN,resp);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_MSG_STATUS,"Status: Connected");
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_CONNECT,ATTR_DIMMED,1);
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_DISCONNECT,ATTR_DIMMED,0);
    SetPSUDimmed (0);
    ch1OutState=ch2OutState=ch3OutState=0;
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH1_OUTPUT,0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH2_OUTPUT,0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH3_OUTPUT,0);
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_ALL_OUTPUT,ATTR_LABEL_TEXT,"ALL OUT: OFF");
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_TIMER_READBACK,ATTR_ENABLED,1);
    return 0;
}

int CVICALLBACK PsuDisconnect (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_TIMER_READBACK,ATTR_ENABLED,0);
    PSU_Disconnect();
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_MSG_STATUS,"Status: Disconnected");
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_MSG_IDN,"");
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_CONNECT,ATTR_DIMMED,0);
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_DISCONNECT,ATTR_DIMMED,1);
    SetPSUDimmed (1);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH1_OUTPUT,0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH2_OUTPUT,0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH3_OUTPUT,0);
    SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_ALL_OUTPUT,ATTR_LABEL_TEXT,"ALL OUT: OFF");
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_VOLT_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_CURR_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_PWR_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_VOLT_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_CURR_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_PWR_READ,0.0);
    return 0;
}

int CVICALLBACK CH1ApplyCB (int p,int c,int ev,void*cbd,int e1,int e2)
{ double v,a; if(ev!=EVENT_COMMIT)return 0;
  GetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_VOLT_SET,&v);
  GetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_CURR_SET,&a);
  PSU_SetVoltage(1,v); PSU_SetCurrent(1,a); return 0; }

int CVICALLBACK CH2ApplyCB (int p,int c,int ev,void*cbd,int e1,int e2)
{ double v,a; if(ev!=EVENT_COMMIT)return 0;
  GetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_VOLT_SET,&v);
  GetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_CURR_SET,&a);
  PSU_SetVoltage(2,v); PSU_SetCurrent(2,a); return 0; }

int CVICALLBACK CH1OutCB (int p,int c,int ev,void*cbd,int e1,int e2)
{ if(ev!=EVENT_COMMIT)return 0; ch1OutState=!ch1OutState; PSU_OutputOnOff(1,ch1OutState);
  SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH1_OUTPUT,ch1OutState);
  SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_CH1_OUTPUT,ATTR_LABEL_TEXT,ch1OutState?"CH1 OUT: ON":"CH1 OUT: OFF"); return 0; }

int CVICALLBACK CH2OutCB (int p,int c,int ev,void*cbd,int e1,int e2)
{ if(ev!=EVENT_COMMIT)return 0; ch2OutState=!ch2OutState; PSU_OutputOnOff(2,ch2OutState);
  SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH2_OUTPUT,ch2OutState);
  SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_CH2_OUTPUT,ATTR_LABEL_TEXT,ch2OutState?"CH2 OUT: ON":"CH2 OUT: OFF"); return 0; }

int CVICALLBACK CH3OutCB (int p,int c,int ev,void*cbd,int e1,int e2)
{ if(ev!=EVENT_COMMIT)return 0; ch3OutState=!ch3OutState; PSU_OutputOnOff(3,ch3OutState);
  SetCtrlVal(psuTabHandle,PSU_TAB_PSU_LED_CH3_OUTPUT,ch3OutState);
  SetCtrlAttribute(psuTabHandle,PSU_TAB_PSU_BTN_CH3_OUTPUT,ATTR_LABEL_TEXT,ch3OutState?"CH3 OUT: ON":"CH3 OUT: OFF"); return 0; }

int CVICALLBACK PsuAllOutCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int allOn;
    if (ev != EVENT_COMMIT) return 0;

    /* If any channel is off, turn all on; if all are on, turn all off */
    allOn = (ch1OutState && ch2OutState && ch3OutState);

    if (allOn)
    {
        /* Turn all OFF */
        ch1OutState = ch2OutState = ch3OutState = 0;
        PSU_OutputOnOff (1, 0);
        PSU_OutputOnOff (2, 0);
        PSU_OutputOnOff (3, 0);
    }
    else
    {
        /* Turn all ON */
        ch1OutState = ch2OutState = ch3OutState = 1;
        PSU_OutputOnOff (1, 1);
        PSU_OutputOnOff (2, 1);
        PSU_OutputOnOff (3, 1);
    }

    /* Update all LEDs and labels */
    SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH1_OUTPUT, ch1OutState);
    SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH2_OUTPUT, ch2OutState);
    SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH3_OUTPUT, ch3OutState);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH1_OUTPUT, ATTR_LABEL_TEXT,
                      ch1OutState ? "CH1 OUT: ON" : "CH1 OUT: OFF");
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH2_OUTPUT, ATTR_LABEL_TEXT,
                      ch2OutState ? "CH2 OUT: ON" : "CH2 OUT: OFF");
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH3_OUTPUT, ATTR_LABEL_TEXT,
                      ch3OutState ? "CH3 OUT: ON" : "CH3 OUT: OFF");
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_ALL_OUTPUT, ATTR_LABEL_TEXT,
                      ch1OutState ? "ALL OUT: ON" : "ALL OUT: OFF");

    return 0;
}

int CVICALLBACK ReadbackTimerCB (int p,int c,int ev,void*cbd,int e1,int e2)
{ if(ev!=EVENT_TIMER_TICK)return 0; if(psuSession!=VI_NULL) PSU_ReadMeasurements(); return 0; }

/*===========================================================================
 *  DDS TIMING CALCULATION HELPER
 *
 *  Hardware signal chain:
 *    SYNC_CLK = DDS_CLOCK / 24          (AD9914 internal)
 *    ADC_CLK  = SYNC_CLK / (hmcDiv × 2) (HMC432 #1 × HMC432 #2 fixed ÷2)
 *    Trigger  = ADC_CLK / progDiv        (programmable divider)
 *             = SYNC_CLK / (hmcDiv × 2 × progDiv)
 *
 *  Chirp duration  = sampsPerChirp × hmcDiv × 2   SYNC_CLK cycles
 *  DRCTRL period   = hmcDiv × 2 × progDiv         SYNC_CLK cycles
 *  Dead time       = hmcDiv × 2 × (progDiv - sampsPerChirp)  SYNC_CLK cycles
 *  Dead samples    = progDiv - sampsPerChirp       ADC samples
 *  Min progDiv     = sampsPerChirp + 1
 *===========================================================================*/
/*static double lastActualPeriod_us = 0.0;    remember last sweep period  */

static void DDS_UpdateTimingDisplay (void)
{
    double clockMHz, syncClkMHz, syncClkPeriod_us;
    double adcClkMHz, trigFreqHz, drctrlPeriod_us, deadTime_us, calcPeriod_us;
    int    hmcDiv, progDiv, sampsPerChirp;
    int    adcClkDivTotal, chirpSteps, drctrlSteps, minProgDiv, deadSamples;
    char   warnMsg[256];

    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CLOCK_MHZ,       &clockMHz);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_HMC_DIV,         &hmcDiv);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_PROG_DIV,        &progDiv);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_SAMPS_PER_CHI, &sampsPerChirp);

    if (hmcDiv < 2)         hmcDiv = 2;
    if (progDiv < 1)        progDiv = 1;
    if (sampsPerChirp < 1)  sampsPerChirp = 1;

    /* SYNC_CLK = DDS_CLOCK / 24 */
    syncClkMHz       = clockMHz / 24.0;
    syncClkPeriod_us = (syncClkMHz > 0.0) ? 1.0 / syncClkMHz : 0.0;

    /* ADC_CLK = SYNC_CLK / (hmcDiv × 2)   [HMC432 #1 + fixed ÷2] */
    adcClkDivTotal = hmcDiv * 2;
    adcClkMHz      = syncClkMHz / (double)adcClkDivTotal;

    /* Update global sample rate for file headers */
    currentAdcSampleRateHz = adcClkMHz * 1e6;

    /* Chirp: exactly sampsPerChirp ADC samples = sampsPerChirp × adcClkDivTotal SYNC_CLK cycles */
    chirpSteps    = sampsPerChirp * adcClkDivTotal;
    calcPeriod_us = (double)chirpSteps * syncClkPeriod_us;

    /* DRCTRL trigger = ADC_CLK / progDiv */
    drctrlSteps     = adcClkDivTotal * progDiv;
    trigFreqHz      = (adcClkMHz > 0.0) ? adcClkMHz * 1e6 / (double)progDiv : 0.0;
    drctrlPeriod_us = (double)drctrlSteps * syncClkPeriod_us;

    /* Dead time */
    deadTime_us = drctrlPeriod_us - lastActualPeriod_us;
    deadSamples = progDiv - sampsPerChirp;
    minProgDiv  = sampsPerChirp + 1;

    /* Update indicators */
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_SYNC_CLK,       syncClkMHz);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ADC_CLK,        adcClkMHz);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_TRIG_FREQ,      trigFreqHz);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_DRCTRL_PERIOD,  drctrlPeriod_us);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CHIRP_STEPS,    chirpSteps);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CALC_PERIOD,    calcPeriod_us);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_DEAD_TIME,      deadTime_us);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_DEAD_SAMPLES,   deadSamples);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_MIN_PROG_DIV,   minProgDiv);

    /* Validation warnings */
    warnMsg[0] = '\0';

    if (progDiv <= sampsPerChirp)
    {
        sprintf (warnMsg, "WARNING: Prog divider %d too small! "
                 "Need > %d samples/chirp. No dead time!",
                 progDiv, sampsPerChirp);
    }
    else if (deadTime_us < 0.0 && lastActualPeriod_us > 0.0)
    {
        sprintf (warnMsg, "WARNING: Actual sweep (%.3f us) exceeds "
                 "DRCTRL period (%.3f us). Increase prog divider.",
                 lastActualPeriod_us, drctrlPeriod_us);
    }

    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_TIMING_WARN, warnMsg);
}

/*===========================================================================
 *  CALLBACKS  -  DDS TAB
 *===========================================================================*/
int CVICALLBACK DdsConnectCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    char comStr[64];
    char fullPort[80];
    double clockMHz;

    if (ev != EVENT_COMMIT) return 0;

    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_STR_COM_PORT, comStr);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CLOCK_MHZ, &clockMHz);

    /* Build full path: "\\\\.\\COMx" */
    sprintf (fullPort, "\\\\.\\%s", comStr);
    dds_set_com_port (fullPort);
    dds_set_clock (clockMHz * 1e6);

    if (!init_dds ())
    {
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Status: Connection FAILED");
        return 0;
    }

    ddsConnected = 1;
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Status: Connected");
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_CONNECT,    ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_DISCONNECT, ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_INIT_CAL,   ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_START,      ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_STOP,       ATTR_DIMMED, 1);

    return 0;
}

int CVICALLBACK DdsDisconnectCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    if (ddsSweepActive) { dds_powerdown (); ddsSweepActive = 0; }
    deinit_dds ();
    ddsConnected = 0;

    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Status: Disconnected");
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_CONNECT,    ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_INIT_CAL,   ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_START,      ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_STOP,       ATTR_DIMMED, 1);

    return 0;
}

int CVICALLBACK DdsInitCalCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Resetting & calibrating...");
    ProcessSystemEvents ();   /* update UI before blocking call */

    if (!dds_reset ())
    { SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Reset FAILED"); return 0; }

    if (!ad9914_calibrate_dac ())
    { SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "DAC cal FAILED"); return 0; }

    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Init & calibration OK");
    return 0;
}

int CVICALLBACK DdsStartCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    double startF, stopF, period, cwFreq;
    double actStart, actStop, actPeriod, actCW;
    int    chirpMode;
    uint32_t rampFlags;
    char   msg[256];

    if (ev != EVENT_COMMIT) return 0;

    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_RING_CHIRP_MODE, &chirpMode);

    /* chirpMode: 0 = Triggered (DRCTRL), 1 = Free-run, 2 = CW */
    if (chirpMode == 2)
    {
        GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CW_FREQ, &cwFreq);

        /* 1. Reset all internal registers */
        if (!dds_reset ()) { return 0; }

        /* 2. Power up the DDS core and PLL before calibration */
        if (!dds_powerup ()) { return 0; }
        Delay(0.01); /* Allow PLL to achieve lock */

        /* 3. Calibrate the active DAC */
        if (!ad9914_calibrate_dac ()) { return 0; }

        /* 4. Configure Single Tone and Update */
        if (!ad9914_single_tone (cwFreq, &actCW)) { return 0; }
        if (!dds_update ()) { return 0; }

        ddsSweepActive = 1;
        lastActualPeriod_us = 0.0;

        sprintf (msg, "CW output: %.6f MHz (requested %.6f MHz)", actCW, cwFreq);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, msg);

        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_START,  actCW);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_STOP,   0.0);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_PERIOD, 0.0);
    }
    else
    {
        /* ---- Sweep modes (Triggered or Free-run) ---- */
        GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_START_FREQ, &startF);
        GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_STOP_FREQ,  &stopF);
        GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_PERIOD,      &period);

        /* 1. Validation: Prevent Accumulator Hang */
        if (startF == stopF)
        {
            SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "ERROR: Start/Stop frequencies cannot match in Sweep Mode");
            return 0;
        }

        /* 2. Reset all internal registers */
        if (!dds_reset ()) { return 0; }

        /* 3. Power up the DDS core and PLL before calibration */
        if (!dds_powerup ()) { return 0; }
        Delay(0.01); /* Allow PLL to achieve lock */

        /* 4. Calibrate the active DAC */
        if (!ad9914_calibrate_dac ()) { return 0; }

        /* 5. Mode Specific Configuration */
        if (chirpMode == 0)
        {
            rampFlags = DRG_NO_DWELL_HIGH;

            /* Set DRCTRL to non-inverting (rising edge starts chirp) */
            if (!dds_drctrl (0))
            {
                SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "DRCTRL config FAILED");
                return 0;
            }
        }
        else
        {
            rampFlags = DRG_NO_DWELL;
        }

        /* 6. Configure Digital Ramp Generator */
        if (!ad9914_ramp_generator (startF, stopF, period, rampFlags,
                                    &actStart, &actStop, &actPeriod))
        {
            SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Ramp config FAILED");
            return 0;
        }

        /* 7. Apply Configuration to Active Registers */
        if (!dds_update ()) { return 0; }

        ddsSweepActive = 1;

        /* Show actual values */
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_START,  actStart);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_STOP,   actStop);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_PERIOD, actPeriod);

        /* Update timing calculations with actual sweep period */
        lastActualPeriod_us = actPeriod;
        DDS_UpdateTimingDisplay ();

        sprintf (msg, "%s sweep  (%.6f to %.6f MHz, %.3f us)",
                 (chirpMode == 0) ? "Triggered" : "Free-run",
                 actStart, actStop, actPeriod);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, msg);
    }

    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_START, ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_STOP,  ATTR_DIMMED, 0);

    return 0;
}

int CVICALLBACK DdsStopCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    dds_powerdown ();
    ddsSweepActive = 0;

    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Sweep stopped");
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_START, ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, TABPANEL_DDS_BTN_STOP,  ATTR_DIMMED, 1);

    return 0;
}

/* Called when division ratio or clock changes — update timing display live */
int CVICALLBACK DdsDivRatioCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT && ev != EVENT_VAL_CHANGED) return 0;
    DDS_UpdateTimingDisplay ();
    return 0;
}

/*===========================================================================
 *  CALLBACKS  -  ADC TAB
 *===========================================================================*/
int CVICALLBACK AdcStrictDiagnosticCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    I16  err;
    U16  testCard;
    U16  chans[2] = {0, 1};
    U16  bufId;
    U32  testSamplesPerCh = 2048; /* Small, safe, aligned even integer */
    U32  testTotalSamples = testSamplesPerCh * 2 * 2; /* 2 channels, 2 half-buffers */
    U16 *testBuffer = NULL;
    BOOLEAN halfReady, stopFlag;
    int  buffersProcessed = 0;

    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "DIAG: Registering...");
    ProcessSystemEvents();

    /* 1. Pristine Registration */
    testCard = WD_Register_Card(PCI_9846H, 0);
    if (testCard < 0) {
        MessagePopup("DIAG ERROR", "Failed to register card.");
        return 0;
    }

    /* 2. Strict Configuration */
    WD_AI_Config(testCard, WD_IntTimeBase, 0, WD_AI_ADCONVSRC_TimePacer, 0, 1);
    WD_AI_CH_Config(testCard, 0, AD_B_1_V);
    WD_AI_CH_Config(testCard, 1, AD_B_1_V);

    /* 3. Trigger: External Digital, Infinite continuous */
    err = WD_AI_Trig_Config(testCard, WD_AI_TRGMOD_POST, WD_AI_TRGSRC_ExtD, WD_AI_TrgPositive, 0, 0.0, 0, 0, 0, 1);
    
    /* 4. Memory Allocation (Kernel Aligned) */
    testBuffer = (U16 *)WD_Buffer_Alloc(testCard, testTotalSamples * sizeof(U16));
    if (!testBuffer) {
        WD_Release_Card(testCard);
        MessagePopup("DIAG ERROR", "WD_Buffer_Alloc failed.");
        return 0;
    }

    WD_AI_ContBufferReset(testCard);
    err = WD_AI_ContBufferSetup(testCard, testBuffer, testTotalSamples, &bufId);

    /* 5. Enable Standard Double Buffering */
    err = WD_AI_AsyncDblBufferMode(testCard, 1);

    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "DIAG: Waiting for Trigger...");
    ProcessSystemEvents();

    /* 6. Execute MultiChannel Read */
    err = WD_AI_ContReadMultiChannels(testCard, 2, chans, bufId, testSamplesPerCh, 1, 1, ASYNCH_OP);
    
    if (err != NoError) {
        char msg[128];
        sprintf(msg, "DIAG FAILED at Execution: %d", err);
        MessagePopup("IOCTL Error", msg);
    } else {
        /* 7. Polled Hardware Interrupt Loop (Bypasses Threading) */
        while (buffersProcessed < 4) {
            WD_AI_AsyncDblBufferHalfReady(testCard, &halfReady, &stopFlag);
            if (halfReady) {
                WD_AI_AsyncDblBufferHandled(testCard);
                buffersProcessed++;
                
                char msg[64];
                sprintf(msg, "DIAG: Processed Buffer %d/4", buffersProcessed);
                SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
                ProcessSystemEvents();
            }
            if (stopFlag) break;
        }
        MessagePopup("DIAG SUCCESS", "Hardware successfully streamed 4 half-buffers.");
    }

    /* 8. Mandatory Teardown */
    WD_AI_AsyncClear(testCard, NULL, NULL);
    WD_AI_AsyncDblBufferMode(testCard, 0);
    WD_AI_ContBufferReset(testCard);
    WD_Buffer_Free(testCard, testBuffer);
    WD_Release_Card(testCard);

    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "DIAG: Complete and Released.");
    return 0;
}

int CVICALLBACK AdcRegisterCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int cardNum;
    U32 driverDmaSizeKB = 0;
    char msg[128];

    if (ev != EVENT_COMMIT) return 0;
    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_NUM_CARD_NUM, &cardNum);
    
    adcCard = WD_Register_Card (PCI_9846H, (U16)cardNum);
    if (adcCard < 0) {
        sprintf (msg, "Register FAILED (err %d)", adcCard);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return 0;
    }

    // --- NEW DMA DIAGNOSTIC CHECK ---
    WD_AI_InitialMemoryAllocated(adcCard, &driverDmaSizeKB);
    
    // 128,000 KB is ~128 MB. 
    // If it's less than what we need (e.g. 16MB = 16384 KB), or absurdly high (corrupted registry)
    if (driverDmaSizeKB < 16384 || driverDmaSizeKB > 1048576) {
        sprintf(msg, "FATAL: Invalid Driver DMA Size (%lu KB). Check ADLINK Utility.", driverDmaSizeKB);
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        WD_Release_Card(adcCard);
        adcCard = -1;
        return 0;
    }
    // --------------------------------

    WD_AD_Auto_Calibration_ALL(adcCard);
    adcRegistered = 1;
    
    sprintf(msg, "Card OK. DMA Pool: %lu KB", driverDmaSizeKB);
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
    
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_REGISTER,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_CONFIGURE, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RELEASE,   ATTR_DIMMED, 0);
    return 0;
}

// Inside DeviceControl_FullThreaded.c

int CVICALLBACK AdcConfigureCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int    rangeIdx, impedIdx, pDiv, i;
    U16    adRange, impedance;
    I16    err;
    char   diagMsg[512];
    U32    targetScans, scansPerHalf, adcTotalSamples;

    if (ev != EVENT_COMMIT || !adcRegistered) return 0;

    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_RING_RANGE,     &rangeIdx);
    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_RING_IMPEDANCE, &impedIdx);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_PROG_DIV,     &pDiv);

    adRange = (rangeIdx == 0) ? AD_B_1_V : AD_B_5_V;
    currentAdcVoltageLimit = (rangeIdx == 0) ? 1.0 : 5.0;
    impedance = (impedIdx == 0) ? IMPEDANCE_50Ohm : IMPEDANCE_HI;

    err = WD_AI_Config (adcCard, WD_ExtTimeBase, 1, WD_AI_ADCONVSRC_TimePacer, 0, 1);
    if (err != NoError) {
        sprintf(diagMsg, "AI_Config Error: %d", err);
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, diagMsg);
        return 0;
    }

    for (i = 0; i < ADC_NUM_CHANNELS; i++) {
        WD_AI_CH_Config (adcCard, i, adRange);
        WD_AI_CH_ChangeParam (adcCard, i, AI_IMPEDANCE, impedance);
    }

    if (pDiv <= 0) pDiv = 1;
    samplesPerChirp = 0;
    GetCtrlVal(adcTabHandle, TABPANEL_2_ADC_NUM_SAMP_PER_TRIG, &samplesPerChirp);
    if (samplesPerChirp == 0) samplesPerChirp = 1024;

    /* DYNAMIC BUFFER SIZING: Target 50ms of data per half-buffer */
    targetScans = (U32)(currentAdcSampleRateHz * 0.05);
    if (targetScans < 8192) targetScans = 8192;
    
    /* Align strictly to pDiv so radar sweeps do not straddle buffer edges awkwardly */
    scansPerHalf = ((targetScans / pDiv) + 1) * pDiv;
    halfBufferSize = scansPerHalf * ADC_NUM_CHANNELS;
    adcTotalSamples = halfBufferSize * 2;

    if (dmaBuffer1) { WD_Buffer_Free(adcCard, dmaBuffer1); dmaBuffer1 = NULL; }
    if (dmaBuffer2) { WD_Buffer_Free(adcCard, dmaBuffer2); dmaBuffer2 = NULL; }
    WD_AI_ContBufferReset(adcCard);

    dmaBuffer1 = (U16 *)WD_Buffer_Alloc(adcCard, halfBufferSize * sizeof(U16));
    dmaBuffer2 = (U16 *)WD_Buffer_Alloc(adcCard, halfBufferSize * sizeof(U16));

    if (!dmaBuffer1 || !dmaBuffer2) {
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Memory Allocation Failed");
        return 0;
    }

    err = WD_AI_ContBufferSetup(adcCard, dmaBuffer1, halfBufferSize, &adcBufId);
    err = WD_AI_ContBufferSetup(adcCard, dmaBuffer2, halfBufferSize, &adcBufId);
    WD_AI_AsyncDblBufferMode(adcCard, 1);

    sprintf(diagMsg, "Config OK | Half-Buffer: %lu Samples (~50ms)", (unsigned long)halfBufferSize);
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, diagMsg);
    
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SINGLE,    ATTR_DIMMED, 0);
    return 0;
}
int CVICALLBACK AdcStartCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    I16 err;
    U32 totalScansPerCh;

    if (ev != EVENT_COMMIT || !adcRegistered) return 0;

    GetCtrlVal(adcTabHandle, TABPANEL_2_ADC_NUM_SAMP_PER_TRIG, &samplesPerChirp);
    GetCtrlVal(ddsTabHandle, TABPANEL_DDS_NUM_PROG_DIV, &currentProgDiv);

    absoluteSampleCount = 0;
    heartbeatCounter = 0;
    totalScansPerCh = halfBufferSize / ADC_NUM_CHANNELS;

    WD_AI_ContBufferReset(adcCard);

    /* PostTrigScans MUST be 0 for POST trigger mode */
    err = WD_AI_Trig_Config(adcCard, WD_AI_TRGMOD_POST, WD_AI_TRGSRC_ExtD,
                            WD_AI_TrgPositive, 0, 0.0, 0, 0, 0, 1);
    if (err != NoError) return 0;

    WD_AI_AsyncDblBufferMode(adcCard, 1);

    /* Register dual buffers */
    WD_AI_ContBufferSetup(adcCard, dmaBuffer1, halfBufferSize, &adcBufId);
    WD_AI_ContBufferSetup(adcCard, dmaBuffer2, halfBufferSize, &adcBufId);

    /* Launch Threads */
    CmtNewTSQ(20, halfBufferSize * sizeof(U16), OPT_TSQ_DYNAMIC_SIZE, &dmaQueue);
    isAcquiring = 1;
    adcRunning = 1;
    CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, DiskWriterThreadFunction, NULL, &consumerThreadID);
    CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, HardwarePollThreadFunction, NULL, &producerThreadID);

    /* Arm hardware with totalScansPerCh mapping to the hardware DataCnt register */
    err = WD_AI_ContScanChannels(adcCard, 1, adcBufId, totalScansPerCh, 1, 1, ASYNCH_OP);

    if (err == NoError) {
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Acquisition Armed [Dual-Buffer Polled]");
        SetCtrlAttribute(adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
        SetCtrlAttribute(adcTabHandle, TABPANEL_2_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 0);
    } else {
        AdcStopCB(0, 0, EVENT_COMMIT, NULL, 0, 0);
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Execution Failed");
    }
    return 0;
}

int CVICALLBACK AdcSingleShotCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    I16 err;
    U16 chans[2] = {0, 1};
    U32 totalScans, totalSamples;
    U16 *diagBuffer = NULL;
    U16 diagBufId;
    BOOLEAN bStopped = FALSE;
    U32 accessCnt = 0;
    double startT;

    if (ev != EVENT_COMMIT || !adcRegistered) return 0;

    GetCtrlVal(adcTabHandle, TABPANEL_2_ADC_NUM_SAMP_PER_TRIG, &samplesPerChirp);
    if (samplesPerChirp == 0) return 0;

    totalScans = samplesPerChirp;
    totalSamples = totalScans * ADC_NUM_CHANNELS;

    diagBuffer = (U16 *)WD_Buffer_Alloc(adcCard, totalSamples * sizeof(U16));
    if (!diagBuffer) {
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Diag Memory Alloc Failed");
        return 0;
    }

    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Waiting for Hardware Trigger (Max 5s)...");
    ProcessSystemEvents();

    WD_AI_AsyncDblBufferMode(adcCard, 0);
    WD_AI_ContBufferReset(adcCard);

    err = WD_AI_ContBufferSetup(adcCard, diagBuffer, totalSamples, &diagBufId);
    err = WD_AI_Trig_Config(adcCard, WD_AI_TRGMOD_POST, WD_AI_TRGSRC_ExtD,
                             WD_AI_TrgPositive, 0, 0.0, 0, 0, 0, 1);

    /* Run ASYNCHRONOUSLY to prevent UI freeze */
    err = WD_AI_ContReadMultiChannels(adcCard, ADC_NUM_CHANNELS, chans, diagBufId, totalScans, 1, 1, ASYNCH_OP);
    
    if (err == NoError) {
        startT = Timer();
        /* Poll the driver allowing the UI to breathe, with a 5.0 second timeout */
        while (!bStopped && (Timer() - startT < 5.0)) {
            WD_AI_AsyncCheck(adcCard, &bStopped, &accessCnt);
            ProcessSystemEvents();
            Sleep(10);
        }

        if (!bStopped) {
            /* Hardware never received external trigger */
            WD_AI_AsyncClear(adcCard, &accessCnt, &accessCnt);
            SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Single-shot TIMEOUT: No Ext Trigger");
        } else {
            /* Trigger captured successfully */
            SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Single-shot captured OK");
            
            plotBusy = 1;
            memcpy(plotBuffer, diagBuffer, totalSamples * sizeof(U16));
            plotSamples = samplesPerChirp;
            PostDeferredCall((DeferredCallbackPtr)ADC_PlotFFT_Deferred, NULL);

            {
                int saveChoice = ConfirmPopup("Save Single Shot",
                    "Save this capture to a binary file for MATLAB post-processing?");
                if (saveChoice == 1) {
                    char savePath[MAX_PATHNAME_LEN];
                    int sel = FileSelectPopup("", "*.bin", "Binary Data (*.bin)",
                                              "Save Single-Shot Data",
                                              VAL_SAVE_BUTTON, 0, 0, 1, 1, savePath);
                    if (sel != VAL_NO_FILE_SELECTED) {
                        FILE *fp = fopen(savePath, "wb");
                        if (fp) {
                            RadarFileHeader hdr;
                            double startF, stopF, period;
                            GetCtrlVal(ddsTabHandle, TABPANEL_DDS_NUM_START_FREQ, &startF);
                            GetCtrlVal(ddsTabHandle, TABPANEL_DDS_NUM_STOP_FREQ,  &stopF);
                            GetCtrlVal(ddsTabHandle, TABPANEL_DDS_NUM_PERIOD,      &period);

                            memset(&hdr, 0, sizeof(hdr));
                            hdr.magic               = RADAR_FILE_MAGIC;
                            hdr.num_channels        = ADC_NUM_CHANNELS;
                            hdr.samples_per_trigger = samplesPerChirp;
                            hdr.num_triggers        = 1;
                            hdr.sample_rate_hz      = currentAdcSampleRateHz;
                            hdr.dds_start_freq_hz   = startF * 1e6;
                            hdr.dds_stop_freq_hz    = stopF  * 1e6;
                            hdr.dds_sweep_period_us = period;

                            fwrite(&hdr, sizeof(hdr), 1, fp);
                            fwrite(diagBuffer, sizeof(U16), totalSamples, fp);
                            fclose(fp);
                            SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Single-shot saved OK");
                        }
                    }
                }
            }
        }
    } else {
        char msg[128];
        sprintf(msg, "Single-shot execution error: %d", err);
        SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
    }

    WD_AI_ContBufferReset(adcCard);
    WD_Buffer_Free(adcCard, diagBuffer);
    return 0;
}

int CVICALLBACK AdcStopCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    isAcquiring = 0;
    adcRunning = 0;

    /* Terminate threads gracefully */
    if (producerThreadID != 0) {
        CmtWaitForThreadPoolFunctionCompletion(DEFAULT_THREAD_POOL_HANDLE, producerThreadID, 0);
        CmtReleaseThreadPoolFunctionID(DEFAULT_THREAD_POOL_HANDLE, producerThreadID);
        producerThreadID = 0;
    }
    if (consumerThreadID != 0) {
        CmtWaitForThreadPoolFunctionCompletion(DEFAULT_THREAD_POOL_HANDLE, consumerThreadID, 0);
        CmtReleaseThreadPoolFunctionID(DEFAULT_THREAD_POOL_HANDLE, consumerThreadID);
        consumerThreadID = 0;
    }

    if (dmaQueue != 0) {
        CmtDiscardTSQ(dmaQueue);
        dmaQueue = 0;
    }

    if (adcCard >= 0) {
        U32 d1, d2;
        WD_AI_AsyncClear(adcCard, &d1, &d2);
        WD_AI_AsyncDblBufferMode(adcCard, 0);
        WD_AI_ContBufferReset(adcCard);
    }

    if (recording) {
        ADC_RecordFinish();
        recording = 0;
    }

    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Acquisition stopped cleanly");
    SetCtrlAttribute(adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 0);
    SetCtrlAttribute(adcTabHandle, TABPANEL_2_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);
    
    ADC_PlotFFT_Deferred(NULL);

    return 0;
}

int CVICALLBACK AdcReleaseCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    /* 1. Ensure acquisition, recording, and multithreading are fully halted */
    if (isAcquiring || adcRunning || recording) {
        AdcStopCB(0, 0, EVENT_COMMIT, NULL, 0, 0);
    }

    /* 2. Free the dual kernel-aligned DMA buffers */
    if (dmaBuffer1) { 
        WD_Buffer_Free(adcCard, dmaBuffer1); 
        dmaBuffer1 = NULL; 
    }
    if (dmaBuffer2) { 
        WD_Buffer_Free(adcCard, dmaBuffer2); 
        dmaBuffer2 = NULL; 
    }

    /* 3. Unmap the buffer ID and release the PCIe hardware lock */
    if (adcCard >= 0) {
        WD_AI_ContBufferReset(adcCard);
        WD_Release_Card(adcCard);
        adcCard = -1;
    }
    
    adcRegistered = 0;

    /* 4. Reset User Interface State */
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Card released");
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_REGISTER,  ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RECORD,    ATTR_DIMMED, 1);

    return 0;
}

/* Poll timer: check if async acquisition has completed */
int CVICALLBACK AdcPollTimerCB (int p, int c, int ev, void *cbd, int e1, int e2) { return 0; }

/* One-shot save (still available via Save button) */
int CVICALLBACK AdcSaveCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    char path[MAX_PATHNAME_LEN];
    int  selected;

    if (ev != EVENT_COMMIT) return 0;

    selected = FileSelectPopup ("", "*.rdr", "Radar Data (*.rdr)",
                                "Save Raw Acquisition Data",
                                VAL_SAVE_BUTTON, 0, 0, 1, 1, path);
    if (selected == VAL_NO_FILE_SELECTED) return 0;

    if (ADC_SaveRawFile (path) < 0)
        MessagePopup ("Save", "Failed to save file.");
    else
    {
        char msg[256];
        sprintf (msg, "Saved: %s", path);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
    }

    return 0;
}

int CVICALLBACK AdcRecordCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    if (!recording)
    {
        char path[MAX_PATHNAME_LEN];
        int  selected;

        /* Enforce .bin extension for MATLAB 2025 integration */
        selected = FileSelectPopup ("", "*.bin", "Binary Data (*.bin)",
                                    "Choose Recording File",
                                    VAL_SAVE_BUTTON, 0, 0, 1, 1, path);
        if (selected == VAL_NO_FILE_SELECTED) return 0;

        recordFile = fopen (path, "wb");
        if (recordFile == NULL) return 0;

        strncpy (recordPath, path, sizeof(recordPath) - 1);
        recordedTrigs = 0;

        /* Write the 128-byte RadarFileHeader */
        ADC_RecordWriteHeader ();

        recording = 1;
        SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RECORD, ATTR_LABEL_TEXT, "Stop Record");

        /* Start acquisition automatically if hardware is idle */
        if (!adcRunning) {
            AdcStartCB(0, 0, EVENT_COMMIT, NULL, 0, 0);
        }

        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "Recording...");
    }
    else
    {
        char msg[256];
        recording = 0;
        ADC_RecordFinish ();

        sprintf (msg, "Recording stopped. %lu triggers saved.", (unsigned long)recordedTrigs);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RECORD, ATTR_LABEL_TEXT, "Record");
    }
    return 0;
}
