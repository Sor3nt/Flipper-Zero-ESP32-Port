////////////////////////////////////////////////////////////////////
//
// WL_MENU.C
// by John Romero (C) 1992 Id Software, Inc.
//
////////////////////////////////////////////////////////////////////

#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
    #include <io.h>
    #include <direct.h>
#else
    #include <unistd.h>
#endif

#include "wl_def.h"

extern int lastgamemusicoffset;
extern int numEpisodesMissing;

//
// PRIVATE PROTOTYPES
//

#define STARTITEM       newgame

// ENDSTRx constants are defined in foreign.h
char endStrings[9][80] = {
    ENDSTR1,
    ENDSTR2,
    ENDSTR3,
    ENDSTR4,
    ENDSTR5,
    ENDSTR6,
    ENDSTR7,
    ENDSTR8,
    ENDSTR9
};

CP_itemtype MainMenu[] = {

    {1, STR_NG, CP_NewGame},
    {1, STR_SD, CP_Sound},
    {1, STR_CL, CP_Control},
    {1, STR_LG, CP_LoadGame},
    {0, STR_SG, CP_SaveGame},
    {1, STR_CV, CP_ChangeView},
    {1, STR_VS, CP_ViewScores},
    {1, STR_BD, 0},
    {1, STR_QT, 0}
};

CP_itemtype SndMenu[] = {
    {1, STR_NONE, 0},
    {1, STR_PC, 0},
    {1, STR_ALSB, 0},
    {0, "", 0},
    {0, "", 0},
    {1, STR_NONE, 0},
    {0, STR_DISNEY, 0},
    {1, STR_SB, 0},
    {0, "", 0},
    {0, "", 0},
    {1, STR_NONE, 0},
    {1, STR_ALSB, 0}
};

enum { CTL_MOUSEENABLE, CTL_MOUSESENS, CTL_JOYENABLE, CTL_CUSTOMIZE, CTL_CUSTOMIZE2, CTL_ALWAYSRUN, CTL_CROSSHAIR }; // [FG] extended "Customize" menus, toggle always run

CP_itemtype CtlMenu[] = {
    {0, STR_MOUSEEN, 0},
    {0, STR_SENS, MouseSensitivity},
    {0, STR_JOYEN, 0},
    {1, "Customize Keyboard", CustomControls}, // [FG] extended "Customize" menus
    {1, "Customize Mouse/JS", Custom2Controls}, // [FG] extended "Customize" menus
    {1, "Always Run", 0}, // [FG] toggle always run
    {1, "Crosshair", 0} // [FG] toggle crosshair
};

#ifndef SPEAR
CP_itemtype NewEmenu[] = {
    {1, "Episode 1\n" "Escape from Wolfenstein", 0},
    {0, "", 0},
    {3, "Episode 2\n" "Operation: Eisenfaust", 0},
    {0, "", 0},
    {3, "Episode 3\n" "Die, Fuhrer, Die!", 0},
    {0, "", 0},
    {3, "Episode 4\n" "A Dark Secret", 0},
    {0, "", 0},
    {3, "Episode 5\n" "Trail of the Madman", 0},
    {0, "", 0},
    {3, "Episode 6\n" "Confrontation", 0}
};
#endif


CP_itemtype NewMenu[] = {
    {1, STR_DADDY, 0},
    {1, STR_HURTME, 0},
    {1, STR_BRINGEM, 0},
    {1, STR_DEATH, 0}
};

CP_itemtype LSMenu[] = {
    {1, "", 0},
    {1, "", 0},
    {1, "", 0},
    {1, "", 0},
    {1, "", 0},
    {1, "", 0},
    {1, "", 0},
    {1, "", 0},
    {1, "", 0},
    {1, "", 0}
};

CP_itemtype CusMenu[] = {
    {1, "", 0},
    {0, "", 0},
    {1, "", 0},
    {0, "", 0},
    {1, "", 0},
    {0, "", 0},
    {1, "", 0},
    {0, "", 0},
    {1, "", 0}
};

// [FG] extended "Customize" menus
CP_itemtype Cus2Menu[] = {
    {1, "", 0},
    {0, "", 0},
    {0, "", 0},
    {1, "", 0},
    {0, "", 0},
    {1, "", 0},
    {0, "", 0},
    {1, "", 0},
    {0, "", 0}
};

// CP_iteminfo struct format: short x, y, amount, curpos, indent;
CP_iteminfo MainItems = { MENU_X, MENU_Y, lengthof(MainMenu), STARTITEM, 24 },
            SndItems  = { SM_X, SM_Y1, lengthof(SndMenu), 0, 52 },
            LSItems   = { LSM_X, LSM_Y, lengthof(LSMenu), 0, 24 },
            CtlItems  = { CTL_X, CTL_Y, lengthof(CtlMenu), -1, 56 },
            CusItems  = { 8, CST_Y + 13 * 2, lengthof(CusMenu), -1, 0},
            Cus2Items = { 8, CST_Y + 13 * 2, lengthof(Cus2Menu), -1, 0}, // [FG] extended "Customize" menus
#ifndef SPEAR
            NewEitems = { NE_X, NE_Y, lengthof(NewEmenu), 0, 88 },
#endif
            NewItems  = { NM_X, NM_Y, lengthof(NewMenu), 2, 24 };

int color_hlite[] = {
    DEACTIVE,
    HIGHLIGHT,
    READHCOLOR,
    0x67
};

int color_norml[] = {
    DEACTIVE,
    TEXTCOLOR,
    READCOLOR,
    0x6b
};

int EpisodeSelect[6] = { 1 };


static int SaveGamesAvail[10];
static int StartGame;
static int SoundStatus = 1;
static int pickquick;
static char SaveGameNames[10][32];
static char SaveName[13] = "savegam?.";


////////////////////////////////////////////////////////////////////
//
// INPUT MANAGER SCANCODE TABLES
//
////////////////////////////////////////////////////////////////////

#if 0
static const char *ScanNames[] =      // Scan code names with single chars
{
    "?", "?", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "+", "?", "?",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "|", "?", "A", "S",
    "D", "F", "G", "H", "J", "K", "L", ";", "\"", "?", "?", "?", "Z", "X", "C", "V",
    "B", "N", "M", ",", ".", "/", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
    "?", "?", "?", "?", "?", "?", "?", "?", "\xf", "?", "-", "\x15", "5", "\x11", "+", "?",
    "\x13", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
    "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
    "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?"
};                              // DEBUG - consolidate these
static ScanCode ExtScanCodes[] =        // Scan codes with >1 char names
{
    1, 0xe, 0xf, 0x1d, 0x2a, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
    0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x59, 0x46, 0x1c, 0x36,
    0x37, 0x38, 0x47, 0x49, 0x4f, 0x51, 0x52, 0x53, 0x45, 0x48,
    0x50, 0x4b, 0x4d, 0x00
};
static const char *ExtScanNames[] =   // Names corresponding to ExtScanCodes
{
    "Esc", "BkSp", "Tab", "Ctrl", "LShft", "Space", "CapsLk", "F1", "F2", "F3", "F4",
    "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "ScrlLk", "Enter", "RShft",
    "PrtSc", "Alt", "Home", "PgUp", "End", "PgDn", "Ins", "Del", "NumLk", "Up",
    "Down", "Left", "Right", ""
};

/*#pragma warning 737 9
static byte
                                        *ScanNames[] =          // Scan code names with single chars
                                        {
        "?","?","1","2","3","4","5","6","7","8","9","0","-","+","?","?",
        "Q","W","E","R","T","Y","U","I","O","P","[","]","|","?","A","S",
        "D","F","G","H","J","K","L",";","\"","?","?","?","Z","X","C","V",
        "B","N","M",",",".","/","?","?","?","?","?","?","?","?","?","?",
        "?","?","?","?","?","?","?","?","\xf","?","-","\x15","5","\x11","+","?",
        "\x13","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?",
        "?","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?",
        "?","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?"
                                        };      // DEBUG - consolidate these
static byte ExtScanCodes[] =    // Scan codes with >1 char names
                                        {
        1,0xe,0xf,0x1d,0x2a,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,
        0x3f,0x40,0x41,0x42,0x43,0x44,0x57,0x59,0x46,0x1c,0x36,
        0x37,0x38,0x47,0x49,0x4f,0x51,0x52,0x53,0x45,0x48,
        0x50,0x4b,0x4d,0x00
                                        };
static byte *ExtScanNames[] =   // Names corresponding to ExtScanCodes
                                        {
        "Esc","BkSp","Tab","Ctrl","LShft","Space","CapsLk","F1","F2","F3","F4",
        "F5","F6","F7","F8","F9","F10","F11","F12","ScrlLk","Enter","RShft",
        "PrtSc","Alt","Home","PgUp","End","PgDn","Ins","Del","NumLk","Up",
        "Down","Left","Right",""
                                        };*/

#else
static WolfMap<ScanCode, const char *> ScanNames;
void US_SetScanNames()
{
    ScanNames[0] = "None";
    ScanNames[sc_Enter] = "Enter";
    ScanNames[SDLK_UP] = "Up";
    ScanNames[SDLK_DOWN] = "Down";
    ScanNames[SDLK_RIGHT] = "Right";
    ScanNames[SDLK_LEFT] = "Left";
    ScanNames[SDLK_INSERT] = "Ins";
    ScanNames[SDLK_HOME] = "Home";
    ScanNames[SDLK_END] = "End";
    ScanNames[SDLK_PAGEUP] = "PgUp";
    ScanNames[SDLK_PAGEDOWN] = "PgDn";
    ScanNames[SDLK_F1] = "F1";
    ScanNames[SDLK_F2] = "F2";
    ScanNames[SDLK_F3] = "F3";
    ScanNames[SDLK_F4] = "F4";
    ScanNames[SDLK_F5] = "F5";
    ScanNames[SDLK_F6] = "F6";
    ScanNames[SDLK_F7] = "F7";
    ScanNames[SDLK_F8] = "F8";
    ScanNames[SDLK_F9] = "F9";
    ScanNames[SDLK_F10] = "F10";
    ScanNames[SDLK_F11] = "F11";
    ScanNames[SDLK_F12] = "F12";
    ScanNames[SDLK_F13] = "F13";
    ScanNames[SDLK_F14] = "F14";
    ScanNames[SDLK_F15] = "F15";
    ScanNames[SDLK_F16] = "F16";
    ScanNames[SDLK_F17] = "F17";
    ScanNames[SDLK_F18] = "F18";
    ScanNames[SDLK_F19] = "F19";
    ScanNames[SDLK_NUMLOCKCLEAR] = "NumLk";
    ScanNames[SDLK_CAPSLOCK] = "CapsLk";
    ScanNames[SDLK_SCROLLLOCK] = "ScrlLk";
    ScanNames[SDLK_RSHIFT] = "RShft";
    ScanNames[SDLK_LSHIFT] = "Shift";
    ScanNames[SDLK_RCTRL] = "RCtrl";
    ScanNames[SDLK_LCTRL] = "LCtrl";
    ScanNames[SDLK_RALT] = "RAlt";
    ScanNames[SDLK_LALT] = "Alt";
    ScanNames[SDLK_RGUI] = "RMeta";
    ScanNames[SDLK_LGUI] = "LMeta";
    ScanNames[SDLK_PRINTSCREEN] = "PrtSc";
    ScanNames[sc_BackSpace] = "BkSp";
    ScanNames[sc_Tab] = "Tab";
    ScanNames[sc_Return] = "Return";
    ScanNames[SDLK_PAUSE] = "Pause";
    ScanNames[sc_Escape] = "Esc";
    ScanNames[sc_Space] = "Space";
    ScanNames[SDLK_EXCLAIM] = "!";
    ScanNames[SDLK_QUOTEDBL] = "\"";
    ScanNames[SDLK_HASH] = "#";
    ScanNames[SDLK_DOLLAR] = "$";
    ScanNames[SDLK_QUESTION] = "?";
    ScanNames[SDLK_AMPERSAND] = "&";
    ScanNames[SDLK_QUOTE] = "'";
    ScanNames[SDLK_LEFTPAREN] = "(";
    ScanNames[SDLK_RIGHTPAREN] = ")";
    ScanNames[SDLK_ASTERISK] = "*";
    ScanNames[SDLK_PLUS] = "+";
    ScanNames[SDLK_COMMA] = ",";
    ScanNames[SDLK_MINUS] = "-";
    ScanNames[SDLK_PERIOD] = ".";
    ScanNames[SDLK_SLASH] = "/";
    ScanNames[SDLK_0] = "0";
    ScanNames[SDLK_1] = "1";
    ScanNames[SDLK_2] = "2";
    ScanNames[SDLK_3] = "3";
    ScanNames[SDLK_4] = "4";
    ScanNames[SDLK_5] = "5";
    ScanNames[SDLK_6] = "6";
    ScanNames[SDLK_7] = "7";
    ScanNames[SDLK_8] = "8";
    ScanNames[SDLK_9] = "9";
    ScanNames[SDLK_COLON] = ":";
    ScanNames[SDLK_SEMICOLON] = ";";
    ScanNames[SDLK_LESS] = "<";
    ScanNames[SDLK_EQUALS] = "=";
    ScanNames[SDLK_GREATER] = ">";
    ScanNames[SDLK_AT] = "@";
    ScanNames[SDLK_a] = "A";
    ScanNames[SDLK_b] = "B";
    ScanNames[SDLK_c] = "C";
    ScanNames[SDLK_d] = "D";
    ScanNames[SDLK_e] = "E";
    ScanNames[SDLK_f] = "F";
    ScanNames[SDLK_g] = "G";
    ScanNames[SDLK_h] = "H";
    ScanNames[SDLK_i] = "I";
    ScanNames[SDLK_j] = "J";
    ScanNames[SDLK_k] = "K";
    ScanNames[SDLK_l] = "L";
    ScanNames[SDLK_m] = "M";
    ScanNames[SDLK_n] = "N";
    ScanNames[SDLK_o] = "O";
    ScanNames[SDLK_p] = "P";
    ScanNames[SDLK_q] = "Q";
    ScanNames[SDLK_r] = "R";
    ScanNames[SDLK_s] = "S";
    ScanNames[SDLK_t] = "T";
    ScanNames[SDLK_u] = "U";
    ScanNames[SDLK_v] = "V";
    ScanNames[SDLK_w] = "W";
    ScanNames[SDLK_x] = "X";
    ScanNames[SDLK_y] = "Y";
    ScanNames[SDLK_z] = "Z";
    ScanNames[SDLK_LEFTBRACKET] = "[";
    ScanNames[SDLK_BACKSLASH] = "\\";
    ScanNames[SDLK_RIGHTBRACKET] = "]";
    ScanNames[KEYD_MWHEELUP] = "MWUP";
    ScanNames[KEYD_MWHEELDOWN] = "MWDN";
}

#endif

bool menuactive; // [FG] true if menu is active

