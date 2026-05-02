//
// Copyright(C) 1992 Id Software, Inc.
// Copyright(C) 2007-2011 Moritz "Ripper" Kroll
// Copyright(C) 2020 Fabian Greffrath
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Organize all available statetype variables in a states[] array.
//
//	This replaces the current relying on a specific order in which
//	global variables are stored in the executable for e.g. saving
//	and restoring of savegames and thus does not require the
//	-fno-toplevel-reorder compiler flag anymore.
//

#include "wl_def.h"

extern void A_Breathing (objtype *);
extern void A_Dormant (objtype *);
extern void A_HitlerMorph (objtype *);
extern void A_MechaSound (objtype *);
extern void A_Relaunch (objtype *);
extern void A_Slurpie (objtype *);
extern void A_Smoke (objtype *);
extern void A_StartAttack (objtype *);
extern void A_StartDeathCam (objtype *);
extern void A_Victory (objtype *);
extern void T_Attack (objtype *);
extern void T_BJDone (objtype *);
extern void T_BJJump (objtype *);
extern void T_BJRun (objtype *);
extern void T_BJYell (objtype *);
extern void T_Bite (objtype *);
extern void T_Chase (objtype *);
extern void T_DogChase (objtype *);
extern void T_Fake (objtype *);
extern void T_FakeFire (objtype *);
extern void T_Fat (objtype *);
extern void T_Ghosts (objtype *);
extern void T_Gift (objtype *);
extern void T_GiftThrow (objtype *);
extern void T_Launch (objtype *);
extern void T_Path (objtype *);
extern void T_Player (objtype *);
extern void T_Projectile (objtype *);
extern void T_Schabb (objtype *);
extern void T_SchabbThrow (objtype *);
extern void T_Shoot (objtype *);
extern void T_Stand (objtype *);
extern void T_UShoot (objtype *);
extern void T_Will (objtype *);

