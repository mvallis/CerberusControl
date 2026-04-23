/*===========================================================================
 * DeviceControl_Full.c
 *
 * CVI/LabWindows application for controlling:
 *   1. KMTronic 2-Channel USB Relay  (serial / VISA)
 *   2. Siglent SPD3303X DC Power Supply (USBTMC / VISA)
 *   3. AD9914 DDS via custom USB interface board (COM port)
 *   4. ADLINK PCI-9846H ADC digitiser (WD-DASK) - double-buffer acquisition
 *
 * Loads GUI from DeviceControl_Full.uir
 *
 * Build:
 *   Add this .c, dds.c, and the .uir to your CVI project.
 *   Link:  visa32.lib/visa64.lib
 *===========================================================================*/

#include <windows.h>
#include <utility.h>
#include <userint.h>
#include <ansi_c.h>
#include <stddef.h>
#include <visa.h>
#include <formatio.h>
#include <analysis.h>

#include "dds.h"
#include "DeviceControl_FullThreaded.h"
#include "Wd-dask64.h"




/*---------------------------------------------------------------------------
 * Constants
 *---------------------------------------------------------------------------*/
#define MAX_RESOURCES       32
#define RESOURCE_STR_LEN    256
#define READ_BUF_LEN        256
#define UIR_FILE            "DeviceControl_FullThreaded.uir"

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
static int masterTabHandle, relayTabHandle, psuTabHandle, ddsTabHandle, adcTabHandle;

/* ---- Relay state ---- */
static int relay1State = 0, relay2State = 0;

/* ---- PSU state ---- */
static int ch1OutState = 0, ch2OutState = 0, ch3OutState = 0;

/* ---- DDS state ---- */
static int    ddsConnected      = 0;
static int    ddsSweepActive    = 0;
static int    ddsInitDone       = 0;       /* 1 after successful init/cal    */
static double lastActualPeriod_us = 0.0;   /* remember last sweep period */


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

/* ADC */
static void ADC_Cleanup (void);
static void ADC_UpdateTrigCountDim (void);
static int  CVICALLBACK TrigModeRingCB (int panel, int control, int event,
                                        void *callbackData, int eventData1,
                                        int eventData2);

/* Master setup (async: register → calibrate → re-register → configure) */
static int  CVICALLBACK MasterSetupThread         (void *data);
static void CVICALLBACK MasterSetupStatusDeferred (void *data);
static void CVICALLBACK MasterSetupConfigDeferred (void *data);
static void CVICALLBACK MasterSetupDoneDeferred   (void *data);
static void CVICALLBACK MasterSetupFailDeferred   (void *data);

/* Master startup sequence */
static int  CVICALLBACK MasterSequenceThread (void *data);
static void CVICALLBACK SeqStep_PSU       (void *data);
static void CVICALLBACK SeqStep_DDS       (void *data);
static void CVICALLBACK SeqStep_ADCSetup  (void *data);
static void CVICALLBACK SeqStep_ADCStart  (void *data);
static void CVICALLBACK SeqStep_Done      (void *data);

/* New sequence steps for redesigned startup */
static void CVICALLBACK SeqStep_ConnectRelayDds (void *data);
static void CVICALLBACK SeqStep_DdsInitCW       (void *data);
static void CVICALLBACK SeqStep_TxRelayOn       (void *data);
static void CVICALLBACK SeqStep_DdsStartChirp   (void *data);

/* Master shutdown sequence */
static int  CVICALLBACK MasterShutdownThread (void *data);
static void CVICALLBACK ShutStep_ADC   (void *data);
static void CVICALLBACK ShutStep_Relay (void *data);
static void CVICALLBACK ShutStep_DDS   (void *data);
static void CVICALLBACK ShutStep_PSU   (void *data);
static void CVICALLBACK ShutStep_Done  (void *data);

/* Master reset-and-start (synchronized DDS/ADC restart for INS sync) */
int CVICALLBACK MasterResetStartCB (int panel, int control, int event,
                                     void *callbackData, int eventData1,
                                     int eventData2);

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

    /* Tab page handles - order: 0=Master, 1=PSU, 2=Relay, 3=DDS, 4=ADC */
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 0, &masterTabHandle);
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 1, &psuTabHandle);
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 2, &relayTabHandle);
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 3, &ddsTabHandle);
    GetPanelHandleFromTabPage (mainPanel, MAIN_PANEL_MAIN_PANEL_TAB, 4, &adcTabHandle);

    /* Initial dimming */
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,       ATTR_DIMMED, 1);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,       ATTR_DIMMED, 1);

    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_TIMER_READBACK, ATTR_ENABLED, 0);
    SetPSUDimmed (1);

    /* Master tab: acquisition buttons dimmed until setup completes */
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_START,  ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_RECORD, ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_STOP,   ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_TIMER_UPDATE,   ATTR_ENABLED, 0);

    /* ADC tab: all buttons dimmed except Register until card is registered */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);

    /* DDS tab: all action buttons dimmed until connected */
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_INIT_CAL,   ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START,      ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,       ATTR_DIMMED, 1);

    /* Trigger mode ring: install callback for interactive dim/undim of count,
       then set initial dim state (POST → count dimmed). */
    InstallCtrlCallback (adcTabHandle, ADC_TAB_ADC_RING_TRIG_MODE,
                         TrigModeRingCB, NULL);
    ADC_UpdateTrigCountDim ();

    ScanVISAResources ();

    DisplayPanel (mainPanel);
    RunUserInterface ();

    /* Cleanup */
    ADC_Cleanup ();
    Relay_Disconnect ();
    PSU_Disconnect ();
    if (ddsConnected) { deinit_dds (); ddsConnected = 0; ddsInitDone = 0; }
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
    ClearListCtrl (ddsTabHandle,   DDS_TAB_DDS_RING_RESOURCE);

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
        else if (strstr(desc, "ASRL19::INSTR") != NULL)
        {
            sprintf(displayStr, "%s (AD9914 DDS Controller)", desc);
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

        /* 3. Insert into all UI rings using the friendly label */
        InsertListItem (relayTabHandle, RELAY_TAB_RELAY_RING_RESOURCE, -1, displayStr, numResources);
        InsertListItem (psuTabHandle,   PSU_TAB_PSU_RING_RESOURCE,   -1, displayStr, numResources);
        InsertListItem (ddsTabHandle,   DDS_TAB_DDS_RING_RESOURCE,  -1, displayStr, numResources);

        numResources++;

    } while (numResources < (int)numFound && numResources < MAX_RESOURCES &&
             viFindNext (findList, desc) == VI_SUCCESS);

    viClose (findList);
}

/*===========================================================================
 *  RELAY DEVICE FUNCTIONS
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
 *  PSU DEVICE FUNCTIONS
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
    if (PSU_Query("MEASure:CURRent? CH1",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_CURR_READ,v);
                                                                        SetCtrlVal(psuTabHandle,PSU_TAB_PSU_CH1_METER,v); }
    if (PSU_Query("MEASure:POWEr? CH1",  r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_PWR_READ,v);  }
    if (PSU_Query("MEASure:VOLTage? CH2",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_VOLT_READ,v); }
    if (PSU_Query("MEASure:CURRent? CH2",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_CURR_READ,v);
                                                                        SetCtrlVal(psuTabHandle,PSU_TAB_PSU_CH2_METER,v); }
    if (PSU_Query("MEASure:POWEr? CH2",  r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_PWR_READ,v);  }
}

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
 *  CALLBACKS  -  PSU TAB
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
    /* Enable master tab update timer when PSU connects */
    SetCtrlAttribute(masterTabHandle,MASTER_TAB_MASTER_TIMER_UPDATE,ATTR_ENABLED,1);
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
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_CH1_METER,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_VOLT_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_CURR_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_PWR_READ,0.0);
    SetCtrlVal(psuTabHandle,PSU_TAB_PSU_CH2_METER,0.0);
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

    allOn = (ch1OutState && ch2OutState && ch3OutState);

    if (allOn)
    {
        ch1OutState = ch2OutState = ch3OutState = 0;
        PSU_OutputOnOff (1, 0);
        PSU_OutputOnOff (2, 0);
        PSU_OutputOnOff (3, 0);
    }
    else
    {
        ch1OutState = ch2OutState = ch3OutState = 1;
        PSU_OutputOnOff (1, 1);
        PSU_OutputOnOff (2, 1);
        PSU_OutputOnOff (3, 1);
    }

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
 *    ADC_CLK  = SYNC_CLK / (hmcDiv x 2) (HMC432 #1 x HMC432 #2 fixed /2)
 *    Trigger  = ADC_CLK / progDiv        (programmable divider)
 *             = SYNC_CLK / (hmcDiv x 2 x progDiv)
 *
 *  ADC sampling rate = ADC_CLK / scanInterval   (ContScan ScanInterval)
 *
 *  Chirp duration  = sampsPerChirp x scanInterval x hmcDiv x 2   SYNC_CLK cycles
 *  DRCTRL period   = hmcDiv x 2 x progDiv                       SYNC_CLK cycles
 *  Dead time       = DRCTRL period - actual sweep period
 *  Dead samples    = progDiv / scanInterval - sampsPerChirp      ADC samples
 *  Min progDiv     = sampsPerChirp x scanInterval + 1
 *===========================================================================*/
static void DDS_UpdateTimingDisplay (void)
{
    double clockMHz, syncClkMHz, syncClkPeriod_us;
    double adcClkMHz, adcSampRateMHz, trigFreqHz;
    double drctrlPeriod_us, deadTime_us, calcPeriod_us;
    int    hmcDiv, progDiv, sampsPerChirp, scanInterval;
    int    adcClkDivTotal, chirpSteps, drctrlSteps, minProgDiv, deadSamples;
    char   warnMsg[256];

    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CLOCK_MHZ,     &clockMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_HMC_DIV,       &hmcDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_PROG_DIV,      &progDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SAMPS_PER_CHI, &sampsPerChirp);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_SCAN_INTERVAL, &scanInterval);

    if (hmcDiv < 2)          hmcDiv = 2;
    if (progDiv < 1)         progDiv = 1;
    if (sampsPerChirp < 1)   sampsPerChirp = 1;
    if (scanInterval < 1)    scanInterval = 1;

    /* SYNC_CLK = DDS_CLOCK / 24 */
    syncClkMHz       = clockMHz / 24.0;
    syncClkPeriod_us = (syncClkMHz > 0.0) ? 1.0 / syncClkMHz : 0.0;

    /* ADC_CLK = SYNC_CLK / (hmcDiv x 2)  [HMC432 #1 + fixed /2] */
    adcClkDivTotal = hmcDiv * 2;
    adcClkMHz      = syncClkMHz / (double)adcClkDivTotal;

    /* ADC sampling rate = ADC_CLK / scanInterval */
    adcSampRateMHz = adcClkMHz / (double)scanInterval;

    /* Chirp: sampsPerChirp ADC samples, each taking scanInterval ADC_CLK cycles
       = sampsPerChirp x scanInterval x adcClkDivTotal SYNC_CLK cycles */
    chirpSteps    = sampsPerChirp * scanInterval * adcClkDivTotal;
    calcPeriod_us = (double)chirpSteps * syncClkPeriod_us;

    /* DRCTRL trigger = ADC_CLK / progDiv */
    drctrlSteps     = adcClkDivTotal * progDiv;
    trigFreqHz      = (adcClkMHz > 0.0) ? adcClkMHz * 1e6 / (double)progDiv : 0.0;
    drctrlPeriod_us = (double)drctrlSteps * syncClkPeriod_us;

    /* Dead time */
    deadTime_us = drctrlPeriod_us - lastActualPeriod_us;
    deadSamples = progDiv / scanInterval - sampsPerChirp;
    minProgDiv  = sampsPerChirp * scanInterval + 1;

    /* Update indicators */
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SYNC_CLK,       syncClkMHz);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ADC_CLK,        adcClkMHz);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TRIG_FREQ,      trigFreqHz);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DRCTRL_PERIOD,  drctrlPeriod_us);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CHIRP_STEPS,    chirpSteps);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CALC_PERIOD,    calcPeriod_us);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_TIME,      deadTime_us);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_SAMPLES,   deadSamples);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_MIN_PROG_DIV,   minProgDiv);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_FIXED_DIV2,     2);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SCAN_INTERVAL,  scanInterval);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ADC_SAMP_RATE,  adcSampRateMHz);

    /* TX sweep bandwidth and centre frequency from DDS actual values.
       Tx_GHz = (((DDS_MHz / 1000 + 7) * 3) - 14)                     */
    {
        double actStart, actStop, txStartGHz, txStopGHz, txBW, txCentre;
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_START, &actStart);
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_STOP,  &actStop);
        txStartGHz = (((actStart / 1000.0 + 7.0) * 3.0) - 14.0);
        txStopGHz  = (((actStop  / 1000.0 + 7.0) * 3.0) - 14.0);
        txBW       = fabs (txStopGHz - txStartGHz);
        txCentre   = (txStartGHz + txStopGHz) / 2.0;
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_BW,     txBW);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_CENTRE, txCentre);
    }

    /* Validation warnings */
    warnMsg[0] = '\0';

    if (progDiv <= sampsPerChirp * scanInterval)
    {
        sprintf (warnMsg, "WARNING: Prog divider %d too small! "
                 "Need > %d (samples x scanInterval). Min = %d.",
                 progDiv, sampsPerChirp * scanInterval, minProgDiv);
    }
    else if (deadTime_us < 0.0 && lastActualPeriod_us > 0.0)
    {
        sprintf (warnMsg, "WARNING: Actual sweep (%.3f us) exceeds "
                 "DRCTRL period (%.3f us). Increase prog divider.",
                 lastActualPeriod_us, drctrlPeriod_us);
    }

    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_TIMING_WARN, warnMsg);
}

/*===========================================================================
 *  CALLBACKS  -  DDS TAB
 *===========================================================================*/
int CVICALLBACK DdsConnectCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int    resIdx;
    char   visaStr[RESOURCE_STR_LEN];
    char   fullPort[80];
    double clockMHz;
    int    comNum;
    char  *p2;

    if (ev != EVENT_COMMIT) return 0;

    /* Read the selected VISA resource from the ring (e.g. "ASRL19::INSTR") */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_RESOURCE, &resIdx);
    if (resIdx < 0 || resIdx >= numResources)
    {
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Status: No resource selected");
        return 0;
    }
    strncpy (visaStr, resourceList[resIdx], sizeof(visaStr) - 1);
    visaStr[sizeof(visaStr) - 1] = '\0';

    /* DDS uses Win32 serial directly — parse "ASRLxx::INSTR" → "\\.\COMxx" */
    if (strncmp (visaStr, "ASRL", 4) != 0)
    {
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS,
                    "Status: DDS requires a serial (ASRL) resource");
        return 0;
    }
    comNum = (int)strtol (visaStr + 4, &p2, 10);
    if (p2 == visaStr + 4)
    {
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS,
                    "Status: Could not parse COM number from VISA string");
        return 0;
    }
    sprintf (fullPort, "\\\\.\\COM%d", comNum);

    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CLOCK_MHZ, &clockMHz);
    dds_set_com_port (fullPort);
    dds_set_clock (clockMHz * 1e6);

    if (!init_dds ())
    {
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Status: Connection FAILED");
        return 0;
    }

    ddsConnected = 1;
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Status: Connected");
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_CONNECT,    ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_DISCONNECT, ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_INIT_CAL,   ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START,      ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,       ATTR_DIMMED, 1);

    return 0;
}