////////////////////////////////////////////////////////////////////
//
// Wolfenstein Control Panel!  Ta Da!
//
////////////////////////////////////////////////////////////////////
void
US_ControlPanel (ScanCode scancode)
{
    extern volatile int g_wolf3d_in_menu;
    g_wolf3d_in_menu = 1;
    int which;

    if (ingame)
    {
        if (CP_CheckQuick (scancode))
            return;
        lastgamemusicoffset = StartCPMusic (MENUSONG);
    }
    else
        StartCPMusic (MENUSONG);
    SetupControlPanel ();

    //
    // F-KEYS FROM WITHIN GAME
    //
    switch (scancode)
    {
        case sc_F1:
            BossKey ();
            goto finishup;

        case sc_F2:
            CP_SaveGame (0);
            goto finishup;

        case sc_F3:
            CP_LoadGame (0);
            goto finishup;

        case sc_F4:
            CP_Sound (0);
            goto finishup;

        case sc_F5:
            CP_ChangeView (0);
            goto finishup;

        case sc_F6:
            CP_Control (0);
            goto finishup;

        finishup:
            CleanupControlPanel ();
#ifdef SPEAR
            UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif
            g_wolf3d_in_menu = 0;
            return;
    }

#ifdef SPEAR
    CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif

    DrawMainMenu ();
    MenuFadeIn ();
    StartGame = 0;

    //
    // MAIN MENU LOOP
    //
    do
    {
        which = HandleMenu (&MainItems, &MainMenu[0], NULL);

#ifdef SPEAR
#ifndef SPEARDEMO
        IN_ProcessEvents();

        //
        // EASTER EGG FOR SPEAR OF DESTINY!
        //
        if (Keyboard[sc_I] && Keyboard[sc_D])
        {
            VW_FadeOut ();
            StartCPMusic (XJAZNAZI_MUS);
            UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
            UnCacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
            ClearMemory ();


            CA_CacheGrChunk (IDGUYS1PIC);
            VWB_DrawPic (0, 0, IDGUYS1PIC);
            UNCACHEGRCHUNK (IDGUYS1PIC);

            CA_CacheGrChunk (IDGUYS2PIC);
            VWB_DrawPic (0, 80, IDGUYS2PIC);
            UNCACHEGRCHUNK (IDGUYS2PIC);

            VW_UpdateScreen ();

            SDL_Color pal[256];
            CA_CacheGrChunk (IDGUYSPALETTE);
            VL_ConvertPalette(grsegs[IDGUYSPALETTE], pal, 256);
            VL_FadeIn (0, 255, pal, 30);
            UNCACHEGRCHUNK (IDGUYSPALETTE);

            while (Keyboard[sc_I] || Keyboard[sc_D])
                IN_WaitAndProcessEvents();
            IN_ClearKeysDown ();
            IN_Ack ();

            VW_FadeOut ();

            CacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
            CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
            DrawMainMenu ();
            StartCPMusic (MENUSONG);
            MenuFadeIn ();
        }
#endif
#endif

        switch (which)
        {
            case viewscores:
                if (MainMenu[viewscores].routine == NULL)
                {
                    if (CP_EndGame (0))
                        StartGame = 1;
                }
                else
                {
                    DrawMainMenu();
                    MenuFadeIn ();
                }
                break;

            case backtodemo:
                StartGame = 1;
                if (!ingame)
                    StartCPMusic (INTROSONG);
                VL_FadeOut (0, 255, 0, 0, 0, 10);
                break;

            case -1:
            case quit:
                CP_Quit (0);
                break;

            default:
                if (!StartGame)
                {
                    DrawMainMenu ();
                    MenuFadeIn ();
                }
        }

        //
        // "EXIT OPTIONS" OR "NEW GAME" EXITS
        //
    }
    while (!StartGame);

    //
    // DEALLOCATE EVERYTHING
    //
    CleanupControlPanel ();

    //
    // CHANGE MAINMENU ITEM
    //
    if (startgame || loadedgame)
        EnableEndGameMenuItem();

    // RETURN/START GAME EXECUTION

#ifdef SPEAR
    UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif
    g_wolf3d_in_menu = 0;
}

void EnableEndGameMenuItem()
{
    MainMenu[viewscores].routine = NULL;
    strcpy (MainMenu[viewscores].string, STR_EG);
}

////////////////////////
//
// DRAW MAIN MENU SCREEN
//
void
DrawMainMenu (void)
{
    ClearMScreen ();

    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);
    DrawStripes (10);
    VWB_DrawPic (84, 0, C_OPTIONSPIC);

    DrawWindow (MENU_X - 8, MENU_Y - 3, MENU_W, MENU_H, BKGDCOLOR);

    //
    // CHANGE "GAME" AND "DEMO"
    //
    if (ingame)
    {
        strcpy (&MainMenu[backtodemo].string[8], STR_GAME);
        MainMenu[backtodemo].active = 2;
        MainItems.curpos = backtodemo;
    }
    else
    {
        strcpy (&MainMenu[backtodemo].string[8], STR_DEMO);
        MainMenu[backtodemo].active = 1;
    }

    DrawMenu (&MainItems, &MainMenu[0]);
    VW_UpdateScreen ();
}


////////////////////////////////////////////////////////////////////
//
// BOSS KEY
//
////////////////////////////////////////////////////////////////////
void
BossKey (void)
{
    const char *prompt[] = {"C:\\>", "C:\\>_"};
    int i = 0;

    int lastoffs = StopMusic();
    int32_t lastBlinkTime = GetTimeCount();

    // [FG] instant fade-in to cleared screen
    VWB_Bar(0, 0, 320, 200, 0);
    VL_FadeIn(0, 255, gamepal, 1);

    fontnumber = 0;
    SETFONTCOLOR(15, 0);

    do
    {
        // [FG] clear screen
        VWB_Bar (0, 0, 320, 200, 0);

        // [FG] print DOS prompt line with blinking cursor
        if ((int32_t)GetTimeCount() - lastBlinkTime > 20)
        {
            lastBlinkTime = GetTimeCount();
            i ^= 1;
        }
        PrintX = PrintY = 0;
        US_Print(prompt[i]);

        VW_UpdateScreen();
        TicDelay(20);
    } while (!(IN_KeyDown(sc_Escape) || IN_KeyDown(sc_Control) || IN_KeyDown(sc_Alt)));

    IN_ClearKeysDown ();

    ContinueMusic(lastoffs);
}


////////////////////////////////////////////////////////////////////
//
// CHECK QUICK-KEYS & QUIT (WHILE IN A GAME)
//
////////////////////////////////////////////////////////////////////
int
CP_CheckQuick (ScanCode scancode)
{
    switch (scancode)
    {
        //
        // END GAME
        //
        case sc_F7:
            CA_CacheGrChunk (STARTFONT + 1);

            WindowH = 160;
            if (Confirm (ENDGAMESTR))
            {
                playstate = ex_died;
                killerobj = NULL;
                pickquick = gamestate.lives = 0;
            }

            WindowH = 200;
            fontnumber = 0;
            MainMenu[savegame].active = 0;
            return 1;

        //
        // QUICKSAVE
        //
        case sc_F8:
            if (SaveGamesAvail[LSItems.curpos] && pickquick)
            {
                CA_CacheGrChunk (STARTFONT + 1);
                fontnumber = 1;
                Message (STR_SAVING "...");
                CP_SaveGame (1);
                fontnumber = 0;
            }
            else
            {
#ifndef SPEAR
                CA_CacheGrChunk (STARTFONT + 1);
                CA_CacheGrChunk (C_CURSOR1PIC);
                CA_CacheGrChunk (C_CURSOR2PIC);
                CA_CacheGrChunk (C_DISKLOADING1PIC);
                CA_CacheGrChunk (C_DISKLOADING2PIC);
                CA_CacheGrChunk (C_SAVEGAMEPIC);
                CA_CacheGrChunk (C_MOUSELBACKPIC);
#else
                CacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
                CA_CacheGrChunk (C_CURSOR1PIC);
#endif

                VW_FadeOut ();
                if(screenHeight % 200 != 0)
                    VL_ClearScreen(0);

                lastgamemusicoffset = StartCPMusic (MENUSONG);
                pickquick = CP_SaveGame (0);

                SETFONTCOLOR (0, 15);
                IN_ClearKeysDown ();
                VW_FadeOut();
                if(viewsize != 21)
                    DrawPlayScreen ();

                if (!startgame && !loadedgame)
                    ContinueMusic (lastgamemusicoffset);

                if (loadedgame)
                    playstate = ex_abort;
                lasttimecount = GetTimeCount ();

#ifndef SPEAR
                UNCACHEGRCHUNK (C_CURSOR1PIC);
                UNCACHEGRCHUNK (C_CURSOR2PIC);
                UNCACHEGRCHUNK (C_DISKLOADING1PIC);
                UNCACHEGRCHUNK (C_DISKLOADING2PIC);
                UNCACHEGRCHUNK (C_SAVEGAMEPIC);
                UNCACHEGRCHUNK (C_MOUSELBACKPIC);
#else
                UnCacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
#endif
            }
            return 1;

        //
        // QUICKLOAD
        //
        case sc_F9:
            if (SaveGamesAvail[LSItems.curpos] && pickquick)
            {
                char string[100] = STR_LGC;


                CA_CacheGrChunk (STARTFONT + 1);
                fontnumber = 1;

                strcat (string, SaveGameNames[LSItems.curpos]);
                strcat (string, "\"?");

                if (Confirm (string))
                    CP_LoadGame (1);

                fontnumber = 0;
            }
            else
            {
#ifndef SPEAR
                CA_CacheGrChunk (STARTFONT + 1);
                CA_CacheGrChunk (C_CURSOR1PIC);
                CA_CacheGrChunk (C_CURSOR2PIC);
                CA_CacheGrChunk (C_DISKLOADING1PIC);
                CA_CacheGrChunk (C_DISKLOADING2PIC);
                CA_CacheGrChunk (C_LOADGAMEPIC);
                CA_CacheGrChunk (C_MOUSELBACKPIC);
#else
                CA_CacheGrChunk (C_CURSOR1PIC);
                CacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
#endif

                VW_FadeOut ();
                if(screenHeight % 200 != 0)
                    VL_ClearScreen(0);

                lastgamemusicoffset = StartCPMusic (MENUSONG);
                pickquick = CP_LoadGame (0);    // loads lastgamemusicoffs

                SETFONTCOLOR (0, 15);
                IN_ClearKeysDown ();
                VW_FadeOut();
                if(viewsize != 21)
                    DrawPlayScreen ();

                if (!startgame && !loadedgame)
                    ContinueMusic (lastgamemusicoffset);

                if (loadedgame)
                    playstate = ex_abort;

                lasttimecount = GetTimeCount ();

#ifndef SPEAR
                UNCACHEGRCHUNK (C_CURSOR1PIC);
                UNCACHEGRCHUNK (C_CURSOR2PIC);
                UNCACHEGRCHUNK (C_DISKLOADING1PIC);
                UNCACHEGRCHUNK (C_DISKLOADING2PIC);
                UNCACHEGRCHUNK (C_LOADGAMEPIC);
                UNCACHEGRCHUNK (C_MOUSELBACKPIC);
#else
                UnCacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
#endif
            }
            return 1;

        //
        // QUIT
        //
        case sc_F10:
            CA_CacheGrChunk (STARTFONT + 1);

            WindowX = WindowY = 0;
            WindowW = 320;
            WindowH = 160;
            if (Confirm (endStrings[US_RndT () & (0x7 + (US_RndT () & 1))]))
            {
                VW_UpdateScreen ();
                SD_MusicOff ();
                SD_StopSound ();
                MenuFadeOut ();

                Quit (NULL);
            }

            DrawPlayBorder ();
            WindowH = 200;
            fontnumber = 0;
            return 1;
    }

    return 0;
}


////////////////////////////////////////////////////////////////////
//
// END THE CURRENT GAME
//
////////////////////////////////////////////////////////////////////
int
CP_EndGame (int)
{
    int res;
    res = Confirm (ENDGAMESTR);
    DrawMainMenu();
    if(!res) return 0;

    pickquick = gamestate.lives = 0;
    playstate = ex_died;
    killerobj = NULL;

    MainMenu[savegame].active = 0;
    MainMenu[viewscores].routine = CP_ViewScores;
    strcpy (MainMenu[viewscores].string, STR_VS);

    return 1;
}


////////////////////////////////////////////////////////////////////
//
// VIEW THE HIGH SCORES
//
////////////////////////////////////////////////////////////////////
int
CP_ViewScores (int)
{
    fontnumber = 0;

#ifdef SPEAR
    UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
    StartCPMusic (XAWARD_MUS);
#else
    StartCPMusic (ROSTER_MUS);
#endif

    DrawHighScores ();
    VW_UpdateScreen ();
    MenuFadeIn ();
    fontnumber = 1;

    IN_Ack ();

    StartCPMusic (MENUSONG);
    MenuFadeOut ();

#ifdef SPEAR
    CacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
    CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif
    return 0;
}


////////////////////////////////////////////////////////////////////
//
// START A NEW GAME
//
////////////////////////////////////////////////////////////////////
int
CP_NewGame (int)
{
    int which, episode;

#ifdef SPEAR
    UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif


#ifndef SPEAR
  firstpart:

    DrawNewEpisode ();
    do
    {
        which = HandleMenu (&NewEitems, &NewEmenu[0], NULL);
        switch (which)
        {
            case -1:
                MenuFadeOut ();
                return 0;

            default:
                if (!EpisodeSelect[which / 2])
                {
                    SD_PlaySound (NOWAYSND);
                    Message ("Please select \"Read This!\"\n"
                             "from the Options menu to\n"
                             "find out how to order this\n" "episode from Apogee.");
                    IN_ClearKeysDown ();
                    IN_Ack ();
                    DrawNewEpisode ();
                    which = 0;
                }
                else
                {
                    episode = which / 2;
                    which = 1;
                }
                break;
        }

    }
    while (!which);

    ShootSnd ();

    //
    // ALREADY IN A GAME?
    //
    if (ingame)
        if (!Confirm (CURGAME))
        {
            MenuFadeOut ();
            return 0;
        }

    MenuFadeOut ();

#else
    episode = 0;

    //
    // ALREADY IN A GAME?
    //
    CacheLump (NEWGAME_LUMP_START, NEWGAME_LUMP_END);
    DrawNewGame ();
    if (ingame)
        if (!Confirm (CURGAME))
        {
            MenuFadeOut ();
            UnCacheLump (NEWGAME_LUMP_START, NEWGAME_LUMP_END);
            CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
            return 0;
        }

#endif

    DrawNewGame ();
    which = HandleMenu (&NewItems, &NewMenu[0], DrawNewGameDiff);
    if (which < 0)
    {
        MenuFadeOut ();
#ifndef SPEAR
        goto firstpart;
#else
        UnCacheLump (NEWGAME_LUMP_START, NEWGAME_LUMP_END);
        CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
        return 0;
#endif
    }

    ShootSnd ();
    NewGame (which, episode);
    StartGame = 1;
    MenuFadeOut ();

    pickquick = 0;

#ifdef SPEAR
    UnCacheLump (NEWGAME_LUMP_START, NEWGAME_LUMP_END);
    CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif

    return 0;
}


