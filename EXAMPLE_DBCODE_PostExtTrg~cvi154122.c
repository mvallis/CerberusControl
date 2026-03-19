#include <windows.h>
#include "ansi_c.h"
#include <stdio.h>
#include "wd-dask.h"
#include "wddaskex.h"

/* Link WD-DASK library */
#pragma comment(lib, "WD-Dask.lib")

#define CHANNELNUMBER 0
#define SCANCOUNT     2000 // half buffer size in effect. Should be a large -ish number, any particular multiple rather than 2 (e.g. 8?)
#define TIMEBASE      WD_ExtTimeBase // Now external timebase for testing
#define BUFAUTORESET   1 
#define ADCONVERTSRC  WD_AI_ADCONVSRC_TimePacer
#define ADTRIGSRC     WD_AI_TRGSRC_ExtD
#define ADTRIGMODE    WD_AI_TRGMOD_POST
#define ADTRIGPOL     WD_AI_TrgNegative
#define SCAN_INTERVAL  1 // Set to not divide the timebase for intervals, or should this in fact be > 1, e.g. 2000 or 1024??
// e.g. if the trigger is 4 Hz, and the sample rate is 1 MHz, there should be some number of division so that each trigger results in multiple samples?
#define SAMPLE_INTERVAL  1 // Set to not divide timebase by any amount so just equal = 1


U16 ai_buf[SCANCOUNT];
U16 ai_buf2[SCANCOUNT];
FILE *svfile;
I16 card;
U16 ai_range=0;
DAS_IOT_DEV_PROP cardProp;

/* ===== Card Reset & Recovery Function ===== */
I16 ResetCard(I16 card_handle) {
    I16 err = 0;
    U32 dummy_count = 0, dummy_pos = 0;
    
    if (card_handle < 0) {
        printf("Card handle invalid, skipping reset\n");
        return -1;
    }
    
    printf("Resetting card state...\n");
    
    /* Step 1: Clear any pending async operations */
    err = WD_AI_AsyncClear(card_handle, &dummy_pos, &dummy_count);
    if (err != NoError) {
        fprintf(stderr, "AsyncClear error=%d\n", err);
    }
    
    /* Step 2: Release buffers (if any are registered) */
    WD_AI_ContBufferReset(card_handle);
    
    /* Step 3: Reconfigure from scratch */
    err = WD_AI_Config(card_handle, TIMEBASE, 1, WD_AI_ADCONVSRC_TimePacer, 0, BUFAUTORESET);
    if (err != NoError) {
        fprintf(stderr, "AI_Config error=%d\n", err);
        return err;
    }
    
    err = WD_AI_Trig_Config(card_handle, ADTRIGMODE, ADTRIGSRC, ADTRIGPOL, 0, 0.0, 0, 0, 0, 1);
    if (err != NoError) {
        fprintf(stderr, "Trig_Config error=%d\n", err);
        return err;
    }
    
    err = WD_AI_AsyncDblBufferMode(card_handle, 1);
    if (err != NoError) {
        fprintf(stderr, "AsyncDblBufferMode error=%d\n", err);
        return err;
    }
    
    printf("Card reset complete\n");
    return 0;
}

void write_to_file( U16 *buf, U32 write_count )
{
  /* Binary write: much faster than fprintf (10-100x improvement) */
  size_t written = fwrite(buf, sizeof(U16), write_count, svfile);
  if (written != write_count) {
    fprintf(stderr, "Write error: expected %u items, wrote %zu\n", write_count, written);
  }
  fflush(svfile);
}

/* Windows Console API replacement for conio.h kbhit() */
int CheckKeyPressed(void) {
    INPUT_RECORD inBuf;
    DWORD numRead = 0;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    
    if (!PeekConsoleInput(hStdin, &inBuf, 1, &numRead)) {
        return 0;
    }
    if (numRead == 0) return 0;
    if (inBuf.EventType == KEY_EVENT && inBuf.Event.KeyEvent.bKeyDown) {
        FlushConsoleInputBuffer(hStdin);  /* Clear the buffer */
        return 1;
    }
    ReadConsoleInput(hStdin, &inBuf, 1, &numRead);  /* Remove the processed event */
    return 0;
}

/* Windows Console API replacement for conio.h getch() */
int WaitForKeyPress(void) {
    INPUT_RECORD inBuf;
    DWORD numRead = 0;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    
    while (1) {
        ReadConsoleInput(hStdin, &inBuf, 1, &numRead);
        if (inBuf.EventType == KEY_EVENT && inBuf.Event.KeyEvent.bKeyDown) {
            return inBuf.Event.KeyEvent.uChar.AsciiChar;
        }
    }
}

