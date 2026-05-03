// WL_INTER.C

#include "wl_def.h"

LRstruct LevelRatios[LRpack];
int32_t lastBreathTime = 0;

void Write (int x, int y, const char *string);

//==========================================================================

/*
==================
=
= CLearSplitVWB
=
==================
*/

void
ClearSplitVWB (void)
{
    WindowX = 0;
    WindowY = 0;
    WindowW = 320;
    WindowH = 160;
}


//==========================================================================

#ifdef SPEAR
#ifndef SPEARDEMO
////////////////////////////////////////////////////////
//
// End of Spear of Destiny
//
////////////////////////////////////////////////////////

void
EndScreen (int palette, int screen)
{
    SDL_Color pal[256];
    CA_CacheScreen (screen);
    VW_UpdateScreen ();
    CA_CacheGrChunk (palette);
    VL_ConvertPalette(grsegs[palette], pal, 256);
    VL_FadeIn (0, 255, pal, 30);
    UNCACHEGRCHUNK (palette);
    IN_ClearKeysDown ();
    IN_Ack ();
    VW_FadeOut ();
}


void
EndSpear (void)
{
    SDL_Color pal[256];

    EndScreen (END1PALETTE, ENDSCREEN11PIC);

    CA_CacheScreen (ENDSCREEN3PIC);
    VW_UpdateScreen ();
    CA_CacheGrChunk (END3PALETTE);
    VL_ConvertPalette(grsegs[END3PALETTE], pal, 256);
    VL_FadeIn (0, 255, pal, 30);
    UNCACHEGRCHUNK (END3PALETTE);
    fontnumber = 0;
    fontcolor = 0xd0;
    WindowX = 0;
    WindowW = 320;
    PrintX = 0;
    PrintY = 180;
    US_CPrint (STR_ENDGAME1 "\n");
    US_CPrint (STR_ENDGAME2);
    VW_UpdateScreen ();
    IN_UserInput(700);

    PrintX = 0;
    PrintY = 180;
    VWB_Bar (0, 180, 320, 20, 0);
    US_CPrint (STR_ENDGAME3 "\n");
    US_CPrint (STR_ENDGAME4);
    VW_UpdateScreen ();
    IN_UserInput(700);

    VW_FadeOut ();

    EndScreen (END4PALETTE, ENDSCREEN4PIC);
    EndScreen (END5PALETTE, ENDSCREEN5PIC);
    EndScreen (END6PALETTE, ENDSCREEN6PIC);
    EndScreen (END7PALETTE, ENDSCREEN7PIC);
    EndScreen (END8PALETTE, ENDSCREEN8PIC);
    EndScreen (END9PALETTE, ENDSCREEN9PIC);

    EndScreen (END2PALETTE, ENDSCREEN12PIC);

    MainMenu[savegame].active = 0;
}
#endif
#endif

//==========================================================================

/*
==================
=
= Victory
=
==================
*/