#ifndef SPEAR
/////////////////////
//
// DRAW NEW EPISODE MENU
//
void
DrawNewEpisode (void)
{
    int i;

    ClearMScreen ();
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);

    DrawWindow (NE_X - 4, NE_Y - 4, NE_W + 8, NE_H + 8, BKGDCOLOR);
    SETFONTCOLOR (READHCOLOR, BKGDCOLOR);
    PrintY = 2;
    WindowX = 0;
    US_CPrint ("Which episode to play?");

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    DrawMenu (&NewEitems, &NewEmenu[0]);

    for (i = 0; i < 6; i++)
        VWB_DrawPic (NE_X + 32, NE_Y + i * 26, C_EPISODE1PIC + i);

    VW_UpdateScreen ();
    MenuFadeIn ();
    WaitKeyUp ();
}
#endif

/////////////////////
//
// DRAW NEW GAME MENU
//
void
DrawNewGame (void)
{
    ClearMScreen ();
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);

    SETFONTCOLOR (READHCOLOR, BKGDCOLOR);
    PrintX = NM_X + 20;
    PrintY = NM_Y - 32;

#ifndef SPEAR
    US_Print ("How tough are you?");
#else
    VWB_DrawPic (PrintX, PrintY, C_HOWTOUGHPIC);
#endif

    DrawWindow (NM_X - 5, NM_Y - 10, NM_W, NM_H, BKGDCOLOR);

    DrawMenu (&NewItems, &NewMenu[0]);
    DrawNewGameDiff (NewItems.curpos);
    VW_UpdateScreen ();
    MenuFadeIn ();
    WaitKeyUp ();
}


////////////////////////
//
// DRAW NEW GAME GRAPHIC
//
void
DrawNewGameDiff (int w)
{
    VWB_DrawPic (NM_X + 185, NM_Y + 7, w + C_BABYMODEPIC);
}


////////////////////////////////////////////////////////////////////
//
// HANDLE SOUND MENU
//
////////////////////////////////////////////////////////////////////
int
CP_Sound (int)
{
    int which;


#ifdef SPEAR
    UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
    CacheLump (SOUND_LUMP_START, SOUND_LUMP_END);
#endif

    DrawSoundMenu ();
    MenuFadeIn ();
    WaitKeyUp ();

    do
    {
        which = HandleMenu (&SndItems, &SndMenu[0], NULL);
        //
        // HANDLE MENU CHOICES
        //
        switch (which)
        {
                //
                // SOUND EFFECTS
                //
            case 0:
                if (SoundMode != sdm_Off)
                {
                    SD_WaitSoundDone ();
                    SD_SetSoundMode (sdm_Off);
                    DrawSoundMenu ();
                }
                break;
            case 1:
                if (SoundMode != sdm_PC)
                {
                    SD_WaitSoundDone ();
                    SD_SetSoundMode (sdm_PC);
                    CA_LoadAllSounds ();
                    DrawSoundMenu ();
                    ShootSnd ();
                }
                break;
            case 2:
                if (SoundMode != sdm_AdLib)
                {
                    SD_WaitSoundDone ();
                    SD_SetSoundMode (sdm_AdLib);
                    CA_LoadAllSounds ();
                    DrawSoundMenu ();
                    ShootSnd ();
                }
                break;

                //
                // DIGITIZED SOUND
                //
            case 5:
                if (DigiMode != sds_Off)
                {
                    SD_SetDigiDevice (sds_Off);
                    DrawSoundMenu ();
                }
                break;
            case 6:
/*                if (DigiMode != sds_SoundSource)
                {
                    SD_SetDigiDevice (sds_SoundSource);
                    DrawSoundMenu ();
                    ShootSnd ();
                }*/
                break;
            case 7:
                if (DigiMode != sds_SoundBlaster)
                {
                    SD_SetDigiDevice (sds_SoundBlaster);
                    DrawSoundMenu ();
                    ShootSnd ();
                }
                break;

                //
                // MUSIC
                //
            case 10:
                if (MusicMode != smm_Off)
                {
                    SD_SetMusicMode (smm_Off);
                    DrawSoundMenu ();
                    ShootSnd ();
                }
                break;
            case 11:
                if (MusicMode != smm_AdLib)
                {
                    SD_SetMusicMode (smm_AdLib);
                    DrawSoundMenu ();
                    ShootSnd ();
                    StartCPMusic (MENUSONG);
                }
                break;
        }
    }
    while (which >= 0);

    MenuFadeOut ();

#ifdef SPEAR
    UnCacheLump (SOUND_LUMP_START, SOUND_LUMP_END);
    CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif
    return 0;
}


//////////////////////
//
// DRAW THE SOUND MENU
//
void
DrawSoundMenu (void)
{
    int i, on;


    //
    // DRAW SOUND MENU
    //
    ClearMScreen ();
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);

    DrawWindow (SM_X - 8, SM_Y1 - 3, SM_W, SM_H1, BKGDCOLOR);
    DrawWindow (SM_X - 8, SM_Y2 - 3, SM_W, SM_H2, BKGDCOLOR);
    DrawWindow (SM_X - 8, SM_Y3 - 3, SM_W, SM_H3, BKGDCOLOR);

    //
    // IF NO ADLIB, NON-CHOOSENESS!
    //
    if (!AdLibPresent && !SoundBlasterPresent)
    {
        SndMenu[2].active = SndMenu[10].active = SndMenu[11].active = 0;
    }

    if (!SoundBlasterPresent)
        SndMenu[7].active = 0;

    if (!SoundBlasterPresent)
        SndMenu[5].active = 0;

    DrawMenu (&SndItems, &SndMenu[0]);
    VWB_DrawPic (100, SM_Y1 - 20, C_FXTITLEPIC);
    VWB_DrawPic (100, SM_Y2 - 20, C_DIGITITLEPIC);
    VWB_DrawPic (100, SM_Y3 - 20, C_MUSICTITLEPIC);

    for (i = 0; i < SndItems.amount; i++)
        if (SndMenu[i].string[0])
        {
            //
            // DRAW SELECTED/NOT SELECTED GRAPHIC BUTTONS
            //
            on = 0;
            switch (i)
            {
                    //
                    // SOUND EFFECTS
                    //
                case 0:
                    if (SoundMode == sdm_Off)
                        on = 1;
                    break;
                case 1:
                    if (SoundMode == sdm_PC)
                        on = 1;
                    break;
                case 2:
                    if (SoundMode == sdm_AdLib)
                        on = 1;
                    break;

                    //
                    // DIGITIZED SOUND
                    //
                case 5:
                    if (DigiMode == sds_Off)
                        on = 1;
                    break;
                case 6:
//                    if (DigiMode == sds_SoundSource)
//                        on = 1;
                    break;
                case 7:
                    if (DigiMode == sds_SoundBlaster)
                        on = 1;
                    break;

                    //
                    // MUSIC
                    //
                case 10:
                    if (MusicMode == smm_Off)
                        on = 1;
                    break;
                case 11:
                    if (MusicMode == smm_AdLib)
                        on = 1;
                    break;
            }

            if (on)
                VWB_DrawPic (SM_X + 24, SM_Y1 + i * 13 + 2, C_SELECTEDPIC);
            else
                VWB_DrawPic (SM_X + 24, SM_Y1 + i * 13 + 2, C_NOTSELECTEDPIC);
        }

    DrawMenuGun (&SndItems);
    VW_UpdateScreen ();
}


//
// DRAW LOAD/SAVE IN PROGRESS
//
void
DrawLSAction (int which)
{
#define LSA_X   96
#define LSA_Y   80
#define LSA_W   130
#define LSA_H   42

    DrawWindow (LSA_X, LSA_Y, LSA_W, LSA_H, TEXTCOLOR);
    DrawOutline (LSA_X, LSA_Y, LSA_W, LSA_H, 0, HIGHLIGHT);
    VWB_DrawPic (LSA_X + 8, LSA_Y + 5, C_DISKLOADING1PIC);

    fontnumber = 1;
    SETFONTCOLOR (0, TEXTCOLOR);
    PrintX = LSA_X + 46;
    PrintY = LSA_Y + 13;

    if (!which)
        US_Print (STR_LOADING "...");
    else
        US_Print (STR_SAVING "...");

    VW_UpdateScreen ();
}


////////////////////////////////////////////////////////////////////
//
// LOAD SAVED GAMES
//
////////////////////////////////////////////////////////////////////
int
CP_LoadGame (int quick)
{
    FILE *file;
    int which, exit = 0;
    char name[13];
    char loadpath[300];

    strcpy (name, SaveName);

    //
    // QUICKLOAD?
    //
    if (quick)
    {
        which = LSItems.curpos;

        if (SaveGamesAvail[which])
        {
            name[7] = which + '0';

            if(configdir[0])
                snprintf(loadpath, sizeof(loadpath), "%s/%s", configdir, name);
            else
                strcpy(loadpath, name);

            file = fopen (loadpath, "rb");
            fseek (file, 32, SEEK_SET);
            loadedgame = true;
            LoadTheGame (file, 0, 0);
            loadedgame = false;
            fclose (file);

            DrawFace ();
            DrawHealth ();
            DrawLives ();
            DrawLevel ();
            DrawAmmo ();
            DrawKeys ();
            DrawWeapon ();
            DrawScore ();
            ContinueMusic (lastgamemusicoffset);
            return 1;
        }
    }


#ifdef SPEAR
    UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
    CacheLump (LOADSAVE_LUMP_START, LOADSAVE_LUMP_END);
#endif

    DrawLoadSaveScreen (0);

    do
    {
        which = HandleMenu (&LSItems, &LSMenu[0], TrackWhichGame);
        if (which >= 0 && SaveGamesAvail[which])
        {
            ShootSnd ();
            name[7] = which + '0';

            if(configdir[0])
                snprintf(loadpath, sizeof(loadpath), "%s/%s", configdir, name);
            else
                strcpy(loadpath, name);

            file = fopen (loadpath, "rb");
            fseek (file, 32, SEEK_SET);

            DrawLSAction (0);
            loadedgame = true;

            LoadTheGame (file, LSA_X + 8, LSA_Y + 5);
            fclose (file);

            StartGame = 1;
            ShootSnd ();

            exit = 1;
            break;
        }

    }
    while (which >= 0);

    MenuFadeOut ();

#ifdef SPEAR
    UnCacheLump (LOADSAVE_LUMP_START, LOADSAVE_LUMP_END);
    CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif

    return exit;
}


///////////////////////////////////
//
// HIGHLIGHT CURRENT SELECTED ENTRY
//
void
TrackWhichGame (int w)
{
    static int lastgameon = 0;

    PrintLSEntry (lastgameon, TEXTCOLOR);
    PrintLSEntry (w, HIGHLIGHT);

    lastgameon = w;
}


////////////////////////////
//
// DRAW THE LOAD/SAVE SCREEN
//
void
DrawLoadSaveScreen (int loadsave)
{
#define DISKX   100
#define DISKY   0

    int i;


    ClearMScreen ();
    fontnumber = 1;
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);
    DrawWindow (LSM_X - 10, LSM_Y - 5, LSM_W, LSM_H, BKGDCOLOR);
    DrawStripes (10);

    if (!loadsave)
        VWB_DrawPic (60, 0, C_LOADGAMEPIC);
    else
        VWB_DrawPic (60, 0, C_SAVEGAMEPIC);

    for (i = 0; i < 10; i++)
        PrintLSEntry (i, TEXTCOLOR);

    DrawMenu (&LSItems, &LSMenu[0]);
    VW_UpdateScreen ();
    MenuFadeIn ();
    WaitKeyUp ();
}


///////////////////////////////////////////
//
// PRINT LOAD/SAVE GAME ENTRY W/BOX OUTLINE
//
void
PrintLSEntry (int w, int color)
{
    SETFONTCOLOR (color, BKGDCOLOR);
    DrawOutline (LSM_X + LSItems.indent, LSM_Y + w * 13, LSM_W - LSItems.indent - 15, 11, color,
                 color);
    PrintX = LSM_X + LSItems.indent + 2;
    PrintY = LSM_Y + w * 13 + 1;
    fontnumber = 0;

    if (SaveGamesAvail[w])
        US_Print (SaveGameNames[w]);
    else
        US_Print ("      - " STR_EMPTY " -");

    fontnumber = 1;
}


////////////////////////////////////////////////////////////////////
//
// SAVE CURRENT GAME
//
////////////////////////////////////////////////////////////////////
int
CP_SaveGame (int quick)
{
    int which, exit = 0;
    FILE *file;
    char name[13];
    char savepath[300];
    char input[32];

    strcpy (name, SaveName);

    //
    // QUICKSAVE?
    //
    if (quick)
    {
        which = LSItems.curpos;

        if (SaveGamesAvail[which])
        {
            name[7] = which + '0';

            if(configdir[0])
                snprintf(savepath, sizeof(savepath), "%s/%s", configdir, name);
            else
                strcpy(savepath, name);

            unlink (savepath);
            file = fopen (savepath, "wb");

            strcpy (input, &SaveGameNames[which][0]);

            fwrite (input, 1, 32, file);
            fseek (file, 32, SEEK_SET);
            SaveTheGame (file, 0, 0);
            fclose (file);

            return 1;
        }
    }


#ifdef SPEAR
    UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
    CacheLump (LOADSAVE_LUMP_START, LOADSAVE_LUMP_END);
#endif

    DrawLoadSaveScreen (1);

    do
    {
        which = HandleMenu (&LSItems, &LSMenu[0], TrackWhichGame);
        if (which >= 0)
        {
            //
            // OVERWRITE EXISTING SAVEGAME?
            //
            if (SaveGamesAvail[which])
            {
                if (!Confirm (GAMESVD))
                {
                    DrawLoadSaveScreen (1);
                    continue;
                }
                else
                {
                    DrawLoadSaveScreen (1);
                    PrintLSEntry (which, HIGHLIGHT);
                    VW_UpdateScreen ();
                }
            }

            ShootSnd ();

            strcpy (input, &SaveGameNames[which][0]);
            name[7] = which + '0';

            fontnumber = 0;
            if (!SaveGamesAvail[which])
                VWB_Bar (LSM_X + LSItems.indent + 1, LSM_Y + which * 13 + 1,
                         LSM_W - LSItems.indent - 16, 10, BKGDCOLOR);
            VW_UpdateScreen ();

            if (US_LineInput
                (LSM_X + LSItems.indent + 2, LSM_Y + which * 13 + 1, input, input, true, 31,
                 LSM_W - LSItems.indent - 30))
            {
                SaveGamesAvail[which] = 1;
                strcpy (&SaveGameNames[which][0], input);

                if(configdir[0])
                    snprintf(savepath, sizeof(savepath), "%s/%s", configdir, name);
                else
                    strcpy(savepath, name);

                unlink (savepath);
                file = fopen (savepath, "wb");
                fwrite (input, 32, 1, file);
                fseek (file, 32, SEEK_SET);

                DrawLSAction (1);
                SaveTheGame (file, LSA_X + 8, LSA_Y + 5);

                fclose (file);

                ShootSnd ();
                exit = 1;
            }
            else
            {
                VWB_Bar (LSM_X + LSItems.indent + 1, LSM_Y + which * 13 + 1,
                         LSM_W - LSItems.indent - 16, 10, BKGDCOLOR);
                PrintLSEntry (which, HIGHLIGHT);
                VW_UpdateScreen ();
                SD_PlaySound (ESCPRESSEDSND);
                continue;
            }

            fontnumber = 1;
            break;
        }

    }
    while (which >= 0);

    MenuFadeOut ();

#ifdef SPEAR
    UnCacheLump (LOADSAVE_LUMP_START, LOADSAVE_LUMP_END);
    CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif

    return exit;
}