int CVICALLBACK DdsDisconnectCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    //if (ddsSweepActive) { dds_powerdown (); ddsSweepActive = 0; }, 
	dds_powerdown(); // making sure off
    deinit_dds (); //ensure turned off then quit
    ddsConnected = 0;
    ddsInitDone  = 0;

    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Status: Disconnected");
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_CONNECT,    ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_INIT_CAL,   ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START,      ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,       ATTR_DIMMED, 1);

    return 0;
}

int CVICALLBACK DdsInitCalCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Resetting & calibrating...");
    ProcessSystemEvents ();   /* update UI before blocking call */
	
	dds_powerdown(); // making sure off
    dds_reset(); // safe reset
	if (!dds_powerup ()) { return 0; } // switch on
    if (!dds_reset ()) //double safe reset
    { SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Reset FAILED"); return 0; }

    if (!ad9914_calibrate_dac ()) // calibrate
    { SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "DAC cal FAILED"); return 0; }

    ddsInitDone = 1;
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Init & calibration OK");
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

    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, &chirpMode);

    /* chirpMode: 0 = Triggered (DRCTRL), 1 = Free-run, 2 = CW */
    if (chirpMode == 2)
    {
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CW_FREQ, &cwFreq);
	// Temporarily removing, keep for now
        //if (!dds_reset ())   { return 0; }
        //if (!dds_powerup ()) { return 0; }
        //Delay(0.01);
        //if (!ad9914_calibrate_dac ()) { return 0; }
        if (!ad9914_single_tone (cwFreq, &actCW)) { return 0; }
        if (!dds_update ()) { return 0; }

        ddsSweepActive = 1;
        lastActualPeriod_us = 0.0;

        sprintf (msg, "CW output: %.6f MHz (requested %.6f MHz)", actCW, cwFreq);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, msg);

        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_START,  actCW);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_STOP,   0.0);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_PERIOD, 0.0);

        /* CW mode: show TX frequency, BW = 0 */
        {
            double txCW = (((actCW / 1000.0 + 7.0) * 3.0) - 14.0);
            SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_BW,     0.0);
            SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_CENTRE, txCW);
        }
    }
    else
    {
        /* ---- Sweep modes (Triggered or Free-run) ---- */
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_START_FREQ, &startF);
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_STOP_FREQ,  &stopF);
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_PERIOD,     &period);

        if (startF == stopF)
        {
            SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS,
                        "ERROR: Start/Stop frequencies cannot match in Sweep Mode");
            return 0;
        }

        //if (!dds_reset ())   { return 0; } // Don't need all these resets and re-calibration.
        //if (!dds_powerup ()) { return 0; }
        //Delay(0.01);
        //if (!ad9914_calibrate_dac ()) { return 0; }

        if (chirpMode == 0)
        {
            rampFlags = DRG_NO_DWELL_HIGH;
            if (!dds_drctrl (0))
            {
                SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "DRCTRL config FAILED");
                return 0;
            }
        }
        else
        {
            rampFlags = DRG_NO_DWELL;
        }

        if (!ad9914_ramp_generator (startF, stopF, period, rampFlags,
                                    &actStart, &actStop, &actPeriod))
        {
            SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Ramp config FAILED");
            return 0;
        }

        if (!dds_update ()) { return 0; }

        ddsSweepActive = 1;

        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_START,  actStart);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_STOP,   actStop);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_PERIOD, actPeriod);

        lastActualPeriod_us = actPeriod;
        DDS_UpdateTimingDisplay ();

        {
            double txS = (((actStart / 1000.0 + 7.0) * 3.0) - 14.0);
            double txE = (((actStop  / 1000.0 + 7.0) * 3.0) - 14.0);
            sprintf (msg, "%s sweep  (%.6f-%.6f MHz, %.3f us) TX BW=%.3f GHz Fc=%.3f GHz",
                     (chirpMode == 0) ? "Triggered" : "Free-run",
                     actStart, actStop, actPeriod,
                     fabs (txE - txS), (txS + txE) / 2.0);
        }
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, msg);
    }

    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START, ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,  ATTR_DIMMED, 0);

    return 0;
}

int CVICALLBACK DdsStopCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int    chirpMode;
    double cwFreq, actCW;
    char   msg[256];

    if (ev != EVENT_COMMIT) return 0;

    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, &chirpMode);

    /* If running an FMCW waveform (triggered or free-run), switch to CW
       to cleanly stop the DROver trigger signal without powering down */
    if (chirpMode == 0 || chirpMode == 1)
    {
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CW_FREQ, &cwFreq);
        if (ad9914_single_tone (cwFreq, &actCW) && dds_update ())
        {
            sprintf (msg, "Stopped sweep -> CW %.6f MHz", actCW);
            SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, msg);
        }
        else
        {
            SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS,
                        "Sweep stopped (CW switch failed)");
        }
    }
    else
    {
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Sweep stopped");
    }

    ddsSweepActive = 0;
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START, ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,  ATTR_DIMMED, 1);

    return 0;
}

/* Called when division ratio or clock changes - update timing display live */
int CVICALLBACK DdsDivRatioCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT && ev != EVENT_VAL_CHANGED) return 0;
    DDS_UpdateTimingDisplay ();
    return 0;
}

/*===========================================================================
 *  ADC TAB — PCI-9846H double-buffer acquisition (WD-DASK)
 *===========================================================================*/
/*---------------------------------------------------------------------------
 * Constants
 *---------------------------------------------------------------------------*/
#define ADC_NUM_CH          2U
#define HALF_BUF_SAMPLES    1048576U    /* 2^20 U16 samples per half-buffer (2 MB)   */
#define SCANS_PER_HALF      524288U     /* HALF_BUF_SAMPLES / ADC_NUM_CH             */
#define SAVE_QUEUE_CAP      32          /* 32 × 2 MB = 64 MB queue headroom          */
#define TSQ_WRITE_MS        50          /* ms to wait before counting a save drop     */
#define PLOT_SCANS_MAX      131072U     /* max scans copied to plot buffer (one chirp)*/
#define RETRIG_CNT_INF      1U          /* reTrgCnt=1 confirmed to give continuous acquisition
                                           with external digital trigger in double-buffer mode.
                                           Larger values (e.g. 65535) are NOT required here —
                                           the parameter behaves differently in WD_AI_Trig_Config
                                           vs WD_AI_ContReadMultiChannels (where reTrgCnt=1 stopped
                                           acquisition after one event). Tested: single-shot button
                                           with reTrgCnt=1 produced continuous data, confirming
                                           this value is correct for all acquisition modes.       */
#define RETRIG_CNT_ONE      1U          /* same value — kept separate for code clarity           */

#define SAVE_NONE    0
#define SAVE_THREAD  1                  /* user-space TSQ → fwrite thread            */
#define SAVE_TOFILE  2                  /* WD_AI_AsyncDblBufferToFile (driver-managed)
// Define the Trig Digital threshold level here!?

/* Radar physical constants for FFT axis scaling */
#define SPEED_OF_LIGHT      299792458.0 /* m/s                                       */
#define FREQ_MULTIPLIER     3.0         /* RF frequency multiplier (×3 tripler)      */

/* Range ring index → WD-DASK constant for PCI-9846H.
   The hardware supports exactly two bipolar ranges:
     Ring index 0  →  AD_B_5_V  (±5 V)
     Ring index 1  →  AD_B_1_V  (±1 V)
   Set the UIR ring items in this order; their item values can stay at defaults (0, 1). */
static const U16 adcRangeTable[] = {
    AD_B_5_V,   /* ring index 0 — ±5 V */
    AD_B_1_V,   /* ring index 1 — ±1 V */
};
#define ADC_RANGE_TABLE_LEN  ((int)(sizeof(adcRangeTable) / sizeof(adcRangeTable[0])))

/*---------------------------------------------------------------------------
 * ADC Globals
 *---------------------------------------------------------------------------*/
static I16   adcCard       = -1;
static int   adcRegistered = 0;
static int   adcConfigured = 0;
static int   adcConfiguredRange     = 0;   /* cached at Configure time for sidecar */
static int   adcConfiguredImpedance = 0;
static int   adcConfiguredTimebase  = 0;
static HGLOBAL hDmaBuffer1  = NULL;   /* GlobalAlloc handles — needed for GlobalUnfix */
static HGLOBAL hDmaBuffer2  = NULL;
static U16  *dmaBuffer1    = NULL;
static U16  *dmaBuffer2    = NULL;
static U16   firstBufId    = 0;
static U16   secondBufId   = 0;

// Set up trigger alignment from here... make sure to prototype function etc.!

static volatile int    isAcquiring = 0;
static int             saveMode    = SAVE_NONE;

static CmtThreadFunctionID pollThreadID = 0;
static CmtThreadFunctionID saveThreadID = 0;
static CmtTSQHandle        saveQueue    = 0;

static FILE *recordFile    = NULL;
static char  recordPath[512];

/* Plot state — written by poll thread, consumed by deferred UI callback */
static U16          plotBuffer[PLOT_SCANS_MAX * ADC_NUM_CH];
static unsigned int plotScans    = 0;
static double       adcVoltScale = 1.0;
static volatile int plotBusy     = 0;

/* FFT processing — pre-computed table cache + dynamic work buffers
   (all allocated/freed on the UI thread; never touched from poll thread)  */
static PFFTTable        fftTableCached  = NULL;   /* CreateFFTTable result   */
static U32              fftTableN       = 0;      /* size table was built for */
static double          *fftPaddedWork   = NULL;   /* windowed + zero-padded input */
static NIComplexNumber *fftOut0         = NULL;   /* FFT result CH0 (LRX)    */
static NIComplexNumber *fftOut1         = NULL;   /* FFT result CH1 (URX)    */
static U32              fftBufSize      = 0;      /* current allocation (elements) */

/* Diagnostic counters (written by poll thread, read by timer on UI thread) */
static volatile U32 diagPollCount   = 0;
static volatile U32 diagHalfReady   = 0;
static volatile U32 diagFStopCount  = 0;
static volatile U32 diagSaveWritten = 0;
static volatile U32 diagSaveDropped = 0;
static volatile U32 diagPlotPosted  = 0;
static volatile U32 diagPlotDone    = 0;
static volatile U32 diagDmaOverrun  = 0;

/*---------------------------------------------------------------------------
 * Forward declarations (ADC-internal)
 *---------------------------------------------------------------------------*/
static int  CVICALLBACK HardwarePollThread  (void *data);
static int  CVICALLBACK DiskSaveThread      (void *data);
static void             ADC_ApplyWindow     (double *data, U32 n, int winType);
static PFFTTable        ADC_GetFFTTable     (U32 n);
static int              ADC_EnsureFFTBufs   (U32 n);
static void CVICALLBACK ADC_PlotDeferred    (void *data);
static void CVICALLBACK ADC_StopDeferred    (void *data);
static int  CVICALLBACK AdcCalThread        (void *data);
static void CVICALLBACK ADC_CalDoneDeferred (void *data);
static double           ADC_RangeToVolts    (int rangeConst);
static void             ADC_GenerateFilename(char *buf, int bufLen);
static void             ADC_WriteSidecarHeader (const char *baseName, int mode,
                                                const char *context);
static void             ADC_StopAcquisition (void);
static int              ADC_StartCommon     (int mode, U32 reTrgCnt);

/* Dim or undim the trigger-count control based on the selected trigger mode.
   Ring: 0 = Post (count unused → dimmed), 1 = Delay (count = delay ticks → active).
   Pre and Middle modes removed. */
static void ADC_UpdateTrigCountDim (void)
{
    int trigModeIdx = 0;
    int dimmed;

    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TRIG_MODE, &trigModeIdx);
    dimmed = (trigModeIdx == 0) ? 1 : 0;   /* 0 = POST → dim; 1 = DELAY → active */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_NUM_TRIG_COUNT, ATTR_DIMMED, dimmed);
}

/* Callback installed on the trigger-mode ring so the count control
   dims/undims interactively when the user changes the mode. */
static int CVICALLBACK TrigModeRingCB (int panel, int control, int event,
                                       void *callbackData, int eventData1,
                                       int eventData2)
{
    if (event == EVENT_COMMIT)
        ADC_UpdateTrigCountDim ();
    return 0;
}

/* Unpin and release one DMA buffer allocated with GlobalAlloc+GlobalFix.
   Must be called before any free/release so the driver can unlock the pages. */
static void ADC_FreeBuffer (HGLOBAL *phMem, U16 **ppBuf)
{
    if (*phMem)
    {
        GlobalUnfix  (*phMem);
        GlobalUnlock (*phMem);
        GlobalFree   (*phMem);
        *phMem = NULL;
        *ppBuf = NULL;
    }
}

/* Allocate one DMA half-buffer using GlobalAlloc+GlobalFix (matches P98x6 reference).
   Returns 0 on success, -1 on failure. */
static int ADC_AllocBuffer (HGLOBAL *phMem, U16 **ppBuf)
{
    HGLOBAL h = GlobalAlloc (GMEM_FIXED | GMEM_ZEROINIT,
                             HALF_BUF_SAMPLES * sizeof (U16));
    if (!h) return -1;
    *ppBuf = (U16 *)GlobalLock (h);
    if (!*ppBuf) { GlobalFree (h); return -1; }
    GlobalFix (h);
    *phMem = h;
    return 0;
}

/*---------------------------------------------------------------------------
 * ADC_RangeToVolts — maps WD-DASK range constant to full-scale volts
 *---------------------------------------------------------------------------*/
static double ADC_RangeToVolts (int rangeConst)
{
    switch (rangeConst)
    {
        case AD_B_10_V:    return 10.0;
        case AD_B_5_V:     return  5.0;
        case AD_B_2_5_V:   return  2.5;
        case AD_B_1_25_V:  return  1.25;
        case AD_B_1_V:     return  1.0;
        case AD_B_0_625_V: return  0.625;
        case AD_B_0_5_V:   return  0.5;
        default:           return  1.0;
    }
}

/*---------------------------------------------------------------------------
 * ADC_GenerateFilename — CerberusData_YYYYMMDD_HHMMSS  (no extension)
 *
 *   The caller appends ".dat" when opening the file directly (SAVE_THREAD).
 *   For SAVE_TOFILE the WD-DASK driver appends ".dat" automatically, so the
 *   base name must NOT include the extension.
 *   The sidecar header is always written to "<base>.hdr".
 *---------------------------------------------------------------------------*/
static void ADC_GenerateFilename (char *buf, int bufLen)
{
    int month, day, year, hours, minutes, seconds;

    /* Ensure output directory exists (ignore -9 "already exists") */
    SetBreakOnLibraryErrors (0);
    MakeDir ("Radar Data");
    SetBreakOnLibraryErrors (1);

    GetSystemDate (&month, &day, &year);
    GetSystemTime (&hours, &minutes, &seconds);
    snprintf (buf, bufLen, "Radar Data\\CerberusData_%04d%02d%02d_%02d%02d%02d",
              year, month, day, hours, minutes, seconds);
}

/*---------------------------------------------------------------------------
 * ADC_WriteSidecarHeader — write a .hdr text file alongside the .dat file
 *
 *   Comprehensive capture of the entire radar system state at the moment
 *   recording starts: data format, ADC configuration, trigger setup, DDS
 *   waveform, timing/divider chain, TX RF parameters, radar performance
 *   metrics, FFT display settings, PSU readback, and system-level state.
 *
 *   baseName — path without extension (.hdr appended here)
 *   mode     — SAVE_THREAD or SAVE_TOFILE
 *   context  — descriptive string: "record-thread", "record-tofile",
 *              "master-record", "synced-reset"
 *---------------------------------------------------------------------------*/
