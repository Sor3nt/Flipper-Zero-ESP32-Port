// WL_ACT2.C

#include <stdio.h>
#include <math.h>
#include "wl_def.h"

/*
=============================================================================

                               LOCAL CONSTANTS

=============================================================================
*/

#define PROJECTILESIZE  0xc000l

#define BJRUNSPEED      2048
#define BJJUMPSPEED     680


/*
=============================================================================

                              GLOBAL VARIABLES

=============================================================================
*/



/*
=============================================================================

                              LOCAL VARIABLES

=============================================================================
*/


dirtype dirtable[9] = {northwest,north,northeast,west,nodir,east,
    southwest,south,southeast};

short starthitpoints[4][NUMENEMIES] =
//
// BABY MODE
//
{
    {
        25,   // guards
        50,   // officer
        100,  // SS
        1,    // dogs
        850,  // Hans
        850,  // Schabbs
        200,  // fake hitler
        800,  // mecha hitler
        45,   // mutants
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts

        850,  // Gretel
        850,  // Gift
        850,  // Fat
        5,    // en_spectre,
        1450, // en_angel,
        850,  // en_trans,
        1050, // en_uber,
        950,  // en_will,
        1250  // en_death
    },
    //
    // DON'T HURT ME MODE
    //
    {
        25,   // guards
        50,   // officer
        100,  // SS
        1,    // dogs
        950,  // Hans
        950,  // Schabbs
        300,  // fake hitler
        950,  // mecha hitler
        55,   // mutants
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts

        950,  // Gretel
        950,  // Gift
        950,  // Fat
        10,   // en_spectre,
        1550, // en_angel,
        950,  // en_trans,
        1150, // en_uber,
        1050, // en_will,
        1350  // en_death
    },
    //
    // BRING 'EM ON MODE
    //
    {
        25,   // guards
        50,   // officer
        100,  // SS
        1,    // dogs

        1050, // Hans
        1550, // Schabbs
        400,  // fake hitler
        1050, // mecha hitler

        55,   // mutants
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts

        1050, // Gretel
        1050, // Gift
        1050, // Fat
        15,   // en_spectre,
        1650, // en_angel,
        1050, // en_trans,
        1250, // en_uber,
        1150, // en_will,
        1450  // en_death
    },
    //
    // DEATH INCARNATE MODE
    //
    {
        25,   // guards
        50,   // officer
        100,  // SS
        1,    // dogs

        1200, // Hans
        2400, // Schabbs
        500,  // fake hitler
        1200, // mecha hitler

        65,   // mutants
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts
        25,   // ghosts

        1200, // Gretel
        1200, // Gift
        1200, // Fat
        25,   // en_spectre,
        2000, // en_angel,
        1200, // en_trans,
        1400, // en_uber,
        1300, // en_will,
        1600  // en_death
    }
};

void    T_Shoot (objtype *ob);

/*
=================
=
= A_Smoke
=
=================
*/

void A_Smoke (objtype *ob)
{
    GetNewActor ();
#ifdef SPEAR
    if (ob->obclass == hrocketobj)
        newobj->state = &states[s_hsmoke1];
    else
#endif
        newobj->state = &states[s_smoke1];
    newobj->ticcount = 6;

    newobj->tilex = ob->tilex;
    newobj->tiley = ob->tiley;
    newobj->x = ob->x;
    newobj->y = ob->y;
    newobj->obclass = inertobj;
    newobj->active = ac_yes;

    newobj->flags = FL_NEVERMARK;
}


/*
===================
=
= ProjectileTryMove
=
= returns true if move ok
===================
*/

#define PROJSIZE        0x2000

boolean ProjectileTryMove (objtype *ob)
{
    int      xl,yl,xh,yh,x,y;
    objtype *check;

    xl = (ob->x-PROJSIZE) >> TILESHIFT;
    yl = (ob->y-PROJSIZE) >> TILESHIFT;

    xh = (ob->x+PROJSIZE) >> TILESHIFT;
    yh = (ob->y+PROJSIZE) >> TILESHIFT;

    //
    // check for solid walls
    //
    for (y=yl;y<=yh;y++)
    {
        for (x=xl;x<=xh;x++)
        {
            check = actorat[x][y];
            if (check && !ISPOINTER(check))
                return false;
        }
    }

        return true;
}



/*
=================
=
= T_Projectile
=
=================
*/

void T_Projectile (objtype *ob)
{
    int32_t deltax,deltay;
    int     damage = 0;
    int32_t speed;

    speed = (int32_t)ob->speed*tics;

    deltax = FixedMul(speed,costable[ob->angle]);
    deltay = -FixedMul(speed,sintable[ob->angle]);

    if (deltax>0x10000l)
        deltax = 0x10000l;
    if (deltay>0x10000l)
        deltay = 0x10000l;

    ob->x += deltax;
    ob->y += deltay;

    deltax = LABS(ob->x - player->x);
    deltay = LABS(ob->y - player->y);

    if (!ProjectileTryMove (ob))
    {
#ifndef APOGEE_1_0          // actually the whole method is never reached in shareware 1.0
        if (ob->obclass == rocketobj)
        {
            PlaySoundLocActor(MISSILEHITSND,ob);
            ob->state = &states[s_boom1];
        }
#ifdef SPEAR
        else if (ob->obclass == hrocketobj)
        {
            PlaySoundLocActor(MISSILEHITSND,ob);
            ob->state = &states[s_hboom1];
        }
#endif
        else
#endif
            ob->state = NULL;               // mark for removal

        return;
    }

    if (deltax < PROJECTILESIZE && deltay < PROJECTILESIZE)
    {       // hit the player
        switch (ob->obclass)
        {
        case needleobj:
            damage = (US_RndT() >>3) + 20;
            break;
        case rocketobj:
        case hrocketobj:
        case sparkobj:
            damage = (US_RndT() >>3) + 30;
            break;
        case fireobj:
            damage = (US_RndT() >>3);
            break;
        default:
            break;
        }

        TakeDamage (damage,ob);
        ob->state = NULL;               // mark for removal
        return;
    }

    ob->tilex = (short)(ob->x >> TILESHIFT);
    ob->tiley = (short)(ob->y >> TILESHIFT);
}