void
Victory (void)
{
#ifndef SPEARDEMO
    int32_t sec;
    int i, min, kr, sr, tr, x;
    char tempstr[16];

#define RATIOX  6
#define RATIOY  14
#define TIMEX   14
#define TIMEY   8


#ifdef SPEAR
    StartCPMusic (XTHEEND_MUS);

    CA_CacheGrChunk (BJCOLLAPSE1PIC);
    CA_CacheGrChunk (BJCOLLAPSE2PIC);
    CA_CacheGrChunk (BJCOLLAPSE3PIC);
    CA_CacheGrChunk (BJCOLLAPSE4PIC);

    VWB_Bar (0, 0, 320, 200, VIEWCOLOR);
    VWB_DrawPic (124, 44, BJCOLLAPSE1PIC);
    VW_UpdateScreen ();
    VW_FadeIn ();
    VW_WaitVBL (2 * 70);
    VWB_DrawPic (124, 44, BJCOLLAPSE2PIC);
    VW_UpdateScreen ();
    VW_WaitVBL (105);
    VWB_DrawPic (124, 44, BJCOLLAPSE3PIC);
    VW_UpdateScreen ();
    VW_WaitVBL (105);
    VWB_DrawPic (124, 44, BJCOLLAPSE4PIC);
    VW_UpdateScreen ();
    VW_WaitVBL (3 * 70);

    UNCACHEGRCHUNK (BJCOLLAPSE1PIC);
    UNCACHEGRCHUNK (BJCOLLAPSE2PIC);
    UNCACHEGRCHUNK (BJCOLLAPSE3PIC);
    UNCACHEGRCHUNK (BJCOLLAPSE4PIC);
    VL_FadeOut (0, 255, 0, 17, 17, 5);
#endif

    StartCPMusic (URAHERO_MUS);
    ClearSplitVWB ();
    CacheLump (LEVELEND_LUMP_START, LEVELEND_LUMP_END);
    CA_CacheGrChunk (STARTFONT);

#ifndef SPEAR
    CA_CacheGrChunk (C_TIMECODEPIC);
#endif

    VWB_Bar (0, 0, 320, screenHeight / scaleFactor - STATUSLINES + 1, VIEWCOLOR);
    if (bordercol != VIEWCOLOR)
        DrawStatusBorder (VIEWCOLOR);

    Write (18, 2, STR_YOUWIN);

    Write (TIMEX, TIMEY - 2, STR_TOTALTIME);

    Write (12, RATIOY - 2, "averages");

    Write (RATIOX + 8, RATIOY, STR_RATKILL);
    Write (RATIOX + 4, RATIOY + 2, STR_RATSECRET);
    Write (RATIOX, RATIOY + 4, STR_RATTREASURE);

    VWB_DrawPic (8, 4, L_BJWINSPIC);


    for (kr = sr = tr = sec = i = 0; i < LRpack; i++)
    {
        sec += LevelRatios[i].time;
        kr += LevelRatios[i].kill;
        sr += LevelRatios[i].secret;
        tr += LevelRatios[i].treasure;
    }

#ifndef SPEAR
    kr /= LRpack;
    sr /= LRpack;
    tr /= LRpack;
#else
    kr /= 14;
    sr /= 14;
    tr /= 14;
#endif

    min = sec / 60;
    sec %= 60;

    if (min > 99)
        min = sec = 99;

    i = TIMEX * 8 + 1;
    VWB_DrawPic (i, TIMEY * 8, L_NUM0PIC + (min / 10));
    i += 2 * 8;
    VWB_DrawPic (i, TIMEY * 8, L_NUM0PIC + (min % 10));
    i += 2 * 8;
    Write (i / 8, TIMEY, ":");
    i += 1 * 8;
    VWB_DrawPic (i, TIMEY * 8, L_NUM0PIC + (sec / 10));
    i += 2 * 8;
    VWB_DrawPic (i, TIMEY * 8, L_NUM0PIC + (sec % 10));
    VW_UpdateScreen ();

    intoa (kr, sizeof(tempstr), tempstr);
    x = RATIOX + 24 - (int) strlen(tempstr) * 2;
    Write (x, RATIOY, tempstr);

    intoa (sr, sizeof(tempstr), tempstr);
    x = RATIOX + 24 - (int) strlen(tempstr) * 2;
    Write (x, RATIOY + 2, tempstr);

    intoa (tr, sizeof(tempstr), tempstr);
    x = RATIOX + 24 - (int) strlen(tempstr) * 2;
    Write (x, RATIOY + 4, tempstr);

#ifndef UPLOAD
#ifndef SPEAR
    //
    // TOTAL TIME VERIFICATION CODE
    //
    if (gamestate.difficulty >= gd_medium)
    {
        VWB_DrawPic (30 * 8, TIMEY * 8, C_TIMECODEPIC);
        fontnumber = 0;
        fontcolor = READHCOLOR;
        PrintX = 30 * 8 - 3;
        PrintY = TIMEY * 8 + 8;
        PrintX += 4;
        tempstr[0] = (((min / 10) ^ (min % 10)) ^ 0xa) + 'A';
        tempstr[1] = (int) ((((sec / 10) ^ (sec % 10)) ^ 0xa) + 'A');
        tempstr[2] = (tempstr[0] ^ tempstr[1]) + 'A';
        tempstr[3] = 0;
        US_Print (tempstr);
    }
#endif
#endif

    fontnumber = 1;

    VW_UpdateScreen ();
    VW_FadeIn ();

    IN_Ack ();

    VW_FadeOut ();
    if(screenHeight % 200 != 0)
        VL_ClearScreen(0);

#ifndef SPEAR
    UNCACHEGRCHUNK (C_TIMECODEPIC);
#endif
    UnCacheLump (LEVELEND_LUMP_START, LEVELEND_LUMP_END);

#ifndef SPEAR
    EndText ();
#else
    EndSpear ();
#endif

#endif // SPEARDEMO
}


