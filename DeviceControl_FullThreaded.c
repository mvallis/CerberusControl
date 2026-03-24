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

    ScanVISAResources ();

    DisplayPanel (mainPanel);
    RunUserInterface ();

    /* Cleanup */
    ADC_Cleanup ();
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

    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CLOCK_MHZ,     &clockMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_HMC_DIV,       &hmcDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_PROG_DIV,      &progDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SAMPS_PER_CHI, &sampsPerChirp);

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
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SYNC_CLK,      syncClkMHz);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ADC_CLK,       adcClkMHz);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TRIG_FREQ,     trigFreqHz);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DRCTRL_PERIOD, drctrlPeriod_us);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CHIRP_STEPS,   chirpSteps);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CALC_PERIOD,   calcPeriod_us);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_TIME,     deadTime_us);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_SAMPLES,  deadSamples);
    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_MIN_PROG_DIV,  minProgDiv);

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

    if (ddsSweepActive) { dds_powerdown (); ddsSweepActive = 0; }
    deinit_dds ();
    ddsConnected = 0;

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

    if (!dds_reset ())
    { SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Reset FAILED"); return 0; }

    if (!ad9914_calibrate_dac ())
    { SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "DAC cal FAILED"); return 0; }

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

        if (!dds_reset ())   { return 0; }
        if (!dds_powerup ()) { return 0; }
        Delay(0.01);
        if (!ad9914_calibrate_dac ()) { return 0; }
        if (!ad9914_single_tone (cwFreq, &actCW)) { return 0; }
        if (!dds_update ()) { return 0; }

        ddsSweepActive = 1;
        lastActualPeriod_us = 0.0;

        sprintf (msg, "CW output: %.6f MHz (requested %.6f MHz)", actCW, cwFreq);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, msg);

        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_START,  actCW);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_STOP,   0.0);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_PERIOD, 0.0);
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

        if (!dds_reset ())   { return 0; }
        if (!dds_powerup ()) { return 0; }
        Delay(0.01);
        if (!ad9914_calibrate_dac ()) { return 0; }

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

        sprintf (msg, "%s sweep  (%.6f to %.6f MHz, %.3f us)",
                 (chirpMode == 0) ? "Triggered" : "Free-run",
                 actStart, actStop, actPeriod);
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, msg);
    }

    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START, ATTR_DIMMED, 1);
    SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,  ATTR_DIMMED, 0);

    return 0;
}

int CVICALLBACK DdsStopCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    if (ev != EVENT_COMMIT) return 0;

    dds_powerdown ();
    ddsSweepActive = 0;

    SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Sweep stopped");
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
 * Provisional UIR control IDs for FFT window + zero-pad rings
 *
 * In the CVI User Interface Editor, add two ring controls to the ADC tab:
 *   1. Name: ADC_RING_WINDOW    Label: "FFT Window"
 *      Items (label → value):  Rectangular→0, Hann→1, Hamming→2,
 *                               Blackman-Harris→3, Blackman→5, Flat Top→6
 *   2. Name: ADC_RING_ZEROPAD  Label: "Zero Pad"
 *      Items (label → value):  None(1x)→0, 2x→1, 4x→2, 8x→3, 16x→4,
 *                               32x→5, 64x→6, 128x→7, 256x→8
 *
 * CVI will regenerate DeviceControl_FullThreaded.h with the real #define
 * values (ADC_TAB_ADC_RING_WINDOW, ADC_TAB_ADC_RING_ZEROPAD).
 * REMOVE the two provisional lines below once that is done.
 *---------------------------------------------------------------------------*/