/*
===============
=
= SpawnStand
=
===============
*/

void SpawnStand (enemy_t which, int tilex, int tiley, int dir)
{
    word *map;
    word tile;

    switch (which)
    {
        case en_guard:
            SpawnNewObj (tilex,tiley,&states[s_grdstand]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        case en_officer:
            SpawnNewObj (tilex,tiley,&states[s_ofcstand]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        case en_mutant:
            SpawnNewObj (tilex,tiley,&states[s_mutstand]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        case en_ss:
            SpawnNewObj (tilex,tiley,&states[s_ssstand]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        default:
            break;
    }


    map = mapsegs[0]+(tiley<<mapshift)+tilex;
    tile = *map;
    if (tile == AMBUSHTILE)
    {
        tilemap[tilex][tiley] = 0;

        if (*(map+1) >= AREATILE)
            tile = *(map+1);
        if (*(map-mapwidth) >= AREATILE)
            tile = *(map-mapwidth);
        if (*(map+mapwidth) >= AREATILE)
            tile = *(map+mapwidth);
        if ( *(map-1) >= AREATILE)
            tile = *(map-1);

        *map = tile;
        newobj->areanumber = tile-AREATILE;

        newobj->flags |= FL_AMBUSH;
    }

    newobj->obclass = (classtype)(guardobj + which);
    newobj->hitpoints = starthitpoints[gamestate.difficulty][which];
    newobj->dir = (dirtype)(dir * 2);
    newobj->flags |= FL_SHOOTABLE;
}



/*
===============
=
= SpawnDeadGuard
=
===============
*/

void SpawnDeadGuard (int tilex, int tiley)
{
    SpawnNewObj (tilex,tiley,&states[s_grddie4]);
    DEMOIF_SDL
    {
        newobj->flags |= FL_NONMARK;    // walk through moving enemy fix
    }
    newobj->obclass = inertobj;
}



#ifndef SPEAR
/*
===============
=
= SpawnBoss
=
===============
*/

void SpawnBoss (int tilex, int tiley)
{
    SpawnNewObj (tilex,tiley,&states[s_bossstand]);
    newobj->speed = SPDPATROL;

    newobj->obclass = bossobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_boss];
    newobj->dir = nodir;
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}

/*
===============
=
= SpawnGretel
=
===============
*/

void SpawnGretel (int tilex, int tiley)
{
    SpawnNewObj (tilex,tiley,&states[s_gretelstand]);
    newobj->speed = SPDPATROL;

    newobj->obclass = gretelobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_gretel];
    newobj->dir = nodir;
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}
#endif

/*
===============
=
= SpawnPatrol
=
===============
*/

void SpawnPatrol (enemy_t which, int tilex, int tiley, int dir)
{
    switch (which)
    {
        case en_guard:
            SpawnNewObj (tilex,tiley,&states[s_grdpath1]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        case en_officer:
            SpawnNewObj (tilex,tiley,&states[s_ofcpath1]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        case en_ss:
            SpawnNewObj (tilex,tiley,&states[s_sspath1]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        case en_mutant:
            SpawnNewObj (tilex,tiley,&states[s_mutpath1]);
            newobj->speed = SPDPATROL;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        case en_dog:
            SpawnNewObj (tilex,tiley,&states[s_dogpath1]);
            newobj->speed = SPDDOG;
            if (!loadedgame)
                gamestate.killtotal++;
            break;

        default:
            break;
    }

    newobj->obclass = (classtype)(guardobj+which);
    newobj->dir = (dirtype)(dir*2);
    newobj->hitpoints = starthitpoints[gamestate.difficulty][which];
    newobj->distance = TILEGLOBAL;
    newobj->flags |= FL_SHOOTABLE;
    newobj->active = ac_yes;

    actorat[newobj->tilex][newobj->tiley] = NULL;           // don't use original spot

    switch (dir)
    {
        case 0:
            newobj->tilex++;
            break;
        case 1:
            newobj->tiley--;
            break;
        case 2:
            newobj->tilex--;
            break;
        case 3:
            newobj->tiley++;
            break;
    }

    actorat[newobj->tilex][newobj->tiley] = newobj;
}



/*
==================
=
= A_DeathScream
=
==================
*/

void A_DeathScream (objtype *ob)
{
#ifndef UPLOAD
#ifndef SPEAR
    if (mapon==9 && !US_RndT())
#else
    if ((mapon==18 || mapon==19) && !US_RndT())
#endif
    {
        switch(ob->obclass)
        {
            case mutantobj:
            case guardobj:
            case officerobj:
            case ssobj:
            case dogobj:
                PlaySoundLocActor(DEATHSCREAM6SND,ob);
                return;
            default:
                break;
        }
    }
#endif

    switch (ob->obclass)
    {
        case mutantobj:
            PlaySoundLocActor(AHHHGSND,ob);
            break;

        case guardobj:
        {
            int sounds[9]={ DEATHSCREAM1SND,
                DEATHSCREAM2SND,
                DEATHSCREAM3SND,
#ifndef APOGEE_1_0
                DEATHSCREAM4SND,
                DEATHSCREAM5SND,
                DEATHSCREAM7SND,
                DEATHSCREAM8SND,
                DEATHSCREAM9SND
#endif
            };

#ifndef UPLOAD
            PlaySoundLocActor(sounds[US_RndT()%8],ob);
#else
            PlaySoundLocActor(sounds[US_RndT()%2],ob);
#endif
            break;
        }
        case officerobj:
            PlaySoundLocActor(NEINSOVASSND,ob);
            break;
        case ssobj:
            PlaySoundLocActor(LEBENSND,ob); // JAB
            break;
        case dogobj:
            PlaySoundLocActor(DOGDEATHSND,ob);      // JAB
            break;
#ifndef SPEAR
        case bossobj:
            SD_PlaySound(MUTTISND);                         // JAB
            break;
        case schabbobj:
            SD_PlaySound(MEINGOTTSND);
            break;
        case fakeobj:
            SD_PlaySound(HITLERHASND);
            break;
        case mechahitlerobj:
            SD_PlaySound(SCHEISTSND);
            break;
        case realhitlerobj:
            SD_PlaySound(EVASND);
            break;
#ifndef APOGEE_1_0
        case gretelobj:
            SD_PlaySound(MEINSND);
            break;
        case giftobj:
            SD_PlaySound(DONNERSND);
            break;
        case fatobj:
            SD_PlaySound(ROSESND);
            break;
#endif
#else
        case spectreobj:
            SD_PlaySound(GHOSTFADESND);
            break;
        case angelobj:
            SD_PlaySound(ANGELDEATHSND);
            break;
        case transobj:
            SD_PlaySound(TRANSDEATHSND);
            break;
        case uberobj:
            SD_PlaySound(UBERDEATHSND);
            break;
        case willobj:
            SD_PlaySound(WILHELMDEATHSND);
            break;
        case deathobj:
            SD_PlaySound(KNIGHTDEATHSND);
            break;
#endif
        default:
            break;
    }
}


/*
=============================================================================

                                SPEAR ACTORS

=============================================================================
*/

#ifdef SPEAR


/*
===============
=
= SpawnTrans
=
===============
*/

void SpawnTrans (int tilex, int tiley)
{
    //        word *map;
    //        word tile;

    if (SoundBlasterPresent && DigiMode != sds_Off)
        states[s_transdie01].tictime = 105;

    SpawnNewObj (tilex,tiley,&states[s_transstand]);
    newobj->obclass = transobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_trans];
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
===============
=
= SpawnUber
=
===============
*/

void SpawnUber (int tilex, int tiley)
{
    if (SoundBlasterPresent && DigiMode != sds_Off)
        states[s_uberdie01].tictime = 70;

    SpawnNewObj (tilex,tiley,&states[s_uberstand]);
    newobj->obclass = uberobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_uber];
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
===============
=
= T_UShoot
=
===============
*/

void T_UShoot (objtype *ob)
{
    int     dx,dy,dist;

    T_Shoot (ob);

    dx = abs(ob->tilex - player->tilex);
    dy = abs(ob->tiley - player->tiley);
    dist = dx>dy ? dx : dy;
    if (dist <= 1)
        TakeDamage (10,ob);
}


/*
===============
=
= SpawnWill
=
===============
*/

void SpawnWill (int tilex, int tiley)
{
    if (SoundBlasterPresent && DigiMode != sds_Off)
        states[s_willdie2].tictime = 70;

    SpawnNewObj (tilex,tiley,&states[s_willstand]);
    newobj->obclass = willobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_will];
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
================
=
= T_Will
=
================
*/

void T_Will (objtype *ob)
{
    int32_t move;
    int     dx,dy,dist;
    boolean dodge;

    dodge = false;
    dx = abs(ob->tilex - player->tilex);
    dy = abs(ob->tiley - player->tiley);
    dist = dx>dy ? dx : dy;

    if (CheckLine(ob))                                              // got a shot at player?
    {
        ob->hidden = false;
        if ( (unsigned) US_RndT() < (tics<<3) && objfreelist)
        {
            //
            // go into attack frame
            //
            if (ob->obclass == willobj)
                NewState (ob,&states[s_willshoot1]);
            else if (ob->obclass == angelobj)
                NewState (ob,&states[s_angelshoot1]);
            else
                NewState (ob,&states[s_deathshoot1]);
            return;
        }
        dodge = true;
    }
    else
        ob->hidden = true;

    if (ob->dir == nodir)
    {
        if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        if (ob->distance < 0)
        {
            //
            // waiting for a door to open
            //
            OpenDoor (-ob->distance-1);
            if (doorobjlist[-ob->distance-1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;      // go ahead, the door is now open
            TryWalk(ob);
        }

        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        if (dist <4)
            SelectRunDir (ob);
        else if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

}


/*
===============
=
= SpawnDeath
=
===============
*/

void SpawnDeath (int tilex, int tiley)
{
    if (SoundBlasterPresent && DigiMode != sds_Off)
        states[s_deathdie2].tictime = 105;

    SpawnNewObj (tilex,tiley,&states[s_deathstand]);
    newobj->obclass = deathobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_death];
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}

/*
===============
=
= T_Launch
=
===============
*/

void T_Launch (objtype *ob)
{
    int32_t deltax,deltay;
    float   angle;
    int     iangle;

    deltax = player->x - ob->x;
    deltay = ob->y - player->y;
    angle = (float) atan2 ((float) deltay, (float) deltax);
    if (angle<0)
        angle = (float) (M_PI*2+angle);
    iangle = (int) (angle/(M_PI*2)*ANGLES);
    if (ob->obclass == deathobj)
    {
        T_Shoot (ob);
        if (ob->state == &states[s_deathshoot2])
        {
            iangle-=4;
            if (iangle<0)
                iangle+=ANGLES;
        }
        else
        {
            iangle+=4;
            if (iangle>=ANGLES)
                iangle-=ANGLES;
        }
    }

    GetNewActor ();
    newobj->state = &states[s_rocket];
    newobj->ticcount = 1;

    newobj->tilex = ob->tilex;
    newobj->tiley = ob->tiley;
    newobj->x = ob->x;
    newobj->y = ob->y;
    newobj->obclass = rocketobj;
    switch(ob->obclass)
    {
        case deathobj:
            newobj->state = &states[s_hrocket];
            newobj->obclass = hrocketobj;
            PlaySoundLocActor (KNIGHTMISSILESND,newobj);
            break;
        case angelobj:
            newobj->state = &states[s_spark1];
            newobj->obclass = sparkobj;
            PlaySoundLocActor (ANGELFIRESND,newobj);
            break;
        default:
            PlaySoundLocActor (MISSILEFIRESND,newobj);
    }

    newobj->dir = nodir;
    newobj->angle = iangle;
    newobj->speed = 0x2000l;
    newobj->flags = FL_NEVERMARK;
    newobj->active = ac_yes;
}



//
// angel
//

void A_Slurpie (objtype *)
{
    SD_PlaySound(SLURPIESND);
}

void A_Breathing (objtype *)
{
    SD_PlaySound(ANGELTIREDSND);
}

/*
===============
=
= SpawnAngel
=
===============
*/

void SpawnAngel (int tilex, int tiley)
{
    if (SoundBlasterPresent && DigiMode != sds_Off)
        states[s_angeldie11].tictime = 105;

    SpawnNewObj (tilex,tiley,&states[s_angelstand]);
    newobj->obclass = angelobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_angel];
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
=================
=
= A_Victory
=
=================
*/

void A_Victory (objtype *)
{
    playstate = ex_victorious;
}


/*
=================
=
= A_StartAttack
=
=================
*/

void A_StartAttack (objtype *ob)
{
    ob->temp1 = 0;
}


/*
=================
=
= A_Relaunch
=
=================
*/

void A_Relaunch (objtype *ob)
{
    if (++ob->temp1 == 3)
    {
        NewState (ob,&states[s_angeltired]);
        return;
    }

    if (US_RndT()&1)
    {
        NewState (ob,&states[s_angelchase1]);
        return;
    }
}




//
// spectre
//

/*
===============
=
= SpawnSpectre
=
===============
*/

void SpawnSpectre (int tilex, int tiley)
{
    SpawnNewObj (tilex,tiley,&states[s_spectrewait1]);
    newobj->obclass = spectreobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_spectre];
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH|FL_BONUS; // |FL_NEVERMARK|FL_NONMARK;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
===============
=
= A_Dormant
=
===============
*/

void A_Dormant (objtype *ob)
{
    int32_t     deltax,deltay;
    int         xl,xh,yl,yh;
    int         x,y;
    uintptr_t   tile;

    deltax = ob->x - player->x;
    if (deltax < -MINACTORDIST || deltax > MINACTORDIST)
        goto moveok;
    deltay = ob->y - player->y;
    if (deltay < -MINACTORDIST || deltay > MINACTORDIST)
        goto moveok;

    return;
moveok:

    xl = (ob->x-MINDIST) >> TILESHIFT;
    xh = (ob->x+MINDIST) >> TILESHIFT;
    yl = (ob->y-MINDIST) >> TILESHIFT;
    yh = (ob->y+MINDIST) >> TILESHIFT;

    for (y=yl ; y<=yh ; y++)
    {
        for (x=xl ; x<=xh ; x++)
        {
            tile = (uintptr_t)actorat[x][y];
            if (!tile)
                continue;
            if (!ISPOINTER(tile))
                return;
            if (((objtype *)tile)->flags&FL_SHOOTABLE)
                return;
        }
    }

        ob->flags |= FL_AMBUSH | FL_SHOOTABLE;
        ob->flags &= ~FL_ATTACKMODE;
        ob->flags &= ~FL_NONMARK;      // stuck bugfix 1
        ob->dir = nodir;
        NewState (ob,&states[s_spectrewait1]);
}


#endif

/*
=============================================================================

                            SCHABBS / GIFT / FAT

=============================================================================
*/

#ifndef SPEAR
/*
===============
=
= SpawnGhosts
=
===============
*/

void SpawnGhosts (int which, int tilex, int tiley)
{
    switch(which)
    {
        case en_blinky:
            SpawnNewObj (tilex,tiley,&states[s_blinkychase1]);
            break;
        case en_clyde:
            SpawnNewObj (tilex,tiley,&states[s_clydechase1]);
            break;
        case en_pinky:
            SpawnNewObj (tilex,tiley,&states[s_pinkychase1]);
            break;
        case en_inky:
            SpawnNewObj (tilex,tiley,&states[s_inkychase1]);
            break;
    }

    newobj->obclass = ghostobj;
    newobj->speed = SPDDOG;

    newobj->dir = east;
    newobj->flags |= FL_AMBUSH;
    if (!loadedgame)
    {
        gamestate.killtotal++;
        gamestate.killcount++;
    }
}


/*
===============
=
= SpawnSchabbs
=
===============
*/

void SpawnSchabbs (int tilex, int tiley)
{
    if (DigiMode != sds_Off)
        states[s_schabbdie2].tictime = 140;
    else
        states[s_schabbdie2].tictime = 5;

    SpawnNewObj (tilex,tiley,&states[s_schabbstand]);
    newobj->speed = SPDPATROL;

    newobj->obclass = schabbobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_schabbs];
    newobj->dir = nodir;
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
===============
=
= SpawnGift
=
===============
*/

void SpawnGift (int tilex, int tiley)
{
    if (DigiMode != sds_Off)
        states[s_giftdie2].tictime = 140;
    else
        states[s_giftdie2].tictime = 5;

    SpawnNewObj (tilex,tiley,&states[s_giftstand]);
    newobj->speed = SPDPATROL;

    newobj->obclass = giftobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_gift];
    newobj->dir = nodir;
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
===============
=
= SpawnFat
=
===============
*/

void SpawnFat (int tilex, int tiley)
{
    if (DigiMode != sds_Off)
        states[s_fatdie2].tictime = 140;
    else
        states[s_fatdie2].tictime = 5;

    SpawnNewObj (tilex,tiley,&states[s_fatstand]);
    newobj->speed = SPDPATROL;

    newobj->obclass = fatobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_fat];
    newobj->dir = nodir;
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
=================
=
= T_SchabbThrow
=
=================
*/

void T_SchabbThrow (objtype *ob)
{
    int32_t deltax,deltay;
    float   angle;
    int     iangle;

    deltax = player->x - ob->x;
    deltay = ob->y - player->y;
    angle = (float) atan2((float) deltay, (float) deltax);
    if (angle<0)
        angle = (float) (M_PI*2+angle);
    iangle = (int) (angle/(M_PI*2)*ANGLES);

    GetNewActor ();
    newobj->state = &states[s_needle1];
    newobj->ticcount = 1;

    newobj->tilex = ob->tilex;
    newobj->tiley = ob->tiley;
    newobj->x = ob->x;
    newobj->y = ob->y;
    newobj->obclass = needleobj;
    newobj->dir = nodir;
    newobj->angle = iangle;
    newobj->speed = 0x2000l;

    newobj->flags = FL_NEVERMARK;
    newobj->active = ac_yes;

    PlaySoundLocActor (SCHABBSTHROWSND,newobj);
}

/*
=================
=
= T_GiftThrow
=
=================
*/

void T_GiftThrow (objtype *ob)
{
    int32_t deltax,deltay;
    float   angle;
    int     iangle;

    deltax = player->x - ob->x;
    deltay = ob->y - player->y;
    angle = (float) atan2((float) deltay, (float) deltax);
    if (angle<0)
        angle = (float) (M_PI*2+angle);
    iangle = (int) (angle/(M_PI*2)*ANGLES);

    GetNewActor ();
    newobj->state = &states[s_rocket];
    newobj->ticcount = 1;

    newobj->tilex = ob->tilex;
    newobj->tiley = ob->tiley;
    newobj->x = ob->x;
    newobj->y = ob->y;
    newobj->obclass = rocketobj;
    newobj->dir = nodir;
    newobj->angle = iangle;
    newobj->speed = 0x2000l;
    newobj->flags = FL_NEVERMARK;
    newobj->active = ac_yes;

#ifndef APOGEE_1_0          // T_GiftThrow will never be called in shareware v1.0
    PlaySoundLocActor (MISSILEFIRESND,newobj);
#endif
}


/*
=================
=
= T_Schabb
=
=================
*/

void T_Schabb (objtype *ob)
{
    int32_t move;
    int     dx,dy,dist;
    boolean dodge;

    dodge = false;
    dx = abs(ob->tilex - player->tilex);
    dy = abs(ob->tiley - player->tiley);
    dist = dx>dy ? dx : dy;

    if (CheckLine(ob))                                              // got a shot at player?
    {
        ob->hidden = false;
        if ( (unsigned) US_RndT() < (tics<<3) && objfreelist)
        {
            //
            // go into attack frame
            //
            NewState (ob,&states[s_schabbshoot1]);
            return;
        }
        dodge = true;
    }
    else
        ob->hidden = true;

    if (ob->dir == nodir)
    {
        if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        if (ob->distance < 0)
        {
            //
            // waiting for a door to open
            //
            OpenDoor (-ob->distance-1);
            if (doorobjlist[-ob->distance-1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;      // go ahead, the door is now open
            TryWalk(ob);
        }

        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        if (dist <4)
            SelectRunDir (ob);
        else if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }
}


/*
=================
=
= T_Gift
=
=================
*/

void T_Gift (objtype *ob)
{
    int32_t move;
    int     dx,dy,dist;
    boolean dodge;

    dodge = false;
    dx = abs(ob->tilex - player->tilex);
    dy = abs(ob->tiley - player->tiley);
    dist = dx>dy ? dx : dy;

    if (CheckLine(ob))                                              // got a shot at player?
    {
        ob->hidden = false;
        if ( (unsigned) US_RndT() < (tics<<3) && objfreelist)
        {
            //
            // go into attack frame
            //
            NewState (ob,&states[s_giftshoot1]);
            return;
        }
        dodge = true;
    }
    else
        ob->hidden = true;

    if (ob->dir == nodir)
    {
        if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        if (ob->distance < 0)
        {
            //
            // waiting for a door to open
            //
            OpenDoor (-ob->distance-1);
            if (doorobjlist[-ob->distance-1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;      // go ahead, the door is now open
            TryWalk(ob);
        }

        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        if (dist <4)
            SelectRunDir (ob);
        else if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }
}


/*
=================
=
= T_Fat
=
=================
*/

void T_Fat (objtype *ob)
{
    int32_t move;
    int     dx,dy,dist;
    boolean dodge;

    dodge = false;
    dx = abs(ob->tilex - player->tilex);
    dy = abs(ob->tiley - player->tiley);
    dist = dx>dy ? dx : dy;

    if (CheckLine(ob))                                              // got a shot at player?
    {
        ob->hidden = false;
        if ( (unsigned) US_RndT() < (tics<<3) && objfreelist)
        {
            //
            // go into attack frame
            //
            NewState (ob,&states[s_fatshoot1]);
            return;
        }
        dodge = true;
    }
    else
        ob->hidden = true;

    if (ob->dir == nodir)
    {
        if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        if (ob->distance < 0)
        {
            //
            // waiting for a door to open
            //
            OpenDoor (-ob->distance-1);
            if (doorobjlist[-ob->distance-1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;      // go ahead, the door is now open
            TryWalk(ob);
        }

        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        if (dist <4)
            SelectRunDir (ob);
        else if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }
}


/*
=============================================================================

                                    HITLERS

=============================================================================
*/


/*
===============
=
= SpawnFakeHitler
=
===============
*/

void SpawnFakeHitler (int tilex, int tiley)
{
    if (DigiMode != sds_Off)
        states[s_hitlerdie2].tictime = 140;
    else
        states[s_hitlerdie2].tictime = 5;

    SpawnNewObj (tilex,tiley,&states[s_fakestand]);
    newobj->speed = SPDPATROL;

    newobj->obclass = fakeobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_fake];
    newobj->dir = nodir;
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
===============
=
= SpawnHitler
=
===============
*/

void SpawnHitler (int tilex, int tiley)
{
    if (DigiMode != sds_Off)
        states[s_hitlerdie2].tictime = 140;
    else
        states[s_hitlerdie2].tictime = 5;


    SpawnNewObj (tilex,tiley,&states[s_mechastand]);
    newobj->speed = SPDPATROL;

    newobj->obclass = mechahitlerobj;
    newobj->hitpoints = starthitpoints[gamestate.difficulty][en_hitler];
    newobj->dir = nodir;
    newobj->flags |= FL_SHOOTABLE|FL_AMBUSH;
    if (!loadedgame)
        gamestate.killtotal++;
}


/*
===============
=
= A_HitlerMorph
=
===============
*/

void A_HitlerMorph (objtype *ob)
{
    short hitpoints[4]={500,700,800,900};

    SpawnNewObj (ob->tilex,ob->tiley,&states[s_hitlerchase1]);
    newobj->speed = SPDPATROL*5;

    newobj->x = ob->x;
    newobj->y = ob->y;

    newobj->distance = ob->distance;
    newobj->dir = ob->dir;
    newobj->flags = ob->flags | FL_SHOOTABLE;
    newobj->flags &= ~FL_NONMARK;   // hitler stuck with nodir fix

    newobj->obclass = realhitlerobj;
    newobj->hitpoints = hitpoints[gamestate.difficulty];
}


////////////////////////////////////////////////////////
//
// A_MechaSound
// A_Slurpie
//
////////////////////////////////////////////////////////
void A_MechaSound (objtype *ob)
{
    if (areabyplayer[ob->areanumber])
        PlaySoundLocActor (MECHSTEPSND,ob);
}

void A_Slurpie (objtype *)
{
    SD_PlaySound(SLURPIESND);
}

/*
=================
=
= T_FakeFire
=
=================
*/

void T_FakeFire (objtype *ob)
{
    int32_t deltax,deltay;
    float   angle;
    int     iangle;

    if (!objfreelist)       // stop shooting if over MAXACTORS
    {
        NewState (ob,&states[s_fakechase1]);
        return;
    }

    deltax = player->x - ob->x;
    deltay = ob->y - player->y;
    angle = (float) atan2((float) deltay, (float) deltax);
    if (angle<0)
        angle = (float)(M_PI*2+angle);
    iangle = (int) (angle/(M_PI*2)*ANGLES);

    GetNewActor ();
    newobj->state = &states[s_fire1];
    newobj->ticcount = 1;

    newobj->tilex = ob->tilex;
    newobj->tiley = ob->tiley;
    newobj->x = ob->x;
    newobj->y = ob->y;
    newobj->dir = nodir;
    newobj->angle = iangle;
    newobj->obclass = fireobj;
    newobj->speed = 0x1200l;
    newobj->flags = FL_NEVERMARK;
    newobj->active = ac_yes;

    PlaySoundLocActor (FLAMETHROWERSND,newobj);
}



/*
=================
=
= T_Fake
=
=================
*/

void T_Fake (objtype *ob)
{
    int32_t move;

    if (CheckLine(ob))                      // got a shot at player?
    {
        ob->hidden = false;
        if ( (unsigned) US_RndT() < (tics<<1) && objfreelist)
        {
            //
            // go into attack frame
            //
            NewState (ob,&states[s_fakeshoot1]);
            return;
        }
    }
    else
        ob->hidden = true;

    if (ob->dir == nodir)
    {
        SelectDodgeDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        SelectDodgeDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }
}

#endif
/*
============================================================================

STAND

============================================================================
*/


/*
===============
=
= T_Stand
=
===============
*/

void T_Stand (objtype *ob)
{
    SightPlayer (ob);
}


/*
============================================================================

CHASE

============================================================================
*/

/*
=================
=
= T_Chase
=
=================
*/

void T_Chase (objtype *ob)
{
    int32_t move,target;
    int     dx,dy,dist,chance;
    boolean dodge;

    if (gamestate.victoryflag)
        return;

    dodge = false;
    if (CheckLine(ob))      // got a shot at player?
    {
        ob->hidden = false;
        dx = abs(ob->tilex - player->tilex);
        dy = abs(ob->tiley - player->tiley);
        dist = dx>dy ? dx : dy;

#ifdef PLAYDEMOLIKEORIGINAL
        if(DEMOCOND_ORIG)
        {
            if(!dist || (dist == 1 && ob->distance < 0x4000))
                chance = 300;
            else
                chance = (tics<<4)/dist;
        }
        else
#endif
        {
            if (dist)
                chance = (tics<<4)/dist;
            else
                chance = 300;

            if (dist == 1)
            {
                target = abs(ob->x - player->x);
                if (target < 0x14000l)
                {
                    target = abs(ob->y - player->y);
                    if (target < 0x14000l)
                        chance = 300;
                }
            }
        }

        if ( US_RndT()<chance)
        {
            //
            // go into attack frame
            //
            switch (ob->obclass)
            {
                case guardobj:
                    NewState (ob,&states[s_grdshoot1]);
                    break;
                case officerobj:
                    NewState (ob,&states[s_ofcshoot1]);
                    break;
                case mutantobj:
                    NewState (ob,&states[s_mutshoot1]);
                    break;
                case ssobj:
                    NewState (ob,&states[s_ssshoot1]);
                    break;
#ifndef SPEAR
                case bossobj:
                    NewState (ob,&states[s_bossshoot1]);
                    break;
                case gretelobj:
                    NewState (ob,&states[s_gretelshoot1]);
                    break;
                case mechahitlerobj:
                    NewState (ob,&states[s_mechashoot1]);
                    break;
                case realhitlerobj:
                    NewState (ob,&states[s_hitlershoot1]);
                    break;
#else
                case angelobj:
                    NewState (ob,&states[s_angelshoot1]);
                    break;
                case transobj:
                    NewState (ob,&states[s_transshoot1]);
                    break;
                case uberobj:
                    NewState (ob,&states[s_ubershoot1]);
                    break;
                case willobj:
                    NewState (ob,&states[s_willshoot1]);
                    break;
                case deathobj:
                    NewState (ob,&states[s_deathshoot1]);
                    break;
#endif
                default:
                    break;
            }
            return;
        }
        dodge = true;
    }
    else
        ob->hidden = true;

    if (ob->dir == nodir)
    {
        if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        if (ob->distance < 0)
        {
            //
            // waiting for a door to open
            //
            OpenDoor (-ob->distance-1);
            if (doorobjlist[-ob->distance-1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;      // go ahead, the door is now open
            DEMOIF_SDL
            {
                TryWalk(ob);
            }
        }

        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        if (dodge)
            SelectDodgeDir (ob);
        else
            SelectChaseDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }
}


/*
=================
=
= T_Ghosts
=
=================
*/

void T_Ghosts (objtype *ob)
{
    int32_t move;

    if (ob->dir == nodir)
    {
        SelectChaseDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        SelectChaseDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }
}

/*
=================
=
= T_DogChase
=
=================
*/

void T_DogChase (objtype *ob)
{
    int32_t    move;
    int32_t    dx,dy;


    if (ob->dir == nodir)
    {
        SelectDodgeDir (ob);
        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }

    move = ob->speed*tics;

    while (move)
    {
        //
        // check for byte range
        //
        dx = player->x - ob->x;
        if (dx<0)
            dx = -dx;
        dx -= move;
        if (dx <= MINACTORDIST)
        {
            dy = player->y - ob->y;
            if (dy<0)
                dy = -dy;
            dy -= move;
            if (dy <= MINACTORDIST)
            {
                NewState (ob,&states[s_dogjump1]);
                return;
            }
        }

        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        //
        // reached goal tile, so select another one
        //

        //
        // fix position to account for round off during moving
        //
        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;

        move -= ob->distance;

        SelectDodgeDir (ob);

        if (ob->dir == nodir)
            return;                                                 // object is blocked in
    }
}



/*
============================================================================

                                    PATH

============================================================================
*/


/*
===============
=
= SelectPathDir
=
===============
*/

void SelectPathDir (objtype *ob)
{
    unsigned spot;

    spot = MAPSPOT(ob->tilex,ob->tiley,1)-ICONARROWS;

    if (spot<8)
    {
        // new direction
        ob->dir = (dirtype)(spot);
    }

    ob->distance = TILEGLOBAL;

    if (!TryWalk (ob))
        ob->dir = nodir;
}


/*
===============
=
= T_Path
=
===============
*/

void T_Path (objtype *ob)
{
    int32_t    move;

    if (SightPlayer (ob))
        return;

    if (ob->dir == nodir)
    {
        SelectPathDir (ob);
        if (ob->dir == nodir)
            return;                                 // all movement is blocked
    }


    move = ob->speed*tics;

    while (move)
    {
        if (ob->distance < 0)
        {
            //
            // waiting for a door to open
            //
            OpenDoor (-ob->distance-1);
            if (doorobjlist[-ob->distance-1].action != dr_open)
                return;
            ob->distance = TILEGLOBAL;      // go ahead, the door is now open
            DEMOIF_SDL
            {
                TryWalk(ob);
            }
        }

        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }

        if (ob->tilex>MAPSIZE || ob->tiley>MAPSIZE)
        {
            snprintf (str, sizeof(str), "T_Path hit a wall at %u,%u, dir %u",
                ob->tilex,ob->tiley,ob->dir);
            Quit (str);
        }

        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;
        move -= ob->distance;

        SelectPathDir (ob);

        if (ob->dir == nodir)
            return;                                 // all movement is blocked
    }
}


/*
=============================================================================

                                    FIGHT

=============================================================================
*/


/*
===============
=
= T_Shoot
=
= Try to damage the player, based on skill level and player's speed
=
===============
*/

void T_Shoot (objtype *ob)
{
    int     dx,dy,dist;
    int     hitchance,damage;

    hitchance = 128;

    if (!areabyplayer[ob->areanumber])
        return;

    if (CheckLine (ob))                    // player is not behind a wall
    {
        dx = abs(ob->tilex - player->tilex);
        dy = abs(ob->tiley - player->tiley);
        dist = dx>dy ? dx:dy;

        if (ob->obclass == ssobj || ob->obclass == bossobj)
            dist = dist*2/3;                                        // ss are better shots

        if (thrustspeed >= RUNSPEED)
        {
            if (ob->flags&FL_VISABLE)
                hitchance = 160-dist*16;                // player can see to dodge
            else
                hitchance = 160-dist*8;
        }
        else
        {
            if (ob->flags&FL_VISABLE)
                hitchance = 256-dist*16;                // player can see to dodge
            else
                hitchance = 256-dist*8;
        }

        // see if the shot was a hit

        if (US_RndT()<hitchance)
        {
            if (dist<2)
                damage = US_RndT()>>2;
            else if (dist<4)
                damage = US_RndT()>>3;
            else
                damage = US_RndT()>>4;

            TakeDamage (damage,ob);
        }
    }

    switch(ob->obclass)
    {
        case ssobj:
            PlaySoundLocActor(SSFIRESND,ob);
            break;
#ifndef SPEAR
#ifndef APOGEE_1_0
        case giftobj:
        case fatobj:
            PlaySoundLocActor(MISSILEFIRESND,ob);
            break;
#endif
        case mechahitlerobj:
        case realhitlerobj:
        case bossobj:
            PlaySoundLocActor(BOSSFIRESND,ob);
            break;
        case schabbobj:
            PlaySoundLocActor(SCHABBSTHROWSND,ob);
            break;
        case fakeobj:
            PlaySoundLocActor(FLAMETHROWERSND,ob);
            break;
#endif
        default:
            PlaySoundLocActor(NAZIFIRESND,ob);
    }
}


/*
===============
=
= T_Bite
=
===============
*/

void T_Bite (objtype *ob)
{
    int32_t    dx,dy;

    PlaySoundLocActor(DOGATTACKSND,ob);     // JAB

    dx = player->x - ob->x;
    if (dx<0)
        dx = -dx;
    dx -= TILEGLOBAL;
    if (dx <= MINACTORDIST)
    {
        dy = player->y - ob->y;
        if (dy<0)
            dy = -dy;
        dy -= TILEGLOBAL;
        if (dy <= MINACTORDIST)
        {
            if (US_RndT()<180)
            {
                TakeDamage (US_RndT()>>4,ob);
                return;
            }
        }
    }
}


#ifndef SPEAR
/*
============================================================================

                                    BJ VICTORY

============================================================================
*/


//
// BJ victory
//


/*
===============
=
= SpawnBJVictory
=
===============
*/

void SpawnBJVictory (void)
{
    SpawnNewObj (player->tilex,player->tiley+1,&states[s_bjrun1]);
    newobj->x = player->x;
    newobj->y = player->y;
    newobj->obclass = bjobj;
    newobj->dir = north;
    newobj->temp1 = 6;                      // tiles to run forward
}



/*
===============
=
= T_BJRun
=
===============
*/

void T_BJRun (objtype *ob)
{
    int32_t    move;

    move = BJRUNSPEED*tics;

    while (move)
    {
        if (move < ob->distance)
        {
            MoveObj (ob,move);
            break;
        }


        ob->x = ((int32_t)ob->tilex<<TILESHIFT)+TILEGLOBAL/2;
        ob->y = ((int32_t)ob->tiley<<TILESHIFT)+TILEGLOBAL/2;
        move -= ob->distance;

        SelectPathDir (ob);

        if ( !(--ob->temp1) )
        {
            NewState (ob,&states[s_bjjump1]);
            return;
        }
    }
}


/*
===============
=
= T_BJJump
=
===============
*/

void T_BJJump (objtype *ob)
{
    int32_t    move;

    move = BJJUMPSPEED*tics;
    MoveObj (ob,move);
}


/*
===============
=
= T_BJYell
=
===============
*/

void T_BJYell (objtype *ob)
{
    PlaySoundLocActor(YEAHSND,ob);  // JAB
}


/*
===============
=
= T_BJDone
=
===============
*/

void T_BJDone (objtype *)
{
    playstate = ex_victorious;                              // exit castle tile
}



//===========================================================================


/*
===============
=
= CheckPosition
=
===============
*/

boolean CheckPosition (objtype *ob)
{
    int     x,y,xl,yl,xh,yh;
    objtype *check;

    xl = (ob->x-PLAYERSIZE) >> TILESHIFT;
    yl = (ob->y-PLAYERSIZE) >> TILESHIFT;

    xh = (ob->x+PLAYERSIZE) >> TILESHIFT;
    yh = (ob->y+PLAYERSIZE) >> TILESHIFT;

    //
    // check for solid walls
    //
    for (y=yl;y<=yh;y++)
    {
        for (x=xl;x<=xh;x++)
        {
            check = actorat[x][y];
            if (check && !ISPOINTER(check))
                return false;
        }
    }

    return true;
}


/*
===============
=
= A_StartDeathCam
=
===============
*/

void    A_StartDeathCam (objtype *ob)
{
    int32_t dx,dy;
    float   fangle;
    int32_t xmove,ymove;
    int32_t dist;

    FinishPaletteShifts ();

    VW_WaitVBL (100);

    if (gamestate.victoryflag)
    {
        playstate = ex_victorious;                              // exit castle tile
        return;
    }

    VH_UpdateScreen();

    gamestate.victoryflag = true;
    unsigned fadeheight = viewsize != 21 ? screenHeight-scaleFactor*STATUSLINES : screenHeight;
    VL_BarScaledCoord (0, 0, screenWidth, fadeheight, bordercol);
    FizzleFade(screenBuffer, 0, 0, screenWidth, fadeheight, 70, false);

    if (bordercol != VIEWCOLOR)
    {
        CA_CacheGrChunk (STARTFONT+1);
        fontnumber = 1;
        SETFONTCOLOR(15,bordercol);
        PrintX = 68; PrintY = 45;
        US_Print (STR_SEEAGAIN);
        UNCACHEGRCHUNK(STARTFONT+1);
    }
    else
    {
        CacheLump(LEVELEND_LUMP_START,LEVELEND_LUMP_END);
        Write(0,7,STR_SEEAGAIN);
    }

    VW_UpdateScreen ();

    IN_UserInput(300);

    //
    // line angle up exactly
    //
    NewState (player,&states[s_deathcam]);

    player->x = gamestate.killx;
    player->y = gamestate.killy;

    dx = ob->x - player->x;
    dy = player->y - ob->y;

    fangle = (float) atan2((float) dy, (float) dx);          // returns -pi to pi
    if (fangle<0)
        fangle = (float) (M_PI*2+fangle);

    player->angle = (short) (fangle/(M_PI*2)*ANGLES);

    //
    // try to position as close as possible without being in a wall
    //
    dist = 0x14000l;
    do
    {
        xmove = FixedMul(dist,costable[player->angle]);
        ymove = -FixedMul(dist,sintable[player->angle]);

        player->x = ob->x - xmove;
        player->y = ob->y - ymove;
        dist += 0x1000;

    } while (!CheckPosition (player));
    plux = (word)(player->x >> UNSIGNEDSHIFT);                      // scale to fit in unsigned
    pluy = (word)(player->y >> UNSIGNEDSHIFT);
    player->tilex = (word)(player->x >> TILESHIFT);         // scale to tile values
    player->tiley = (word)(player->y >> TILESHIFT);

    //
    // go back to the game
    //

    DrawPlayBorder ();

    fizzlein = true;

    switch (ob->obclass)
    {
#ifndef SPEAR
        case schabbobj:
            NewState (ob,&states[s_schabbdeathcam]);
            break;
        case realhitlerobj:
            NewState (ob,&states[s_hitlerdeathcam]);
            break;
        case giftobj:
            NewState (ob,&states[s_giftdeathcam]);
            break;
        case fatobj:
            NewState (ob,&states[s_fatdeathcam]);
            break;
        default:
            break;
#endif
    }
}

#endif
