// Copyright (C) 1999-2000 Id Software, Inc.
//

/*****************************************************************************
 * name:		ai_main.c
 *
 * desc:		Quake3 bot AI
 *
 * $Archive: /MissionPack/code/game/ai_main.c $
 * $Author: Mrelusive $ 
 * $Revision: 35 $
 * $Modtime: 6/06/01 1:11p $
 * $Date: 6/06/01 12:06p $
 *
 *****************************************************************************/


#include "g_local.h"
#include "q_shared.h"
#include "botlib.h"		//bot lib interface
#include "be_aas.h"
#include "be_ea.h"
#include "be_ai_char.h"
#include "be_ai_chat.h"
#include "be_ai_gen.h"
#include "be_ai_goal.h"
#include "be_ai_move.h"
#include "be_ai_weap.h"
//
#include "ai_main.h"
#include "w_saber.h"
//
#include "chars.h"
#include "inv.h"
#include "syn.h"

/*
#define BOT_CTF_DEBUG	1
*/

#define MAX_PATH		144

#define BOT_THINK_TIME	0

//bot states
bot_state_t	*botstates[MAX_CLIENTS];
//number of bots
int numbots;
//floating point time
float floattime;
//time to do a regular update
float regularupdate_time;
//

//for saga:
extern int rebel_attackers;
extern int imperial_attackers;

boteventtracker_t gBotEventTracker[MAX_CLIENTS];

//rww - new bot cvars..
vmCvar_t bot_forcepowers;
vmCvar_t bot_forgimmick;
vmCvar_t bot_honorableduelacceptance;
#ifdef _DEBUG
vmCvar_t bot_nogoals;
vmCvar_t bot_debugmessages;
#endif

vmCvar_t bot_attachments;
vmCvar_t bot_camp;

vmCvar_t bot_wp_info;
vmCvar_t bot_wp_edit;
vmCvar_t bot_wp_clearweight;
vmCvar_t bot_wp_distconnect;
vmCvar_t bot_wp_visconnect;
//end rww

wpobject_t *flagRed;
wpobject_t *oFlagRed;
wpobject_t *flagBlue;
wpobject_t *oFlagBlue;

gentity_t *eFlagRed;
gentity_t *droppedRedFlag;
gentity_t *eFlagBlue;
gentity_t *droppedBlueFlag;

// PowTecH
#define IS_VALID_ENEMY(enemy_ptr) \
    ((enemy_ptr) && \
     (enemy_ptr)->client && \
     (enemy_ptr)->client->pers.connected == CON_CONNECTED && \
     (enemy_ptr)->client->sess.sessionTeam != TEAM_SPECTATOR)

#define ASTAR_INFINITE_COST 999999.0f
#define BOT_WAYPOINT_TRAVEL_TIMEOUT 1000

typedef struct astar_node_data_s {
	int parentWaypointIndex;    // Index of the waypoint this node came from
	float gCost;                // Cost from start to this waypoint
	float hCost;                // Heuristic cost from this waypoint to goal
	float fCost;                // gCost + hCost
	qboolean inOpenSet;         // True if this waypoint is in the open set
	qboolean inClosedSet;       // True if this waypoint is in the closed set
} astar_node_data_t;


// This array will hold temporary A* data for each waypoint during a pathfind.
// Ensure MAX_WAYPOINTS is correctly defined based on your map's capabilities.
static astar_node_data_t astar_node_workspace[MAX_WPARRAY_SIZE]; // Corrected variable name

// Open set (list of waypoint indices to be evaluated)
static int openSet[MAX_WPARRAY_SIZE];
static int openSetCount;
//

char *ctfStateNames[] = {
	"CTFSTATE_NONE",
	"CTFSTATE_ATTACKER",
	"CTFSTATE_DEFENDER",
	"CTFSTATE_RETRIEVAL",
	"CTFSTATE_GUARDCARRIER",
	"CTFSTATE_GETFLAGHOME",
	"CTFSTATE_MAXCTFSTATES"
};

char *ctfStateDescriptions[] = {
	"I'm not occupied",
	"I'm attacking the enemy's base",
	"I'm defending our base",
	"I'm getting our flag back",
	"I'm escorting our flag carrier",
	"I've got the enemy's flag"
};

char *teamplayStateDescriptions[] = {
	"I'm not occupied",
	"I'm following my squad commander",
	"I'm assisting my commanding",
	"I'm attempting to regroup and form a new squad"
};

//
static void StandardBotAI(bot_state_t* bs, float thinktime);
//

void BotStraightTPOrderCheck(gentity_t *ent, int ordernum, bot_state_t *bs)
{
	switch (ordernum)
	{
	case 0:
		if (bs->squadLeader == ent)
		{
			bs->teamplayState = 0;
			bs->squadLeader = NULL;
		}
		break;
	case TEAMPLAYSTATE_FOLLOWING:
		bs->teamplayState = ordernum;
		bs->isSquadLeader = 0;
		bs->squadLeader = ent;
		bs->wpDestSwitchTime = 0;
		break;
	case TEAMPLAYSTATE_ASSISTING:
		bs->teamplayState = ordernum;
		bs->isSquadLeader = 0;
		bs->squadLeader = ent;
		bs->wpDestSwitchTime = 0;
		break;
	default:
		bs->teamplayState = ordernum;
		break;
	}
}

void BotSelectWeapon(int client, int weapon)
{
	if (weapon <= WP_NONE)
	{
		return;
	}
	trap_EA_SelectWeapon(client, weapon);
}

void BotReportStatus(bot_state_t *bs)
{
	if (g_gametype.integer == GT_TEAM)
	{
		trap_EA_SayTeam(bs->client, teamplayStateDescriptions[bs->teamplayState]);
	}
	else if (g_gametype.integer == GT_CTF || g_gametype.integer == GT_CTY)
	{
		trap_EA_SayTeam(bs->client, ctfStateDescriptions[bs->ctfState]);
	}
}

void BotOrder(gentity_t *ent, int clientnum, int ordernum)
{
	int stateMin = 0;
	int stateMax = 0;
	int i = 0;

	if (!ent || !ent->client || !ent->client->sess.teamLeader)
	{
		return;
	}

	if (clientnum != -1 && !botstates[clientnum])
	{
		return;
	}

	if (clientnum != -1 && !OnSameTeam(ent, &g_entities[clientnum]))
	{
		return;
	}

	if (g_gametype.integer != GT_CTF && g_gametype.integer != GT_CTY && g_gametype.integer != GT_SAGA &&
		g_gametype.integer != GT_TEAM)
	{
		return;
	}

	if (g_gametype.integer == GT_CTF || g_gametype.integer == GT_CTY)
	{
		stateMin = CTFSTATE_NONE;
		stateMax = CTFSTATE_MAXCTFSTATES;
	}
	else if (g_gametype.integer == GT_TEAM)
	{
		stateMin = TEAMPLAYSTATE_NONE;
		stateMax = TEAMPLAYSTATE_MAXTPSTATES;
	}

	if ((ordernum < stateMin && ordernum != -1) || ordernum >= stateMax)
	{
		return;
	}

	if (clientnum != -1)
	{
		if (ordernum == -1)
		{
			BotReportStatus(botstates[clientnum]);
		}
		else
		{
			BotStraightTPOrderCheck(ent, ordernum, botstates[clientnum]);
			botstates[clientnum]->state_Forced = ordernum;
			botstates[clientnum]->chatObject = ent;
			botstates[clientnum]->chatAltObject = NULL;
			if (BotDoChat(botstates[clientnum], "OrderAccepted", 1))
			{
				botstates[clientnum]->chatTeam = 1;
			}
		}
	}
	else
	{
		while (i < MAX_CLIENTS)
		{
			if (botstates[i] && OnSameTeam(ent, &g_entities[i]))
			{
				if (ordernum == -1)
				{
					BotReportStatus(botstates[i]);
				}
				else
				{
					BotStraightTPOrderCheck(ent, ordernum, botstates[i]);
					botstates[i]->state_Forced = ordernum;
					botstates[i]->chatObject = ent;
					botstates[i]->chatAltObject = NULL;
					if (BotDoChat(botstates[i], "OrderAccepted", 0))
					{
						botstates[i]->chatTeam = 1;
					}
				}
			}

			i++;
		}
	}
}

int BotMindTricked(int botClient, int enemyClient)
{
	forcedata_t *fd;

	if (!g_entities[enemyClient].client)
	{
		return 0;
	}
	
	fd = &g_entities[enemyClient].client->ps.fd;

	if (!fd)
	{
		return 0;
	}

	if (botClient > 47)
	{
		if (fd->forceMindtrickTargetIndex4 & (1 << (botClient-48)))
		{
			return 1;
		}
	}
	else if (botClient > 31)
	{
		if (fd->forceMindtrickTargetIndex3 & (1 << (botClient-32)))
		{
			return 1;
		}
	}
	else if (botClient > 15)
	{
		if (fd->forceMindtrickTargetIndex2 & (1 << (botClient-16)))
		{
			return 1;
		}
	}
	else
	{
		if (fd->forceMindtrickTargetIndex & (1 << botClient))
		{
			return 1;
		}
	}

	return 0;
}

int BotGetWeaponRange(bot_state_t *bs);
int PassLovedOneCheck(bot_state_t *bs, gentity_t *ent);

void ExitLevel( void );

void QDECL BotAI_Print(int type, char *fmt, ...) { return; }

qboolean WP_ForcePowerUsable( gentity_t *self, forcePowers_t forcePower );

int IsTeamplay(void)
{
	if ( g_gametype.integer < GT_TEAM )
	{
		return 0;
	}

	return 1;
}

/*
==================
BotAI_GetClientState
==================
*/
int BotAI_GetClientState( int clientNum, playerState_t *state ) {
	gentity_t	*ent;

	ent = &g_entities[clientNum];
	if ( !ent->inuse ) {
		return qfalse;
	}
	if ( !ent->client ) {
		return qfalse;
	}

	memcpy( state, &ent->client->ps, sizeof(playerState_t) );
	return qtrue;
}

/*
==================
BotAI_GetEntityState
==================
*/
int BotAI_GetEntityState( int entityNum, entityState_t *state ) {
	gentity_t	*ent;

	ent = &g_entities[entityNum];
	memset( state, 0, sizeof(entityState_t) );
	if (!ent->inuse) return qfalse;
	if (!ent->r.linked) return qfalse;
	if (ent->r.svFlags & SVF_NOCLIENT) return qfalse;
	memcpy( state, &ent->s, sizeof(entityState_t) );
	return qtrue;
}

/*
==================
BotAI_GetSnapshotEntity
==================
*/
int BotAI_GetSnapshotEntity( int clientNum, int sequence, entityState_t *state ) {
	int		entNum;

	entNum = trap_BotGetSnapshotEntity( clientNum, sequence );
	if ( entNum == -1 ) {
		memset(state, 0, sizeof(entityState_t));
		return -1;
	}

	BotAI_GetEntityState( entNum, state );

	return sequence + 1;
}

/*
==============
BotEntityInfo
==============
*/
void BotEntityInfo(int entnum, aas_entityinfo_t *info) {
	trap_AAS_EntityInfo(entnum, info);
}

/*
==============
NumBots
==============
*/
int NumBots(void) {
	return numbots;
}

/*
==============
AngleDifference
==============
*/
float AngleDifference(float ang1, float ang2) {
	float diff;

	diff = ang1 - ang2;
	if (ang1 > ang2) {
		if (diff > 180.0) diff -= 360.0;
	}
	else {
		if (diff < -180.0) diff += 360.0;
	}
	return diff;
}

/*
==============
BotChangeViewAngle
==============
*/
float BotChangeViewAngle(float angle, float ideal_angle, float speed) {
	float move;

	angle = AngleMod(angle);
	ideal_angle = AngleMod(ideal_angle);
	if (angle == ideal_angle) return angle;
	move = ideal_angle - angle;
	if (ideal_angle > angle) {
		if (move > 180.0) move -= 360.0;
	}
	else {
		if (move < -180.0) move += 360.0;
	}
	if (move > 0) {
		if (move > speed) move = speed;
	}
	else {
		if (move < -speed) move = -speed;
	}
	return AngleMod(angle + move);
}

/*
==============
BotChangeViewAngles
==============
*/
void BotChangeViewAngles(bot_state_t *bs, float thinktime) {
	float diff, factor, maxchange, anglespeed, disired_speed;
	int i;

	if (bs->ideal_viewangles[PITCH] > 180) bs->ideal_viewangles[PITCH] -= 360;
	
	if (bs->currentEnemy && bs->frame_Enemy_Vis)
	{
		factor = bs->skills.turnspeed_combat*bs->settings.skill;
	}
	else
	{
		factor = bs->skills.turnspeed;
	}

	if (factor > 1)
	{
		factor = 1;
	}
	if (factor < 0.001)
	{
		factor = 0.001f;
	}

	maxchange = bs->skills.maxturn;

	//if (maxchange < 240) maxchange = 240;
	maxchange *= thinktime;
	for (i = 0; i < 2; i++) {
		bs->viewangles[i] = AngleMod(bs->viewangles[i]);
		bs->ideal_viewangles[i] = AngleMod(bs->ideal_viewangles[i]);
		diff = AngleDifference(bs->viewangles[i], bs->ideal_viewangles[i]);
		disired_speed = diff * factor;
		bs->viewanglespeed[i] += (bs->viewanglespeed[i] - disired_speed);
		if (bs->viewanglespeed[i] > 180) bs->viewanglespeed[i] = maxchange;
		if (bs->viewanglespeed[i] < -180) bs->viewanglespeed[i] = -maxchange;
		anglespeed = bs->viewanglespeed[i];
		if (anglespeed > maxchange) anglespeed = maxchange;
		if (anglespeed < -maxchange) anglespeed = -maxchange;
		bs->viewangles[i] += anglespeed;
		bs->viewangles[i] = AngleMod(bs->viewangles[i]);
		bs->viewanglespeed[i] *= 0.45 * (1 - factor);
	}
	if (bs->viewangles[PITCH] > 180) bs->viewangles[PITCH] -= 360;
	trap_EA_View(bs->client, bs->viewangles);
}

/*
==============
BotInputToUserCommand
==============
*/
void BotInputToUserCommand(bot_input_t *bi, usercmd_t *ucmd, int delta_angles[3], int time, int useTime) {
	vec3_t angles, forward, right;
	short temp;
	int j;

	//clear the whole structure
	memset(ucmd, 0, sizeof(usercmd_t));
	//
	//Com_Printf("dir = %f %f %f speed = %f\n", bi->dir[0], bi->dir[1], bi->dir[2], bi->speed);
	//the duration for the user command in milli seconds
	ucmd->serverTime = time;
	//
	if (bi->actionflags & ACTION_DELAYEDJUMP) {
		bi->actionflags |= ACTION_JUMP;
		bi->actionflags &= ~ACTION_DELAYEDJUMP;
	}
	//set the buttons
	if (bi->actionflags & ACTION_RESPAWN) ucmd->buttons = BUTTON_ATTACK;
	if (bi->actionflags & ACTION_ATTACK) ucmd->buttons |= BUTTON_ATTACK;
	if (bi->actionflags & ACTION_ALT_ATTACK) ucmd->buttons |= BUTTON_ALT_ATTACK;
//	if (bi->actionflags & ACTION_TALK) ucmd->buttons |= BUTTON_TALK;
	if (bi->actionflags & ACTION_GESTURE) ucmd->buttons |= BUTTON_GESTURE;
	if (bi->actionflags & ACTION_USE) ucmd->buttons |= BUTTON_USE_HOLDABLE;
	if (bi->actionflags & ACTION_WALK) ucmd->buttons |= BUTTON_WALKING;

	if (bi->actionflags & ACTION_FORCEPOWER) ucmd->buttons |= BUTTON_FORCEPOWER;

	if (useTime < level.time && Q_irand(1, 10) < 5)
	{ //for now just hit use randomly in case there's something useable around
		ucmd->buttons |= BUTTON_USE;
	}
#if 0
// Here's an interesting bit.  The bots in TA used buttons to do additional gestures.
// I ripped them out because I didn't want too many buttons given the fact that I was already adding some for JK2.
// We can always add some back in if we want though.
	if (bi->actionflags & ACTION_AFFIRMATIVE) ucmd->buttons |= BUTTON_AFFIRMATIVE;
	if (bi->actionflags & ACTION_NEGATIVE) ucmd->buttons |= BUTTON_NEGATIVE;
	if (bi->actionflags & ACTION_GETFLAG) ucmd->buttons |= BUTTON_GETFLAG;
	if (bi->actionflags & ACTION_GUARDBASE) ucmd->buttons |= BUTTON_GUARDBASE;
	if (bi->actionflags & ACTION_PATROL) ucmd->buttons |= BUTTON_PATROL;
	if (bi->actionflags & ACTION_FOLLOWME) ucmd->buttons |= BUTTON_FOLLOWME;
#endif //0

	if (bi->weapon == WP_NONE)
	{
#ifdef _DEBUG
//		Com_Printf("WARNING: Bot tried to use WP_NONE!\n");
#endif
		bi->weapon = WP_BRYAR_PISTOL;
	}

	//
	ucmd->weapon = bi->weapon;
	//set the view angles
	//NOTE: the ucmd->angles are the angles WITHOUT the delta angles
	ucmd->angles[PITCH] = ANGLE2SHORT(bi->viewangles[PITCH]);
	ucmd->angles[YAW] = ANGLE2SHORT(bi->viewangles[YAW]);
	ucmd->angles[ROLL] = ANGLE2SHORT(bi->viewangles[ROLL]);
	//subtract the delta angles
	for (j = 0; j < 3; j++) {
		temp = ucmd->angles[j] - delta_angles[j];
		ucmd->angles[j] = temp;
	}
	//NOTE: movement is relative to the REAL view angles
	//get the horizontal forward and right vector
	//get the pitch in the range [-180, 180]
	if (bi->dir[2]) angles[PITCH] = bi->viewangles[PITCH];
	else angles[PITCH] = 0;
	angles[YAW] = bi->viewangles[YAW];
	angles[ROLL] = 0;
	AngleVectors(angles, forward, right, NULL);
	//bot input speed is in the range [0, 400]
	bi->speed = bi->speed * 127 / 400;
	//set the view independent movement
	ucmd->forwardmove = DotProduct(forward, bi->dir) * bi->speed;
	ucmd->rightmove = DotProduct(right, bi->dir) * bi->speed;
	// This was probably a bug in original code. Uncommenting
	// fabs(... line makes bots more eager to jump, kick and roll.
	ucmd->upmove = 0; // fabs(forward[2]) * bi->dir[2] * bi->speed;
	//normal keyboard movement
	if (bi->actionflags & ACTION_MOVEFORWARD) ucmd->forwardmove += 127;
	if (bi->actionflags & ACTION_MOVEBACK) ucmd->forwardmove -= 127;
	if (bi->actionflags & ACTION_MOVELEFT) ucmd->rightmove -= 127;
	if (bi->actionflags & ACTION_MOVERIGHT) ucmd->rightmove += 127;
	//jump/moveup
	if (bi->actionflags & ACTION_JUMP) ucmd->upmove += 127;
	//crouch/movedown
	if (bi->actionflags & ACTION_CROUCH) ucmd->upmove -= 127;
	//
	//Com_Printf("forward = %d right = %d up = %d\n", ucmd.forwardmove, ucmd.rightmove, ucmd.upmove);
	//Com_Printf("ucmd->serverTime = %d\n", ucmd->serverTime);
}

/*
==============
BotUpdateInput
==============
*/
void BotUpdateInput(bot_state_t *bs, int time, int elapsed_time) {
	bot_input_t bi;
	int j;

	//add the delta angles to the bot's current view angles
	for (j = 0; j < 3; j++) {
		bs->viewangles[j] = AngleMod(bs->viewangles[j] + SHORT2ANGLE(bs->cur_ps.delta_angles[j]));
	}
	//change the bot view angles
	BotChangeViewAngles(bs, (float) elapsed_time / 1000);
	//retrieve the bot input
	trap_EA_GetInput(bs->client, (float) time / 1000, &bi);
	//respawn hack
	if (bi.actionflags & ACTION_RESPAWN) {
		if (bs->lastucmd.buttons & BUTTON_ATTACK) bi.actionflags &= ~(ACTION_RESPAWN|ACTION_ATTACK);
	}
	//convert the bot input to a usercmd
	BotInputToUserCommand(&bi, &bs->lastucmd, bs->cur_ps.delta_angles, time, bs->noUseTime);
	//subtract the delta angles
	for (j = 0; j < 3; j++) {
		bs->viewangles[j] = AngleMod(bs->viewangles[j] - SHORT2ANGLE(bs->cur_ps.delta_angles[j]));
	}
}

/*
==============
BotAIRegularUpdate
==============
*/
void BotAIRegularUpdate(void) {
	if (regularupdate_time < FloatTime()) {
		trap_BotUpdateEntityItems();
		regularupdate_time = FloatTime() + 0.3;
	}
}

/*
==============
RemoveColorEscapeSequences
==============
*/
void RemoveColorEscapeSequences( char *text ) {
	int i, l;

	l = 0;
	for ( i = 0; text[i]; i++ ) {
		if ((jk2gameplay == VERSION_1_02 ? Q_IsColorString_1_02(&text[i]) : Q_IsColorString(&text[i]))) {
			i++;
			continue;
		}
		if (text[i] > 0x7E)
			continue;
		text[l++] = text[i];
	}
	text[l] = '\0';
}


