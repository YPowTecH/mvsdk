#include "bg_saga.h"

#define DEFAULT_FORCEPOWERS		"5-1-000000000000000000"

//#define FORCEJUMP_INSTANTMETHOD 1

#define MAX_CHAT_BUFFER_SIZE 8192
#define MAX_CHAT_LINE_SIZE 128

#define MAX_WPARRAY_SIZE 4096
#define MAX_NEIGHBOR_SIZE 32

// PowTecH - Defines
#define MAX_WP_CONNECTION_SIZE 8
//

#define MAX_NEIGHBOR_LINK_DISTANCE 128
#define MAX_NEIGHBOR_FORCEJUMP_LINK_DISTANCE 400

#define TABLE_BRANCH_DISTANCE 32
#define MAX_NODETABLE_SIZE 4096

#define MAX_LOVED_ONES 4
#define MAX_ATTACHMENT_NAME 64

#define MAX_FORCE_INFO_SIZE 2048

#define WPFLAG_JUMP				0x00000010 //jump when we hit this
#define WPFLAG_DUCK				0x00000020 //duck while moving around here
#define WPFLAG_NOVIS			0x00000400 //go here for a bit even with no visibility
#define WPFLAG_SNIPEORCAMPSTAND	0x00000800 //a good position to snipe or camp - stand
#define WPFLAG_WAITFORFUNC		0x00001000 //wait for a func brushent under this point before moving here
#define WPFLAG_SNIPEORCAMP		0x00002000 //a good position to snipe or camp - crouch
#define WPFLAG_ONEWAY_FWD		0x00004000 //can only go forward on the trial from here (e.g. went over a ledge)
#define WPFLAG_ONEWAY_BACK		0x00008000 //can only go backward on the trail from here
#define WPFLAG_GOALPOINT		0x00010000 //make it a goal to get here.. goal points will be decided by setting "weight" values
#define WPFLAG_RED_FLAG			0x00020000 //red flag
#define WPFLAG_BLUE_FLAG		0x00040000 //blue flag
#define WPFLAG_NOMOVEFUNC		0x00200000 //don't move over if a func is under

#define LEVELFLAG_NOPOINTPREDICTION			1 //don't take waypoint beyond current into account when adjusting path view angles
#define LEVELFLAG_IGNOREINFALLBACK			2 //ignore enemies when in a fallback navigation routine
#define LEVELFLAG_IMUSTNTRUNAWAY			4 //don't be scared

#define WP_KEEP_FLAG_DIST			128

#define BWEAPONRANGE_MELEE			1
#define BWEAPONRANGE_MID			2
#define BWEAPONRANGE_LONG			3
#define BWEAPONRANGE_SABER			4

#define MELEE_ATTACK_RANGE			256
#define SABER_ATTACK_RANGE			128
#define MAX_CHICKENWUSS_TIME		10000 //wait 10 secs between checking which run-away path to take

#define BOT_RUN_HEALTH				40
#define BOT_WPTOUCH_DISTANCE		32
#define ENEMY_FORGET_MS				10000
//if our enemy isn't visible within 10000ms (aprx 10sec) then "forget" about him and treat him like every other threat, but still look for
//more immediate threats while main enemy is not visible

#define BOT_PLANT_DISTANCE			256 //plant if within this radius from the last spotted enemy position
#define BOT_PLANT_INTERVAL			15000 //only plant once per 15 seconds at max
#define BOT_PLANT_BLOW_DISTANCE		256 //blow det packs if enemy is within this radius and I am further away than the enemy

#define BOT_MAX_WEAPON_GATHER_TIME	1000 //spend a max of 1 second after spawn issuing orders to gather weapons before attacking enemy base
#define BOT_MAX_WEAPON_CHASE_TIME	15000 //time to spend gathering the weapon before persuing the enemy base (in case it takes longer than expected)

#define BOT_MAX_WEAPON_CHASE_CTF	5000 //time to spend gathering the weapon before persuing the enemy base (in case it takes longer than expected) [ctf-only]

#define BOT_MIN_SAGA_GOAL_SHOOT		1024
#define BOT_MIN_SAGA_GOAL_TRAVEL	128

#define BASE_GUARD_DISTANCE			256 //guarding the flag
#define BASE_FLAGWAIT_DISTANCE		256 //has the enemy flag and is waiting in his own base for his flag to be returned
#define BASE_GETENEMYFLAG_DISTANCE	256 //waiting around to get the enemy's flag

#define BOT_FLAG_GET_DISTANCE		256

#define BOT_SABER_THROW_RANGE		800