//==========================================================================

/*
==================
=
= PG13
=
==================
*/

void
PG13 (void)
{
    VW_FadeOut ();
    VWB_Bar (0, 0, 320, 200, 0x82);     // background

    CA_CacheGrChunk (PG13PIC);
    VWB_DrawPic (216, 110, PG13PIC);
    VW_UpdateScreen ();

    UNCACHEGRCHUNK (PG13PIC);

    VW_FadeIn ();
    IN_UserInput (TickBase * 7);

    VW_FadeOut ();
}


//==========================================================================

void
Write (int x, int y, const char *string)
{
    static const int alpha[] = { L_NUM0PIC, L_NUM1PIC, L_NUM2PIC, L_NUM3PIC, L_NUM4PIC, L_NUM5PIC,
        L_NUM6PIC, L_NUM7PIC, L_NUM8PIC, L_NUM9PIC, L_COLONPIC, 0, 0, 0, 0, 0, 0, L_APIC, L_BPIC,
        L_CPIC, L_DPIC, L_EPIC, L_FPIC, L_GPIC, L_HPIC, L_IPIC, L_JPIC, L_KPIC,
        L_LPIC, L_MPIC, L_NPIC, L_OPIC, L_PPIC, L_QPIC, L_RPIC, L_SPIC, L_TPIC,
        L_UPIC, L_VPIC, L_WPIC, L_XPIC, L_YPIC, L_ZPIC
    };

    int i, ox, nx, ny, len = (int) strlen(string);
    unsigned char ch;

    ox = nx = x * 8;
    ny = y * 8;
    for (i = 0; i < len; i++)
    {
        if (string[i] == '\n')
        {
            nx = ox;
            ny += 16;
        }
        else
        {
            ch = string[i];
            if (ch >= 'a')
                ch -= ('a' - 'A');
            ch -= '0';

            switch (string[i])
            {
                case '!':
                    VWB_DrawPic (nx, ny, L_EXPOINTPIC);
                    nx += 8;
                    continue;

#ifndef APOGEE_1_0
                case '\'':
                    VWB_DrawPic (nx, ny, L_APOSTROPHEPIC);
                    nx += 8;
                    continue;
#endif

                case ' ':
                    break;

                case 0x3a:     // ':'
                    VWB_DrawPic (nx, ny, L_COLONPIC);
                    nx += 8;
                    continue;

                case '%':
                    VWB_DrawPic (nx, ny, L_PERCENTPIC);
                    break;

                default:
                    VWB_DrawPic (nx, ny, alpha[ch]);
            }
            nx += 16;
        }
    }
}


//
// Breathe Mr. BJ!!!
//
void
BJ_Breathe (void)
{
    static int which = 0, max = 10;
    int pics[2] = { L_GUYPIC, L_GUY2PIC };

    SDL_Delay(5);

    if ((int32_t) GetTimeCount () - lastBreathTime > max)
    {
        which ^= 1;
        VWB_DrawPic (0, 16, pics[which]);
        VW_UpdateScreen ();
        lastBreathTime = GetTimeCount();
        max = 35;
    }
}



/*
==================
=
= LevelCompleted
=
= Entered with the screen faded out
= Still in split screen mode with the status bar
=
= Exit with the screen faded out
=
==================
*/