/*
==============
BotAI
==============
*/
int BotAI(int client, float thinktime) {
	bot_state_t *bs;
	char buf[1024], *args;
	int j;
#ifdef _DEBUG
	int start = 0;
	int end = 0;
#endif

	trap_EA_ResetInput(client);
	//
	bs = botstates[client];
	if (!bs || !bs->inuse) {
		BotAI_Print(PRT_FATAL, "BotAI: client %d is not setup\n", client);
		return qfalse;
	}

	//retrieve the current client state
	BotAI_GetClientState( client, &bs->cur_ps );

	//retrieve any waiting server commands
	while( trap_BotGetServerCommand(client, buf, sizeof(buf)) ) {
		//have buf point to the command and args to the command arguments
		args = strchr( buf, ' ');
		if (!args) continue;
		*args++ = '\0';

		//remove color espace sequences from the arguments
		RemoveColorEscapeSequences( args );

		if (!Q_stricmp(buf, "cp "))
			{ /*CenterPrintf*/ }
		else if (!Q_stricmp(buf, "cs"))
			{ /*ConfigStringModified*/ }
		else if (!Q_stricmp(buf, "scores"))
			{ /*FIXME: parse scores?*/ }
		else if (!Q_stricmp(buf, "clientLevelShot"))
			{ /*ignore*/ }
	}
	//add the delta angles to the bot's current view angles
	for (j = 0; j < 3; j++) {
		bs->viewangles[j] = AngleMod(bs->viewangles[j] + SHORT2ANGLE(bs->cur_ps.delta_angles[j]));
	}
	//increase the local time of the bot
	bs->ltime += thinktime;
	//
	bs->thinktime = thinktime;
	//origin of the bot
	VectorCopy(bs->cur_ps.origin, bs->origin);
	//eye coordinates of the bot
	VectorCopy(bs->cur_ps.origin, bs->eye);
	bs->eye[2] += bs->cur_ps.viewheight;
	//get the area the bot is in

#ifdef _DEBUG
	start = trap_Milliseconds();
#endif
	StandardBotAI(bs, thinktime);
#ifdef _DEBUG
	end = trap_Milliseconds();

	trap_Cvar_Update(&bot_debugmessages);

	if (bot_debugmessages.integer)
	{
		Com_Printf("Single AI frametime: %i\n", (end - start));
	}
#endif

	//subtract the delta angles
	for (j = 0; j < 3; j++) {
		bs->viewangles[j] = AngleMod(bs->viewangles[j] - SHORT2ANGLE(bs->cur_ps.delta_angles[j]));
	}
	//everything was ok
	return qtrue;
}

/*
==================
BotScheduleBotThink
==================
*/
void BotScheduleBotThink(void) {
	int i, botnum;

	botnum = 0;

	for( i = 0; i < MAX_CLIENTS; i++ ) {
		if( !botstates[i] || !botstates[i]->inuse ) {
			continue;
		}
		//initialize the bot think residual time
		botstates[i]->botthink_residual = BOT_THINK_TIME * botnum / numbots;
		botnum++;
	}
}

int PlayersInGame(void)
{
	int i = 0;
	gentity_t *ent;
	int pl = 0;

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client && ent->client->pers.connected == CON_CONNECTED)
		{
			pl++;
		}

		i++;
	}

	return pl;
}

/*
==============
BotAISetupClient
==============
*/
int BotAISetupClient(int client, struct bot_settings_s *settings, qboolean restart) {
	bot_state_t *bs;

	if (!botstates[client]) botstates[client] = B_Alloc(sizeof(bot_state_t)); //G_Alloc(sizeof(bot_state_t));
																			  //rww - G_Alloc bad! B_Alloc good.

	memset(botstates[client], 0, sizeof(bot_state_t));

	bs = botstates[client];

	if (bs && bs->inuse) {
		BotAI_Print(PRT_FATAL, "BotAISetupClient: client %d already setup\n", client);
		return qfalse;
	}

	if ( !bs )
	{
		BotAI_Print(PRT_FATAL, "BotAISetupClient: client %d has no bot_state\n", client);
		return qfalse;
	}

	memcpy(&bs->settings, settings, sizeof(bot_settings_t));

	bs->client = client; //need to know the client number before doing personality stuff

	//initialize weapon weight defaults..
	bs->botWeaponWeights[WP_NONE] = 0;
	bs->botWeaponWeights[WP_STUN_BATON] = 1;
	bs->botWeaponWeights[WP_SABER] = 10;
	bs->botWeaponWeights[WP_BRYAR_PISTOL] = 11;
	bs->botWeaponWeights[WP_BLASTER] = 12;
	bs->botWeaponWeights[WP_DISRUPTOR] = 13;
	bs->botWeaponWeights[WP_BOWCASTER] = 14;
	bs->botWeaponWeights[WP_REPEATER] = 15;
	bs->botWeaponWeights[WP_DEMP2] = 16;
	bs->botWeaponWeights[WP_FLECHETTE] = 17;
	bs->botWeaponWeights[WP_ROCKET_LAUNCHER] = 18;
	bs->botWeaponWeights[WP_THERMAL] = 14;
	bs->botWeaponWeights[WP_TRIP_MINE] = 0;
	bs->botWeaponWeights[WP_DET_PACK] = 0;

	BotUtilizePersonality(bs);

	if (g_gametype.integer == GT_TOURNAMENT)
	{
		bs->botWeaponWeights[WP_SABER] = 13;
	}

	//allocate a goal state
	bs->gs = trap_BotAllocGoalState(client);

	//allocate a weapon state
	bs->ws = trap_BotAllocWeaponState();

	bs->inuse = qtrue;
	bs->entitynum = client;
	bs->setupcount = 4;
	bs->entergame_time = FloatTime();
	bs->ms = trap_BotAllocMoveState();
	numbots++;

	//NOTE: reschedule the bot thinking
	BotScheduleBotThink();

	if (PlayersInGame())
	{ //don't talk to yourself
		BotDoChat(bs, "GeneralGreetings", 0);
	}

	return qtrue;
}

/*
==============
BotAIShutdownClient
==============
*/
int BotAIShutdownClient(int client, qboolean restart) {
	bot_state_t *bs;

	bs = botstates[client];
	if (!bs || !bs->inuse) {
		//BotAI_Print(PRT_ERROR, "BotAIShutdownClient: client %d already shutdown\n", client);
		return qfalse;
	}

	trap_BotFreeMoveState(bs->ms);
	//free the goal state`			
	trap_BotFreeGoalState(bs->gs);
	//free the weapon weights
	trap_BotFreeWeaponState(bs->ws);
	//
	//clear the bot state
	memset(bs, 0, sizeof(bot_state_t));
	//set the inuse flag to qfalse
	bs->inuse = qfalse;
	//there's one bot less
	numbots--;
	//everything went ok
	return qtrue;
}

/*
==============
BotResetState

called when a bot enters the intermission or observer mode and
when the level is changed
==============
*/
void BotResetState(bot_state_t *bs) {
	int client, entitynum, inuse;
	int movestate, goalstate, weaponstate;
	bot_settings_t settings;
	playerState_t ps;							//current player state
	float entergame_time;

	//save some things that should not be reset here
	memcpy(&settings, &bs->settings, sizeof(bot_settings_t));
	memcpy(&ps, &bs->cur_ps, sizeof(playerState_t));
	inuse = bs->inuse;
	client = bs->client;
	entitynum = bs->entitynum;
	movestate = bs->ms;
	goalstate = bs->gs;
	weaponstate = bs->ws;
	entergame_time = bs->entergame_time;
	//reset the whole state
	memset(bs, 0, sizeof(bot_state_t));
	//copy back some state stuff that should not be reset
	bs->ms = movestate;
	bs->gs = goalstate;
	bs->ws = weaponstate;
	memcpy(&bs->cur_ps, &ps, sizeof(playerState_t));
	memcpy(&bs->settings, &settings, sizeof(bot_settings_t));
	bs->inuse = inuse;
	bs->client = client;
	bs->entitynum = entitynum;
	bs->entergame_time = entergame_time;
	//reset several states
	if (bs->ms) trap_BotResetMoveState(bs->ms);
	if (bs->gs) trap_BotResetGoalState(bs->gs);
	if (bs->ws) trap_BotResetWeaponState(bs->ws);
	if (bs->gs) trap_BotResetAvoidGoals(bs->gs);
	if (bs->ms) trap_BotResetAvoidReach(bs->ms);
}

/*
==============
BotAILoadMap
==============
*/
int BotAILoadMap( int restart ) {
	int			i;

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (botstates[i] && botstates[i]->inuse) {
			BotResetState( botstates[i] );
			botstates[i]->setupcount = 4;
		}
	}

	return qtrue;
}

//rww - bot ai
int OrgVisible(vec3_t org1, vec3_t org2, int ignore)
{
	trace_t tr;

	trap_Trace(&tr, org1, NULL, NULL, org2, ignore, MASK_SOLID);

	if (tr.fraction == 1)
	{
		return 1;
	}

	return 0;
}

int WPOrgVisible(gentity_t *bot, vec3_t org1, vec3_t org2, int ignore)
{
	trace_t tr;
	gentity_t *ownent;

	trap_Trace(&tr, org1, NULL, NULL, org2, ignore, MASK_SOLID);

	if (tr.fraction == 1)
	{
		trap_Trace(&tr, org1, NULL, NULL, org2, ignore, MASK_PLAYERSOLID);

		if (tr.fraction != 1 && tr.entityNum != ENTITYNUM_NONE && g_entities[tr.entityNum].s.eType == ET_SPECIAL)
		{
			if (g_entities[tr.entityNum].parent && g_entities[tr.entityNum].parent->client)
			{
				ownent = g_entities[tr.entityNum].parent;

				if (OnSameTeam(bot, ownent) || bot->s.number == ownent->s.number)
				{
					return 1;
				}
			}
			return 2;
		}

		return 1;
	}

	return 0;
}

int OrgVisibleBox(vec3_t org1, vec3_t mins, vec3_t maxs, vec3_t org2, int ignore)
{
	trace_t tr;

	trap_Trace(&tr, org1, mins, maxs, org2, ignore, MASK_SOLID);

	if (tr.fraction == 1 && !tr.startsolid && !tr.allsolid)
	{
		return 1;
	}

	return 0;
}

int CheckForFunc(vec3_t org, int ignore)
{
	gentity_t *fent;
	vec3_t under;
	trace_t tr;

	VectorCopy(org, under);

	under[2] -= 64;

	trap_Trace(&tr, org, NULL, NULL, under, ignore, MASK_SOLID);

	if (tr.fraction == 1)
	{
		return 0;
	}

	fent = &g_entities[tr.entityNum];

	if (!fent)
	{
		return 0;
	}

	if (strstr(fent->classname, "func_"))
	{
		return 1; //there's a func brush here
	}

	return 0;
}

int GetNearestVisibleWP(vec3_t org, int ignore)
{
	int i;
	float bestdist;
	float flLen;
	int bestindex;
	vec3_t a, mins, maxs;

	i = 0;
	bestdist = 800;//99999;
			   //don't trace over 800 units away to avoid GIANT HORRIBLE SPEED HITS ^_^
	bestindex = -1;

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = -1;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 1;

	while (i < gWPNum)
	{
		if (gWPArray[i] && gWPArray[i]->inuse)
		{
			VectorSubtract(org, gWPArray[i]->origin, a);
			flLen = VectorLength(a);

			if (flLen < bestdist && trap_InPVS(org, gWPArray[i]->origin) && OrgVisibleBox(org, mins, maxs, gWPArray[i]->origin, ignore))
			{
				bestdist = flLen;
				bestindex = i;
			}
		}

		i++;
	}

	return bestindex;
}

//wpDirection
//0 == FORWARD
//1 == BACKWARD

int PassWayCheck(bot_state_t *bs, int windex)
{
	if (!gWPArray[windex] || !gWPArray[windex]->inuse)
	{
		return 0;
	}

	if (bs->wpDirection && (gWPArray[windex]->flags & WPFLAG_ONEWAY_FWD))
	{
		return 0;
	}
	else if (!bs->wpDirection && (gWPArray[windex]->flags & WPFLAG_ONEWAY_BACK))
	{
		return 0;
	}

	if (bs->wpCurrent && gWPArray[windex]->forceJumpTo &&
		gWPArray[windex]->origin[2] > (bs->wpCurrent->origin[2]+64) &&
		bs->cur_ps.fd.forcePowerLevel[FP_LEVITATION] < gWPArray[windex]->forceJumpTo)
	{
		return 0;
	}

	return 1;
}

float TotalTrailDistance(int start, int end, bot_state_t *bs)
{
	int beginat;
	int endat;
	float distancetotal;
	float gdif = 0;

	distancetotal = 0;

	if (start > end)
	{
		beginat = end;
		endat = start;
	}
	else
	{
		beginat = start;
		endat = end;
	}

	while (beginat < endat)
	{
		if (beginat >= gWPNum || !gWPArray[beginat] || !gWPArray[beginat]->inuse)
		{
			return -1; //error
		}

		if ((end > start && gWPArray[beginat]->flags & WPFLAG_ONEWAY_BACK) ||
			(start > end && gWPArray[beginat]->flags & WPFLAG_ONEWAY_FWD))
		{
			return -1;
		}
	
		if (gWPArray[beginat]->forceJumpTo)
		{
			if (gWPArray[beginat-1] && gWPArray[beginat-1]->origin[2]+64 < gWPArray[beginat]->origin[2])
			{
				gdif = gWPArray[beginat]->origin[2] - gWPArray[beginat-1]->origin[2];
			}

			if (gdif)
			{
			//	if (bs && bs->cur_ps.fd.forcePowerLevel[FP_LEVITATION] < gWPArray[beginat]->forceJumpTo)
			//	{
			//		return -1;
			//	}
			}
		}
		
	/*	if (bs->wpCurrent && gWPArray[windex]->forceJumpTo &&
			gWPArray[windex]->origin[2] > (bs->wpCurrent->origin[2]+64) &&
			bs->cur_ps.fd.forcePowerLevel[FP_LEVITATION] < gWPArray[windex]->forceJumpTo)
		{
			return -1;
		}*/

		distancetotal += gWPArray[beginat]->disttonext;

		beginat++;
	}

	return distancetotal;
}

void CheckForShorterRoutes(bot_state_t *bs, int newwpindex)
{
	float bestlen;
	float checklen;
	int bestindex;
	int i;
	int fj;

	i = 0;
	fj = 0;

	if (!bs->wpDestination)
	{
		return;
	}

	if (newwpindex < bs->wpDestination->index)
	{
		bs->wpDirection = 0;
	}
	else if (newwpindex > bs->wpDestination->index)
	{
		bs->wpDirection = 1;
	}

	if (bs->wpSwitchTime > level.time)
	{
		return;
	}

	if (!gWPArray[newwpindex]->neighbornum)
	{
		return;
	}

	bestindex = newwpindex;
	bestlen = TotalTrailDistance(newwpindex, bs->wpDestination->index, bs);

	while (i < gWPArray[newwpindex]->neighbornum)
	{
		checklen = TotalTrailDistance(gWPArray[newwpindex]->neighbors[i].num, bs->wpDestination->index, bs);

		if (checklen < bestlen-64 || bestlen == -1)
		{
			if (bs->cur_ps.fd.forcePowerLevel[FP_LEVITATION] >= gWPArray[newwpindex]->neighbors[i].forceJumpTo)
			{
				bestlen = checklen;
				bestindex = gWPArray[newwpindex]->neighbors[i].num;

				if (gWPArray[newwpindex]->neighbors[i].forceJumpTo)
				{
					fj = gWPArray[newwpindex]->neighbors[i].forceJumpTo;
				}
				else
				{
					fj = 0;
				}
			}
		}

		i++;
	}

	if (bestindex != newwpindex && bestindex != -1)
	{
		bs->wpCurrent = gWPArray[bestindex];
		bs->wpSwitchTime = level.time + 3000;

		if (fj)
		{
#ifndef FORCEJUMP_INSTANTMETHOD
			bs->forceJumpChargeTime = level.time + 1000;
			bs->beStill = level.time + 1000;
			bs->forceJumping = bs->forceJumpChargeTime;
#else
			bs->beStill = level.time + 500;
			bs->jumpTime = level.time + fj*1200;
			bs->jDelay = level.time + 200;
			bs->forceJumping = bs->jumpTime;
#endif
		}
	}
}

void WPConstantRoutine(bot_state_t *bs)
{
	if (!bs->wpCurrent)
	{
		return;
	}

	if (bs->wpCurrent->flags & WPFLAG_DUCK)
	{
		bs->duckTime = level.time + 100;
	}

#ifndef FORCEJUMP_INSTANTMETHOD
	if (bs->wpCurrent->flags & WPFLAG_JUMP)
	{
		float heightDif = (bs->wpCurrent->origin[2] - bs->origin[2]+16);

		if (bs->origin[2]+16 >= bs->wpCurrent->origin[2])
		{ //then why exactly would we be force jumping?
			heightDif = 0;
		}

		if (heightDif > 40 && (bs->cur_ps.fd.forcePowersKnown & (1 << FP_LEVITATION)) && (bs->cur_ps.fd.forceJumpCharge < (forceJumpStrength[bs->cur_ps.fd.forcePowerLevel[FP_LEVITATION]]-100) || bs->cur_ps.groundEntityNum == ENTITYNUM_NONE))
		{
			bs->forceJumpChargeTime = level.time + 1000;
			if (bs->cur_ps.groundEntityNum != ENTITYNUM_NONE && bs->jumpPrep < (level.time-300))
			{
				bs->jumpPrep = level.time + 700;
			}
			bs->beStill = level.time + 300;
			bs->jumpTime = 0;

			if (bs->wpSeenTime < (level.time + 600))
			{
				bs->wpSeenTime = level.time + 600;
			}
		}
		else if (heightDif > 64 && !(bs->cur_ps.fd.forcePowersKnown & (1 << FP_LEVITATION)))
		{ //this point needs force jump to reach and we don't have it
			//Kill the current point and turn around
			bs->wpCurrent = NULL;
			if (bs->wpDirection)
			{
				bs->wpDirection = 0;
			}
			else
			{
				bs->wpDirection = 1;
			}

			return;
		}
	}
#endif

	if (bs->wpCurrent->forceJumpTo)
	{
#ifdef FORCEJUMP_INSTANTMETHOD
		if (bs->origin[2]+16 < bs->wpCurrent->origin[2])
		{
			bs->jumpTime = level.time + 100;
		}
#else
		if (bs->cur_ps.fd.forceJumpCharge < (forceJumpStrength[bs->cur_ps.fd.forcePowerLevel[FP_LEVITATION]]-100))
		{
			bs->forceJumpChargeTime = level.time + 200;
		}
#endif
	}
}

qboolean BotCTFGuardDuty(bot_state_t *bs)
{
	if (g_gametype.integer != GT_CTF &&
		g_gametype.integer != GT_CTY)
	{
		return qfalse;
	}

	if (bs->ctfState == CTFSTATE_DEFENDER)
	{
		return qtrue;
	}

	return qfalse;
}

void WPTouchRoutine(bot_state_t *bs)
{
	int lastNum;

	if (!bs->wpCurrent)
	{
		return;
	}

	bs->wpTravelTime = level.time + 10000;

	if (bs->wpCurrent->flags & WPFLAG_NOMOVEFUNC)
	{
		bs->noUseTime = level.time + 4000;
	}

#ifdef FORCEJUMP_INSTANTMETHOD
	if ((bs->wpCurrent->flags & WPFLAG_JUMP) && bs->wpCurrent->forceJumpTo)
	{ //jump if we're flagged to but not if this indicates a force jump point. Force jumping is
	  //handled elsewhere.
		bs->jumpTime = level.time + 100;
	}
#else
	if ((bs->wpCurrent->flags & WPFLAG_JUMP) && !bs->wpCurrent->forceJumpTo)
	{ //jump if we're flagged to but not if this indicates a force jump point. Force jumping is
	  //handled elsewhere.
		bs->jumpTime = level.time + 100;
	}
#endif

	trap_Cvar_Update(&bot_camp);

	if (bs->isCamper && bot_camp.integer && (BotIsAChickenWuss(bs) || BotCTFGuardDuty(bs) || bs->isCamper == 2) && ((bs->wpCurrent->flags & WPFLAG_SNIPEORCAMP) || (bs->wpCurrent->flags & WPFLAG_SNIPEORCAMPSTAND)) &&
		bs->cur_ps.weapon != WP_SABER && bs->cur_ps.weapon != WP_STUN_BATON)
	{ //if we're a camper and a chicken then camp
		if (bs->wpDirection)
		{
			lastNum = bs->wpCurrent->index+1;
		}
		else
		{
			lastNum = bs->wpCurrent->index-1;
		}

		if (gWPArray[lastNum] && gWPArray[lastNum]->inuse && gWPArray[lastNum]->index && bs->isCamping < level.time)
		{
			bs->isCamping = level.time + rand()%15000 + 30000;
			bs->wpCamping = bs->wpCurrent;
			bs->wpCampingTo = gWPArray[lastNum];

			if (bs->wpCurrent->flags & WPFLAG_SNIPEORCAMPSTAND)
			{
				bs->campStanding = qtrue;
			}
			else
			{
				bs->campStanding = qfalse;
			}
		}

	}
	else if ((bs->cur_ps.weapon == WP_SABER || bs->cur_ps.weapon == WP_STUN_BATON) &&
		bs->isCamping > level.time)
	{
		bs->isCamping = 0;
		bs->wpCampingTo = NULL;
		bs->wpCamping = NULL;
	}

	if (bs->wpDestination)
	{
		if (bs->wpCurrent->index == bs->wpDestination->index)
		{
			bs->wpDestination = NULL;

			if (bs->runningLikeASissy)
			{ //this obviously means we're scared and running, so we'll want to keep our navigational priorities less delayed
				bs->destinationGrabTime = level.time + 500;
			}
			else
			{
				bs->destinationGrabTime = level.time + 3500;
			}
		}
		else
		{
			CheckForShorterRoutes(bs, bs->wpCurrent->index);
		}
	}
}
     