#ifndef ADC_TAB_ADC_RING_WINDOW
#define ADC_TAB_ADC_RING_WINDOW   23   /* PROVISIONAL — remove after UIR edit */
#endif
#ifndef ADC_TAB_ADC_RING_ZEROPAD
#define ADC_TAB_ADC_RING_ZEROPAD  24   /* PROVISIONAL — remove after UIR edit */
#endif

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
#define SAVE_TOFILE  2                  /* WD_AI_AsyncDblBufferToFile (driver-managed)*/

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
static HGLOBAL hDmaBuffer1  = NULL;   /* GlobalAlloc handles — needed for GlobalUnfix */
static HGLOBAL hDmaBuffer2  = NULL;
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
static void             ADC_WriteSidecarHeader (const char *baseName, int mode);
static void             ADC_StopAcquisition (void);
static int              ADC_StartCommon     (int mode, U32 reTrgCnt);

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
    GetSystemDate (&month, &day, &year);
    GetSystemTime (&hours, &minutes, &seconds);
    snprintf (buf, bufLen, "CerberusData_%04d%02d%02d_%02d%02d%02d",
              year, month, day, hours, minutes, seconds);
}

/*---------------------------------------------------------------------------
 * ADC_WriteSidecarHeader — write a .hdr text file alongside the .dat file
 *
 *   Captures all user-defined configuration at the moment recording starts:
 *   ADC settings, DDS sweep parameters, timing / divider chain, and the
 *   save mode used.  baseName is the path without any extension.
 *---------------------------------------------------------------------------*/