typedef enum
{
	CTFSTATE_NONE,
	CTFSTATE_ATTACKER,
	CTFSTATE_DEFENDER,
	CTFSTATE_RETRIEVAL,
	CTFSTATE_GUARDCARRIER,
	CTFSTATE_GETFLAGHOME,
	CTFSTATE_MAXCTFSTATES
} bot_ctf_state_t;

typedef enum
{
	TEAMPLAYSTATE_NONE,
	TEAMPLAYSTATE_FOLLOWING,
	TEAMPLAYSTATE_ASSISTING,
	TEAMPLAYSTATE_REGROUP,
	TEAMPLAYSTATE_MAXTPSTATES
} bot_teamplay_state_t;

typedef struct wpneighbor_s
{
	int num;
	int forceJumpTo;
} wpneighbor_t;

// PowTecH - Structs
typedef struct wpconnection_s
{
	int index;
	float distance;
} wpconnection_t;
//

typedef struct wpobject_s
{
	vec3_t origin;
	int inuse;
	int index;
	float weight;
	float disttonext;
	int flags;
	int associated_entity;

	int forceJumpTo;

	int neighbornum;
	//int neighbors[MAX_NEIGHBOR_SIZE];
	wpneighbor_t neighbors[MAX_NEIGHBOR_SIZE];

	// PowTecH - Properties
	wpconnection_t connections[MAX_WP_CONNECTION_SIZE];
	int connectionsCount;
	//
} wpobject_t;

typedef struct botattachment_s
{
	int level;
	char name[MAX_ATTACHMENT_NAME];
} botattachment_t;

typedef struct nodeobject_s
{
	vec3_t origin;
	int inuse;
	int index;
	float weight;
	int flags;
	int neighbornum;
} nodeobject_t;

typedef struct boteventtracker_s
{
	int			eventSequence;
	int			events[MAX_PS_EVENTS];
	float		eventTime;
} boteventtracker_t;

typedef struct botskills_s
{
	int					reflex;
	float				accuracy;
	float				turnspeed;
	float				turnspeed_combat;
	float				maxturn;
	int					perfectaim;
} botskills_t;

//bot state
typedef struct bot_state_s
{
	int inuse;										//true if this state is used by a bot client
	int botthink_residual;							//residual for the bot thinks
	int client;										//client number of the bot
	int entitynum;									//entity number of the bot
	playerState_t cur_ps;							//current player state
	usercmd_t lastucmd;								//usercmd from last frame
	bot_settings_t settings;						//several bot settings
	float thinktime;								//time the bot thinks this frame
	vec3_t origin;									//origin of the bot
	vec3_t velocity;								//velocity of the bot
	vec3_t eye;										//eye coordinates of the bot
	int setupcount;									//true when the bot has just been setup
	float ltime;									//local bot time
	float entergame_time;							//time the bot entered the game
	int ms;											//move state of the bot
	int gs;											//goal state of the bot
	int ws;											//weapon state of the bot
	vec3_t viewangles;								//current view angles
	vec3_t ideal_viewangles;						//ideal view angles
	vec3_t viewanglespeed;

	//rww - new AI values
	gentity_t			*currentEnemy;
	gentity_t			*revengeEnemy;

	gentity_t			*squadLeader;

	gentity_t			*lastHurt;
	gentity_t			*lastAttacked;

	gentity_t			*wantFlag;

	gentity_t			*touchGoal;
	gentity_t			*shootGoal;

	gentity_t			*dangerousObject;

	vec3_t				staticFlagSpot;

	int					revengeHateLevel;
	int					isSquadLeader;

	int					squadRegroupInterval;
	int					squadCannotLead;

	int					lastDeadTime;

	wpobject_t			*wpCurrent;
	wpobject_t			*wpDestination;
	wpobject_t			*wpStoreDest;
	vec3_t				goalAngles;
	vec3_t				goalMovedir;
	vec3_t				goalPosition;

	vec3_t				lastEnemySpotted;
	vec3_t				hereWhenSpotted;
	int					lastVisibleEnemyIndex;
	int					hitSpotted;

	int					wpDirection;

	float				destinationGrabTime;
	float				wpSeenTime;
	float				wpTravelTime; // PowTecH - Basically an int at this point but scared to change it
	float				wpDestSwitchTime;
	float				wpSwitchTime;
	float				wpDestIgnoreTime;

	float				timeToReact;

	float				enemySeenTime;

	float				chickenWussCalculationTime;

	float				beStill;
	float				duckTime;
	float				jumpTime;
	float				jumpHoldTime;
	float				jumpPrep;
	float				forceJumping;
	float				jDelay;

	float				aimOffsetTime;
	float				aimOffsetAmtYaw;
	float				aimOffsetAmtPitch;

	float				frame_Waypoint_Len;
	int					frame_Waypoint_Vis;
	float				frame_Enemy_Len;
	int					frame_Enemy_Vis;

	int					isCamper;
	float				isCamping;
	wpobject_t			*wpCamping;
	wpobject_t			*wpCampingTo;
	qboolean			campStanding;

	int					randomNavTime;
	int					randomNav;

	int					saberSpecialist;

	int					canChat;
	int					chatFrequency;
	char				currentChat[MAX_CHAT_LINE_SIZE];
	float				chatTime;
	float				chatTime_stored;
	int					doChat;
	int					chatTeam;
	gentity_t			*chatObject;
	gentity_t			*chatAltObject;

	float				meleeStrafeTime;
	int					meleeStrafeDir;
	float				meleeStrafeDisable;

	int					altChargeTime;

	float				escapeDirTime;

	float				dontGoBack;

	int					doAttack;
	int					doAltAttack;

	int					forceWeaponSelect;
	int					virtualWeapon;

	int					plantTime;
	int					plantDecided;
	int					plantContinue;
	int					plantKillEmAll;

	int					runningLikeASissy;
	int					runningToEscapeThreat;

	//char				chatBuffer[MAX_CHAT_BUFFER_SIZE];
	//Since we're once again not allocating bot structs dynamically,
	//shoving a 64k chat buffer into one is a bad thing.

	botskills_t			skills;

	botattachment_t		loved[MAX_LOVED_ONES];
	int					lovednum;

	int					loved_death_thresh;

	int					deathActivitiesDone;

	float				botWeaponWeights[WP_NUM_WEAPONS];

	int					ctfState;

	int					sagaState;

	int					teamplayState;

	int					jmState;

	int					state_Forced; //set by player ordering menu

	int					saberDefending;
	int					saberDefendDecideTime;
	int					saberBFTime;
	int					saberBTime;
	int					saberSTime;
	int					saberThrowTime;

	qboolean			saberPower;
	int					saberPowerTime;

	int					botChallengingTime;

	char				forceinfo[MAX_FORCE_INFO_SIZE];

#ifndef FORCEJUMP_INSTANTMETHOD
	int					forceJumpChargeTime;
#endif

	int					doForcePush;

	int					noUseTime;
	qboolean			doingFallback;
	//end rww

	// PowTecH - Properties
	int wpToGoal[MAX_NEIGHBOR_LINK_DISTANCE];
	int wpToGoalCount;
	int currentPathWaypointIndex;
	//
} bot_state_t;