#define BOT_STRAFE_AVOIDANCE

#ifdef BOT_STRAFE_AVOIDANCE
#define STRAFEAROUND_RIGHT			1
#define STRAFEAROUND_LEFT			2

int BotTrace_Strafe(bot_state_t *bs, vec3_t traceto)
{
	vec3_t playerMins = {-15, -15, /*DEFAULT_MINS_2*/-8};
	vec3_t playerMaxs = {15, 15, DEFAULT_MAXS_2};
	vec3_t from, to;
	vec3_t dirAng, dirDif;
	vec3_t forward, right;
	trace_t tr;

	if (bs->cur_ps.groundEntityNum == ENTITYNUM_NONE)
	{ //don't do this in the air, it can be.. dangerous.
		return 0;
	}

	VectorSubtract(traceto, bs->origin, dirAng);
	VectorNormalize(dirAng);
	vectoangles(dirAng, dirAng);

	if (AngleDifference(bs->viewangles[YAW], dirAng[YAW]) > 60 ||
		AngleDifference(bs->viewangles[YAW], dirAng[YAW]) < -60)
	{ //If we aren't facing the direction we're going here, then we've got enough excuse to be too stupid to strafe around anyway
		return 0;
	}

	VectorCopy(bs->origin, from);
	VectorCopy(traceto, to);

	VectorSubtract(to, from, dirDif);
	VectorNormalize(dirDif);
	vectoangles(dirDif, dirDif);

	AngleVectors(dirDif, forward, 0, 0);

	to[0] = from[0] + forward[0]*32;
	to[1] = from[1] + forward[1]*32;
	to[2] = from[2] + forward[2]*32;

	trap_Trace(&tr, from, playerMins, playerMaxs, to, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction == 1)
	{
		return 0;
	}

	AngleVectors(dirAng, 0, right, 0);

	from[0] += right[0]*32;
	from[1] += right[1]*32;
	from[2] += right[2]*16;

	to[0] += right[0]*32;
	to[1] += right[1]*32;
	to[2] += right[2]*32;

	trap_Trace(&tr, from, playerMins, playerMaxs, to, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction == 1)
	{
		return STRAFEAROUND_RIGHT;
	}

	from[0] -= right[0]*64;
	from[1] -= right[1]*64;
	from[2] -= right[2]*64;

	to[0] -= right[0]*64;
	to[1] -= right[1]*64;
	to[2] -= right[2]*64;

	trap_Trace(&tr, from, playerMins, playerMaxs, to, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction == 1)
	{
		return STRAFEAROUND_LEFT;
	}

	return 0;
}
#endif

int BotTrace_Jump(bot_state_t *bs, vec3_t traceto)
{
	vec3_t mins, maxs, a, fwd, traceto_mod, tracefrom_mod;
	trace_t tr;
	int orTr;

	VectorSubtract(traceto, bs->origin, a);
	vectoangles(a, a);

	AngleVectors(a, fwd, NULL, NULL);

	traceto_mod[0] = bs->origin[0] + fwd[0]*4;
	traceto_mod[1] = bs->origin[1] + fwd[1]*4;
	traceto_mod[2] = bs->origin[2] + fwd[2]*4;

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = -18;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 32;

	trap_Trace(&tr, bs->origin, mins, maxs, traceto_mod, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction == 1)
	{
		return 0;
	}

	orTr = tr.entityNum;

	VectorCopy(bs->origin, tracefrom_mod);

	tracefrom_mod[2] += 41;
	traceto_mod[2] += 41;

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = 0;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 8;

	trap_Trace(&tr, tracefrom_mod, mins, maxs, traceto_mod, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction == 1)
	{
		if (orTr >= 0 && orTr < MAX_CLIENTS && botstates[orTr] && botstates[orTr]->jumpTime > level.time)
		{
			return 0; //so bots don't try to jump over each other at the same time
		}

		if (bs->currentEnemy && bs->currentEnemy->s.number == orTr && (BotGetWeaponRange(bs) == BWEAPONRANGE_SABER || BotGetWeaponRange(bs) == BWEAPONRANGE_MELEE))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int BotTrace_Duck(bot_state_t *bs, vec3_t traceto)
{
	vec3_t mins, maxs, a, fwd, traceto_mod, tracefrom_mod;
	trace_t tr;

	VectorSubtract(traceto, bs->origin, a);
	vectoangles(a, a);

	AngleVectors(a, fwd, NULL, NULL);

	traceto_mod[0] = bs->origin[0] + fwd[0]*4;
	traceto_mod[1] = bs->origin[1] + fwd[1]*4;
	traceto_mod[2] = bs->origin[2] + fwd[2]*4;

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = -23;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 8;

	trap_Trace(&tr, bs->origin, mins, maxs, traceto_mod, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction != 1)
	{
		return 0;
	}

	VectorCopy(bs->origin, tracefrom_mod);

	tracefrom_mod[2] += 31;//33;
	traceto_mod[2] += 31;//33;

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = 0;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 32;

	trap_Trace(&tr, tracefrom_mod, mins, maxs, traceto_mod, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction != 1)
	{
		return 1;
	}

	return 0;
}

int PassStandardEnemyChecks(bot_state_t *bs, gentity_t *en)
{
	if (!bs || !en)
	{
		return 0;
	}

	if (!en->client)
	{
		return 0;
	}

	if (en->health < 1)
	{
		return 0;
	}

	if (!en->takedamage)
	{
		return 0;
	}

	if (bs->doingFallback &&
		(gLevelFlags & LEVELFLAG_IGNOREINFALLBACK))
	{
		return 0;
	}

	if (en->client)
	{
		if (en->client->ps.pm_type == PM_INTERMISSION ||
			en->client->ps.pm_type == PM_SPECTATOR)
		{
			return 0;
		}

		if (en->client->sess.sessionTeam == TEAM_SPECTATOR)
		{
			return 0;
		}

		if (!en->client->pers.connected)
		{
			return 0;
		}
	}

	if (!en->s.solid)
	{
		return 0;
	}

	if (bs->client == en->s.number)
	{
		return 0;
	}

	if (OnSameTeam(&g_entities[bs->client], en))
	{
		return 0;
	}

	if (BotMindTricked(bs->client, en->s.number))
	{
		if (bs->currentEnemy && bs->currentEnemy->s.number == en->s.number)
		{
			vec3_t vs;
			float vLen = 0;

			VectorSubtract(bs->origin, en->client->ps.origin, vs);
			vLen = VectorLength(vs);

			if (vLen > 256 && (level.time - en->client->dangerTime) > 150)
			{
				return 0;
			}
		}
	}

	if (en->client->ps.duelInProgress && en->client->ps.duelIndex != bs->client)
	{
		return 0;
	}

	if (bs->cur_ps.duelInProgress && en->s.number != bs->cur_ps.duelIndex)
	{
		return 0;
	}

	if (g_gametype.integer == GT_JEDIMASTER && !en->client->ps.isJediMaster && !bs->cur_ps.isJediMaster)
	{ //rules for attacking non-JM in JM mode
		vec3_t vs;
		float vLen = 0;

		if (!g_friendlyFire.integer)
		{ //can't harm non-JM in JM mode if FF is off
			return 0;
		}

		VectorSubtract(bs->origin, en->client->ps.origin, vs);
		vLen = VectorLength(vs);

		if (vLen > 350)
		{
			return 0;
		}
	}

	/*
	if (en->client && en->client->pers.connected != CON_CONNECTED)
	{
		return 0;
	}
	*/

	return 1;
}

void BotDamageNotification(gclient_t *bot, gentity_t *attacker)
{
	bot_state_t *bs;
	bot_state_t *bs_a;
	int i;

	if (!bot || !attacker || !attacker->client)
	{
		return;
	}

	bs_a = botstates[attacker->s.number];

	if (bs_a)
	{
		bs_a->lastAttacked = &g_entities[bot->ps.clientNum];
		i = 0;

		while (i < MAX_CLIENTS)
		{
			if (botstates[i] &&
				i != bs_a->client &&
				botstates[i]->lastAttacked == &g_entities[bot->ps.clientNum])
			{
				botstates[i]->lastAttacked = NULL;
			}

			i++;
		}
	}
	else //got attacked by a real client, so no one gets rights to lastAttacked
	{
		i = 0;

		while (i < MAX_CLIENTS)
		{
			if (botstates[i] &&
				botstates[i]->lastAttacked == &g_entities[bot->ps.clientNum])
			{
				botstates[i]->lastAttacked = NULL;
			}

			i++;
		}
	}

	bs = botstates[bot->ps.clientNum];

	if (!bs)
	{
		return;
	}

	bs->lastHurt = attacker;

	if (bs->currentEnemy)
	{
		return;
	}

	if (!PassStandardEnemyChecks(bs, attacker))
	{
		return;
	}

	if (PassLovedOneCheck(bs, attacker))
	{
		bs->currentEnemy = attacker;
		bs->enemySeenTime = level.time + ENEMY_FORGET_MS;
	}
}

int BotCanHear(bot_state_t *bs, gentity_t *en, float endist)
{
	float minlen;

	if (!en || !en->client)
	{
		return 0;
	}

	if (en && en->client && en->client->ps.otherSoundTime > level.time)
	{
		minlen = en->client->ps.otherSoundLen;
		goto checkStep;
	}

	if (en && en->client && en->client->ps.footstepTime > level.time)
	{
		minlen = 256;
		goto checkStep;
	}

	if (gBotEventTracker[en->s.number].eventTime < level.time)
	{
		return 0;
	}

	switch(gBotEventTracker[en->s.number].events[gBotEventTracker[en->s.number].eventSequence & (MAX_PS_EVENTS-1)])
	{
	case EV_GLOBAL_SOUND:
		minlen = 256;
		break;
	case EV_FIRE_WEAPON:
	case EV_ALT_FIRE:
	case EV_SABER_ATTACK:
		minlen = 512;
		break;
	case EV_STEP_4:
	case EV_STEP_8:
	case EV_STEP_12:
	case EV_STEP_16:
	case EV_FOOTSTEP:
	case EV_FOOTSTEP_METAL:
	case EV_FOOTWADE:
		minlen = 256;
		break;
	case EV_JUMP:
	case EV_ROLL:
		minlen = 256;
		break;
	default:
		minlen = 999999;
		break;
	}
checkStep:
	if (BotMindTricked(bs->client, en->s.number))
	{ //if mindtricked by this person, cut down on the minlen
		minlen /= 4;
	}

	if (endist <= minlen)
	{
		return 1;
	}

	return 0;
}

void UpdateEventTracker(void)
{
	int i;

	i = 0;

	while (i < MAX_CLIENTS)
	{
		if (gBotEventTracker[i].eventSequence != level.clients[i].ps.eventSequence)
		{ //updated event
			gBotEventTracker[i].eventSequence = level.clients[i].ps.eventSequence;
			gBotEventTracker[i].events[0] = level.clients[i].ps.events[0];
			gBotEventTracker[i].events[1] = level.clients[i].ps.events[1];
			gBotEventTracker[i].eventTime = level.time + 0.5;
		}

		i++;
	}
}

int InFieldOfVision(vec3_t viewangles, float fov, vec3_t angles)
{
	int i;
	float diff, angle;

	for (i = 0; i < 2; i++)
	{
		angle = AngleMod(viewangles[i]);
		angles[i] = AngleMod(angles[i]);
		diff = angles[i] - angle;
		if (angles[i] > angle)
		{
			if (diff > 180.0)
			{
				diff -= 360.0;
			}
		}
		else
		{
			if (diff < -180.0)
			{
				diff += 360.0;
			}
		}
		if (diff > 0)
		{
			if (diff > fov * 0.5)
			{
				return 0;
			}
		}
		else
		{
			if (diff < -fov * 0.5)
			{
				return 0;
			}
		}
	}
	return 1;
}

int PassLovedOneCheck(bot_state_t *bs, gentity_t *ent)
{
	int i;
	bot_state_t *loved;

	if (!bs->lovednum)
	{
		return 1;
	}

	if (g_gametype.integer == GT_TOURNAMENT)
	{ //There is no love in 1-on-1
		return 1;
	}

	i = 0;

	if (!botstates[ent->s.number])
	{ //not a bot
		return 1;
	}

	trap_Cvar_Update(&bot_attachments);

	if (!bot_attachments.integer)
	{
		return 1;
	}

	loved = botstates[ent->s.number];

	while (i < bs->lovednum)
	{
		if (strcmp(level.clients[loved->client].pers.netname, bs->loved[i].name) == 0)
		{
			if (!IsTeamplay() && bs->loved[i].level < 2)
			{ //if FFA and level of love is not greater than 1, just don't care
				return 1;
			}
			else if (IsTeamplay() && !OnSameTeam(&g_entities[bs->client], &g_entities[loved->client]) && bs->loved[i].level < 2)
			{ //is teamplay, but not on same team and level < 2
				return 1;
			}
			else
			{
				return 0;
			}
		}

		i++;
	}

	return 1;
}

qboolean G_ThereIsAMaster(void);

int ScanForEnemies(bot_state_t *bs)
{
	vec3_t a;
	float distcheck;
	float closest;
	int bestindex;
	int i;
	float hasEnemyDist = 0;
	qboolean noAttackNonJM = qfalse;

	closest = 999999;
	i = 0;
	bestindex = -1;

	if (bs->currentEnemy)
	{
		hasEnemyDist = bs->frame_Enemy_Len;
	}

	if (bs->currentEnemy && bs->currentEnemy->client &&
		bs->currentEnemy->client->ps.isJediMaster)
	{ //The Jedi Master must die.
		return -1;
	}

	if (g_gametype.integer == GT_JEDIMASTER)
	{
		if (G_ThereIsAMaster() && !bs->cur_ps.isJediMaster)
		{
			if (!g_friendlyFire.integer)
			{
				noAttackNonJM = qtrue;
			}
			else
			{
				closest = 128; //only get mad at people if they get close enough to you to anger you, or hurt you
			}
		}
	}

	while (i < MAX_CLIENTS)
	{
		if (i != bs->client && g_entities[i].client && !OnSameTeam(&g_entities[bs->client], &g_entities[i]) && PassStandardEnemyChecks(bs, &g_entities[i]) && trap_InPVS(g_entities[i].client->ps.origin, bs->eye) && PassLovedOneCheck(bs, &g_entities[i]))
		{
			VectorSubtract(g_entities[i].client->ps.origin, bs->eye, a);
			distcheck = VectorLength(a);
			vectoangles(a, a);

			if (g_entities[i].client->ps.isJediMaster)
			{ //make us think the Jedi Master is close so we'll attack him above all
				distcheck = 1;
			}

			if (distcheck < closest && ((InFieldOfVision(bs->viewangles, 90, a) && !BotMindTricked(bs->client, i)) || BotCanHear(bs, &g_entities[i], distcheck)) && OrgVisible(bs->eye, g_entities[i].client->ps.origin, -1))
			{
				if (BotMindTricked(bs->client, i))
				{
					if (distcheck < 256 || (level.time - g_entities[i].client->dangerTime) < 100)
					{
						if (!hasEnemyDist || distcheck < (hasEnemyDist - 128))
						{ //if we have an enemy, only switch to closer if he is 128+ closer to avoid flipping out
							if (!noAttackNonJM || g_entities[i].client->ps.isJediMaster)
							{
								closest = distcheck;
								bestindex = i;
							}
						}
					}
				}
				else
				{
					if (!hasEnemyDist || distcheck < (hasEnemyDist - 128))
					{ //if we have an enemy, only switch to closer if he is 128+ closer to avoid flipping out
						if (!noAttackNonJM || g_entities[i].client->ps.isJediMaster)
						{
							closest = distcheck;
							bestindex = i;
						}
					}
				}
			}
		}
		i++;
	}
	
	return bestindex;
}

int WaitingForNow(bot_state_t *bs, vec3_t goalpos)
{ //checks if the bot is doing something along the lines of waiting for an elevator to raise up
	vec3_t xybot, xywp, a;

	if (!bs->wpCurrent)
	{
		return 0;
	}

	if ((int)goalpos[0] != (int)bs->wpCurrent->origin[0] ||
		(int)goalpos[1] != (int)bs->wpCurrent->origin[1] ||
		(int)goalpos[2] != (int)bs->wpCurrent->origin[2])
	{
		return 0;
	}

	VectorCopy(bs->origin, xybot);
	VectorCopy(bs->wpCurrent->origin, xywp);

	xybot[2] = 0;
	xywp[2] = 0;

	VectorSubtract(xybot, xywp, a);

	if (VectorLength(a) < 16 && bs->frame_Waypoint_Len > 100)
	{
		if (CheckForFunc(bs->origin, bs->client))
		{
			return 1; //we're probably standing on an elevator and riding up/down. Or at least we hope so.
		}
	}
	else if (VectorLength(a) < 64 && bs->frame_Waypoint_Len > 64 &&
		CheckForFunc(bs->origin, bs->client))
	{
		bs->noUseTime = level.time + 2000;
	}

	return 0;
}

int BotGetWeaponRange(bot_state_t *bs)
{
	switch (bs->cur_ps.weapon)
	{
	case WP_STUN_BATON:
		return BWEAPONRANGE_MELEE;
	case WP_SABER:
		return BWEAPONRANGE_SABER;
	case WP_BRYAR_PISTOL:
		return BWEAPONRANGE_MID;
	case WP_BLASTER:
		return BWEAPONRANGE_MID;
	case WP_DISRUPTOR:
		return BWEAPONRANGE_MID;
	case WP_BOWCASTER:
		return BWEAPONRANGE_LONG;
	case WP_REPEATER:
		return BWEAPONRANGE_MID;
	case WP_DEMP2:
		return BWEAPONRANGE_LONG;
	case WP_FLECHETTE:
		return BWEAPONRANGE_LONG;
	case WP_ROCKET_LAUNCHER:
		return BWEAPONRANGE_LONG;
	case WP_THERMAL:
		return BWEAPONRANGE_LONG;
	case WP_TRIP_MINE:
		return BWEAPONRANGE_LONG;
	case WP_DET_PACK:
		return BWEAPONRANGE_LONG;
	default:
		return BWEAPONRANGE_MID;
	}
}

int BotIsAChickenWuss(bot_state_t *bs)
{
	int bWRange;

	if (gLevelFlags & LEVELFLAG_IMUSTNTRUNAWAY)
	{
		return 0;
	}

	if (g_gametype.integer == GT_SINGLE_PLAYER)
	{
		return 0;
	}

	if (g_gametype.integer == GT_JEDIMASTER && !bs->cur_ps.isJediMaster)
	{ //Then you may know no fear.
		//Well, unless he's strong.
		if (bs->currentEnemy && bs->currentEnemy->client &&
			bs->currentEnemy->client->ps.isJediMaster &&
			bs->currentEnemy->health > 40 &&
			bs->cur_ps.weapon < WP_ROCKET_LAUNCHER)
		{ //explosive weapons are most effective against the Jedi Master
			goto jmPass;
		}
		return 0;
	}
jmPass:
	if (bs->chickenWussCalculationTime > level.time)
	{
		return 2; //don't want to keep going between two points...
	}

	if (g_gametype.integer == GT_JEDIMASTER && !bs->cur_ps.isJediMaster)
	{
		return 1;
	}

	bs->chickenWussCalculationTime = level.time + MAX_CHICKENWUSS_TIME;

	if (g_entities[bs->client].health < BOT_RUN_HEALTH)
	{
		return 1;
	}

	bWRange = BotGetWeaponRange(bs);

	if (bWRange == BWEAPONRANGE_MELEE || bWRange == BWEAPONRANGE_SABER)
	{
		if (bWRange != BWEAPONRANGE_SABER || !bs->saberSpecialist)
		{
			return 1;
		}
	}

	if (bs->cur_ps.weapon == WP_BRYAR_PISTOL)
	{ //the bryar is a weak weapon, so just try to find a new one if it's what you're having to use
		return 1;
	}

	if (bs->currentEnemy && bs->currentEnemy->client &&
		bs->currentEnemy->client->ps.weapon == WP_SABER &&
		bs->frame_Enemy_Len < 512 && bs->cur_ps.weapon != WP_SABER)
	{ //if close to an enemy with a saber and not using a saber, then try to back off
		return 1;
	}

	//didn't run, reset the timer
	bs->chickenWussCalculationTime = 0;

	return 0;
}

gentity_t *GetNearestBadThing(bot_state_t *bs)
{
	int i = 0;
	float glen;
	vec3_t hold;
	int bestindex = 0;
	float bestdist = 800; //if not within a radius of 800, it's no threat anyway
	int foundindex = 0;
	float factor = 0;
	gentity_t *ent;
	trace_t tr;

	while (i < MAX_GENTITIES)
	{
		ent = &g_entities[i];

		if ( (ent &&
			!ent->client &&
			ent->inuse &&
			ent->damage &&
			/*(ent->s.weapon == WP_THERMAL || ent->s.weapon == WP_FLECHETTE)*/
			ent->s.weapon &&
			ent->splashDamage) ||
			(ent &&
			ent->bolt_Head == 1000 &&
			ent->inuse &&
			ent->health > 0 &&
			ent->boltpoint3 != bs->client &&
			g_entities[ent->boltpoint3].client && !OnSameTeam(&g_entities[bs->client], &g_entities[ent->boltpoint3])) )
		{ //try to escape from anything with a non-0 s.weapon and non-0 damage. This hopefully only means dangerous projectiles.
		  //Or a sentry gun if bolt_Head == 1000. This is a terrible hack, yes.
			VectorSubtract(bs->origin, ent->r.currentOrigin, hold);
			glen = VectorLength(hold);

			if (ent->s.weapon != WP_THERMAL && ent->s.weapon != WP_FLECHETTE &&
				ent->s.weapon != WP_DET_PACK && ent->s.weapon != WP_TRIP_MINE)
			{
				factor = 0.5;

				if (ent->s.weapon && glen <= 256 && bs->settings.skill > 2)
				{ //it's a projectile so push it away
					bs->doForcePush = level.time + 700;
					//G_Printf("PUSH PROJECTILE\n");
				}
			}
			else
			{
				factor = 1;
			}

			if (ent->s.weapon == WP_ROCKET_LAUNCHER &&
				(ent->r.ownerNum == bs->client ||
				(ent->r.ownerNum >= 0 && ent->r.ownerNum < MAX_CLIENTS &&
				g_entities[ent->r.ownerNum].client && OnSameTeam(&g_entities[bs->client], &g_entities[ent->r.ownerNum]))) )
			{ //don't be afraid of your own rockets or your teammates' rockets
				factor = 0;
			}

			if (glen < bestdist*factor && trap_InPVS(bs->origin, ent->s.pos.trBase))
			{
				trap_Trace(&tr, bs->origin, NULL, NULL, ent->s.pos.trBase, bs->client, MASK_SOLID);

				if (tr.fraction == 1 || tr.entityNum == ent->s.number)
				{
					bestindex = i;
					bestdist = glen;
					foundindex = 1;
				}
			}
		}

		if (ent && !ent->client && ent->inuse && ent->damage && ent->s.weapon && ent->r.ownerNum < MAX_CLIENTS && ent->r.ownerNum >= 0)
		{ //if we're in danger of a projectile belonging to someone and don't have an enemy, set the enemy to them
			gentity_t *projOwner = &g_entities[ent->r.ownerNum];

			if (projOwner && projOwner->inuse && projOwner->client)
			{
				if (!bs->currentEnemy)
				{
					if (PassStandardEnemyChecks(bs, projOwner))
					{
						if (PassLovedOneCheck(bs, projOwner))
						{
							VectorSubtract(bs->origin, ent->r.currentOrigin, hold);
							glen = VectorLength(hold);

							if (glen < 512)
							{
								bs->currentEnemy = projOwner;
								bs->enemySeenTime = level.time + ENEMY_FORGET_MS;
							}
						}
					}
				}
			}
		}

		i++;
	}

	if (foundindex)
	{
		bs->dontGoBack = level.time + 1500;
		return &g_entities[bestindex];
	}
	else
	{
		return NULL;
	}
}

int BotDefendFlag(bot_state_t *bs)
{
	wpobject_t *flagPoint;
	vec3_t a;

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		flagPoint = flagRed;
	}
	else if (level.clients[bs->client].sess.sessionTeam == TEAM_BLUE)
	{
		flagPoint = flagBlue;
	}
	else
	{
		return 0;
	}

	if (!flagPoint)
	{
		return 0;
	}

	VectorSubtract(bs->origin, flagPoint->origin, a);

	if (VectorLength(a) > BASE_GUARD_DISTANCE)
	{
		bs->wpDestination = flagPoint;
	}

	return 1;
}

int BotGetEnemyFlag(bot_state_t *bs)
{
	wpobject_t *flagPoint;
	vec3_t a;

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		flagPoint = flagBlue;
	}
	else if (level.clients[bs->client].sess.sessionTeam == TEAM_BLUE)
	{
		flagPoint = flagRed;
	}
	else
	{
		return 0;
	}

	if (!flagPoint)
	{
		return 0;
	}

	VectorSubtract(bs->origin, flagPoint->origin, a);

	if (VectorLength(a) > BASE_GETENEMYFLAG_DISTANCE)
	{
		bs->wpDestination = flagPoint;
	}

	return 1;
}

