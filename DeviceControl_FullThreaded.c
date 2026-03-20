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
 *   Link:  visa32.lib
 *===========================================================================*/

#include <windows.h>
#include <utility.h>
#include <userint.h>
#include <ansi_c.h>
#include <stddef.h>
#include <visa.h>
#include <formatio.h>

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
static int relayTabHandle, psuTabHandle, ddsTabHandle, adcTabHandle;

/* ---- Relay state ---- */
static int relay1State = 0, relay2State = 0;

/* ---- PSU state ---- */
static int ch1OutState = 0, ch2OutState = 0, ch3OutState = 0;

/* ---- DDS state ---- */
static int    ddsConnected      = 0;
static int    ddsSweepActive    = 0;
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
    if (PSU_Query("MEASure:CURRent? CH1",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_CURR_READ,v); }
    if (PSU_Query("MEASure:POWEr? CH1",  r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH1_PWR_READ,v);  }
    if (PSU_Query("MEASure:VOLTage? CH2",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_VOLT_READ,v); }
    if (PSU_Query("MEASure:CURRent? CH2",r,sizeof(r))==0) { v=atof(r); SetCtrlVal(psuTabHandle,PSU_TAB_PSU_NUM_CH2_CURR_READ,v); }
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
 *  Chirp duration  = sampsPerChirp x hmcDiv x 2   SYNC_CLK cycles
 *  DRCTRL period   = hmcDiv x 2 x progDiv         SYNC_CLK cycles
 *  Dead time       = hmcDiv x 2 x (progDiv - sampsPerChirp)  SYNC_CLK cycles
 *  Dead samples    = progDiv - sampsPerChirp       ADC samples
 *  Min progDiv     = sampsPerChirp + 1
 *===========================================================================*/
static void DDS_UpdateTimingDisplay (void)
{
    double clockMHz, syncClkMHz, syncClkPeriod_us;
    double adcClkMHz, trigFreqHz, drctrlPeriod_us, deadTime_us, calcPeriod_us;
    int    hmcDiv, progDiv, sampsPerChirp;
    int    adcClkDivTotal, chirpSteps, drctrlSteps, minProgDiv, deadSamples;
    char   warnMsg[256];

    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CLOCK_MHZ,     &clockMHz);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_HMC_DIV,       &hmcDiv);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_PROG_DIV,      &progDiv);
    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_SAMPS_PER_CHI, &sampsPerChirp);

    if (hmcDiv < 2)         hmcDiv = 2;
    if (progDiv < 1)        progDiv = 1;
    if (sampsPerChirp < 1)  sampsPerChirp = 1;

    /* SYNC_CLK = DDS_CLOCK / 24 */
    syncClkMHz       = clockMHz / 24.0;
    syncClkPeriod_us = (syncClkMHz > 0.0) ? 1.0 / syncClkMHz : 0.0;

    /* ADC_CLK = SYNC_CLK / (hmcDiv x 2)  [HMC432 #1 + fixed /2] */
    adcClkDivTotal = hmcDiv * 2;
    adcClkMHz      = syncClkMHz / (double)adcClkDivTotal;

    /* Chirp: exactly sampsPerChirp ADC samples = sampsPerChirp x adcClkDivTotal SYNC_CLK cycles */
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
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_SYNC_CLK,      syncClkMHz);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ADC_CLK,       adcClkMHz);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_TRIG_FREQ,     trigFreqHz);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_DRCTRL_PERIOD, drctrlPeriod_us);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CHIRP_STEPS,   chirpSteps);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_CALC_PERIOD,   calcPeriod_us);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_DEAD_TIME,     deadTime_us);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_DEAD_SAMPLES,  deadSamples);
    SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_MIN_PROG_DIV,  minProgDiv);

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

    GetCtrlVal (ddsTabHandle, TABPANEL_DDS_STR_COM_PORT,  comStr);
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

        if (!dds_reset ())   { return 0; }
        if (!dds_powerup ()) { return 0; }
        Delay(0.01);
        if (!ad9914_calibrate_dac ()) { return 0; }
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
        GetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_PERIOD,     &period);

        if (startF == stopF)
        {
            SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS,
                        "ERROR: Start/Stop frequencies cannot match in Sweep Mode");
            return 0;
        }

        if (!dds_reset ())   { return 0; }
        if (!dds_powerup ()) { return 0; }
        Delay(0.01);
        if (!ad9914_calibrate_dac ()) { return 0; }

        if (chirpMode == 0)
        {
            rampFlags = DRG_NO_DWELL_HIGH;
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

        if (!ad9914_ramp_generator (startF, stopF, period, rampFlags,
                                    &actStart, &actStop, &actPeriod))
        {
            SetCtrlVal (ddsTabHandle, TABPANEL_DDS_MSG_STATUS, "Ramp config FAILED");
            return 0;
        }

        if (!dds_update ()) { return 0; }

        ddsSweepActive = 1;

        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_START,  actStart);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_STOP,   actStop);
        SetCtrlVal (ddsTabHandle, TABPANEL_DDS_NUM_ACT_PERIOD, actPeriod);

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
#define HALF_BUF_SAMPLES    262144U     /* 2^18 U16 samples per half-buffer          */
#define SCANS_PER_HALF      131072U     /* HALF_BUF_SAMPLES / ADC_NUM_CH             */
#define SAVE_QUEUE_CAP      32          /* 32 × 512 KB = 16 MB queue headroom        */
#define TSQ_WRITE_MS        200         /* ms to wait before counting a save drop     */
#define PLOT_SCANS_MAX      131072U     /* max scans copied to plot buffer (one half) */
#define RETRIG_CNT_INF      65535U      /* continuous re-trigger count                */
#define RETRIG_CNT_ONE      1U          /* single-shot re-trigger count               */