int main(void)
{
    I16 err, card_num, tmpId = 0;
    U16 Id = 0;
    BOOLEAN halfReady, fStop, fok=0;
    U32 count=0, count1, startPos;
	U16 card_type = 1;
    int fok_input = 0;

    printf("This program inputs %d scans from CH-0.\n", SCANCOUNT);
	card_type = PCI_9846H;
	printf("Please input a card number: ");
    scanf(" %hd", &card_num);
	printf("Save data to a file (DmaData.dat): Yes(1) or No(0)? ");
	scanf(" %d", &fok_input);
    fok = (BOOLEAN)fok_input;
    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));  /* Clear keyboard buffer before acquisition */
    if ((card=WD_Register_Card (card_type, card_num)) <0 ) {
        printf("Register_Card error=%d", card);
        exit(1);
    }
	/* Perform startup reset to clear any residual card state */
	printf("Performing startup card reset...\n");
	ResetCard(card);
	printf("Startup reset complete. Proceeding with initialization.\n\n");
	
	WD_GetDeviceProperties (card, 0, &cardProp);
	ai_range = cardProp.default_range;
	err = WD_AI_CH_Config (card, CHANNELNUMBER, ai_range);
    if (err!=NoError) {
       printf("WD_AI_CH_Config error=%d", err);
       exit(1);
    }
	err = WD_AI_Config (card, TIMEBASE, 1, WD_AI_ADCONVSRC_TimePacer, 0, BUFAUTORESET);
    if (err!=0) {
       printf("WD_AI_Config error=%d", err);
       exit(1);
    }
    err = WD_AI_Trig_Config (card, ADTRIGMODE, ADTRIGSRC, ADTRIGPOL, 0, 0.0, 0, 0, 0, 1);
    if (err!=0) {
       printf("WD_AI_Trig_Config error=%d", err);
       exit(1);
    }
    err=WD_AI_AsyncDblBufferMode (card, 1);
    if (err!=NoError) {
       printf("WD_AI_AsyncDblBufferMode error=%d", err);
       exit(1);
    }
	if(fok) {
		svfile = fopen("DmaData.dat", "wb");  /* Open in binary mode */
		/* Write simple header: channel number as U16 */
		U16 header = (U16)CHANNELNUMBER;
		fwrite(&header, sizeof(U16), 1, svfile);
	}
	err=WD_AI_ContBufferSetup (card, ai_buf, SCANCOUNT, &Id);
	if (err != NoError) {
		printf("ContBufferSetup error (buf1)=%d\n", err);
		exit(1);
	}
	err=WD_AI_ContBufferSetup (card, ai_buf2, SCANCOUNT, &Id);
	if (err != NoError) {
		printf("ContBufferSetup error (buf2)=%d\n", err);
		exit(1);
	}
    err = WD_AI_ContReadChannel (card, CHANNELNUMBER, 0, SCANCOUNT, SCAN_INTERVAL, SAMPLE_INTERVAL, ASYNCH_OP);
    if (err!=0) {
       printf("WD_AI_ContReadChannel error=%d\n", err);
       printf("Attempting card reset...\n");
       ResetCard(card);
       exit(1);
    }
    printf("\n\n\nStart Data Conversion by External Trigger Signal\nAnd Press any key to stop Opeartion.\n");
    printf("\n\nData count : \n");
    do {
        do {
             WD_AI_AsyncDblBufferHalfReady(card, &halfReady, &fStop);
		} while (!halfReady && !fStop);

		//Here to handle the data stored in ready buffer
	    count += (SCANCOUNT);
        printf("%d\r", count);
		if(fok)
           write_to_file( tmpId?ai_buf2:ai_buf, SCANCOUNT );
		tmpId = (tmpId + 1) % 2;
    } while(!CheckKeyPressed());
    WD_AI_AsyncClear(card, &startPos, &count1);
	if(fok) {
	 write_to_file( tmpId?ai_buf2:ai_buf, count1 ); 
     fclose(svfile);
	}
	printf("\n\nTotal %d input data.\n", count+count1);
    ResetCard(card);  /* Ensure clean shutdown */
    WD_Release_Card(card);
    printf("\nPress ENTER to exit the program. "); WaitForKeyPress();
    return 0;
}

// Data will currently be saved to the old directly @ ADCTests/PostExtDTrig as the fixed file name.