int BotGetFlagBack(bot_state_t *bs)
{
	int i = 0;
	int myFlag = 0;
	int foundCarrier = 0;
	int tempInt = 0;
	gentity_t *ent = NULL;
	vec3_t usethisvec;

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		myFlag = PW_REDFLAG;
	}
	else
	{
		myFlag = PW_BLUEFLAG;
	}

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client && ent->client->ps.powerups[myFlag] && !OnSameTeam(&g_entities[bs->client], ent))
		{
			foundCarrier = 1;
			break;
		}

		i++;
	}

	if (!foundCarrier)
	{
		return 0;
	}

	if (!ent)
	{
		return 0;
	}

	if (bs->wpDestSwitchTime < level.time)
	{
		if (ent->client)
		{
			VectorCopy(ent->client->ps.origin, usethisvec);
		}
		else
		{
			VectorCopy(ent->s.origin, usethisvec);
		}

		tempInt = GetNearestVisibleWP(usethisvec, 0);

		if (tempInt != -1 && TotalTrailDistance(bs->wpCurrent->index, tempInt, bs) != -1)
		{
			bs->wpDestination = gWPArray[tempInt];
			bs->wpDestSwitchTime = level.time + Q_irand(1000, 5000);
		}
	}

	return 1;
}

int BotGuardFlagCarrier(bot_state_t *bs)
{
	int i = 0;
	int enemyFlag = 0;
	int foundCarrier = 0;
	int tempInt = 0;
	gentity_t *ent = NULL;
	vec3_t usethisvec;

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		enemyFlag = PW_BLUEFLAG;
	}
	else
	{
		enemyFlag = PW_REDFLAG;
	}

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client && ent->client->ps.powerups[enemyFlag] && OnSameTeam(&g_entities[bs->client], ent))
		{
			foundCarrier = 1;
			break;
		}

		i++;
	}

	if (!foundCarrier)
	{
		return 0;
	}

	if (!ent)
	{
		return 0;
	}

	if (bs->wpDestSwitchTime < level.time)
	{
		if (ent->client)
		{
			VectorCopy(ent->client->ps.origin, usethisvec);
		}
		else
		{
			VectorCopy(ent->s.origin, usethisvec);
		}

		tempInt = GetNearestVisibleWP(usethisvec, 0);

		if (tempInt != -1 && TotalTrailDistance(bs->wpCurrent->index, tempInt, bs) != -1)
		{
			bs->wpDestination = gWPArray[tempInt];
			bs->wpDestSwitchTime = level.time + Q_irand(1000, 5000);
		}
	}

	return 1;
}

int BotGetFlagHome(bot_state_t *bs)
{
	wpobject_t *flagPoint;
	vec3_t a;

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		flagPoint = flagRed;
	}
	else if (level.clients[bs->client].sess.sessionTeam == TEAM_BLUE)
	{
		flagPoint = flagBlue;
	}
	else
	{
		return 0;
	}

	if (!flagPoint)
	{
		return 0;
	}

	VectorSubtract(bs->origin, flagPoint->origin, a);

	if (VectorLength(a) > BASE_FLAGWAIT_DISTANCE)
	{
		bs->wpDestination = flagPoint;
	}

	return 1;
}

void GetNewFlagPoint(wpobject_t *wp, gentity_t *flagEnt, int team)
{ //get the nearest possible waypoint to the flag since it's not in its original position
	int i = 0;
	vec3_t a, mins, maxs;
	float bestdist;
	float testdist;
	int bestindex = 0;
	int foundindex = 0;
	trace_t tr;

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = -5;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 5;

	VectorSubtract(wp->origin, flagEnt->s.pos.trBase, a);

	bestdist = VectorLength(a);

	if (bestdist <= WP_KEEP_FLAG_DIST)
	{
		trap_Trace(&tr, wp->origin, mins, maxs, flagEnt->s.pos.trBase, flagEnt->s.number, MASK_SOLID);

		if (tr.fraction == 1)
		{ //this point is good
			return;
		}
	}

	while (i < gWPNum)
	{
		VectorSubtract(gWPArray[i]->origin, flagEnt->s.pos.trBase, a);
		testdist = VectorLength(a);

		if (testdist < bestdist)
		{
			trap_Trace(&tr, gWPArray[i]->origin, mins, maxs, flagEnt->s.pos.trBase, flagEnt->s.number, MASK_SOLID);

			if (tr.fraction == 1)
			{
				foundindex = 1;
				bestindex = i;
				bestdist = testdist;
			}
		}

		i++;
	}

	if (foundindex)
	{
		if (team == TEAM_RED)
		{
			flagRed = gWPArray[bestindex];
		}
		else
		{
			flagBlue = gWPArray[bestindex];
		}
	}
}

int CTFTakesPriority(bot_state_t *bs)
{
	gentity_t *ent = NULL;
	int enemyFlag = 0;
	int myFlag = 0;
	int enemyHasOurFlag = 0;
	// int weHaveEnemyFlag = 0;
	int numOnMyTeam = 0;
	int numOnEnemyTeam = 0;
	int numAttackers = 0;
	int numDefenders = 0;
	int i = 0;
	int idleWP;
	int dosw = 0;
	wpobject_t *dest_sw = NULL;
#ifdef BOT_CTF_DEBUG
	vec3_t t;

	G_Printf("CTFSTATE: %s\n", ctfStateNames[bs->ctfState]);
#endif

	if (g_gametype.integer != GT_CTF && g_gametype.integer != GT_CTY)
	{
		return 0;
	}

	if (bs->cur_ps.weapon == WP_BRYAR_PISTOL &&
		(level.time - bs->lastDeadTime) < BOT_MAX_WEAPON_GATHER_TIME)
	{ //get the nearest weapon laying around base before heading off for battle
		idleWP = GetBestIdleGoal(bs);

		if (idleWP != -1 && gWPArray[idleWP] && gWPArray[idleWP]->inuse)
		{
			if (bs->wpDestSwitchTime < level.time)
			{
				bs->wpDestination = gWPArray[idleWP];
			}
			return 1;
		}
	}
	else if (bs->cur_ps.weapon == WP_BRYAR_PISTOL &&
		(level.time - bs->lastDeadTime) < BOT_MAX_WEAPON_CHASE_CTF &&
		bs->wpDestination && bs->wpDestination->weight)
	{
		dest_sw = bs->wpDestination;
		dosw = 1;
	}

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		myFlag = PW_REDFLAG;
	}
	else
	{
		myFlag = PW_BLUEFLAG;
	}

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		enemyFlag = PW_BLUEFLAG;
	}
	else
	{
		enemyFlag = PW_REDFLAG;
	}

	if (!flagRed || !flagBlue ||
		!flagRed->inuse || !flagBlue->inuse ||
		!eFlagRed || !eFlagBlue)
	{
		return 0;
	}

#ifdef BOT_CTF_DEBUG
	VectorCopy(flagRed->origin, t);
	t[2] += 128;
	G_TestLine(flagRed->origin, t, 0x0000ff, 500);

	VectorCopy(flagBlue->origin, t);
	t[2] += 128;
	G_TestLine(flagBlue->origin, t, 0x0000ff, 500);
#endif

	if (droppedRedFlag && (droppedRedFlag->flags & FL_DROPPED_ITEM))
	{
		GetNewFlagPoint(flagRed, droppedRedFlag, TEAM_RED);
	}
	else
	{
		flagRed = oFlagRed;
	}

	if (droppedBlueFlag && (droppedBlueFlag->flags & FL_DROPPED_ITEM))
	{
		GetNewFlagPoint(flagBlue, droppedBlueFlag, TEAM_BLUE);
	}
	else
	{
		flagBlue = oFlagBlue;
	}

	if (!bs->ctfState)
	{
		return 0;
	}

	i = 0;

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client)
		{
			if (ent->client->ps.powerups[enemyFlag] && OnSameTeam(&g_entities[bs->client], ent))
			{
				// weHaveEnemyFlag = 1;
			}
			else if (ent->client->ps.powerups[myFlag] && !OnSameTeam(&g_entities[bs->client], ent))
			{
				enemyHasOurFlag = 1;
			}

			if (OnSameTeam(&g_entities[bs->client], ent))
			{
				numOnMyTeam++;
			}
			else
			{
				numOnEnemyTeam++;
			}

			if (botstates[ent->s.number])
			{
				if (botstates[ent->s.number]->ctfState == CTFSTATE_ATTACKER ||
					botstates[ent->s.number]->ctfState == CTFSTATE_RETRIEVAL)
				{
					numAttackers++;
				}
				else
				{
					numDefenders++;
				}
			}
			else
			{ //assume real players to be attackers in our logic
				numAttackers++;
			}
		}
		i++;
	}

	if (bs->cur_ps.powerups[enemyFlag])
	{
		if ((numOnMyTeam < 2 || !numAttackers) && enemyHasOurFlag)
		{
			bs->ctfState = CTFSTATE_RETRIEVAL;
		}
		else
		{
			bs->ctfState = CTFSTATE_GETFLAGHOME;
		}
	}
	else if (bs->ctfState == CTFSTATE_GETFLAGHOME)
	{
		bs->ctfState = 0;
	}

	if (bs->state_Forced)
	{
		bs->ctfState = bs->state_Forced;
	}

	if (bs->ctfState == CTFSTATE_DEFENDER)
	{
		if (BotDefendFlag(bs))
		{
			goto success;
		}
	}

	if (bs->ctfState == CTFSTATE_ATTACKER)
	{
		if (BotGetEnemyFlag(bs))
		{
			goto success;
		}
	}

	if (bs->ctfState == CTFSTATE_RETRIEVAL)
	{
		if (BotGetFlagBack(bs))
		{
			goto success;
		}
		else
		{ //can't find anyone on another team being a carrier, so ignore this priority
			bs->ctfState = 0;
		}
	}

	if (bs->ctfState == CTFSTATE_GUARDCARRIER)
	{
		if (BotGuardFlagCarrier(bs))
		{
			goto success;
		}
		else
		{ //can't find anyone on our team being a carrier, so ignore this priority
			bs->ctfState = 0;
		}
	}

	if (bs->ctfState == CTFSTATE_GETFLAGHOME)
	{
		if (BotGetFlagHome(bs))
		{
			goto success;
		}
	}

	return 0;

success:
	if (dosw)
	{ //allow ctf code to run, but if after a particular item then keep going after it
		bs->wpDestination = dest_sw;
	}

	return 1;
}

int EntityVisibleBox(vec3_t org1, vec3_t mins, vec3_t maxs, vec3_t org2, int ignore, int ignore2)
{
	trace_t tr;

	trap_Trace(&tr, org1, mins, maxs, org2, ignore, MASK_SOLID);

	if (tr.fraction == 1 && !tr.startsolid && !tr.allsolid)
	{
		return 1;
	}
	else if (tr.entityNum != ENTITYNUM_NONE && tr.entityNum == ignore2)
	{
		return 1;
	}

	return 0;
}

int JMTakesPriority(bot_state_t *bs)
{
	int i = 0;
	int wpClose = -1;
	gentity_t *theImportantEntity = NULL;

	if (g_gametype.integer != GT_JEDIMASTER)
	{
		return 0;
	}

	if (bs->cur_ps.isJediMaster)
	{
		return 0;
	}

	//jmState becomes the index for the one who carries the saber. If jmState is -1 then the saber is currently
	//without an owner
	bs->jmState = -1;

	while (i < MAX_CLIENTS)
	{
		if (g_entities[i].client && g_entities[i].inuse &&
			g_entities[i].client->ps.isJediMaster)
		{
			bs->jmState = i;
			break;
		}

		i++;
	}

	if (bs->jmState != -1)
	{
		theImportantEntity = &g_entities[bs->jmState];
	}
	else
	{
		theImportantEntity = gJMSaberEnt;
	}

	if (theImportantEntity && theImportantEntity->inuse && bs->destinationGrabTime < level.time)
	{
		if (theImportantEntity->client)
		{
			wpClose = GetNearestVisibleWP(theImportantEntity->client->ps.origin, theImportantEntity->s.number);	
		}
		else
		{
			wpClose = GetNearestVisibleWP(theImportantEntity->r.currentOrigin, theImportantEntity->s.number);	
		}

		if (wpClose != -1 && gWPArray[wpClose] && gWPArray[wpClose]->inuse)
		{
			/*
			Com_Printf("BOT GRABBED IDEAL JM LOCATION\n");
			if (bs->wpDestination != gWPArray[wpClose])
			{
				Com_Printf("IDEAL WAS NOT ALREADY IDEAL\n");

				if (!bs->wpDestination)
				{
					Com_Printf("IDEAL WAS NULL\n");
				}
			}
			*/
			bs->wpDestination = gWPArray[wpClose];
			bs->destinationGrabTime = level.time + 4000;
		}
	}

	return 1;
}

int BotHasAssociated(bot_state_t *bs, wpobject_t *wp)
{
	gentity_t *as;

	if (wp->associated_entity == ENTITYNUM_NONE)
	{ //make it think this is an item we have so we don't go after nothing
		return 1;
	}

	as = &g_entities[wp->associated_entity];

	if (!as || !as->item)
	{
		return 0;
	}

	if (as->item->giType == IT_WEAPON)
	{
		if (bs->cur_ps.stats[STAT_WEAPONS] & (1 << as->item->giTag))
		{
			return 1;
		}

		return 0;
	}
	else if (as->item->giType == IT_HOLDABLE)
	{
		if (bs->cur_ps.stats[STAT_HOLDABLE_ITEMS] & (1 << as->item->giTag))
		{
			return 1;
		}

		return 0;
	}
	else if (as->item->giType == IT_POWERUP)
	{
		if (bs->cur_ps.powerups[as->item->giTag])
		{
			return 1;
		}

		return 0;
	}
	else if (as->item->giType == IT_AMMO)
	{
		if (bs->cur_ps.ammo[as->item->giTag] > 10) //hack
		{
			return 1;
		}

		return 0;
	}

	return 0;
}

int GetBestIdleGoal(bot_state_t *bs)
{
	int i = 0;
	int highestweight = 0;
	int desiredindex = -1;
	int dist_to_weight = 0;
	int traildist;

	if (!bs->wpCurrent)
	{
		return -1;
	}

	if (bs->isCamper != 2)
	{
		if (bs->randomNavTime < level.time)
		{
			if (Q_irand(1, 10) < 5)
			{
				bs->randomNav = 1;
			}
			else
			{
				bs->randomNav = 0;
			}
			
			bs->randomNavTime = level.time + Q_irand(5000, 15000);
		}
	}

	if (bs->randomNav)
	{ //stop looking for items and/or camping on them
		return -1;
	}

	while (i < gWPNum)
	{
		if (gWPArray[i] &&
			gWPArray[i]->inuse &&
			(gWPArray[i]->flags & WPFLAG_GOALPOINT) &&
			gWPArray[i]->weight > highestweight &&
			!BotHasAssociated(bs, gWPArray[i]))
		{
			traildist = TotalTrailDistance(bs->wpCurrent->index, i, bs);

			if (traildist != -1)
			{
				dist_to_weight = (int)traildist/10000;
				dist_to_weight = (gWPArray[i]->weight)-dist_to_weight;

				if (dist_to_weight > highestweight)
				{
					highestweight = dist_to_weight;
					desiredindex = i;
				}
			}
		}

		i++;
	}

	return desiredindex;
}