#define SAVE_NONE    0
#define SAVE_THREAD  1                  /* user-space TSQ → fwrite thread            */
#define SAVE_TOFILE  2                  /* WD_AI_AsyncDblBufferToFile (driver-managed)*/

/*---------------------------------------------------------------------------
 * ADC Globals
 *---------------------------------------------------------------------------*/
static I16   adcCard       = -1;
static int   adcRegistered = 0;
static int   adcConfigured = 0;
static U16  *dmaBuffer1    = NULL;
static U16  *dmaBuffer2    = NULL;
static U16   firstBufId    = 0;
static U16   secondBufId   = 0;

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
static void CVICALLBACK ADC_PlotDeferred    (void *data);
static void CVICALLBACK ADC_StopDeferred    (void *data);
static double           ADC_RangeToVolts    (int rangeConst);
static void             ADC_GenerateFilename(char *buf, int bufLen);
static void             ADC_StopAcquisition (void);
static int              ADC_StartCommon     (int mode, U32 reTrgCnt);

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
 * ADC_GenerateFilename — CerberusData_YYYYMMDD_HHMMSS.dat
 *---------------------------------------------------------------------------*/
static void ADC_GenerateFilename (char *buf, int bufLen)
{
    int month, day, year, hours, minutes, seconds;
    GetSystemDate (&month, &day, &year);
    GetSystemTime (&hours, &minutes, &seconds);
    snprintf (buf, bufLen, "CerberusData_%04d%02d%02d_%02d%02d%02d.dat",
              year, month, day, hours, minutes, seconds);
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
 * ADC_PlotDeferred — runs on UI thread; plots one half-buffer of CH0 + CH1
 *---------------------------------------------------------------------------*/
static void CVICALLBACK ADC_PlotDeferred (void *data)
{
    static double ch0[PLOT_SCANS_MAX];
    static double ch1[PLOT_SCANS_MAX];
    U32    i, scans;
    double scale;

    scans = plotScans;
    if (scans == 0 || scans > PLOT_SCANS_MAX) { plotBusy = 0; return; }

    scale = adcVoltScale / 32768.0;
    for (i = 0; i < scans; i++)
    {
        ch0[i] = (double)((I16)plotBuffer[i * 2    ]) * scale;
        ch1[i] = (double)((I16)plotBuffer[i * 2 + 1]) * scale;
    }

    DeleteGraphPlot (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, -1, VAL_DELAYED_DRAW);
    PlotY (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, ch0, (int)scans,
           VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
    PlotY (adcTabHandle, TABPANEL_2_ADC_GRAPH_TIME, ch1, (int)scans,
           VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_BLUE);

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
        WD_AI_AsyncClear (adcCard, &startPos, &accessCnt);

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

    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_TIMER_POLL,  ATTR_ENABLED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RECORD,    ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SAVE,      ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SINGLE,    ATTR_DIMMED, 0);
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
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
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
    char msg[256];

    if (!adcConfigured)
    {
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Configure first");
        return -1;
    }
    if (isAcquiring)
    {
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Already running");
        return -1;
    }

    saveMode = mode;

    /* Reset and enable double-buffer mode */
    WD_AI_ContBufferReset (adcCard);
    err = WD_AI_AsyncDblBufferMode (adcCard, 1);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: AsyncDblBufferMode failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Register both DMA half-buffers — save buf1's ID separately */
    err = WD_AI_ContBufferSetup (adcCard, dmaBuffer1, HALF_BUF_SAMPLES, &firstBufId);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: ContBufferSetup buf1 failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return -1;
    }
    err = WD_AI_ContBufferSetup (adcCard, dmaBuffer2, HALF_BUF_SAMPLES, &secondBufId);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: ContBufferSetup buf2 failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Configure external digital trigger: POST, positive edge */
    err = WD_AI_Trig_Config (adcCard,
                             WD_AI_TRGMOD_POST,
                             WD_AI_TRGSRC_ExtD,
                             WD_AI_TrgPositive,
                             0, 0.0, 0, 0, 0,
                             (U32)reTrgCnt);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: Trig_Config failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Reset diagnostics */
    diagPollCount = diagHalfReady = diagFStopCount = 0;
    diagSaveWritten = diagSaveDropped = diagPlotPosted = diagPlotDone = diagDmaOverrun = 0;
    plotBusy = 0;

    /* Arm hardware */
    if (mode == SAVE_TOFILE)
    {
        err = WD_AI_ContScanChannelsToFile (adcCard,
                                            (U16)(ADC_NUM_CH - 1),
                                            firstBufId,
                                            (U8 *)recordPath,
                                            SCANS_PER_HALF,
                                            1, 1,
                                            ASYNCH_OP);
    }
    else
    {
        err = WD_AI_ContScanChannels (adcCard,
                                      (U16)(ADC_NUM_CH - 1),
                                      firstBufId,
                                      SCANS_PER_HALF,
                                      1, 1,
                                      ASYNCH_OP);
    }
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: ContScanChannels arm failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Start save thread (SAVE_THREAD mode only) */
    if (mode == SAVE_THREAD)
    {
        if (CmtNewTSQ (SAVE_QUEUE_CAP, HALF_BUF_SAMPLES * sizeof (U16), 0, &saveQueue) < 0)
        {
            SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: TSQ creation failed");
            WD_AI_AsyncClear (adcCard, NULL, NULL);
            return -1;
        }
        if (CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                           DiskSaveThread, NULL, &saveThreadID) < 0)
        {
            SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Save thread failed");
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
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Poll thread failed");
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
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_TIMER_POLL, ATTR_INTERVAL, 0.5);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_TIMER_POLL, ATTR_ENABLED,  1);

    /* Update button states */
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 0);

    return 0;
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
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Already registered");
        return 0;
    }

    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_NUM_CARD_NUM, &cardNum);
    handle = WD_Register_Card (PCI_9846H, (U16)cardNum);
    if (handle < 0)
    {
        snprintf (msg, sizeof(msg), "ADC: Register_Card failed (%d)", (int)handle);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return 0;
    }

    adcCard       = handle;
    adcRegistered = 1;
    adcConfigured = 0;

    SetCtrlVal       (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Registered OK");
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_CONFIGURE, ATTR_DIMMED, 0);
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
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Register first");
        return 0;
    }
    if (isAcquiring)
    {
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Stop acquisition first");
        return 0;
    }

    /* Free any existing DMA buffers */
    if (dmaBuffer1) { WD_Buffer_Free (adcCard, dmaBuffer1); dmaBuffer1 = NULL; }
    if (dmaBuffer2) { WD_Buffer_Free (adcCard, dmaBuffer2); dmaBuffer2 = NULL; }
    adcConfigured = 0;

    /* Read UI settings */
    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_RING_TIMEBASE,    &timebase);
    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_RING_IMPEDANCE,   &impedance);
    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_RING_RANGE,       &range);
    GetCtrlVal (adcTabHandle, TABPANEL_2_ADC_NUM_SAMP_PER_TRIG, (int *)&plotScans);

    if (plotScans > PLOT_SCANS_MAX) plotScans = PLOT_SCANS_MAX;
    adcVoltScale = ADC_RangeToVolts (range);

    /* Configure ADC: timebase from ring, no duty restore, ConvSrc=0, single-edge,
       no auto-reset */
    err = WD_AI_Config (adcCard, (U16)timebase, 0, 0, 0, 0);
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: AI_Config failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
        return 0;
    }

    /* Per-channel configuration */
    for (i = 0; i < (int)ADC_NUM_CH; i++)
    {
        err = WD_AI_CH_Config (adcCard, (U16)i, (U16)range);
        if (err != NoError)
        {
            snprintf (msg, sizeof(msg), "ADC: CH_Config ch%d failed (%d)", i, (int)err);
            SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
            return 0;
        }
        if (impedance)
        {
            err = WD_AI_CH_ChangeParam (adcCard, (U16)i, AI_IMPEDANCE, 1);
            if (err != NoError)
            {
                snprintf (msg, sizeof(msg),
                          "ADC: CH_ChangeParam impedance ch%d failed (%d)", i, (int)err);
                SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
                return 0;
            }
        }
    }

    /* Allocate kernel-aligned DMA buffers */
    dmaBuffer1 = (U16 *)WD_Buffer_Alloc (adcCard, HALF_BUF_SAMPLES * sizeof (U16));
    dmaBuffer2 = (U16 *)WD_Buffer_Alloc (adcCard, HALF_BUF_SAMPLES * sizeof (U16));
    if (!dmaBuffer1 || !dmaBuffer2)
    {
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS,
                    "ADC: DMA buffer allocation failed");
        if (dmaBuffer1) { WD_Buffer_Free (adcCard, dmaBuffer1); dmaBuffer1 = NULL; }
        if (dmaBuffer2) { WD_Buffer_Free (adcCard, dmaBuffer2); dmaBuffer2 = NULL; }
        return 0;
    }

    adcConfigured = 1;
    snprintf (msg, sizeof(msg),
              "ADC: Configured — ±%.3fV, %u scans/half-buf, plotScans=%u",
              adcVoltScale, SCANS_PER_HALF, plotScans);
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);

    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RECORD,    ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SAVE,      ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SINGLE,    ATTR_DIMMED, 0);
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcStartCB — monitor mode: acquire + plot, no saving
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcStartCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    if (ADC_StartCommon (SAVE_NONE, RETRIG_CNT_INF) == 0)
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Acquiring (monitor)");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcRecordCB — SAVE_THREAD mode: TSQ producer/consumer → raw binary file
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcRecordCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    ADC_GenerateFilename (recordPath, sizeof (recordPath));
    recordFile = fopen (recordPath, "wb");
    if (!recordFile)
    {
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS,
                    "ADC: Cannot open output file");
        return 0;
    }

    if (ADC_StartCommon (SAVE_THREAD, RETRIG_CNT_INF) < 0)
    {
        fclose (recordFile); recordFile = NULL;
        return 0;
    }

    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Recording (thread)");
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

    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Recording (ToFile)");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcSingleShotCB — monitor mode, one trigger only
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcSingleShotCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    if (ADC_StartCommon (SAVE_NONE, RETRIG_CNT_ONE) == 0)
        SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Single-shot armed");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcStopCB — stop all threads and clean up
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcStopCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    ADC_StopAcquisition ();
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Stopped");
    return 0;
}

/*---------------------------------------------------------------------------
 * AdcReleaseCB — stop if running, free DMA buffers, release card
 *---------------------------------------------------------------------------*/
int CVICALLBACK AdcReleaseCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    if (isAcquiring)
        ADC_StopAcquisition ();

    if (dmaBuffer1) { WD_Buffer_Free (adcCard, dmaBuffer1); dmaBuffer1 = NULL; }
    if (dmaBuffer2) { WD_Buffer_Free (adcCard, dmaBuffer2); dmaBuffer2 = NULL; }

    if (adcRegistered)
    {
        WD_Release_Card (adcCard);
        adcCard       = -1;
        adcRegistered = 0;
        adcConfigured = 0;
    }

    SetCtrlVal       (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Released");
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, TABPANEL_2_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);
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

    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
    return 0;
}