void
LevelCompleted (void)
{
#define VBLWAIT 30
#define PAR_AMOUNT      500
#define PERCENT100AMT   10000
    typedef struct
    {
        float time;
        char timestr[6];
    } times;

    int x, i, min, sec, ratio, kr, sr, tr;
    char tempstr[16];
    int32_t bonus, timeleft = 0;
    times parTimes[] = {
#ifndef SPEAR
        //
        // Episode One Par Times
        //
        {1.5, "01:30"},
        {2, "02:00"},
        {2, "02:00"},
        {3.5, "03:30"},
        {3, "03:00"},
        {3, "03:00"},
        {2.5, "02:30"},
        {2.5, "02:30"},
        {0, "??:??"},           // Boss level
        {0, "??:??"},           // Secret level

        //
        // Episode Two Par Times
        //
        {1.5, "01:30"},
        {3.5, "03:30"},
        {3, "03:00"},
        {2, "02:00"},
        {4, "04:00"},
        {6, "06:00"},
        {1, "01:00"},
        {3, "03:00"},
        {0, "??:??"},
        {0, "??:??"},

        //
        // Episode Three Par Times
        //
        {1.5, "01:30"},
        {1.5, "01:30"},
        {2.5, "02:30"},
        {2.5, "02:30"},
        {3.5, "03:30"},
        {2.5, "02:30"},
        {2, "02:00"},
        {6, "06:00"},
        {0, "??:??"},
        {0, "??:??"},

        //
        // Episode Four Par Times
        //
        {2, "02:00"},
        {2, "02:00"},
        {1.5, "01:30"},
        {1, "01:00"},
        {4.5, "04:30"},
        {3.5, "03:30"},
        {2, "02:00"},
        {4.5, "04:30"},
        {0, "??:??"},
        {0, "??:??"},

        //
        // Episode Five Par Times
        //
        {2.5, "02:30"},
        {1.5, "01:30"},
        {2.5, "02:30"},
        {2.5, "02:30"},
        {4, "04:00"},
        {3, "03:00"},
        {4.5, "04:30"},
        {3.5, "03:30"},
        {0, "??:??"},
        {0, "??:??"},

        //
        // Episode Six Par Times
        //
        {6.5, "06:30"},
        {4, "04:00"},
        {4.5, "04:30"},
        {6, "06:00"},
        {5, "05:00"},
        {5.5, "05:30"},
        {5.5, "05:30"},
        {8.5, "08:30"},
        {0, "??:??"},
        {0, "??:??"}
#else
        //
        // SPEAR OF DESTINY TIMES
        //
        {1.5, "01:30"},
        {3.5, "03:30"},
        {2.75, "02:45"},
        {3.5, "03:30"},
        {0, "??:??"},           // Boss 1
        {4.5, "04:30"},
        {3.25, "03:15"},
        {2.75, "02:45"},
        {4.75, "04:45"},
        {0, "??:??"},           // Boss 2
        {6.5, "06:30"},
        {4.5, "04:30"},
        {2.75, "02:45"},
        {4.5, "04:30"},
        {6, "06:00"},
        {0, "??:??"},           // Boss 3
        {6, "06:00"},
        {0, "??:??"},           // Boss 4
        {0, "??:??"},           // Secret level 1
        {0, "??:??"},           // Secret level 2
#endif
    };

    CacheLump (LEVELEND_LUMP_START, LEVELEND_LUMP_END);
    ClearSplitVWB ();           // set up for double buffering in split screen
    VWB_Bar (0, 0, 320, screenHeight / scaleFactor - STATUSLINES + 1, VIEWCOLOR);

    if (bordercol != VIEWCOLOR)
        DrawStatusBorder (VIEWCOLOR);

    StartCPMusic (ENDLEVEL_MUS);

//
// do the intermission
//
    IN_ClearKeysDown ();
    IN_StartAck ();

    VWB_DrawPic (0, 16, L_GUYPIC);

#ifndef SPEAR
    if (mapon < 8)
#else
    if (mapon != 4 && mapon != 9 && mapon != 15 && mapon < 17)
#endif
    {
        Write (14, 2, "floor\ncompleted");

        Write (14, 7, STR_BONUS "     0");
        Write (16, 10, STR_TIME);
        Write (16, 12, STR_PAR);

        Write (9, 14, STR_RAT2KILL);
        Write (5, 16, STR_RAT2SECRET);
        Write (1, 18, STR_RAT2TREASURE);

        Write (26, 2, intoa (gamestate.mapon + 1, sizeof(tempstr), tempstr));

        Write (26, 12, parTimes[gamestate.episode * 10 + mapon].timestr);

        //
        // PRINT TIME
        //
        sec = gamestate.TimeCount / 70;

        if (sec > 99 * 60)      // 99 minutes max
            sec = 99 * 60;

        if (gamestate.TimeCount < parTimes[gamestate.episode * 10 + mapon].time * 4200)
            timeleft = (int32_t) ((parTimes[gamestate.episode * 10 + mapon].time * 4200) / 70 - sec);

        min = sec / 60;
        sec %= 60;

        i = 26 * 8;
        VWB_DrawPic (i, 10 * 8, L_NUM0PIC + (min / 10));
        i += 2 * 8;
        VWB_DrawPic (i, 10 * 8, L_NUM0PIC + (min % 10));
        i += 2 * 8;
        Write (i / 8, 10, ":");
        i += 1 * 8;
        VWB_DrawPic (i, 10 * 8, L_NUM0PIC + (sec / 10));
        i += 2 * 8;
        VWB_DrawPic (i, 10 * 8, L_NUM0PIC + (sec % 10));

        VW_UpdateScreen ();
        VW_FadeIn ();


        //
        // FIGURE RATIOS OUT BEFOREHAND
        //
        kr = sr = tr = 0;
        if (gamestate.killtotal)
            kr = (gamestate.killcount * 100) / gamestate.killtotal;
        if (gamestate.secrettotal)
            sr = (gamestate.secretcount * 100) / gamestate.secrettotal;
        if (gamestate.treasuretotal)
            tr = (gamestate.treasurecount * 100) / gamestate.treasuretotal;


        //
        // PRINT TIME BONUS
        //
        bonus = timeleft * PAR_AMOUNT;
        if (bonus)
        {
            for (i = 0; i <= timeleft; i++)
            {
                lntoa ((int32_t) i * PAR_AMOUNT, sizeof(tempstr), tempstr);
                x = 36 - (int) strlen(tempstr) * 2;
                Write (x, 7, tempstr);
                if (!(i % (PAR_AMOUNT / 10)))
                    SD_PlaySound (ENDBONUS1SND);
                if(!(i % (PAR_AMOUNT / 50))) VW_UpdateScreen ();
                while(SD_SoundPlaying ())
                    BJ_Breathe ();
                if (IN_CheckAck ())
                    goto done;
            }

            VW_UpdateScreen ();

            SD_PlaySound (ENDBONUS2SND);
            while (SD_SoundPlaying ())
                BJ_Breathe ();
        }


#define RATIOXX                37
        //
        // KILL RATIO
        //
        ratio = kr;
        for (i = 0; i <= ratio; i++)
        {
            intoa (i, sizeof(tempstr), tempstr);
            x = RATIOXX - (int) strlen(tempstr) * 2;
            Write (x, 14, tempstr);
            if (!(i % 10))
                SD_PlaySound (ENDBONUS1SND);
            if(!(i & 1)) VW_UpdateScreen ();
            while (SD_SoundPlaying ())
                BJ_Breathe ();

            if (IN_CheckAck ())
                goto done;
        }
        if (ratio >= 100)
        {
            VW_WaitVBL (VBLWAIT);
            SD_StopSound ();
            bonus += PERCENT100AMT;
            lntoa (bonus, sizeof(tempstr), tempstr);
            x = (RATIOXX - 1) - (int) strlen(tempstr) * 2;
            Write (x, 7, tempstr);
            VW_UpdateScreen ();
            SD_PlaySound (PERCENT100SND);
        }
        else if (!ratio)
        {
            VW_WaitVBL (VBLWAIT);
            SD_StopSound ();
            SD_PlaySound (NOBONUSSND);
        }
        else
            SD_PlaySound (ENDBONUS2SND);

        VW_UpdateScreen ();
        while (SD_SoundPlaying ())
            BJ_Breathe ();

        //
        // SECRET RATIO
        //
        ratio = sr;
        for (i = 0; i <= ratio; i++)
        {
            intoa (i, sizeof(tempstr), tempstr);
            x = RATIOXX - (int) strlen(tempstr) * 2;
            Write (x, 16, tempstr);
            if (!(i % 10))
                SD_PlaySound (ENDBONUS1SND);
            if(!(i & 1)) VW_UpdateScreen ();
            while (SD_SoundPlaying ())
                BJ_Breathe ();

            if (IN_CheckAck ())
                goto done;
        }
        if (ratio >= 100)
        {
            VW_WaitVBL (VBLWAIT);
            SD_StopSound ();
            bonus += PERCENT100AMT;
            lntoa (bonus, sizeof(tempstr), tempstr);
            x = (RATIOXX - 1) - (int) strlen(tempstr) * 2;
            Write (x, 7, tempstr);
            VW_UpdateScreen ();
            SD_PlaySound (PERCENT100SND);
        }
        else if (!ratio)
        {
            VW_WaitVBL (VBLWAIT);
            SD_StopSound ();
            SD_PlaySound (NOBONUSSND);
        }
        else
            SD_PlaySound (ENDBONUS2SND);
        VW_UpdateScreen ();
        while (SD_SoundPlaying ())
            BJ_Breathe ();

        //
        // TREASURE RATIO
        //
        ratio = tr;
        for (i = 0; i <= ratio; i++)
        {
            intoa (i, sizeof(tempstr), tempstr);
            x = RATIOXX - (int) strlen(tempstr) * 2;
            Write (x, 18, tempstr);
            if (!(i % 10))
                SD_PlaySound (ENDBONUS1SND);
            if(!(i & 1)) VW_UpdateScreen ();
            while (SD_SoundPlaying ())
                BJ_Breathe ();
            if (IN_CheckAck ())
                goto done;
        }
        if (ratio >= 100)
        {
            VW_WaitVBL (VBLWAIT);
            SD_StopSound ();
            bonus += PERCENT100AMT;
            lntoa (bonus, sizeof(tempstr), tempstr);
            x = (RATIOXX - 1) - (int) strlen(tempstr) * 2;
            Write (x, 7, tempstr);
            VW_UpdateScreen ();
            SD_PlaySound (PERCENT100SND);
        }
        else if (!ratio)
        {
            VW_WaitVBL (VBLWAIT);
            SD_StopSound ();
            SD_PlaySound (NOBONUSSND);
        }
        else
            SD_PlaySound (ENDBONUS2SND);
        VW_UpdateScreen ();
        while (SD_SoundPlaying ())
            BJ_Breathe ();


        //
        // JUMP STRAIGHT HERE IF KEY PRESSED
        //
done:   intoa (kr, sizeof(tempstr), tempstr);
        x = RATIOXX - (int) strlen(tempstr) * 2;
        Write (x, 14, tempstr);

        intoa (sr, sizeof(tempstr), tempstr);
        x = RATIOXX - (int) strlen(tempstr) * 2;
        Write (x, 16, tempstr);

        intoa (tr, sizeof(tempstr), tempstr);
        x = RATIOXX - (int) strlen(tempstr) * 2;
        Write (x, 18, tempstr);

        bonus = (int32_t) timeleft *PAR_AMOUNT +
            (PERCENT100AMT * (kr >= 100)) +
            (PERCENT100AMT * (sr >= 100)) + (PERCENT100AMT * (tr >= 100));

        GivePoints (bonus);
        lntoa (bonus, sizeof(tempstr), tempstr);
        x = 36 - (int) strlen(tempstr) * 2;
        Write (x, 7, tempstr);

        //
        // SAVE RATIO INFORMATION FOR ENDGAME
        //
        LevelRatios[mapon].kill = kr;
        LevelRatios[mapon].secret = sr;
        LevelRatios[mapon].treasure = tr;
        LevelRatios[mapon].time = min * 60 + sec;
    }
    else
    {
#ifdef SPEAR
#ifndef SPEARDEMO
        switch (mapon)
        {
            case 4:
                Write (14, 4, " trans\n" " grosse\n" STR_DEFEATED);
                break;
            case 9:
                Write (14, 4, "barnacle\n" "wilhelm\n" STR_DEFEATED);
                break;
            case 15:
                Write (14, 4, "ubermutant\n" STR_DEFEATED);
                break;
            case 17:
                Write (14, 4, " death\n" " knight\n" STR_DEFEATED);
                break;
            case 18:
                Write (13, 4, "secret tunnel\n" "    area\n" "  completed!");
                break;
            case 19:
                Write (13, 4, "secret castle\n" "    area\n" "  completed!");
                break;
        }
#endif
#else
        Write (14, 4, "secret floor\n completed!");
#endif

        Write (10, 16, "15000 bonus!");

        VW_UpdateScreen ();
        VW_FadeIn ();

        GivePoints (15000);
    }


    DrawScore ();
    VW_UpdateScreen ();

    lastBreathTime = GetTimeCount();
    IN_StartAck ();
    while (!IN_CheckAck ())
        BJ_Breathe ();

//
// done
//
#ifdef SPEARDEMO
    if (gamestate.mapon == 1)
    {
        SD_PlaySound (BONUS1UPSND);

        CA_CacheGrChunk (STARTFONT + 1);
        Message ("This concludes your demo\n"
                 "of Spear of Destiny! Now,\n" "go to your local software\n" "store and buy it!");
        UNCACHEGRCHUNK (STARTFONT + 1);

        IN_ClearKeysDown ();
        IN_Ack ();
    }
#endif

    VW_FadeOut ();
    DrawPlayBorder();

    UnCacheLump (LEVELEND_LUMP_START, LEVELEND_LUMP_END);
}