void GetIdealDestination(bot_state_t *bs)
{
	int tempInt, cWPIndex, bChicken, idleWP;
	float distChange, plusLen, minusLen;
	vec3_t usethisvec, a;
	gentity_t *badthing;

#ifdef _DEBUG
	trap_Cvar_Update(&bot_nogoals);

	if (bot_nogoals.integer)
	{
		return;
	}
#endif

	if (!bs->wpCurrent)
	{
		return;
	}

	if ((level.time - bs->escapeDirTime) > 4000)
	{
		badthing = GetNearestBadThing(bs);
	}
	else
	{
		badthing = NULL;
	}

	if (badthing && badthing->inuse &&
		badthing->health > 0 && badthing->takedamage)
	{
		bs->dangerousObject = badthing;
	}
	else
	{
		bs->dangerousObject = NULL;
	}

	if (!badthing && bs->wpDestIgnoreTime > level.time)
	{
		return;
	}

	if (!badthing && bs->dontGoBack > level.time)
	{
		if (bs->wpDestination)
		{
			bs->wpStoreDest = bs->wpDestination;
		}
		bs->wpDestination = NULL;
		return;
	}
	else if (!badthing && bs->wpStoreDest)
	{ //after we finish running away, switch back to our original destination
		bs->wpDestination = bs->wpStoreDest;
		bs->wpStoreDest = NULL;
	}

	if (badthing && bs->wpCamping)
	{
		bs->wpCamping = NULL;
	}

	if (bs->wpCamping)
	{
		bs->wpDestination = bs->wpCamping;
		return;
	}

	if (!badthing && CTFTakesPriority(bs))
	{
		if (bs->ctfState)
		{
			bs->runningToEscapeThreat = 1;
		}
		return;
	}
	else if (!badthing && JMTakesPriority(bs))
	{
		bs->runningToEscapeThreat = 1;
	}

	if (badthing)
	{
		bs->runningLikeASissy = level.time + 100;

		if (bs->wpDestination)
		{
			bs->wpStoreDest = bs->wpDestination;
		}
		bs->wpDestination = NULL;

		if (bs->wpDirection)
		{
			tempInt = bs->wpCurrent->index+1;
		}
		else
		{
			tempInt = bs->wpCurrent->index-1;
		}

		if (gWPArray[tempInt] && gWPArray[tempInt]->inuse && bs->escapeDirTime < level.time)
		{
			VectorSubtract(badthing->s.pos.trBase, bs->wpCurrent->origin, a);
			plusLen = VectorLength(a);
			VectorSubtract(badthing->s.pos.trBase, gWPArray[tempInt]->origin, a);
			minusLen = VectorLength(a);

			if (plusLen < minusLen)
			{
				if (bs->wpDirection)
				{
					bs->wpDirection = 0;
				}
				else
				{
					bs->wpDirection = 1;
				}

				bs->wpCurrent = gWPArray[tempInt];

				bs->escapeDirTime = level.time + Q_irand(500, 1000);//Q_irand(1000, 1400);

				//G_Printf("Escaping from scary bad thing [%s]\n", badthing->classname);
			}
		}
		//G_Printf("Run away run away run away!\n");
		return;
	}

	distChange = 0; //keep the compiler from complaining

	tempInt = BotGetWeaponRange(bs);

	if (tempInt == BWEAPONRANGE_MELEE)
	{
		distChange = 1;
	}
	else if (tempInt == BWEAPONRANGE_SABER)
	{
		distChange = 1;
	}
	else if (tempInt == BWEAPONRANGE_MID)
	{
		distChange = 128;
	}
	else if (tempInt == BWEAPONRANGE_LONG)
	{
		distChange = 300;
	}

	if (bs->revengeEnemy && bs->revengeEnemy->health > 0 &&
		bs->revengeEnemy->client && bs->revengeEnemy->client->pers.connected == CON_CONNECTED)
	{ //if we hate someone, always try to get to them
		if (bs->wpDestSwitchTime < level.time)
		{
			if (bs->revengeEnemy->client)
			{
				VectorCopy(bs->revengeEnemy->client->ps.origin, usethisvec);
			}
			else
			{
				VectorCopy(bs->revengeEnemy->s.origin, usethisvec);
			}

			tempInt = GetNearestVisibleWP(usethisvec, 0);

			if (tempInt != -1 && TotalTrailDistance(bs->wpCurrent->index, tempInt, bs) != -1)
			{
				bs->wpDestination = gWPArray[tempInt];
				bs->wpDestSwitchTime = level.time + Q_irand(5000, 10000);
			}
		}
	}
	else if (bs->squadLeader && bs->squadLeader->health > 0 &&
		bs->squadLeader->client && bs->squadLeader->client->pers.connected == CON_CONNECTED)
	{
		if (bs->wpDestSwitchTime < level.time)
		{
			if (bs->squadLeader->client)
			{
				VectorCopy(bs->squadLeader->client->ps.origin, usethisvec);
			}
			else
			{
				VectorCopy(bs->squadLeader->s.origin, usethisvec);
			}

			tempInt = GetNearestVisibleWP(usethisvec, 0);

			if (tempInt != -1 && TotalTrailDistance(bs->wpCurrent->index, tempInt, bs) != -1)
			{
				bs->wpDestination = gWPArray[tempInt];
				bs->wpDestSwitchTime = level.time + Q_irand(5000, 10000);
			}
		}
	}
	else if (bs->currentEnemy)
	{
		if (bs->currentEnemy->client)
		{
			VectorCopy(bs->currentEnemy->client->ps.origin, usethisvec);
		}
		else
		{
			VectorCopy(bs->currentEnemy->s.origin, usethisvec);
		}

		bChicken = BotIsAChickenWuss(bs);
		bs->runningToEscapeThreat = bChicken;

		if (bs->frame_Enemy_Len < distChange || (bChicken && bChicken != 2))
		{
			cWPIndex = bs->wpCurrent->index;

			if (bs->frame_Enemy_Len > 400)
			{ //good distance away, start running toward a good place for an item or powerup or whatever
				idleWP = GetBestIdleGoal(bs);

				if (idleWP != -1 && gWPArray[idleWP] && gWPArray[idleWP]->inuse)
				{
					bs->wpDestination = gWPArray[idleWP];
				}
			}
			else if (gWPArray[cWPIndex-1] && gWPArray[cWPIndex-1]->inuse &&
				gWPArray[cWPIndex+1] && gWPArray[cWPIndex+1]->inuse)
			{
				VectorSubtract(gWPArray[cWPIndex+1]->origin, usethisvec, a);
				plusLen = VectorLength(a);
				VectorSubtract(gWPArray[cWPIndex-1]->origin, usethisvec, a);
				minusLen = VectorLength(a);

				if (minusLen > plusLen)
				{
					bs->wpDestination = gWPArray[cWPIndex-1];
				}
				else
				{
					bs->wpDestination = gWPArray[cWPIndex+1];
				}
			}
		}
		else if (bChicken != 2 && bs->wpDestSwitchTime < level.time)
		{
			tempInt = GetNearestVisibleWP(usethisvec, 0);

			if (tempInt != -1 && TotalTrailDistance(bs->wpCurrent->index, tempInt, bs) != -1)
			{
				bs->wpDestination = gWPArray[tempInt];

				if (g_gametype.integer == GT_SINGLE_PLAYER)
				{ //be more aggressive
					bs->wpDestSwitchTime = level.time + Q_irand(300, 1000);
				}
				else
				{
					bs->wpDestSwitchTime = level.time + Q_irand(1000, 5000);
				}
			}
		}
	}

	if (!bs->wpDestination && bs->wpDestSwitchTime < level.time)
	{
		//G_Printf("I need something to do\n");
		idleWP = GetBestIdleGoal(bs);

		if (idleWP != -1 && gWPArray[idleWP] && gWPArray[idleWP]->inuse)
		{
			bs->wpDestination = gWPArray[idleWP];
		}
	}
}

void CommanderBotCTFAI(bot_state_t *bs)
{
	int i = 0;
	gentity_t *ent;
	int squadmates = 0;
	gentity_t *squad[MAX_CLIENTS];
	int defendAttackPriority = 0; //0 == attack, 1 == defend
	int guardDefendPriority = 0; //0 == defend, 1 == guard
	int attackRetrievePriority = 0; //0 == retrieve, 1 == attack
	int myFlag = 0;
	int enemyFlag = 0;
	int enemyHasOurFlag = 0;
	int weHaveEnemyFlag = 0;
	int numOnMyTeam = 0;
	int numOnEnemyTeam = 0;
	int numAttackers = 0;
	int numDefenders = 0;

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		myFlag = PW_REDFLAG;
	}
	else
	{
		myFlag = PW_BLUEFLAG;
	}

	if (level.clients[bs->client].sess.sessionTeam == TEAM_RED)
	{
		enemyFlag = PW_BLUEFLAG;
	}
	else
	{
		enemyFlag = PW_REDFLAG;
	}

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client)
		{
			if (ent->client->ps.powerups[enemyFlag] && OnSameTeam(&g_entities[bs->client], ent))
			{
				weHaveEnemyFlag = 1;
			}
			else if (ent->client->ps.powerups[myFlag] && !OnSameTeam(&g_entities[bs->client], ent))
			{
				enemyHasOurFlag = 1;
			}

			if (OnSameTeam(&g_entities[bs->client], ent))
			{
				numOnMyTeam++;
			}
			else
			{
				numOnEnemyTeam++;
			}

			if (botstates[ent->s.number])
			{
				if (botstates[ent->s.number]->ctfState == CTFSTATE_ATTACKER ||
					botstates[ent->s.number]->ctfState == CTFSTATE_RETRIEVAL)
				{
					numAttackers++;
				}
				else
				{
					numDefenders++;
				}
			}
			else
			{ //assume real players to be attackers in our logic
				numAttackers++;
			}
		}
		i++;
	}

	i = 0;

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client && botstates[i] && botstates[i]->squadLeader && botstates[i]->squadLeader->s.number == bs->client && i != bs->client)
		{
			squad[squadmates] = ent;
			squadmates++;
		}

		i++;
	}

	squad[squadmates] = &g_entities[bs->client];
	squadmates++;

	i = 0;

	if (enemyHasOurFlag && !weHaveEnemyFlag)
	{ //start off with an attacker instead of a retriever if we don't have the enemy flag yet so that they can't capture it first.
	  //after that we focus on getting our flag back.
		attackRetrievePriority = 1;
	}

	while (i < squadmates)
	{
		if (squad[i] && squad[i]->client && botstates[squad[i]->s.number])
		{
			if (botstates[squad[i]->s.number]->ctfState != CTFSTATE_GETFLAGHOME)
			{ //never tell a bot to stop trying to bring the flag to the base
				if (defendAttackPriority)
				{
					if (weHaveEnemyFlag)
					{
						if (guardDefendPriority)
						{
							botstates[squad[i]->s.number]->ctfState = CTFSTATE_GUARDCARRIER;
							guardDefendPriority = 0;
						}
						else
						{
							botstates[squad[i]->s.number]->ctfState = CTFSTATE_DEFENDER;
							guardDefendPriority = 1;
						}
					}
					else
					{
						botstates[squad[i]->s.number]->ctfState = CTFSTATE_DEFENDER;
					}
					defendAttackPriority = 0;
				}
				else
				{
					if (enemyHasOurFlag)
					{
						if (attackRetrievePriority)
						{
							botstates[squad[i]->s.number]->ctfState = CTFSTATE_ATTACKER;
							attackRetrievePriority = 0;
						}
						else
						{
							botstates[squad[i]->s.number]->ctfState = CTFSTATE_RETRIEVAL;
							attackRetrievePriority = 1;
						}
					}
					else
					{
						botstates[squad[i]->s.number]->ctfState = CTFSTATE_ATTACKER;
					}
					defendAttackPriority = 1;
				}
			}
			else if ((numOnMyTeam < 2 || !numAttackers) && enemyHasOurFlag)
			{ //I'm the only one on my team who will attack and the enemy has my flag, I have to go after him
				botstates[squad[i]->s.number]->ctfState = CTFSTATE_RETRIEVAL;
			}
		}

		i++;
	}
}

void CommanderBotSagaAI(bot_state_t *bs)
{
	int i = 0;
	int squadmates = 0;
	int commanded = 0;
	int teammates = 0;
	gentity_t *squad[MAX_CLIENTS];
	gentity_t *ent;
	bot_state_t *bst;

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client && OnSameTeam(&g_entities[bs->client], ent) && botstates[ent->s.number])
		{
			bst = botstates[ent->s.number];

			if (bst && !bst->isSquadLeader && !bst->state_Forced)
			{
				squad[squadmates] = ent;
				squadmates++;
			}
			else if (bst && !bst->isSquadLeader && bst->state_Forced)
			{ //count them as commanded
				commanded++;
			}
		}

		if (ent && ent->client && OnSameTeam(&g_entities[bs->client], ent))
		{
			teammates++;
		}

		i++;
	}
	
	if (!squadmates)
	{
		return;
	}

	//tell squad mates to do what I'm doing, up to half of team, let the other half make their own decisions
	i = 0;

	while (i < squadmates && squad[i])
	{
		bst = botstates[squad[i]->s.number];

		if (commanded > teammates/2)
		{
			break;
		}

		if (bst)
		{
			bst->state_Forced = bs->sagaState;
			bst->sagaState = bs->sagaState;
			commanded++;
		}

		i++;
	}
}

void BotDoTeamplayAI(bot_state_t *bs)
{
	if (bs->state_Forced)
	{
		bs->teamplayState = bs->state_Forced;
	}

	if (bs->teamplayState == TEAMPLAYSTATE_REGROUP)
	{ //force to find a new leader
		bs->squadLeader = NULL;
		bs->isSquadLeader = 0;
	}
}

void CommanderBotTeamplayAI(bot_state_t *bs)
{
	int i = 0;
	int squadmates = 0;
	int teammates = 0;
	int teammate_indanger = -1;
	int teammate_helped = 0;
	int foundsquadleader = 0;
	int worsthealth = 50;
	gentity_t *squad[MAX_CLIENTS];
	gentity_t *ent;
	bot_state_t *bst;

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client && OnSameTeam(&g_entities[bs->client], ent) && botstates[ent->s.number])
		{
			bst = botstates[ent->s.number];

			if (foundsquadleader && bst && bst->isSquadLeader)
			{ //never more than one squad leader
				bst->isSquadLeader = 0;
			}

			if (bst && !bst->isSquadLeader)
			{
				squad[squadmates] = ent;
				squadmates++;
			}
			else if (bst)
			{
				foundsquadleader = 1;
			}
		}

		if (ent && ent->client && OnSameTeam(&g_entities[bs->client], ent))
		{
			teammates++;

			if (ent->health < worsthealth)
			{
				teammate_indanger = ent->s.number;
				worsthealth = ent->health;
			}
		}

		i++;
	}
	
	if (!squadmates)
	{
		return;
	}

	i = 0;

	while (i < squadmates && squad[i])
	{
		bst = botstates[squad[i]->s.number];

		if (bst && !bst->state_Forced)
		{ //only order if this guy is not being ordered directly by the real player team leader
			if (teammate_indanger >= 0 && !teammate_helped)
			{ //send someone out to help whoever needs help most at the moment
				bst->teamplayState = TEAMPLAYSTATE_ASSISTING;
				bst->squadLeader = &g_entities[teammate_indanger];
				teammate_helped = 1;
			}
			else if ((teammate_indanger == -1 || teammate_helped) && bst->teamplayState == TEAMPLAYSTATE_ASSISTING)
			{ //no teammates need help badly, but this guy is trying to help them anyway, so stop
				bst->teamplayState = TEAMPLAYSTATE_FOLLOWING;
				bst->squadLeader = &g_entities[bs->client];
			}

			if (bs->squadRegroupInterval < level.time && Q_irand(1, 10) < 5)
			{ //every so often tell the squad to regroup for the sake of variation
				if (bst->teamplayState == TEAMPLAYSTATE_FOLLOWING)
				{
					bst->teamplayState = TEAMPLAYSTATE_REGROUP;
				}

				bs->isSquadLeader = 0;
				bs->squadCannotLead = level.time + 500;
				bs->squadRegroupInterval = level.time + Q_irand(45000, 65000);
			}
		}

		i++;
	}	
}

void CommanderBotAI(bot_state_t *bs)
{
	if (g_gametype.integer == GT_CTF || g_gametype.integer == GT_CTY)
	{
		CommanderBotCTFAI(bs);
	}
	else if (g_gametype.integer == GT_SAGA)
	{
		CommanderBotSagaAI(bs);
	}
	else if (g_gametype.integer == GT_TEAM)
	{
		CommanderBotTeamplayAI(bs);
	}
}

void MeleeCombatHandling(bot_state_t *bs)
{
	vec3_t usethisvec;
	vec3_t downvec;
	vec3_t midorg;
	vec3_t a;
	vec3_t fwd;
	vec3_t mins, maxs;
	trace_t tr;
	int en_down;
	int me_down;
	int mid_down;

	if (!bs->currentEnemy)
	{
		return;
	}

	if (bs->currentEnemy->client)
	{
		VectorCopy(bs->currentEnemy->client->ps.origin, usethisvec);
	}
	else
	{
		VectorCopy(bs->currentEnemy->s.origin, usethisvec);
	}

	if (bs->meleeStrafeTime < level.time)
	{
		if (bs->meleeStrafeDir)
		{
			bs->meleeStrafeDir = 0;
		}
		else
		{
			bs->meleeStrafeDir = 1;
		}

		bs->meleeStrafeTime = level.time + Q_irand(500, 1800);
	}

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = -24;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 32;

	VectorCopy(usethisvec, downvec);
	downvec[2] -= 4096;

	trap_Trace(&tr, usethisvec, mins, maxs, downvec, -1, MASK_SOLID);

	en_down = (int)tr.endpos[2];

	VectorCopy(bs->origin, downvec);
	downvec[2] -= 4096;

	trap_Trace(&tr, bs->origin, mins, maxs, downvec, -1, MASK_SOLID);

	me_down = (int)tr.endpos[2];

	VectorSubtract(usethisvec, bs->origin, a);
	vectoangles(a, a);
	AngleVectors(a, fwd, NULL, NULL);

	midorg[0] = bs->origin[0] + fwd[0]*bs->frame_Enemy_Len/2;
	midorg[1] = bs->origin[1] + fwd[1]*bs->frame_Enemy_Len/2;
	midorg[2] = bs->origin[2] + fwd[2]*bs->frame_Enemy_Len/2;

	VectorCopy(midorg, downvec);
	downvec[2] -= 4096;

	trap_Trace(&tr, midorg, mins, maxs, downvec, -1, MASK_SOLID);

	mid_down = (int)tr.endpos[2];

	if (me_down == en_down &&
		en_down == mid_down)
	{
		VectorCopy(usethisvec, bs->goalPosition);
	}
}

void SaberCombatHandling(bot_state_t *bs)
{
	vec3_t usethisvec;
	vec3_t downvec;
	vec3_t midorg;
	vec3_t a;
	vec3_t fwd;
	vec3_t mins, maxs;
	trace_t tr;
	int en_down;
	int me_down;
	int mid_down;

	if (!bs->currentEnemy)
	{
		return;
	}

	if (bs->currentEnemy->client)
	{
		VectorCopy(bs->currentEnemy->client->ps.origin, usethisvec);
	}
	else
	{
		VectorCopy(bs->currentEnemy->s.origin, usethisvec);
	}

	if (bs->meleeStrafeTime < level.time)
	{
		if (bs->meleeStrafeDir)
		{
			bs->meleeStrafeDir = 0;
		}
		else
		{
			bs->meleeStrafeDir = 1;
		}

		bs->meleeStrafeTime = level.time + Q_irand(500, 1800);
	}

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = -24;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 32;

	VectorCopy(usethisvec, downvec);
	downvec[2] -= 4096;

	trap_Trace(&tr, usethisvec, mins, maxs, downvec, -1, MASK_SOLID);

	en_down = (int)tr.endpos[2];

	if (tr.startsolid || tr.allsolid)
	{
		en_down = 1;
		me_down = 2;
	}
	else
	{
		VectorCopy(bs->origin, downvec);
		downvec[2] -= 4096;

		trap_Trace(&tr, bs->origin, mins, maxs, downvec, -1, MASK_SOLID);

		me_down = (int)tr.endpos[2];

		if (tr.startsolid || tr.allsolid)
		{
			en_down = 1;
			me_down = 2;
		}
	}

	VectorSubtract(usethisvec, bs->origin, a);
	vectoangles(a, a);
	AngleVectors(a, fwd, NULL, NULL);

	midorg[0] = bs->origin[0] + fwd[0]*bs->frame_Enemy_Len/2;
	midorg[1] = bs->origin[1] + fwd[1]*bs->frame_Enemy_Len/2;
	midorg[2] = bs->origin[2] + fwd[2]*bs->frame_Enemy_Len/2;

	VectorCopy(midorg, downvec);
	downvec[2] -= 4096;

	trap_Trace(&tr, midorg, mins, maxs, downvec, -1, MASK_SOLID);

	mid_down = (int)tr.endpos[2];

	if (me_down == en_down &&
		en_down == mid_down)
	{
		if (usethisvec[2] > (bs->origin[2]+32) &&
			bs->currentEnemy->client &&
			bs->currentEnemy->client->ps.groundEntityNum == ENTITYNUM_NONE)
		{
			bs->jumpTime = level.time + 100;
		}

		if (bs->frame_Enemy_Len > 128)
		{ //be ready to attack
			bs->saberDefending = 0;
			bs->saberDefendDecideTime = level.time + Q_irand(1000, 2000);
		}
		else
		{
			if (bs->saberDefendDecideTime < level.time)
			{
				if (bs->saberDefending)
				{
					bs->saberDefending = 0;
				}
				else
				{
					bs->saberDefending = 1;
				}

				bs->saberDefendDecideTime = level.time + Q_irand(500, 2000);
			}
		}

		if (bs->frame_Enemy_Len < 54)
		{
			VectorCopy(bs->origin, bs->goalPosition);
			bs->saberBFTime = 0;
		}
		else
		{
			VectorCopy(usethisvec, bs->goalPosition);
		}

		if (bs->frame_Enemy_Len > 90 && bs->saberBFTime > level.time && bs->saberBTime > level.time && bs->beStill < level.time && bs->saberSTime < level.time)
		{
			bs->beStill = level.time + Q_irand(500, 1000);
			bs->saberSTime = level.time + Q_irand(1200, 1800);
		}
		else if (bs->currentEnemy->client && bs->currentEnemy->client->ps.weapon == WP_SABER && bs->frame_Enemy_Len < 80 && ((Q_irand(1, 10) < 8 && bs->saberBFTime < level.time) || bs->saberBTime > level.time))
		{
			vec3_t vs;
			vec3_t groundcheck;

			VectorSubtract(bs->origin, usethisvec, vs);
			VectorNormalize(vs);

			bs->goalPosition[0] = bs->origin[0] + vs[0]*64;
			bs->goalPosition[1] = bs->origin[1] + vs[1]*64;
			bs->goalPosition[2] = bs->origin[2] + vs[2]*64;

			if (bs->saberBTime < level.time)
			{
				bs->saberBFTime = level.time + Q_irand(900, 1300);
				bs->saberBTime = level.time + Q_irand(300, 700);
			}

			VectorCopy(bs->goalPosition, groundcheck);

			groundcheck[2] -= 64;

			trap_Trace(&tr, bs->goalPosition, NULL, NULL, groundcheck, bs->client, MASK_SOLID);
			
			if (tr.fraction == 1.0)
			{ //don't back off of a ledge
				VectorCopy(usethisvec, bs->goalPosition);
			}
		}
		else if (bs->currentEnemy->client && bs->currentEnemy->client->ps.weapon == WP_SABER && bs->frame_Enemy_Len >= 75)
		{
			bs->saberBFTime = level.time + Q_irand(700, 1300);
			bs->saberBTime = 0;
		}
	}
	else if (bs->frame_Enemy_Len <= 56)
	{
		bs->doAttack = 1;
		bs->saberDefending = 0;
	}
}