////////////////////////////////////////////////////////////////////
//
// DEFINE CONTROLS
//
////////////////////////////////////////////////////////////////////
int
CP_Control (int)
{
    int which;

#ifdef SPEAR
    UnCacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
    CacheLump (CONTROL_LUMP_START, CONTROL_LUMP_END);
#endif

    DrawCtlScreen ();
    MenuFadeIn ();
    WaitKeyUp ();

    do
    {
        which = HandleMenu (&CtlItems, CtlMenu, NULL);
        switch (which)
        {
            case CTL_MOUSEENABLE:
                mouseenabled ^= 1;
                DrawCtlScreen ();
                Cus2Items.curpos = CusItems.curpos = -1;
                ShootSnd ();
                break;

            case CTL_JOYENABLE:
                joystickenabled ^= 1;
                DrawCtlScreen ();
                Cus2Items.curpos = CusItems.curpos = -1;
                ShootSnd ();
                break;

            case CTL_MOUSESENS:
            case CTL_CUSTOMIZE:
            // [FG] extended "Customize" menus
            case CTL_CUSTOMIZE2:
                DrawCtlScreen ();
                MenuFadeIn ();
                WaitKeyUp ();
                break;

            // [FG] toggle always run
            case CTL_ALWAYSRUN:
                always_run ^= 1;
                DrawCtlScreen ();
                Cus2Items.curpos = CusItems.curpos = -1;
                ShootSnd ();
                break;

            // [FG] toggle crosshair
            case CTL_CROSSHAIR:
                crosshair ^= 1;
                DrawCtlScreen ();
                Cus2Items.curpos = CusItems.curpos = -1;
                ShootSnd ();
                break;
        }
    }
    while (which >= 0);

    MenuFadeOut ();

#ifdef SPEAR
    UnCacheLump (CONTROL_LUMP_START, CONTROL_LUMP_END);
    CacheLump (OPTIONS_LUMP_START, OPTIONS_LUMP_END);
#endif
    return 0;
}


// [FG] vertical mouse sensitivity menu offset
static const int offset = 3 * 13;

////////////////////////////////
//
// DRAW MOUSE SENSITIVITY SCREEN
//
void
DrawMouseSens (void)
{
    ClearMScreen ();
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);
    DrawWindow (10, 80, 300, 30 + offset, BKGDCOLOR);

    WindowX = 0;
    WindowW = 320;
    PrintY = 82;
    SETFONTCOLOR (READCOLOR, BKGDCOLOR);
    US_CPrint ("Horizontal Sensitivity");
    PrintY = 82 + offset;
    SETFONTCOLOR (READCOLOR, BKGDCOLOR);
    US_CPrint ("Vertical Sensitivity");

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = 14;
    PrintY = 95;
    US_Print (STR_SLOW);
    PrintX = 269;
    US_Print (STR_FAST);
    PrintX = 14;
    PrintY = 95 + offset;
    US_Print (STR_SLOW);
    PrintX = 269;
    US_Print (STR_FAST);

    VWB_Bar (60, 97, 200, 10, TEXTCOLOR);
    DrawOutline (60, 97, 200, 10, 0, HIGHLIGHT);
    DrawOutline (60 + 20 * mouseadjustment, 97, 20, 10, 0, READCOLOR);
    VWB_Bar (61 + 20 * mouseadjustment, 98, 19, 9, READHCOLOR);

    // [FG] vertical mouse sensitivity
    VWB_Bar (60, 97 + offset, 200, 10, TEXTCOLOR);
    DrawOutline (60, 97 + offset, 200, 10, 0, HIGHLIGHT);
    DrawOutline (60 + 20 * mouseadjustment_v, 97 + offset, 20, 10, 0, READCOLOR);
    VWB_Bar (61 + 20 * mouseadjustment_v, 98 + offset, 19, 9, READHCOLOR);

    VW_UpdateScreen ();
    MenuFadeIn ();
}


///////////////////////////
//
// ADJUST MOUSE SENSITIVITY
//
int
MouseSensitivity (int)
{
    ControlInfo ci;
    int exit = 0, oldMA, oldMA_v;
    int *ma = &mouseadjustment;

    // [FG] animate gun cursor
    int shape = C_CURSOR1PIC;
    int32_t lastBlinkTime = GetTimeCount(), timer = 8;

    oldMA = mouseadjustment;
    oldMA_v = mouseadjustment_v;
    DrawMouseSens ();
    do
    {
        // [FG] draw gun background
        VWB_Bar (30, 81 + (ma == &mouseadjustment ? offset : 0), 25, 16, BKGDCOLOR);
        // [FG] animate gun cursor
        if ((int32_t)GetTimeCount() - lastBlinkTime > timer)
        {
            lastBlinkTime = GetTimeCount();
            if (shape == C_CURSOR1PIC)
            {
                shape = C_CURSOR2PIC;
                timer = 8;
            }
            else
            {
                shape = C_CURSOR1PIC;
                timer = 70;
            }
        }
        VWB_DrawPic (33, 81 + (ma == &mouseadjustment ? 0 : offset), shape);
        VW_UpdateScreen ();

        SDL_Delay(5);
        ReadAnyControl (&ci);
        switch (ci.dir)
        {
            case dir_North:
                // [FG] horizontal mouse sensitivity
                ma = &mouseadjustment;
                SD_PlaySound (MOVEGUN1SND);
                break;
            case dir_West:
                if (*ma)
                {
                    *ma -= 1;
                    VWB_Bar (60, 97, 200, 10, TEXTCOLOR);
                    DrawOutline (60, 97, 200, 10, 0, HIGHLIGHT);
                    DrawOutline (60 + 20 * mouseadjustment, 97, 20, 10, 0, READCOLOR);
                    VWB_Bar (61 + 20 * mouseadjustment, 98, 19, 9, READHCOLOR);

                    // [FG] vertical mouse sensitivity
                    VWB_Bar (60, 97 + offset, 200, 10, TEXTCOLOR);
                    DrawOutline (60, 97 + offset, 200, 10, 0, HIGHLIGHT);
                    DrawOutline (60 + 20 * mouseadjustment_v, 97 + offset, 20, 10, 0, READCOLOR);
                    VWB_Bar (61 + 20 * mouseadjustment_v, 98 + offset, 19, 9, READHCOLOR);
                    VW_UpdateScreen ();
                    SD_PlaySound (MOVEGUN1SND);
                    TicDelay(20);
                }
                break;

            case dir_South:
                // [FG] vertical mouse sensitivity
                ma = &mouseadjustment_v;
                SD_PlaySound (MOVEGUN1SND);
                break;
            case dir_East:
                if (*ma < 9)
                {
                    *ma += 1;
                    VWB_Bar (60, 97, 200, 10, TEXTCOLOR);
                    DrawOutline (60, 97, 200, 10, 0, HIGHLIGHT);
                    DrawOutline (60 + 20 * mouseadjustment, 97, 20, 10, 0, READCOLOR);
                    VWB_Bar (61 + 20 * mouseadjustment, 98, 19, 9, READHCOLOR);

                    // [FG] vertical mouse sensitivity
                    VWB_Bar (60, 97 + offset, 200, 10, TEXTCOLOR);
                    DrawOutline (60, 97 + offset, 200, 10, 0, HIGHLIGHT);
                    DrawOutline (60 + 20 * mouseadjustment_v, 97 + offset, 20, 10, 0, READCOLOR);
                    VWB_Bar (61 + 20 * mouseadjustment_v, 98 + offset, 19, 9, READHCOLOR);
                    VW_UpdateScreen ();
                    SD_PlaySound (MOVEGUN1SND);
                    TicDelay(20);
                }
                break;

            default:
                break;
        }

        if (ci.button0 || Keyboard[sc_Space] || Keyboard[sc_Enter])
            exit = 1;
        else if (ci.button1 || Keyboard[sc_Escape])
            exit = 2;

    }
    while (!exit);

    if (exit == 2)
    {
        mouseadjustment = oldMA;
        mouseadjustment_v = oldMA_v;
        SD_PlaySound (ESCPRESSEDSND);
    }
    else
        SD_PlaySound (SHOOTSND);

    WaitKeyUp ();
    MenuFadeOut ();

    return 0;
}


///////////////////////////
//
// DRAW CONTROL MENU SCREEN
//
void
DrawCtlScreen (void)
{
    int i, x, y;

    ClearMScreen ();
    DrawStripes (10);
    VWB_DrawPic (80, 0, C_CONTROLPIC);
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);
    DrawWindow (CTL_X - 8, CTL_Y - 5, CTL_W, CTL_H, BKGDCOLOR);
    WindowX = 0;
    WindowW = 320;
    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);

    if (IN_JoyPresent())
        CtlMenu[CTL_JOYENABLE].active = 1;

    if (MousePresent)
    {
        CtlMenu[CTL_MOUSESENS].active = CtlMenu[CTL_MOUSEENABLE].active = 1;
    }

    CtlMenu[CTL_MOUSESENS].active = mouseenabled;


    DrawMenu (&CtlItems, CtlMenu);


    x = CTL_X + CtlItems.indent - 24;
    y = CTL_Y + 3;
    if (mouseenabled)
        VWB_DrawPic (x, y, C_SELECTEDPIC);
    else
        VWB_DrawPic (x, y, C_NOTSELECTEDPIC);

    y = CTL_Y + 29;
    if (joystickenabled)
        VWB_DrawPic (x, y, C_SELECTEDPIC);
    else
        VWB_DrawPic (x, y, C_NOTSELECTEDPIC);

    // [FG] toggle always run
    y = CTL_Y + 3 + 5*13;
    if (always_run)
        VWB_DrawPic (x, y, C_SELECTEDPIC);
    else
        VWB_DrawPic (x, y, C_NOTSELECTEDPIC);

    // [FG] toggle crosshair
    y = CTL_Y + 3 + 6*13;
    if (crosshair)
        VWB_DrawPic (x, y, C_SELECTEDPIC);
    else
        VWB_DrawPic (x, y, C_NOTSELECTEDPIC);

    //
    // PICK FIRST AVAILABLE SPOT
    //
    if (CtlItems.curpos < 0 || !CtlMenu[CtlItems.curpos].active)
    {
        for (i = 0; i < CtlItems.amount; i++)
        {
            if (CtlMenu[i].active)
            {
                CtlItems.curpos = i;
                break;
            }
        }
    }

    DrawMenuGun (&CtlItems);
    VW_UpdateScreen ();
}


////////////////////////////////////////////////////////////////////
//
// CUSTOMIZE CONTROLS
//
////////////////////////////////////////////////////////////////////
enum
{ FIRE, STRAFE, RUN, OPEN };
// [FG] support up to 32 joystick buttons
char mbarray[32][4] = { "b00", "b01", "b02", "b03", "b04", "b05", "b06", "b07", "b08", "b09",
                        "b10", "b11", "b12", "b13", "b14", "b15", "b16", "b17", "b18", "b19",
                        "b20", "b21", "b22", "b23", "b24", "b25", "b26", "b27", "b28", "b29",
                        "b30", "b31" };
// [FG] support up to 32 joystick buttons
int8_t order[32] = { RUN, OPEN, FIRE, STRAFE, 4, 5, 6, 7, 8, 9,
                    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
                    31, 31 };


// [FG] extended "Customize" menus
int
Custom2Controls (int)
{
    int which;

    DrawCustom2Screen ();
    do
    {
        which = HandleMenu (&Cus2Items, &Cus2Menu[0], FixupCustom2);
        switch (which)
        {
            case 0:
                DefineMouseBtns ();
                DrawCustMouse (1);
                break;
            case 3:
                DefineJoyBtns ();
                DrawCustJoy (0);
                break;
            // [FG] joystick buttons 2 (Wp+, Wp-, Menu, Pause)
            case 5:
                DefineJoy2Btns ();
                DrawCust2Joy (0);
                break;
            case 7:
                DefineJoy3Btns ();
                DrawCust3Joy (0);
                break;
        }
    }
    while (which >= 0);

    MenuFadeOut ();

    return 0;
}

int
CustomControls (int)
{
    int which;

    DrawCustomScreen ();
    do
    {
        which = HandleMenu (&CusItems, &CusMenu[0], FixupCustom);
        switch (which)
        {
            case 0:
                DefineKeyBtns ();
                DrawCustKeybd (0);
                break;
            case 2:
                DefineKeyMove ();
                DrawCustKeys (0);
                break;
            // [FG] keyboard buttons 2 (Wp1, Wp2, Wp3, Wp4)
            case 4:
                DefineKey2Btns ();
                DrawCust2Keybd (0);
                break;
            // [FG] keyboard buttons 3 (Wp+, Wp-, Menu, Pause)
            case 6:
                DefineKey3Btns ();
                DrawCust3Keybd (0);
                break;
            // [FG] keyboard buttons 4 (StrLft, StrRgt)
            case 8:
                DefineKey4Btns ();
                DrawCust4Keybd (0);
                break;
        }
    }
    while (which >= 0);

    MenuFadeOut ();

    return 0;
}


////////////////////////
//
// DEFINE THE MOUSE BUTTONS
//
void
DefineMouseBtns (void)
{
    CustomCtrls mouseallowed = { 0, 1, 1, 1 };
    EnterCtrlData (2, &mouseallowed, DrawCustMouse, PrintCustMouse, MOUSE);
}


////////////////////////
//
// DEFINE THE JOYSTICK BUTTONS
//
void
DefineJoyBtns (void)
{
    CustomCtrls joyallowed = { 1, 1, 1, 1 };
    EnterCtrlData (5, &joyallowed, DrawCustJoy, PrintCustJoy, JOYSTICK);
}