static void ADC_WriteSidecarHeader (const char *baseName, int mode,
                                    const char *context)
{
    char   hdrPath[512];
    FILE  *hdr;
    int    month, day, year, hours, minutes, seconds;

    /* ADC UI values */
    int    timebase, impedance, range, sampsPerTrig, scanInterval;
    double voltRange, adcSampRateMHz;

    /* DDS UI values */
    double clockMHz, startF, stopF, period, cwFreq;
    int    hmcDiv, progDiv, sampsPerChirp, chirpMode;

    /* DDS actual / computed */
    double actStart, actStop, actPeriod;
    double syncClkMHz, adcClkMHz, trigFreqHz, drctrlPeriod;
    int    deadSamples, chirpSteps, minProgDiv;
    double deadTime, calcPeriod;

    /* TX radar */
    double txBW, txCentre, txStartGHz, txStopGHz;

    /* FFT display */
    int    windowType, padFactor;

    /* PSU readback */
    double v1, i1, v2, i2;

    /* Run note (256-char free-form text from master tab) */
    char   runNote[257];

    snprintf (hdrPath, sizeof (hdrPath), "%s.hdr", baseName);
    hdr = fopen (hdrPath, "w");
    if (!hdr) return;

    /* ================================================================
     *  [Timestamp]
     * ================================================================ */
    GetSystemDate (&month, &day, &year);
    GetSystemTime (&hours, &minutes, &seconds);
    fprintf (hdr, "[Timestamp]\n");
    fprintf (hdr, "Date              = %04d-%02d-%02d\n", year, month, day);
    fprintf (hdr, "Time              = %02d:%02d:%02d\n\n", hours, minutes, seconds);

    /* ================================================================
     *  [Recording]  — how this file was created
     * ================================================================ */
    fprintf (hdr, "[Recording]\n");
    fprintf (hdr, "SaveMode          = %s\n",
             (mode == SAVE_THREAD) ? "SAVE_THREAD" : "SAVE_TOFILE");
    fprintf (hdr, "Context           = %s\n\n", context);

    /* ================================================================
     *  [Data_Format]  — everything needed to parse the binary .dat
     * ================================================================ */
    /* Use cached values from AdcConfigureCB — these reflect what was actually
       programmed into hardware, not the current GUI ring state (which may have
       been changed between Configure and Record). */
    range     = adcConfiguredRange;
    impedance = adcConfiguredImpedance;
    timebase  = adcConfiguredTimebase;
    if (range < 0 || range >= ADC_RANGE_TABLE_LEN) range = 0;
    voltRange = ADC_RangeToVolts (adcRangeTable[range]);

    fprintf (hdr, "[Data_Format]\n");
    fprintf (hdr, "NumChannels       = %u\n", ADC_NUM_CH);
    fprintf (hdr, "Interleave        = CH0,CH1,CH0,CH1,...\n");
    fprintf (hdr, "DataType          = U16\n");
    fprintf (hdr, "ByteOrder         = Little-Endian\n");
    fprintf (hdr, "MidCodeOffset     = 32768\n");
    fprintf (hdr, "VoltageRange_V    = %.3f\n", voltRange);
    fprintf (hdr, "VoltsPerCount     = %.10e\n", voltRange / 32768.0);
    fprintf (hdr, "HalfBufSamples    = %u\n", HALF_BUF_SAMPLES);
    fprintf (hdr, "ScansPerHalf      = %u\n\n", SCANS_PER_HALF);

    /* ================================================================
     *  [ADC]  — digitiser configuration (timebase, impedance, range from cache above)
     * ================================================================ */
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_SAMP_PER_TRIG, &sampsPerTrig);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_SCAN_INTERVAL, &scanInterval);
    if (scanInterval < 1) scanInterval = 1;

    fprintf (hdr, "[ADC]\n");
    fprintf (hdr, "Card              = PCI-9846H\n");
    fprintf (hdr, "Timebase          = %s\n",
             (timebase == 0) ? "External" : "Internal");
    fprintf (hdr, "Impedance         = %s\n",
             impedance ? "1 Mohm" : "50 ohm");
    fprintf (hdr, "SampsPerChirp_ADC = %d\n", sampsPerTrig);
    fprintf (hdr, "ScanInterval      = %d\n\n", scanInterval);

    /* ================================================================
     *  [ADC_Trigger]  — trigger configuration
     * ================================================================ */
    {
        int   trigModeIdx, trigSrcI, trigPolI, trigCnt;
        float trigLvl;

        GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TRIG_SRC, &trigSrcI);
        GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TRIG_POL, &trigPolI);

        if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TRIG_MODE, &trigModeIdx) != 0)
            trigModeIdx = 0;
        if (GetCtrlVal (adcTabHandle, ADC_TAB_NUM_TRIG_LEVEL, &trigLvl) != 0)
            trigLvl = 2.4;
        if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_TRIG_COUNT, &trigCnt) != 0)
            trigCnt = 0;

        fprintf (hdr, "[ADC_Trigger]\n");
        fprintf (hdr, "TriggerSource     = %s\n",
                 (trigSrcI == 0) ? "External Digital" : "Software");
        fprintf (hdr, "TriggerPolarity   = %s\n",
                 (trigPolI == 1) ? "Negative Edge" : "Positive Edge");
        fprintf (hdr, "TriggerMode       = %s\n",
                 (trigModeIdx == 1) ? "Delay" : "Post");
        fprintf (hdr, "TriggerThresh_V   = %.2f\n", trigLvl);
        fprintf (hdr, "TriggerCount      = %d\n\n", trigCnt);
    }

    /* ================================================================
     *  [DDS_Settings]  — requested waveform parameters
     * ================================================================ */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, &chirpMode);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CLOCK_MHZ,   &clockMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_START_FREQ,  &startF);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_STOP_FREQ,   &stopF);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_PERIOD,      &period);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CW_FREQ,     &cwFreq);

    fprintf (hdr, "[DDS_Settings]\n");
    fprintf (hdr, "ChirpMode         = %s\n",
             (chirpMode == 0) ? "Triggered (DRCTRL)" :
             (chirpMode == 1) ? "Free-run"           : "CW");
    fprintf (hdr, "ClockMHz          = %.6f\n", clockMHz);
    fprintf (hdr, "StartFreqMHz      = %.6f\n", startF);
    fprintf (hdr, "StopFreqMHz       = %.6f\n", stopF);
    fprintf (hdr, "RequestedPeriod_us= %.6f\n", period);
    fprintf (hdr, "CW_FreqMHz        = %.6f\n\n", cwFreq);

    /* ================================================================
     *  [DDS_Actual]  — what the hardware actually produced
     * ================================================================ */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_START,  &actStart);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_STOP,   &actStop);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_PERIOD, &actPeriod);

    fprintf (hdr, "[DDS_Actual]\n");
    fprintf (hdr, "ActualStartMHz    = %.6f\n", actStart);
    fprintf (hdr, "ActualStopMHz     = %.6f\n", actStop);
    fprintf (hdr, "ActualPeriod_us   = %.6f\n\n", actPeriod);

    /* ================================================================
     *  [TX_Radar]  — transmit-side RF parameters after multiplier chain
     *   Tx_GHz = (((DDS_MHz / 1000 + 7) * 3) - 14)
     * ================================================================ */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_BW,     &txBW);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_CENTRE, &txCentre);
    txStartGHz = (((actStart / 1000.0 + 7.0) * 3.0) - 14.0);
    txStopGHz  = (((actStop  / 1000.0 + 7.0) * 3.0) - 14.0);

    fprintf (hdr, "[TX_Radar]\n");
    fprintf (hdr, "FreqMultiplier    = 3\n");
    fprintf (hdr, "TX_StartFreq_GHz  = %.6f\n", txStartGHz);
    fprintf (hdr, "TX_StopFreq_GHz   = %.6f\n", txStopGHz);
    fprintf (hdr, "TX_Bandwidth_GHz  = %.6f\n", txBW);
    fprintf (hdr, "TX_CentreFreq_GHz = %.6f\n\n", txCentre);

    /* ================================================================
     *  [Timing]  — divider chain and sweep timing
     * ================================================================ */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_HMC_DIV,        &hmcDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_PROG_DIV,       &progDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SAMPS_PER_CHI,  &sampsPerChirp);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SYNC_CLK,       &syncClkMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ADC_CLK,        &adcClkMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TRIG_FREQ,      &trigFreqHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DRCTRL_PERIOD,  &drctrlPeriod);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_TIME,      &deadTime);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_SAMPLES,   &deadSamples);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CHIRP_STEPS,    &chirpSteps);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CALC_PERIOD,    &calcPeriod);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_MIN_PROG_DIV,   &minProgDiv);

    adcSampRateMHz = (scanInterval > 0) ? adcClkMHz / (double)scanInterval : adcClkMHz;

    fprintf (hdr, "[Timing]\n");
    fprintf (hdr, "HMC432_Divider    = %d\n", hmcDiv);
    fprintf (hdr, "FixedDiv2         = 2\n");
    fprintf (hdr, "ProgDivider       = %d\n", progDiv);
    fprintf (hdr, "ScanInterval      = %d\n", scanInterval);
    fprintf (hdr, "SampsPerChirp     = %d\n", sampsPerChirp);
    fprintf (hdr, "SyncClkMHz        = %.6f\n", syncClkMHz);
    fprintf (hdr, "ADC_ClkMHz        = %.6f\n", adcClkMHz);
    fprintf (hdr, "ADC_SampRateMHz   = %.6f\n", adcSampRateMHz);
    fprintf (hdr, "PRF_Hz            = %.3f\n", trigFreqHz);
    fprintf (hdr, "DRCTRL_Period_us  = %.6f\n", drctrlPeriod);
    fprintf (hdr, "DeadTime_us       = %.6f\n", deadTime);
    fprintf (hdr, "DeadSamples       = %d\n", deadSamples);
    fprintf (hdr, "ChirpSteps        = %d\n", chirpSteps);
    fprintf (hdr, "CalcChirpPd_us    = %.6f\n", calcPeriod);
    fprintf (hdr, "MinProgDivider    = %d\n\n", minProgDiv);

    /* ================================================================
     *  [Radar_Performance]  — derived metrics for post-processing
     * ================================================================ */
    {
        double txBW_Hz       = txBW * 1e9;
        double txCentre_Hz   = txCentre * 1e9;
        double rangeRes      = (txBW_Hz > 0.0)
                               ? SPEED_OF_LIGHT / (2.0 * txBW_Hz) : 0.0;
        double maxRange      = (txBW_Hz > 0.0)
                               ? SPEED_OF_LIGHT * (double)sampsPerChirp
                                 / (2.0 * txBW_Hz * 2.0) : 0.0;
        double maxVelocity   = (txCentre_Hz > 0.0 && trigFreqHz > 0.0)
                               ? SPEED_OF_LIGHT * trigFreqHz
                                 / (4.0 * txCentre_Hz) : 0.0;

        fprintf (hdr, "[Radar_Performance]\n");
        fprintf (hdr, "RangeResolution_m = %.4f\n", rangeRes);
        fprintf (hdr, "MaxRange_m        = %.2f\n", maxRange);
        fprintf (hdr, "MaxVelocity_mps   = %.4f\n\n", maxVelocity);
    }

    /* ================================================================
     *  [FFT_Processing]  — live display settings at time of recording
     * ================================================================ */
    if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_WINDOW,  &windowType) != 0)
        windowType = 1;
    if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_ZEROPAD, &padFactor) != 0)
        padFactor  = 0;

    fprintf (hdr, "[FFT_Processing]\n");
    fprintf (hdr, "WindowType        = %s\n",
             (windowType == 0) ? "Rectangular" :
             (windowType == 1) ? "Hann"        :
             (windowType == 2) ? "Hamming"      :
             (windowType == 3) ? "Blackman-Harris" :
             (windowType == 5) ? "Blackman"     :
             (windowType == 6) ? "Flat Top"     : "Unknown");
    fprintf (hdr, "ZeroPadFactor     = %d\n\n", padFactor);

    /* ================================================================
     *  [System_State]  — snapshot of hardware state at recording start
     * ================================================================ */
    fprintf (hdr, "[System_State]\n");
    fprintf (hdr, "DDS_Connected     = %s\n", ddsConnected  ? "Yes" : "No");
    fprintf (hdr, "DDS_InitDone      = %s\n", ddsInitDone   ? "Yes" : "No");
    fprintf (hdr, "DDS_SweepActive   = %s\n", ddsSweepActive ? "Yes" : "No");
    fprintf (hdr, "TX_Relay          = %s\n", relay1State   ? "On"  : "Off");
    fprintf (hdr, "Relay2            = %s\n\n", relay2State   ? "On"  : "Off");

    /* ================================================================
     *  [PSU_Readback]  — latest measured voltages and currents
     * ================================================================ */
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH1_VOLT_READ, &v1);
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH1_CURR_READ, &i1);
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH2_VOLT_READ, &v2);
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH2_CURR_READ, &i2);

    fprintf (hdr, "[PSU_Readback]\n");
    fprintf (hdr, "CH1_Voltage_V     = %.3f\n", v1);
    fprintf (hdr, "CH1_Current_A     = %.3f\n", i1);
    fprintf (hdr, "CH2_Voltage_V     = %.3f\n", v2);
    fprintf (hdr, "CH2_Current_A     = %.3f\n\n", i2);

    /* ================================================================
     *  [Run_Note]  — free-form user annotation (max 256 chars)
     * ================================================================ */
    runNote[0] = '\0';
    GetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_TEXT_SAVE_NOTE, runNote);
    runNote[256] = '\0';   /* hard guard against overrun */
    fprintf (hdr, "[Run_Note]\n");
    fprintf (hdr, "Note              = %s\n", runNote);

    fclose (hdr);
}

/*---------------------------------------------------------------------------
 * ADC_ApplyWindow — in-place window function dispatch
 *   windowType values match CVI analysis.h GetWinProperties constants:
 *     0 = Rectangular (none), 1 = Hann, 2 = Hamming, 3 = Blackman-Harris,
 *     5 = Blackman,           6 = Flat Top
 *---------------------------------------------------------------------------*/
static void ADC_ApplyWindow (double *data, U32 n, int windowType)
{
    switch (windowType)
    {
        case 1: HanWin      (data, (ssize_t)n); break;
        case 2: HamWin      (data, (ssize_t)n); break;
        case 3: BlkHarrisWin(data, (ssize_t)n); break;
        case 5: BkmanWin    (data, (ssize_t)n); break;
        case 6: FlatTopWin  (data, (ssize_t)n); break;
        /* case 0: Rectangular — no windowing applied */
        default: break;
    }
}

/*---------------------------------------------------------------------------
 * ADC_GetFFTTable — return a cached PFFTTable of size n.
 *   Destroys and recreates the table only when n changes.
 *---------------------------------------------------------------------------*/
static PFFTTable ADC_GetFFTTable (U32 n)
{
    if (fftTableCached && fftTableN == n)
        return fftTableCached;

    if (fftTableCached)
    {
        DestroyFFTTable (fftTableCached);
        fftTableCached = NULL;
        fftTableN      = 0;
    }

    fftTableCached = CreateFFTTable ((ssize_t)n);
    if (fftTableCached)
        fftTableN = n;

    return fftTableCached;
}

/*---------------------------------------------------------------------------
 * ADC_EnsureFFTBufs — (re)allocate dynamic FFT work buffers if too small.
 *   Returns 1 on success, 0 on allocation failure.
 *---------------------------------------------------------------------------*/
