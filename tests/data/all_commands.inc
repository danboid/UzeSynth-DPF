/*
 * Test patches using every kernel patch command.
 */

//Square lead with tremolo and a slide
const char lead[] PROGMEM ={
0,PC_WAVE,4,
0,PC_ENV_VOL,200,
0,PC_ENV_SPEED,-2,
0,PC_TREMOLO_LEVEL,96,
0,PC_TREMOLO_RATE,40,
0,PC_SLIDE_SPEED,6,
4,PC_SLIDE,-12,
0,PC_NOTE_HOLD,0,
1,PC_ENV_SPEED,-12,
20,PATCH_END
};

//Arpeggio using a loop
const char arp[] PROGMEM ={
0,PC_WAVE,1,
0,PC_LOOP_START,3,
2,PC_NOTE_UP,4,
2,PC_NOTE_UP,3,
2,PC_NOTE_DOWN,7,
0,PC_LOOP_END,0,
0,PC_PITCH,72,
3,PC_NOTE_CUT,0,
0,PATCH_END
};

//Snare
const char snare[] PROGMEM ={
0,PC_NOISE_PARAMS,2,
0,PC_ENV_SPEED,-20,
2,PC_NOISE_PARAMS,5,
0,PATCH_END
};

const struct PatchStruct patches[] PROGMEM = {
{0,NULL,lead,0,0},
{0,NULL,arp,0,0},
{1,NULL,snare,0,0}
};
