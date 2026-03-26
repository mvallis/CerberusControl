/**************************************************************************/
/* LabWindows/CVI User Interface Resource (UIR) Include File              */
/*                                                                        */
/* WARNING: Do not add to, delete from, or otherwise modify the contents  */
/*          of this include file.                                         */
/**************************************************************************/

#include <userint.h>

#ifdef __cplusplus
    extern "C" {
#endif

     /* Panels and Controls: */

#define  MAIN_PANEL                       1       /* callback function: MainPanelCB */
#define  MAIN_PANEL_MAIN_PANEL_TAB        2       /* control type: tab, callback function: (none) */

     /* tab page panel controls */
#define  ADC_TAB_ADC_NUM_CARD_NUM         2       /* control type: numeric, callback function: (none) */
#define  ADC_TAB_ADC_RING_TRIG_POL        3       /* control type: ring, callback function: (none) */
#define  ADC_TAB_ADC_RING_TRIG_SRC        4       /* control type: ring, callback function: (none) */
#define  ADC_TAB_ADC_RING_TIMEBASE        5       /* control type: ring, callback function: (none) */
#define  ADC_TAB_ADC_RING_IMPEDANCE       6       /* control type: ring, callback function: (none) */
#define  ADC_TAB_ADC_RING_RANGE           7       /* control type: ring, callback function: (none) */
#define  ADC_TAB_ADC_NUM_TRIGGERS         8       /* control type: numeric, callback function: (none) */
#define  ADC_TAB_ADC_NUM_SAMP_PER_TRIG    9       /* control type: numeric, callback function: (none) */
#define  ADC_TAB_ADC_BTN_RELEASE          10      /* control type: command, callback function: AdcReleaseCB */
#define  ADC_TAB_ADC_BTN_STOP_ACQ         11      /* control type: command, callback function: AdcStopCB */
#define  ADC_TAB_ADC_BTN_SINGLE           12      /* control type: command, callback function: AdcSingleShotCB */
#define  ADC_TAB_ADC_BTN_START_ACQ        13      /* control type: command, callback function: AdcStartCB */
#define  ADC_TAB_ADC_BTN_SAVE             14      /* control type: command, callback function: AdcSaveCB */
#define  ADC_TAB_ADC_BTN_CONFIGURE        15      /* control type: command, callback function: AdcConfigureCB */
#define  ADC_TAB_ADC_BTN_REGISTER         16      /* control type: command, callback function: AdcRegisterCB */
#define  ADC_TAB_ADC_GRAPH_TIME           17      /* control type: graph, callback function: (none) */
#define  ADC_TAB_ADC_GRAPH_FFT            18      /* control type: graph, callback function: (none) */
#define  ADC_TAB_ADC_MSG_STATUS           19      /* control type: textMsg, callback function: (none) */
#define  ADC_TAB_ADC_BTN_RECORD           20      /* control type: command, callback function: AdcRecordCB */
#define  ADC_TAB_ADC_TIMER_POLL           21      /* control type: timer, callback function: AdcPollTimerCB */
#define  ADC_TAB_ADC_BTN_CALIBRATE        22      /* control type: command, callback function: AdcCalibrateCB */
#define  ADC_TAB_ADC_RING_ZEROPAD         23      /* control type: ring, callback function: (none) */
#define  ADC_TAB_ADC_RING_WINDOW          24      /* control type: ring, callback function: (none) */

     /* tab page panel controls */
#define  DDS_TAB_DDS_NUM_CLOCK_MHZ        2       /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_BTN_INIT_CAL         3       /* control type: command, callback function: DdsInitCalCB */
#define  DDS_TAB_DDS_BTN_DISCONNECT       4       /* control type: command, callback function: DdsDisconnectCB */
#define  DDS_TAB_DDS_BTN_STOP             5       /* control type: command, callback function: DdsStopCB */
#define  DDS_TAB_DDS_BTN_START            6       /* control type: command, callback function: DdsStartCB */
#define  DDS_TAB_DDS_NUM_SAMPS_PER_CHI    7       /* control type: numeric, callback function: DdsDivRatioCB */
#define  DDS_TAB_DDS_NUM_PROG_DIV         8       /* control type: numeric, callback function: DdsDivRatioCB */
#define  DDS_TAB_DDS_NUM_HMC_DIV          9       /* control type: numeric, callback function: DdsDivRatioCB */
#define  DDS_TAB_DDS_BTN_CONNECT          10      /* control type: command, callback function: DdsConnectCB */
#define  DDS_TAB_DDS_NUM_PERIOD           11      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_STOP_FREQ        12      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_CW_FREQ          13      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_START_FREQ       14      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_ACT_PERIOD       15      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_ACT_STOP         16      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_MIN_PROG_DIV     17      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_DEAD_SAMPLES     18      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_DEAD_TIME        19      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_DRCTRL_PERIOD    20      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_TRIG_FREQ        21      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_ADC_CLK          22      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_CALC_PERIOD      23      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_CHIRP_STEPS      24      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_SYNC_CLK         25      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_NUM_ACT_START        26      /* control type: numeric, callback function: (none) */
#define  DDS_TAB_DDS_RING_CHIRP_MODE      27      /* control type: ring, callback function: (none) */
#define  DDS_TAB_DDS_MSG_STATUS           28      /* control type: textMsg, callback function: (none) */
#define  DDS_TAB_DDS_TIMING_TEXT          29      /* control type: textMsg, callback function: (none) */
#define  DDS_TAB_DDS_MSG_TIMING_WARN      30      /* control type: textMsg, callback function: (none) */
#define  DDS_TAB_DDS_RING_RESOURCE        31      /* control type: ring, callback function: (none) */

     /* tab page panel controls */
#define  MASTER_TAB_MASTER_NUM_CH2_CURR   2       /* control type: color, callback function: (none) */
#define  MASTER_TAB_MASTER_NUM_CH2_VOLT   3       /* control type: color, callback function: (none) */
#define  MASTER_TAB_MASTER_NUM_CH1_CURR   4       /* control type: color, callback function: (none) */
#define  MASTER_TAB_MASTER_NUM_CH1_VOLT   5       /* control type: color, callback function: (none) */
#define  MASTER_TAB_MASTER_CH2_METER      6       /* control type: scale, callback function: (none) */
#define  MASTER_TAB_MASTER_CH1_METER      7       /* control type: scale, callback function: (none) */
#define  MASTER_TAB_MASTER_GRAPH_FFT      8       /* control type: graph, callback function: (none) */
#define  MASTER_TAB_MASTER_BTN_DDS        9       /* control type: command, callback function: MasterDdsCB */
#define  MASTER_TAB_MASTER_BTN_TX_RELAY   10      /* control type: command, callback function: MasterTxRelayCB */
#define  MASTER_TAB_MASTER_BTN_ACQ_STOP   11      /* control type: command, callback function: MasterAcqStopCB */
#define  MASTER_TAB_MASTER_BTN_RESET_STAR 12      /* control type: command, callback function: MasterResetStartCB */
#define  MASTER_TAB_MASTER_BTN_ACQ_RECORD 13      /* control type: command, callback function: MasterAcqRecordCB */
#define  MASTER_TAB_MASTER_BTN_ACQ_START  14      /* control type: command, callback function: MasterAcqStartCB */
#define  MASTER_TAB_MASTER_BTN_ACQ_SETUP  15      /* control type: command, callback function: MasterAcqSetupCB */
#define  MASTER_TAB_MASTER_BTN_PSU_ALL    16      /* control type: command, callback function: MasterPsuAllCB */
#define  MASTER_TAB_MASTER_LED_TX_RELAY   17      /* control type: LED, callback function: (none) */
#define  MASTER_TAB_MASTER_LED_ADC        18      /* control type: LED, callback function: (none) */
#define  MASTER_TAB_MASTER_LED_DDS        19      /* control type: LED, callback function: (none) */
#define  MASTER_TAB_MASTER_LED_PSU        20      /* control type: LED, callback function: (none) */
#define  MASTER_TAB_STATUS_LABEL          21      /* control type: textMsg, callback function: (none) */
#define  MASTER_TAB_PSU_LABEL             22      /* control type: textMsg, callback function: (none) */
#define  MASTER_TAB_MAST_ADC_LABEL        23      /* control type: textMsg, callback function: (none) */
#define  MASTER_TAB_MASTER_LABEL          24      /* control type: textMsg, callback function: (none) */
#define  MASTER_TAB_MASTER_MSG_DDS_STATUS 25      /* control type: textMsg, callback function: (none) */
#define  MASTER_TAB_GRAPH_LABEL           26      /* control type: textMsg, callback function: (none) */
#define  MASTER_TAB_MASTER_MSG_ADC_STATUS 27      /* control type: textMsg, callback function: (none) */
#define  MASTER_TAB_MASTER_TIMER_UPDATE   28      /* control type: timer, callback function: MasterUpdateTimerCB */
#define  MASTER_TAB_MASTER_BTN_SEQUENCE   29      /* control type: command, callback function: MasterSequenceCB */
#define  MASTER_TAB_MASTER_LED_SAVING     30      /* control type: LED, callback function: (none) */
#define  MASTER_TAB_MASTER_TAB_MASTER_BTN 31      /* control type: command, callback function: MasterShutdownCB */

     /* tab page panel controls */
#define  PSU_TAB_PSU_RING_RESOURCE        2       /* control type: ring, callback function: (none) */
#define  PSU_TAB_PSU_BTN_CONNECT          3       /* control type: command, callback function: PsuConnectCB */
#define  PSU_TAB_PSU_BTN_DISCONNECT       4       /* control type: command, callback function: PsuDisconnect */
#define  PSU_TAB_PSU_BTN_REFRESH          5       /* control type: command, callback function: RefreshResCB */
#define  PSU_TAB_PSU_MSG_STATUS           6       /* control type: textMsg, callback function: (none) */
#define  PSU_TAB_PSU_MSG_IDN              7       /* control type: textMsg, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH2_CURR_SET     8       /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH2_VOLT_SET     9       /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_BTN_CH2_APPLY        10      /* control type: command, callback function: CH2ApplyCB */
#define  PSU_TAB_PSU_BTN_CH3_OUTPUT       11      /* control type: command, callback function: CH3OutCB */
#define  PSU_TAB_PSU_LED_CH3_OUTPUT       12      /* control type: LED, callback function: (none) */
#define  PSU_TAB_PSU_BTN_CH2_OUTPUT       13      /* control type: command, callback function: CH2OutCB */
#define  PSU_TAB_PSU_LED_CH2_OUTPUT       14      /* control type: LED, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH2_PWR_READ     15      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH2_CURR_READ    16      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH2_VOLT_READ    17      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH1_CURR_SET     18      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH1_VOLT_SET     19      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_BTN_CH1_APPLY        20      /* control type: command, callback function: CH1ApplyCB */
#define  PSU_TAB_PSU_BTN_CH1_OUTPUT       21      /* control type: command, callback function: CH1OutCB */
#define  PSU_TAB_PSU_LED_CH1_OUTPUT       22      /* control type: LED, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH1_PWR_READ     23      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH1_CURR_READ    24      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_NUM_CH1_VOLT_READ    25      /* control type: numeric, callback function: (none) */
#define  PSU_TAB_PSU_LBL_CH1_HEADER       26      /* control type: textMsg, callback function: (none) */
#define  PSU_TAB_PSU_LBL_CH2_HEADER_2     27      /* control type: textMsg, callback function: (none) */
#define  PSU_TAB_PSU_LBL_CH3_HEADER       28      /* control type: textMsg, callback function: (none) */
#define  PSU_TAB_PSU_TIMER_READBACK       29      /* control type: timer, callback function: ReadbackTimerCB */
#define  PSU_TAB_PSU_BTN_ALL_OUTPUT       30      /* control type: command, callback function: PsuAllOutCB */
#define  PSU_TAB_PSU_CH2_METER            31      /* control type: scale, callback function: (none) */
#define  PSU_TAB_PSU_CH1_METER            32      /* control type: scale, callback function: (none) */

     /* tab page panel controls */
#define  RELAY_TAB_RELAY_RING_RESOURCE    2       /* control type: ring, callback function: (none) */
#define  RELAY_TAB_RELAY_BTN_CONNECT      3       /* control type: command, callback function: RlConnectCB */
#define  RELAY_TAB_RELAY_BTN_DISCONNECT   4       /* control type: command, callback function: RlDisconnect */
#define  RELAY_TAB_RELAY_BTN_REFRESH      5       /* control type: command, callback function: RefreshResCB */
#define  RELAY_TAB_RELAY_BTN_RLY1         6       /* control type: command, callback function: Rl1CB */
#define  RELAY_TAB_RELAY_LED_RLY2         7       /* control type: LED, callback function: (none) */
#define  RELAY_TAB_RELAY_LED_RLY1         8       /* control type: LED, callback function: (none) */
#define  RELAY_TAB_RELAY_BTN_RLY2         9       /* control type: command, callback function: Rl2CB */
#define  RELAY_TAB_RELAY_MSG_STATUS       10      /* control type: textMsg, callback function: (none) */


     /* Control Arrays: */

          /* (no control arrays in the resource file) */


     /* Menu Bars, Menus, and Menu Items: */

          /* (no menu bars in the resource file) */


     /* Callback Prototypes: */

int  CVICALLBACK AdcCalibrateCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcConfigureCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcPollTimerCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcRecordCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcRegisterCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcReleaseCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcSaveCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcSingleShotCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcStartCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK AdcStopCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK CH1ApplyCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK CH1OutCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK CH2ApplyCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK CH2OutCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK CH3OutCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK DdsConnectCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK DdsDisconnectCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK DdsDivRatioCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK DdsInitCalCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK DdsStartCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK DdsStopCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MainPanelCB(int panel, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterAcqRecordCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterAcqSetupCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterAcqStartCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterAcqStopCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterDdsCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterPsuAllCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterResetStartCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterSequenceCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterShutdownCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterTxRelayCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK MasterUpdateTimerCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK PsuAllOutCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK PsuConnectCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK PsuDisconnect(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK ReadbackTimerCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK RefreshResCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK Rl1CB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK Rl2CB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK RlConnectCB(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);
int  CVICALLBACK RlDisconnect(int panel, int control, int event, void *callbackData, int eventData1, int eventData2);


#ifdef __cplusplus
    }
#endif