static int ADC_EnsureFFTBufs (U32 n)
{
    if (n <= fftBufSize)
        return 1;

    free (fftPaddedWork); fftPaddedWork = NULL;
    free (fftOut0);       fftOut0       = NULL;
    free (fftOut1);       fftOut1       = NULL;
    fftBufSize = 0;

    fftPaddedWork = (double *)         malloc (n * sizeof (double));
    fftOut0       = (NIComplexNumber *)malloc (n * sizeof (NIComplexNumber));
    fftOut1       = (NIComplexNumber *)malloc (n * sizeof (NIComplexNumber));

    if (!fftPaddedWork || !fftOut0 || !fftOut1)
    {
        free (fftPaddedWork); free (fftOut0); free (fftOut1);
        fftPaddedWork = NULL; fftOut0 = NULL; fftOut1 = NULL;
        return 0;
    }

    fftBufSize = n;
    return 1;
}

/*---------------------------------------------------------------------------
 * HardwarePollThread — polls for half-buffer ready events
 *---------------------------------------------------------------------------*/
static int CVICALLBACK HardwarePollThread (void *data)
{
    BOOLEAN halfReady, fStop;
    U32     sts;
    int     activeBuf = 0;

    while (isAcquiring)
    {
        WD_AI_AsyncDblBufferHalfReady (adcCard, &halfReady, &fStop);
        diagPollCount++;

        if (fStop)
        {
            diagFStopCount++;
            isAcquiring = 0;
            PostDeferredCall ((DeferredCallbackPtr)ADC_StopDeferred, NULL);
            break;
        }

        if (halfReady)
        {
            U16 *src = (activeBuf == 0) ? dmaBuffer1 : dmaBuffer2;
            diagHalfReady++;

            /* Check for DMA overrun */
            WD_AI_ContStatus (adcCard, &sts);
            if (sts & 0x60000000U)
                diagDmaOverrun++;

            /* Save (priority) */
            if (saveMode == SAVE_THREAD && saveQueue != 0)
            {
                if (CmtWriteTSQData (saveQueue, src, 1, TSQ_WRITE_MS, NULL) < 1)
                    diagSaveDropped++;
            }

            /* Plot (lower priority — skip if previous plot not yet consumed) */
            if (!plotBusy && plotScans > 0)
            {
                U32 scans = (plotScans > PLOT_SCANS_MAX) ? PLOT_SCANS_MAX : plotScans;
                memcpy (plotBuffer, src, scans * ADC_NUM_CH * sizeof (U16));
                plotBusy = 1;
                diagPlotPosted++;
                PostDeferredCall ((DeferredCallbackPtr)ADC_PlotDeferred, NULL);
            }

            /* Re-arm hardware for next half */
            if (saveMode == SAVE_TOFILE)
                WD_AI_AsyncDblBufferToFile (adcCard);
            else
                WD_AI_AsyncDblBufferHandled (adcCard);

            activeBuf ^= 1;
        }
        else
        {
            Sleep (0);   /* yield CPU when no half-buffer is ready */
        }
    }

    return 0;
}

/*---------------------------------------------------------------------------
 * DiskSaveThread — reads from TSQ and writes raw U16 to file
 *---------------------------------------------------------------------------*/
static int CVICALLBACK DiskSaveThread (void *data)
{
    U16  *slotBuf;
    U32   flushCounter = 0;
    int   n;

    slotBuf = (U16 *)malloc (HALF_BUF_SAMPLES * sizeof (U16));
    if (!slotBuf) return -1;

    /* Drain queue while acquisition is running */
    while (isAcquiring)
    {
        n = CmtReadTSQData (saveQueue, slotBuf, 1, 200, 0);
        if (n > 0 && recordFile)
        {
            fwrite (slotBuf, sizeof (U16), HALF_BUF_SAMPLES, recordFile);
            diagSaveWritten++;
            if (++flushCounter % 4 == 0)
                fflush (recordFile);
        }
    }

    /* Drain any remaining items after poll thread has stopped */
    while (CmtReadTSQData (saveQueue, slotBuf, 1, 0, 0) > 0)
    {
        if (recordFile)
        {
            fwrite (slotBuf, sizeof (U16), HALF_BUF_SAMPLES, recordFile);
            diagSaveWritten++;
        }
    }

    if (recordFile)
    {
        fflush (recordFile);
        fclose (recordFile);
        recordFile = NULL;
    }

    free (slotBuf);
    return 0;
}

/*---------------------------------------------------------------------------
 * ADC_PlotDeferred — runs on UI thread; plots one chirp of CH0 + CH1
 *   in the time domain and (if N is a power of 2) the windowed, zero-padded
 *   FFT magnitude with coherent-gain amplitude correction.
 *
 *   FFT output is in dBm, converted from peak voltage via:
 *     dBm = dBV_peak − 3 dB (peak→RMS) + 30 dB (dBV→dBm)
 *   Formula:  corrected_dB = 20·log10(|X|) − 20·log10(N/2) + window_correction + 27
 *   where window_correction = −20·log10(coherentgain)  from GetWinProperties().
 *   Need additional factor for -10*log10(50)!
 *---------------------------------------------------------------------------*/
static void CVICALLBACK ADC_PlotDeferred (void *data)
{
    static double ch0[PLOT_SCANS_MAX];
    static double ch1[PLOT_SCANS_MAX];
    static double fftMag0[PLOT_SCANS_MAX];
    static double fftMag1[PLOT_SCANS_MAX];

    U32         i, scans, paddedN, halfPadN;
    int         windowType, padFactor;
    double      scale, correction_dB, window_correction_dB;
    WindowConst winProps;
    PFFTTable   tbl;

    scans = plotScans;
    if (scans == 0 || scans > PLOT_SCANS_MAX) { plotBusy = 0; return; }

    /* Read FFT parameters from UI rings (defaults if controls not yet in UIR) */
    if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_WINDOW,  &windowType) != 0)
        windowType = 1;   /* default: Hann */
    if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_ZEROPAD, &padFactor)  != 0)
        padFactor  = 0;   /* default: no padding */
    if (padFactor < 0) padFactor = 0;
    if (padFactor > 8) padFactor = 8;

    /* Deinterleave and convert to volts.
       Raw U16 (0–65535): 32768 = 0 V.  Subtract mid-point as double to
       avoid signed-overflow from the (I16) cast on values >= 32768. */
    scale = adcVoltScale / 32768.0;
    for (i = 0; i < scans; i++)
    {
        ch0[i] = ((double)plotBuffer[i * 2    ] - 32768.0) * scale;
        ch1[i] = ((double)plotBuffer[i * 2 + 1] - 32768.0) * scale;
    }

    /* ---- Time-domain plot ---- */
    {
        int    ph;
        double yLimit = adcVoltScale * 1.1;
        double adcSampRate, timePerSample_us;

        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ADC_SAMP_RATE, &adcSampRate);
        timePerSample_us = (adcSampRate > 0.0) ? 1.0 / adcSampRate : 1.0;  /* us */

        DeleteGraphPlot (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, -1, VAL_DELAYED_DRAW);
        SetAxisScalingMode (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                            VAL_LEFT_YAXIS, VAL_MANUAL, -yLimit, yLimit);

        /* Bottom x-axis: sample index */
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                          ATTR_ACTIVE_XAXIS, VAL_BOTTOM_XAXIS);
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                          ATTR_XNAME, "Sample");

        /* Top x-axis: time (us) */
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                          ATTR_ACTIVE_XAXIS, VAL_TOP_XAXIS);
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                          ATTR_XAXIS_GAIN, timePerSample_us);
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                          ATTR_XAXIS_OFFSET, 0.0);
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                          ATTR_XNAME, "Time (us)");

        /* Plot against bottom (sample) axis */
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME,
                          ATTR_ACTIVE_XAXIS, VAL_BOTTOM_XAXIS);

        ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, ch0, (int)scans,
                    VAL_DOUBLE, VAL_FAT_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_GREEN);
        SetPlotAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, ph,
                          ATTR_PLOT_LG_TEXT, "LRX (CH0)");
        ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, ch1, (int)scans,
                    VAL_DOUBLE, VAL_FAT_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_MAGENTA);
        SetPlotAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, ph,
                          ATTR_PLOT_LG_TEXT, "URX (CH1)");
    }

    /* ---- FFT — only when scans is a power of 2 (>= 4) ---- */
    if (scans >= 4 && (scans & (scans - 1)) == 0)
    {
        /* Compute paddedN = scans * 2^padFactor, capped so halfPadN <= PLOT_SCANS_MAX */
        paddedN = scans << padFactor;
        if (paddedN > PLOT_SCANS_MAX * 2U || paddedN < scans) /* overflow guard */
            paddedN = PLOT_SCANS_MAX * 2U;
        halfPadN = paddedN / 2;

        /* Get/create cached FFT table and allocate work buffers */
        tbl = ADC_GetFFTTable (paddedN);
        if (!tbl)                         { plotBusy = 0; return; }
        if (!ADC_EnsureFFTBufs (paddedN)) { plotBusy = 0; return; }

        /* Amplitude correction (dBm, one-sided spectrum):
             correction_dB = window_correction - 20·log10(N/2) - 3 + 30
           −3 dB converts peak → RMS;  +30 dB converts dBV → dBm.
           N is the number of real signal samples (scans), not the padded length,
           because the signal energy comes from scans points.                      */
        GetWinProperties (windowType, &winProps);
        window_correction_dB = (winProps.coherentgain > 0.0)
                                ? -20.0 * log10 (winProps.coherentgain)
                                :  0.0;
        correction_dB = window_correction_dB
                        - 20.0 * log10 ((double)(scans / 2)) // length of FFT
                        - 3.0    /* peak → RMS  */
                        + 30.0 /* dBW → dBm   */
						- 10.0*log10(50.0);  /* 50 ohm input   */
						// account for bits for voltage like BB24? 

        /* --- CH0 (LRX): window → zero-pad → FFTEx → magnitude → dBm --- */
        memcpy (fftPaddedWork, ch0, scans * sizeof (double));
        ADC_ApplyWindow (fftPaddedWork, scans, windowType);
        if (paddedN > scans)
            memset (fftPaddedWork + scans, 0,
                    (paddedN - scans) * sizeof (double));
        FFTEx (fftPaddedWork, (ssize_t)paddedN, (ssize_t)paddedN,
               tbl, FALSE, fftOut0);
        for (i = 0; i < halfPadN; i++)
        {
            double re = fftOut0[i].real;
            double im = fftOut0[i].imaginary;
            fftMag0[i] = 20.0 * log10 (sqrt (re*re + im*im) + 1e-20)
                         + correction_dB;
        }

        /* --- CH1 (URX): same processing --- */
        memcpy (fftPaddedWork, ch1, scans * sizeof (double));
        ADC_ApplyWindow (fftPaddedWork, scans, windowType);
        if (paddedN > scans)
            memset (fftPaddedWork + scans, 0,
                    (paddedN - scans) * sizeof (double));
        FFTEx (fftPaddedWork, (ssize_t)paddedN, (ssize_t)paddedN,
               tbl, FALSE, fftOut1);
        for (i = 0; i < halfPadN; i++)
        {
            double re = fftOut1[i].real;
            double im = fftOut1[i].imaginary;
            fftMag1[i] = 20.0 * log10 (sqrt (re*re + im*im) + 1e-20)
                         + correction_dB;
        }

        /* ---- FFT x-axis scaling: range (bottom) + beat frequency (top) ----
           Beat freq per bin  = f_s / paddedN
           Range per bin      = c × scans / (2 × BW_tx × paddedN)
           where BW_tx        = FREQ_MULTIPLIER × |actStop − actStart| (Hz)
           f_s                = adcClkMHz × 1e6 (Hz)                          */
        {
            int    ph;
            double adcSampRate, actStart, actStop;
            double fs_Hz, txBW_Hz, rangePerBin, freqPerBin_kHz;
            U32    peak0 = 1, peak1 = 1;
            char   lgText0[64], lgText1[64];

            GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ADC_SAMP_RATE, &adcSampRate);
            GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_START,     &actStart);
            GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_STOP,      &actStop);

            fs_Hz   = adcSampRate * 1e6;
            txBW_Hz = FREQ_MULTIPLIER * fabs (actStop - actStart) * 1e6;

            freqPerBin_kHz = (fs_Hz > 0.0)
                             ? (fs_Hz / (double)paddedN) / 1000.0
                             : 1.0;
            rangePerBin    = (txBW_Hz > 0.0)
                             ? (SPEED_OF_LIGHT * (double)scans)
                               / (2.0 * txBW_Hz * (double)paddedN)
                             : 1.0;

            /* ---- ADC tab FFT graph ---- */
            DeleteGraphPlot (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, -1, VAL_DELAYED_DRAW);

            /* Bottom x-axis: range (m) */
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_ACTIVE_XAXIS, VAL_BOTTOM_XAXIS);
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_XAXIS_GAIN, rangePerBin);
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_XAXIS_OFFSET, 0.0);
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_XNAME, "Range (m)");

            /* Top x-axis: beat frequency (kHz) */
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_ACTIVE_XAXIS, VAL_TOP_XAXIS);
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_XAXIS_GAIN, freqPerBin_kHz);
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_XAXIS_OFFSET, 0.0);
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_XNAME, "Beat Freq (kHz)");

            /* ---- Find peak bin per channel (skip DC at bin 0) ---- */
            {
                U32 k;
                for (k = 2; k < halfPadN; k++)
                {
                    if (fftMag0[k] > fftMag0[peak0]) peak0 = k;
                    if (fftMag1[k] > fftMag1[peak1]) peak1 = k;
                }
            }
            snprintf (lgText0, sizeof(lgText0), "LRX  %.2fm  %.2fdBm",
                      (double)peak0 * rangePerBin, fftMag0[peak0]);
            snprintf (lgText1, sizeof(lgText1), "URX  %.2fm  %.2fdBm",
                      (double)peak1 * rangePerBin, fftMag1[peak1]);

            /* Plot against bottom (range) axis */
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                              ATTR_ACTIVE_XAXIS, VAL_BOTTOM_XAXIS);

            ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, fftMag0, (int)halfPadN,
                        VAL_DOUBLE, VAL_FAT_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_GREEN);
            SetPlotAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, lgText0);
            ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, fftMag1, (int)halfPadN,
                        VAL_DOUBLE, VAL_FAT_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_MAGENTA);
            SetPlotAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, lgText1);

            /* Peak markers (diamond) */
            PlotPoint (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                       (double)peak0, fftMag0[peak0],
                       VAL_SOLID_DIAMOND, VAL_RED);
            PlotPoint (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT,
                       (double)peak1, fftMag1[peak1],
                       VAL_SOLID_DIAMOND, VAL_BLUE);

            /* ---- Master tab FFT graph (same scaling + markers) ---- */
            DeleteGraphPlot (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, -1, VAL_DELAYED_DRAW);

            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_ACTIVE_XAXIS, VAL_BOTTOM_XAXIS);
            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_XAXIS_GAIN, rangePerBin);
            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_XAXIS_OFFSET, 0.0);
            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_XNAME, "Range (m)");

            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_ACTIVE_XAXIS, VAL_TOP_XAXIS);
            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_XAXIS_GAIN, freqPerBin_kHz);
            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_XAXIS_OFFSET, 0.0);
            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_XNAME, "Beat Freq (kHz)");

            SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                              ATTR_ACTIVE_XAXIS, VAL_BOTTOM_XAXIS);

            ph = PlotY (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, fftMag0, (int)halfPadN,
                        VAL_DOUBLE, VAL_FAT_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_GREEN);
            SetPlotAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, lgText0);
            ph = PlotY (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, fftMag1, (int)halfPadN,
                        VAL_DOUBLE, VAL_FAT_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_MAGENTA);
            SetPlotAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, lgText1);

            PlotPoint (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                       (double)peak0, fftMag0[peak0],
                       VAL_SOLID_DIAMOND, VAL_RED);
            PlotPoint (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT,
                       (double)peak1, fftMag1[peak1],
                       VAL_SOLID_DIAMOND, VAL_BLUE);
        }
    }

    diagPlotDone++;
    plotBusy = 0;
}