// [FG] joystick buttons 2 (Wp+, Wp-, Menu, Pause)
void
DefineJoy2Btns (void)
{
    CustomCtrls joyallowed = { 1, 1, 1, 1 };
    EnterCtrlData (7, &joyallowed, DrawCust2Joy, PrintCust2Joy, JOYSTICK2);
}

// [FG] joystick buttons 3 (StrLft, StrRgt)
void
DefineJoy3Btns (void)
{
    CustomCtrls joyallowed = { 1, 1, 0, 0 };
    EnterCtrlData (9, &joyallowed, DrawCust3Joy, PrintCust3Joy, JOYSTICK3);
}


////////////////////////
//
// DEFINE THE KEYBOARD BUTTONS
//
void
DefineKeyBtns (void)
{
    CustomCtrls keyallowed = { 1, 1, 1, 1 };
    EnterCtrlData (2, &keyallowed, DrawCustKeybd, PrintCustKeybd, KEYBOARDBTNS);
}

// [FG] keyboard buttons 2 (Wp1, Wp2, Wp3, Wp4)
void
DefineKey2Btns (void)
{
    CustomCtrls keyallowed = { 1, 1, 1, 1 };
    EnterCtrlData (6, &keyallowed, DrawCust2Keybd, PrintCust2Keybd, KEYBOARD2BTNS);
}

// [FG] keyboard buttons 3 (Wp+, Wp-, Menu, Pause)
void
DefineKey3Btns (void)
{
    CustomCtrls keyallowed = { 1, 1, 1, 1 };
    EnterCtrlData (8, &keyallowed, DrawCust3Keybd, PrintCust3Keybd, KEYBOARD3BTNS);
}

// [FG] keyboard buttons 4 (StrLft, StrRgt)
void
DefineKey4Btns (void)
{
    CustomCtrls keyallowed = { 1, 1, 0, 0 };
    EnterCtrlData (10, &keyallowed, DrawCust4Keybd, PrintCust4Keybd, KEYBOARD4BTNS);
}


////////////////////////
//
// DEFINE THE KEYBOARD BUTTONS
//
void
DefineKeyMove (void)
{
    CustomCtrls keyallowed = { 1, 1, 1, 1 };
    EnterCtrlData (4, &keyallowed, DrawCustKeys, PrintCustKeys, KEYBOARDMOVE);
}


////////////////////////
//
// ENTER CONTROL DATA FOR ANY TYPE OF CONTROL
//
enum
{ FWRD, RIGHT, BKWD, LEFT };
int moveorder[4] = { LEFT, RIGHT, FWRD, BKWD };

void
EnterCtrlData (int index, CustomCtrls * cust, void (*DrawRtn) (int), void (*PrintRtn) (int),
               int type)
{
    int j, exit, tick, redraw, which = 0, x, picked, lastFlashTime;
    ControlInfo ci;


    ShootSnd ();
    PrintY = CST_Y + 13 * index;
    IN_ClearKeysDown ();
    exit = 0;
    redraw = 1;
    //
    // FIND FIRST SPOT IN ALLOWED ARRAY
    //
    for (j = 0; j < 4; j++)
        if (cust->allowed[j])
        {
            which = j;
            break;
        }

    do
    {
        if (redraw)
        {
            x = CST_START + CST_SPC * which;
            DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);

            DrawRtn (1);
            DrawWindow (x - 2, PrintY, CST_SPC, 11, TEXTCOLOR);
            DrawOutline (x - 2, PrintY, CST_SPC, 11, 0, HIGHLIGHT);
            SETFONTCOLOR (0, TEXTCOLOR);
            PrintRtn (which);
            PrintX = x;
            SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
            VW_UpdateScreen ();
            WaitKeyUp ();
            redraw = 0;
        }

        SDL_Delay(5);
        ReadAnyControl (&ci);

        // [FG] extended "Customize" menus
        if (type == MOUSE || type == JOYSTICK || type == JOYSTICK2 || type == JOYSTICK3)
            if (IN_KeyDown (sc_Enter) || IN_KeyDown (sc_Control) || IN_KeyDown (sc_Alt))
            {
                IN_ClearKeysDown ();
                ci.button0 = ci.button1 = false;
            }

        //
        // CHANGE BUTTON VALUE?
        //
        // [FG] extended "Customize" menus, support up to 32 joystick buttons
        if (((type != KEYBOARD4BTNS && type != KEYBOARD3BTNS && type != KEYBOARD2BTNS &&
              type != KEYBOARDBTNS && type != KEYBOARDMOVE) && (ci.button0 | ci.button1 | ci.button2 | ci.button3 | IN_JoyButtons())) ||
            ((type == KEYBOARD4BTNS || type == KEYBOARD3BTNS || type == KEYBOARD2BTNS ||
              type == KEYBOARDBTNS || type == KEYBOARDMOVE) && LastScan == sc_Enter))
        {
            lastFlashTime = GetTimeCount();
            tick = picked = 0;
            SETFONTCOLOR (0, TEXTCOLOR);

            // [FG] extended "Customize" menus
            if (type == KEYBOARD4BTNS || type == KEYBOARD3BTNS || type == KEYBOARD2BTNS ||
                type == KEYBOARDBTNS || type == KEYBOARDMOVE)
                IN_ClearKeysDown ();

            while(1)
            {
                int button, result = 0;

                //
                // FLASH CURSOR
                //
                if (GetTimeCount() - lastFlashTime > 10)
                {
                    switch (tick)
                    {
                        case 0:
                            VWB_Bar (x, PrintY + 1, CST_SPC - 2, 10, TEXTCOLOR);
                            break;
                        case 1:
                            PrintX = x;
                            US_Print ("?");
                            SD_PlaySound (HITWALLSND);
                    }
                    tick ^= 1;
                    lastFlashTime = GetTimeCount();
                    VW_UpdateScreen ();
                }
                else SDL_Delay(5);

                //
                // WHICH TYPE OF INPUT DO WE PROCESS?
                //
                switch (type)
                {
                    case MOUSE:
                        button = IN_MouseButtons();
                        switch (button)
                        {
                            case 1:
                                result = 1;
                                break;
                            case 2:
                                result = 2;
                                break;
                            case 4:
                                result = 3;
                                break;
                        }

                        if (result)
                        {
                            for (int z = 0; z < 4; z++)
                                if (order[which] == buttonmouse[z])
                                {
                                    buttonmouse[z] = bt_nobutton;
                                    break;
                                }

                            buttonmouse[result - 1] = order[which];
                            picked = 1;
                            SD_PlaySound (SHOOTDOORSND);
                        }
                        break;

                    case JOYSTICK:
                        button = IN_JoyButtons();
                        // [FG] support up to 32 joystick buttons
                        for (int i = 0; i < 32; i++)
                        {
                          if (button & (1 << i))
                          {
                            result = i + 1;
                            break;
                          }
                        }

                        if (result)
                        {
                            // [FG] support up to 32 joystick buttons
                            for (int z = 0; z < 32; z++)
                            {
                                if (order[which] == buttonjoy[z])
                                {
                                    buttonjoy[z] = bt_nobutton;
                                    break;
                                }
                            }

                            buttonjoy[result - 1] = order[which];
                            picked = 1;
                            SD_PlaySound (SHOOTDOORSND);

                        }
                        break;

                    // [FG] joystick buttons 2 (Wp+, Wp-, Menu, Pause)
                    case JOYSTICK2:
                        button = IN_JoyButtons();
                        // [FG] support up to 32 joystick buttons
                        for (int i = 0; i < 32; i++)
                        {
                          if (button & (1 << i))
                          {
                            result = i + 1;
                            break;
                          }
                        }

                        if (result)
                        {
                            // [FG] support up to 32 joystick buttons
                            for (int z = 0; z < 32; z++)
                            {
                                if (which+8 == buttonjoy[z])
                                {
                                    buttonjoy[z] = bt_nobutton;
                                    break;
                                }
                            }

                            buttonjoy[result - 1] = which+8;
                            picked = 1;
                            SD_PlaySound (SHOOTDOORSND);
                        }
                        break;

                    // [FG] joystick buttons 3 (StrLft, StrRgt)
                    case JOYSTICK3:
                        button = IN_JoyButtons();
                        // [FG] support up to 32 joystick buttons
                        for (int i = 0; i < 32; i++)
                        {
                          if (button & (1 << i))
                          {
                            result = i + 1;
                            break;
                          }
                        }

                        if (result)
                        {
                            // [FG] support up to 32 joystick buttons
                            for (int z = 0; z < 32; z++)
                            {
                                if (which+12 == buttonjoy[z])
                                {
                                    buttonjoy[z] = bt_nobutton;
                                    break;
                                }
                            }

                            buttonjoy[result - 1] = which+12;
                            picked = 1;
                            SD_PlaySound (SHOOTDOORSND);
                        }
                        break;

                    case KEYBOARDBTNS:
                        if (LastScan && LastScan != sc_Escape)
                        {
                            buttonscan[order[which]] = LastScan;
                            picked = 1;
                            ShootSnd ();
                            IN_ClearKeysDown ();
                        }
                        break;

                    // [FG] keyboard buttons 2 (Wp1, Wp2, Wp3, Wp4)
                    case KEYBOARD2BTNS:
                        if (LastScan && LastScan != sc_Escape)
                        {
                            buttonscan[which+4] = LastScan;
                            picked = 1;
                            ShootSnd ();
                            IN_ClearKeysDown ();
                        }
                        break;

                    // [FG] keyboard buttons 3 (Wp+, Wp-, Menu, Pause)
                    case KEYBOARD3BTNS:
                        if (LastScan && LastScan != sc_Escape)
                        {
                            buttonscan[which+8] = LastScan;
                            picked = 1;
                            ShootSnd ();
                            IN_ClearKeysDown ();
                        }
                        break;

                    // [FG] keyboard buttons 4 (StrLft, StrRgt)
                    case KEYBOARD4BTNS:
                        if (LastScan && LastScan != sc_Escape)
                        {
                            buttonscan[which+12] = LastScan;
                            picked = 1;
                            ShootSnd ();
                            IN_ClearKeysDown ();
                        }
                        break;

                    case KEYBOARDMOVE:
                        if (LastScan && LastScan != sc_Escape)
                        {
                            dirscan[moveorder[which]] = LastScan;
                            picked = 1;
                            ShootSnd ();
                            IN_ClearKeysDown ();
                        }
                        break;
                }

                //
                // EXIT INPUT?
                //
                if (IN_KeyDown (sc_Escape) || (type != JOYSTICK && ci.button1))
                {
                    picked = 1;
                    SD_PlaySound (ESCPRESSEDSND);
                }

                if(picked) break;

                ReadAnyControl (&ci);
            }

            SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
            redraw = 1;
            WaitKeyUp ();
            continue;
        }

        if (ci.button1 || IN_KeyDown (sc_Escape))
            exit = 1;

        //
        // MOVE TO ANOTHER SPOT?
        //
        switch (ci.dir)
        {
            case dir_West:
                do
                {
                    which--;
                    if (which < 0)
                        which = 3;
                }
                while (!cust->allowed[which]);
                redraw = 1;
                SD_PlaySound (MOVEGUN1SND);
                while (ReadAnyControl (&ci), ci.dir != dir_None) SDL_Delay(5);
                IN_ClearKeysDown ();
                break;

            case dir_East:
                do
                {
                    which++;
                    if (which > 3)
                        which = 0;
                }
                while (!cust->allowed[which]);
                redraw = 1;
                SD_PlaySound (MOVEGUN1SND);
                while (ReadAnyControl (&ci), ci.dir != dir_None) SDL_Delay(5);
                IN_ClearKeysDown ();
                break;
            case dir_North:
            case dir_South:
                exit = 1;
            default:
                break;
        }
    }
    while (!exit);

    SD_PlaySound (ESCPRESSEDSND);
    WaitKeyUp ();
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
}


////////////////////////
//
// FIXUP GUN CURSOR OVERDRAW SHIT
//
// [FG] extended "Customize" menus
void
FixupCustom2 (int w)
{
    static int lastwhich = -1;
    int y = CST_Y + 26 + w * 13;


    VWB_Hlin (7, 32, y - 1, DEACTIVE);
    VWB_Hlin (7, 32, y + 12, BORD2COLOR);
#ifndef SPEAR
    VWB_Hlin (7, 32, y - 2, BORDCOLOR);
    VWB_Hlin (7, 32, y + 13, BORDCOLOR);
#else
    VWB_Hlin (7, 32, y - 2, BORD2COLOR);
    VWB_Hlin (7, 32, y + 13, BORD2COLOR);
#endif

    switch (w)
    {
        case 0:
            DrawCustMouse (1);
            break;
        case 3:
            DrawCustJoy (1);
            break;
        // [FG] joystick buttons 2 (Wp+, Wp-, Menu, Pause)
        case 5:
            DrawCust2Joy (1);
            break;
        // [FG] joystick buttons 3 (StrLft, StrRgt)
        case 7:
            DrawCust3Joy (1);
            break;
    }


    if (lastwhich >= 0)
    {
        y = CST_Y + 26 + lastwhich * 13;
        VWB_Hlin (7, 32, y - 1, DEACTIVE);
        VWB_Hlin (7, 32, y + 12, BORD2COLOR);
#ifndef SPEAR
        VWB_Hlin (7, 32, y - 2, BORDCOLOR);
        VWB_Hlin (7, 32, y + 13, BORDCOLOR);
#else
        VWB_Hlin (7, 32, y - 2, BORD2COLOR);
        VWB_Hlin (7, 32, y + 13, BORD2COLOR);
#endif

        if (lastwhich != w)
            switch (lastwhich)
            {
                case 0:
                    DrawCustMouse (0);
                    break;
                case 3:
                    DrawCustJoy (0);
                    break;
                // [FG] joystick buttons 2 (Wp+, Wp-, Menu, Pause)
                case 5:
                    DrawCust2Joy (0);
                    break;
                // [FG] joystick buttons 3 (StrLft, StrRgt)
                case 7:
                    DrawCust3Joy (0);
                    break;
            }
    }

    lastwhich = w;
}