float BotWeaponCanLead(bot_state_t *bs)
{
	int weap = bs->cur_ps.weapon;

	if (weap == WP_BRYAR_PISTOL)
	{
		return 0.5;
	}
	if (weap == WP_BLASTER)
	{
		return 0.35;
	}
	if (weap == WP_BOWCASTER)
	{
		return 0.5;
	}
	if (weap == WP_REPEATER)
	{
		return 0.45;
	}
	if (weap == WP_THERMAL)
	{
		return 0.5;
	}
	if (weap == WP_DEMP2)
	{
		return 0.35;
	}
	if (weap == WP_ROCKET_LAUNCHER)
	{
		return 0.7;
	}
	
	return 0;
}

void BotAimLeading(bot_state_t *bs, vec3_t headlevel, float leadAmount)
{
	int x;
	vec3_t predictedSpot;
	vec3_t movementVector;
	vec3_t a, ang;
	float vtotal;

	if (!bs->currentEnemy ||
		!bs->currentEnemy->client)
	{
		return;
	}

	if (!bs->frame_Enemy_Len)
	{
		return;
	}

	vtotal = 0;

	if (bs->currentEnemy->client->ps.velocity[0] < 0)
	{
		vtotal += -bs->currentEnemy->client->ps.velocity[0];
	}
	else
	{
		vtotal += bs->currentEnemy->client->ps.velocity[0];
	}

	if (bs->currentEnemy->client->ps.velocity[1] < 0)
	{
		vtotal += -bs->currentEnemy->client->ps.velocity[1];
	}
	else
	{
		vtotal += bs->currentEnemy->client->ps.velocity[1];
	}

	if (bs->currentEnemy->client->ps.velocity[2] < 0)
	{
		vtotal += -bs->currentEnemy->client->ps.velocity[2];
	}
	else
	{
		vtotal += bs->currentEnemy->client->ps.velocity[2];
	}

	//G_Printf("Leadin target with a velocity total of %f\n", vtotal);

	VectorCopy(bs->currentEnemy->client->ps.velocity, movementVector);

	VectorNormalize(movementVector);

	x = bs->frame_Enemy_Len*leadAmount; //hardly calculated with an exact science, but it works

	if (vtotal > 400)
	{
		vtotal = 400;
	}

	if (vtotal)
	{
		x = (bs->frame_Enemy_Len*0.9)*leadAmount*(vtotal*0.0012); //hardly calculated with an exact science, but it works
	}
	else
	{
		x = (bs->frame_Enemy_Len*0.9)*leadAmount; //hardly calculated with an exact science, but it works
	}

	predictedSpot[0] = headlevel[0] + (movementVector[0]*x);
	predictedSpot[1] = headlevel[1] + (movementVector[1]*x);
	predictedSpot[2] = headlevel[2] + (movementVector[2]*x);

	VectorSubtract(predictedSpot, bs->eye, a);
	vectoangles(a, ang);
	VectorCopy(ang, bs->goalAngles);
}

void BotAimOffsetGoalAngles(bot_state_t *bs)
{
	int i;
	float accVal;
	i = 0;

	if (bs->skills.perfectaim)
	{
		return;
	}

	if (bs->aimOffsetTime > level.time)
	{
		if (bs->aimOffsetAmtYaw)
		{
			bs->goalAngles[YAW] += bs->aimOffsetAmtYaw;
		}

		if (bs->aimOffsetAmtPitch)
		{
			bs->goalAngles[PITCH] += bs->aimOffsetAmtPitch;
		}
		
		while (i <= 2)
		{
			if (bs->goalAngles[i] > 360)
			{
				bs->goalAngles[i] -= 360;
			}

			if (bs->goalAngles[i] < 0)
			{
				bs->goalAngles[i] += 360;
			}

			i++;
		}
		return;
	}

	accVal = bs->skills.accuracy/bs->settings.skill;

	if (bs->currentEnemy && BotMindTricked(bs->client, bs->currentEnemy->s.number))
	{ //having to judge where they are by hearing them, so we should be quite inaccurate here
		accVal *= 7;

		if (accVal < 30)
		{
			accVal = 30;
		}
	}

	if (bs->revengeEnemy && bs->revengeHateLevel &&
		bs->currentEnemy == bs->revengeEnemy)
	{ //bot becomes more skilled as anger level raises
		accVal = accVal/bs->revengeHateLevel;
	}

	if (bs->currentEnemy && bs->frame_Enemy_Vis)
	{ //assume our goal is aiming at the enemy, seeing as he's visible and all
		if (!bs->currentEnemy->s.pos.trDelta[0] &&
			!bs->currentEnemy->s.pos.trDelta[1] &&
			!bs->currentEnemy->s.pos.trDelta[2])
		{
			accVal = 0; //he's not even moving, so he shouldn't really be hard to hit.
		}
		else
		{
			accVal += accVal*0.25; //if he's moving he's this much harder to hit
		}

		if (g_entities[bs->client].s.pos.trDelta[0] ||
			g_entities[bs->client].s.pos.trDelta[1] ||
			g_entities[bs->client].s.pos.trDelta[2])
		{
			accVal += accVal*0.15; //make it somewhat harder to aim if we're moving also
		}
	}

	if (accVal > 90)
	{
		accVal = 90;
	}
	if (accVal < 1)
	{
		accVal = 0;
	}

	if (!accVal)
	{
		bs->aimOffsetAmtYaw = 0;
		bs->aimOffsetAmtPitch = 0;
		return;
	}

	if (rand()%10 <= 5)
	{
		bs->aimOffsetAmtYaw = rand()%(int)accVal;
	}
	else
	{
		bs->aimOffsetAmtYaw = -(rand()%(int)accVal);
	}

	if (rand()%10 <= 5)
	{
		bs->aimOffsetAmtPitch = rand()%(int)accVal;
	}
	else
	{
		bs->aimOffsetAmtPitch = -(rand()%(int)accVal);
	}

	bs->aimOffsetTime = level.time + rand()%500 + 200;
}

int ShouldSecondaryFire(bot_state_t *bs)
{
	int weap;
	int dif;
	float rTime;

	weap = bs->cur_ps.weapon;

	if (bs->cur_ps.ammo[weaponData[weap].ammoIndex] < weaponData[weap].altEnergyPerShot)
	{
		return 0;
	}

	if (bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT && bs->cur_ps.weapon == WP_ROCKET_LAUNCHER)
	{
		float heldTime = (level.time - bs->cur_ps.weaponChargeTime);

		rTime = bs->cur_ps.rocketLockTime;

		if (rTime < 1)
		{
			rTime = bs->cur_ps.rocketLastValidTime;
		}

		if (heldTime > 5000)
		{ //just give up and release it if we can't manage a lock in 5 seconds
			return 2;
		}

		if (rTime > 0)
		{
			dif = ( level.time - rTime ) / ( 1200.0f / 16.0f );
			
			if (dif >= 10)
			{
				return 2;
			}
			else if (bs->frame_Enemy_Len > 250)
			{
				return 1;
			}
		}
		else if (bs->frame_Enemy_Len > 250)
		{
			return 1;
		}
	}
	else if ((bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT) && (level.time - bs->cur_ps.weaponChargeTime) > bs->altChargeTime)
	{
		return 2;
	}
	else if (bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT)
	{
		return 1;
	}

	if (weap == WP_BRYAR_PISTOL && bs->frame_Enemy_Len < 300)
	{
		return 1;
	}
	else if (weap == WP_BOWCASTER && bs->frame_Enemy_Len > 300)
	{
		return 1;
	}
	else if (weap == WP_REPEATER && bs->frame_Enemy_Len < 600 && bs->frame_Enemy_Len > 250)
	{
		return 1;
	}
	else if (weap == WP_BLASTER && bs->frame_Enemy_Len < 300)
	{
		return 1;
	}
	else if (weap == WP_ROCKET_LAUNCHER && bs->frame_Enemy_Len > 250)
	{
		return 1;
	}

	return 0;
}

int CombatBotAI(bot_state_t *bs, float thinktime)
{
	vec3_t eorg, a;
	int secFire;
	float fovcheck;

	if (!bs->currentEnemy)
	{
		return 0;
	}

	if (bs->currentEnemy->client)
	{
		VectorCopy(bs->currentEnemy->client->ps.origin, eorg);
	}
	else
	{
		VectorCopy(bs->currentEnemy->s.origin, eorg);
	}

	VectorSubtract(eorg, bs->eye, a);
	vectoangles(a, a);

	if (BotGetWeaponRange(bs) == BWEAPONRANGE_SABER)
	{
		if (bs->frame_Enemy_Len <= SABER_ATTACK_RANGE)
		{
			bs->doAttack = 1;
		}
	}
	else if (BotGetWeaponRange(bs) == BWEAPONRANGE_MELEE)
	{
		if (bs->frame_Enemy_Len <= MELEE_ATTACK_RANGE)
		{
			bs->doAttack = 1;
		}
	}
	else
	{
		if (bs->cur_ps.weapon == WP_THERMAL || bs->cur_ps.weapon == WP_ROCKET_LAUNCHER)
		{ //be careful with the hurty weapons
			fovcheck = 40;

			if (bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT &&
				bs->cur_ps.weapon == WP_ROCKET_LAUNCHER)
			{ //if we're charging the weapon up then we can hold fire down within a normal fov
				fovcheck = 60;
			}
		}
		else
		{
			fovcheck = 60;
		}

		if (bs->cur_ps.weaponstate == WEAPON_CHARGING ||
			bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT)
		{
			fovcheck = 160;
		}

		if (bs->frame_Enemy_Len < 128)
		{
			fovcheck *= 2;
		}

		if (InFieldOfVision(bs->viewangles, fovcheck, a))
		{
			if (bs->cur_ps.weapon == WP_THERMAL)
			{
				if (((level.time - bs->cur_ps.weaponChargeTime) < (bs->frame_Enemy_Len*2) &&
					(level.time - bs->cur_ps.weaponChargeTime) < 4000 &&
					bs->frame_Enemy_Len > 64) ||
					(bs->cur_ps.weaponstate != WEAPON_CHARGING &&
					bs->cur_ps.weaponstate != WEAPON_CHARGING_ALT))
				{
					if (bs->cur_ps.weaponstate != WEAPON_CHARGING && bs->cur_ps.weaponstate != WEAPON_CHARGING_ALT)
					{
						if (bs->frame_Enemy_Len > 512 && bs->frame_Enemy_Len < 800)
						{
							bs->doAltAttack = 1;
							//bs->doAttack = 1;
						}
						else
						{
							bs->doAttack = 1;
							//bs->doAltAttack = 1;
						}
					}

					if (bs->cur_ps.weaponstate == WEAPON_CHARGING)
					{
						bs->doAttack = 1;
					}
					else if (bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT)
					{
						bs->doAltAttack = 1;
					}
				}
			}
			else
			{
				secFire = ShouldSecondaryFire(bs);

				if (bs->cur_ps.weaponstate != WEAPON_CHARGING_ALT &&
					bs->cur_ps.weaponstate != WEAPON_CHARGING)
				{
					bs->altChargeTime = Q_irand(500, 1000);
				}

				if (secFire == 1)
				{
					bs->doAltAttack = 1;
				}
				else if (!secFire)
				{
					if (bs->cur_ps.weapon != WP_THERMAL)
					{
						if (bs->cur_ps.weaponstate != WEAPON_CHARGING ||
							bs->altChargeTime > (level.time - bs->cur_ps.weaponChargeTime))
						{
							bs->doAttack = 1;
						}
					}
					else
					{
						bs->doAttack = 1;
					}
				}

				if (secFire == 2)
				{ //released a charge
					return 1;
				}
			}
		}
	}

	return 0;
}

int BotFallbackNavigation(bot_state_t *bs)
{
	vec3_t b_angle, fwd, trto, mins, maxs;
	trace_t tr;

	if (bs->currentEnemy && bs->frame_Enemy_Vis)
	{
		return 2; //we're busy
	}

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = 0;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 32;

	bs->goalAngles[PITCH] = 0;
	bs->goalAngles[ROLL] = 0;

	VectorCopy(bs->goalAngles, b_angle);

	AngleVectors(b_angle, fwd, NULL, NULL);

	trto[0] = bs->origin[0] + fwd[0]*16;
	trto[1] = bs->origin[1] + fwd[1]*16;
	trto[2] = bs->origin[2] + fwd[2]*16;

	trap_Trace(&tr, bs->origin, mins, maxs, trto, -1, MASK_SOLID);

	if (tr.fraction == 1)
	{
		VectorCopy(trto, bs->goalPosition);
		return 1; //success!
	}
	else
	{
		bs->goalAngles[YAW] = rand()%360;
	}

	return 0;
}

int BotTryAnotherWeapon(bot_state_t *bs)
{ //out of ammo, resort to the first weapon we come across that has ammo
	int i;

	i = 0;

	while (i < WP_NUM_WEAPONS)
	{
		if (bs->cur_ps.ammo[weaponData[i].ammoIndex] > weaponData[i].energyPerShot &&
			(bs->cur_ps.stats[STAT_WEAPONS] & (1 << i)))
		{
			bs->virtualWeapon = i;
			BotSelectWeapon(bs->client, i);
			//bs->cur_ps.weapon = i;
			//level.clients[bs->client].ps.weapon = i;
			return 1;
		}

		i++;
	}

	if (bs->cur_ps.weapon != 1 && bs->virtualWeapon != 1)
	{ //should always have this.. shouldn't we?
		bs->virtualWeapon = 1;
		BotSelectWeapon(bs->client, 1);
		//bs->cur_ps.weapon = 1;
		//level.clients[bs->client].ps.weapon = 1;
		return 1;
	}

	return 0;
}

qboolean BotWeaponSelectable(bot_state_t *bs, int weapon)
{
	if (bs->cur_ps.ammo[weaponData[weapon].ammoIndex] >= weaponData[weapon].energyPerShot &&
		(bs->cur_ps.stats[STAT_WEAPONS] & (1 << weapon)))
	{
		return qtrue;
	}
	
	return qfalse;
}

int BotSelectIdealWeapon(bot_state_t *bs)
{
	int i;
	int bestweight = -1;
	int bestweapon = 0;

	i = 0;

	while (i < WP_NUM_WEAPONS)
	{
		if (bs->cur_ps.ammo[weaponData[i].ammoIndex] >= weaponData[i].energyPerShot &&
			bs->botWeaponWeights[i] > bestweight &&
			(bs->cur_ps.stats[STAT_WEAPONS] & (1 << i)))
		{
			if (i == WP_THERMAL)
			{ //special case..
				if (bs->currentEnemy && bs->frame_Enemy_Len < 700)
				{
					bestweight = bs->botWeaponWeights[i];
					bestweapon = i;
				}
			}
			else
			{
				bestweight = bs->botWeaponWeights[i];
				bestweapon = i;
			}
		}

		i++;
	}

	if ( bs->currentEnemy && bs->frame_Enemy_Len < 300 &&
		(bestweapon == WP_BRYAR_PISTOL || bestweapon == WP_BLASTER || bestweapon == WP_BOWCASTER) &&
		(bs->cur_ps.stats[STAT_WEAPONS] & (1 << WP_SABER)) )
	{
		bestweapon = WP_SABER;
		bestweight = 1;
	}

	if ( bs->currentEnemy && bs->frame_Enemy_Len > 300 &&
		bs->currentEnemy->client && bs->currentEnemy->client->ps.weapon != WP_SABER &&
		(bestweapon == WP_SABER) )
	{ //if the enemy is far away, and we have our saber selected, see if we have any good distance weapons instead
		if (BotWeaponSelectable(bs, WP_DISRUPTOR))
		{
			bestweapon = WP_DISRUPTOR;
			bestweight = 1;
		}
		else if (BotWeaponSelectable(bs, WP_ROCKET_LAUNCHER))
		{
			bestweapon = WP_ROCKET_LAUNCHER;
			bestweight = 1;
		}
		else if (BotWeaponSelectable(bs, WP_BOWCASTER))
		{
			bestweapon = WP_BOWCASTER;
			bestweight = 1;
		}
		else if (BotWeaponSelectable(bs, WP_BLASTER))
		{
			bestweapon = WP_BLASTER;
			bestweight = 1;
		}
		else if (BotWeaponSelectable(bs, WP_REPEATER))
		{
			bestweapon = WP_REPEATER;
			bestweight = 1;
		}
		else if (BotWeaponSelectable(bs, WP_DEMP2))
		{
			bestweapon = WP_DEMP2;
			bestweight = 1;
		}
	}

	if (bestweight != -1 && bs->cur_ps.weapon != bestweapon && bs->virtualWeapon != bestweapon)
	{
		bs->virtualWeapon = bestweapon;
		BotSelectWeapon(bs->client, bestweapon);
		//bs->cur_ps.weapon = bestweapon;
		//level.clients[bs->client].ps.weapon = bestweapon;
		return 1;
	}

	return 0;
}

int BotSelectChoiceWeapon(bot_state_t *bs, int weapon, int doselection)
{ //if !doselection then bot will only check if he has the specified weapon and return 1 (yes) or 0 (no)
	int i;
	int hasit = 0;

	i = 0;

	while (i < WP_NUM_WEAPONS)
	{
		if (bs->cur_ps.ammo[weaponData[i].ammoIndex] > weaponData[i].energyPerShot &&
			i == weapon &&
			(bs->cur_ps.stats[STAT_WEAPONS] & (1 << i)))
		{
			hasit = 1;
			break;
		}

		i++;
	}

	if (hasit && bs->cur_ps.weapon != weapon && doselection && bs->virtualWeapon != weapon)
	{
		bs->virtualWeapon = weapon;
		BotSelectWeapon(bs->client, weapon);
		//bs->cur_ps.weapon = weapon;
		//level.clients[bs->client].ps.weapon = weapon;
		return 2;
	}

	if (hasit)
	{
		return 1;
	}

	return 0;
}

int BotSelectMelee(bot_state_t *bs)
{
	if (bs->cur_ps.weapon != 1 && bs->virtualWeapon != 1)
	{
		bs->virtualWeapon = 1;
		BotSelectWeapon(bs->client, 1);
		//bs->cur_ps.weapon = 1;
		//level.clients[bs->client].ps.weapon = 1;
		return 1;
	}

	return 0;
}

int GetLoveLevel(bot_state_t *bs, bot_state_t *love)
{
	int i = 0;
	const char *lname = NULL;

	if (g_gametype.integer == GT_TOURNAMENT)
	{ //There is no love in 1-on-1
		return 0;
	}

	if (!bs || !love || !g_entities[love->client].client)
	{
		return 0;
	}

	if (!bs->lovednum)
	{
		return 0;
	}

	trap_Cvar_Update(&bot_attachments);

	if (!bot_attachments.integer)
	{
		return 1;
	}

	lname = g_entities[love->client].client->pers.netname;

	if (!lname)
	{
		return 0;
	}

	while (i < bs->lovednum)
	{
		if (strcmp(bs->loved[i].name, lname) == 0)
		{
			return bs->loved[i].level;
		}

		i++;
	}

	return 0;
}

void BotLovedOneDied(bot_state_t *bs, bot_state_t *loved, int lovelevel)
{
	if (!loved->lastHurt || !loved->lastHurt->client ||
		loved->lastHurt->s.number == loved->client)
	{
		return;
	}

	if (g_gametype.integer == GT_TOURNAMENT)
	{ //There is no love in 1-on-1
		return;
	}

	if (!IsTeamplay())
	{
		if (lovelevel < 2)
		{
			return;
		}
	}
	else if (OnSameTeam(&g_entities[bs->client], loved->lastHurt))
	{ //don't hate teammates no matter what
		return;
	}

	if (loved->client == loved->lastHurt->s.number)
	{
		return;
	}

	if (bs->client == loved->lastHurt->s.number)
	{ //oops!
		return;
	}
	
	trap_Cvar_Update(&bot_attachments);

	if (!bot_attachments.integer)
	{
		return;
	}

	if (!PassLovedOneCheck(bs, loved->lastHurt))
	{ //a loved one killed a loved one.. you cannot hate them
		bs->chatObject = loved->lastHurt;
		bs->chatAltObject = &g_entities[loved->client];
		BotDoChat(bs, "LovedOneKilledLovedOne", 0);
		return;
	}

	if (bs->revengeEnemy == loved->lastHurt)
	{
		if (bs->revengeHateLevel < bs->loved_death_thresh)
		{
			bs->revengeHateLevel++;

			if (bs->revengeHateLevel == bs->loved_death_thresh)
			{
				//broke into the highest anger level
				//CHAT: Hatred section
				bs->chatObject = loved->lastHurt;
				bs->chatAltObject = NULL;
				BotDoChat(bs, "Hatred", 1);
			}
		}
	}
	else if (bs->revengeHateLevel < bs->loved_death_thresh-1)
	{ //only switch hatred if we don't hate the existing revenge-enemy too much
		//CHAT: BelovedKilled section
		bs->chatObject = &g_entities[loved->client];
		bs->chatAltObject = loved->lastHurt;
		BotDoChat(bs, "BelovedKilled", 0);
		bs->revengeHateLevel = 0;
		bs->revengeEnemy = loved->lastHurt;
	}
}

void BotDeathNotify(bot_state_t *bs)
{ //in case someone has an emotional attachment to us, we'll notify them
	int i = 0;
	int ltest = 0;

	while (i < MAX_CLIENTS)
	{
		if (botstates[i] && botstates[i]->lovednum)
		{
			ltest = 0;
			while (ltest < botstates[i]->lovednum)
			{
				if (strcmp(level.clients[bs->client].pers.netname, botstates[i]->loved[ltest].name) == 0)
				{
					BotLovedOneDied(botstates[i], bs, botstates[i]->loved[ltest].level);
					break;
				}

				ltest++;
			}
		}

		i++;
	}
}