void *B_TempAlloc(int size);
void B_TempFree(int size);

void *B_Alloc(int size);
void B_Free(void *ptr);

//resets the whole bot state
void BotResetState(bot_state_t *bs);
//returns the number of bots in the game
int NumBots(void);

void BotUtilizePersonality(bot_state_t *bs);
int BotDoChat(bot_state_t *bs, char *section, int always);
void BotWaypointRender(void);
int OrgVisibleBox(vec3_t org1, vec3_t mins, vec3_t maxs, vec3_t org2, int ignore);
int BotIsAChickenWuss(bot_state_t *bs);
int GetNearestVisibleWP(vec3_t org, int ignore);
int GetBestIdleGoal(bot_state_t *bs);

char *ConcatArgs( int start );

extern vmCvar_t bot_forcepowers;
extern vmCvar_t bot_forgimmick;
extern vmCvar_t bot_honorableduelacceptance;
#ifdef _DEBUG
extern vmCvar_t bot_nogoals;
extern vmCvar_t bot_debugmessages;
#endif

extern vmCvar_t bot_attachments;
extern vmCvar_t bot_camp;

extern vmCvar_t bot_wp_info;
extern vmCvar_t bot_wp_edit;
extern vmCvar_t bot_wp_clearweight;
extern vmCvar_t bot_wp_distconnect;
extern vmCvar_t bot_wp_visconnect;

extern wpobject_t *flagRed;
extern wpobject_t *oFlagRed;
extern wpobject_t *flagBlue;
extern wpobject_t *oFlagBlue;

extern gentity_t *eFlagRed;
extern gentity_t *eFlagBlue;

extern char gBotChatBuffer[MAX_CLIENTS][MAX_CHAT_BUFFER_SIZE];
extern float gWPRenderTime;
extern float gDeactivated;
extern float gBotEdit;
extern int gWPRenderedFrame;
extern wpobject_t *gWPArray[MAX_WPARRAY_SIZE];
extern int gWPNum;
extern int gLastPrintedIndex;
extern nodeobject_t nodetable[MAX_NODETABLE_SIZE];
extern int nodenum;

extern int gLevelFlags;

extern float floattime;
#define FloatTime() floattime