//==========================================================================


/*
=================
=
= PreloadGraphics
=
= Fill the cache up
=
=================
*/

boolean
PreloadUpdate (unsigned current, unsigned total)
{
    unsigned w = WindowW - scaleFactor * 10;

    VWB_BarScaledCoord (WindowX + scaleFactor * 5, WindowY + WindowH - scaleFactor * 3,
        w, scaleFactor * 2, BLACK);
    w = ((int32_t) w * current) / total;
    if (w)
    {
        VWB_BarScaledCoord (WindowX + scaleFactor * 5, WindowY + WindowH - scaleFactor * 3,
            w, scaleFactor * 2, 0x37);       //SECONDCOLOR);
        VWB_BarScaledCoord (WindowX + scaleFactor * 5, WindowY + WindowH - scaleFactor * 3,
            w - scaleFactor * 1, scaleFactor * 1, 0x32);

    }
    VW_UpdateScreen ();
//      if (LastScan == sc_Escape)
//      {
//              IN_ClearKeysDown();
//              return(true);
//      }
//      else
    return (false);
}

void
PreloadGraphics (void)
{
    DrawLevel ();
    ClearSplitVWB ();           // set up for double buffering in split screen

    VWB_BarScaledCoord (0, 0, screenWidth, screenHeight - scaleFactor * (STATUSLINES - 1), bordercol);
    LatchDrawPicScaledCoord ((screenWidth-scaleFactor*224)/16,
        (screenHeight-scaleFactor*(STATUSLINES+48))/2, GETPSYCHEDPIC);

    WindowX = (screenWidth - scaleFactor*224)/2;
    WindowY = (screenHeight - scaleFactor*(STATUSLINES+48))/2;
    WindowW = scaleFactor * 28 * 8;
    WindowH = scaleFactor * 48;

    VW_UpdateScreen ();
    VW_FadeIn ();

//      PM_Preload (PreloadUpdate);
    PreloadUpdate (10, 10);
    IN_UserInput (70);
    VW_FadeOut ();

    DrawPlayBorder ();
    VW_UpdateScreen ();
}