static void ADC_WriteSidecarHeader (const char *baseName, int mode)
{
    char hdrPath[512];
    FILE *hdr;
    int  month, day, year, hours, minutes, seconds;

    /* --- ADC UI values --- */
    int    timebase, impedance, range, sampsPerTrig;

    /* --- DDS UI values --- */
    double clockMHz, startF, stopF, period, cwFreq;
    int    hmcDiv, progDiv, sampsPerChirp, chirpMode;

    /* --- DDS actual / computed values --- */
    double actStart, actStop, actPeriod;
    double syncClkMHz, adcClkMHz, trigFreqHz, drctrlPeriod;
    int    chirpSteps, deadSamples, minProgDiv;
    double calcPeriod, deadTime;

    snprintf (hdrPath, sizeof (hdrPath), "%s.hdr", baseName);
    hdr = fopen (hdrPath, "w");
    if (!hdr) return;

    /* Timestamp */
    GetSystemDate (&month, &day, &year);
    GetSystemTime (&hours, &minutes, &seconds);
    fprintf (hdr, "[Timestamp]\n");
    fprintf (hdr, "Date = %04d-%02d-%02d\n", year, month, day);
    fprintf (hdr, "Time = %02d:%02d:%02d\n\n", hours, minutes, seconds);

    /* Save mode */
    fprintf (hdr, "[SaveMode]\n");
    fprintf (hdr, "Mode = %s\n\n",
             (mode == SAVE_THREAD) ? "SAVE_THREAD" : "SAVE_TOFILE");

    /* ---- ADC configuration ---- */
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_TIMEBASE,     &timebase);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_IMPEDANCE,    &impedance);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_RING_RANGE,        &range);
    GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_SAMP_PER_TRIG, &sampsPerTrig);

    if (range < 0 || range >= ADC_RANGE_TABLE_LEN)
        range = ADC_RANGE_TABLE_LEN - 1;

    fprintf (hdr, "[ADC]\n");
    fprintf (hdr, "NumChannels       = %u\n", ADC_NUM_CH);
    fprintf (hdr, "HalfBufSamples    = %u\n", HALF_BUF_SAMPLES);
    fprintf (hdr, "ScansPerHalf      = %u\n", SCANS_PER_HALF);
    fprintf (hdr, "SampsPerTrig      = %d\n", sampsPerTrig);
    fprintf (hdr, "RangeIndex        = %d\n", range);
    fprintf (hdr, "VoltageRange_V    = %.3f\n", ADC_RangeToVolts (adcRangeTable[range]));
    fprintf (hdr, "TimebaseIndex     = %d\n", timebase);
    fprintf (hdr, "ImpedanceIndex    = %d\n", impedance);
    fprintf (hdr, "DataType          = U16\n\n");

    /* ---- DDS configuration ---- */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_RING_CHIRP_MODE,    &chirpMode);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CLOCK_MHZ,      &clockMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_START_FREQ,     &startF);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_STOP_FREQ,      &stopF);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_PERIOD,         &period);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CW_FREQ,        &cwFreq);

    fprintf (hdr, "[DDS_Settings]\n");
    fprintf (hdr, "ChirpMode         = %d\n", chirpMode);
    fprintf (hdr, "ClockMHz          = %.6f\n", clockMHz);
    fprintf (hdr, "StartFreqMHz      = %.6f\n", startF);
    fprintf (hdr, "StopFreqMHz       = %.6f\n", stopF);
    fprintf (hdr, "RequestedPeriod_us= %.6f\n", period);
    fprintf (hdr, "CW_FreqMHz        = %.6f\n\n", cwFreq);

    /* ---- DDS actual values (set by last DdsStartCB) ---- */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_START,      &actStart);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_STOP,       &actStop);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ACT_PERIOD,     &actPeriod);

    fprintf (hdr, "[DDS_Actual]\n");
    fprintf (hdr, "ActualStartMHz    = %.6f\n", actStart);
    fprintf (hdr, "ActualStopMHz     = %.6f\n", actStop);
    fprintf (hdr, "ActualPeriod_us   = %.6f\n\n", actPeriod);

    /* ---- Divider / timing chain ---- */
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_HMC_DIV,        &hmcDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_PROG_DIV,       &progDiv);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SAMPS_PER_CHI,  &sampsPerChirp);

    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_SYNC_CLK,       &syncClkMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_ADC_CLK,        &adcClkMHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_TRIG_FREQ,      &trigFreqHz);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DRCTRL_PERIOD,  &drctrlPeriod);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CHIRP_STEPS,    &chirpSteps);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_CALC_PERIOD,    &calcPeriod);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_TIME,      &deadTime);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_DEAD_SAMPLES,   &deadSamples);
    GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_NUM_MIN_PROG_DIV,   &minProgDiv);

    fprintf (hdr, "[Timing]\n");
    fprintf (hdr, "HMC432_Divider    = %d\n", hmcDiv);
    fprintf (hdr, "ProgDivider       = %d\n", progDiv);
    fprintf (hdr, "SampsPerChirp     = %d\n", sampsPerChirp);
    fprintf (hdr, "SyncClkMHz        = %.6f\n", syncClkMHz);
    fprintf (hdr, "ADC_ClkMHz        = %.6f\n", adcClkMHz);
    fprintf (hdr, "TrigFreqHz        = %.3f\n", trigFreqHz);
    fprintf (hdr, "DRCTRL_Period_us  = %.6f\n", drctrlPeriod);
    fprintf (hdr, "ChirpSteps        = %d\n", chirpSteps);
    fprintf (hdr, "CalcChirpPeriod_us= %.6f\n", calcPeriod);
    fprintf (hdr, "DeadTime_us       = %.6f\n", deadTime);
    fprintf (hdr, "DeadSamples       = %d\n", deadSamples);
    fprintf (hdr, "MinProgDivider    = %d\n", minProgDiv);

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
 *   FFT output is in dBV (peak), where 0 dBV = 1 V peak amplitude.
 *   A full-scale ±5 V sine reads ≈ +14 dBV; ±1 V sine reads ≈ 0 dBV.
 *   Formula:  corrected_dB = 20·log10(|X|) − 20·log10(N/2) + window_correction_dB
 *   where window_correction_dB = −20·log10(coherentgain)  from GetWinProperties().
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
        int ph;
        DeleteGraphPlot (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, -1, VAL_DELAYED_DRAW);
        ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, ch0, (int)scans,
                    VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
        SetPlotAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, ph,
                          ATTR_PLOT_LG_TEXT, "LRX (CH0)");
        ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_TIME, ch1, (int)scans,
                    VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_BLUE);
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

        /* Amplitude correction (dBV, peak, one-sided spectrum):
             correction_dB = window_correction - 20·log10(N/2)
           N is the number of real signal samples (scans), not the padded length,
           because the signal energy comes from scans points.                      */
        GetWinProperties (windowType, &winProps);
        window_correction_dB = (winProps.coherentgain > 0.0)
                                ? -20.0 * log10 (winProps.coherentgain)
                                :  0.0;
        correction_dB = window_correction_dB - 20.0 * log10 ((double)(scans / 2)); // Needs the additional values later once the radar characterisation has been completed!
		// Additional terms are:... 

        /* --- CH0 (LRX): window → zero-pad → FFTEx → magnitude → dBV --- */
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

        /* ---- Plot FFT magnitude (positive-frequency half, halfPadN bins) ---- */
        {
            int ph;
            DeleteGraphPlot (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, -1, VAL_DELAYED_DRAW);
            ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, fftMag0, (int)halfPadN,
                        VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
            SetPlotAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, "LRX (CH0)");
            ph = PlotY (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, fftMag1, (int)halfPadN,
                        VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_BLUE);
            SetPlotAttribute (adcTabHandle, ADC_TAB_ADC_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, "URX (CH1)");

            /* Mirror to master tab */
            DeleteGraphPlot (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, -1, VAL_DELAYED_DRAW);
            ph = PlotY (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, fftMag0, (int)halfPadN,
                        VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
            SetPlotAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, "LRX (CH0)");
            ph = PlotY (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, fftMag1, (int)halfPadN,
                        VAL_DOUBLE, VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_BLUE);
            SetPlotAttribute (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, ph,
                              ATTR_PLOT_LG_TEXT, "URX (CH1)");
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

    /* Configure trigger before enabling double-buffer mode (matches reference sample order):
       POST trigger, external digital, positive edge */
	// Hardcoded currently rather than using ring values...
    err = WD_AI_Trig_Config (adcCard,
                             WD_AI_TRGMOD_POST,
                             WD_AI_TRGSRC_ExtD,
                             WD_AI_TrgPositive,
                             0, 0.0, 0, 0, 0,
                             (U32)reTrgCnt); // Is the reTrgCnt correct, should it be 1 for DB mode?
    if (err != NoError)
    {
        snprintf (msg, sizeof(msg), "ADC: Trig_Config failed (%d)", (int)err);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
        return -1;
    }

    /* Safety: ensure no stale buffer registrations remain from a previous run
       or from WD_AD_Auto_Calibration_ALL (which may register internal buffers).
       Without this, ContBufferSetup below can return -201. */
    WD_AI_ContBufferReset (adcCard);

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

    /* Register both DMA half-buffers — save buf1's ID separately */
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
static volatile int masterSetupPending = 0;  /* auto-configure after cal */

