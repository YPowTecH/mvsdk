#include "g_local.h";
/*
void G_PowNewGame() {
	int i = 0;
	vec3_t  spawn_origin, spawn_angles;
	gentity_t  *spawnPoint, *target;

	if (g_powGame.integer == 1) {
		//the game that was going on only has 1 person left
		if (level.currentGameCount <= 1 && level.gameStarted == 1) {
			G_Printf("^1game ended\n");
			//game ended
			level.gameStarted = 0;
			//restart the time for the next queue
			level.brStartTime = level.time + level.queueTime;

			for (i = 0; i < ARRAY_LEN(level.currentGame); i++) {
				if (level.currentGame[i] != -1) {
					SetTeam(&g_entities[level.currentGame[i]], "spectator");
					level.currentGame[i] = -1;
					level.currentGameCount = 0;
				}
			}

			for (i = 0; i < level.maxclients; i++) {
				if (level.clients[i].pers.connected != CON_DISCONNECTED) {
					if (level.clients[i].pers.connected == CON_CONNECTED) {
						if (!(g_entities[i].r.svFlags & SVF_BOT)) {
							SetTeam(&g_entities[i], "x");
						}
					}
				}
			}

			level.timer = G_Spawn();
			level.timer->think = G_PowNewGame;
			level.timer->nextthink = level.brStartTime;

		}

		//By PowTecH - BR: should be time to start the next game
		if (level.brStartTime <= level.time && !level.gameStarted) {
			//can only start if we have 2 or more players
			if (level.numVotingClients >= 2) {
				G_Printf("^2game starting\n");
				G_FreeEntity(level.timer);

				for (i = 0; i < level.numVotingClients; i++) {
					//find the avilable players
					target = &g_entities[level.playersAlive[i]];
					level.currentGame[i] = level.playersAlive[i];
					level.currentGameCount++;
					trap_UnlinkEntity(target);

					//if the player is dead he needs to respawn quick so he can catch the bus
					if (target->client->ps.stats[STAT_HEALTH] <= 0) {
						respawn(target);
					}

					//spawn them at a ctf spawn i guess
					spawnPoint = SelectCTFSpawnPoint(TEAM_RED, TEAM_BEGIN, spawn_origin, spawn_angles);

					//gogo
					if (spawnPoint) {
						target->client->ps.stats[STAT_WEAPONS] &= ~(1 << (WP_SABER));//minus the saber
						target->client->ps.stats[STAT_WEAPONS] &= ~(1 << (WP_BRYAR_PISTOL));//minus the saber
						target->client->ps.stats[STAT_WEAPONS] |= (1 << (WP_STUN_BATON));//plus a stun baton
						target->client->ps.weapon = WP_STUN_BATON;
						TeleportPlayer(target, spawnPoint->s.origin, spawnPoint->s.angles);
					}
					else {
						G_Printf("^3wtf?\n");
					}
				}

				//game started dont start it over
				level.gameStarted = 1;

				trap_SendServerCommand(-1, va("print \"^2GL HF Idoits\n\""));
			}
			else {
				//not enough players so restart the timer
				level.brStartTime = level.time + level.queueTime;
				level.timer->think = G_PowNewGame;
				level.timer->nextthink = level.brStartTime;
			}
		}
	}
}
*/
