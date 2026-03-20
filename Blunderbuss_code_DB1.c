/*
User inputs the required Scancount,
number of bursts (triggers),
and how many channels (default 2, works with 1 too - use channels 0 & 1)

Scan to aquire data
Plot Graph 1 plots channel 0 data
Plot Graph 2 plots channel 1 data
Write to file gives to option of writing to a file, of users choice, both data streams separated by a  gap
*/

// setup parameters
#include <ansi_c.h> 
#include <cvirte.h>
#include <userint.h>
#include "Blunderbuss_code_DB1.h"
#include <windows.h>
#include <analysis.h>
#include <utility.h>
#include <ansi_c.h>
#include <stdio.h>
#include <Stdlib.h>
#include <math.h>
#include "wd-Dask64.h"
#include "wdDaskex.h"
																			  
#define CHANNELNUMBER All_Channels
#define MAXCHANNELCOUNT 4				 // 	property of card - 4 channels - only need 2 max
//#define TIMEBASE      WD_IntTimeBase	 //		WD_ExtTimeBase for external clock - 	internal timer is   WD_IntTimeBase
#define TIMEBASE      WD_ExtTimeBase	 //		WD_ExtTimeBase for external clock - 	internal timer is   WD_IntTimeBase
#define ADTRIGSRC     WD_AI_TRGSRC_ExtD  //     WD_AI_TRGSRC_ANA - analog trigger pin	 software=WD_AI_TRGSRC_SOFT			WD_AI_TRGSRC_ExtD - external digital
#define ADTRIGMODE    WD_AI_TRGMOD_POST  //		trigger control settings - POST is default
#define ADTRIGPOL     WD_AI_TrgNegative  //		positive/negative edge trigger
#define BUFAUTORESET  1
#define SCAN_INTERVAL 2				 //	ADC division factor,	page 33 of handbook has details
#define size 256				 // number of data points per chirp 
#define RE1 2048						 // number of chirps in each batch
#define arraySize1 size*RE1				 // = size*RE1, limit see below...

double timeBaseValue = 3.125;               // set the value of the time base - in MHz
double time_array[arraySize1];    				// time values 
static U16 ai_buf[arraySize1*2];			 		// doesn't matter if too big, as long as >SCANCOUNT*RE
static double voltage_array_Rx1[arraySize1];    	// scaled voltage values
static double voltage_array_Rx2[arraySize1];
double Im_Rx1[arraySize1];							// first set of imaginary values
double Im_Rx2[arraySize1];
double freq1[arraySize1/1];						// set of frequency values 
double dB_Rx1[arraySize1/1];   						// set of power values for fast time FFT
double dB_Rx2[arraySize1/1];
double magnitude_Rx1[arraySize1/1]; 					// set of magnitude values for fast time FFT
double magnitude_Rx2[arraySize1/1];
double dBslow_Rx1[size/2][RE1/2];						// set of power values for slow time FFT
double dBslow_Rx2[size/2][RE1/2];
double VoltSlow_Rx1[size/2][RE1/2]; 					// set of voltage values that are operated on
double VoltSlow_Rx2[size/2][RE1/2];
double ImSlow_Rx1[size/2][RE1/2];						// set of Im values that are operated on
double ImSlow_Rx2[size/2][RE1/2];
double voltsRelevant_Rx1[arraySize1/1];				// relevant FFT data - Hilbert Transform
double voltsRelevant_Rx2[arraySize1/1];
double ImRelevant_Rx1[arraySize1/1];				// relevant FFT data - Hilbert Transform
double ImRelevant_Rx2[arraySize1/1];
double voltsFast_Rx1[arraySize1/1];					// set of voltage values that are operated on
double voltsFast_Rx2[arraySize1/1];
double ImFast_Rx1[arraySize1/1];						// set of Im values that are operated on
double ImFast_Rx2[arraySize1/1];
double magnitudeSlow_Rx1[size/2][RE1/2];				// set of magnitude values for slow time FFT
double magnitudeSlow_Rx2[size/2][RE1/2];
double rangeDoppler_Rx1[size/2][RE1];				// 2D array for the range doppler values
double rangeDoppler_Rx2[size/2][RE1];
double PowerFastFFT_Rx1[size/2][RE1];				// 2D array for the fast time FFT power values (plotted against time/chirp number) 
double PowerFastFFT_Rx2[size/2][RE1];
double PhaseArray2D_Rx1[size/2][RE1];				// 2D array for the phase of the sample signal
double PhaseArray2D_Rx2[size/2][RE1];

#define MAXCOLORS   		256							// Max no. colour levels in image (256)
static int					numberofcolors[1];			// Number of colors varies with choice of map
ColorMapEntry				ColorMap[1][MAXCOLORS];		// Define color map entry as 2D choice of color maps
ColorMapEntry				ColorMap1[1][MAXCOLORS];		// Define color map entry as 2D choice of color maps
void 	SetPPIColourMap(void);							 // set up colour maps 
void 	SetPPIColourMap1(void);

//double timeBaseValue = 3.125;               // set the value of the time base - in MHz  
//double timeBaseValue = 9.939236068;               // set the value of the time base - in MHz  
long SCANCOUNT=size;				    // set by user min is 80 max is set to 5000
long RE = RE1;							// set by user min is 1 max is set to 1000  // combined has to be < 827000 for some reason???? seems arbitrary number
static int chcnt=2;							// set by user 1-4
long totscan;			 				// minimum  of 80 - must be an even number - upper limit???????
LARGE_INTEGER freq, start_count, current_count;
FILE *svfile;
DAS_IOT_DEV_PROP cardProp;
BOOLEAN fStop=0;
static U16 ai_range=0;
static I16 card=-1;
static I16 err;
static I16 card_num=0;
static U16 card_type=0;
static int panel;
void show_channel_data(U16 *buf);
int keep_going; 
double timeT; 
double step; 
double sampling_rate; 
int windowchoice;
double correction_dB;				// FFT scaling correction factor
double correction_dB_Slow;
double window_gain_correction;	// Window function coherent gain
double window_gain_correction_Slow;
WindowConst constants;			// Structure containing window parameters 
int	window_type;				// FFT window type, from GUI