static int CVICALLBACK AdcCalThread (void *data)
{
    adcCalResult = WD_AD_Auto_Calibration_ALL (adcCard);

    /* Do NOT call WD_AI_ContBufferReset here — no buffers are registered at
       this point and calling it can crash the driver.  The safety
       ContBufferReset at the top of ADC_StartCommon handles any stale state
       that calibration may leave behind. */

    PostDeferredCall ((DeferredCallbackPtr)ADC_CalDoneDeferred, NULL);
    return 0;
}

static void CVICALLBACK ADC_CalDoneDeferred (void *data)
{
    adcCalibrating = 0;

    /* Calibration writes new ADC constants — any prior AI_Config / CH_Config
       state references stale calibration data.  Force re-configure so the
       channel setup uses the new constants. */
    adcConfigured = 0;

    if (adcCalResult == NoError)
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS,
                    "ADC: Calibration OK — please re-Configure");
    else
    {
        char msg[128];
        snprintf (msg, sizeof(msg), "ADC: Calibration failed (%d)", (int)adcCalResult);
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
    }

    /* Re-enable Configure/Release/Calibrate; keep acquisition buttons dimmed
       since adcConfigured is now 0 — user must re-configure first. */
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 0);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);

    /* If master setup sequence triggered this calibration, auto-configure */
    if (masterSetupPending && adcCalResult == NoError)
    {
        masterSetupPending = 0;
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "ADC: Setup — configuring...");
        /* Trigger configure — reuse AdcConfigureCB */
        AdcConfigureCB (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE,
                        EVENT_COMMIT, NULL, 0, 0);
        if (adcConfigured)
        {
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "ADC: Setup complete — ready");
            SetCtrlAttribute (masterTabHandle,
                              MASTER_TAB_MASTER_BTN_ACQ_START,  ATTR_DIMMED, 0);
            SetCtrlAttribute (masterTabHandle,
                              MASTER_TAB_MASTER_BTN_ACQ_RECORD, ATTR_DIMMED, 0);
            /* Enable the master update timer */
            SetCtrlAttribute (masterTabHandle,
                              MASTER_TAB_MASTER_TIMER_UPDATE, ATTR_ENABLED, 1);
        }
        else
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                        "ADC: Setup — configure failed");
        SetCtrlAttribute (masterTabHandle,
                          MASTER_TAB_MASTER_BTN_ACQ_SETUP, ATTR_DIMMED, 0);
    }
    else
    {
        masterSetupPending = 0;
        SetCtrlAttribute (masterTabHandle,
                          MASTER_TAB_MASTER_BTN_ACQ_SETUP, ATTR_DIMMED, 0);
    }
}