/*---------------------------------------------------------------------------
 * ADC_StopAcquisition — idempotent cleanup; safe to call from UI thread
 *---------------------------------------------------------------------------*/
static void ADC_StopAcquisition (void)
{
    U32 startPos, accessCnt;

    isAcquiring = 0;

    if (pollThreadID != 0)
    {
        CmtWaitForThreadPoolFunctionCompletion (DEFAULT_THREAD_POOL_HANDLE,
                                               pollThreadID,
                                               OPT_TP_PROCESS_EVENTS_WHILE_WAITING);
        CmtReleaseThreadPoolFunctionID (DEFAULT_THREAD_POOL_HANDLE, pollThreadID);
        pollThreadID = 0;
    }

    if (saveThreadID != 0)
    {
        CmtWaitForThreadPoolFunctionCompletion (DEFAULT_THREAD_POOL_HANDLE,
                                               saveThreadID,
                                               OPT_TP_PROCESS_EVENTS_WHILE_WAITING);
        CmtReleaseThreadPoolFunctionID (DEFAULT_THREAD_POOL_HANDLE, saveThreadID);
        saveThreadID = 0;
    }

    if (adcCard >= 0)
    {
        WD_AI_AsyncClear (adcCard, &startPos, &accessCnt);

        /* Disable double-buffer mode and release all driver-side buffer
           registrations.  Without ContBufferReset the driver keeps the old
           page-pin entries, causing -201 ErrorConfigIoctl on the next
           WD_AI_ContBufferSetup call (even with freshly allocated memory). */
        WD_AI_AsyncDblBufferMode (adcCard, 0);
        WD_AI_ContBufferReset (adcCard);
    }

    /* Now safe to free user-space buffers — driver registrations are cleared */
    ADC_FreeBuffer (&hDmaBuffer1, &dmaBuffer1);
    ADC_FreeBuffer (&hDmaBuffer2, &dmaBuffer2);

    if (saveQueue != 0)
    {
        CmtDiscardTSQ (saveQueue);
        saveQueue = 0;
    }

    /* Safety: close file if save thread failed to do so */
    if (recordFile)
    {
        fflush (recordFile);
        fclose (recordFile);
        recordFile = NULL;
    }

    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_TIMER_POLL,    ATTR_ENABLED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 0);

    /* Sync master tab */
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_START,  ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_RECORD, ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_STOP,   ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,  ATTR_DIMMED, 0);

    /* Re-enable PSU readback timer if PSU is connected */
    if (psuSession != VI_NULL)
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_TIMER_READBACK, ATTR_ENABLED, 1);
}

/*---------------------------------------------------------------------------
 * ADC_StopDeferred — posted by poll thread when fStop fires; runs on UI thread
 *---------------------------------------------------------------------------*/
static void CVICALLBACK ADC_StopDeferred (void *data)
{
    char msg[128];
    snprintf (msg, sizeof(msg),
              "ADC: Stopped (fStop) — HR:%u FS:%u Qd:%u",
              (unsigned)diagHalfReady,
              (unsigned)diagFStopCount,
              (unsigned)diagSaveWritten);
    ADC_StopAcquisition ();
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
}

/*---------------------------------------------------------------------------
 * ADC_StartCommon — arm hardware and start threads
 *   mode     : SAVE_NONE | SAVE_THREAD | SAVE_TOFILE
 *   reTrgCnt : RETRIG_CNT_INF (continuous) or RETRIG_CNT_ONE (single shot)
 * Returns 0 on success, -1 on failure (status message already set).
 *---------------------------------------------------------------------------*/
static int ADC_StartCommon (int mode, U32 reTrgCnt)
{
    I16  err;
    int  trigPolIdx, trigModeIdx;
    U16  trigPol, trigMode;
    float  trigLevel;
    int    trigCount;
    int    scanInterval;
    U32    postTrigScans = 0, preTrigScans = 0, trigDelayTicks = 0;
    char msg[256];

    if (!adcConfigured)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Configure first");
        return -1;
    }
    if (isAcquiring)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Already running");
        return -1;
    }

    saveMode = mode;

    /* Read trigger polarity from UI ring.
       Ring items: 0 = Positive edge, 1 = Negative edge. */
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TRIG_POL, &trigPolIdx);
    trigPol = (trigPolIdx == 1) ? WD_AI_TrgNegative : WD_AI_TrgPositive;

    /* Read trigger mode from UI ring.
       Ring items: 0 = Post, 1 = Delay.
       Pre and Middle modes removed — incompatible with double-buffer DMA streaming.
       Defaults to POST if control read fails (UIR not yet updated). */
    if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TRIG_MODE, &trigModeIdx) != 0)
        trigModeIdx = 0;

    switch (trigModeIdx)
    {
        /* case 1:  trigMode = WD_AI_TRGMOD_PRE;   break;  // REMOVED: incompatible with double-buffer */
        /* case 2:  trigMode = WD_AI_TRGMOD_MIDL;  break;  // REMOVED: incompatible with double-buffer */
        case 1:  trigMode = WD_AI_TRGMOD_DELAY; break;
        default: trigMode = WD_AI_TRGMOD_POST;  break;
    }

    /* Read trigger level (0.0 – 3.3 V) from UI numeric.
       Used as the external digital trigger threshold voltage.
       Defaults to 2.4 V if control read fails. */
    if (GetCtrlVal (adcTabHandle, ADC_TAB_NUM_TRIG_LEVEL, &trigLevel) != 0)
        trigLevel = 2.4;
    if (trigLevel < 0.0) trigLevel = 0.0;
    if (trigLevel > 3.3) trigLevel = 3.3;

    /* Read trigger count from UI numeric (used for Delay mode only).
       Defaults to 0 if control read fails. */
    if (GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_TRIG_COUNT, &trigCount) != 0)
        trigCount = 0;
    if (trigCount < 0) trigCount = 0;

    /* Route the trigger count to the correct Trig_Config parameter based on mode.
       POST:  postTrigScans/preTrigScans/trigDelayTicks all 0 (continuous double-buffer)
       DELAY: trigDelayTicks = trigCount (in sampling clock ticks)
       PRE and MIDL removed — single-buffer finite modes, incompatible with AsyncDblBuffer. */
    switch (trigModeIdx)
    {
        /* case 1:  preTrigScans   = (U32)trigCount; break;  // PRE  — REMOVED */
        /* case 2:  preTrigScans   = (U32)trigCount; break;  // MIDL — REMOVED */
        case 1:  trigDelayTicks = (U32)trigCount; break;  /* DELAY */
        default: break;                                   /* POST  */
    }

    /* Clear any stale buffer registrations from a previous acquisition cycle
       or from WD_AD_Auto_Calibration_ALL (which may register internal buffers).
       This MUST come before Trig_Config — calling ContBufferReset after
       Trig_Config can wipe the trigger configuration, causing immediate fStop
       on subsequent acquisitions.  Without this call, ContBufferSetup below
       can return -201 ErrorConfigIoctl. */
    WD_AI_ContBufferReset (adcCard);

    /* Configure trigger (matches ADLINK reference sample order:
       Trig_Config → AsyncDblBufferMode → ContBufferSetup → ContScanChannels). */
    err = WD_AI_Trig_Config (adcCard,
                             trigMode,
                             WD_AI_TRGSRC_ExtD,
                             trigPol,
                             0, trigLevel,
                             postTrigScans, preTrigScans, trigDelayTicks,
                             (U32)reTrgCnt);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: Trig_Config failed (%d) mode=%d lvl=%.1f",
                  (int)err, trigModeIdx, trigLevel);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Enable double-buffer mode */
    err = WD_AI_AsyncDblBufferMode (adcCard, 1);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: AsyncDblBufferMode failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Free any previously registered buffers, then allocate fresh ones.
       WD_AI_ContBufferSetup pins pages in the driver; calling it again with the
       same addresses (without GlobalUnfix first) returns -201.  Re-allocating on
       every start guarantees the driver always sees virgin, unregistered memory. */
    ADC_FreeBuffer (&hDmaBuffer1, &dmaBuffer1);
    ADC_FreeBuffer (&hDmaBuffer2, &dmaBuffer2);
    if (ADC_AllocBuffer (&hDmaBuffer1, &dmaBuffer1) != 0 ||
        ADC_AllocBuffer (&hDmaBuffer2, &dmaBuffer2) != 0)
    {
        ADC_FreeBuffer (&hDmaBuffer1, &dmaBuffer1);
        ADC_FreeBuffer (&hDmaBuffer2, &dmaBuffer2);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                    "ADC: DMA buffer allocation failed");
        return -1;
    }

    /* Register both DMA half-buffers.  ADLINK double-buffer samples always
       pass the LAST buffer's ID to ContScanChannels/ContReadChannel.  */
    err = WD_AI_ContBufferSetup (adcCard, dmaBuffer1, HALF_BUF_SAMPLES, &firstBufId);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: ContBufferSetup buf1 failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return -1;
    }
    err = WD_AI_ContBufferSetup (adcCard, dmaBuffer2, HALF_BUF_SAMPLES, &secondBufId);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: ContBufferSetup buf2 failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Reset diagnostics */
    diagPollCount = diagHalfReady = diagFStopCount = 0;
    diagSaveWritten = diagSaveDropped = diagPlotPosted = diagPlotDone = diagDmaOverrun = 0;
    plotBusy = 0;

    /* Read ScanInterval from ADC tab (1..16777215) */
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_SCAN_INTERVAL, &scanInterval);
    if (scanInterval < 1) scanInterval = 1;

    /* Arm hardware — pass secondBufId (last registered buffer) to match
       ADLINK double-buffer reference samples.
       ScanIntrv & SampIntrv both set to scanInterval (see Blunderbuss ref). */
    if (mode == SAVE_TOFILE)
    {
        err = WD_AI_ContScanChannelsToFile (adcCard,
                                            (U16)(ADC_NUM_CH - 1),
                                            secondBufId,
                                            (U8 *)recordPath,
                                            SCANS_PER_HALF,
                                            (U32)scanInterval, (U32)scanInterval,
                                            ASYNCH_OP);
    }
    else
    {
        err = WD_AI_ContScanChannels (adcCard,
                                      (U16)(ADC_NUM_CH - 1),
                                      secondBufId,
                                      SCANS_PER_HALF,
                                      (U32)scanInterval, (U32)scanInterval,
                                      ASYNCH_OP);
    }
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: ContScanChannels arm failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Start save thread (SAVE_THREAD mode only) */
    if (mode == SAVE_THREAD)
    {
        if (CmtNewTSQ (SAVE_QUEUE_CAP, HALF_BUF_SAMPLES * sizeof (U16), 0, &saveQueue) < 0)
        {
            SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: TSQ creation failed");
            WD_AI_AsyncClear (adcCard, NULL, NULL);
            return -1;
        }
        if (CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                           DiskSaveThread, NULL, &saveThreadID) < 0)
        {
            SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Save thread failed");
            CmtDiscardTSQ (saveQueue); saveQueue = 0;
            WD_AI_AsyncClear (adcCard, NULL, NULL);
            return -1;
        }
    }

    /* Start poll thread */
    isAcquiring = 1;
    if (CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                       HardwarePollThread, NULL, &pollThreadID) < 0)
    {
        isAcquiring = 0;
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Poll thread failed");
        if (saveThreadID != 0)
        {
            CmtWaitForThreadPoolFunctionCompletion (DEFAULT_THREAD_POOL_HANDLE,
                                                   saveThreadID, 0);
            CmtReleaseThreadPoolFunctionID (DEFAULT_THREAD_POOL_HANDLE, saveThreadID);
            saveThreadID = 0;
        }
        if (saveQueue != 0) { CmtDiscardTSQ (saveQueue); saveQueue = 0; }
        WD_AI_AsyncClear (adcCard, NULL, NULL);
        return -1;
    }

    /* Enable diagnostic timer */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_TIMER_POLL, ATTR_INTERVAL, 0.5);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_TIMER_POLL, ATTR_ENABLED,  1);

    /* Update button states — dim everything except Stop while acquiring */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);

    /* Sync master tab */
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_START,  ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_RECORD, ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_STOP,   ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,  ATTR_DIMMED, 1);

    /* Suspend PSU readback timer — its blocking VISA I/O on the UI thread
       can delay processing of PostDeferredCall plot updates */
    SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_TIMER_READBACK, ATTR_ENABLED, 0);

    return 0;
}

/*---------------------------------------------------------------------------
 * ADC_Cleanup — full teardown; called from main() on application exit
 *---------------------------------------------------------------------------*/
static void ADC_Cleanup (void)
{
    if (isAcquiring)
        ADC_StopAcquisition ();

    ADC_FreeBuffer (&hDmaBuffer1, &dmaBuffer1);
    ADC_FreeBuffer (&hDmaBuffer2, &dmaBuffer2);

    /* Release FFT table and dynamic work buffers */
    if (fftTableCached)
    {
        DestroyFFTTable (fftTableCached);
        fftTableCached = NULL;
        fftTableN      = 0;
    }
    free (fftPaddedWork); fftPaddedWork = NULL;
    free (fftOut0);       fftOut0       = NULL;
    free (fftOut1);       fftOut1       = NULL;
    fftBufSize = 0;

    if (adcRegistered)
    {
        WD_Release_Card (adcCard);
        adcCard       = -1;
        adcRegistered = 0;
        adcConfigured = 0;
    }
}

/*===========================================================================
 *  CALLBACKS  -  ADC TAB
 *===========================================================================*/

/*---------------------------------------------------------------------------
 * AdcRegisterCB — register the PCI-9846H with the WD-DASK driver
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcRegisterCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int  cardNum;
    I16  handle;
    char msg[128];

    if (ev != EVENT_COMMIT) return 0;
    if (adcRegistered)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Already registered");
        return 0;
    }

    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_CARD_NUM, &cardNum);
    handle = WD_Register_Card (PCI_9846H, (U16)cardNum);
    if (handle < 0)
    {
        snprintf (msg, sizeof(msg), "ADC: Register_Card failed (%d)", (int)handle);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return 0;
    }

    adcCard       = handle;
    adcRegistered = 1;
    adcConfigured = 0;

    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Registered OK");

    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 0);
    return 0;
}

/*---------------------------------------------------------------------------
 * ADC auto-calibration — runs in a background thread to avoid blocking the UI.
 *   WD_AD_Auto_Calibration_ALL can take several seconds and previously caused
 *   fatal errors / hangs when called on the UI thread.
 *---------------------------------------------------------------------------*/
static volatile int adcCalibrating    = 0;
static volatile I16 adcCalResult      = 0;
static volatile int adcCalCardNum     = 0;   /* card number for cal thread */