statetype states[numstates] = {
	{false,0,0,NULL,NULL,s_null},	// s_null
	{false,0,0,(statefunc)T_Player,NULL,s_null},	// s_player
	{false,0,0,(statefunc)T_Attack,NULL,s_null},	// s_attack
	{true,SPR_ROCKET_1,3,(statefunc)T_Projectile,(statefunc)A_Smoke,s_rocket},	// s_rocket
	{false,SPR_SMOKE_1,3,NULL,NULL,s_smoke2},	// s_smoke1
	{false,SPR_SMOKE_2,3,NULL,NULL,s_smoke3},	// s_smoke2
	{false,SPR_SMOKE_3,3,NULL,NULL,s_smoke4},	// s_smoke3
	{false,SPR_SMOKE_4,3,NULL,NULL,s_null},	// s_smoke4
	{false,SPR_BOOM_1,6,NULL,NULL,s_boom2},	// s_boom1
	{false,SPR_BOOM_2,6,NULL,NULL,s_boom3},	// s_boom2
	{false,SPR_BOOM_3,6,NULL,NULL,s_null},	// s_boom3
#ifdef SPEAR
	{true,SPR_HROCKET_1,3,(statefunc)T_Projectile,(statefunc)A_Smoke,s_hrocket},	// s_hrocket
	{false,SPR_HSMOKE_1,3,NULL,NULL,s_hsmoke2},	// s_hsmoke1
	{false,SPR_HSMOKE_2,3,NULL,NULL,s_hsmoke3},	// s_hsmoke2
	{false,SPR_HSMOKE_3,3,NULL,NULL,s_hsmoke4},	// s_hsmoke3
	{false,SPR_HSMOKE_4,3,NULL,NULL,s_null},	// s_hsmoke4
	{false,SPR_HBOOM_1,6,NULL,NULL,s_hboom2},	// s_hboom1
	{false,SPR_HBOOM_2,6,NULL,NULL,s_hboom3},	// s_hboom2
	{false,SPR_HBOOM_3,6,NULL,NULL,s_null},	// s_hboom3
#endif
	{true,SPR_GRD_S_1,0,(statefunc)T_Stand,NULL,s_grdstand},	// s_grdstand
	{true,SPR_GRD_W1_1,20,(statefunc)T_Path,NULL,s_grdpath1s},	// s_grdpath1
	{true,SPR_GRD_W1_1,5,NULL,NULL,s_grdpath2},	// s_grdpath1s
	{true,SPR_GRD_W2_1,15,(statefunc)T_Path,NULL,s_grdpath3},	// s_grdpath2
	{true,SPR_GRD_W3_1,20,(statefunc)T_Path,NULL,s_grdpath3s},	// s_grdpath3
	{true,SPR_GRD_W3_1,5,NULL,NULL,s_grdpath4},	// s_grdpath3s
	{true,SPR_GRD_W4_1,15,(statefunc)T_Path,NULL,s_grdpath1},	// s_grdpath4
	{2,SPR_GRD_PAIN_1,10,NULL,NULL,s_grdchase1},	// s_grdpain
	{2,SPR_GRD_PAIN_2,10,NULL,NULL,s_grdchase1},	// s_grdpain1
	{false,SPR_GRD_SHOOT1,20,NULL,NULL,s_grdshoot2},	// s_grdshoot1
	{false,SPR_GRD_SHOOT2,20,NULL,(statefunc)T_Shoot,s_grdshoot3},	// s_grdshoot2
	{false,SPR_GRD_SHOOT3,20,NULL,NULL,s_grdchase1},	// s_grdshoot3
	{true,SPR_GRD_W1_1,10,(statefunc)T_Chase,NULL,s_grdchase1s},	// s_grdchase1
	{true,SPR_GRD_W1_1,3,NULL,NULL,s_grdchase2},	// s_grdchase1s
	{true,SPR_GRD_W2_1,8,(statefunc)T_Chase,NULL,s_grdchase3},	// s_grdchase2
	{true,SPR_GRD_W3_1,10,(statefunc)T_Chase,NULL,s_grdchase3s},	// s_grdchase3
	{true,SPR_GRD_W3_1,3,NULL,NULL,s_grdchase4},	// s_grdchase3s
	{true,SPR_GRD_W4_1,8,(statefunc)T_Chase,NULL,s_grdchase1},	// s_grdchase4
	{false,SPR_GRD_DIE_1,15,NULL,(statefunc)A_DeathScream,s_grddie2},	// s_grddie1
	{false,SPR_GRD_DIE_2,15,NULL,NULL,s_grddie3},	// s_grddie2
	{false,SPR_GRD_DIE_3,15,NULL,NULL,s_grddie4},	// s_grddie3
	{false,SPR_GRD_DEAD,0,NULL,NULL,s_grddie4},	// s_grddie4
#ifndef SPEAR
	{false,SPR_BLINKY_W1,10,(statefunc)T_Ghosts,NULL,s_blinkychase2},	// s_blinkychase1
	{false,SPR_BLINKY_W2,10,(statefunc)T_Ghosts,NULL,s_blinkychase1},	// s_blinkychase2
	{false,SPR_INKY_W1,10,(statefunc)T_Ghosts,NULL,s_inkychase2},	// s_inkychase1
	{false,SPR_INKY_W2,10,(statefunc)T_Ghosts,NULL,s_inkychase1},	// s_inkychase2
	{false,SPR_PINKY_W1,10,(statefunc)T_Ghosts,NULL,s_pinkychase2},	// s_pinkychase1
	{false,SPR_PINKY_W2,10,(statefunc)T_Ghosts,NULL,s_pinkychase1},	// s_pinkychase2
	{false,SPR_CLYDE_W1,10,(statefunc)T_Ghosts,NULL,s_clydechase2},	// s_clydechase1
	{false,SPR_CLYDE_W2,10,(statefunc)T_Ghosts,NULL,s_clydechase1},	// s_clydechase2
#endif
	{true,SPR_DOG_W1_1,20,(statefunc)T_Path,NULL,s_dogpath1s},	// s_dogpath1
	{true,SPR_DOG_W1_1,5,NULL,NULL,s_dogpath2},	// s_dogpath1s
	{true,SPR_DOG_W2_1,15,(statefunc)T_Path,NULL,s_dogpath3},	// s_dogpath2
	{true,SPR_DOG_W3_1,20,(statefunc)T_Path,NULL,s_dogpath3s},	// s_dogpath3
	{true,SPR_DOG_W3_1,5,NULL,NULL,s_dogpath4},	// s_dogpath3s
	{true,SPR_DOG_W4_1,15,(statefunc)T_Path,NULL,s_dogpath1},	// s_dogpath4
	{false,SPR_DOG_JUMP1,10,NULL,NULL,s_dogjump2},	// s_dogjump1
	{false,SPR_DOG_JUMP2,10,NULL,(statefunc)T_Bite,s_dogjump3},	// s_dogjump2
	{false,SPR_DOG_JUMP3,10,NULL,NULL,s_dogjump4},	// s_dogjump3
	{false,SPR_DOG_JUMP1,10,NULL,NULL,s_dogjump5},	// s_dogjump4
	{false,SPR_DOG_W1_1,10,NULL,NULL,s_dogchase1},	// s_dogjump5
	{true,SPR_DOG_W1_1,10,(statefunc)T_DogChase,NULL,s_dogchase1s},	// s_dogchase1
	{true,SPR_DOG_W1_1,3,NULL,NULL,s_dogchase2},	// s_dogchase1s
	{true,SPR_DOG_W2_1,8,(statefunc)T_DogChase,NULL,s_dogchase3},	// s_dogchase2
	{true,SPR_DOG_W3_1,10,(statefunc)T_DogChase,NULL,s_dogchase3s},	// s_dogchase3
	{true,SPR_DOG_W3_1,3,NULL,NULL,s_dogchase4},	// s_dogchase3s
	{true,SPR_DOG_W4_1,8,(statefunc)T_DogChase,NULL,s_dogchase1},	// s_dogchase4
	{false,SPR_DOG_DIE_1,15,NULL,(statefunc)A_DeathScream,s_dogdie2},	// s_dogdie1
	{false,SPR_DOG_DIE_2,15,NULL,NULL,s_dogdie3},	// s_dogdie2
	{false,SPR_DOG_DIE_3,15,NULL,NULL,s_dogdead},	// s_dogdie3
	{false,SPR_DOG_DEAD,15,NULL,NULL,s_dogdead},	// s_dogdead
	{true,SPR_OFC_S_1,0,(statefunc)T_Stand,NULL,s_ofcstand},	// s_ofcstand
	{true,SPR_OFC_W1_1,20,(statefunc)T_Path,NULL,s_ofcpath1s},	// s_ofcpath1
	{true,SPR_OFC_W1_1,5,NULL,NULL,s_ofcpath2},	// s_ofcpath1s
	{true,SPR_OFC_W2_1,15,(statefunc)T_Path,NULL,s_ofcpath3},	// s_ofcpath2
	{true,SPR_OFC_W3_1,20,(statefunc)T_Path,NULL,s_ofcpath3s},	// s_ofcpath3
	{true,SPR_OFC_W3_1,5,NULL,NULL,s_ofcpath4},	// s_ofcpath3s
	{true,SPR_OFC_W4_1,15,(statefunc)T_Path,NULL,s_ofcpath1},	// s_ofcpath4
	{2,SPR_OFC_PAIN_1,10,NULL,NULL,s_ofcchase1},	// s_ofcpain
	{2,SPR_OFC_PAIN_2,10,NULL,NULL,s_ofcchase1},	// s_ofcpain1
	{false,SPR_OFC_SHOOT1,6,NULL,NULL,s_ofcshoot2},	// s_ofcshoot1
	{false,SPR_OFC_SHOOT2,20,NULL,(statefunc)T_Shoot,s_ofcshoot3},	// s_ofcshoot2
	{false,SPR_OFC_SHOOT3,10,NULL,NULL,s_ofcchase1},	// s_ofcshoot3
	{true,SPR_OFC_W1_1,10,(statefunc)T_Chase,NULL,s_ofcchase1s},	// s_ofcchase1
	{true,SPR_OFC_W1_1,3,NULL,NULL,s_ofcchase2},	// s_ofcchase1s
	{true,SPR_OFC_W2_1,8,(statefunc)T_Chase,NULL,s_ofcchase3},	// s_ofcchase2
	{true,SPR_OFC_W3_1,10,(statefunc)T_Chase,NULL,s_ofcchase3s},	// s_ofcchase3
	{true,SPR_OFC_W3_1,3,NULL,NULL,s_ofcchase4},	// s_ofcchase3s
	{true,SPR_OFC_W4_1,8,(statefunc)T_Chase,NULL,s_ofcchase1},	// s_ofcchase4
	{false,SPR_OFC_DIE_1,11,NULL,(statefunc)A_DeathScream,s_ofcdie2},	// s_ofcdie1
	{false,SPR_OFC_DIE_2,11,NULL,NULL,s_ofcdie3},	// s_ofcdie2
	{false,SPR_OFC_DIE_3,11,NULL,NULL,s_ofcdie4},	// s_ofcdie3
	{false,SPR_OFC_DIE_4,11,NULL,NULL,s_ofcdie5},	// s_ofcdie4
	{false,SPR_OFC_DEAD,0,NULL,NULL,s_ofcdie5},	// s_ofcdie5
	{true,SPR_MUT_S_1,0,(statefunc)T_Stand,NULL,s_mutstand},	// s_mutstand
	{true,SPR_MUT_W1_1,20,(statefunc)T_Path,NULL,s_mutpath1s},	// s_mutpath1
	{true,SPR_MUT_W1_1,5,NULL,NULL,s_mutpath2},	// s_mutpath1s
	{true,SPR_MUT_W2_1,15,(statefunc)T_Path,NULL,s_mutpath3},	// s_mutpath2
	{true,SPR_MUT_W3_1,20,(statefunc)T_Path,NULL,s_mutpath3s},	// s_mutpath3
	{true,SPR_MUT_W3_1,5,NULL,NULL,s_mutpath4},	// s_mutpath3s
	{true,SPR_MUT_W4_1,15,(statefunc)T_Path,NULL,s_mutpath1},	// s_mutpath4
	{2,SPR_MUT_PAIN_1,10,NULL,NULL,s_mutchase1},	// s_mutpain
	{2,SPR_MUT_PAIN_2,10,NULL,NULL,s_mutchase1},	// s_mutpain1
	{false,SPR_MUT_SHOOT1,6,NULL,(statefunc)T_Shoot,s_mutshoot2},	// s_mutshoot1
	{false,SPR_MUT_SHOOT2,20,NULL,NULL,s_mutshoot3},	// s_mutshoot2
	{false,SPR_MUT_SHOOT3,10,NULL,(statefunc)T_Shoot,s_mutshoot4},	// s_mutshoot3
	{false,SPR_MUT_SHOOT4,20,NULL,NULL,s_mutchase1},	// s_mutshoot4
	{true,SPR_MUT_W1_1,10,(statefunc)T_Chase,NULL,s_mutchase1s},	// s_mutchase1
	{true,SPR_MUT_W1_1,3,NULL,NULL,s_mutchase2},	// s_mutchase1s
	{true,SPR_MUT_W2_1,8,(statefunc)T_Chase,NULL,s_mutchase3},	// s_mutchase2
	{true,SPR_MUT_W3_1,10,(statefunc)T_Chase,NULL,s_mutchase3s},	// s_mutchase3
	{true,SPR_MUT_W3_1,3,NULL,NULL,s_mutchase4},	// s_mutchase3s
	{true,SPR_MUT_W4_1,8,(statefunc)T_Chase,NULL,s_mutchase1},	// s_mutchase4
	{false,SPR_MUT_DIE_1,7,NULL,(statefunc)A_DeathScream,s_mutdie2},	// s_mutdie1
	{false,SPR_MUT_DIE_2,7,NULL,NULL,s_mutdie3},	// s_mutdie2
	{false,SPR_MUT_DIE_3,7,NULL,NULL,s_mutdie4},	// s_mutdie3
	{false,SPR_MUT_DIE_4,7,NULL,NULL,s_mutdie5},	// s_mutdie4
	{false,SPR_MUT_DEAD,0,NULL,NULL,s_mutdie5},	// s_mutdie5
	{true,SPR_SS_S_1,0,(statefunc)T_Stand,NULL,s_ssstand},	// s_ssstand
	{true,SPR_SS_W1_1,20,(statefunc)T_Path,NULL,s_sspath1s},	// s_sspath1
	{true,SPR_SS_W1_1,5,NULL,NULL,s_sspath2},	// s_sspath1s
	{true,SPR_SS_W2_1,15,(statefunc)T_Path,NULL,s_sspath3},	// s_sspath2
	{true,SPR_SS_W3_1,20,(statefunc)T_Path,NULL,s_sspath3s},	// s_sspath3
	{true,SPR_SS_W3_1,5,NULL,NULL,s_sspath4},	// s_sspath3s
	{true,SPR_SS_W4_1,15,(statefunc)T_Path,NULL,s_sspath1},	// s_sspath4
	{2,SPR_SS_PAIN_1,10,NULL,NULL,s_sschase1},	// s_sspain
	{2,SPR_SS_PAIN_2,10,NULL,NULL,s_sschase1},	// s_sspain1
	{false,SPR_SS_SHOOT1,20,NULL,NULL,s_ssshoot2},	// s_ssshoot1
	{false,SPR_SS_SHOOT2,20,NULL,(statefunc)T_Shoot,s_ssshoot3},	// s_ssshoot2
	{false,SPR_SS_SHOOT3,10,NULL,NULL,s_ssshoot4},	// s_ssshoot3
	{false,SPR_SS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_ssshoot5},	// s_ssshoot4
	{false,SPR_SS_SHOOT3,10,NULL,NULL,s_ssshoot6},	// s_ssshoot5
	{false,SPR_SS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_ssshoot7},	// s_ssshoot6
	{false,SPR_SS_SHOOT3,10,NULL,NULL,s_ssshoot8},	// s_ssshoot7
	{false,SPR_SS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_ssshoot9},	// s_ssshoot8
	{false,SPR_SS_SHOOT3,10,NULL,NULL,s_sschase1},	// s_ssshoot9
	{true,SPR_SS_W1_1,10,(statefunc)T_Chase,NULL,s_sschase1s},	// s_sschase1
	{true,SPR_SS_W1_1,3,NULL,NULL,s_sschase2},	// s_sschase1s
	{true,SPR_SS_W2_1,8,(statefunc)T_Chase,NULL,s_sschase3},	// s_sschase2
	{true,SPR_SS_W3_1,10,(statefunc)T_Chase,NULL,s_sschase3s},	// s_sschase3
	{true,SPR_SS_W3_1,3,NULL,NULL,s_sschase4},	// s_sschase3s
	{true,SPR_SS_W4_1,8,(statefunc)T_Chase,NULL,s_sschase1},	// s_sschase4
	{false,SPR_SS_DIE_1,15,NULL,(statefunc)A_DeathScream,s_ssdie2},	// s_ssdie1
	{false,SPR_SS_DIE_2,15,NULL,NULL,s_ssdie3},	// s_ssdie2
	{false,SPR_SS_DIE_3,15,NULL,NULL,s_ssdie4},	// s_ssdie3
	{false,SPR_SS_DEAD,0,NULL,NULL,s_ssdie4},	// s_ssdie4
#ifndef SPEAR
	{false,SPR_BOSS_W1,0,(statefunc)T_Stand,NULL,s_bossstand},	// s_bossstand
	{false,SPR_BOSS_W1,10,(statefunc)T_Chase,NULL,s_bosschase1s},	// s_bosschase1
	{false,SPR_BOSS_W1,3,NULL,NULL,s_bosschase2},	// s_bosschase1s
	{false,SPR_BOSS_W2,8,(statefunc)T_Chase,NULL,s_bosschase3},	// s_bosschase2
	{false,SPR_BOSS_W3,10,(statefunc)T_Chase,NULL,s_bosschase3s},	// s_bosschase3
	{false,SPR_BOSS_W3,3,NULL,NULL,s_bosschase4},	// s_bosschase3s
	{false,SPR_BOSS_W4,8,(statefunc)T_Chase,NULL,s_bosschase1},	// s_bosschase4
	{false,SPR_BOSS_DIE1,15,NULL,(statefunc)A_DeathScream,s_bossdie2},	// s_bossdie1
	{false,SPR_BOSS_DIE2,15,NULL,NULL,s_bossdie3},	// s_bossdie2
	{false,SPR_BOSS_DIE3,15,NULL,NULL,s_bossdie4},	// s_bossdie3
	{false,SPR_BOSS_DEAD,0,NULL,NULL,s_bossdie4},	// s_bossdie4
	{false,SPR_BOSS_SHOOT1,30,NULL,NULL,s_bossshoot2},	// s_bossshoot1
	{false,SPR_BOSS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_bossshoot3},	// s_bossshoot2
	{false,SPR_BOSS_SHOOT3,10,NULL,(statefunc)T_Shoot,s_bossshoot4},	// s_bossshoot3
	{false,SPR_BOSS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_bossshoot5},	// s_bossshoot4
	{false,SPR_BOSS_SHOOT3,10,NULL,(statefunc)T_Shoot,s_bossshoot6},	// s_bossshoot5
	{false,SPR_BOSS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_bossshoot7},	// s_bossshoot6
	{false,SPR_BOSS_SHOOT3,10,NULL,(statefunc)T_Shoot,s_bossshoot8},	// s_bossshoot7
	{false,SPR_BOSS_SHOOT1,10,NULL,NULL,s_bosschase1},	// s_bossshoot8
	{false,SPR_GRETEL_W1,0,(statefunc)T_Stand,NULL,s_gretelstand},	// s_gretelstand
	{false,SPR_GRETEL_W1,10,(statefunc)T_Chase,NULL,s_gretelchase1s},	// s_gretelchase1
	{false,SPR_GRETEL_W1,3,NULL,NULL,s_gretelchase2},	// s_gretelchase1s
	{false,SPR_GRETEL_W2,8,(statefunc)T_Chase,NULL,s_gretelchase3},	// s_gretelchase2
	{false,SPR_GRETEL_W3,10,(statefunc)T_Chase,NULL,s_gretelchase3s},	// s_gretelchase3
	{false,SPR_GRETEL_W3,3,NULL,NULL,s_gretelchase4},	// s_gretelchase3s
	{false,SPR_GRETEL_W4,8,(statefunc)T_Chase,NULL,s_gretelchase1},	// s_gretelchase4
	{false,SPR_GRETEL_DIE1,15,NULL,(statefunc)A_DeathScream,s_greteldie2},	// s_greteldie1
	{false,SPR_GRETEL_DIE2,15,NULL,NULL,s_greteldie3},	// s_greteldie2
	{false,SPR_GRETEL_DIE3,15,NULL,NULL,s_greteldie4},	// s_greteldie3
	{false,SPR_GRETEL_DEAD,0,NULL,NULL,s_greteldie4},	// s_greteldie4
	{false,SPR_GRETEL_SHOOT1,30,NULL,NULL,s_gretelshoot2},	// s_gretelshoot1
	{false,SPR_GRETEL_SHOOT2,10,NULL,(statefunc)T_Shoot,s_gretelshoot3},	// s_gretelshoot2
	{false,SPR_GRETEL_SHOOT3,10,NULL,(statefunc)T_Shoot,s_gretelshoot4},	// s_gretelshoot3
	{false,SPR_GRETEL_SHOOT2,10,NULL,(statefunc)T_Shoot,s_gretelshoot5},	// s_gretelshoot4
	{false,SPR_GRETEL_SHOOT3,10,NULL,(statefunc)T_Shoot,s_gretelshoot6},	// s_gretelshoot5
	{false,SPR_GRETEL_SHOOT2,10,NULL,(statefunc)T_Shoot,s_gretelshoot7},	// s_gretelshoot6
	{false,SPR_GRETEL_SHOOT3,10,NULL,(statefunc)T_Shoot,s_gretelshoot8},	// s_gretelshoot7
	{false,SPR_GRETEL_SHOOT1,10,NULL,NULL,s_gretelchase1},	// s_gretelshoot8
#endif
#ifdef SPEAR
	{false,SPR_TRANS_W1,0,(statefunc)T_Stand,NULL,s_transstand},	// s_transstand
	{false,SPR_TRANS_W1,10,(statefunc)T_Chase,NULL,s_transchase1s},	// s_transchase1
	{false,SPR_TRANS_W1,3,NULL,NULL,s_transchase2},	// s_transchase1s
	{false,SPR_TRANS_W2,8,(statefunc)T_Chase,NULL,s_transchase3},	// s_transchase2
	{false,SPR_TRANS_W3,10,(statefunc)T_Chase,NULL,s_transchase3s},	// s_transchase3
	{false,SPR_TRANS_W3,3,NULL,NULL,s_transchase4},	// s_transchase3s
	{false,SPR_TRANS_W4,8,(statefunc)T_Chase,NULL,s_transchase1},	// s_transchase4
	{false,SPR_TRANS_W1,1,NULL,(statefunc)A_DeathScream,s_transdie01},	// s_transdie0
	{false,SPR_TRANS_W1,1,NULL,NULL,s_transdie1},	// s_transdie01
	{false,SPR_TRANS_DIE1,15,NULL,NULL,s_transdie2},	// s_transdie1
	{false,SPR_TRANS_DIE2,15,NULL,NULL,s_transdie3},	// s_transdie2
	{false,SPR_TRANS_DIE3,15,NULL,NULL,s_transdie4},	// s_transdie3
	{false,SPR_TRANS_DEAD,0,NULL,NULL,s_transdie4},	// s_transdie4
	{false,SPR_TRANS_SHOOT1,30,NULL,NULL,s_transshoot2},	// s_transshoot1
	{false,SPR_TRANS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_transshoot3},	// s_transshoot2
	{false,SPR_TRANS_SHOOT3,10,NULL,(statefunc)T_Shoot,s_transshoot4},	// s_transshoot3
	{false,SPR_TRANS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_transshoot5},	// s_transshoot4
	{false,SPR_TRANS_SHOOT3,10,NULL,(statefunc)T_Shoot,s_transshoot6},	// s_transshoot5
	{false,SPR_TRANS_SHOOT2,10,NULL,(statefunc)T_Shoot,s_transshoot7},	// s_transshoot6
	{false,SPR_TRANS_SHOOT3,10,NULL,(statefunc)T_Shoot,s_transshoot8},	// s_transshoot7
	{false,SPR_TRANS_SHOOT1,10,NULL,NULL,s_transchase1},	// s_transshoot8
	{false,SPR_UBER_W1,0,(statefunc)T_Stand,NULL,s_uberstand},	// s_uberstand
	{false,SPR_UBER_W1,10,(statefunc)T_Chase,NULL,s_uberchase1s},	// s_uberchase1
	{false,SPR_UBER_W1,3,NULL,NULL,s_uberchase2},	// s_uberchase1s
	{false,SPR_UBER_W2,8,(statefunc)T_Chase,NULL,s_uberchase3},	// s_uberchase2
	{false,SPR_UBER_W3,10,(statefunc)T_Chase,NULL,s_uberchase3s},	// s_uberchase3
	{false,SPR_UBER_W3,3,NULL,NULL,s_uberchase4},	// s_uberchase3s
	{false,SPR_UBER_W4,8,(statefunc)T_Chase,NULL,s_uberchase1},	// s_uberchase4
	{false,SPR_UBER_W1,1,NULL,(statefunc)A_DeathScream,s_uberdie01},	// s_uberdie0
	{false,SPR_UBER_W1,1,NULL,NULL,s_uberdie1},	// s_uberdie01
	{false,SPR_UBER_DIE1,15,NULL,NULL,s_uberdie2},	// s_uberdie1
	{false,SPR_UBER_DIE2,15,NULL,NULL,s_uberdie3},	// s_uberdie2
	{false,SPR_UBER_DIE3,15,NULL,NULL,s_uberdie4},	// s_uberdie3
	{false,SPR_UBER_DIE4,15,NULL,NULL,s_uberdie5},	// s_uberdie4
	{false,SPR_UBER_DEAD,0,NULL,NULL,s_uberdie5},	// s_uberdie5
	{false,SPR_UBER_SHOOT1,30,NULL,NULL,s_ubershoot2},	// s_ubershoot1
	{false,SPR_UBER_SHOOT2,12,NULL,(statefunc)T_UShoot,s_ubershoot3},	// s_ubershoot2
	{false,SPR_UBER_SHOOT3,12,NULL,(statefunc)T_UShoot,s_ubershoot4},	// s_ubershoot3
	{false,SPR_UBER_SHOOT4,12,NULL,(statefunc)T_UShoot,s_ubershoot5},	// s_ubershoot4
	{false,SPR_UBER_SHOOT3,12,NULL,(statefunc)T_UShoot,s_ubershoot6},	// s_ubershoot5
	{false,SPR_UBER_SHOOT2,12,NULL,(statefunc)T_UShoot,s_ubershoot7},	// s_ubershoot6
	{false,SPR_UBER_SHOOT1,12,NULL,NULL,s_uberchase1},	// s_ubershoot7
	{false,SPR_WILL_W1,0,(statefunc)T_Stand,NULL,s_willstand},	// s_willstand
	{false,SPR_WILL_W1,10,(statefunc)T_Will,NULL,s_willchase1s},	// s_willchase1
	{false,SPR_WILL_W1,3,NULL,NULL,s_willchase2},	// s_willchase1s
	{false,SPR_WILL_W2,8,(statefunc)T_Will,NULL,s_willchase3},	// s_willchase2
	{false,SPR_WILL_W3,10,(statefunc)T_Will,NULL,s_willchase3s},	// s_willchase3
	{false,SPR_WILL_W3,3,NULL,NULL,s_willchase4},	// s_willchase3s
	{false,SPR_WILL_W4,8,(statefunc)T_Will,NULL,s_willchase1},	// s_willchase4
	{false,SPR_WILL_W1,1,NULL,NULL,s_willdie1},	// s_willdeathcam
	{false,SPR_WILL_W1,1,NULL,(statefunc)A_DeathScream,s_willdie2},	// s_willdie1
	{false,SPR_WILL_W1,10,NULL,NULL,s_willdie3},	// s_willdie2
	{false,SPR_WILL_DIE1,10,NULL,NULL,s_willdie4},	// s_willdie3
	{false,SPR_WILL_DIE2,10,NULL,NULL,s_willdie5},	// s_willdie4
	{false,SPR_WILL_DIE3,10,NULL,NULL,s_willdie6},	// s_willdie5
	{false,SPR_WILL_DEAD,20,NULL,NULL,s_willdie6},	// s_willdie6
	{false,SPR_WILL_SHOOT1,30,NULL,NULL,s_willshoot2},	// s_willshoot1
	{false,SPR_WILL_SHOOT2,10,NULL,(statefunc)T_Launch,s_willshoot3},	// s_willshoot2
	{false,SPR_WILL_SHOOT3,10,NULL,(statefunc)T_Shoot,s_willshoot4},	// s_willshoot3
	{false,SPR_WILL_SHOOT4,10,NULL,(statefunc)T_Shoot,s_willshoot5},	// s_willshoot4
	{false,SPR_WILL_SHOOT3,10,NULL,(statefunc)T_Shoot,s_willshoot6},	// s_willshoot5
	{false,SPR_WILL_SHOOT4,10,NULL,(statefunc)T_Shoot,s_willchase1},	// s_willshoot6
	{false,SPR_DEATH_W1,0,(statefunc)T_Stand,NULL,s_deathstand},	// s_deathstand
	{false,SPR_DEATH_W1,10,(statefunc)T_Will,NULL,s_deathchase1s},	// s_deathchase1
	{false,SPR_DEATH_W1,3,NULL,NULL,s_deathchase2},	// s_deathchase1s
	{false,SPR_DEATH_W2,8,(statefunc)T_Will,NULL,s_deathchase3},	// s_deathchase2
	{false,SPR_DEATH_W3,10,(statefunc)T_Will,NULL,s_deathchase3s},	// s_deathchase3
	{false,SPR_DEATH_W3,3,NULL,NULL,s_deathchase4},	// s_deathchase3s
	{false,SPR_DEATH_W4,8,(statefunc)T_Will,NULL,s_deathchase1},	// s_deathchase4
	{false,SPR_DEATH_W1,1,NULL,NULL,s_deathdie1},	// s_deathdeathcam
	{false,SPR_DEATH_W1,1,NULL,(statefunc)A_DeathScream,s_deathdie2},	// s_deathdie1
	{false,SPR_DEATH_W1,10,NULL,NULL,s_deathdie3},	// s_deathdie2
	{false,SPR_DEATH_DIE1,10,NULL,NULL,s_deathdie4},	// s_deathdie3
	{false,SPR_DEATH_DIE2,10,NULL,NULL,s_deathdie5},	// s_deathdie4
	{false,SPR_DEATH_DIE3,10,NULL,NULL,s_deathdie6},	// s_deathdie5
	{false,SPR_DEATH_DIE4,10,NULL,NULL,s_deathdie7},	// s_deathdie6
	{false,SPR_DEATH_DIE5,10,NULL,NULL,s_deathdie8},	// s_deathdie7
	{false,SPR_DEATH_DIE6,10,NULL,NULL,s_deathdie9},	// s_deathdie8
	{false,SPR_DEATH_DEAD,0,NULL,NULL,s_deathdie9},	// s_deathdie9
	{false,SPR_DEATH_SHOOT1,30,NULL,NULL,s_deathshoot2},	// s_deathshoot1
	{false,SPR_DEATH_SHOOT2,10,NULL,(statefunc)T_Launch,s_deathshoot3},	// s_deathshoot2
	{false,SPR_DEATH_SHOOT4,10,NULL,(statefunc)T_Shoot,s_deathshoot4},	// s_deathshoot3
	{false,SPR_DEATH_SHOOT3,10,NULL,(statefunc)T_Launch,s_deathshoot5},	// s_deathshoot4
	{false,SPR_DEATH_SHOOT4,10,NULL,(statefunc)T_Shoot,s_deathchase1},	// s_deathshoot5
	{false,SPR_ANGEL_W1,0,(statefunc)T_Stand,NULL,s_angelstand},	// s_angelstand
	{false,SPR_ANGEL_W1,10,(statefunc)T_Will,NULL,s_angelchase1s},	// s_angelchase1
	{false,SPR_ANGEL_W1,3,NULL,NULL,s_angelchase2},	// s_angelchase1s
	{false,SPR_ANGEL_W2,8,(statefunc)T_Will,NULL,s_angelchase3},	// s_angelchase2
	{false,SPR_ANGEL_W3,10,(statefunc)T_Will,NULL,s_angelchase3s},	// s_angelchase3
	{false,SPR_ANGEL_W3,3,NULL,NULL,s_angelchase4},	// s_angelchase3s
	{false,SPR_ANGEL_W4,8,(statefunc)T_Will,NULL,s_angelchase1},	// s_angelchase4
	{false,SPR_ANGEL_W1,1,NULL,(statefunc)A_DeathScream,s_angeldie11},	// s_angeldie1
	{false,SPR_ANGEL_W1,1,NULL,NULL,s_angeldie2},	// s_angeldie11
	{false,SPR_ANGEL_DIE1,10,NULL,(statefunc)A_Slurpie,s_angeldie3},	// s_angeldie2
	{false,SPR_ANGEL_DIE2,10,NULL,NULL,s_angeldie4},	// s_angeldie3
	{false,SPR_ANGEL_DIE3,10,NULL,NULL,s_angeldie5},	// s_angeldie4
	{false,SPR_ANGEL_DIE4,10,NULL,NULL,s_angeldie6},	// s_angeldie5
	{false,SPR_ANGEL_DIE5,10,NULL,NULL,s_angeldie7},	// s_angeldie6
	{false,SPR_ANGEL_DIE6,10,NULL,NULL,s_angeldie8},	// s_angeldie7
	{false,SPR_ANGEL_DIE7,10,NULL,NULL,s_angeldie9},	// s_angeldie8
	{false,SPR_ANGEL_DEAD,130,NULL,(statefunc)A_Victory,s_angeldie9},	// s_angeldie9
	{false,SPR_ANGEL_SHOOT1,10,NULL,(statefunc)A_StartAttack,s_angelshoot2},	// s_angelshoot1
	{false,SPR_ANGEL_SHOOT2,20,NULL,(statefunc)T_Launch,s_angelshoot3},	// s_angelshoot2
	{false,SPR_ANGEL_SHOOT1,10,NULL,(statefunc)A_Relaunch,s_angelshoot2},	// s_angelshoot3
	{false,SPR_ANGEL_TIRED1,40,NULL,(statefunc)A_Breathing,s_angeltired2},	// s_angeltired
	{false,SPR_ANGEL_TIRED2,40,NULL,NULL,s_angeltired3},	// s_angeltired2
	{false,SPR_ANGEL_TIRED1,40,NULL,(statefunc)A_Breathing,s_angeltired4},	// s_angeltired3
	{false,SPR_ANGEL_TIRED2,40,NULL,NULL,s_angeltired5},	// s_angeltired4
	{false,SPR_ANGEL_TIRED1,40,NULL,(statefunc)A_Breathing,s_angeltired6},	// s_angeltired5
	{false,SPR_ANGEL_TIRED2,40,NULL,NULL,s_angeltired7},	// s_angeltired6
	{false,SPR_ANGEL_TIRED1,40,NULL,(statefunc)A_Breathing,s_angelchase1},	// s_angeltired7
	{false,SPR_SPARK1,6,(statefunc)T_Projectile,NULL,s_spark2},	// s_spark1
	{false,SPR_SPARK2,6,(statefunc)T_Projectile,NULL,s_spark3},	// s_spark2
	{false,SPR_SPARK3,6,(statefunc)T_Projectile,NULL,s_spark4},	// s_spark3
	{false,SPR_SPARK4,6,(statefunc)T_Projectile,NULL,s_spark1},	// s_spark4
	{false,SPR_SPECTRE_W1,10,(statefunc)T_Stand,NULL,s_spectrewait2},	// s_spectrewait1
	{false,SPR_SPECTRE_W2,10,(statefunc)T_Stand,NULL,s_spectrewait3},	// s_spectrewait2
	{false,SPR_SPECTRE_W3,10,(statefunc)T_Stand,NULL,s_spectrewait4},	// s_spectrewait3
	{false,SPR_SPECTRE_W4,10,(statefunc)T_Stand,NULL,s_spectrewait1},	// s_spectrewait4
	{false,SPR_SPECTRE_W1,10,(statefunc)T_Ghosts,NULL,s_spectrechase2},	// s_spectrechase1
	{false,SPR_SPECTRE_W2,10,(statefunc)T_Ghosts,NULL,s_spectrechase3},	// s_spectrechase2
	{false,SPR_SPECTRE_W3,10,(statefunc)T_Ghosts,NULL,s_spectrechase4},	// s_spectrechase3
	{false,SPR_SPECTRE_W4,10,(statefunc)T_Ghosts,NULL,s_spectrechase1},	// s_spectrechase4
	{false,SPR_SPECTRE_F1,10,NULL,NULL,s_spectredie2},	// s_spectredie1
	{false,SPR_SPECTRE_F2,10,NULL,NULL,s_spectredie3},	// s_spectredie2
	{false,SPR_SPECTRE_F3,10,NULL,NULL,s_spectredie4},	// s_spectredie3
	{false,SPR_SPECTRE_F4,300,NULL,NULL,s_spectrewake},	// s_spectredie4
	{false,SPR_SPECTRE_F4,10,NULL,(statefunc)A_Dormant,s_spectrewake},	// s_spectrewake
#endif
#ifndef SPEAR
	{false,SPR_SCHABB_W1,0,(statefunc)T_Stand,NULL,s_schabbstand},	// s_schabbstand
	{false,SPR_SCHABB_W1,10,(statefunc)T_Schabb,NULL,s_schabbchase1s},	// s_schabbchase1
	{false,SPR_SCHABB_W1,3,NULL,NULL,s_schabbchase2},	// s_schabbchase1s
	{false,SPR_SCHABB_W2,8,(statefunc)T_Schabb,NULL,s_schabbchase3},	// s_schabbchase2
	{false,SPR_SCHABB_W3,10,(statefunc)T_Schabb,NULL,s_schabbchase3s},	// s_schabbchase3
	{false,SPR_SCHABB_W3,3,NULL,NULL,s_schabbchase4},	// s_schabbchase3s
	{false,SPR_SCHABB_W4,8,(statefunc)T_Schabb,NULL,s_schabbchase1},	// s_schabbchase4
	{false,SPR_SCHABB_W1,1,NULL,NULL,s_schabbdie1},	// s_schabbdeathcam
	{false,SPR_SCHABB_W1,10,NULL,(statefunc)A_DeathScream,s_schabbdie2},	// s_schabbdie1
	{false,SPR_SCHABB_W1,10,NULL,NULL,s_schabbdie3},	// s_schabbdie2
	{false,SPR_SCHABB_DIE1,10,NULL,NULL,s_schabbdie4},	// s_schabbdie3
	{false,SPR_SCHABB_DIE2,10,NULL,NULL,s_schabbdie5},	// s_schabbdie4
	{false,SPR_SCHABB_DIE3,10,NULL,NULL,s_schabbdie6},	// s_schabbdie5
	{false,SPR_SCHABB_DEAD,20,NULL,(statefunc)A_StartDeathCam,s_schabbdie6},	// s_schabbdie6
	{false,SPR_SCHABB_SHOOT1,30,NULL,NULL,s_schabbshoot2},	// s_schabbshoot1
	{false,SPR_SCHABB_SHOOT2,10,NULL,(statefunc)T_SchabbThrow,s_schabbchase1},	// s_schabbshoot2
	{false,SPR_HYPO1,6,(statefunc)T_Projectile,NULL,s_needle2},	// s_needle1
	{false,SPR_HYPO2,6,(statefunc)T_Projectile,NULL,s_needle3},	// s_needle2
	{false,SPR_HYPO3,6,(statefunc)T_Projectile,NULL,s_needle4},	// s_needle3
	{false,SPR_HYPO4,6,(statefunc)T_Projectile,NULL,s_needle1},	// s_needle4
	{false,SPR_GIFT_W1,0,(statefunc)T_Stand,NULL,s_giftstand},	// s_giftstand
	{false,SPR_GIFT_W1,10,(statefunc)T_Gift,NULL,s_giftchase1s},	// s_giftchase1
	{false,SPR_GIFT_W1,3,NULL,NULL,s_giftchase2},	// s_giftchase1s
	{false,SPR_GIFT_W2,8,(statefunc)T_Gift,NULL,s_giftchase3},	// s_giftchase2
	{false,SPR_GIFT_W3,10,(statefunc)T_Gift,NULL,s_giftchase3s},	// s_giftchase3
	{false,SPR_GIFT_W3,3,NULL,NULL,s_giftchase4},	// s_giftchase3s
	{false,SPR_GIFT_W4,8,(statefunc)T_Gift,NULL,s_giftchase1},	// s_giftchase4
	{false,SPR_GIFT_W1,1,NULL,NULL,s_giftdie1},	// s_giftdeathcam
	{false,SPR_GIFT_W1,1,NULL,(statefunc)A_DeathScream,s_giftdie2},	// s_giftdie1
	{false,SPR_GIFT_W1,10,NULL,NULL,s_giftdie3},	// s_giftdie2
	{false,SPR_GIFT_DIE1,10,NULL,NULL,s_giftdie4},	// s_giftdie3
	{false,SPR_GIFT_DIE2,10,NULL,NULL,s_giftdie5},	// s_giftdie4
	{false,SPR_GIFT_DIE3,10,NULL,NULL,s_giftdie6},	// s_giftdie5
	{false,SPR_GIFT_DEAD,20,NULL,(statefunc)A_StartDeathCam,s_giftdie6},	// s_giftdie6
	{false,SPR_GIFT_SHOOT1,30,NULL,NULL,s_giftshoot2},	// s_giftshoot1
	{false,SPR_GIFT_SHOOT2,10,NULL,(statefunc)T_GiftThrow,s_giftchase1},	// s_giftshoot2
	{false,SPR_FAT_W1,0,(statefunc)T_Stand,NULL,s_fatstand},	// s_fatstand
	{false,SPR_FAT_W1,10,(statefunc)T_Fat,NULL,s_fatchase1s},	// s_fatchase1
	{false,SPR_FAT_W1,3,NULL,NULL,s_fatchase2},	// s_fatchase1s
	{false,SPR_FAT_W2,8,(statefunc)T_Fat,NULL,s_fatchase3},	// s_fatchase2
	{false,SPR_FAT_W3,10,(statefunc)T_Fat,NULL,s_fatchase3s},	// s_fatchase3
	{false,SPR_FAT_W3,3,NULL,NULL,s_fatchase4},	// s_fatchase3s
	{false,SPR_FAT_W4,8,(statefunc)T_Fat,NULL,s_fatchase1},	// s_fatchase4
	{false,SPR_FAT_W1,1,NULL,NULL,s_fatdie1},	// s_fatdeathcam
	{false,SPR_FAT_W1,1,NULL,(statefunc)A_DeathScream,s_fatdie2},	// s_fatdie1
	{false,SPR_FAT_W1,10,NULL,NULL,s_fatdie3},	// s_fatdie2
	{false,SPR_FAT_DIE1,10,NULL,NULL,s_fatdie4},	// s_fatdie3
	{false,SPR_FAT_DIE2,10,NULL,NULL,s_fatdie5},	// s_fatdie4
	{false,SPR_FAT_DIE3,10,NULL,NULL,s_fatdie6},	// s_fatdie5
	{false,SPR_FAT_DEAD,20,NULL,(statefunc)A_StartDeathCam,s_fatdie6},	// s_fatdie6
	{false,SPR_FAT_SHOOT1,30,NULL,NULL,s_fatshoot2},	// s_fatshoot1
	{false,SPR_FAT_SHOOT2,10,NULL,(statefunc)T_GiftThrow,s_fatshoot3},	// s_fatshoot2
	{false,SPR_FAT_SHOOT3,10,NULL,(statefunc)T_Shoot,s_fatshoot4},	// s_fatshoot3
	{false,SPR_FAT_SHOOT4,10,NULL,(statefunc)T_Shoot,s_fatshoot5},	// s_fatshoot4
	{false,SPR_FAT_SHOOT3,10,NULL,(statefunc)T_Shoot,s_fatshoot6},	// s_fatshoot5
	{false,SPR_FAT_SHOOT4,10,NULL,(statefunc)T_Shoot,s_fatchase1},	// s_fatshoot6
	{false,SPR_FAKE_W1,0,(statefunc)T_Stand,NULL,s_fakestand},	// s_fakestand
	{false,SPR_FAKE_W1,10,(statefunc)T_Fake,NULL,s_fakechase1s},	// s_fakechase1
	{false,SPR_FAKE_W1,3,NULL,NULL,s_fakechase2},	// s_fakechase1s
	{false,SPR_FAKE_W2,8,(statefunc)T_Fake,NULL,s_fakechase3},	// s_fakechase2
	{false,SPR_FAKE_W3,10,(statefunc)T_Fake,NULL,s_fakechase3s},	// s_fakechase3
	{false,SPR_FAKE_W3,3,NULL,NULL,s_fakechase4},	// s_fakechase3s
	{false,SPR_FAKE_W4,8,(statefunc)T_Fake,NULL,s_fakechase1},	// s_fakechase4
	{false,SPR_FAKE_DIE1,10,NULL,(statefunc)A_DeathScream,s_fakedie2},	// s_fakedie1
	{false,SPR_FAKE_DIE2,10,NULL,NULL,s_fakedie3},	// s_fakedie2
	{false,SPR_FAKE_DIE3,10,NULL,NULL,s_fakedie4},	// s_fakedie3
	{false,SPR_FAKE_DIE4,10,NULL,NULL,s_fakedie5},	// s_fakedie4
	{false,SPR_FAKE_DIE5,10,NULL,NULL,s_fakedie6},	// s_fakedie5
	{false,SPR_FAKE_DEAD,0,NULL,NULL,s_fakedie6},	// s_fakedie6
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot2},	// s_fakeshoot1
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot3},	// s_fakeshoot2
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot4},	// s_fakeshoot3
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot5},	// s_fakeshoot4
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot6},	// s_fakeshoot5
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot7},	// s_fakeshoot6
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot8},	// s_fakeshoot7
	{false,SPR_FAKE_SHOOT,8,NULL,(statefunc)T_FakeFire,s_fakeshoot9},	// s_fakeshoot8
	{false,SPR_FAKE_SHOOT,8,NULL,NULL,s_fakechase1},	// s_fakeshoot9
	{false,SPR_FIRE1,6,NULL,(statefunc)T_Projectile,s_fire2},	// s_fire1
	{false,SPR_FIRE2,6,NULL,(statefunc)T_Projectile,s_fire1},	// s_fire2
	{false,SPR_MECHA_W1,0,(statefunc)T_Stand,NULL,s_mechastand},	// s_mechastand
	{false,SPR_MECHA_W1,10,(statefunc)T_Chase,(statefunc)A_MechaSound,s_mechachase1s},	// s_mechachase1
	{false,SPR_MECHA_W1,6,NULL,NULL,s_mechachase2},	// s_mechachase1s
	{false,SPR_MECHA_W2,8,(statefunc)T_Chase,NULL,s_mechachase3},	// s_mechachase2
	{false,SPR_MECHA_W3,10,(statefunc)T_Chase,(statefunc)A_MechaSound,s_mechachase3s},	// s_mechachase3
	{false,SPR_MECHA_W3,6,NULL,NULL,s_mechachase4},	// s_mechachase3s
	{false,SPR_MECHA_W4,8,(statefunc)T_Chase,NULL,s_mechachase1},	// s_mechachase4
	{false,SPR_MECHA_DIE1,10,NULL,(statefunc)A_DeathScream,s_mechadie2},	// s_mechadie1
	{false,SPR_MECHA_DIE2,10,NULL,NULL,s_mechadie3},	// s_mechadie2
	{false,SPR_MECHA_DIE3,10,NULL,(statefunc)A_HitlerMorph,s_mechadie4},	// s_mechadie3
	{false,SPR_MECHA_DEAD,0,NULL,NULL,s_mechadie4},	// s_mechadie4
	{false,SPR_MECHA_SHOOT1,30,NULL,NULL,s_mechashoot2},	// s_mechashoot1
	{false,SPR_MECHA_SHOOT2,10,NULL,(statefunc)T_Shoot,s_mechashoot3},	// s_mechashoot2
	{false,SPR_MECHA_SHOOT3,10,NULL,(statefunc)T_Shoot,s_mechashoot4},	// s_mechashoot3
	{false,SPR_MECHA_SHOOT2,10,NULL,(statefunc)T_Shoot,s_mechashoot5},	// s_mechashoot4
	{false,SPR_MECHA_SHOOT3,10,NULL,(statefunc)T_Shoot,s_mechashoot6},	// s_mechashoot5
	{false,SPR_MECHA_SHOOT2,10,NULL,(statefunc)T_Shoot,s_mechachase1},	// s_mechashoot6
	{false,SPR_HITLER_W1,6,(statefunc)T_Chase,NULL,s_hitlerchase1s},	// s_hitlerchase1
	{false,SPR_HITLER_W1,4,NULL,NULL,s_hitlerchase2},	// s_hitlerchase1s
	{false,SPR_HITLER_W2,2,(statefunc)T_Chase,NULL,s_hitlerchase3},	// s_hitlerchase2
	{false,SPR_HITLER_W3,6,(statefunc)T_Chase,NULL,s_hitlerchase3s},	// s_hitlerchase3
	{false,SPR_HITLER_W3,4,NULL,NULL,s_hitlerchase4},	// s_hitlerchase3s
	{false,SPR_HITLER_W4,2,(statefunc)T_Chase,NULL,s_hitlerchase1},	// s_hitlerchase4
	{false,SPR_HITLER_W1,10,NULL,NULL,s_hitlerdie1},	// s_hitlerdeathcam
	{false,SPR_HITLER_W1,1,NULL,(statefunc)A_DeathScream,s_hitlerdie2},	// s_hitlerdie1
	{false,SPR_HITLER_W1,10,NULL,NULL,s_hitlerdie3},	// s_hitlerdie2
	{false,SPR_HITLER_DIE1,10,NULL,(statefunc)A_Slurpie,s_hitlerdie4},	// s_hitlerdie3
	{false,SPR_HITLER_DIE2,10,NULL,NULL,s_hitlerdie5},	// s_hitlerdie4
	{false,SPR_HITLER_DIE3,10,NULL,NULL,s_hitlerdie6},	// s_hitlerdie5
	{false,SPR_HITLER_DIE4,10,NULL,NULL,s_hitlerdie7},	// s_hitlerdie6
	{false,SPR_HITLER_DIE5,10,NULL,NULL,s_hitlerdie8},	// s_hitlerdie7
	{false,SPR_HITLER_DIE6,10,NULL,NULL,s_hitlerdie9},	// s_hitlerdie8
	{false,SPR_HITLER_DIE7,10,NULL,NULL,s_hitlerdie10},	// s_hitlerdie9
	{false,SPR_HITLER_DEAD,20,NULL,(statefunc)A_StartDeathCam,s_hitlerdie10},	// s_hitlerdie10
	{false,SPR_HITLER_SHOOT1,30,NULL,NULL,s_hitlershoot2},	// s_hitlershoot1
	{false,SPR_HITLER_SHOOT2,10,NULL,(statefunc)T_Shoot,s_hitlershoot3},	// s_hitlershoot2
	{false,SPR_HITLER_SHOOT3,10,NULL,(statefunc)T_Shoot,s_hitlershoot4},	// s_hitlershoot3
	{false,SPR_HITLER_SHOOT2,10,NULL,(statefunc)T_Shoot,s_hitlershoot5},	// s_hitlershoot4
	{false,SPR_HITLER_SHOOT3,10,NULL,(statefunc)T_Shoot,s_hitlershoot6},	// s_hitlershoot5
	{false,SPR_HITLER_SHOOT2,10,NULL,(statefunc)T_Shoot,s_hitlerchase1},	// s_hitlershoot6
	{false,SPR_BJ_W1,12,(statefunc)T_BJRun,NULL,s_bjrun1s},	// s_bjrun1
	{false,SPR_BJ_W1,3,NULL,NULL,s_bjrun2},	// s_bjrun1s
	{false,SPR_BJ_W2,8,(statefunc)T_BJRun,NULL,s_bjrun3},	// s_bjrun2
	{false,SPR_BJ_W3,12,(statefunc)T_BJRun,NULL,s_bjrun3s},	// s_bjrun3
	{false,SPR_BJ_W3,3,NULL,NULL,s_bjrun4},	// s_bjrun3s
	{false,SPR_BJ_W4,8,(statefunc)T_BJRun,NULL,s_bjrun1},	// s_bjrun4
	{false,SPR_BJ_JUMP1,14,(statefunc)T_BJJump,NULL,s_bjjump2},	// s_bjjump1
	{false,SPR_BJ_JUMP2,14,(statefunc)T_BJJump,(statefunc)T_BJYell,s_bjjump3},	// s_bjjump2
	{false,SPR_BJ_JUMP3,14,(statefunc)T_BJJump,NULL,s_bjjump4},	// s_bjjump3
	{false,SPR_BJ_JUMP4,300,NULL,(statefunc)T_BJDone,s_bjjump4},	// s_bjjump4
	{false,0,0,NULL,NULL,s_null},	// s_deathcam
#endif
};