int CVICALLBACK AdcCalibrateCB (int p, int c, int ev, void *cbd, int e1, int e2)
{
    CmtThreadFunctionID calThreadID;

    if (ev != EVENT_COMMIT) return 0;

    if (!adcRegistered)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Register first");
        return 0;
    }
    if (isAcquiring || adcCalibrating)
    {
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Busy");
        return 0;
    }

    adcCalibrating = 1;
    SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Calibrating...");

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
    if (impedance)
    {
        for (i = 0; i < (int)ADC_NUM_CH; i++)
        {
            err = WD_AI_CH_ChangeParam (adcCard, (U16)i, AI_IMPEDANCE, 1);
            if (err != NoError)
            {
                snprintf (msg, sizeof(msg),
                          "ADC: CH_ChangeParam impedance ch%d failed (%d)", i, (int)err);
                SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, msg);
                return 0;
            }
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

    ADC_WriteSidecarHeader (recordPath, SAVE_THREAD);
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

    ADC_WriteSidecarHeader (recordPath, SAVE_TOFILE);
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
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_START_ACQ, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RECORD,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SAVE,      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_SINGLE,    ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_STOP_ACQ,  ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);
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
                      relay1State ? "Shields: ON" : "Shields: OFF");

    /* ---- DDS LED ---- */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_DDS, ddsSweepActive);
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_DDS,
                      ATTR_LABEL_TEXT,
                      ddsSweepActive ? "Weapons: ON" : "Weapons: OFF");

    /* ---- ADC LED ---- */
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_LED_ADC,
                adcConfigured ? 1 : 0);

    /* ---- Status messages ---- */
    {
        char ddsMsg[128], adcMsg[128];
        GetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, ddsMsg);
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_DDS_STATUS, ddsMsg);
        if (isAcquiring)
            snprintf (adcMsg, sizeof(adcMsg),
                      "Acquiring — HR:%u OVR:%u",
                      (unsigned)diagHalfReady, (unsigned)diagDmaOverrun);
        else if (adcConfigured)
            strcpy (adcMsg, "Acq. Status: Ready");
        else if (adcRegistered)
            strcpy (adcMsg, "Acq. Status: Registered");
        else
            strcpy (adcMsg, "Acq. Status: Offline");
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS, adcMsg);
    }

    /* ---- Power status label ---- */
    if (psuSession != VI_NULL)
        SetCtrlVal (masterTabHandle, MASTER_TAB_STATUS_LABEL,
                    "Power Status: Online");
    else
        SetCtrlVal (masterTabHandle, MASTER_TAB_STATUS_LABEL,
                    "Power Status: Offline");

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
        /* Stop — delegate to existing DdsStopCB logic */
        dds_powerdown ();
        ddsSweepActive = 0;
        SetCtrlVal (ddsTabHandle, DDS_TAB_DDS_MSG_STATUS, "Sweep stopped");
        SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_START, ATTR_DIMMED, 0);
        SetCtrlAttribute (ddsTabHandle, DDS_TAB_DDS_BTN_STOP,  ATTR_DIMMED, 1);
    }
    else
    {
        /* Start — trigger DdsStartCB which reads params from the DDS tab */
        DdsStartCB (ddsTabHandle, DDS_TAB_DDS_BTN_START,
                    EVENT_COMMIT, NULL, 0, 0);
    }
    return 0;
}