void StrafeTracing(bot_state_t *bs)
{
	vec3_t mins, maxs;
	vec3_t right, rorg, drorg;
	trace_t tr;

	mins[0] = -15;
	mins[1] = -15;
	//mins[2] = -24;
	mins[2] = -22;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 32;

	AngleVectors(bs->viewangles, NULL, right, NULL);

	if (bs->meleeStrafeDir)
	{
		rorg[0] = bs->origin[0] - right[0]*32;
		rorg[1] = bs->origin[1] - right[1]*32;
		rorg[2] = bs->origin[2] - right[2]*32;
	}
	else
	{
		rorg[0] = bs->origin[0] + right[0]*32;
		rorg[1] = bs->origin[1] + right[1]*32;
		rorg[2] = bs->origin[2] + right[2]*32;
	}

	trap_Trace(&tr, bs->origin, mins, maxs, rorg, bs->client, MASK_SOLID);

	if (tr.fraction != 1)
	{
		bs->meleeStrafeDisable = level.time + Q_irand(500, 1500);
	}

	VectorCopy(rorg, drorg);

	drorg[2] -= 32;

	trap_Trace(&tr, rorg, NULL, NULL, drorg, bs->client, MASK_SOLID);

	if (tr.fraction == 1)
	{ //this may be a dangerous ledge, so don't strafe over it just in case
		bs->meleeStrafeDisable = level.time + Q_irand(500, 1500);
	}
}

int PrimFiring(bot_state_t *bs)
{
	if (bs->cur_ps.weaponstate != WEAPON_CHARGING &&
		bs->doAttack)
	{
		return 1;
	}

	if (bs->cur_ps.weaponstate == WEAPON_CHARGING &&
		!bs->doAttack)
	{
		return 1;
	}

	return 0;
}

int KeepPrimFromFiring(bot_state_t *bs)
{
	if (bs->cur_ps.weaponstate != WEAPON_CHARGING &&
		bs->doAttack)
	{
		bs->doAttack = 0;
	}

	if (bs->cur_ps.weaponstate == WEAPON_CHARGING &&
		!bs->doAttack)
	{
		bs->doAttack = 1;
	}

	return 0;
}

int AltFiring(bot_state_t *bs)
{
	if (bs->cur_ps.weaponstate != WEAPON_CHARGING_ALT &&
		bs->doAltAttack)
	{
		return 1;
	}

	if (bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT &&
		!bs->doAltAttack)
	{
		return 1;
	}

	return 0;
}

int KeepAltFromFiring(bot_state_t *bs)
{
	if (bs->cur_ps.weaponstate != WEAPON_CHARGING_ALT &&
		bs->doAltAttack)
	{
		bs->doAltAttack = 0;
	}

	if (bs->cur_ps.weaponstate == WEAPON_CHARGING_ALT &&
		!bs->doAltAttack)
	{
		bs->doAltAttack = 1;
	}

	return 0;
}

gentity_t *CheckForFriendInLOF(bot_state_t *bs)
{
	vec3_t fwd;
	vec3_t trfrom, trto;
	vec3_t mins, maxs;
	gentity_t *trent;
	trace_t tr;

	mins[0] = -3;
	mins[1] = -3;
	mins[2] = -3;

	maxs[0] = 3;
	maxs[1] = 3;
	maxs[2] = 3;

	AngleVectors(bs->viewangles, fwd, NULL, NULL);

	VectorCopy(bs->eye, trfrom);

	trto[0] = trfrom[0] + fwd[0]*2048;
	trto[1] = trfrom[1] + fwd[1]*2048;
	trto[2] = trfrom[2] + fwd[2]*2048;

	trap_Trace(&tr, trfrom, mins, maxs, trto, bs->client, MASK_PLAYERSOLID);

	if (tr.fraction != 1 && tr.entityNum < MAX_CLIENTS)
	{
		trent = &g_entities[tr.entityNum];

		if (trent && trent->client)
		{
			if (IsTeamplay() && OnSameTeam(&g_entities[bs->client], trent))
			{
				return trent;
			}

			if (botstates[trent->s.number] && GetLoveLevel(bs, botstates[trent->s.number]) > 1)
			{
				return trent;
			}
		}
	}

	return NULL;
}

void BotScanForLeader(bot_state_t *bs)
{ //bots will only automatically obtain a leader if it's another bot using this method.
	int i = 0;
	gentity_t *ent;

	if (bs->isSquadLeader)
	{
		return;
	}

	while (i < MAX_CLIENTS)
	{
		ent = &g_entities[i];

		if (ent && ent->client && botstates[i] && botstates[i]->isSquadLeader && bs->client != i)
		{
			if (OnSameTeam(&g_entities[bs->client], ent))
			{
				bs->squadLeader = ent;
				break;
			}
			if (GetLoveLevel(bs, botstates[i]) > 1 && !IsTeamplay())
			{ //ignore love status regarding squad leaders if we're in teamplay
				bs->squadLeader = ent;
				break;
			}
		}

		i++;
	}
}

void BotReplyGreetings(bot_state_t *bs)
{
	int i = 0;
	int numhello = 0;

	while (i < MAX_CLIENTS)
	{
		if (botstates[i] &&
			botstates[i]->canChat &&
			i != bs->client)
		{
			botstates[i]->chatObject = &g_entities[bs->client];
			botstates[i]->chatAltObject = NULL;
			if (BotDoChat(botstates[i], "ResponseGreetings", 0))
			{
				numhello++;
			}
		}

		if (numhello > 3)
		{ //don't let more than 4 bots say hello at once
			return;
		}

		i++;
	}
}

void CTFFlagMovement(bot_state_t *bs)
{
	int diddrop = 0;
	gentity_t *desiredDrop = NULL;
	vec3_t a, mins, maxs;
	trace_t tr;

	mins[0] = -15;
	mins[1] = -15;
	mins[2] = -7;
	maxs[0] = 15;
	maxs[1] = 15;
	maxs[2] = 7;

	if (bs->wantFlag && (bs->wantFlag->flags & FL_DROPPED_ITEM))
	{
		if (bs->staticFlagSpot[0] == bs->wantFlag->s.pos.trBase[0] &&
			bs->staticFlagSpot[1] == bs->wantFlag->s.pos.trBase[1] &&
			bs->staticFlagSpot[2] == bs->wantFlag->s.pos.trBase[2])
		{
			VectorSubtract(bs->origin, bs->wantFlag->s.pos.trBase, a);

			if (VectorLength(a) <= BOT_FLAG_GET_DISTANCE)
			{
				VectorCopy(bs->wantFlag->s.pos.trBase, bs->goalPosition);
				return;
			}
			else
			{
				bs->wantFlag = NULL;
			}
		}
		else
		{
			bs->wantFlag = NULL;
		}
	}
	else if (bs->wantFlag)
	{
		bs->wantFlag = NULL;
	}

	if (flagRed && flagBlue)
	{
		if (bs->wpDestination == flagRed ||
			bs->wpDestination == flagBlue)
		{
			if (bs->wpDestination == flagRed && droppedRedFlag && (droppedRedFlag->flags & FL_DROPPED_ITEM) && droppedRedFlag->classname && strcmp(droppedRedFlag->classname, "freed") != 0)
			{
				desiredDrop = droppedRedFlag;
				diddrop = 1;
			}
			if (bs->wpDestination == flagBlue && droppedBlueFlag && (droppedBlueFlag->flags & FL_DROPPED_ITEM) && droppedBlueFlag->classname && strcmp(droppedBlueFlag->classname, "freed") != 0)
			{
				desiredDrop = droppedBlueFlag;
				diddrop = 1;
			}

			if (diddrop && desiredDrop)
			{
				VectorSubtract(bs->origin, desiredDrop->s.pos.trBase, a);

				if (VectorLength(a) <= BOT_FLAG_GET_DISTANCE)
				{
					trap_Trace(&tr, bs->origin, mins, maxs, desiredDrop->s.pos.trBase, bs->client, MASK_SOLID);

					if (tr.fraction == 1 || tr.entityNum == desiredDrop->s.number)
					{
						VectorCopy(desiredDrop->s.pos.trBase, bs->goalPosition);
						VectorCopy(desiredDrop->s.pos.trBase, bs->staticFlagSpot);
						return;
					}
				}
			}
		}
	}
}

void BotCheckDetPacks(bot_state_t *bs)
{
	gentity_t *dp = NULL;
	gentity_t *myDet = NULL;
	vec3_t a;
	float enLen;
	float myLen;

	while ( (dp = G_Find( dp, FOFS(classname), "detpack") ) != NULL )
	{
		if (dp && dp->parent && dp->parent->s.number == bs->client)
		{
			myDet = dp;
			break;
		}
	}

	if (!myDet)
	{
		return;
	}

	if (!bs->currentEnemy || !bs->currentEnemy->client || !bs->frame_Enemy_Vis)
	{ //require the enemy to be visilbe just to be fair..

		//unless..
		if (bs->currentEnemy && bs->currentEnemy->client &&
			(level.time - bs->plantContinue) < 5000)
		{ //it's a fresh plant (within 5 seconds) so we should be able to guess
			goto stillmadeit;
		}
		return;
	}

stillmadeit:

	VectorSubtract(bs->currentEnemy->client->ps.origin, myDet->s.pos.trBase, a);
	enLen = VectorLength(a);

	VectorSubtract(bs->origin, myDet->s.pos.trBase, a);
	myLen = VectorLength(a);

	if (enLen > myLen)
	{
		return;
	}

	if (enLen < BOT_PLANT_BLOW_DISTANCE && OrgVisible(bs->currentEnemy->client->ps.origin, myDet->s.pos.trBase, bs->currentEnemy->s.number))
	{ //we could just call the "blow all my detpacks" function here, but I guess that's cheating.
		bs->plantKillEmAll = level.time + 500;
	}
}

int BotUseInventoryItem(bot_state_t *bs)
{
	if (bs->cur_ps.stats[STAT_HOLDABLE_ITEMS] & (1 << HI_MEDPAC))
	{
		if (g_entities[bs->client].health <= 50)
		{
			bs->cur_ps.stats[STAT_HOLDABLE_ITEM] = BG_GetItemIndexByTag(HI_MEDPAC, IT_HOLDABLE);
			goto wantuseitem;
		}
	}
	if (bs->cur_ps.stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SEEKER))
	{
		if (bs->currentEnemy && bs->frame_Enemy_Vis)
		{
			bs->cur_ps.stats[STAT_HOLDABLE_ITEM] = BG_GetItemIndexByTag(HI_SEEKER, IT_HOLDABLE);
			goto wantuseitem;
		}
	}
	if (bs->cur_ps.stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SENTRY_GUN))
	{
		if (bs->currentEnemy && bs->frame_Enemy_Vis)
		{
			bs->cur_ps.stats[STAT_HOLDABLE_ITEM] = BG_GetItemIndexByTag(HI_SENTRY_GUN, IT_HOLDABLE);
			goto wantuseitem;
		}
	}
	if (bs->cur_ps.stats[STAT_HOLDABLE_ITEMS] & (1 << HI_SHIELD))
	{
		if (bs->currentEnemy && bs->frame_Enemy_Vis && bs->runningToEscapeThreat)
		{ //this will (hopefully) result in the bot placing the shield down while facing
		  //the enemy and running away
			bs->cur_ps.stats[STAT_HOLDABLE_ITEM] = BG_GetItemIndexByTag(HI_SHIELD, IT_HOLDABLE);
			goto wantuseitem;
		}
	}

	return 0;

wantuseitem:
	level.clients[bs->client].ps.stats[STAT_HOLDABLE_ITEM] = bs->cur_ps.stats[STAT_HOLDABLE_ITEM];

	return 1;
}

int BotSurfaceNear(bot_state_t *bs)
{
	trace_t tr;
	vec3_t fwd;

	AngleVectors(bs->viewangles, fwd, NULL, NULL);

	fwd[0] = bs->origin[0]+(fwd[0]*64);
	fwd[1] = bs->origin[1]+(fwd[1]*64);
	fwd[2] = bs->origin[2]+(fwd[2]*64);

	trap_Trace(&tr, bs->origin, NULL, NULL, fwd, bs->client, MASK_SOLID);

	if (tr.fraction != 1)
	{
		return 1;
	}

	return 0;
}

int BotWeaponBlockable(int weapon)
{
	switch (weapon)
	{
	case WP_STUN_BATON:
		return 0;
	case WP_DISRUPTOR:
		return 0;
	case WP_DEMP2:
		return 0;
	case WP_ROCKET_LAUNCHER:
		return 0;
	case WP_THERMAL:
		return 0;
	case WP_TRIP_MINE:
		return 0;
	case WP_DET_PACK:
		return 0;
	default:
		return 1;
	}
}

void Cmd_EngageDuel_f(gentity_t *ent);
void Cmd_ToggleSaber_f(gentity_t *ent);

static void UseInventoryItem(bot_state_t* bs)
{
	if (BotUseInventoryItem(bs))
	{
		if (rand() % 10 < 5)
		{
			trap_EA_Use(bs->client);
		}
	}
}

// PowTecH - Start Point
static void RespawnResetState(bot_state_t* bs) {
	// BotResetState(...) - might want to use this?
	bs->wpCurrent = NULL;
	bs->currentEnemy = NULL;
	bs->wpDestination = NULL;
	bs->wpCamping = NULL;
	bs->wpCampingTo = NULL;
	bs->wpStoreDest = NULL;

	bs->wpDestIgnoreTime = 0;
	bs->wpDestSwitchTime = 0;
	bs->wpSeenTime = 0;
	bs->wpTravelTime = 0;
	bs->wpDirection = 0;
	bs->wpToGoalCount = 0;
	bs->currentPathWaypointIndex = 0;

	// Reset the attack states
	bs->doAttack = qfalse;
	bs->doAltAttack = qfalse;

	// Reset other combat/movement states
	bs->frame_Enemy_Vis = 0;
	bs->frame_Enemy_Len = 0;
	VectorClear(bs->goalMovedir);
	// bs->goalPosition might be set by spawn point logic, or cleared if needed: VectorClear(bs->goalPosition);

	// Set default weapon (e.g., saber)
	bs->virtualWeapon = WP_SABER;
	BotSelectWeapon(bs->client, WP_SABER);
}

static qboolean Respawn(bot_state_t* bs) {
	gentity_t* bot_ent = &g_entities[bs->client];

	// Case 1: Initial spawn into the game (or first time this function is called for the bot)
	if (!bs->lastDeadTime) {
		bs->lastDeadTime = level.time;
		RespawnResetState(bs);
		bs->deathActivitiesDone = qfalse;
		return qtrue; // spawn
	}

	// Case 2: Bot has died (health is less than 1) and needs to respawn
	if (bot_ent->client->ps.stats[STAT_HEALTH] <= 0) {
		if (bot_ent->client->respawnTime &&
			bot_ent->client->respawnTime > 0) {
			if (level.time < bot_ent->client->respawnTime) {
				return qtrue; // respawning
			}
			else if (bs->deathActivitiesDone && level.time >= bot_ent->client->respawnTime) {
				if (level.zCurrentTickets > 0) {
					level.zCurrentTickets--;
					respawn(bot_ent);
				}
				else {
					SetTeam(bot_ent, "spectator");
				}

				UpdateGameState();

				return qtrue; // respawned
			}
		}

		if (!bs->deathActivitiesDone) {
			bs->deathActivitiesDone = qtrue;
		}
		else {
			return qtrue;
		}

		// Reset state for the new life (after "respawn" which is usually handled by engine)
		// This function is more about reacting to the death and preparing for the next spawn event.
		// The actual respawn (health restored, moved to spawn point) is typically engine-driven.
		// We set lastDeadTime here to indicate the bot is currently in a "dead" state awaiting engine respawn.
		bs->lastDeadTime = level.time;
		RespawnResetState(bs); // Reset state for the upcoming new life
		// deathActivitiesDone will be reset to qfalse when the bot's health is > 0 again (next spawn)
		// OR, if the engine respawns immediately and this function is called again,
		// the !bs->lastDeadTime logic might not trigger as expected if health > 0.
		// A common pattern is to check if health > 0 AND lastDeadTime indicates a recent death.

		bot_ent->client->respawnTime = level.time + (5 * 1000);
		return qtrue;
	}

	// If bot was dead and is now alive (engine respawned it)
	if (bs->lastDeadTime != 0 && bot_ent->client->ps.stats[STAT_HEALTH] > 0 && bs->deathActivitiesDone) {
		// Bot was dead (deathActivitiesDone was true), and now is alive.
		// This signifies a fresh spawn after a death.
		RespawnResetState(bs);       // Reset state for the new life
		bs->deathActivitiesDone = qfalse; // Ready for new death activities if it dies again
		bs->lastDeadTime = level.time; // Update last "death" time to effectively mean "last spawn time" now
		// or could be set to 0 to indicate "fully alive, not recently dead"
		// For simplicity, let's keep it as current time, meaning "just spawned".
		return qtrue; // Bot just effectively respawned into a new life
	}

	return qfalse; // Bot is alive and not in a spawn/respawn transition
}

static gentity_t* BotFindBestAvailableEnemy(bot_state_t* bs) {
	gentity_t* ent = NULL;
	gentity_t* bestEnemy = NULL;
	int i;
	gentity_t* self_ent = &g_entities[bs->client];

	for (i = 0; i < MAX_CLIENTS; i++) {
		ent = &g_entities[i];
		if (IS_VALID_ENEMY(ent) &&
			ent->s.number != bs->client &&
			!OnSameTeam(ent, self_ent))
		{
			bestEnemy = ent;
			break;
		}
	}
	return bestEnemy;
}

static void SetEnemyTarget(bot_state_t* bs) {
	if (!IS_VALID_ENEMY(bs->currentEnemy)) {
		gentity_t* newEnemy = BotFindBestAvailableEnemy(bs);
		if (newEnemy != bs->currentEnemy || !bs->currentEnemy) {
			bs->currentEnemy = newEnemy;
			bs->wpToGoalCount = 0;
			bs->currentPathWaypointIndex = 0;
			bs->wpTravelTime = 0;

			if (bs->currentEnemy) {
				bs->frame_Enemy_Vis = 0;
				bs->lastVisibleEnemyIndex = ENTITYNUM_NONE;
			}
		}
	}
}

static wpobject_t* GetWaypointByIndex(int index) {
	if (index < 0 || index >= MAX_WPARRAY_SIZE) {
		return NULL;
	}

	if (!gWPArray[index] || !gWPArray[index]->inuse) {
		return NULL;
	}
	return gWPArray[index];
}

static int GetNearestWaypoint(vec3_t origin, float max_search_dist) {
	int best_wp_index = -1;
	float best_dist_sq = max_search_dist * max_search_dist;
	int i;
	wpobject_t* wp;

	for (i = 0; i < MAX_WPARRAY_SIZE; i++) {
		wp = GetWaypointByIndex(i);
		if (wp) {
			float dist_sq = DistanceSquared(origin, wp->origin);
			if (dist_sq < best_dist_sq) {
				best_dist_sq = dist_sq;
				best_wp_index = i;
			}
		}
	}
	return best_wp_index;
}

static float CalculateHeuristic(int waypointIndexA, int waypointIndexB) {
	wpobject_t* wpA = GetWaypointByIndex(waypointIndexA);
	wpobject_t* wpB = GetWaypointByIndex(waypointIndexB);
	vec3_t diff;

	if (!wpA || !wpB) {
		return ASTAR_INFINITE_COST;
	}
	VectorSubtract(wpB->origin, wpA->origin, diff);
	return VectorLength(diff);
}