static int CVICALLBACK AdcCalThread (void *data)
{
    I16    handle;

    /* ---- Step 1: Fresh register (ADLINK examples always calibrate on a
       freshly registered card with no AI_Config/buffers). ---- */
    if (adcRegistered)
    {
        WD_Release_Card (adcCard);
        adcRegistered = 0;
        adcConfigured = 0;
    }

    handle = WD_Register_Card (PCI_9846H, (U16)adcCalCardNum);
    if (handle < 0)
    {
        adcCalResult = handle;
        PostDeferredCall ((DeferredCallbackPtr)ADC_CalDoneDeferred, NULL);
        return 0;
    }
    adcCard       = handle;
    adcRegistered = 1;

    /* ---- Step 2: Calibrate ---- */
    adcCalResult = WD_AD_Auto_Calibration_ALL (adcCard);

    /* ---- Step 3: Wait 5 s for internal processing to settle ---- */
    Delay (5.0);

    /* ---- Step 4: Release the card so user gets a clean re-register
       on next Configure. ---- */
    WD_Release_Card (adcCard);
    adcRegistered = 0;
    adcConfigured = 0;

    PostDeferredCall ((DeferredCallbackPtr)ADC_CalDoneDeferred, NULL);
    return 0;
}

static void CVICALLBACK ADC_CalDoneDeferred (void *data)
{
    adcCalibrating = 0;

    /* Card has been released by AdcCalThread — user must re-register+configure
       via the Setup button or the Configure button on the ADC tab. */
    adcRegistered = 0;
    adcConfigured = 0;

    if (adcCalResult >= NoError)
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                    "ADC: Calibration OK — please re-Configure");
    else
    {
        char msg[128];
        snprintf (msg, sizeof(msg), "ADC: Calibration failed (%d)", (int)adcCalResult);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
    }

    /* Card is released — user must re-register before configuring.
       Only Register is enabled; everything else is dimmed. */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER,  ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);

    /* Master tab — un-dim setup button so user can re-setup */
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,
                      ATTR_DIMMED, 0);
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Offline — re-Configure");
}

int CVICALLBACK AdcCalibrateCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    CmtThreadFunctionID calThreadID;

    if (ev != EVENT_COMMIT) return 0;

    if (isAcquiring || adcCalibrating)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Busy");
        return 0;
    }

    /* Read card number on UI thread before spawning the background thread.
       The background thread will release any existing card, re-register fresh,
       calibrate, wait 5 s, then release again.  This follows the ADLINK
       pattern: Register → Calibrate → Release (no AI_Config/buffers). */
    {
        int cn;
        GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_CARD_NUM, &cn);
        adcCalCardNum = cn;
    }
    adcCalibrating = 1;
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                "ADC: Calibrating (fresh register)...");

    /* Dim all buttons while calibration runs */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);

    CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                   AdcCalThread, NULL, &calThreadID);
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcConfigureCB — configure ADC channels and allocate kernel-aligned DMA buffers
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcConfigureCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    int  timebase, impedance, range, i;
    I16  err;
    char msg[256];

    if (ev != EVENT_COMMIT) return 0;
    if (!adcRegistered)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Register first");
        return 0;
    }
    if (isAcquiring)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Stop acquisition first");
        return 0;
    }

    adcConfigured = 0;

    /* Read UI settings */
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TIMEBASE,     &timebase);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_IMPEDANCE,    &impedance);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_RANGE,        &range);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_SAMP_PER_TRIG, (int *)&plotScans);

    /* Map ring index to the WD-DASK range constant.
       The ring returns its item index (0 or 1); clamp to valid table bounds. */
    if (range < 0 || range >= ADC_RANGE_TABLE_LEN)
        range = 0;                                /* default to ±5 V */

    if (plotScans > PLOT_SCANS_MAX) plotScans = PLOT_SCANS_MAX;
    adcVoltScale = ADC_RangeToVolts (adcRangeTable[range]);

    /* Cache the UI-selected settings that were actually sent to hardware.
       ADC_WriteSidecarHeader reads from these instead of re-querying the GUI,
       eliminating any race between Configure and Record. */
    adcConfiguredRange     = range;
    adcConfiguredImpedance = impedance;
    adcConfiguredTimebase  = timebase;

    /* Configure ADC: external timebase, no duty restore, ConvSrc=0, single-edge,
       no auto-reset */
    err = WD_AI_Config (adcCard, WD_ExtTimeBase, 0, 0, 0, 0);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: AI_Config failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return 0;
    }

    /* Configure all channels with a single call (matches P98x6/DbfTrig reference sample).
       Impedance setting still applied per-channel if requested. */
    err = WD_AI_CH_Config (adcCard, (U16)All_Channels, adcRangeTable[range]);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: CH_Config failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return 0;
    }
    /* Set impedance per-channel: ring value maps directly to driver constants
       (IMPEDANCE_50Ohm=0, IMPEDANCE_HI=1). Always call explicitly. */
    for (i = 0; i < (int)ADC_NUM_CH; i++)
    {
        err = WD_AI_CH_ChangeParam (adcCard, (U16)i, AI_IMPEDANCE, (U32)impedance);
        if (err != NoError)
        {
            snprintf (msg, sizeof(msg),
                      "ADC: CH_ChangeParam impedance ch%d failed (%d)", i, (int)err);
            SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
            return 0;
        }
    }

    adcConfigured = 1;
    snprintf (msg, sizeof(msg),
              "ADC: Configured — ±%.3fV, %u scans/half-buf, plotScans=%u",
              adcVoltScale, SCANS_PER_HALF, plotScans);
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);

    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 0);
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcStartCB — monitor mode: acquire + plot, no saving
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcStartCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    if (ADC_StartCommon (SAVE_NONE, RETRIG_CNT_INF) == 0)
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Acquiring (monitor)");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcRecordCB — SAVE_THREAD mode: TSQ producer/consumer → raw binary file
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcRecordCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    char datPath[512];

    if (ev != EVENT_COMMIT) return 0;

    ADC_GenerateFilename (recordPath, sizeof (recordPath));
    snprintf (datPath, sizeof (datPath), "%s.dat", recordPath);
    recordFile = fopen (datPath, "wb");
    if (!recordFile)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                    "ADC: Cannot open output file");
        return 0;
    }

    if (ADC_StartCommon (SAVE_THREAD, RETRIG_CNT_INF) < 0)
    {
        fclose (recordFile); recordFile = NULL;
        return 0;
    }

    ADC_WriteSidecarHeader (recordPath, SAVE_THREAD, "record-thread");
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Recording (thread)");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcSaveCB — SAVE_TOFILE mode: driver writes DMA data directly to file
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcSaveCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    ADC_GenerateFilename (recordPath, sizeof (recordPath));

    if (ADC_StartCommon (SAVE_TOFILE, RETRIG_CNT_INF) < 0)
        return 0;

    ADC_WriteSidecarHeader (recordPath, SAVE_TOFILE, "record-tofile");
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Recording (ToFile)");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcSingleShotCB — monitor mode, one trigger only
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcSingleShotCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    if (ADC_StartCommon (SAVE_NONE, RETRIG_CNT_ONE) == 0)
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Single-shot armed");
    return 0; // This worked on first config, but failed when resetting.
}

/*---------------------------------------------------------------------------
 * AdcStopCB — stop all threads and clean up
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcStopCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    ADC_StopAcquisition ();
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Stopped");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcReleaseCB — free DMA buffers and release card
 *   Release is dimmed during acquisition, so isAcquiring should never be true
 *   here.  The guard remains as a safety net.
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcReleaseCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    if (isAcquiring)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                    "ADC: Stop acquisition before releasing");
        return 0;
    }

    ADC_FreeBuffer (&hDmaBuffer1, &dmaBuffer1);
    ADC_FreeBuffer (&hDmaBuffer2, &dmaBuffer2);

    if (adcRegistered)
    {
        WD_Release_Card (adcCard);
        adcCard       = -1;
        adcRegistered = 0;
        adcConfigured = 0;
    }

    SetCtrlVal       (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Released");
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER,  ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcPollTimerCB — display pipeline diagnostics every 0.5 s
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcPollTimerCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    char msg[256];
    if (ev != EVENT_TIMER_TICK) return 0;

    snprintf (msg, sizeof(msg),
              "Poll:%u HR:%u FS:%u | Sw:%u Sd:%u | PTrg:%u PDn:%u | OVR:%u",
              (unsigned)diagPollCount,
              (unsigned)diagHalfReady,
              (unsigned)diagFStopCount,
              (unsigned)diagSaveWritten,
              (unsigned)diagSaveDropped,
              (unsigned)diagPlotPosted,
              (unsigned)diagPlotDone,
              (unsigned)diagDmaOverrun);

    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
    return 0;
}

/*===========================================================================
 *  CALLBACKS  -  MASTER TAB
 *===========================================================================*/

/*---------------------------------------------------------------------------
 * FindResourceBySubstring — returns resourceList index containing substr,
 *   or -1 if not found.  Must be called after ScanVISAResources().
 *---------------------------------------------------------------------------*/
static int FindResourceBySubstring (const char *substr)
{
    int i;
    for (i = 0; i < numResources; i++)
        if (strstr (resourceList[i], substr) != NULL)
            return i;
    return -1;
}

/* Expected PSU values for colour numeric range checking */
#define CH1_VOLT_NOM   18.0
#define CH1_VOLT_TOL    0.5
#define CH2_VOLT_NOM    8.0
#define CH2_VOLT_TOL    0.5
#define CH1_CURR_NOM    2.0    /* nominal current draw (A) — green within ±1 A */
#define CH2_CURR_NOM    1.9
#define CH1_CURR_TOL    1.0
#define CH2_CURR_TOL    1.0

/*---------------------------------------------------------------------------
 * MasterUpdateTimerCB — periodic refresh (~500 ms)
 *   Reads PSU values from the PSU tab indicators (no duplicate VISA I/O),
 *   mirrors relay/DDS/ADC state from globals, updates colour numerics.
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterUpdateTimerCB (int p, int c, int ev, void *cbd,
                                     int e1, int e2)
{
    double v1, i1, v2, i2;
    char buf[64];

    if (ev != EVENT_TIMER_TICK) return 0;

    /* ---- PSU readback: mirror from PSU tab numerics ---- */
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH1_VOLT_READ, &v1);
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH1_CURR_READ, &i1);
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH2_VOLT_READ, &v2);
    GetCtrlVal (psuTabHandle, PSU_TAB_PSU_NUM_CH2_CURR_READ, &i2);

    /* CH1 voltage colour numeric */
    snprintf (buf, sizeof(buf), "%.2f", v1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_VOLT,
                      ATTR_LABEL_TEXT, buf);
    if (v1 >= (CH1_VOLT_NOM - CH1_VOLT_TOL) &&
        v1 <= (CH1_VOLT_NOM + CH1_VOLT_TOL))
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_VOLT,
                    VAL_GREEN);
    else if (v1 < 0.1)
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_VOLT,
                    VAL_DK_GRAY);
    else
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_VOLT,
                    VAL_RED);

    /* CH1 current colour numeric */
    snprintf (buf, sizeof(buf), "%.3f", i1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_CURR,
                      ATTR_LABEL_TEXT, buf);
    if (i1 >= (CH1_CURR_NOM - CH1_CURR_TOL) && i1 <= (CH1_CURR_NOM + CH1_CURR_TOL))
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_CURR, VAL_GREEN);
    else if (i1 < 0.1)
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_CURR, VAL_DK_GRAY);
    else
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH1_CURR, VAL_RED);

    /* CH2 voltage colour numeric */
    snprintf (buf, sizeof(buf), "%.2f", v2);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_VOLT,
                      ATTR_LABEL_TEXT, buf);
    if (v2 >= (CH2_VOLT_NOM - CH2_VOLT_TOL) &&
        v2 <= (CH2_VOLT_NOM + CH2_VOLT_TOL))
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_VOLT,
                    VAL_GREEN);
    else if (v2 < 0.1)
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_VOLT,
                    VAL_DK_GRAY);
    else
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_VOLT,
                    VAL_RED);

    /* CH2 current colour numeric */
    snprintf (buf, sizeof(buf), "%.3f", i2);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_CURR,
                      ATTR_LABEL_TEXT, buf);
    if (i2 >= (CH2_CURR_NOM - CH2_CURR_TOL) && i2 <= (CH2_CURR_NOM + CH2_CURR_TOL))
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_CURR, VAL_GREEN);
    else if (i2 < 0.1)
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_CURR, VAL_DK_GRAY);
    else
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_NUM_CH2_CURR, VAL_RED);

    /* Meters */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_CH1_METER, i1);
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_CH2_METER, i2);

    /* ---- PSU all-on LED ---- */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_PSU,
                (ch1OutState && ch2OutState && ch3OutState) ? 1 : 0);

    /* ---- Relay LED ---- */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_TX_RELAY, relay1State);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_TX_RELAY,
                      ATTR_LABEL_TEXT,
                      relay1State ? "Active: On" : "Active: Off");

    /* ---- DDS LED ---- */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_DDS, ddsSweepActive);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_DDS,
                      ATTR_LABEL_TEXT,
                      ddsSweepActive ? "Sensors: ON" : "Sensors: OFF");

    /* ---- ADC LED ---- */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_ADC,
                adcConfigured ? 1 : 0);

    /* ---- Saving LED: alternate on/off while saving ---- */
    if (saveMode != SAVE_NONE && isAcquiring)
    {
        static int saveLedState = 0;
        saveLedState = !saveLedState;
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_SAVING, saveLedState);
    }
    else
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_SAVING, 0);

    /* ---- Status messages ---- */
    {
        char ddsMsg[256], adcMsg[128];

        /* When FMCW sweeping, show TX BW and centre frequency on master */
        if (ddsSweepActive && lastActualPeriod_us > 0.0)
        {
            double txBW, txCentre;
            GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_BW,     &txBW);
            GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TX_CENTRE, &txCentre);
            snprintf (ddsMsg, sizeof(ddsMsg),
                      "FMCW: BW=%.3f GHz  Fc=%.3f GHz",
                      txBW, txCentre);
        }
        else
        {
            GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, ddsMsg);
        }
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS, ddsMsg);
        if (isAcquiring)
            snprintf (adcMsg, sizeof(adcMsg),
                      "Targeting Status: Tracking — HR:%u OVR:%u",
                      (unsigned)diagHalfReady, (unsigned)diagDmaOverrun);
        else if (adcConfigured)
            strcpy (adcMsg, "Targeting Status: Target Locked");
        else if (adcRegistered)
            strcpy (adcMsg, "Targeting Status: Standby");
        else
            strcpy (adcMsg, "Targeting Status: Offline");
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS, adcMsg);
    }

    /* ---- Power / reactor button label ---- */
    if (psuSession == VI_NULL)
        SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_PSU_ALL,
                          ATTR_LABEL_TEXT, "Reactor: Offline");
    else if (ch1OutState && ch2OutState)
        SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_PSU_ALL,
                          ATTR_LABEL_TEXT, "Reactor: On");
    else
        SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_PSU_ALL,
                          ATTR_LABEL_TEXT, "Reactor: Off");

    return 0;
}