/*---------------------------------------------------------------------------
 * MasterAcqSetupCB — one-button: Register → Calibrate → Configure
 *   Calibration runs in a background thread; configure is deferred to
 *   ADC_CalDoneDeferred via a flag so it happens after cal completes.
 *---------------------------------------------------------------------------*/
static int CVICALLBACK MasterCalThread (void *data)
{
    adcCalResult = WD_AD_Auto_Calibration_ALL (adcCard);
    PostDeferredCall ((DeferredCallbackPtr)ADC_CalDoneDeferred, NULL);
    return 0;
}

int CVICALLBACK MasterAcqSetupCB (int p, int c, int ev, void *cbd,
                                  int e1, int e2)
{
    int cardNum;
    I16 handle;
    char msg[128];

    if (ev != EVENT_COMMIT) return 0;

    if (isAcquiring || adcCalibrating)
    {
        SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                    "ADC: Busy");
        return 0;
    }

    /* Step 1: Register (if not already) */
    if (!adcRegistered)
    {
        GetCtrlVal (adcTabHandle, ADC_TAB_ADC_NUM_CARD_NUM, &cardNum);
        handle = WD_Register_Card (PCI_9846H, (U16)cardNum);
        if (handle < 0)
        {
            snprintf (msg, sizeof(msg), "ADC: Register failed (%d)",
                      (int)handle);
            SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS, msg);
            return 0;
        }
        adcCard       = handle;
        adcRegistered = 1;
        adcConfigured = 0;
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_STATUS, "ADC: Registered OK");
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 0);
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 0);
        SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 0);
    }

    /* Step 2: Calibrate (background thread) — set flag so cal-done
       callback will automatically run Configure afterwards */
    masterSetupPending = 1;
    adcCalibrating = 1;
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "ADC: Setup — calibrating...");
    SetCtrlAttribute (masterTabHandle, MASTER_TAB_MASTER_BTN_ACQ_SETUP,
                      ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CONFIGURE, ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_RELEASE,   ATTR_DIMMED, 1);
    SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_BTN_CALIBRATE, ATTR_DIMMED, 1);

    {
        CmtThreadFunctionID calID;
        CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
                                       MasterCalThread, NULL, &calID);
    }
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
                    "ADC: Acquiring (monitor)");
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
                    "ADC: Cannot open output file");
        return 0;
    }

    if (ADC_StartCommon (SAVE_THREAD, RETRIG_CNT_INF) < 0)
    {
        fclose (recordFile); recordFile = NULL;
        return 0;
    }

    ADC_WriteSidecarHeader (recordPath, SAVE_THREAD);
    SetCtrlVal (adcTabHandle,    ADC_TAB_ADC_MSG_STATUS,
                "ADC: Recording (thread)");
    SetCtrlVal (masterTabHandle, MASTER_TAB_MASTER_MSG_ADC_STATUS,
                "ADC: Recording");
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
                "ADC: Stopped");
    return 0;
}
