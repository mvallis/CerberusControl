#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include "wd-dask.h"
#include "wddaskex.h"

#define CHANNELNUMBER 0
#define SCANCOUNT     4000
#define TIMEBASE      WD_IntTimeBase
#define BUFAUTORESET   1
#define ADCONVERTSRC  WD_AI_ADCONVSRC_TimePacer
#define ADTRIGSRC     WD_AI_TRGSRC_ExtD
#define ADTRIGMODE    WD_AI_TRGMOD_POST
#define ADTRIGPOL     WD_AI_TrgNegative
#define SCAN_INTERVAL  6000


short ai_buf[SCANCOUNT];
short ai_buf2[SCANCOUNT];
FILE *svfile;
I16 card;
U16 ai_range=0;
DAS_IOT_DEV_PROP cardProp;

void write_to_file( U16 *buf, U32 write_count )
{
  U32 i;
  F64 vol = 0.0;
  
  for(i=0; i<write_count; i++) {
	WD_AI_VoltScale (card, ai_range, buf[i], &vol);
    fprintf( svfile, "0x%04x(%4.2fV)\n", (U16) (buf[i] & cardProp.mask), vol);
  }
}

main()
{
    I16 err, card_num,i,Id, tmpId = 0;
    BOOLEAN halfReady, fStop, fok=0;
    U32 count=0, count1, startPos;
	U16 card_type = 1;

    printf("This program inputs %d scans from CH-0.\n", SCANCOUNT);
	card_type = WD_ChooseDeviceType(0);
	printf("Please input a card number: ");
    scanf(" %hd", &card_num);
	printf("Save data to a file (DmaData.dat): Yes(1) or No(0)? ");
	scanf(" %d", &fok);
    if ((card=WD_Register_Card (card_type, card_num)) <0 ) {
        printf("Register_Card error=%d", card);
        exit(1);
    }
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
		svfile = fopen("DmaData.dat", "w");
		fprintf( svfile, "CH%d :\n", CHANNELNUMBER);
	}
	err=WD_AI_ContBufferSetup (card, ai_buf, SCANCOUNT, &Id);
	err=WD_AI_ContBufferSetup (card, ai_buf2, SCANCOUNT, &Id);
    err = WD_AI_ContReadChannel (card, CHANNELNUMBER, Id, SCANCOUNT, SCAN_INTERVAL, SCAN_INTERVAL, ASYNCH_OP);
    if (err!=0) {
       printf("WD_AI_ContReadChannel error=%d", err);
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
    } while(!kbhit());
    WD_AI_AsyncClear(card, &startPos, &count1);
	if(fok) {
	 write_to_file( tmpId?ai_buf2:ai_buf, count1 ); 
     fclose(svfile);
	}
	printf("\n\nTotal %d input data.\n", count+count1);
    WD_Release_Card(card);
    printf("\nPress ENTER to exit the program. "); getch();
}