//==========================================================================

/*
==================
=
= DrawHighScores
=
==================
*/

void
DrawHighScores (void)
{
    char buffer[16];
#ifndef SPEAR
    char *str;
#ifndef UPLOAD
    char buffer1[8];
#endif
#endif
    word i, w, h;
    HighScore *s;

#ifndef SPEAR
    CA_CacheGrChunk (HIGHSCORESPIC);
    CA_CacheGrChunk (STARTFONT);
#ifndef APOGEE_1_0
    CA_CacheGrChunk (C_LEVELPIC);
    CA_CacheGrChunk (C_SCOREPIC);
    CA_CacheGrChunk (C_NAMEPIC);
#else
    CA_CacheGrChunk (C_CODEPIC);
#endif

    ClearMScreen ();
    DrawStripes (10);

    VWB_DrawPic (48, 0, HIGHSCORESPIC);
    UNCACHEGRCHUNK (HIGHSCORESPIC);

#ifndef APOGEE_1_0
    VWB_DrawPic (4 * 8, 68, C_NAMEPIC);
    VWB_DrawPic (20 * 8, 68, C_LEVELPIC);
    VWB_DrawPic (28 * 8, 68, C_SCOREPIC);
#else
    VWB_DrawPic(35*8,68,C_CODEPIC);
#endif
    fontnumber = 0;

#else
    CacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
    ClearMScreen ();
    DrawStripes (10);
    UnCacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);

    CacheLump (HIGHSCORES_LUMP_START, HIGHSCORES_LUMP_END);
    CA_CacheGrChunk (STARTFONT + 1);
    VWB_DrawPic (0, 0, HIGHSCORESPIC);

    fontnumber = 1;