/*---------------------------------------------------------------------------
 * MasterPsuAllCB — toggle all PSU outputs (delegates to existing logic)
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterPsuAllCB (int p, int c, int ev, void *cbd,
                                int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    /* Auto-connect PSU (USBTMC/USB resource) if not already connected */
    if (psuSession == VI_NULL)
    {
        int    idx;
        char   resp[READ_BUF_LEN];

        ScanVISAResources ();

        /* Find a USB instrument resource that is not a serial (ASRL) port */
        idx = -1;
        {
            int k;
            for (k = 0; k < numResources; k++)
            {
                if (strstr (resourceList[k], "USB")  != NULL &&
                    strstr (resourceList[k], "ASRL") == NULL)
                {
                    idx = k;
                    break;
                }
            }
        }

        if (idx < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_STATUS_LABEL,
                        "PSU: No USB instrument found");
            return 0;
        }

        SetCtrlVal (psuTabHandle, PSU_TAB_PSU_RING_RESOURCE, idx);

        if (PSU_Connect (resourceList[idx]) < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_STATUS_LABEL,
                        "PSU: USB connect failed");
            return 0;
        }

        /* Mirror the connected state into the PSU tab */
        if (PSU_Query ("*IDN?", resp, sizeof(resp)) == 0)
            SetCtrlVal (psuTabHandle, PSU_TAB_PSU_MSG_IDN, resp);
        SetCtrlVal       (psuTabHandle, PSU_TAB_PSU_MSG_STATUS, "Status: Connected");
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CONNECT,    ATTR_DIMMED, 1);
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_DISCONNECT, ATTR_DIMMED, 0);
        SetPSUDimmed (0);
        ch1OutState = ch2OutState = ch3OutState = 0;
        SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH1_OUTPUT, 0);
        SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH2_OUTPUT, 0);
        SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH3_OUTPUT, 0);
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_ALL_OUTPUT,
                          ATTR_LABEL_TEXT, "ALL OUT: OFF");
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_TIMER_READBACK, ATTR_ENABLED, 1);
        SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_TIMER_UPDATE,
                          ATTR_ENABLED, 1);
    }

    /* Now delegate to the normal all-output toggle */
    return PsuAllOutCB (p, c, ev, cbd, e1, e2);
}

/*---------------------------------------------------------------------------
 * MasterTxRelayCB — toggle TX relay (relay 1)
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterTxRelayCB (int p, int c, int ev, void *cbd,
                                 int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    if (relaySession == VI_NULL)
    {
        int idx;
        ScanVISAResources ();
        idx = FindResourceBySubstring ("ASRL18");
        if (idx < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Relay: ASRL18 not found");
            return 0;
        }
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_RING_RESOURCE, idx);
        if (Relay_Connect (resourceList[idx]) < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Relay: connect to ASRL18 failed");
            return 0;
        }
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_MSG_STATUS, "Status: Connected");
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_CONNECT,    ATTR_DIMMED, 1);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_DISCONNECT, ATTR_DIMMED, 0);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,       ATTR_DIMMED, 0);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,       ATTR_DIMMED, 0);
        relay1State = relay2State = 0;
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, 0);
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY2, 0);
    }
    relay1State = !relay1State;
    if (Relay_Set (1, relay1State) < 0)
    {
        relay1State = !relay1State;
        return 0;
    }
    /* Sync relay tab */
    SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, relay1State);
    SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,
                      ATTR_LABEL_TEXT,
                      relay1State ? "Relay 1: ON" : "Relay 1: OFF");
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterDdsCB — start or stop DDS chirp
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterDdsCB (int p, int c, int ev, void *cbd,
                             int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    /* Auto-connect DDS if not already connected */
    if (!ddsConnected)
    {
        int idx;
        ScanVISAResources ();
        idx = FindResourceBySubstring ("ASRL19");
        if (idx < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS,
                        "DDS: ASRL19 not found");
            return 0;
        }
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_RESOURCE, idx);
        /* Delegate to DdsConnectCB which handles port parsing and init */
        DdsConnectCB (ddsTabHandle, DDS_TAB_DDS_BTN_CONNECT,
                      EVENT_COMMIT, NULL, 0, 0);
        if (!ddsConnected)
            return 0;   /* DdsConnectCB already set an error status */
    }

    if (ddsSweepActive)
    {
        /* Active sweep: switch to CW mode (stops DROver trigger cleanly) */
        DdsStopCB (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,
                   EVENT_COMMIT, NULL, 0, 0);
    }
    else if (ddsInitDone)
    {
        /* Already init'd and in CW: just resume FMCW sweep (no re-init) */
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, 0);
        DdsStartCB (ddsTabHandle, DDS_TAB_DDS_BTN_START,
                    EVENT_COMMIT, NULL, 0, 0);

        if (ddsSweepActive)
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS,
                        "DDS: FMCW resumed (no re-init)");
    }
    else
    {
        /* Not initialised: full init/cal then start FMCW triggered sweep */
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS,
                    "DDS: Init & calibrating...");

        dds_powerdown ();
        dds_reset ();
        if (!dds_powerup ())
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS,
                        "DDS: Powerup FAILED");
            return 0;
        }
        if (!dds_reset ())
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS,
                        "DDS: Reset FAILED");
            return 0;
        }
        if (!ad9914_calibrate_dac ())
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS,
                        "DDS: DAC cal FAILED");
            return 0;
        }

        ddsInitDone = 1;
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Init & calibration OK");

        /* Force triggered (DRCTRL) chirp mode and start FMCW sweep */
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, 0);
        DdsStartCB (ddsTabHandle, DDS_TAB_DDS_BTN_START,
                    EVENT_COMMIT, NULL, 0, 0);

        if (ddsSweepActive)
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS,
                        "DDS: FMCW triggered sweep active");
    }
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterAcqSetupCB — one-button: Register → Calibrate → Release →
 *   Re-register → Configure.  Runs in a background thread because
 *   calibration blocks for several seconds.
 *---------------------------------------------------------------------------*/
static volatile int masterSetupRunning    = 0;
static volatile int masterSetupCardNum    = 0;
static volatile int masterSetupConfigDone = 0;

/* Deferred: update master status label (pass a string-literal pointer) */
static void CVICALLBACK MasterSetupStatusDeferred (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                (const char *)data);
}

/* Deferred: run AdcConfigureCB on UI thread, then signal done */
static void CVICALLBACK MasterSetupConfigDeferred (void *data)
{
    AdcConfigureCB (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE,
                    EVENT_COMMIT, NULL, 0, 0);
    masterSetupConfigDone = 1;
}

/* Deferred: final UI update on success */
static void CVICALLBACK MasterSetupDoneDeferred (void *data)
{
    masterSetupRunning = 0;
    adcCalibrating     = 0;

    if (adcConfigured)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Target Locked");
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                    "ADC: Calibrated & Configured OK");
        SetCtrlAttribute (masterTabHandle,
                          MASTER_TAB_MASTER_BTN_ACQ_START,  ATTR_DIMMED, 0);
        SetCtrlAttribute (masterTabHandle,
                          MASTER_TAB_MASTER_BTN_ACQ_RECORD, ATTR_DIMMED, 0);
        SetCtrlAttribute (masterTabHandle,
                          MASTER_TAB_MASTER_TIMER_UPDATE, ATTR_ENABLED, 1);
    }
    else
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Configure Failed");
    }

    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,
                      ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_RESET_STAR,
                      ATTR_DIMMED, 0);
    /* ADC tab: card is registered+configured, enable the usual buttons */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 0);
}

/* Deferred: final UI update on failure */
static void CVICALLBACK MasterSetupFailDeferred (void *data)
{
    masterSetupRunning = 0;
    adcCalibrating     = 0;

    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                (const char *)data);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,
                      ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_RESET_STAR,
                      ATTR_DIMMED, 0);

    /* Set ADC tab buttons based on current card state */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER,  ATTR_DIMMED, adcRegistered);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, !adcRegistered);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, !adcRegistered);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, !adcRegistered);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
}

/* Background thread: Register → Calibrate → Wait → Release → Re-register
   → Configure (deferred) */
static int CVICALLBACK MasterSetupThread (void *data)
{
    I16 handle, calResult;

    /* Step 1: Release if already registered */
    if (adcRegistered)
    {
        WD_Release_Card (adcCard);
        adcRegistered = 0;
        adcConfigured = 0;
    }

    /* Step 2: Register fresh */
    PostDeferredCall ((DeferredCallbackPtr)MasterSetupStatusDeferred,
                      "Targeting Status: Registering...");
    handle = WD_Register_Card (PCI_9846H, (U16)masterSetupCardNum);
    if (handle < 0)
    {
        PostDeferredCall ((DeferredCallbackPtr)MasterSetupFailDeferred,
                          "Targeting Status: Register failed");
        return 0;
    }
    adcCard       = handle;
    adcRegistered = 1;

    /* Step 3: Calibrate */
    PostDeferredCall ((DeferredCallbackPtr)MasterSetupStatusDeferred,
                      "Targeting Status: Calibrating...");
    calResult = WD_AD_Auto_Calibration_ALL (adcCard);
    if (calResult < NoError)
    {
        PostDeferredCall ((DeferredCallbackPtr)MasterSetupFailDeferred,
                          "Targeting Status: Calibration failed");
        return 0;
    }

    /* Step 4: Wait 5 s for internal processing to settle */
    PostDeferredCall ((DeferredCallbackPtr)MasterSetupStatusDeferred,
                      "Targeting Status: Cal settling...");
    Delay (5.0);

    /* Step 5: Release */
    WD_Release_Card (adcCard);
    adcRegistered = 0;
    adcConfigured = 0;

    /* Step 6: Re-register */
    PostDeferredCall ((DeferredCallbackPtr)MasterSetupStatusDeferred,
                      "Targeting Status: Re-registering...");
    handle = WD_Register_Card (PCI_9846H, (U16)masterSetupCardNum);
    if (handle < 0)
    {
        PostDeferredCall ((DeferredCallbackPtr)MasterSetupFailDeferred,
                          "Targeting Status: Re-register failed");
        return 0;
    }
    adcCard       = handle;
    adcRegistered = 1;

    /* Step 7: Configure (must run on UI thread — reads ring controls) */
    masterSetupConfigDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)MasterSetupConfigDeferred, NULL);
    while (!masterSetupConfigDone)
        Delay (0.05);

    /* Step 8: Done */
    PostDeferredCall ((DeferredCallbackPtr)MasterSetupDoneDeferred, NULL);
    return 0;
}

int CVICALLBACK MasterAcqSetupCB (int p, int c, int ev, void *cbd,
                                  int e1, int e2)
{
    CmtThreadFunctionID setupThreadID;
    int cardNum;

    if (ev != EVENT_COMMIT) return 0;

    if (isAcquiring || adcCalibrating || masterSetupRunning)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Busy");
        return 0;
    }

    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_CARD_NUM, &cardNum);
    masterSetupCardNum = cardNum;
    masterSetupRunning = 1;
    adcCalibrating     = 1;   /* prevent standalone calibrate button */

    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Target Lock...");
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,
                      ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_RESET_STAR,
                      ATTR_DIMMED, 1);

    /* Dim all ADC tab buttons during setup */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);

    CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                   MasterSetupThread, NULL, &setupThreadID);
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterAcqStartCB — start acquisition (monitor, no saving)
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterAcqStartCB (int p, int c, int ev, void *cbd,
                                  int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    if (ADC_StartCommon (SAVE_NONE, RETRIG_CNT_INF) == 0)
    {
        SetCtrlVal (adcTabHandle,    ADC_TAB_ADC_MSG_STATUS,
                    "ADC: Acquiring (monitor)");
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Tracking");
    }
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterAcqRecordCB — start recording (SAVE_THREAD mode)
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterAcqRecordCB (int p, int c, int ev, void *cbd,
                                   int e1, int e2)
{
    char datPath[512];
    if (ev != EVENT_COMMIT) return 0;

    ADC_GenerateFilename (recordPath, sizeof (recordPath));
    snprintf (datPath, sizeof (datPath), "%s.dat", recordPath);
    recordFile = fopen (datPath, "wb");
    if (!recordFile)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: File error");
        return 0;
    }

    if (ADC_StartCommon (SAVE_THREAD, RETRIG_CNT_INF) < 0)
    {
        fclose (recordFile); recordFile = NULL;
        return 0;
    }

    ADC_WriteSidecarHeader (recordPath, SAVE_THREAD, "master-record");
    SetCtrlVal (adcTabHandle,    ADC_TAB_ADC_MSG_STATUS,
                "ADC: Recording (thread)");
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Recording");
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterAcqStopCB — stop acquisition
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterAcqStopCB (int p, int c, int ev, void *cbd,
                                 int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    ADC_StopAcquisition ();
    SetCtrlVal (adcTabHandle,    ADC_TAB_ADC_MSG_STATUS,    "ADC: Stopped");
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Target Locked");
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterSequenceCB / MasterSequenceThread — single-press automated startup
 *   PSU on → 0.5 s (verify non-zero) → connect relay + DDS →
 *   DDS init/cal + CW start → 0.5 s → ADC register+calibrate+re-register+
 *   configure → start monitor acq → 0.5 s → TX relay on → 0.5 s →
 *   DRG triggered chirp → done.
 *
 *   Each step runs on the UI thread via PostDeferredCall (because the master
 *   callbacks touch UI controls).  The background thread waits for each step
 *   to complete, then delays before posting the next step.
 *---------------------------------------------------------------------------*/
static volatile int seqRunning  = 0;   /* 1 while sequence thread is active */
static volatile int seqStepDone = 0;   /* set by each deferred step         */
static volatile int seqAbort    = 0;   /* set if a step fails               */

/* Helper: wait for seqStepDone (polled from background thread) */
static void SeqWaitForStep (void)
{
    while (!seqStepDone && !seqAbort)
        Delay (0.05);
}

/* ---- Deferred step: PSU connect + outputs on ---- */
static void CVICALLBACK SeqStep_PSU (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Reactor startup...");

    /* If outputs are already on, skip */
    if (psuSession != VI_NULL && ch1OutState && ch2OutState && ch3OutState)
    {
        seqStepDone = 1;
        return;
    }

    /* Delegate to the master PSU button which auto-connects + toggles on */
    MasterPsuAllCB (masterTabHandle, MASTER_TAB_MASTER_BTN_PSU_ALL,
                    EVENT_COMMIT, NULL, 0, 0);

    if (psuSession == VI_NULL || !(ch1OutState && ch2OutState && ch3OutState))
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Reactor FAILED");
        seqAbort = 1;
    }
    seqStepDone = 1;
}

/* ---- Deferred step: DDS connect + start chirp ---- */
static void CVICALLBACK SeqStep_DDS (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: DDS startup...");

    if (ddsSweepActive)
    {
        seqStepDone = 1;
        return;
    }

    MasterDdsCB (masterTabHandle, MASTER_TAB_MASTER_BTN_DDS,
                 EVENT_COMMIT, NULL, 0, 0);

    if (!ddsSweepActive)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: DDS FAILED");
        seqAbort = 1;
    }
    seqStepDone = 1;
}

/* ---- Deferred step: ADC register + calibrate + re-register + configure
       MasterAcqSetupCB spawns MasterSetupThread; the sequence thread
       polls masterSetupRunning to wait for completion. ---- */
static void CVICALLBACK SeqStep_ADCSetup (void *data)
{
    if (!adcConfigured && !masterSetupRunning)
    {
        MasterAcqSetupCB (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,
                          EVENT_COMMIT, NULL, 0, 0);
    }
    seqStepDone = 1;
}

/* ---- Deferred step: start acquisition (monitor, no save) ---- */
static void CVICALLBACK SeqStep_ADCStart (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Engaging...");

    if (!isAcquiring)
    {
        MasterAcqStartCB (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_START,
                          EVENT_COMMIT, NULL, 0, 0);
    }

    if (!isAcquiring)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Acquire FAILED");
        seqAbort = 1;
    }
    seqStepDone = 1;
}

/* ---- Deferred step: final status ---- */
static void CVICALLBACK SeqStep_Done (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Tracking");
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SEQUENCE,
                      ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SHUTDOWN,
                      ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_RESET_STAR,
                      ATTR_DIMMED, 0);
    seqRunning = 0;
}