void
FixupCustom (int w)
{
    static int lastwhich = -1;
    int y = CST_Y + 26 + w * 13;


    VWB_Hlin (7, 32, y - 1, DEACTIVE);
    VWB_Hlin (7, 32, y + 12, BORD2COLOR);
#ifndef SPEAR
    VWB_Hlin (7, 32, y - 2, BORDCOLOR);
    VWB_Hlin (7, 32, y + 13, BORDCOLOR);
#else
    VWB_Hlin (7, 32, y - 2, BORD2COLOR);
    VWB_Hlin (7, 32, y + 13, BORD2COLOR);
#endif

    switch (w)
    {
        case 0:
            DrawCustKeybd (1);
            break;
        case 2:
            DrawCustKeys (1);
            break;
        // [FG] keyboard buttons 2 (Wp1, Wp2, Wp3, Wp4)
        case 4:
            DrawCust2Keybd (1);
            break;
        // [FG] keyboard buttons 3 (Wp+, Wp-, Menu, Pause)
        case 6:
            DrawCust3Keybd (1);
            break;
        // [FG] keyboard buttons 4 (StrLft, StrRgt)
        case 8:
            DrawCust4Keybd (1);
            break;
    }


    if (lastwhich >= 0)
    {
        y = CST_Y + 26 + lastwhich * 13;
        VWB_Hlin (7, 32, y - 1, DEACTIVE);
        VWB_Hlin (7, 32, y + 12, BORD2COLOR);
#ifndef SPEAR
        VWB_Hlin (7, 32, y - 2, BORDCOLOR);
        VWB_Hlin (7, 32, y + 13, BORDCOLOR);
#else
        VWB_Hlin (7, 32, y - 2, BORD2COLOR);
        VWB_Hlin (7, 32, y + 13, BORD2COLOR);
#endif

        if (lastwhich != w)
            switch (lastwhich)
            {
        case 0:
            DrawCustKeybd (0);
            break;
        case 2:
            DrawCustKeys (0);
            break;
        // [FG] keyboard buttons 2 (Wp1, Wp2, Wp3, Wp4)
        case 4:
            DrawCust2Keybd (0);
            break;
        // [FG] keyboard buttons 3 (Wp+, Wp-, Menu, Pause)
        case 6:
            DrawCust3Keybd (0);
            break;
        // [FG] keyboard buttons 4 (StrLft, StrRgt)
        case 8:
            DrawCust4Keybd (0);
            break;
            }
    }

    lastwhich = w;
}


////////////////////////
//
// DRAW CUSTOMIZE SCREEN
//
// [FG] extended "Customize" menus
void
DrawCustom2Screen (void)
{
    int i;


    ClearMScreen ();
    WindowX = 0;
    WindowW = 320;
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);
    DrawStripes (10);
    VWB_DrawPic (80, 0, C_CUSTOMIZEPIC);
    PrintY = CST_Y;

    //
    // MOUSE
    //
    SETFONTCOLOR (READCOLOR, BKGDCOLOR);
    WindowX = 0;
    WindowW = 320;

#ifndef SPEAR
    PrintY = CST_Y;
    US_CPrint ("Mouse\n");
#else
    PrintY = CST_Y + 13;
    VWB_DrawPic (128, 48, C_MOUSEPIC);
#endif

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print (STR_CRUN);
    PrintX = CST_START + CST_SPC * 1;
    US_Print (STR_COPEN);
    PrintX = CST_START + CST_SPC * 2;
    US_Print (STR_CFIRE);
    PrintX = CST_START + CST_SPC * 3;
    US_Print (STR_CSTRAFE "\n");

    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCustMouse (0);
    US_Print ("\n");


    //
    // JOYSTICK/PAD
    //
#ifndef SPEAR
    SETFONTCOLOR (READCOLOR, BKGDCOLOR);
    US_CPrint ("Joystick/Gravis GamePad\n");
#else
    PrintY += 13;
    VWB_DrawPic (40, 88, C_JOYSTICKPIC);
#endif

#ifdef SPEAR
    VWB_DrawPic (112, 120, C_KEYBOARDPIC);