// Declarations and initializations corresponding to multithreading
static int ctrl=0; static int ready=0;
static int CVICALLBACK Data_Acquisition_Thread (void *ctrlID);
static int CVICALLBACK Data_Split_Thread (void *ctrlID);
static int CVICALLBACK Data_Processing_Thread (void *ctrlID);
static int CVICALLBACK Data_Saving_Thread (void *ctrlID); 
CmtTSQHandle queue_process;
CmtTSQHandle queue_save;
CmtTSQHandle queue_save_freq;
static CmtThreadLockHandle Lock;
static double buffer_process[arraySize1*2];
static double buffer_save[arraySize1*2];
static double save_freq_buffer[arraySize1*2];
FILE *ATM;
								

int main (int argc, char *argv[])
{
	if (InitCVIRTE (0, argv, 0) == 0)
		return -1;	/* out of memory */
	if ((panel = LoadPanel (0, "Blunderbuss_code_DB1.uir", PANEL)) < 0)
		return -1;

	//Initialise card & calibrate - causes clicking from the card

	card_type = PCI_9846H;
	if((card = WD_Register_Card(card_type, card_num))<0) /* Set for H temporarily, for BB24 PC it is a 9846D */
	{
		printf("Error %d trying to open device\n", GetLastError());
		exit(1);
	}
	err = WD_AD_Auto_Calibration_ALL(card);
	if(err != NoError)
	{
		printf("\nAuto Calibration Failed %d.\n", err);
	}
	DisplayPanel (panel);
	RunUserInterface ();
	DiscardPanel (panel);
	WD_Release_Card(card);   	// Deinitialise - one click
	
	return 0;
}

int CVICALLBACK quit (int panel, int control, int event,
					  void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
			ctrl=0;
			CmtDiscardTSQ (&queue_process);
	        CmtDiscardTSQ (&queue_save);
			CmtDiscardTSQ (&queue_save_freq);
			QuitUserInterface(0);
			break;
	}
	return 0;
}


//Pressing the scan button starts data acquisition, processing and saving threads
int CVICALLBACK scan (int panel, int control, int event,
		void *callbackData, int eventData1, int eventData2)
{
	switch (event)
		{
		case EVENT_COMMIT:
			SetCtrlAttribute (panel, PANEL_SCAN, ATTR_DIMMED, 1);
			CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
								   Data_Acquisition_Thread, NULL, NULL);
			CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
								   Data_Split_Thread, NULL, NULL);
	        CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
								   Data_Processing_Thread, NULL, NULL);
			CmtScheduleThreadPoolFunction (DEFAULT_THREAD_POOL_HANDLE,
								   Data_Saving_Thread, NULL, NULL);
			ctrl=1;
			break;
		}
	return 0;
}


//Pressing stop button halts all the thread operations
int CVICALLBACK StopAcquisition (int panel, int control, int event,
		void *callbackData, int eventData1, int eventData2)
{
	switch (event)
		{
		case EVENT_COMMIT:
			SetCtrlAttribute (panel, PANEL_SCAN, ATTR_DIMMED, 0);
			ctrl=0;
			CmtDiscardTSQ (&queue_process);
	        CmtDiscardTSQ (&queue_save);
			CmtDiscardTSQ (&queue_save_freq);
			break;
		}
	return 0;
}