/* ---- Deferred step: connect relay and DDS if not already connected ---- */
static void CVICALLBACK SeqStep_ConnectRelayDds (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Connecting peripherals...");

    /* Connect relay if not connected */
    if (relaySession == VI_NULL)
    {
        int idx;
        ScanVISAResources ();
        idx = FindResourceBySubstring ("ASRL18");
        if (idx < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Targeting Status: Relay not found");
            seqAbort = 1;
            seqStepDone = 1;
            return;
        }
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_RING_RESOURCE, idx);
        if (Relay_Connect (resourceList[idx]) < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Targeting Status: Relay connect failed");
            seqAbort = 1;
            seqStepDone = 1;
            return;
        }
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_MSG_STATUS, "Status: Connected");
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_CONNECT,    ATTR_DIMMED, 1);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_DISCONNECT, ATTR_DIMMED, 0);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,       ATTR_DIMMED, 0);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,       ATTR_DIMMED, 0);
        relay1State = relay2State = 0;
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, 0);
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY2, 0);
    }

    /* Connect DDS if not connected */
    if (!ddsConnected)
    {
        int idx;
        ScanVISAResources ();
        idx = FindResourceBySubstring ("ASRL19");
        if (idx < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Targeting Status: DDS not found");
            seqAbort = 1;
            seqStepDone = 1;
            return;
        }
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_RESOURCE, idx);
        DdsConnectCB (ddsTabHandle, DDS_TAB_DDS_BTN_CONNECT,
                      EVENT_COMMIT, NULL, 0, 0);
        if (!ddsConnected)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Targeting Status: DDS connect failed");
            seqAbort = 1;
            seqStepDone = 1;
            return;
        }
    }

    seqStepDone = 1;
}

/* ---- Deferred step: DDS init/cal then start FMCW triggered sweep ---- */
static void CVICALLBACK SeqStep_DdsInitCW (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: DDS init & sweep...");

    /* Full init/cal sequence: powerdown → reset → powerup → reset → calibrate */
    dds_powerdown ();
    dds_reset ();
    if (!dds_powerup ())
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: DDS powerup FAILED");
        seqAbort = 1;
        seqStepDone = 1;
        return;
    }
    if (!dds_reset ())
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: DDS reset FAILED");
        seqAbort = 1;
        seqStepDone = 1;
        return;
    }
    if (!ad9914_calibrate_dac ())
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: DDS DAC cal FAILED");
        seqAbort = 1;
        seqStepDone = 1;
        return;
    }

    ddsInitDone = 1;
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Init & calibration OK");

    /* Force triggered (DRCTRL) mode and start FMCW sweep directly */
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, 0);
    DdsStartCB (ddsTabHandle, DDS_TAB_DDS_BTN_START,
                EVENT_COMMIT, NULL, 0, 0);

    if (!ddsSweepActive)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: DDS sweep start FAILED");
        seqAbort = 1;
    }

    seqStepDone = 1;
}

/* ---- Deferred step: switch on TX relay (relay 1) ---- */
static void CVICALLBACK SeqStep_TxRelayOn (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: TX relay on...");

    if (!relay1State)
    {
        relay1State = 1;
        if (Relay_Set (1, 1) < 0)
        {
            relay1State = 0;
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Targeting Status: TX relay FAILED");
            seqAbort = 1;
            seqStepDone = 1;
            return;
        }
        SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, 1);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,
                          ATTR_LABEL_TEXT, "Relay 1: ON");
    }

    seqStepDone = 1;
}

/* ---- Deferred step: start DRG triggered chirp ---- */
static void CVICALLBACK SeqStep_DdsStartChirp (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Starting chirp...");

    /* Force triggered (DRCTRL) mode and start chirp */
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, 0);
    DdsStartCB (ddsTabHandle, DDS_TAB_DDS_BTN_START,
                EVENT_COMMIT, NULL, 0, 0);

    if (!ddsSweepActive)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Chirp start FAILED");
        seqAbort = 1;
    }
    seqStepDone = 1;
}

/* ---- Background thread: orchestrate startup sequence ---- */
static int CVICALLBACK MasterSequenceThread (void *data)
{
    /* Step 1: PSU on */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_PSU, NULL);
    SeqWaitForStep ();
    if (seqAbort) goto seq_fail;

    /* Verify power output is non-zero, then wait 0.5 s */
    Delay (0.5);
    if (!(ch1OutState && ch2OutState && ch3OutState))
    {
        seqAbort = 1;
        goto seq_fail;
    }

    /* Step 2: Connect relay and DDS */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_ConnectRelayDds, NULL);
    SeqWaitForStep ();
    if (seqAbort) goto seq_fail;

    /* Step 3: DDS init/cal + start FMCW triggered sweep */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_DdsInitCW, NULL);
    SeqWaitForStep ();
    if (seqAbort) goto seq_fail;
    Delay (0.5);

    /* Step 4: ADC register + calibrate + re-register + configure */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_ADCSetup, NULL);
    SeqWaitForStep ();
    if (seqAbort) goto seq_fail;

    /* Wait for the setup thread (including calibration) to complete */
    while (masterSetupRunning && !seqAbort)
        Delay (0.1);

    if (!adcConfigured)
    {
        seqAbort = 1;
        goto seq_fail;
    }

    /* Step 5: Start non-saving acquisition */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_ADCStart, NULL);
    SeqWaitForStep ();
    if (seqAbort) goto seq_fail;
    Delay (0.5);

    /* Step 6: TX relay on */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_TxRelayOn, NULL);
    SeqWaitForStep ();
    if (seqAbort) goto seq_fail;
    Delay (0.5);

    /* Done */
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_Done, NULL);
    return 0;

seq_fail:
    /* Un-dim the sequence button so user can retry */
    PostDeferredCall ((DeferredCallbackPtr)SeqStep_Done, NULL);
    return 0;
}

int CVICALLBACK MasterSequenceCB (int p, int c, int ev, void *cbd,
                                  int e1, int e2)
{
    CmtThreadFunctionID seqThreadID;

    if (ev != EVENT_COMMIT) return 0;

    if (seqRunning)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Sequence in progress...");
        return 0;
    }

    seqRunning  = 1;
    seqAbort    = 0;
    seqStepDone = 0;

    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SEQUENCE,
                      ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SHUTDOWN ,
                      ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_RESET_STAR,
                      ATTR_DIMMED, 1);

    CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                   MasterSequenceThread, NULL, &seqThreadID);
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterShutdownCB / MasterShutdownThread — single-press safe shutdown
 *   Stop ADC → 2 s → Relay off → 2 s → DDS off → 2 s → PSU off → done.
 *---------------------------------------------------------------------------*/

/* ---- Deferred step: stop ADC acquisition/saving, then release card ---- */
static void CVICALLBACK ShutStep_ADC (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Stopping ADC...");

    if (isAcquiring)
        ADC_StopAcquisition ();

    /* Release the card so next startup does a clean
       Register → Calibrate → Re-register → Configure cycle */
    ADC_FreeBuffer (&hDmaBuffer1, &dmaBuffer1);
    ADC_FreeBuffer (&hDmaBuffer2, &dmaBuffer2);
    if (adcRegistered)
    {
        WD_Release_Card (adcCard);
        adcCard       = -1;
        adcRegistered = 0;
        adcConfigured = 0;
    }

    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Released");

    /* Reset ADC tab buttons to initial state */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER,  ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);

    seqStepDone = 1;
}

/* ---- Deferred step: turn off both relays ---- */
static void CVICALLBACK ShutStep_Relay (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Relay off & disconnect...");

    if (relaySession != VI_NULL)
    {
        if (relay1State)
        {
            relay1State = 0;
            Relay_Set (1, 0);
            SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY1, 0);
            SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,
                              ATTR_LABEL_TEXT, "Relay 1: OFF");
        }
        if (relay2State)
        {
            relay2State = 0;
            Relay_Set (2, 0);
            SetCtrlVal (relayTabHandle, RELAY_TAB_RELAY_LED_RLY2, 0);
            SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,
                              ATTR_LABEL_TEXT, "Relay 2: OFF");
        }

        /* Disconnect relay serial session */
        viClose (relaySession);
        relaySession = VI_NULL;

        SetCtrlVal       (relayTabHandle, RELAY_TAB_RELAY_MSG_STATUS, "Status: Disconnected");
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_CONNECT,    ATTR_DIMMED, 0);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_DISCONNECT, ATTR_DIMMED, 1);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY1,       ATTR_DIMMED, 1);
        SetCtrlAttribute (relayTabHandle, RELAY_TAB_RELAY_BTN_RLY2,       ATTR_DIMMED, 1);
    }
    seqStepDone = 1;
}

/* ---- Deferred step: stop DDS chirp and disconnect ---- */
static void CVICALLBACK ShutStep_DDS (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: DDS stop & disconnect...");

    if (ddsSweepActive)
    {
        /* Switch to CW first for FMCW modes to cleanly stop DROver trigger */
        int chirpMode;
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, &chirpMode);
        if (chirpMode == 0 || chirpMode == 1)
        {
            double cwFreq, actCW;
            GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CW_FREQ, &cwFreq);
            ad9914_single_tone (cwFreq, &actCW);
            dds_update ();
        }
        dds_powerdown ();
        ddsSweepActive = 0;
    }

    if (ddsConnected)
    {
        deinit_dds ();
        ddsConnected = 0;
        ddsInitDone  = 0;
    }

    /* Reset DDS tab to disconnected state */
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Status: Disconnected");
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_CONNECT,    ATTR_DIMMED, 0);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_DISCONNECT, ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_INIT_CAL,   ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START,      ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,       ATTR_DIMMED, 1);

    seqStepDone = 1;
}

/* ---- Deferred step: turn off all PSU outputs ---- */
static void CVICALLBACK ShutStep_PSU (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Reactor off...");

    if (psuSession != VI_NULL && (ch1OutState || ch2OutState || ch3OutState))
    {
        ch1OutState = ch2OutState = ch3OutState = 0;
        PSU_OutputOnOff (1, 0);
        PSU_OutputOnOff (2, 0);
        PSU_OutputOnOff (3, 0);

        SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH1_OUTPUT, 0);
        SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH2_OUTPUT, 0);
        SetCtrlVal (psuTabHandle, PSU_TAB_PSU_LED_CH3_OUTPUT, 0);
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH1_OUTPUT,
                          ATTR_LABEL_TEXT, "CH1 OUT: OFF");
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH2_OUTPUT,
                          ATTR_LABEL_TEXT, "CH2 OUT: OFF");
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_CH3_OUTPUT,
                          ATTR_LABEL_TEXT, "CH3 OUT: OFF");
        SetCtrlAttribute (psuTabHandle, PSU_TAB_PSU_BTN_ALL_OUTPUT,
                          ATTR_LABEL_TEXT, "ALL OUT: OFF");
    }
    seqStepDone = 1;
}

/* ---- Deferred step: shutdown complete ---- */
static void CVICALLBACK ShutStep_Done (void *data)
{
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Offline");
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SHUTDOWN,
                      ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SEQUENCE,
                      ATTR_DIMMED, 0);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_RESET_STAR,
                      ATTR_DIMMED, 0);
    seqRunning = 0;
}

/* ---- Background thread: orchestrate shutdown with 2 s delays ---- */
static int CVICALLBACK MasterShutdownThread (void *data)
{
    /* Step 1: Stop ADC */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)ShutStep_ADC, NULL);
    SeqWaitForStep ();
    Delay (2.0);

    /* Step 2: Relay off */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)ShutStep_Relay, NULL);
    SeqWaitForStep ();
    Delay (2.0);

    /* Step 3: DDS off */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)ShutStep_DDS, NULL);
    SeqWaitForStep ();
    Delay (2.0);

    /* Step 4: PSU off */
    seqStepDone = 0;
    PostDeferredCall ((DeferredCallbackPtr)ShutStep_PSU, NULL);
    SeqWaitForStep ();
    Delay (2.0);

    /* Done */
    PostDeferredCall ((DeferredCallbackPtr)ShutStep_Done, NULL);
    return 0;
}

int CVICALLBACK MasterShutdownCB (int p, int c, int ev, void *cbd,
                                   int e1, int e2)
{
    CmtThreadFunctionID shutThreadID;

    if (ev != EVENT_COMMIT) return 0;

    if (seqRunning)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Sequence in progress...");
        return 0;
    }

    seqRunning  = 1;
    seqAbort    = 0;
    seqStepDone = 0;

    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SHUTDOWN,
                      ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_SEQUENCE,
                      ATTR_DIMMED, 1);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_RESET_STAR,
                      ATTR_DIMMED, 1);

    CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                   MasterShutdownThread, NULL, &shutThreadID);
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterResetStartCB — synchronized DDS/ADC restart for INS time sync
 *
 *   Provides a clean time-zero by:
 *   1. Put DDS in CW mode (no re-init, just single-tone switch)
 *   2. Stop any acquisition
 *   3. Configure ADC (register if needed) + start SAVE_TOFILE acquisition
 *   4. Wait 1 second
 *   5. Switch DDS to triggered FMCW sweep (no re-init since already in CW)
 *
 *   Runs synchronously on the UI thread.
 *---------------------------------------------------------------------------*/
int CVICALLBACK MasterResetStartCB (int p, int c, int ev, void *cbd,
                                     int e1, int e2)
{
    int    cardNum;
    I16    handle;
    double cwFreq, actCW;

    if (ev != EVENT_COMMIT) return 0;

    if (seqRunning || masterSetupRunning || adcCalibrating)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: Busy");
        return 0;
    }

    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Reset & Save...");
    ProcessSystemEvents ();

    /* ---- Step 1: Put DDS in CW mode (no re-init) ---- */
    if (ddsSweepActive)
    {
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CW_FREQ, &cwFreq);
        if (ad9914_single_tone (cwFreq, &actCW) && dds_update ())
        {
            ddsSweepActive = 0;
            SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Stopped sweep -> CW");
            SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START, ATTR_DIMMED, 0);
            SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,  ATTR_DIMMED, 1);
        }
    }
    ProcessSystemEvents ();

    /* ---- Step 2: Stop any acquisition ---- */
    if (isAcquiring)
        ADC_StopAcquisition ();
    ProcessSystemEvents ();

    /* ---- Step 3: Configure ADC + start SAVE_TOFILE acquisition ---- */
    if (!adcRegistered)
    {
        GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_CARD_NUM, &cardNum);
        handle = WD_Register_Card (PCI_9846H, (U16)cardNum);
        if (handle < 0)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "Targeting Status: ADC Register failed");
            return 0;
        }
        adcCard       = handle;
        adcRegistered = 1;
        adcConfigured = 0;
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_REGISTER, ATTR_DIMMED, 1);
    }
    if (!adcConfigured)
    {
        AdcConfigureCB (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE,
                        EVENT_COMMIT, NULL, 0, 0);
    }
    if (!adcConfigured)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: ADC Configure failed");
        return 0;
    }

    ADC_GenerateFilename (recordPath, sizeof (recordPath));

    if (ADC_StartCommon (SAVE_TOFILE, RETRIG_CNT_INF) < 0)
        return 0;

    ADC_WriteSidecarHeader (recordPath, SAVE_TOFILE, "synced-reset");
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                "ADC: Armed (ToFile) — waiting for trigger...");
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Armed — waiting 1 s...");
    ProcessSystemEvents ();

    /* ---- Step 4: Wait 1 second ---- */
    Delay (1.0);

    /* ---- Step 5: Switch DDS to triggered FMCW sweep (no re-init) ---- */
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE, 0);
    DdsStartCB (ddsTabHandle, DDS_TAB_DDS_BTN_START,
                EVENT_COMMIT, NULL, 0, 0);

    if (!ddsSweepActive)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "Targeting Status: DDS restart failed");
        return 0;
    }

    /* ---- Done — recording with synchronized time-zero ---- */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "Targeting Status: Recording (synced)");
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                "ADC: Recording (synced reset - ToFile)");

    return 0;
}