#endif

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print (STR_CRUN);
    PrintX = CST_START + CST_SPC * 1;
    US_Print (STR_COPEN);
    PrintX = CST_START + CST_SPC * 2;
    US_Print (STR_CFIRE);
    PrintX = CST_START + CST_SPC * 3;
    US_Print (STR_CSTRAFE "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCustJoy (0);
    US_Print ("\n");

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print ("Wp+");
    PrintX = CST_START + CST_SPC * 1;
    US_Print ("Wp-");
    PrintX = CST_START + CST_SPC * 2;
    US_Print ("Menu");
    PrintX = CST_START + CST_SPC * 3;
    US_Print ("Pause" "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCust2Joy (0);
    US_Print ("\n");

    PrintX = CST_START;
    US_Print ("StrLft");
    PrintX = CST_START + CST_SPC * 1;
    US_Print ("StrRgt" "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCust3Joy (0);
    US_Print ("\n");

    //
    // PICK STARTING POINT IN MENU
    //
    if (Cus2Items.curpos < 0)
        for (i = 0; i < Cus2Items.amount; i++)
            if (Cus2Menu[i].active)
            {
                Cus2Items.curpos = i;
                break;
            }


    VW_UpdateScreen ();
    MenuFadeIn ();
}

void
DrawCustomScreen (void)
{
    int i;


    ClearMScreen ();
    WindowX = 0;
    WindowW = 320;
    VWB_DrawPic (112, 184, C_MOUSELBACKPIC);
    DrawStripes (10);
    VWB_DrawPic (80, 0, C_CUSTOMIZEPIC);
    PrintY = CST_Y;

    //
    // KEYBOARD
    //
#ifndef SPEAR
    SETFONTCOLOR (READCOLOR, BKGDCOLOR);
    US_CPrint ("Keyboard\n");
#else
    PrintY += 13;
#endif
    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print (STR_CRUN);
    PrintX = CST_START + CST_SPC * 1;
    US_Print (STR_COPEN);
    PrintX = CST_START + CST_SPC * 2;
    US_Print (STR_CFIRE);
    PrintX = CST_START + CST_SPC * 3;
    US_Print (STR_CSTRAFE "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCustKeybd (0);
    US_Print ("\n");


    //
    // KEYBOARD MOVE KEYS
    //
    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print (STR_LEFT);
    PrintX = CST_START + CST_SPC * 1;
    US_Print (STR_RIGHT);
    PrintX = CST_START + CST_SPC * 2;
    US_Print (STR_FRWD);
    PrintX = CST_START + CST_SPC * 3;
    US_Print (STR_BKWD "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCustKeys (0);
    US_Print ("\n");

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print ("Wp1");
    PrintX = CST_START + CST_SPC * 1;
    US_Print ("Wp2");
    PrintX = CST_START + CST_SPC * 2;
    US_Print ("Wp3");
    PrintX = CST_START + CST_SPC * 3;
    US_Print ("Wp4" "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCust2Keybd (0);
    US_Print ("\n");

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print ("Wp+");
    PrintX = CST_START + CST_SPC * 1;
    US_Print ("Wp-");
    PrintX = CST_START + CST_SPC * 2;
    US_Print ("Menu");
    PrintX = CST_START + CST_SPC * 3;
    US_Print ("Pause" "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCust3Keybd (0);
    US_Print ("\n");

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    PrintX = CST_START;
    US_Print ("StrLft");
    PrintX = CST_START + CST_SPC * 1;
    US_Print ("StrRgt" "\n");
    DrawWindow (5, PrintY - 1, 310, 13, BKGDCOLOR);
    DrawCust4Keybd (0);
    US_Print ("\n");

    //
    // PICK STARTING POINT IN MENU
    //
    if (CusItems.curpos < 0)
        for (i = 0; i < CusItems.amount; i++)
            if (CusMenu[i].active)
            {
                CusItems.curpos = i;
                break;
            }


    VW_UpdateScreen ();
    MenuFadeIn ();
}


void
PrintCustMouse (int i)
{
    int j;

    for (j = 0; j < 4; j++)
        if (order[i] == buttonmouse[j])
        {
            PrintX = CST_START + CST_SPC * i;
            US_Print (mbarray[j]);
            break;
        }
}

void
DrawCustMouse (int hilight)
{
    int i, color;


    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    if (!mouseenabled)
    {
        SETFONTCOLOR (DEACTIVE, BKGDCOLOR);
        CusMenu[0].active = 0;
    }
    else
        CusMenu[0].active = 1;

    PrintY = CST_Y + 13 * 2;
    for (i = 0; i < 4; i++)
        PrintCustMouse (i);
}

void
PrintCustJoy (int i)
{
    // [FG] support up to 32 joystick buttons
    for (int j = 0; j < 32; j++)
    {
        if (order[i] == buttonjoy[j])
        {
            PrintX = CST_START + CST_SPC * i;
            US_Print (mbarray[j]);
            break;
        }
    }
}

// [FG] joystick buttons 2 (Wp+, Wp-, Menu, Pause)
void
PrintCust2Joy (int i)
{
    // [FG] support up to 32 joystick buttons
    for (int j = 0; j < 32; j++)
    {
        if (i + 8 == buttonjoy[j])
        {
            PrintX = CST_START + CST_SPC * i;
            US_Print (mbarray[j]);
            break;
        }
    }
}

// [FG] joystick buttons 3 (StrLft, StrRgt)
void
PrintCust3Joy (int i)
{
    // [FG] support up to 32 joystick buttons
    for (int j = 0; j < 32; j++)
    {
        if (i + 12 == buttonjoy[j])
        {
            PrintX = CST_START + CST_SPC * i;
            US_Print (mbarray[j]);
            break;
        }
    }
}

void
DrawCustJoy (int hilight)
{
    int i, color;

    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    if (!joystickenabled)
    {
        SETFONTCOLOR (DEACTIVE, BKGDCOLOR);
        Cus2Menu[3].active = 0;
    }
    else
        Cus2Menu[3].active = 1;

    // [FG] redraw other joystick buttons lines
    if (hilight)
    {
        DrawCust2Joy(0);
        DrawCust3Joy(0);
    }

    PrintY = CST_Y + 13 * 5;
    VWB_Bar(CST_START, PrintY - 1, 310 - CST_START + 5, 13, BKGDCOLOR);
    for (i = 0; i < 4; i++)
        PrintCustJoy (i);
}

// [FG] joystick buttons 2 (Wp+, Wp-, Menu, Pause)
void
DrawCust2Joy (int hilight)
{
    int i, color;

    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    if (!joystickenabled)
    {
        SETFONTCOLOR (DEACTIVE, BKGDCOLOR);
        Cus2Menu[5].active = 0;
    }
    else
        Cus2Menu[5].active = 1;

    // [FG] redraw other joystick buttons lines
    if (hilight)
    {
        DrawCustJoy(0);
        DrawCust3Joy(0);
    }

    PrintY = CST_Y + 13 * 7;
    VWB_Bar(CST_START, PrintY - 1, 310 - CST_START + 5, 13, BKGDCOLOR);
    for (i = 0; i < 4; i++)
        PrintCust2Joy (i);
}

// [FG] joystick buttons 3 (StrLft, StrRgt)
void
DrawCust3Joy (int hilight)
{
    int i, color;

    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    if (!joystickenabled)
    {
        SETFONTCOLOR (DEACTIVE, BKGDCOLOR);
        Cus2Menu[7].active = 0;
    }
    else
        Cus2Menu[7].active = 1;

    // [FG] redraw other joystick buttons lines
    if (hilight)
    {
        DrawCustJoy(0);
        DrawCust2Joy(0);
    }

    PrintY = CST_Y + 13 * 9;
    VWB_Bar(CST_START, PrintY - 1, 310 - CST_START + 5, 13, BKGDCOLOR);
    for (i = 0; i < 4; i++)
        PrintCust3Joy (i);
}


void
PrintCustKeybd (int i)
{
    PrintX = CST_START + CST_SPC * i;
    US_Print ((const char *) IN_GetScanName (buttonscan[order[i]]));
}

// [FG] keyboard buttons 2 (Wp1, Wp2, Wp3, Wp4)
void
PrintCust2Keybd (int i)
{
    PrintX = CST_START + CST_SPC * i;
    US_Print ((const char *) IN_GetScanName (buttonscan[i+4]));
}

// [FG] keyboard buttons 3 (Wp+, Wp-, Menu, Pause)
void
PrintCust3Keybd (int i)
{
    PrintX = CST_START + CST_SPC * i;
    US_Print ((const char *) IN_GetScanName (buttonscan[i+8]));
}

// [FG] keyboard buttons 4 (StrLft, StrRgt)
void
PrintCust4Keybd (int i)
{
    PrintX = CST_START + CST_SPC * i;
    US_Print ((const char *) IN_GetScanName (buttonscan[i+12]));
}

void
DrawCustKeybd (int hilight)
{
    int i, color;


    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    PrintY = CST_Y + 13 * 2;
    for (i = 0; i < 4; i++)
        PrintCustKeybd (i);
}

// [FG] keyboard buttons 2 (Wp1, Wp2, Wp3, Wp4)
void
DrawCust2Keybd (int hilight)
{
    int i, color;


    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    PrintY = CST_Y + 13 * 6;
    for (i = 0; i < 4; i++)
        PrintCust2Keybd (i);
}

// [FG] keyboard buttons 3 (Wp+, Wp-, Menu, Pause)
void
DrawCust3Keybd (int hilight)
{
    int i, color;


    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    PrintY = CST_Y + 13 * 8;
    for (i = 0; i < 4; i++)
        PrintCust3Keybd (i);
}

// [FG] keyboard buttons 4 (StrLft, StrRgt)
void
DrawCust4Keybd (int hilight)
{
    int i, color;


    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    PrintY = CST_Y + 13 * 10;
    for (i = 0; i < 2; i++)
        PrintCust4Keybd (i);
}

void
PrintCustKeys (int i)
{
    PrintX = CST_START + CST_SPC * i;
    US_Print ((const char *) IN_GetScanName (dirscan[moveorder[i]]));
}

void
DrawCustKeys (int hilight)
{
    int i, color;


    color = TEXTCOLOR;
    if (hilight)
        color = HIGHLIGHT;
    SETFONTCOLOR (color, BKGDCOLOR);

    PrintY = CST_Y + 13 * 4;
    for (i = 0; i < 4; i++)
        PrintCustKeys (i);
}


////////////////////////////////////////////////////////////////////
//
// CHANGE SCREEN VIEWING SIZE
//
////////////////////////////////////////////////////////////////////
int
CP_ChangeView (int)
{
    int exit = 0, oldview, newview;
    ControlInfo ci;

    WindowX = WindowY = 0;
    WindowW = 320;
    WindowH = 200;
    newview = oldview = viewsize;
    DrawChangeView (oldview);
    MenuFadeIn ();

    do
    {
        CheckPause ();
        SDL_Delay(5);
        ReadAnyControl (&ci);
        switch (ci.dir)
        {
            case dir_South:
            case dir_West:
                newview--;
                if (newview < 4)
                    newview = 4;
                if(newview >= 19) DrawChangeView(newview);
                else ShowViewSize (newview);
                VW_UpdateScreen ();
                SD_PlaySound (HITWALLSND);
                TicDelay (10);
                break;

            case dir_North:
            case dir_East:
                newview++;
                if (newview >= 21)
                {
                    newview = 21;
                    DrawChangeView(newview);
                }
                else ShowViewSize (newview);
                VW_UpdateScreen ();
                SD_PlaySound (HITWALLSND);
                TicDelay (10);
                break;

            default:
                break;
        }

        if (ci.button0 || Keyboard[sc_Enter])
            exit = 1;
        else if (ci.button1 || Keyboard[sc_Escape])
        {
            SD_PlaySound (ESCPRESSEDSND);
            MenuFadeOut ();
            if(screenHeight % 200 != 0)
                VL_ClearScreen(0);
            return 0;
        }
    }
    while (!exit);

    if (oldview != newview)
    {
        SD_PlaySound (SHOOTSND);
        Message (STR_THINK "...");
        NewViewSize (newview);
    }

    ShootSnd ();
    MenuFadeOut ();
    if(screenHeight % 200 != 0)
        VL_ClearScreen(0);

    return 0;
}


/////////////////////////////
//
// DRAW THE CHANGEVIEW SCREEN
//
void
DrawChangeView (int view)
{
    int rescaledHeight = screenHeight / scaleFactor;
    if(view != 21) VWB_Bar (0, rescaledHeight - 40, 320, 40, bordercol);

    ShowViewSize (view);

    PrintY = (screenHeight / scaleFactor) - 39;
    WindowX = 0;
    WindowY = 320;                                  // TODO: Check this!
    SETFONTCOLOR (HIGHLIGHT, BKGDCOLOR);

    US_CPrint (STR_SIZE1 "\n");
    US_CPrint (STR_SIZE2 "\n");
    US_CPrint (STR_SIZE3);
    VW_UpdateScreen ();
}


////////////////////////////////////////////////////////////////////
//
// QUIT THIS INFERNAL GAME!
//
////////////////////////////////////////////////////////////////////
int
CP_Quit (int)
{
    /* T-Embed-Anpassung: kein Y/N-Dialog — User will direkt raus.
     * Quit() ruft via longjmp() unseren wolf3d_app cleanup-pfad. */
    VW_UpdateScreen ();
    SD_MusicOff ();
    SD_StopSound ();
    MenuFadeOut ();
    Quit (NULL);
    return 0;  /* unreachable */
}


////////////////////////////////////////////////////////////////////
//
// HANDLE INTRO SCREEN (SYSTEM CONFIG)
//
////////////////////////////////////////////////////////////////////
void
IntroScreen (void)
{
#ifdef SPEAR

#define MAINCOLOR       0x4f
#define EMSCOLOR        0x4f
#define XMSCOLOR        0x4f

#else

#define MAINCOLOR       0x6c
#define EMSCOLOR        0x6c    // 0x4f
#define XMSCOLOR        0x6c    // 0x7f

#endif
#define FILLCOLOR       14

//      long memory;
//      long emshere,xmshere;
    int i;
/*      int ems[10]={100,200,300,400,500,600,700,800,900,1000},
                xms[10]={100,200,300,400,500,600,700,800,900,1000};
        int main[10]={32,64,96,128,160,192,224,256,288,320};*/


    //
    // DRAW MAIN MEMORY
    //
#ifdef ABCAUS
    memory = (1023l + mminfo.nearheap + mminfo.farheap) / 1024l;
    for (i = 0; i < 10; i++)
        if (memory >= main[i])
            VWB_Bar (49, 163 - 8 * i, 6, 5, MAINCOLOR - i);

    //
    // DRAW EMS MEMORY
    //
    if (EMSPresent)
    {
        emshere = 4l * EMSPagesAvail;
        for (i = 0; i < 10; i++)
            if (emshere >= ems[i])
                VWB_Bar (89, 163 - 8 * i, 6, 5, EMSCOLOR - i);
    }

    //
    // DRAW XMS MEMORY
    //
    if (XMSPresent)
    {
        xmshere = 4l * XMSPagesAvail;
        for (i = 0; i < 10; i++)
            if (xmshere >= xms[i])
                VWB_Bar (129, 163 - 8 * i, 6, 5, XMSCOLOR - i);
    }
#else
    for (i = 0; i < 10; i++)
        VWB_Bar (49, 163 - 8 * i, 6, 5, MAINCOLOR - i);
    for (i = 0; i < 10; i++)
        VWB_Bar (89, 163 - 8 * i, 6, 5, EMSCOLOR - i);
    for (i = 0; i < 10; i++)
        VWB_Bar (129, 163 - 8 * i, 6, 5, XMSCOLOR - i);
#endif


    //
    // FILL BOXES
    //
    if (MousePresent)
        VWB_Bar (164, 82, 12, 2, FILLCOLOR);

    if (IN_JoyPresent())
        VWB_Bar (164, 105, 12, 2, FILLCOLOR);

    if (AdLibPresent && !SoundBlasterPresent)
        VWB_Bar (164, 128, 12, 2, FILLCOLOR);

    if (SoundBlasterPresent)
        VWB_Bar (164, 151, 12, 2, FILLCOLOR);

//    if (SoundSourcePresent)
//        VWB_Bar (164, 174, 12, 2, FILLCOLOR);
}


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
//
// SUPPORT ROUTINES
//
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////
//
// Clear Menu screens to dark red
//
////////////////////////////////////////////////////////////////////
void
ClearMScreen (void)
{
#ifndef SPEAR
    VWB_Bar (0, 0, 320, 200, BORDCOLOR);
#else
    VWB_DrawPic (0, 0, C_BACKDROPPIC);
#endif
}


////////////////////////////////////////////////////////////////////
//
// Un/Cache a LUMP of graphics
//
////////////////////////////////////////////////////////////////////
void
CacheLump (int lumpstart, int lumpend)
{
    int i;

    for (i = lumpstart; i <= lumpend; i++)
        CA_CacheGrChunk (i);
}


void
UnCacheLump (int lumpstart, int lumpend)
{
    int i;

    for (i = lumpstart; i <= lumpend; i++)
        if (grsegs[i])
            UNCACHEGRCHUNK (i);
}


////////////////////////////////////////////////////////////////////
//
// Draw a window for a menu
//
////////////////////////////////////////////////////////////////////
void
DrawWindow (int x, int y, int w, int h, int wcolor)
{
    VWB_Bar (x, y, w, h, wcolor);
    DrawOutline (x, y, w, h, BORD2COLOR, DEACTIVE);
}


void
DrawOutline (int x, int y, int w, int h, int color1, int color2)
{
    VWB_Hlin (x, x + w, y, color2);
    VWB_Vlin (y, y + h, x, color2);
    VWB_Hlin (x, x + w, y + h, color1);
    VWB_Vlin (y, y + h, x + w, color1);
}


////////////////////////////////////////////////////////////////////
//
// Setup Control Panel stuff - graphics, etc.
//
////////////////////////////////////////////////////////////////////
void
SetupControlPanel (void)
{
    //
    // CACHE GRAPHICS & SOUNDS
    //
    CA_CacheGrChunk (STARTFONT + 1);
#ifndef SPEAR
    CacheLump (CONTROLS_LUMP_START, CONTROLS_LUMP_END);
#else
    CacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
#endif

    SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
    fontnumber = 1;
    WindowH = 200;
    if(screenHeight % 200 != 0)
        VL_ClearScreen(0);

    if (!ingame)
        CA_LoadAllSounds ();
    else
        MainMenu[savegame].active = 1;

    menuactive = true;
    IN_UpdateGrab();
}

////////////////////////////////////////////////////////////////////
//
// SEE WHICH SAVE GAME FILES ARE AVAILABLE & READ STRING IN
//
////////////////////////////////////////////////////////////////////
void SetupSaveGames()
{
    char name[13];
    char savepath[300];

    strcpy(name, SaveName);
    for(int i = 0; i < 10; i++)
    {
        name[7] = '0' + i;
            if(configdir[0])
                snprintf(savepath, sizeof(savepath), "%s/%s", configdir, name);
            else
                strcpy(savepath, name);

            const int handle = open(savepath, O_RDONLY | O_BINARY);
            if(handle >= 0)
            {
                char temp[32];

                SaveGamesAvail[i] = 1;
                if (read(handle, temp, 32) < 0)
                {
                    SaveGamesAvail[i] = 0;
                    close(handle);
                    continue;
                }
                close(handle);
                strcpy(&SaveGameNames[i][0], temp);
            }
    }
}

////////////////////////////////////////////////////////////////////
//
// Clean up all the Control Panel stuff
//
////////////////////////////////////////////////////////////////////
void
CleanupControlPanel (void)
{
#ifndef SPEAR
    UnCacheLump (CONTROLS_LUMP_START, CONTROLS_LUMP_END);
#else
    UnCacheLump (BACKDROP_LUMP_START, BACKDROP_LUMP_END);
#endif

    fontnumber = 0;

    menuactive = false;
}


////////////////////////////////////////////////////////////////////
//
// Handle moving gun around a menu
//
////////////////////////////////////////////////////////////////////
int
HandleMenu (CP_iteminfo * item_i, CP_itemtype * items, void (*routine) (int w))
{
    char key;
    static int redrawitem = 1, lastitem = -1;
    int i, x, y, basey, exit, which, shape;
    int32_t lastBlinkTime, timer;
    ControlInfo ci;


    which = item_i->curpos;
    x = item_i->x & -8;
    basey = item_i->y - 2;
    y = basey + which * 13;

    VWB_DrawPic (x, y, C_CURSOR1PIC);
    SetTextColor (items + which, 1);
    if (redrawitem)
    {
        PrintX = item_i->x + item_i->indent;
        PrintY = item_i->y + which * 13;
        US_Print ((items + which)->string);
    }
    //
    // CALL CUSTOM ROUTINE IF IT IS NEEDED
    //
    if (routine)
        routine (which);
    VW_UpdateScreen ();

    shape = C_CURSOR1PIC;
    timer = 8;
    exit = 0;
    lastBlinkTime = GetTimeCount ();
    IN_ClearKeysDown ();


    do
    {
        //
        // CHANGE GUN SHAPE
        //
        if ((int32_t)GetTimeCount () - lastBlinkTime > timer)
        {
            lastBlinkTime = GetTimeCount ();
            if (shape == C_CURSOR1PIC)
            {
                shape = C_CURSOR2PIC;
                timer = 8;
            }
            else
            {
                shape = C_CURSOR1PIC;
                timer = 70;
            }
            VWB_DrawPic (x, y, shape);
            if (routine)
                routine (which);
            VW_UpdateScreen ();
        }
        else SDL_Delay(5);

        CheckPause ();

        //
        // SEE IF ANY KEYS ARE PRESSED FOR INITIAL CHAR FINDING
        //
        key = LastASCII;
        if (key)
        {
            int ok = 0;

            if (key >= 'a')
                key -= 'a' - 'A';

            for (i = which + 1; i < item_i->amount; i++)
                if ((items + i)->active && (items + i)->string[0] == key)
                {
                    EraseGun (item_i, items, x, y, which);
                    which = i;
                    DrawGun (item_i, items, x, &y, which, basey, routine);
                    ok = 1;
                    IN_ClearKeysDown ();
                    break;
                }

            //
            // DIDN'T FIND A MATCH FIRST TIME THRU. CHECK AGAIN.
            //
            if (!ok)
            {
                for (i = 0; i < which; i++)
                    if ((items + i)->active && (items + i)->string[0] == key)
                    {
                        EraseGun (item_i, items, x, y, which);
                        which = i;
                        DrawGun (item_i, items, x, &y, which, basey, routine);
                        IN_ClearKeysDown ();
                        break;
                    }
            }
        }

        //
        // GET INPUT
        //
        ReadAnyControl (&ci);
        switch (ci.dir)
        {
                ////////////////////////////////////////////////
                //
                // MOVE UP
                //
            case dir_North:

                EraseGun (item_i, items, x, y, which);

                //
                // ANIMATE HALF-STEP
                //
                if (which && (items + which - 1)->active)
                {
                    y -= 6;
                    DrawHalfStep (x, y);
                }

                //
                // MOVE TO NEXT AVAILABLE SPOT
                //
                do
                {
                    if (!which)
                        which = item_i->amount - 1;
                    else
                        which--;
                }
                while (!(items + which)->active);

                DrawGun (item_i, items, x, &y, which, basey, routine);
                //
                // WAIT FOR BUTTON-UP OR DELAY NEXT MOVE
                //
                TicDelay (20);
                break;

                ////////////////////////////////////////////////
                //
                // MOVE DOWN
                //
            case dir_South:

                EraseGun (item_i, items, x, y, which);
                //
                // ANIMATE HALF-STEP
                //
                if (which != item_i->amount - 1 && (items + which + 1)->active)
                {
                    y += 6;
                    DrawHalfStep (x, y);
                }

                do
                {
                    if (which == item_i->amount - 1)
                        which = 0;
                    else
                        which++;
                }
                while (!(items + which)->active);

                DrawGun (item_i, items, x, &y, which, basey, routine);

                //
                // WAIT FOR BUTTON-UP OR DELAY NEXT MOVE
                //
                TicDelay (20);
                break;

            default:
                break;
        }

        if (ci.button0 || Keyboard[sc_Space] || Keyboard[sc_Enter])
            exit = 1;

        if ((ci.button1 && !Keyboard[sc_Alt]) || Keyboard[sc_Escape])
            exit = 2;

    }
    while (!exit);


    IN_ClearKeysDown ();

    //
    // ERASE EVERYTHING
    //
    if (lastitem != which)
    {
        VWB_Bar (x - 1, y, 25, 16, BKGDCOLOR);
        PrintX = item_i->x + item_i->indent;
        PrintY = item_i->y + which * 13;
        US_Print ((items + which)->string);
        redrawitem = 1;
    }
    else
        redrawitem = 0;

    if (routine)
        routine (which);
    VW_UpdateScreen ();

    item_i->curpos = which;

    lastitem = which;
    switch (exit)
    {
        case 1:
            //
            // CALL THE ROUTINE
            //
            if ((items + which)->routine != NULL)
            {
                ShootSnd ();
                MenuFadeOut ();
                (items + which)->routine (0);
            }
            return which;

        case 2:
            SD_PlaySound (ESCPRESSEDSND);
            return -1;
    }

    return 0;                   // JUST TO SHUT UP THE ERROR MESSAGES!
}


//
// ERASE GUN & DE-HIGHLIGHT STRING
//
void
EraseGun (CP_iteminfo * item_i, CP_itemtype * items, int x, int y, int which)
{
    VWB_Bar (x - 1, y, 25, 16, BKGDCOLOR);
    SetTextColor (items + which, 0);

    PrintX = item_i->x + item_i->indent;
    PrintY = item_i->y + which * 13;
    US_Print ((items + which)->string);
    VW_UpdateScreen ();
}


//
// DRAW HALF STEP OF GUN TO NEXT POSITION
//
void
DrawHalfStep (int x, int y)
{
    VWB_DrawPic (x, y, C_CURSOR1PIC);
    VW_UpdateScreen ();
    SD_PlaySound (MOVEGUN1SND);
    SDL_Delay (8 * 100 / 7);
}


//
// DRAW GUN AT NEW POSITION
//
void
DrawGun (CP_iteminfo * item_i, CP_itemtype * items, int x, int *y, int which, int basey,
         void (*routine) (int w))
{
    VWB_Bar (x - 1, *y, 25, 16, BKGDCOLOR);
    *y = basey + which * 13;
    VWB_DrawPic (x, *y, C_CURSOR1PIC);
    SetTextColor (items + which, 1);

    PrintX = item_i->x + item_i->indent;
    PrintY = item_i->y + which * 13;
    US_Print ((items + which)->string);

    //
    // CALL CUSTOM ROUTINE IF IT IS NEEDED
    //
    if (routine)
        routine (which);
    VW_UpdateScreen ();
    SD_PlaySound (MOVEGUN2SND);
}

////////////////////////////////////////////////////////////////////
//
// DELAY FOR AN AMOUNT OF TICS OR UNTIL CONTROLS ARE INACTIVE
//
////////////////////////////////////////////////////////////////////
void
TicDelay (int count)
{
    ControlInfo ci;

    int32_t startTime = GetTimeCount ();
    do
    {
        SDL_Delay(5);
        ReadAnyControl (&ci);
    }
    while ((int32_t) GetTimeCount () - startTime < count && ci.dir != dir_None);
}


////////////////////////////////////////////////////////////////////
//
// Draw a menu
//
////////////////////////////////////////////////////////////////////
void
DrawMenu (CP_iteminfo * item_i, CP_itemtype * items)
{
    int i, which = item_i->curpos;


    WindowX = PrintX = item_i->x + item_i->indent;
    WindowY = PrintY = item_i->y;
    WindowW = 320;
    WindowH = 200;

    for (i = 0; i < item_i->amount; i++)
    {
        SetTextColor (items + i, which == i);

        PrintY = item_i->y + i * 13;
        if ((items + i)->active)
            US_Print ((items + i)->string);
        else
        {
            SETFONTCOLOR (DEACTIVE, BKGDCOLOR);
            US_Print ((items + i)->string);
            SETFONTCOLOR (TEXTCOLOR, BKGDCOLOR);
        }

        US_Print ("\n");
    }
}


////////////////////////////////////////////////////////////////////
//
// SET TEXT COLOR (HIGHLIGHT OR NO)
//
////////////////////////////////////////////////////////////////////
void
SetTextColor (CP_itemtype * items, int hlight)
{
    if (hlight)
    {
        SETFONTCOLOR (color_hlite[items->active], BKGDCOLOR);
    }
    else
    {
        SETFONTCOLOR (color_norml[items->active], BKGDCOLOR);
    }
}


////////////////////////////////////////////////////////////////////
//
// WAIT FOR CTRLKEY-UP OR BUTTON-UP
//
////////////////////////////////////////////////////////////////////
void
WaitKeyUp (void)
{
    ControlInfo ci;
    while (ReadAnyControl (&ci), ci.button0 |
           ci.button1 |
           ci.button2 | ci.button3 | Keyboard[sc_Space] | Keyboard[sc_Enter] | Keyboard[sc_Escape])
    {
        IN_WaitAndProcessEvents();
    }
}


////////////////////////////////////////////////////////////////////
//
// READ KEYBOARD, JOYSTICK AND MOUSE FOR INPUT
//
////////////////////////////////////////////////////////////////////
void
ReadAnyControl (ControlInfo * ci)
{
    int mouseactive = 0;

    IN_ReadControl (0, ci);

    if (mouseenabled)
    {
        int mousex, mousey, buttons;
        buttons = SDL_GetRelativeMouseState(&mousex, &mousey);
        int middlePressed = buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE);
        int rightPressed = buttons & SDL_BUTTON(SDL_BUTTON_RIGHT);
        buttons &= ~(SDL_BUTTON(SDL_BUTTON_MIDDLE) | SDL_BUTTON(SDL_BUTTON_RIGHT));
        if(middlePressed) buttons |= 1 << 2;
        if(rightPressed) buttons |= 1 << 1;

        if(mousey < -SENSITIVE)
        {
            ci->dir = dir_North;
            mouseactive = 1;
        }
        else if(mousey  > SENSITIVE)
        {
            ci->dir = dir_South;
            mouseactive = 1;
        }

        if(mousex < -SENSITIVE)
        {
            ci->dir = dir_West;
            mouseactive = 1;
        }
        else if(mousex > SENSITIVE)
        {
            ci->dir = dir_East;
            mouseactive = 1;
        }

        if (buttons)
        {
            ci->button0 = buttons & 1;
            ci->button1 = buttons & 2;
            ci->button2 = buttons & 4;
            ci->button3 = false;
            mouseactive = 1;
        }
    }

    if (joystickenabled && !mouseactive)
    {
        int jx, jy, jb;

        IN_GetJoyDelta (&jx, &jy);
        if (jy < -SENSITIVE)
            ci->dir = dir_North;
        else if (jy > SENSITIVE)
            ci->dir = dir_South;

        if (jx < -SENSITIVE)
            ci->dir = dir_West;
        else if (jx > SENSITIVE)
            ci->dir = dir_East;

        jb = IN_JoyButtons ();
        if (jb)
        {
            ci->button0 = jb & 1;
            ci->button1 = jb & 2;
            ci->button2 = jb & 4;
            ci->button3 = jb & 8;
        }
    }
}


////////////////////////////////////////////////////////////////////
//
// DRAW DIALOG AND CONFIRM YES OR NO TO QUESTION
//
////////////////////////////////////////////////////////////////////
int
Confirm (const char *string)
{
    int xit = 0, x, y, tick = 0, lastBlinkTime;
    int whichsnd[2] = { ESCPRESSEDSND, SHOOTSND };
    ControlInfo ci;

    Message (string);
    IN_ClearKeysDown ();
    WaitKeyUp ();

    //
    // BLINK CURSOR
    //
    x = PrintX;
    y = PrintY;
    lastBlinkTime = GetTimeCount();

    do
    {
        ReadAnyControl(&ci);

        if (GetTimeCount() - lastBlinkTime >= 10)
        {
            switch (tick)
            {
                case 0:
                    VWB_Bar (x, y, 8, 13, TEXTCOLOR);
                    break;
                case 1:
                    PrintX = x;
                    PrintY = y;
                    US_Print ("_");
            }
            VW_UpdateScreen ();
            tick ^= 1;
            lastBlinkTime = GetTimeCount();
        }
        else SDL_Delay(5);

    }
    while (!Keyboard[sc_Y] && !Keyboard[sc_N] && !Keyboard[sc_Escape] && !ci.button0 && !ci.button1);

    if (Keyboard[sc_Y] || ci.button0)
    {
        xit = 1;
        ShootSnd ();
    }

    IN_ClearKeysDown ();
    WaitKeyUp ();

    SD_PlaySound ((soundnames) whichsnd[xit]);

    return xit;
}


////////////////////////////////////////////////////////////////////
//
// PRINT A MESSAGE IN A WINDOW
//
////////////////////////////////////////////////////////////////////
void
Message (const char *string)
{
    int h = 0, w = 0, mw = 0, i, len = (int) strlen(string);
    fontstruct *font;


    CA_CacheGrChunk (STARTFONT + 1);
    fontnumber = 1;
    void *p = grsegs[STARTFONT + fontnumber];
    font = (fontstruct *) p;
    h = font->height;
    for (i = 0; i < len; i++)
    {
        if (string[i] == '\n')
        {
            if (w > mw)
                mw = w;
            w = 0;
            h += font->height;
        }
        else
            w += font->width[(size_t)string[i]];
    }

    if (w + 10 > mw)
        mw = w + 10;

    PrintY = (WindowH / 2) - h / 2;
    PrintX = WindowX = 160 - mw / 2;

    DrawWindow (WindowX - 5, PrintY - 5, mw + 10, h + 10, TEXTCOLOR);
    DrawOutline (WindowX - 5, PrintY - 5, mw + 10, h + 10, 0, HIGHLIGHT);
    SETFONTCOLOR (0, TEXTCOLOR);
    US_Print (string);
    VW_UpdateScreen ();
}

////////////////////////////////////////////////////////////////////
//
// THIS MAY BE FIXED A LITTLE LATER...
//
////////////////////////////////////////////////////////////////////
static int lastmusic;

int
StartCPMusic (int song)
{
    int lastoffs;

    lastmusic = song;
    lastoffs = SD_MusicOff ();
    UNCACHEAUDIOCHUNK (STARTMUSIC + lastmusic);

    SD_StartMusic(STARTMUSIC + song);
    return lastoffs;
}

void
FreeMusic (void)
{
    UNCACHEAUDIOCHUNK (STARTMUSIC + lastmusic);
}


///////////////////////////////////////////////////////////////////////////
//
//      IN_GetScanName() - Returns a string containing the name of the
//              specified scan code
//
///////////////////////////////////////////////////////////////////////////
const char *
IN_GetScanName (ScanCode scan)
{
/*    const char **p;
    ScanCode *s;

    for (s = ExtScanCodes, p = ExtScanNames; *s; p++, s++)
        if (*s == scan)
            return (*p);*/

    auto it = ScanNames.find(scan);
    if (it == ScanNames.end())
    {
        return "?";
    }
    return it->second;
}


///////////////////////////////////////////////////////////////////////////
//
// CHECK FOR PAUSE KEY (FOR MUSIC ONLY)
//
///////////////////////////////////////////////////////////////////////////
void
CheckPause (void)
{
    if (Paused)
    {
        switch (SoundStatus)
        {
            case 0:
                SD_MusicOn ();
                break;
            case 1:
                SD_MusicOff ();
                break;
        }

        SoundStatus ^= 1;
        VW_WaitVBL (3);
        IN_ClearKeysDown ();
        Paused = false;
    }
}


///////////////////////////////////////////////////////////////////////////
//
// DRAW GUN CURSOR AT CORRECT POSITION IN MENU
//
///////////////////////////////////////////////////////////////////////////
void
DrawMenuGun (CP_iteminfo * iteminfo)
{
    int x, y;


    x = iteminfo->x;
    y = iteminfo->y + iteminfo->curpos * 13 - 2;
    VWB_DrawPic (x, y, C_CURSOR1PIC);
}


///////////////////////////////////////////////////////////////////////////
//
// DRAW SCREEN TITLE STRIPES
//
///////////////////////////////////////////////////////////////////////////
void
DrawStripes (int y)
{
#ifndef SPEAR
    VWB_Bar (0, y, 320, 24, 0);
    VWB_Hlin (0, 319, y + 22, STRIPE);
#else
    VWB_Bar (0, y, 320, 22, 0);
    VWB_Hlin (0, 319, y + 23, 0);
#endif
}

void
ShootSnd (void)
{
    SD_PlaySound (SHOOTSND);
}


///////////////////////////////////////////////////////////////////////////
//
// CHECK FOR EPISODES
//
///////////////////////////////////////////////////////////////////////////
void
CheckForEpisodes (void)
{
    struct stat statbuf;

    // On Linux like systems, the configdir defaults to $HOME/.wolf4sdl
#if !defined(_WIN32)
    if(configdir[0] == 0)
    {
        // Set config location to home directory for multi-user support
        char *homedir = getenv("HOME");
        if(homedir == NULL)
        {
            Quit("Your $HOME directory is not defined. You must set this before playing.");
        }
        #define WOLFDIR "/.wolf4sdl"
        if(strlen(homedir) + sizeof(WOLFDIR) > sizeof(configdir))
        {
            Quit("Your $HOME directory path is too long. It cannot be used for saving games.");
        }
        snprintf(configdir, sizeof(configdir), "%s" WOLFDIR, homedir);
    }
#endif

    if(configdir[0] != 0)
    {
        // Ensure config directory exists and create if necessary
        if(stat(configdir, &statbuf) != 0)
        {
#ifdef _WIN32
            if(_mkdir(configdir) != 0)
#else
            if(mkdir(configdir, 0755) != 0)
#endif
            {
                Quit("The configuration directory \"%s\" could not be created.", configdir);
            }
        }
    }


//
// ENGLISH
//
#ifdef UPLOAD
    if(!stat("vswap.wl1", &statbuf))
    {
        strcpy (extension, "wl1");
        numEpisodesMissing = 5;
    }
    else
        Quit ("NO WOLFENSTEIN 3-D DATA FILES to be found!");
#else
#ifndef SPEAR
    if(!stat("vswap.wl6", &statbuf))
    {
        strcpy (extension, "wl6");
        NewEmenu[2].active =
            NewEmenu[4].active =
            NewEmenu[6].active =
            NewEmenu[8].active =
            NewEmenu[10].active =
            EpisodeSelect[1] =
            EpisodeSelect[2] = EpisodeSelect[3] = EpisodeSelect[4] = EpisodeSelect[5] = 1;
    }
    else
    {
        if(!stat("vswap.wl3", &statbuf))
        {
            strcpy (extension, "wl3");
            numEpisodesMissing = 3;
            NewEmenu[2].active = NewEmenu[4].active = EpisodeSelect[1] = EpisodeSelect[2] = 1;
        }
        else
        {
            if(!stat("vswap.wl1", &statbuf))
            {
                strcpy (extension, "wl1");
                numEpisodesMissing = 5;
            }
            else
                Quit ("NO WOLFENSTEIN 3-D DATA FILES to be found!");
        }
    }
#endif
#endif


#ifdef SPEAR
#ifndef SPEARDEMO
    if(param_mission == 0)
    {
        if(!stat("vswap.sod", &statbuf))
            strcpy (extension, "sod");
        else
            Quit ("NO SPEAR OF DESTINY DATA FILES TO BE FOUND!");
    }
    else if(param_mission == 1)
    {
        if(!stat("vswap.sd1", &statbuf))
            strcpy (extension, "sd1");
        else
            Quit ("NO SPEAR OF DESTINY DATA FILES TO BE FOUND!");
    }
    else if(param_mission == 2)
    {
        if(!stat("vswap.sd2", &statbuf))
            strcpy (extension, "sd2");
        else
            Quit ("NO SPEAR OF DESTINY DATA FILES TO BE FOUND!");
    }
    else if(param_mission == 3)
    {
        if(!stat("vswap.sd3", &statbuf))
            strcpy (extension, "sd3");
        else
            Quit ("NO SPEAR OF DESTINY DATA FILES TO BE FOUND!");
    }
    else
        Quit ("UNSUPPORTED MISSION!");
    strcpy (graphext, "sod");
    strcpy (audioext, "sod");
#else
    if(!stat("vswap.sdm", &statbuf))
    {
        strcpy (extension, "sdm");
    }
    else
        Quit ("NO SPEAR OF DESTINY DEMO DATA FILES TO BE FOUND!");
    strcpy (graphext, "sdm");
    strcpy (audioext, "sdm");
#endif
#else
    strcpy (graphext, extension);
    strcpy (audioext, extension);
#endif

    strcat (configname, extension);
    strcat (SaveName, extension);
    strcat (demoname, extension);

#ifndef SPEAR
    strcat (endfilename, extension);
#endif
}