static qboolean BotPathfindAStar(bot_state_t* bs, int startWaypointIndex, int goalWaypointIndex) {
	int i;
	wpobject_t* startNode;
	wpobject_t* goalNode;
	wpobject_t* currentNodeObj;
	wpobject_t* neighborNodeObj;
	int currentWaypointIndex;
	float lowestFCost;
	int openSetNodePos;
	wpconnection_t* neighborConn;
	int neighborIndex;
	float tentativeGCost;
	static int tempPath[MAX_WPARRAY_SIZE];
	int pathIndex;
	int curr;


	startNode = GetWaypointByIndex(startWaypointIndex);
	goalNode = GetWaypointByIndex(goalWaypointIndex);

	if (!startNode || !goalNode) {
		bs->wpToGoalCount = 0;
		return qfalse;
	}

	// 1. Initialize A* data for all waypoints
	for (i = 0; i < MAX_WPARRAY_SIZE; i++) {
		astar_node_workspace[i].gCost = ASTAR_INFINITE_COST;
		astar_node_workspace[i].fCost = ASTAR_INFINITE_COST;
		astar_node_workspace[i].hCost = ASTAR_INFINITE_COST;
		astar_node_workspace[i].parentWaypointIndex = -1;
		astar_node_workspace[i].inOpenSet = qfalse;
		astar_node_workspace[i].inClosedSet = qfalse;
	}

	openSetCount = 0;

	// 2. Initialize start node
	astar_node_workspace[startWaypointIndex].gCost = 0;
	astar_node_workspace[startWaypointIndex].hCost = CalculateHeuristic(startWaypointIndex, goalWaypointIndex);
	astar_node_workspace[startWaypointIndex].fCost = astar_node_workspace[startWaypointIndex].hCost;
	openSet[openSetCount++] = startWaypointIndex;
	astar_node_workspace[startWaypointIndex].inOpenSet = qtrue;

	// 3. Main A* loop
	while (openSetCount > 0) {
		// Initialize for this iteration (C89: declarations at top of block)
		currentWaypointIndex = -1;
		lowestFCost = ASTAR_INFINITE_COST;
		openSetNodePos = -1;

		for (i = 0; i < openSetCount; i++) {
			int nodeIndex = openSet[i];
			if (astar_node_workspace[nodeIndex].fCost < lowestFCost) {
				lowestFCost = astar_node_workspace[nodeIndex].fCost;
				currentWaypointIndex = nodeIndex;
				openSetNodePos = i;
			}
		}

		if (currentWaypointIndex == -1) {
			bs->wpToGoalCount = 0;
			return qfalse;
		}

		// If current is the goal, path found
		if (currentWaypointIndex == goalWaypointIndex) {
			// Reconstruct path
			pathIndex = 0; // Initialize pathIndex
			curr = goalWaypointIndex; // Initialize curr
			while (curr != -1 && pathIndex < MAX_WPARRAY_SIZE) {
				tempPath[pathIndex++] = curr;
				curr = astar_node_workspace[curr].parentWaypointIndex;
			}

			// Reverse path and store in bs->wpToGoal
			bs->wpToGoalCount = 0;
			for (i = pathIndex - 1; i >= 0; i--) {
				if (bs->wpToGoalCount < MAX_WPARRAY_SIZE) {
					bs->wpToGoal[bs->wpToGoalCount++] = tempPath[i];
				}
				else {
					break;
				}
			}
			bs->currentPathWaypointIndex = 0;
			return qtrue;
		}

		// Move current node from open set to closed set
		openSet[openSetNodePos] = openSet[--openSetCount];
		astar_node_workspace[currentWaypointIndex].inOpenSet = qfalse;
		astar_node_workspace[currentWaypointIndex].inClosedSet = qtrue;

		// Process neighbors
		currentNodeObj = GetWaypointByIndex(currentWaypointIndex);
		if (!currentNodeObj) continue;

		for (i = 0; i < currentNodeObj->connectionsCount; i++) {
			neighborConn = &currentNodeObj->connections[i];
			neighborIndex = neighborConn->index;
			neighborNodeObj = GetWaypointByIndex(neighborIndex);

			if (!neighborNodeObj || !neighborNodeObj->inuse) continue;
			if (astar_node_workspace[neighborIndex].inClosedSet) continue;

			tentativeGCost = astar_node_workspace[currentWaypointIndex].gCost + neighborConn->distance;

			if (tentativeGCost < astar_node_workspace[neighborIndex].gCost) {
				astar_node_workspace[neighborIndex].parentWaypointIndex = currentWaypointIndex;
				astar_node_workspace[neighborIndex].gCost = tentativeGCost;
				astar_node_workspace[neighborIndex].hCost = CalculateHeuristic(neighborIndex, goalWaypointIndex);
				astar_node_workspace[neighborIndex].fCost = astar_node_workspace[neighborIndex].gCost + astar_node_workspace[neighborIndex].hCost;

				if (!astar_node_workspace[neighborIndex].inOpenSet) {
					if (openSetCount < MAX_WPARRAY_SIZE) {
						openSet[openSetCount++] = neighborIndex;
						astar_node_workspace[neighborIndex].inOpenSet = qtrue;
					}
				}
			}
		}
	}

	bs->wpToGoalCount = 0;
	return qfalse;
}

static void Movement(bot_state_t* bs) {
	wpobject_t* nextWp = NULL;
	wpobject_t* newNextWp = NULL;
	int startWpIndex;
	int goalWpIndex;

	if (bs->beStill > level.time) {
		trap_EA_Move(bs->client, vec3_origin, 0); // Explicitly stop movement
		VectorClear(bs->goalMovedir);
		return;
	}

	// --- Path Recalculation Logic ---
	if (IS_VALID_ENEMY(bs->currentEnemy)) {
		qboolean needsPathRecalc = qfalse;
		if (bs->wpToGoalCount == 0) {
			needsPathRecalc = qtrue;
		}
		else {
			if (bs->currentPathWaypointIndex < bs->wpToGoalCount &&
				bs->wpTravelTime != 0 &&
				level.time > bs->wpTravelTime) {
				needsPathRecalc = qtrue;
			} else if  (bs->frame_Enemy_Len > 256 && !bs->frame_Enemy_Vis) {
				wpobject_t* lastPathWp = GetWaypointByIndex(bs->wpToGoal[bs->wpToGoalCount - 1]);
				if (!lastPathWp || DistanceSquared(lastPathWp->origin, bs->currentEnemy->r.currentOrigin) > 400 * 400) {
					needsPathRecalc = qtrue;
				}
			}
		}

		if (needsPathRecalc) {
			bs->wpToGoalCount = 0;
			bs->currentPathWaypointIndex = 0;
			bs->wpTravelTime = 0;

			startWpIndex = GetNearestWaypoint(bs->origin, 200.0f);
			goalWpIndex = GetNearestWaypoint(bs->currentEnemy->r.currentOrigin, 200.0f);

			if (startWpIndex != -1 && goalWpIndex != -1 && startWpIndex != goalWpIndex) {
				if (BotPathfindAStar(bs, startWpIndex, goalWpIndex)) {
					if (bs->wpToGoalCount > 0) {
						wpobject_t* firstWpInPath = GetWaypointByIndex(bs->wpToGoal[0]);
						if (firstWpInPath) {
							VectorCopy(firstWpInPath->origin, bs->goalPosition);
							bs->wpTravelTime = level.time + BOT_WAYPOINT_TRAVEL_TIMEOUT;
						}
						else {
							bs->wpToGoalCount = 0;
						}
					}
				}
			}
			else {
				bs->wpToGoalCount = 0;
			}
		}
	}
	else {
		bs->wpToGoalCount = 0;
		bs->currentPathWaypointIndex = 0;
		bs->wpTravelTime = 0;
	}

	// --- Path Following Logic ---
	if (bs->wpToGoalCount > 0 && bs->currentPathWaypointIndex < bs->wpToGoalCount) {
		nextWp = GetWaypointByIndex(bs->wpToGoal[bs->currentPathWaypointIndex]);

		if (nextWp) {
			VectorCopy(nextWp->origin, bs->goalPosition);

			if (DistanceSquared(bs->origin, bs->goalPosition) < 64 * 64) {
				bs->currentPathWaypointIndex++;

				if (bs->currentPathWaypointIndex >= bs->wpToGoalCount) {
					bs->wpToGoalCount = 0;
					bs->wpTravelTime = 0;

					if (VectorCompare(bs->goalPosition, bs->origin)) {
						trap_EA_Jump(bs->client);
					}

					VectorClear(bs->goalMovedir);

					return;
				}

				newNextWp = GetWaypointByIndex(bs->wpToGoal[bs->currentPathWaypointIndex]);
				if (newNextWp) {
					VectorCopy(newNextWp->origin, bs->goalPosition);
					bs->wpTravelTime = level.time + BOT_WAYPOINT_TRAVEL_TIMEOUT; // Set travel time for the new segment
				}
				else { // Invalid next waypoint in path
					bs->wpToGoalCount = 0; 
					bs->wpTravelTime = 0; 
					VectorClear(bs->goalMovedir); 
					
					return;
				}
			}
		}
		else {
			bs->wpToGoalCount = 0;
			bs->currentPathWaypointIndex = 0;
			bs->wpTravelTime = 0;
		}
	}
	else if (IS_VALID_ENEMY(bs->currentEnemy) && bs->frame_Enemy_Vis && bs->frame_Enemy_Len < 1024) {
		VectorCopy(bs->currentEnemy->r.currentOrigin, bs->goalPosition);
		bs->wpTravelTime = 0;
	}
	else {
		VectorClear(bs->goalMovedir);
		bs->wpTravelTime = 0;
		return;
	}

	// Calculate direction to the current goalPosition
	if (VectorCompare(bs->goalPosition, bs->origin)) {
		VectorClear(bs->goalMovedir);
	}
	else {
		VectorSubtract(bs->goalPosition, bs->origin, bs->goalMovedir);
		VectorNormalize(bs->goalMovedir);
	}

	// Issue the move command if there's a direction
	if (VectorLengthSquared(bs->goalMovedir) > 0.1f) {
		//trap_EA_Move(bs->client, bs->goalMovedir, 300);
		trap_EA_Move(bs->client, bs->goalMovedir, 5000);
	}
}

static void AimAtEnemy(bot_state_t* bs, float thinktime) {
	vec3_t enemyHeadPos;                // Stores the calculated head position of the enemy
	vec3_t vecToTarget;                 // Vector from bot's eye to the target
	vec3_t aimAngles;                   // Temporary storage for calculated aim angles
	float leadAmount;
	float skillRatio;
	float distToLastSpot;
	vec3_t enemyMovementSinceSpotted;

	// --- 1. Initial Validation: Current Enemy ---
	// If there's no current enemy, or the enemy is not a client entity or not connected,
	// reset relevant state and exit. SetEnemyTarget should have handled finding an enemy.
	if (!IS_VALID_ENEMY(bs->currentEnemy)) {
		bs->lastVisibleEnemyIndex = ENTITYNUM_NONE;
		bs->frame_Enemy_Vis = 0; // Mark enemy as not visible
		// Optionally clear goal angles if no enemy
		// VectorClear(bs->goalAngles); 
		return;
	}

	// --- 2. Calculate Current Enemy Head Position & Distance ---
	VectorCopy(bs->currentEnemy->client->ps.origin, enemyHeadPos);
	enemyHeadPos[2] += bs->currentEnemy->client->ps.viewheight;
	VectorSubtract(enemyHeadPos, bs->eye, vecToTarget);
	bs->frame_Enemy_Len = VectorLength(vecToTarget);

	// --- 3. Check Current Visibility & Update Bot's Knowledge ---
	if (OrgVisible(bs->eye, enemyHeadPos, bs->client)) { // bs->client is bot's own entity num for passEntities
		bs->frame_Enemy_Vis = 1;
		VectorCopy(enemyHeadPos, bs->lastEnemySpotted);
		VectorCopy(bs->origin, bs->hereWhenSpotted);
		bs->lastVisibleEnemyIndex = bs->currentEnemy->s.number;
		bs->hitSpotted = 0;
		bs->enemySeenTime = level.time + ENEMY_FORGET_MS;
	}
	else {
		bs->frame_Enemy_Vis = 0;
	}

	// --- 4. Reaction Time & Engagement Check ---
	// Check if bot is still in reaction delay or has "forgotten" the enemy.
	// ENEMY_FORGET_MS * 0.8f means 20% of forget time has passed since last seen.
	if (bs->timeToReact >= level.time || bs->enemySeenTime <= (level.time + (ENEMY_FORGET_MS * 0.8f))) {
		// If enemy is not visible, and we are in this state, don't aim at last known spot yet.
		// If enemy IS visible, CombatBotAI was NOT called in original logic at this stage.
		// The bot might be aware but not fully reacting/aiming yet.
		// Consider if goalAngles should be cleared or maintained.
		return;
	}

	// --- 5. Active Combat AI & Aiming (If Enemy is Currently Visible) ---
	if (bs->frame_Enemy_Vis) {
		CombatBotAI(bs, thinktime); // Perform main combat decision-making

		leadAmount = BotWeaponCanLead(bs);
		skillRatio = 0.0f;

		if (bs->settings.skill != 0.0f) { // Prevent division by zero
			skillRatio = bs->skills.accuracy / bs->settings.skill;
		}

		if (leadAmount > 0.0f && skillRatio <= 8.0f) { // Arbitrary skill threshold
			BotAimLeading(bs, enemyHeadPos, leadAmount); // This function should set bs->goalAngles
		}
		else {
			// No leading: aim directly. vecToTarget is already (enemyHeadPos - bs->eye).
			vectoangles(vecToTarget, aimAngles);
			VectorCopy(aimAngles, bs->goalAngles);
		}
		BotAimOffsetGoalAngles(bs); // Apply final aim adjustments

		// PowTecH - Make the bot rotate
		VectorCopy(bs->goalAngles, bs->ideal_viewangles);
		return;
	}

	// --- 6. Aiming at Last Known Position (If Enemy is NOT Currently Visible but remembered) ---
	// This block executes if enemy not currently visible AND bot is past reaction/partial forgetfulness.
	if (OrgVisible(bs->eye, bs->lastEnemySpotted, ENTITYNUM_NONE)) { // Check LOS to last spot
		VectorSubtract(bs->lastEnemySpotted, bs->eye, vecToTarget);
		vectoangles(vecToTarget, aimAngles);
		VectorCopy(aimAngles, bs->goalAngles);

		// Specific logic for Flechette gun when aiming at last known position
		if (bs->cur_ps.weapon == WP_FLECHETTE &&
			bs->cur_ps.weaponstate == WEAPON_READY &&
			IS_VALID_ENEMY(bs->currentEnemy)) // Re-check for safety
		{
			distToLastSpot = VectorLength(vecToTarget);

			if (distToLastSpot > 128.0f && distToLastSpot < 1024.0f) {
				VectorSubtract(bs->currentEnemy->client->ps.origin, bs->lastEnemySpotted, enemyMovementSinceSpotted);
				if (VectorLength(enemyMovementSinceSpotted) < 300.0f) {
					bs->doAltAttack = qtrue;
				}
			}
		}
	}
	else {
		// Last known spot is not visible, don't aim there.
		// Optionally, make the bot look forward or in a search pattern.
		// For now, goalAngles remain as they were (potentially from a previous visible state or cleared).
	}
}

static void AttackEnemy(bot_state_t* bs) {
    gentity_t* bot_entity = &g_entities[bs->client];

    // Ensure currentEnemy and its client are valid before dereferencing.
    // This is a safeguard, though StandardBotAI checks IS_VALID_ENEMY before calling.
    if (!IS_VALID_ENEMY(bs->currentEnemy)) {
        bs->doAttack = qfalse; // Ensure bot doesn't attack if enemy becomes invalid mid-logic
        return;
    }

    // Saber power selection: Randomly switch between strong and normal attacks over time.
    if (bs->saberPowerTime < level.time) {
        if (Q_irand(1, 10) <= 5) {
            bs->saberPower = qtrue;
        } else {
            bs->saberPower = qfalse;
        }
        bs->saberPowerTime = level.time + Q_irand(3000, 15000); // Cooldown for next power switch
    }

    // Select saber attack animation level based on enemy health and bot's force power.
    // Ensure bot_entity and its client are valid before accessing ps.
    if (bot_entity && bot_entity->client) {
        if (bs->currentEnemy->health > 75 && bot_entity->client->ps.fd.forcePowerLevel[FP_SABERATTACK] > 2) {
            if (bot_entity->client->ps.fd.saberAnimLevel != FORCE_LEVEL_3 && bs->saberPower) {
                Cmd_SaberAttackCycle_f(bot_entity); // Switch to strong attack
            }
        } else if (bs->currentEnemy->health > 40 && bot_entity->client->ps.fd.forcePowerLevel[FP_SABERATTACK] > 1) {
            if (bot_entity->client->ps.fd.saberAnimLevel != FORCE_LEVEL_2) {
                Cmd_SaberAttackCycle_f(bot_entity); // Switch to medium attack
            }
        } else {
            if (bot_entity->client->ps.fd.saberAnimLevel != FORCE_LEVEL_1) {
                Cmd_SaberAttackCycle_f(bot_entity); // Switch to quick attack
            }
        }
    }

    // Check if the enemy is within saber attack range.
    if (bs->frame_Enemy_Len <= SABER_ATTACK_RANGE) {
        bs->doAttack = qtrue;
        SaberCombatHandling(bs); // Perform saber-specific combat maneuvers/logic.

        // Original commented-out logic:
        // if(PrimFiring(bs)) { KeepPrimFromFiring(bs); }
        // if (bs->frame_Enemy_Len < 80) { meleestrafe = 1; }
    } else {
        bs->doAttack = qfalse;
    }

    // If all conditions met and doAttack is set, execute the attack.
    if (bs->doAttack) {
        trap_EA_Attack(bs->client);
    }
}

static void StandardBotAI(bot_state_t* bs, float thinktime) {
	// --- 1. Global AI Deactivation Check ---
	// If AI is globally deactivated, clear bot's targets and waypoints and do nothing further.
	if (gDeactivated) {
		bs->wpCurrent = NULL;
		bs->currentEnemy = NULL;
		bs->wpDestination = NULL;
		bs->wpDirection = 0;
		bs->wpToGoalCount = 0;
		bs->wpTravelTime = 0;
		return;
	}

	// --- 2. Spectator Check ---
	// Check if the bot's associated client entity is valid and in spectator mode.
	// If so, clear targets and waypoints and do nothing further.
	// It's important to check g_entities[bs->client].inuse and g_entities[bs->client].client
	// before accessing client->sess.sessionTeam to prevent crashes if the entity is not in use
	// or not a client.
	if (g_entities[bs->client].client->sess.sessionTeam == TEAM_SPECTATOR &&
		level.zCurrentTickets > 0) {
		SetTeam(&g_entities[bs->client], "f");
	}

	if (bs->client >= 0 && bs->client < MAX_CLIENTS &&
		g_entities[bs->client].inuse &&
		g_entities[bs->client].client &&
		g_entities[bs->client].client->sess.sessionTeam == TEAM_SPECTATOR) {
		bs->wpCurrent = NULL;
		bs->currentEnemy = NULL;
		bs->wpDestination = NULL;
		bs->wpDirection = 0;
		bs->wpToGoalCount = 0;
		bs->wpTravelTime = 0;
		return;
	}

	// --- 3. Respawn Check ---
	// If the Respawn function handles the bot's logic for this frame (e.g., bot just respawned),
	// then return and let other logic proceed in a subsequent frame.
	if (Respawn(bs)) {
		bs->wpToGoalCount = 0;
		bs->wpTravelTime = 0;
		return;
	}

	SetEnemyTarget(bs);
	Movement(bs);

	AimAtEnemy(bs, thinktime);
	AttackEnemy(bs);
}
// PowTecH - End Point

/*
==================
BotAIStartFrame
==================
*/
int BotAIStartFrame(int time) {
	int i;
	int elapsed_time, thinktime;
	static int local_time;
	static int lastbotthink_time;

	G_CheckBotSpawn();

	//rww - addl bot frame functions
	if (gBotEdit)
	{
		trap_Cvar_Update(&bot_wp_info);
		BotWaypointRender();
	}

	UpdateEventTracker();
	//end rww

	//cap the bot think time
	//if the bot think time changed we should reschedule the bots
	if (BOT_THINK_TIME != lastbotthink_time) {
		lastbotthink_time = BOT_THINK_TIME;
		BotScheduleBotThink();
	}

	elapsed_time = time - local_time;
	local_time = time;

	if (elapsed_time > BOT_THINK_TIME) thinktime = elapsed_time;
	else thinktime = BOT_THINK_TIME;

	// execute scheduled bot AI
	for( i = 0; i < MAX_CLIENTS; i++ ) {
		if( !botstates[i] || !botstates[i]->inuse ) {
			continue;
		}
		//
		botstates[i]->botthink_residual += elapsed_time;
		//
		if ( botstates[i]->botthink_residual >= thinktime ) {
			botstates[i]->botthink_residual -= thinktime;

			if (g_entities[i].client->pers.connected == CON_CONNECTED) {
				BotAI(i, (float) thinktime / 1000);
			}
		}
	}

	// execute bot user commands every frame
	for( i = 0; i < MAX_CLIENTS; i++ ) {
		if( !botstates[i] || !botstates[i]->inuse ) {
			continue;
		}
		if( g_entities[i].client->pers.connected != CON_CONNECTED ) {
			continue;
		}

		BotUpdateInput(botstates[i], time, elapsed_time);
		trap_BotUserCommand(botstates[i]->client, &botstates[i]->lastucmd);
	}

	return qtrue;
}

/*
==============
BotAISetup
==============
*/
int BotAISetup( int restart ) {
	//rww - new bot cvars..
	trap_Cvar_Register(&bot_forcepowers, "bot_forcepowers", "1", CVAR_CHEAT);
	trap_Cvar_Register(&bot_forgimmick, "bot_forgimmick", "0", CVAR_CHEAT);
	trap_Cvar_Register(&bot_honorableduelacceptance, "bot_honorableduelacceptance", "0", CVAR_CHEAT);
#ifdef _DEBUG
	trap_Cvar_Register(&bot_nogoals, "bot_nogoals", "0", CVAR_CHEAT);
	trap_Cvar_Register(&bot_debugmessages, "bot_debugmessages", "0", CVAR_CHEAT);
#endif

	trap_Cvar_Register(&bot_attachments, "bot_attachments", "1", 0);
	trap_Cvar_Register(&bot_camp, "bot_camp", "1", 0);

	trap_Cvar_Register(&bot_wp_info, "bot_wp_info", "1", 0);
	trap_Cvar_Register(&bot_wp_edit, "bot_wp_edit", "0", CVAR_CHEAT);
	trap_Cvar_Register(&bot_wp_clearweight, "bot_wp_clearweight", "1", 0);
	trap_Cvar_Register(&bot_wp_distconnect, "bot_wp_distconnect", "1", 0);
	trap_Cvar_Register(&bot_wp_visconnect, "bot_wp_visconnect", "1", 0);

	trap_Cvar_Update(&bot_forcepowers);
	//end rww

	//if the game is restarted for a tournament
	if (restart) {
		return qtrue;
	}

	//initialize the bot states
	memset( botstates, 0, sizeof(botstates) );

	if (!trap_BotLibSetup())
	{
		return qfalse; //wts?!
	}

	return qtrue;
}

/*
==============
BotAIShutdown
==============
*/
int BotAIShutdown( int restart ) {

	int i;

	//if the game is restarted for a tournament
	if ( restart ) {
		//shutdown all the bots in the botlib
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (botstates[i] && botstates[i]->inuse) {
				BotAIShutdownClient(botstates[i]->client, restart);
			}
		}
		//don't shutdown the bot library
	}
	else {
		trap_BotLibShutdown();
	}
	return qtrue;
}