#endif


#ifndef SPEAR
    SETFONTCOLOR (15, 0x29);
#else
    SETFONTCOLOR (HIGHLIGHT, 0x29);
#endif

    for (i = 0, s = Scores; i < MaxScores; i++, s++)
    {
        PrintY = 76 + (16 * i);

        //
        // name
        //
#ifndef SPEAR
        PrintX = 4 * 8;
#else
        PrintX = 16;
#endif
        US_Print (s->name);

        //
        // level
        //
        intoa (s->completed, sizeof(buffer), buffer);
#ifndef SPEAR
        for (str = buffer; *str; str++)
            *str = *str + (129 - '0');  // Used fixed-width numbers (129...)
        USL_MeasureString (buffer, &w, &h);
        PrintX = (22 * 8) - w;
#else
        USL_MeasureString (buffer, &w, &h);
        PrintX = 194 - w;
#endif

#ifndef UPLOAD
#ifndef SPEAR
        PrintX -= 6;
        intoa (s->episode + 1, sizeof(buffer1), buffer1);
        US_Print ("E");
        US_Print (buffer1);
        US_Print ("/L");
#endif
#endif

#ifdef SPEAR
        if (s->completed == 21)
            VWB_DrawPic (PrintX + 8, PrintY - 1, C_WONSPEARPIC);
        else
#endif
            US_Print (buffer);

        //
        // score
        //
        intoa (s->score, sizeof(buffer), buffer);
#ifndef SPEAR
        for (str = buffer; *str; str++)
            *str = *str + (129 - '0');  // Used fixed-width numbers (129...)
        USL_MeasureString (buffer, &w, &h);
        PrintX = (34 * 8) - 8 - w;
#else
        USL_MeasureString (buffer, &w, &h);
        PrintX = 292 - w;
#endif
        US_Print (buffer);

#ifdef APOGEE_1_0
//#ifndef UPLOAD
#ifndef SPEAR
        //
        // verification #
        //
        if (!i)
        {
            char temp = (((s->score >> 28) & 0xf) ^ ((s->score >> 24) & 0xf)) + 'A';
            char temp1 = (((s->score >> 20) & 0xf) ^ ((s->score >> 16) & 0xf)) + 'A';
            char temp2 = (((s->score >> 12) & 0xf) ^ ((s->score >> 8) & 0xf)) + 'A';
            char temp3 = (((s->score >> 4) & 0xf) ^ ((s->score >> 0) & 0xf)) + 'A';

            SETFONTCOLOR (0x49, 0x29);
            PrintX = 35 * 8;
            buffer[0] = temp;
            buffer[1] = temp1;
            buffer[2] = temp2;
            buffer[3] = temp3;
            buffer[4] = 0;
            US_Print (buffer);
            SETFONTCOLOR (15, 0x29);
        }
#endif
//#endif
#endif
    }

    VW_UpdateScreen ();

