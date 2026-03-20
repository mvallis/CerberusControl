/*===========================================================================
 * DeviceControl_Full.c
 *
 * CVI/LabWindows application for controlling:
 *   1. KMTronic 2-Channel USB Relay  (serial / VISA)
 *   2. Siglent SPD3303X DC Power Supply (USBTMC / VISA)
 *   3. AD9914 DDS via custom USB interface board (COM port)
 *   4. ADLINK PCI-9846H ADC digitiser (WD-DASK) - stubs only, pending rebuild
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
 *  CALLBACKS  -  ADC TAB  (stubs - ADC acquisition pending rebuild)
 *===========================================================================*/
int CVICALLBACK AdcRegisterCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcConfigureCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcStartCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcStopCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcSingleShotCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcReleaseCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcSaveCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcRecordCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;
    SetCtrlVal (adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, "ADC: Not yet implemented");
    return 0;
}

int CVICALLBACK AdcPollTimerCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    return 0;
}