//Data Acquisition Thread
static int CVICALLBACK Data_Acquisition_Thread (void *ctrlID)
{
	        I16 err;
			U16 Id;
			BOOLEAN fStop;
			U32 count=0, startPos=0;
			totscan=RE*SCANCOUNT*chcnt;
			
			long arraySize = RE*size;
			
	//Function declarations for thread safe queue operations, both for data processing and saving
	CmtNewTSQ (arraySize1*32, sizeof(U16), OPT_TSQ_AUTO_FLUSH_ALL, &queue_process);
	CmtNewTSQ (arraySize1*32, sizeof(U16),OPT_TSQ_AUTO_FLUSH_ALL, &queue_save);
	//CmtInstallTSQCallback (queue_process, EVENT_TSQ_ITEMS_IN_QUEUE, arraySize1*2, Data_Processing_Thread, 0, CmtGetCurrentThreadID(), NULL);
	//CmtInstallTSQCallback (queue_save, EVENT_TSQ_ITEMS_IN_QUEUE, arraySize1*2, Data_Saving_Thread, 0, CmtGetCurrentThreadID(), NULL);
	// initialize card 
			WD_GetDeviceProperties (card, 0, &cardProp);
				ai_range = 2; 								                                           // only works for 2  and 10 V (+/-1 and +/-5)
				
				err = WD_AI_CH_Config (card, CHANNELNUMBER, AD_B_1_V);	                               // AI range error  - Informs the WD-DASK library of the AI range selected for the specified channel of the card with card ID CardNumber
				if (err!=NoError)										    
				{
					printf("WD_AI_CH_Config error=%d", err);
					exit(1);
				}
				
				//err = WD_AI_CH_ChangeParam(card, 0, AI_IMPEDANCE, IMPEDANCE_HI);
			if (err!=0)
				{
					printf("WD_AI_Config error=%d", err);
					exit(1);
				}
				
				err = WD_AI_Config (card, TIMEBASE, 1, WD_AI_ADCONVSRC_TimePacer, 0, BUFAUTORESET);    // Informs the WD-DASK library of the timebase source, conversion source, and sampling mode
				if (err!=0)
				{
					printf("WD_AI_Config error=%d", err);
					exit(1);
				}		
			
			// while loop for continuous collection
			while (ctrl){
			CmtGetLock(Lock);
			ready=0;
			CmtReleaseLock(Lock);
			fStop=0;	
				
			
				
				// TRIGGER 
				
		//		err = WD_AI_Set_Mode (card, DAQSTEPPED, 1);	   											// Configures the advanced mode of continuous AI operation
				

			
			// loop for data collection 
				
				err = WD_AI_Trig_Config (card, ADTRIGMODE, ADTRIGSRC, ADTRIGPOL, 0, 0.0, 0, 0, 0, RE);  // trigger set up -
				if (err!=0)
				{
					printf("WD_AI_Trig_Config error=%d", err);
					exit(1);
				}
				
				err=WD_AI_ContBufferSetup (card, ai_buf, arraySize*2, &Id);
				if (err!=0)
				{
					printf("WD_AI_ContBufferSetup 0 error=%d", err);
				
					exit(1);
				}
				
				
				err = WD_AI_ContScanChannels (card, (chcnt-1), Id, arraySize, SCAN_INTERVAL, SCAN_INTERVAL, ASYNCH_OP);  // Performs continuous A/D conversions
				//err = WD_AI_ContReadMultiChannels (card, chcnt, channel, Id, arraySize, SCAN_INTERVAL, SCAN_INTERVAL, ASYNCH_OP);
				if (err!=0)
				{
					printf("AI_ContScanChannels error=%d", err);
					exit(1);
				}
				
				//DAQ conversion stopped
			//	WD_AI_DMA_Transfer (card, Id);			  		// Transfers the acquired data from the onboard SDRAM or DDR2 to the specified buffer.
				do
				{
					WD_AI_AsyncCheck(card, &fStop, &count);		// Determines the current status of the asynchronous analog input operation.
				}
				
				while (!fStop);
				//ProcessSystemEvents();			
				WD_AI_AsyncClear(card, &startPos, &count);
		//	SetCtrlVal (panel, PANEL_NUMERIC, 2);	
				//Voltage conversion and saving data chunk in to the thread safe queue buffer
			//	F64 vol = 0.0;
			//	long s;
			//	totscan = arraySize*2;
				
				
			//	for(s=0;s<arraySize*2; s++){
			//	WD_AI_VoltScale (card, AD_B_1_V, ai_buf[s], &vol);
			//	buffer_process[s]=vol;
			//	buffer_save[s]=vol;
			//	}
			
			//	CmtWriteTSQData (queue_process, ai_buf, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
			//    CmtWriteTSQData (queue_save, ai_buf, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
			CmtGetLock(Lock);
			ready=1;
			CmtReleaseLock(Lock);
			}
	
	return 0;
}
static int CVICALLBACK Data_Split_Thread (void *ctrlID){
	while(ctrl){
		if(ready){
			CmtWriteTSQData (queue_process, ai_buf, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
			CmtWriteTSQData (queue_save, ai_buf, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
			
		}
	}
	return 0;
}


//Data Processing and display thread
static int CVICALLBACK Data_Processing_Thread (void *ctrlID){
	static U16 data_process[arraySize1*2];
	long arraySize = RE*size;
	
   	long a; 
	long n; 
	sampling_rate = (timeBaseValue)/(SCAN_INTERVAL);
	timeT = size/(sampling_rate); 
	step = timeT/size;
	for (a = 0; a < RE; a++) { 
		for (n = 0; n < size; n++) { 
				 time_array[ n + (a*size) ] = n*step; 
		   }
		} 
				
	double freqT = 1/step; 
	double fstep = freqT/size; 
	for (a = 0; a < RE; a++) {
		for (n = 0; n < size; n++) { 
				freq1[ n + (a*(size)) ] = n*fstep; 
		}
	}
				
	// dB correction - fast time  
	//GetCtrlVal(panel,PANEL_WINDOW_RING,&window_type);
	//GetWinProperties (window_type, &constants);
	//window_gain_correction = -20*log10(constants.coherentgain);
	//correction_dB =  -3 -10*log10(50) +30 -20*log10(size/2) + window_gain_correction;
	//correction_dB_Slow = -20*log10(RE) + window_gain_correction + correction_dB; 
	
	//CmtNewTSQ (arraySize1*32, sizeof(double),OPT_TSQ_AUTO_FLUSH_ALL, &queue_save_freq);
	double tempArrayV_Rx1[size];
	double tempArrayV_Rx2[size]; 
	NIComplexNumber tempArrayV_Rx1out[size];
	NIComplexNumber tempArrayV_Rx2out[size];
    NIComplexNumber tempArrayVslow_Rx1[RE/2];
	NIComplexNumber tempArrayVslow_Rx1out[RE/2];
	NIComplexNumber tempArrayVslow_Rx2[RE/2];
	NIComplexNumber tempArrayVslow_Rx2out[RE/2];
	PFFTTable tbl1=CreateFFTTable(size);
	PFFTTable tbl2=CreateFFTTable(RE/2);
	
	while (ctrl){
		
			// set to MHz and microseconds 

		
			 CmtReadTSQData (queue_process, data_process, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
			 	
			 int s=0; int m=0; int mm=0;
				
				for(s=0; s<arraySize*2; s++){
					if (s % 2 == 0){
					voltage_array_Rx1[m]= data_process[s] -  32768.0;
					m=m+1;
					}
					else{
					voltage_array_Rx2[mm]= data_process[s] -  32768.0;
					mm=mm+1;
					}
				}
				
								// delete all graphs 
			//	DeleteGraphPlot (panel, PANEL_PHASE2D_Rx1, -1, VAL_DELAYED_DRAW);
			//	DeleteGraphPlot (panel, PANEL_PHASE2D_Rx2, -1, VAL_DELAYED_DRAW); 
			//	DeleteGraphPlot (panel, PANEL_PHASE_Rx1, -1, VAL_DELAYED_DRAW);
			//	DeleteGraphPlot (panel, PANEL_PHASE_Rx2, -1, VAL_DELAYED_DRAW);
			//	DeleteGraphPlot (panel,PANEL_GRAPH, -1, VAL_DELAYED_DRAW);
				DeleteGraphPlot (panel,PANEL_GRAPH2_Rx1, -1, VAL_DELAYED_DRAW);
				DeleteGraphPlot (panel,PANEL_GRAPH2_Rx2, -1, VAL_DELAYED_DRAW);
			//	DeleteGraphPlot (panel,PANEL_GRAPH3_Rx1, -1, VAL_DELAYED_DRAW);
			//	DeleteGraphPlot (panel,PANEL_GRAPH3_Rx2, -1, VAL_DELAYED_DRAW);
				DeleteGraphPlot (panel, PANEL_CROSS_2DPLOT_Rx1, -1, VAL_DELAYED_DRAW);
				DeleteGraphPlot (panel, PANEL_CROSS_2DPLOT_Rx2, -1, VAL_DELAYED_DRAW); 
				
			//	PlotXY (panel, PANEL_GRAPH, time_array, voltage_array_Rx1, arraySize/RE1, VAL_DOUBLE, VAL_DOUBLE, VAL_FAT_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_RED);
			//	PlotXY (panel, PANEL_GRAPH, time_array, voltage_array_Rx2,arraySize/RE1, VAL_DOUBLE, VAL_DOUBLE, VAL_FAT_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_YELLOW);
				//PlotY (panel, PANEL_GRAPH, voltage_array_real, arraySize, VAL_DOUBLE,VAL_THIN_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
				
				
				GetCtrlVal (panel, PANEL_WINDOW_RING, &windowchoice);
				GetWinProperties (windowchoice, &constants);
	            window_gain_correction = -20*log10(constants.coherentgain);
	            correction_dB =  -3 -10*log10(50) +30 -20*log10(size/2) + window_gain_correction - 90.3090;
	            correction_dB_Slow = -20*log10(RE) + window_gain_correction + correction_dB;
				
			// fast time FFT 
				long i; 
				
				// FFT alters voltage_array and Im 
				/*double tempArrayV_Rx1[size];
				double tempArrayV_Rx2[size]; */
				/*double tempArrayIm_Rx1[size];
				double tempArrayIm_Rx2[size];*/
				for (a = 0; a < RE/2; a++) {
					// set arrays to the next set of samples  
					for (i = 0; i < size; i++ ) {
      					tempArrayV_Rx1[ i ] = voltage_array_Rx1[ i + (a*size) ];
						tempArrayV_Rx2[ i ] = voltage_array_Rx2[ i + (a*size) ];
					}
					/*for (i = 0; i < size; i++ ) {
      					tempArrayIm_Rx1[ i ] = 0;
						tempArrayIm_Rx2[ i ] = 0;
					}*/
					// apply window 
					if (windowchoice == 6) { 
						FlatTopWin (tempArrayV_Rx1, size);
						FlatTopWin (tempArrayV_Rx2, size);
					} else if (windowchoice == 1) {
						HanWin (tempArrayV_Rx1, size);
						HanWin (tempArrayV_Rx2, size);
					} else if (windowchoice == 5) {
							BkmanWin (tempArrayV_Rx1, size);
							BkmanWin (tempArrayV_Rx2, size);
					} else if (windowchoice == 3) {
						BlkHarrisWin (tempArrayV_Rx1, size);
						BlkHarrisWin (tempArrayV_Rx2, size);
					}
					FFTEx(tempArrayV_Rx1,size,size,tbl1,FALSE,tempArrayV_Rx1out);
					FFTEx(tempArrayV_Rx2,size,size,tbl1,FALSE,tempArrayV_Rx2out);
					for (i = 0; i < size; i++ ) {
      					voltsRelevant_Rx1[ i + (a*size) ] = tempArrayV_Rx1out[ i ].real;
						voltsRelevant_Rx2[ i + (a*size) ] = tempArrayV_Rx2out[ i ].real;
						ImRelevant_Rx1[ i + (a*size) ] = tempArrayV_Rx1out[ i ].imaginary;
						ImRelevant_Rx2[ i + (a*size) ] = tempArrayV_Rx2out[ i ].imaginary;
					}
					/*for (i = 0; i < size; i++ ) {
      					Im_Rx1[ i + (a*size) ] = tempArrayIm_Rx1[ i ];
						Im_Rx2[ i + (a*size) ] = tempArrayIm_Rx2[ i ];
					}*/
				} 

				/* save FFT outputs of relevant data */
				
				/*for (a = 0; a < RE; a++) {
					for (i = 0; i < size/2; i++ ) {
      					voltsRelevant_Rx1[i + a*(size/2)] = voltage_array_Rx1[ i + a*size ];
						voltsRelevant_Rx2[i + a*(size/2)] = voltage_array_Rx2[ i + a*size ];
					}
				}
				
				for (a = 0; a < RE; a++) {
					for (i = 0; i < size/2; i++ ) {
      					ImRelevant_Rx1[i + a*(size/2)] = Im_Rx1[ i + a*size ];
						ImRelevant_Rx2[i + a*(size/2)] = Im_Rx2[ i + a*size ];
					}
				}*/
			// convert to dB 

				// square the array 
				for ( i = 0; i < arraySize/RE/2; i++ ) { 
					//voltsFast_Rx1[ i ] = pow(voltsRelevant_Rx1[ i ], 2.0);
					//voltsFast_Rx2[ i ] = pow(voltsRelevant_Rx2[ i ], 2.0);
					magnitude_Rx1[ i ] = pow(pow(voltsRelevant_Rx1[ i ], 2.0)+pow(ImRelevant_Rx1[ i ], 2.0), 0.5);
					magnitude_Rx2[ i ] = pow(pow(voltsRelevant_Rx2[ i ], 2.0)+pow(ImRelevant_Rx2[ i ], 2.0), 0.5);
					if (magnitude_Rx1[ i ] == 0) {  
						dB_Rx1[ i ] = -100; 
					} else { 
						dB_Rx1[ i ] = 20*log10(magnitude_Rx1[ i ]) + correction_dB;
					}
					if (magnitude_Rx2[ i ] == 0) {  
						dB_Rx2[ i ] = -100; 
					} else { 
						dB_Rx2[ i ] = 20*log10(magnitude_Rx2[ i ]) + correction_dB;
					}
				}
				/* square Im array */
				//for ( i = 0; i < arraySize/2; i++ ) {
      			//	ImFast_Rx1[ i ] = pow(ImRelevant_Rx1[ i ], 2.0);
				//	ImFast_Rx2[ i ] = pow(ImRelevant_Rx2[ i ], 2.0);
				//}
				///* add both arrays/matrices squares to give magnitude squared */
				//for (i = 0; i < arraySize/2; i++) { 
				//	 magnitude_Rx1[ i ] = voltsFast_Rx1[ i ] + ImFast_Rx1[ i ];
				//	 magnitude_Rx1[ i ] = pow(magnitude_Rx1[ i ], 0.5);
				//	 magnitude_Rx2[ i ] = voltsFast_Rx2[ i ] + ImFast_Rx2[ i ];
				//	 magnitude_Rx2[ i ] = pow(magnitude_Rx2[ i ], 0.5);
				//} 
				//for (i = 0; i < arraySize/2; i++) { 
				//	if (magnitude_Rx1[ i ] == 0) {  
				//		dB_Rx1[ i ] = -123; 
				//	} else { 
				//		dB_Rx1[ i ] = 20*log10(magnitude_Rx1[ i ]) + correction_dB;
				//	}
				//	if (magnitude_Rx2[ i ] == 0) {  
				//		dB_Rx2[ i ] = -123; 
				//	} else { 
				//		dB_Rx2[ i ] = 20*log10(magnitude_Rx2[ i ]) + correction_dB;
				//	}
				//}
				
				/*for (i = 0; i<arraySize/2; i++) {
					save_freq_buffer [(4*i)+0] = voltsRelevant_Rx1[i];
					save_freq_buffer [(4*i)+1] = ImRelevant_Rx1[i];
					save_freq_buffer [(4*i)+2] = voltsRelevant_Rx2[i];
					save_freq_buffer [(4*i)+3] = ImRelevant_Rx2[i];
				}*/
				
			//	CmtWriteTSQData (queue_save_freq, save_freq_buffer, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
				
				PlotXY (panel, PANEL_GRAPH2_Rx1, freq1, dB_Rx1, size/2, VAL_DOUBLE,
						VAL_DOUBLE, VAL_FAT_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_GREEN);
				PlotXY (panel, PANEL_GRAPH2_Rx2, freq1, dB_Rx2, size/2, VAL_DOUBLE,
						VAL_DOUBLE, VAL_FAT_LINE, VAL_NO_POINT, VAL_SOLID, 1, VAL_GREEN);
			    //PlotY (panel, PANEL_GRAPH2, dB, arraySize, VAL_DOUBLE, VAL_FAT_LINE, VAL_EMPTY_SQUARE, VAL_SOLID, 1, VAL_RED);
				
				
				// phase array - calculate and display  
				/*double PhaseArray_Rx1[arraySize/2];
				double PhaseArray_Rx2[arraySize/2]; 
				for (i = 0; i < arraySize/2; i++ ) {
      					PhaseArray_Rx1[ i ] = atan(ImRelevant_Rx1[ i ]/voltsRelevant_Rx1[ i ]);
						PhaseArray_Rx2[ i ] = atan(ImRelevant_Rx2[ i ]/voltsRelevant_Rx2[ i ]);
				}
				
				PlotXY (panel, PANEL_PHASE_Rx1, freq1, PhaseArray_Rx1, arraySize/2,
						VAL_DOUBLE, VAL_DOUBLE, VAL_SCATTER, VAL_SIMPLE_DOT,
						VAL_SOLID, 1, VAL_GREEN);
				PlotXY (panel, PANEL_PHASE_Rx2, freq1, PhaseArray_Rx2, arraySize/2,
						VAL_DOUBLE, VAL_DOUBLE, VAL_SCATTER, VAL_SIMPLE_DOT,
						VAL_SOLID, 1, VAL_GREEN);*/
				
				// 2D array of power vs slow time 
				/*for (a = 0; a < RE; a++ ) {
					for ( i = 0; i < size/2; i++ ) { 
						PowerFastFFT_Rx1[ i ][ a ] = dB_Rx1[i + a*(size/2)];
						PowerFastFFT_Rx2[ i ][ a ] = dB_Rx2[i + a*(size/2)];
					}
				}
				
				// 2D array of phase vs slow time 
				for (a = 0; a < RE; a++ ) {
					for ( i = 0; i < size/2; i++ ) { 
						PhaseArray2D_Rx1[ i ][ a ] = PhaseArray_Rx1[i + a*(size/2)];
						PhaseArray2D_Rx2[ i ][ a ] = PhaseArray_Rx2[i + a*(size/2)];
					}
				}
				
				SetPPIColourMap();
				
				PlotScaledIntensity (panel, PANEL_GRAPH3_Rx1, PowerFastFFT_Rx1, RE, size/2, VAL_DOUBLE, 1, 0.0, 1, 0.0, ColorMap[0], VAL_BLACK, numberofcolors[0], 1, 0);
					PlotScaledIntensity (panel, PANEL_GRAPH3_Rx2, PowerFastFFT_Rx2, RE, size/2, VAL_DOUBLE, 1, 0.0, 1, 0.0, ColorMap[0], VAL_BLACK, numberofcolors[0], 1, 0);
				
				SetPPIColourMap1();
				
				PlotScaledIntensity (panel, PANEL_PHASE2D_Rx1, PhaseArray2D_Rx1, RE, size/2, VAL_DOUBLE, 1, 0.0, 1, 0.0, ColorMap1[0], VAL_BLACK, numberofcolors[0], 1, 0);
				PlotScaledIntensity (panel, PANEL_PHASE2D_Rx2, PhaseArray2D_Rx2, RE, size/2, VAL_DOUBLE, 1, 0.0, 1, 0.0, ColorMap1[0], VAL_BLACK, numberofcolors[0], 1, 0);
				
			// do FFT in the slow time domain 
				
			// dB correction - slow time  
				GetCtrlVal(panel,PANEL_WINDOW_RING,&window_type);
				GetWinProperties (window_type, &constants);
				window_gain_correction_Slow = -20*log10(constants.coherentgain); 
				correction_dB_Slow = -20*log10(RE) + window_gain_correction_Slow + correction_dB;*/
				
				// set up 2D array's  
				for (a = 0; a < RE/2; a++ ) {
					for (i = 0; i < size/2; i++) {
						VoltSlow_Rx1[i][a] = voltsRelevant_Rx1[ i + (a*(size)) ];
						ImSlow_Rx1[i][a] = ImRelevant_Rx1[ i + (a*(size)) ];
						VoltSlow_Rx2[i][a] = voltsRelevant_Rx2[ i + (a*(size)) ];
						ImSlow_Rx2[i][a] = ImRelevant_Rx2[ i + (a*(size)) ]; 
					} 
				} 
				
			// do FFT's in slow time 
				/*double tempArrayVslow_Rx1[RE]; 
				double tempArrayImslow_Rx1[RE];
				double tempArrayVslow_Rx2[RE]; 
				double tempArrayImslow_Rx2[RE];*/
				for (a = 0; a < size/2; a++) {
					// set arrays to the next set of chirp data points 
					for (i = 0; i < RE/2; i++ ) {
      					tempArrayVslow_Rx1[ i ].real = VoltSlow_Rx1[ a ][ i ]; 
						tempArrayVslow_Rx1[ i ].imaginary = ImSlow_Rx1[ a ][ i ];
						tempArrayVslow_Rx2[ i ].real = VoltSlow_Rx2[ a ][ i ]; 
						tempArrayVslow_Rx2[ i ].imaginary = ImSlow_Rx2[ a ][ i ];
					}
					// apply window 
					if (windowchoice == 6) { 
						CxFlatTopWin (tempArrayVslow_Rx1, RE/2);
						CxFlatTopWin (tempArrayVslow_Rx2, RE/2);
					} else if (windowchoice == 1) {
						CxHanWin (tempArrayVslow_Rx1, RE/2);
						CxHanWin (tempArrayVslow_Rx2, RE/2);
					} else if (windowchoice == 5) {
							CxBkmanWin (tempArrayVslow_Rx1, RE/2);
							CxBkmanWin (tempArrayVslow_Rx2, RE);
					} else if (windowchoice == 3) {
						CxBlkHarrisWin (tempArrayVslow_Rx1, RE/2);
						CxBlkHarrisWin (tempArrayVslow_Rx2, RE/2);
					}
					/*if (windowchoice == 6) { 
						FlatTopWin (tempArrayImslow_Rx1, RE);
						FlatTopWin (tempArrayImslow_Rx2, RE);
					} else if (windowchoice == 1) {
						HanWin (tempArrayImslow_Rx1, RE);
						HanWin (tempArrayImslow_Rx2, RE);
					} else if (windowchoice == 5) {
							BkmanWin (tempArrayImslow_Rx1, RE);
							BkmanWin (tempArrayImslow_Rx2, RE);
					} else if (windowchoice == 3) {
						BlkHarrisWin (tempArrayImslow_Rx1, RE);
						BlkHarrisWin (tempArrayImslow_Rx2, RE);
					}*/
					CxFFTEx(tempArrayVslow_Rx1,RE/2,RE/2,tbl2,TRUE,tempArrayVslow_Rx1out);
					CxFFTEx(tempArrayVslow_Rx2,RE/2,RE/2,tbl2,TRUE,tempArrayVslow_Rx2out);
					for (i = 0; i < RE/2; i++ ) {
      					VoltSlow_Rx1[a][i] = tempArrayVslow_Rx1out[ i ].real;
						ImSlow_Rx1[a][i] = tempArrayVslow_Rx1out[ i ].imaginary;
						VoltSlow_Rx2[a][i] = tempArrayVslow_Rx2out[ i ].real;
						ImSlow_Rx2[a][i] = tempArrayVslow_Rx2out[ i ].imaginary;
					}
				}
				
			// get dB's in slow time  
															 
				// square the array 
				for (a = 0; a < RE/2; a++ ) {
					for ( i = 0; i < size/2; i++ ) {
						magnitudeSlow_Rx1[ i ][ a ] = pow(pow(VoltSlow_Rx1[ i ][ a ], 2.0)+pow(ImSlow_Rx1[ i ][ a ], 2.0), 0.5);
						magnitudeSlow_Rx2[ i ][ a ] = pow(pow(VoltSlow_Rx2[ i ][ a ], 2.0)+pow(ImSlow_Rx2[ i ][ a ], 2.0), 0.5);

						if (magnitudeSlow_Rx1[ i ][ a ] == 0) {  
							dBslow_Rx1[ i ][ a ] = -90; 
						} else { 
							dBslow_Rx1[ i ][ a ] = 20*log10(magnitudeSlow_Rx1[ i ][ a ]) + correction_dB_Slow;
						}
						
						if (magnitudeSlow_Rx2[ i ][ a ] == 0) {  
							dBslow_Rx2[ i ][ a ] = -90; 
						} else { 
							dBslow_Rx2[ i ][ a ] = 20*log10(magnitudeSlow_Rx2[ i ][ a ]) + correction_dB_Slow;
						}
					}
				}
				/* square Im array */
				/*for (a = 0; a < RE; a++ ) {
					for ( i = 0; i < size/2; i++ ) { 
						ImSlow_Rx1[ i ][ a ] = pow(ImSlow_Rx1[ i ][ a ], 2.0);
						ImSlow_Rx2[ i ][ a ] = pow(ImSlow_Rx2[ i ][ a ], 2.0);
					}
				}*/
				/* add both arrays/matrices squares to give magnitude squared */
				/*for (a = 0; a < RE; a++ ) {
					for ( i = 0; i < size/2; i++ ) { 
						magnitudeSlow_Rx1[ i ][ a ] = VoltSlow_Rx1[ i ][ a ] + ImSlow_Rx1[ i ][ a ];
						magnitudeSlow_Rx1[ i ][ a ] = pow(magnitudeSlow_Rx1[ i ][ a ], 0.5);
						magnitudeSlow_Rx2[ i ][ a ] = VoltSlow_Rx2[ i ][ a ] + ImSlow_Rx2[ i ][ a ];
						magnitudeSlow_Rx2[ i ][ a ] = pow(magnitudeSlow_Rx2[ i ][ a ], 0.5);
					}
				}
				for (a = 0; a < RE; a++ ) {
					for ( i = 0; i < size/2; i++ ) {  
						if (magnitudeSlow_Rx1[ i ][ a ] == 0) {  
							dBslow_Rx1[ i ][ a ] = -90; 
						} else { 
							dBslow_Rx1[ i ][ a ] = 20*log10(magnitudeSlow_Rx1[ i ][ a ]) + correction_dB_Slow;
						}
						
						if (magnitudeSlow_Rx2[ i ][ a ] == 0) {  
							dBslow_Rx2[ i ][ a ] = -90; 
						} else { 
							dBslow_Rx2[ i ][ a ] = 20*log10(magnitudeSlow_Rx2[ i ][ a ]) + correction_dB_Slow;
						}
					}
				}*/
				
				// range doppler plot data - stored in these arrays  
				/*for (a = RE/2; a < RE; a++) {
					for (i = 0; i < size/2; i++) { 
				 		rangeDoppler_Rx1[i][a - RE/2] = dBslow_Rx1[ i ][ a ];
						rangeDoppler_Rx2[i][a - RE/2] = dBslow_Rx2[ i ][ a ]; 
					}
				}
				for (a = 0; a < RE/2; a++) {
					for (i = 0; i < size/2; i++) { 
				 		rangeDoppler_Rx1[i][a + RE/2] = dBslow_Rx1[ i ][ a ];
						rangeDoppler_Rx2[i][a + RE/2] = dBslow_Rx2[ i ][ a ];
					}
				}*/
				
			//	PlotIntensity (panel, PANEL_CROSS_2DPLOT, rangeDoppler, RE, size, VAL_DOUBLE, ColorMap[0], VAL_BLACK, numberofcolors[0], 1, 0);
				SetPPIColourMap();
				PlotScaledIntensity (panel, PANEL_CROSS_2DPLOT_Rx1, dBslow_Rx1, RE/2, size/2, VAL_DOUBLE, 1, 0.0, 1, 0.0, ColorMap[0], VAL_BLACK, numberofcolors[0], 1, 0);
				PlotScaledIntensity (panel, PANEL_CROSS_2DPLOT_Rx2, dBslow_Rx2, RE/2, size/2, VAL_DOUBLE, 1, 0.0, 1, 0.0, ColorMap[0], VAL_BLACK, numberofcolors[0], 1, 0);
				
	}
	DestroyFFTTable(tbl1);
	DestroyFFTTable(tbl2);
	return 0;
}



//Data Saving Thread
static int CVICALLBACK Data_Saving_Thread (void *ctrlID)
{
int saverange;
static U16 data_save[arraySize1*2];
static double data_save_freq[arraySize1*2];
static double data_save_freq_userdefinedrange[arraySize1*2]; 
int save = 0;
int saveformat = 0;
int startrangebin;
int stoprangebin;
int saveflag = 0;
int s, ss, sss;
int	month, day, year;
int	hours, minutes, seconds;

char savedir[35];

	while(ctrl){
	GetCtrlVal (panel, PANEL_SAVE, &save);
	if (save ==1){
		if (saveflag == 0){
		          GetSystemDate (&month, &day, &year);
	              GetSystemTime (&hours, &minutes, &seconds);
                  sprintf (savedir, "%d-%02d-%02d_%02d-%02d-%02d_24GHz_FMCW.dat", 
						   year, month, day, hours, minutes, seconds);
				  ATM= fopen(savedir, "wb");
				  saveflag=1;
		//		  GetCtrlVal (panel, PANEL_SAVE_FORMAT, &saveformat);
		//		  GetCtrlVal (panel, PANEL_START_RANGE_BIN, &startrangebin);
		//		  GetCtrlVal (panel, PANEL_STOP_RANGE_BIN, &stoprangebin);
				  saverange= ((stoprangebin - startrangebin +1) * RE) *4;
				  SetCtrlAttribute (panel, PANEL_WINDOW_RING, ATTR_DIMMED, 1);
				  SetCtrlAttribute (panel, PANEL_STOP, ATTR_DIMMED, 1);
				  SetCtrlAttribute (panel, PANEL_QUIT, ATTR_DIMMED, 1);
		//		  SetCtrlAttribute (panel, PANEL_SAVE_FORMAT, ATTR_DIMMED, 1);
		//		  SetCtrlAttribute (panel, PANEL_START_RANGE_BIN, ATTR_DIMMED, 1);
		//		  SetCtrlAttribute (panel, PANEL_STOP_RANGE_BIN, ATTR_DIMMED, 1);
				  
		}

		             // Saving data either in time series or frequency mode according to user specification
					 if (saveformat == 0){
					 CmtReadTSQData (queue_save, data_save, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
					 fwrite(&data_save, sizeof(U16), arraySize1*2, ATM);
					 }
					 else{
					   CmtReadTSQData (queue_save_freq, data_save_freq, arraySize1*2, TSQ_INFINITE_TIMEOUT, 0);
					   sss=0;
					   for (s = 0; s < RE; s++ ) {
					     for ( ss = startrangebin-1; ss < stoprangebin; ss++ ) { 
						    data_save_freq_userdefinedrange [sss] = data_save_freq [(ss*4) + (s*4*(size/2))];
							data_save_freq_userdefinedrange [sss+1] = data_save_freq [((ss*4)+1) + (s*4*(size/2))];
							data_save_freq_userdefinedrange [sss+2] = data_save_freq [((ss*4)+2) + (s*4*(size/2))];
							data_save_freq_userdefinedrange [sss+3] = data_save_freq [((ss*4)+3) + (s*4*(size/2))];
							sss += 4;
					     }
				       }
					   fwrite(&data_save_freq_userdefinedrange, sizeof(double), saverange, ATM); 
					 }
					 //fprintf(ATM, "\n");
				  //}
	}
				  if (save == 0 && saveflag == 1){
				  fclose(ATM);
				  saveflag=0;
				  SetCtrlAttribute (panel, PANEL_WINDOW_RING, ATTR_DIMMED, 0);
				  SetCtrlAttribute (panel, PANEL_STOP, ATTR_DIMMED, 0);
				  SetCtrlAttribute (panel, PANEL_QUIT, ATTR_DIMMED, 0);
				 // SetCtrlAttribute (panel, PANEL_SAVE_FORMAT, ATTR_DIMMED, 0);
				 // SetCtrlAttribute (panel, PANEL_START_RANGE_BIN, ATTR_DIMMED, 0);
				 // SetCtrlAttribute (panel, PANEL_STOP_RANGE_BIN, ATTR_DIMMED, 0);
				  }
	
	}
	return 0;
}




void show_channel_data( U16 *buf)
{
	long  j, i;
	F64 vol = 0.0;
	printf(" >>>>>>>>>>>>>>> the valid scans  <<<<<<<<<<<<<<< \n");
	for(i=0; i<chcnt; i++)
	{
		printf("    Ch%d        ", i);
	}
	printf("\n");

	for(i=0; i<RE*SCANCOUNT; i=i+chcnt)
	{
		for(j=1; j<(chcnt+1); j++)
		{
			WD_AI_VoltScale (card, AD_B_1_V, ai_buf[(i+j)-1], &vol);
			printf("%4.4fV   ", vol);
		}
		printf("\n");
	}
}
			
// Function to define 2D plot colour map
void SetPPIColourMap(void)
{
	// Define color table for Greyscale map
	ColorMap[0][0].color = 0x000000;							 			// Black
	ColorMap[0][0].dataValue.valDouble = -100;		// Value in dBm
	ColorMap[0][1].color = 0xFFFFFF;							 			// White
	ColorMap[0][1].dataValue.valDouble = 0;		// Value in dBm

	// Define number of colors in each map
	numberofcolors[0] = 2;

}

// Function to define 2D plot colour map
void SetPPIColourMap1(void)
{
	// Define color table for Greyscale map
	ColorMap1[0][0].color = 0x000000;							 			// Black
	ColorMap1[0][0].dataValue.valDouble = -1.6;		// Value in radians
	ColorMap1[0][1].color = 0xFFFFFF;							 			// White
	ColorMap1[0][1].dataValue.valDouble = 1.6;		// Value in radians

	// Define number of colors in each map
	numberofcolors[0] = 2;

}







// plot graph of phase vs chirp number 
//int CVICALLBACK binGraph (int panel, int control, int event,
//							   void *callbackData, int eventData1, int eventData2)
//{
//	switch (event)
//	{
//		case EVENT_COMMIT:
//		// Bin Graph
//		DeleteGraphPlot (panel, PANEL_BIN_GRAPH_Rx1, -1, VAL_DELAYED_DRAW);
//		DeleteGraphPlot (panel, PANEL_BIN_GRAPH_Rx2, -1, VAL_DELAYED_DRAW);
//		int i;
//			int binNumber_Rx1;
//			int binNumber_Rx2;
//			GetCtrlVal(panel,PANEL_NUMERIC_Rx1,&binNumber_Rx1);
//			GetCtrlVal(panel,PANEL_NUMERIC_Rx2,&binNumber_Rx2); 
//			int chirps[RE]; 
//			for (i = 0; i < RE; i++) { 
//				chirps[i] = i + 1; 
//			} 
//			double phase_Rx1[RE];
//			double phase_Rx2[RE];
//			for (i = 0; i < RE; i++) { 																																					  
//				phase_Rx1[i] = PhaseArray2D_Rx1[binNumber_Rx1][i];
//				phase_Rx2[i] = PhaseArray2D_Rx2[binNumber_Rx2][i];
//			} 
//			PlotXY (panel, PANEL_BIN_GRAPH_Rx1, chirps, phase_Rx1, RE, VAL_INTEGER, VAL_DOUBLE, VAL_THIN_LINE, VAL_SOLID_SQUARE, VAL_SOLID, 1, VAL_GREEN);
//			PlotXY (panel, PANEL_BIN_GRAPH_Rx2, chirps, phase_Rx2, RE, VAL_INTEGER, VAL_DOUBLE, VAL_THIN_LINE, VAL_SOLID_SQUARE, VAL_SOLID, 1, VAL_GREEN);
//	}
//	return 0;
//}



// epdate cursor for power vs frequency plot  
int CVICALLBACK update_Graph2_Rx1_Cursor (int panel, int control, int event,
							   void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
		
		double freq_Rx1;
		double power_Rx1;
			
		GetGraphCursor ( panel,  PANEL_GRAPH2_Rx1, 1,  &freq_Rx1,  &power_Rx1);
				
		SetCtrlVal (panel, PANEL_NUMERIC_FREQ_Rx1, freq_Rx1);
		SetCtrlVal (panel, PANEL_NUMERIC_POWER_Rx1, power_Rx1);	
	}
	return 0;
}

int CVICALLBACK update_Graph2_Rx2_Cursor (int panel, int control, int event,
							   void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
		
		double freq_Rx2;
		double power_Rx2;
			
		GetGraphCursor ( panel,  PANEL_GRAPH2_Rx2, 1,  &freq_Rx2,  &power_Rx2);
				
		SetCtrlVal (panel, PANEL_NUMERIC_FREQ_Rx2, freq_Rx2);
		SetCtrlVal (panel, PANEL_NUMERIC_POWER_Rx2, power_Rx2);	
	}
	return 0;
}


// epdate cursor for power vs time plot  
//int CVICALLBACK update_Graph3_Rx1_Cursor (int panel, int control, int event,
//							   void *callbackData, int eventData1, int eventData2)
//{
//	switch (event)
//	{
//		case EVENT_COMMIT:
//		
//		double time_Rx1;
//		double power_Rx1; 
//			
//		GetGraphCursor ( panel,  PANEL_GRAPH3_Rx1, 1,  &time_Rx1,  &power_Rx1);
//				
//		SetCtrlVal (panel, PANEL_NUMERIC_TIME_Rx1, time_Rx1);
//		SetCtrlVal (panel, PANEL_NUMERIC_POWER2_Rx1, power_Rx1);	
//	}
//	return 0;
//}


//int CVICALLBACK update_Graph3_Rx2_Cursor (int panel, int control, int event,
//							   void *callbackData, int eventData1, int eventData2)
//{
//	switch (event)
//	{
//		case EVENT_COMMIT:
//		
//		double time_Rx2;
//		double power_Rx2; 
//			
//		GetGraphCursor ( panel,  PANEL_GRAPH3_Rx2, 1,  &time_Rx2,  &power_Rx2);
//				
//		SetCtrlVal (panel, PANEL_NUMERIC_TIME_Rx2, time_Rx2);
//		SetCtrlVal (panel, PANEL_NUMERIC_POWER2_Rx2, power_Rx2);	
//	}
//	return 0;
//}


// epdate cursor for Range Doppler plot 
int CVICALLBACK update_RangeDoppler_Rx1_Cursor (int panel, int control, int event,
							   void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
		
		double xBin_Rx1;
		double yBin_Rx1; 
			
		GetGraphCursor ( panel,  PANEL_CROSS_2DPLOT_Rx1, 1,  &xBin_Rx1,  &yBin_Rx1);
				
		SetCtrlVal (panel, PANEL_NUMERIC_XBINNUM_Rx1, xBin_Rx1);
		SetCtrlVal (panel, PANEL_NUMERIC_YBINNUM_Rx1, yBin_Rx1);
	}
	return 0;
}

int CVICALLBACK update_RangeDoppler_Rx2_Cursor (int panel, int control, int event,
							   void *callbackData, int eventData1, int eventData2)
{
	switch (event)
	{
		case EVENT_COMMIT:
		
		double xBin_Rx2;
		double yBin_Rx2; 
			
		GetGraphCursor ( panel,  PANEL_CROSS_2DPLOT_Rx2, 1,  &xBin_Rx2,  &yBin_Rx2);
				
		SetCtrlVal (panel, PANEL_NUMERIC_XBINNUM_Rx2, xBin_Rx2);
		SetCtrlVal (panel, PANEL_NUMERIC_YBINNUM_Rx2, yBin_Rx2);
	}
	return 0;
}


// epdate cursor for phase time plot 
//int CVICALLBACK update_PhaseTime_Rx1_Cursor (int panel, int control, int event,
//							   void *callbackData, int eventData1, int eventData2)
//{
//	switch (event)
//	{
//		case EVENT_COMMIT:
//		
//		double time_Rx1;
//		double phase_Rx1; 
//			
//		GetGraphCursor ( panel,  PANEL_PHASE2D_Rx1, 1,  &time_Rx1,  &phase_Rx1);
//				
//		SetCtrlVal (panel, PANEL_NUMERIC_TIME2_Rx1, time_Rx1);
//		SetCtrlVal (panel, PANEL_NUMERIC_PHASE_Rx1, phase_Rx1);	
//	}
//	return 0;
//}

//int CVICALLBACK update_PhaseTime_Rx2_Cursor (int panel, int control, int event,
//							   void *callbackData, int eventData1, int eventData2)
//{
//	switch (event)
//	{
//		case EVENT_COMMIT:
//		
//		double time_Rx2;
//		double phase_Rx2; 
//			
//		GetGraphCursor ( panel,  PANEL_PHASE2D_Rx2, 1,  &time_Rx2,  &phase_Rx2);
//				
//		SetCtrlVal (panel, PANEL_NUMERIC_TIME2_Rx2, time_Rx2);
//		SetCtrlVal (panel, PANEL_NUMERIC_PHASE_Rx2, phase_Rx2);	
//	}
//	return 0;
//}