#ifdef SPEAR
    UnCacheLump (HIGHSCORES_LUMP_START, HIGHSCORES_LUMP_END);
    fontnumber = 0;
#endif
}

//===========================================================================


/*
=======================
=
= CheckHighScore
=
=======================
*/

void
CheckHighScore (int32_t score, word other)
{
    word i, j;
    int n;
    HighScore myscore;

    strcpy (myscore.name, "");
    myscore.score = score;
    myscore.episode = gamestate.episode;
    myscore.completed = other;

    for (i = 0, n = -1; i < MaxScores; i++)
    {
        if ((myscore.score > Scores[i].score)
            || ((myscore.score == Scores[i].score) && (myscore.completed > Scores[i].completed)))
        {
            for (j = MaxScores; --j > i;)
                Scores[j] = Scores[j - 1];
            Scores[i] = myscore;
            n = i;
            break;
        }
    }

#ifdef SPEAR
    StartCPMusic (XAWARD_MUS);
#else
    StartCPMusic (ROSTER_MUS);
#endif
    DrawHighScores ();

    VW_FadeIn ();

    if (n != -1)
    {
        //
        // got a high score
        //
        PrintY = 76 + (16 * n);
#ifndef SPEAR
        PrintX = 4 * 8;
        backcolor = BORDCOLOR;
        fontcolor = 15;
        US_LineInput (PrintX, PrintY, Scores[n].name, 0, true, MaxHighName, 100);
#else
        PrintX = 16;
        fontnumber = 1;
        VWB_Bar (PrintX - 2, PrintY - 2, 145, 15, 0x9c);
        VW_UpdateScreen ();
        backcolor = 0x9c;
        fontcolor = 15;
        US_LineInput (PrintX, PrintY, Scores[n].name, 0, true, MaxHighName, 130);
#endif
    }
    else
    {
        IN_ClearKeysDown ();
        IN_UserInput (500);
    }

}
//===========================================================================
