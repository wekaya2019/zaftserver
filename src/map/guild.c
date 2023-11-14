// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/db.h"
#include "../common/timer.h"
#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/mapindex.h"
#include "../common/showmsg.h"
#include "../common/ers.h"
#include "../common/strlib.h"
#include "../common/utils.h"
#include "../common/socket.h"

#include "map.h"
#include "duel.h"
#include "guild.h"
#include "guild_castle.h"
#include "guild_expcache.h"
#include "storage.h"
#include "battle.h"
#include "npc.h"
#include "pc.h"
#include "status.h"
#include "mob.h"
#include "intif.h"
#include "clif.h"
#include "skill.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static DBMap* guild_db; // int guild_id -> struct guild*
static DBMap* guild_infoevent_db; // int guild_id -> struct eventlist*

struct eventlist {
	char name[EVENT_NAME_LENGTH];
	struct eventlist *next;
};

#define GUILD_SEND_XY_INVERVAL	5000


#define MAX_GUILD_SKILL_REQUIRE 5
struct{
	int id;
	int max;
	struct{
		short id;
		short lv;
	}need[MAX_GUILD_SKILL_REQUIRE];
} guild_skill_tree[MAX_GUILDSKILL];

static int guild_send_xy_timer(int tid, unsigned int tick, int id, intptr_t data);

/*==========================================
 * Guild Wars
 *------------------------------------------*/
bool guild_isatwar(int guild_id)
{
	struct guild *g;

	if( guild_id == 0 ) return false;

	if( battle_config.guild_wars && (g = guild_search(guild_id)) != NULL && g->war )
		return true;

	return false;
}

bool guild_canescape(struct map_session_data *sd)
{
	if( !guild_isatwar(sd->status.guild_id) )
		return true;

	if( DIFF_TICK(gettick(), sd->canescape_tick) > 10000 )
		return true;

	return false;
}

bool guild_isenemy(int guild_id, int tguild_id)
{
	struct guild *g = guild_search(guild_id), *tg;
	int i;

	if( !(battle_config.guild_wars && g && g->war && guild_id && tguild_id) )
		return false;

	ARR_FIND(0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == tguild_id);
	if( i >= MAX_GUILDALLIANCE ) return false;

	tg = guild_search(tguild_id);

	if( tg && g->alliance[i].war && g->alliance[i].opposition )
	{
		g->war_tick = tg->war_tick = last_tick + 600;
		return true;
	}

	return false;
}

bool guild_wardamage(struct map_session_data *sd)
{
	struct guild *g = guild_search(sd->status.guild_id);

	if( !(battle_config.guild_wars && g) || sd->state.pvpmode || sd->duel_group > 0 || map_flag_noguildwar(sd->bl.m) || agit_flag || agit2_flag )
		return false;

	return g->war;
}

bool guild_can_breakwar(int guild_id, int tguild_id)
{
	struct guild *g = guild_search(guild_id);
	int i;

	if( !battle_config.guild_wars || !g ) return true;

	ARR_FIND(0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == tguild_id);
	if( i >= MAX_GUILDALLIANCE ) return true; // ??

	if( g->alliance[i].war && g->alliance[i].opposition	&& DIFF_TICK(g->war_tick, last_tick) > 0 )
		return false;

	return true;
}

void guild_war_init(struct guild *g, struct guild *eg, bool starting)
{
	int i, len;
	struct map_session_data *pl_sd;
	char output[256];
	sprintf(output, "Your Guild is at War with %s.", eg->name);

	len = strlen(output);

	if( starting )
	{
		for( i = 0; i < g->max_member; i++ )
		{
			if( (pl_sd = g->member[i].sd ) == NULL )
				continue;

			clif_disp_onlyself(pl_sd, output, len);

			if( g->war || map_flag_noguildwar(pl_sd->bl.m) )
				continue;

			if( pl_sd->state.pvpmode )
				pc_pvpmodeoff(pl_sd, 1, 1); // Cannot be in PVPMODE at GuildWar
			else if( pl_sd->duel_group )
				duel_leave(pl_sd->duel_group, pl_sd);
			
			clif_map_property(pl_sd, MAPPROPERTY_FREEPVPZONE);
		}

		g->war_tick = last_tick + 600; // 10 Minutes Cannot remove the Opposition
	}

	g->war = true;
}

void guild_war_end(struct guild *g, struct guild *eg, bool surrender)
{
	int i, j, len;
	struct map_session_data *pl_sd;
	char output[256];

	if( !g || !eg )
		return;

	if( surrender )
		sprintf(output, "Your Guild has surrender to %s.", eg->name);
	else
		sprintf(output, "Your Guild has won the War against %s.", eg->name);

	len = strlen(output);

	for( i = j = 0; i < MAX_GUILDALLIANCE; i++ )
	{
		if( g->alliance[i].guild_id <= 0 || g->alliance[i].opposition != 1 )
			continue;

		if( g->alliance[i].guild_id == eg->guild_id )
			g->alliance[i].war = false;
		else if( g->alliance[i].war )
			j++; // To know if War is over
	}

	if( j == 0 )
		g->war = false; // No more enemy guilds

	for( i = 0; i < g->max_member; i++ )
	{
		if( (pl_sd = g->member[i].sd ) == NULL )
			continue;

		clif_disp_onlyself(pl_sd, output, len);
	}

	if( surrender ) return;

	sprintf(output, "Guild [%s] has surrender against Guild [%s]", eg->name, g->name);
	clif_broadcast(NULL, output, strlen(output) + 1, 0, ALL_CLIENT);
}

/*==========================================
 * Retrieves and validates the sd pointer for this guild member [Skotlex]
 *------------------------------------------*/
static TBL_PC* guild_sd_check(int guild_id, int account_id, int char_id)
{
	TBL_PC* sd = map_id2sd(account_id);

	if (!(sd && sd->status.char_id == char_id))
		return NULL;

	if (sd->status.guild_id != guild_id)
	{	//If player belongs to a different guild, kick him out.
 		intif_guild_leave(guild_id,account_id,char_id,0,"** Guild Mismatch **");
		return NULL;
	}

	return sd;
}

 // Modified [Komurka]
int guild_skill_get_max (int id)
{
	if (id < GD_SKILLBASE || id >= GD_SKILLBASE+MAX_GUILDSKILL)
		return 0;
	return guild_skill_tree[id-GD_SKILLBASE].max;
}

// ギルドスキルがあるか確認
int guild_checkskill(struct guild *g,int id)
{
	int idx = id-GD_SKILLBASE;
	if (idx < 0 || idx >= MAX_GUILDSKILL)
		return 0;
	return g->skill[idx].lv;
}

/*==========================================
 * guild_skill_tree.txt reading - from jA [Komurka]
 *------------------------------------------*/
static bool guild_read_guildskill_tree_db(char* split[], int columns, int current)
{// <skill id>,<max lv>,<req id1>,<req lv1>,<req id2>,<req lv2>,<req id3>,<req lv3>,<req id4>,<req lv4>,<req id5>,<req lv5>
	int k, id, skillid;

	skillid = atoi(split[0]);
	id = skillid - GD_SKILLBASE;

	if( id < 0 || id >= MAX_GUILDSKILL )
	{
		ShowWarning("guild_read_guildskill_tree_db: Invalid skill id %d.\n", skillid);
		return false;
	}

	guild_skill_tree[id].id = skillid;
	guild_skill_tree[id].max = atoi(split[1]);

	if( guild_skill_tree[id].id == GD_GLORYGUILD && battle_config.require_glory_guild && guild_skill_tree[id].max == 0 )
	{// enable guild's glory when required for emblems
		guild_skill_tree[id].max = 1;
	}

	for( k = 0; k < MAX_GUILD_SKILL_REQUIRE; k++ )
	{
		guild_skill_tree[id].need[k].id = atoi(split[k*2+2]);
		guild_skill_tree[id].need[k].lv = atoi(split[k*2+3]);
	}

	return true;
}

/*==========================================
 * Guild skill check - from jA [Komurka]
 *------------------------------------------*/
int guild_check_skill_require(struct guild *g,int id)
{
	int i;
	int idx = id-GD_SKILLBASE;

	if(g == NULL)
		return 0;

	if (idx < 0 || idx >= MAX_GUILDSKILL)
		return 0;

	for(i=0;i<MAX_GUILD_SKILL_REQUIRE;i++)
	{
		if(guild_skill_tree[idx].need[i].id == 0) break;
		if(guild_skill_tree[idx].need[i].lv > guild_checkskill(g,guild_skill_tree[idx].need[i].id))
			return 0;
	}
	return 1;
}

/// lookup: guild id -> guild*
struct guild* guild_search(int guild_id)
{
	return (struct guild*)idb_get(guild_db,guild_id);
}

/// lookup: guild name -> guild*
struct guild* guild_searchname(char* str)
{
	struct guild* g;

	DBIterator* iter = guild_db->iterator(guild_db);
	for( g = (struct guild*)iter->first(iter,NULL); iter->exists(iter); g = (struct guild*)iter->next(iter,NULL) )
	{
		if( strcmpi(g->name, str) == 0 )
			break;
	}
	iter->destroy(iter);

	return g;
}


struct map_session_data* guild_getavailablesd(struct guild* g)
{
	int i;

	nullpo_retr(NULL, g);

	ARR_FIND( 0, g->max_member, i, g->member[i].sd != NULL );
	return( i < g->max_member ) ? g->member[i].sd : NULL;
}

/// lookup: player AID/CID -> member index
int guild_getindex(struct guild *g,int account_id,int char_id)
{
	int i;

	if( g == NULL )
		return -1;

	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == account_id && g->member[i].char_id == char_id );
	return( i < g->max_member ) ? i : -1;
}

/// lookup: player sd -> member position
int guild_getposition(struct guild* g, struct map_session_data* sd)
{
	int i;

	if( g == NULL && (g=guild_search(sd->status.guild_id)) == NULL )
		return -1;
	
	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == sd->status.account_id && g->member[i].char_id == sd->status.char_id );
	return( i < g->max_member ) ? g->member[i].position : -1;
}

// メンバー情報の作成
void guild_makemember(struct guild_member *m,struct map_session_data *sd)
{
	nullpo_retv(sd);

	memset(m,0,sizeof(struct guild_member));
	m->account_id	=sd->status.account_id;
	m->char_id		=sd->status.char_id;
	m->hair			=sd->status.hair;
	m->hair_color	=sd->status.hair_color;
	m->gender		=sd->status.sex;
	m->class_		=sd->status.class_;
	m->lv			=sd->status.base_level;
//	m->exp			=0;
//	m->exp_payper	=0;
	m->online		=1;
	m->position		=MAX_GUILDPOSITION-1;
	memcpy(m->name,sd->status.name,NAME_LENGTH);
	return;
}



//Taken from party_send_xy_timer_sub. [Skotlex]
int guild_send_xy_timer_sub(DBKey key,void *data,va_list ap)
{
	struct guild *g=(struct guild *)data;
	int i;

	nullpo_ret(g);

	for(i=0;i<g->max_member;i++){
		//struct map_session_data* sd = g->member[i].sd;
		struct map_session_data* sd = map_charid2sd(g->member[i].char_id); // temporary crashfix
		if( sd != NULL && (sd->guild_x != sd->bl.x || sd->guild_y != sd->bl.y) && !sd->bg_id )
		{
			clif_guild_xy(sd);
			sd->guild_x = sd->bl.x;
			sd->guild_y = sd->bl.y;
		}
	}
	return 0;
}

//Code from party_send_xy_timer [Skotlex]
static int guild_send_xy_timer(int tid, unsigned int tick, int id, intptr_t data)
{
	guild_db->foreach(guild_db,guild_send_xy_timer_sub,tick);
	return 0;
}

int guild_send_dot_remove(struct map_session_data *sd)
{
	if (sd->status.guild_id)
		clif_guild_xy_remove(sd);
	return 0;
}
//------------------------------------------------------------------------

int guild_create(struct map_session_data *sd, const char *name)
{
	char tname[NAME_LENGTH];
	struct guild_member m;
	nullpo_ret(sd);

	safestrncpy(tname, name, NAME_LENGTH);
	trim(tname);

	if( !tname[0] )
		return 0; // empty name

	if( sd->status.guild_id )
	{// already in a guild
		clif_guild_created(sd,1);
		return 0;
	}
	if( battle_config.guild_emperium_check && pc_search_inventory(sd,714) == -1 )
	{// item required
		clif_guild_created(sd,3);
		return 0;
	}

	guild_makemember(&m,sd);
	m.position=0;
	intif_guild_create(name,&m);
	return 1;
}

// 作成可否
int guild_created(int account_id,int guild_id)
{
	struct map_session_data *sd=map_id2sd(account_id);

	if(sd==NULL)
		return 0;
	if(!guild_id) {
		clif_guild_created(sd,2);	// 作成失敗（同名ギルド存在）
		return 0;
	}
	//struct guild *g;
	sd->status.guild_id=guild_id;
	clif_guild_created(sd,0);
	if(battle_config.guild_emperium_check)
		pc_delitem(sd,pc_search_inventory(sd,714),1,0,0,LOG_TYPE_CONSUME);	// エンペリウム消耗
	return 0;
}

// 情報要求
int guild_request_info(int guild_id)
{
	return intif_guild_request_info(guild_id);
}

// イベント付き情報要求
int guild_npc_request_info(int guild_id,const char *event)
{
	if( guild_search(guild_id) )
	{
		if( event && *event )
			npc_event_do(event);

		return 0;
	}

	if( event && *event )
	{
		struct eventlist* ev;
		ev=(struct eventlist *)aCalloc(sizeof(struct eventlist),1);
		memcpy(ev->name,event,strlen(event));
		//The one in the db becomes the next event from this.
		ev->next = (struct eventlist*)idb_put(guild_infoevent_db,guild_id,ev);
	}

	return guild_request_info(guild_id);
}

// 所属キャラの確認
int guild_check_member(struct guild *g)
{
	int i;
	struct map_session_data *sd;
	struct s_mapiterator* iter;

	nullpo_ret(g);

	iter = mapit_getallusers();
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) )
	{
		if( sd->status.guild_id != g->guild_id )
			continue;

		i = guild_getindex(g,sd->status.account_id,sd->status.char_id);
		if (i < 0) {
			sd->status.guild_id=0;
			sd->guild_emblem_id=0;
			ShowWarning("guild: check_member %d[%s] is not member\n",sd->status.account_id,sd->status.name);
			status_calc_pc(sd,0); // Regional System
		}
	}
	mapit_free(iter);

	return 0;
}

// 情報所得失敗（そのIDのキャラを全部未所属にする）
int guild_recv_noinfo(int guild_id)
{
	struct map_session_data *sd;
	struct s_mapiterator* iter;

	iter = mapit_getallusers();
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) )
	{
		if( sd->status.guild_id == guild_id )
		{
			sd->status.guild_id = 0; // erase guild
			status_calc_pc(sd,0); // Regional System
		}
	}
	mapit_free(iter);

	return 0;
}

int guild_recv_info(struct guild *sg)
{
	struct guild *g,before;
	int i,bm,m;
	struct eventlist *ev,*ev2;
	struct map_session_data *sd;
	bool guild_new = false;

	nullpo_ret(sg);

	if((g = (struct guild*)idb_get(guild_db,sg->guild_id))==NULL)
	{
		guild_new = true;
		g=(struct guild *)aCalloc(1,sizeof(struct guild));
		idb_put(guild_db,sg->guild_id,g);
		before=*sg;

		guild_check_member(sg);
		if ((sd = map_nick2sd(sg->master)) != NULL)
		{
			sd->state.gmaster_flag = true;
			clif_charnameupdate(sd); // [LuzZza]
			clif_guild_masterormember(sd);
		}
	}else
		before=*g;
	memcpy(g,sg,sizeof(struct guild));
	for( i = 0; i < MAX_GUILDSKILL; i++ )
	{
		if( guild_new )
			g->skill_block_timer[i] = INVALID_TIMER;
		else
			g->skill_block_timer[i] = before.skill_block_timer[i];
	}

	if(g->max_member > MAX_GUILD)
	{
		ShowError("guild_recv_info: Received guild with %d members, but MAX_GUILD is only %d. Extra guild-members have been lost!\n", g->max_member, MAX_GUILD);
		g->max_member = MAX_GUILD;
	}
	
	for(i=bm=m=0;i<g->max_member;i++){
		if(g->member[i].account_id>0){
			sd = g->member[i].sd = guild_sd_check(g->guild_id, g->member[i].account_id, g->member[i].char_id);
			if (sd) clif_charnameupdate(sd); // [LuzZza]
			m++;
		}else
			g->member[i].sd=NULL;
		if(before.member[i].account_id>0)
			bm++;
	}

	for(i=0;i<g->max_member;i++){
		sd = g->member[i].sd;
		if( sd==NULL )
			continue;

		if(	before.guild_lv!=g->guild_lv || bm!=m ||
			before.max_member!=g->max_member ){
			clif_guild_basicinfo(sd);
			clif_guild_emblem(sd,g);
		}

		if(bm!=m)
			clif_guild_memberlist(g->member[i].sd);

		if( before.skill_point!=g->skill_point)
			clif_guild_skillinfo(sd);	// スキル情報送信

		if( guild_new )
		{
			clif_guild_belonginfo(sd,g);
			clif_guild_notice(sd,g);
			sd->guild_emblem_id=g->emblem_id;
		}
	}

	if( battle_config.guild_wars )
	{
		struct guild *eg; // To hold Enemy Data
		int j;

		for( i = 0; i < MAX_GUILDALLIANCE; i++ )
		{
			if( !g->alliance[i].guild_id || g->alliance[i].opposition == 0 )
				continue;

			if( (eg = idb_get(guild_db, g->alliance[i].guild_id)) == NULL )
				continue; // This guild is not loaded

			ARR_FIND(0, MAX_GUILDALLIANCE, j, eg->alliance[j].guild_id == g->guild_id && eg->alliance[j].opposition != 0 );
			if( j < MAX_GUILDALLIANCE )
			{ // Double Opposition = War
				g->alliance[i].war = eg->alliance[i].war = true;
				guild_war_init(g, eg, guild_new);
				guild_war_init(eg, g, guild_new);
			}
		}
	}

	if( (ev = (struct eventlist*)idb_remove(guild_infoevent_db,sg->guild_id))!=NULL )
	{
		while(ev){
			npc_event_do(ev->name);
			ev2=ev->next;
			aFree(ev);
			ev=ev2;
		}
	}

	return 0;
}


// ギルドへの勧誘
int guild_invite(struct map_session_data *sd,struct map_session_data *tsd)
{
	struct guild *g;
	int i;

	nullpo_ret(sd);

	g=guild_search(sd->status.guild_id);

	if(tsd==NULL || g==NULL)
		return 0;

	if( !battle_config.faction_allow_guild && sd->status.faction_id != tsd->status.faction_id )
	{
		clif_displaymessage(sd->fd,"You cannot invite to guild other faction's members.");
		return 0;
	}

	if( (i=guild_getposition(g,sd))<0 || !(g->position[i].mode&0x0001) )
		return 0; //Invite permission.

	if(!battle_config.invite_request_check) {
		if (tsd->party_invite>0 || tsd->trade_partner || tsd->adopt_invite ) {	// 相手が取引中かどうか
			clif_guild_inviteack(sd,0);
			return 0;
		}
	}
	
	if (!tsd->fd) { //You can't invite someone who has already disconnected.
		clif_guild_inviteack(sd,1);
		return 0;
	}

	if(tsd->status.guild_id>0 ||
		tsd->guild_invite>0 ||
		((agit_flag || agit2_flag) && map[tsd->bl.m].flag.gvg_castle))
	{	//Can't invite people inside castles. [Skotlex]
		clif_guild_inviteack(sd,0);
		return 0;
	}

	// 定員確認
	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == 0 );
	if(i==g->max_member){
		clif_guild_inviteack(sd,3);
		return 0;
	}

	tsd->guild_invite=sd->status.guild_id;
	tsd->guild_invite_account=sd->status.account_id;

	clif_guild_invite(tsd,g);
	return 0;
}

/// Guild invitation reply.
/// flag: 0:rejected, 1:accepted
int guild_reply_invite(struct map_session_data* sd, int guild_id, int flag)
{
	struct map_session_data* tsd;

	nullpo_ret(sd);

	// subsequent requests may override the value
	if( sd->guild_invite != guild_id )
		return 0; // mismatch

	// look up the person who sent the invite
	//NOTE: this can be NULL because the person might have logged off in the meantime
	tsd = map_id2sd(sd->guild_invite_account);

	if ( sd->status.guild_id > 0 ) // [Paradox924X]
	{ // Already in another guild.
		if ( tsd ) clif_guild_inviteack(tsd,0);
		return 0;
	}
	else if( flag == 0 )
	{// rejected
		sd->guild_invite = 0;
		sd->guild_invite_account = 0;
		if( tsd ) clif_guild_inviteack(tsd,1);
	}
	else
	{// accepted
		struct guild_member m;
		struct guild* g;
		int i;

		if( (g=guild_search(guild_id)) == NULL )
		{
			sd->guild_invite = 0;
			sd->guild_invite_account = 0;
			return 0;
		}

		ARR_FIND( 0, g->max_member, i, g->member[i].account_id == 0 );
		if( i == g->max_member )
		{
			sd->guild_invite = 0;
			sd->guild_invite_account = 0;
			if( tsd ) clif_guild_inviteack(tsd,3);
			return 0;
		}

		guild_makemember(&m,sd);
		intif_guild_addmember(guild_id, &m);
		//TODO: send a minimap update to this player
	}

	return 0;
}

//Invoked when a player joins.
//- If guild is not in memory, it is requested
//- Otherwise sd pointer is set up.
//- Player must be authed and must belong to a guild before invoking this method
void guild_member_joined(struct map_session_data *sd)
{
	struct guild* g;
	int i;
	g=guild_search(sd->status.guild_id);
	if (!g) {
		guild_request_info(sd->status.guild_id);
		return;
	}
	if (strcmp(sd->status.name,g->master) == 0)
	{	// set the Guild Master flag
		sd->state.gmaster_flag = true;
		// prevent Guild Skills from being used directly after relog
	}
	i = guild_getindex(g, sd->status.account_id, sd->status.char_id);
	if (i == -1)
	{
		sd->status.guild_id = 0;
		status_calc_pc(sd,0); // Regional System
	}
	else
		g->member[i].sd = sd;
}

// ギルドメンバが追加された
int guild_member_added(int guild_id,int account_id,int char_id,int flag)
{
	struct map_session_data *sd= map_id2sd(account_id),*sd2;
	struct guild *g;

	if( (g=guild_search(guild_id))==NULL )
		return 0;

	if(sd==NULL || sd->guild_invite==0){
		// キャラ側に登録できなかったため脱退要求を出す
		if (flag == 0) {
			ShowError("guild: member added error %d is not online\n",account_id);
 			intif_guild_leave(guild_id,account_id,char_id,0,"** Data Error **");
		}
		return 0;
	}
	sd2 = map_id2sd(sd->guild_invite_account);
	sd->guild_invite = 0;
	sd->guild_invite_account = 0;

	if(flag==1){	// 失敗
		if( sd2!=NULL )
			clif_guild_inviteack(sd2,3);
		return 0;
	}

		// 成功
	sd->status.guild_id = g->guild_id;
	sd->guild_emblem_id = g->emblem_id;
	//Packets which were sent in the previous 'guild_sent' implementation.
	clif_guild_belonginfo(sd,g);
	clif_guild_notice(sd,g);
	status_calc_pc(sd,0); // Regional System

	//TODO: send new emblem info to others

	if( sd2!=NULL )
		clif_guild_inviteack(sd2,2);

	//Next line commented because it do nothing, look at guild_recv_info [LuzZza]
	//clif_charnameupdate(sd); //Update display name [Skotlex]

	return 0;
}

// ギルド脱退要求
int guild_leave(struct map_session_data* sd, int guild_id, int account_id, int char_id, const char* mes)
{
	struct guild *g;

	nullpo_ret(sd);

	g = guild_search(sd->status.guild_id);

	if(g==NULL)
		return 0;

	if(sd->status.account_id!=account_id ||
		sd->status.char_id!=char_id || sd->status.guild_id!=guild_id ||
		((agit_flag || agit2_flag) && map[sd->bl.m].flag.gvg_castle))
		return 0;

	intif_guild_leave(sd->status.guild_id, sd->status.account_id, sd->status.char_id,0,mes);
	return 0;
}

// ギルド追放要求
int guild_expulsion(struct map_session_data* sd, int guild_id, int account_id, int char_id, const char* mes)
{
	struct map_session_data *tsd;
	struct guild *g;
	int i,ps;

	nullpo_ret(sd);

	g = guild_search(sd->status.guild_id);

	if(g==NULL)
		return 0;

	if(sd->status.guild_id!=guild_id)
		return 0;

	if( (ps=guild_getposition(g,sd))<0 || !(g->position[ps].mode&0x0010) )
		return 0;	//Expulsion permission

  	//Can't leave inside guild castles.
	if ((tsd = map_id2sd(account_id)) &&
		tsd->status.char_id == char_id &&
		((agit_flag || agit2_flag) && map[tsd->bl.m].flag.gvg_castle))
		return 0;

	// find the member and perform expulsion
	i = guild_getindex(g, account_id, char_id);
	if( i != -1 && strcmp(g->member[i].name,g->master) != 0 ) //Can't expel the GL!
		intif_guild_leave(g->guild_id,account_id,char_id,1,mes);

	return 0;
}

int guild_member_withdraw(int guild_id, int account_id, int char_id, int flag, const char* name, const char* mes)
{
	int i;
	struct guild* g = guild_search(guild_id);
	struct map_session_data* sd = map_charid2sd(char_id);
	struct map_session_data* online_member_sd;

	if(g == NULL)
		return 0; // no such guild (error!)
	
	i = guild_getindex(g, account_id, char_id);
	if( i == -1 )
		return 0; // not a member (inconsistency!)

	online_member_sd = guild_getavailablesd(g);
	if(online_member_sd == NULL)
		return 0; // noone online to inform

	if(!flag)
		clif_guild_leave(online_member_sd, name, mes);
	else
		clif_guild_expulsion(online_member_sd, name, mes, account_id);

	// remove member from guild
	memset(&g->member[i],0,sizeof(struct guild_member));
	clif_guild_memberlist(online_member_sd);

	// update char, if online
	if(sd != NULL && sd->status.guild_id == guild_id)
	{
		// do stuff that needs the guild_id first, BEFORE we wipe it
		if (sd->state.storage_flag == 2) //Close the guild storage.
			storage_guild_storageclose(sd);
		guild_send_dot_remove(sd);

		sd->status.guild_id = 0;
		sd->guild_emblem_id = 0;
		
		clif_charnameupdate(sd); //Update display name [Skotlex]
		//TODO: send emblem update to self and people around
		status_calc_pc(sd,0); // Regional System
	}
	return 0;
}

int guild_send_memberinfoshort(struct map_session_data *sd,int online)
{ // cleaned up [LuzZza]
	struct guild *g;
	
	nullpo_ret(sd);
		
	if(sd->status.guild_id <= 0)
		return 0;

	if(!(g = guild_search(sd->status.guild_id)))
		return 0;

	intif_guild_memberinfoshort(g->guild_id,
		sd->status.account_id,sd->status.char_id,online,sd->status.base_level,sd->status.class_);

	if(!online){
		int i=guild_getindex(g,sd->status.account_id,sd->status.char_id);
		if(i>=0)
			g->member[i].sd=NULL;
		else
			ShowError("guild_send_memberinfoshort: Failed to locate member %d:%d in guild %d!\n", sd->status.account_id, sd->status.char_id, g->guild_id);
		return 0;
	}
	
	if(sd->state.connect_new)
	{	//Note that this works because it is invoked in parse_LoadEndAck before connect_new is cleared.
		clif_guild_belonginfo(sd,g);
		clif_guild_notice(sd,g);
		sd->guild_emblem_id = g->emblem_id;
	}
	return 0;
}

int guild_recv_memberinfoshort(int guild_id,int account_id,int char_id,int online,int lv,int class_)
{ // cleaned up [LuzZza]
	
	int i,alv,c,idx=-1,om=0,oldonline=-1;
	struct guild *g = guild_search(guild_id);
	
	if(g == NULL)
		return 0;
	
	for(i=0,alv=0,c=0,om=0;i<g->max_member;i++){
		struct guild_member *m=&g->member[i];
		if(!m->account_id) continue;
		if(m->account_id==account_id && m->char_id==char_id ){
			oldonline=m->online;
			m->online=online;
			m->lv=lv;
			m->class_=class_;
			idx=i;
		}
		alv+=m->lv;
		c++;
		if(m->online)
			om++;
	}
	
	if(idx == -1 || c == 0) {
		// ギルドのメンバー外なので追放扱いする
		struct map_session_data *sd = map_id2sd(account_id);
		if(sd && sd->status.char_id == char_id) {
			sd->status.guild_id=0;
			sd->guild_emblem_id=0;
			status_calc_pc(sd,0); // Regional System
		}
		ShowWarning("guild: not found member %d,%d on %d[%s]\n",	account_id,char_id,guild_id,g->name);
		return 0;
	}
	
	g->average_lv=alv/c;
	g->connect_member=om;

	//Ensure validity of pointer (ie: player logs in/out, changes map-server)
	g->member[idx].sd = guild_sd_check(guild_id, account_id, char_id);

	if(oldonline!=online) 
		clif_guild_memberlogin_notice(g, idx, online);
	
	if(!g->member[idx].sd)
		return 0;

	//Send XY dot updates. [Skotlex]
	//Moved from guild_send_memberinfoshort [LuzZza]
	for(i=0; i < g->max_member; i++) {
		
		if(!g->member[i].sd || i == idx ||
			g->member[i].sd->bl.m != g->member[idx].sd->bl.m)
			continue;

		clif_guild_xy_single(g->member[idx].sd->fd, g->member[i].sd);
		clif_guild_xy_single(g->member[i].sd->fd, g->member[idx].sd);
	}

	return 0;
}
// ギルド会話送信
int guild_send_message(struct map_session_data *sd,const char *mes,int len)
{
	nullpo_ret(sd);

	if(sd->status.guild_id==0)
		return 0;
	intif_guild_message(sd->status.guild_id,sd->status.account_id,mes,len);
	guild_recv_message(sd->status.guild_id,sd->status.account_id,mes,len);

	// Chat logging type 'G' / Guild Chat
	log_chat(LOG_CHAT_GUILD, sd->status.guild_id, sd->status.char_id, sd->status.account_id, mapindex_id2name(sd->mapindex), sd->bl.x, sd->bl.y, NULL, mes);

	return 0;
}
// ギルド会話受信
int guild_recv_message(int guild_id,int account_id,const char *mes,int len)
{
	struct guild *g;
	if( (g=guild_search(guild_id))==NULL)
		return 0;
	clif_guild_message(g,account_id,mes,len);
	return 0;
}
// ギルドメンバの役職変更
int guild_change_memberposition(int guild_id,int account_id,int char_id,short idx)
{
	return intif_guild_change_memberinfo(guild_id,account_id,char_id,GMI_POSITION,&idx,sizeof(idx));
}
// ギルドメンバの役職変更通知
int guild_memberposition_changed(struct guild *g,int idx,int pos)
{
	nullpo_ret(g);

	g->member[idx].position=pos;
	clif_guild_memberpositionchanged(g,idx);
	
	// Update char position in client [LuzZza]
	if(g->member[idx].sd != NULL)
		clif_charnameupdate(g->member[idx].sd);
	return 0;
}
// ギルド役職変更
int guild_change_position(int guild_id,int idx,int mode,int exp_mode,const char *name)
{
	struct guild_position p;

	exp_mode = cap_value(exp_mode, 0, battle_config.guild_exp_limit);
	//Mode 0x01 <- Invite
	//Mode 0x10 <- Expel.
	p.mode=mode&0x11;
	p.exp_mode=exp_mode;
	safestrncpy(p.name,name,NAME_LENGTH);
	return intif_guild_position(guild_id,idx,&p);
}
// ギルド役職変更通知
int guild_position_changed(int guild_id,int idx,struct guild_position *p)
{
	struct guild *g=guild_search(guild_id);
	int i;
	if(g==NULL)
		return 0;
	memcpy(&g->position[idx],p,sizeof(struct guild_position));
	clif_guild_positionchanged(g,idx);
	
	// Update char name in client [LuzZza]
	for(i=0;i<g->max_member;i++)
		if(g->member[i].position == idx && g->member[i].sd != NULL)
			clif_charnameupdate(g->member[i].sd);
	return 0;
}
// ギルド告知変更
int guild_change_notice(struct map_session_data *sd,int guild_id,const char *mes1,const char *mes2)
{
	nullpo_ret(sd);

	if(guild_id!=sd->status.guild_id)
		return 0;
	return intif_guild_notice(guild_id,mes1,mes2);
}
// ギルド告知変更通知
int guild_notice_changed(int guild_id,const char *mes1,const char *mes2)
{
	int i;
	struct map_session_data *sd;
	struct guild *g=guild_search(guild_id);
	if(g==NULL)
		return 0;

	memcpy(g->mes1,mes1,MAX_GUILDMES1);
	memcpy(g->mes2,mes2,MAX_GUILDMES2);

	for(i=0;i<g->max_member;i++){
		if((sd=g->member[i].sd)!=NULL)
			clif_guild_notice(sd,g);
	}
	return 0;
}
// ギルドエンブレム変更
int guild_change_emblem(struct map_session_data *sd,int len,const char *data)
{
	struct guild *g;
	nullpo_ret(sd);

	if (battle_config.require_glory_guild &&
		!((g = guild_search(sd->status.guild_id)) && guild_checkskill(g, GD_GLORYGUILD)>0)) {
		clif_skill_fail(sd,GD_GLORYGUILD,USESKILL_FAIL_LEVEL,0);
		return 0;
	}

	return intif_guild_emblem(sd->status.guild_id,len,data);
}
// ギルドエンブレム変更通知
int guild_emblem_changed(int len,int guild_id,int emblem_id,const char *data)
{
	int i;

	struct guild* g = guild_search(guild_id);
	if( g == NULL )
		return 0;

	memcpy(g->emblem_data, data, len);
	g->emblem_len = len;
	g->emblem_id = emblem_id;

	// update members
	for( i = 0; i < g->max_member; ++i )
	{
		struct map_session_data* sd = g->member[i].sd;
		if( sd == NULL )
			continue;

		sd->guild_emblem_id = emblem_id;
		clif_guild_belonginfo(sd,g);
		clif_guild_emblem(sd,g);
		clif_guild_emblem_area(&sd->bl);
	}

	// update guardians (mobs)
	guild_castle_guardian_updateemblem(guild_id, emblem_id);

	// update npcs (flags or other npcs that used flagemblem to attach to this guild)
	{
		// TODO this is not efficient [FlavioJS]
		struct s_mapiterator* iter = mapit_geteachnpc();
		TBL_NPC* nd;
		for( nd = (TBL_NPC*)mapit_first(iter) ; mapit_exists(iter); nd = (TBL_NPC*)mapit_next(iter) )
		{
			if( nd->subtype != SCRIPT || nd->u.scr.guild_id != guild_id )
				continue;
			clif_guild_emblem_area(&nd->bl);
		}
		mapit_free(iter);
	}

	return 0;
}


unsigned int guild_payexp(struct map_session_data* sd, unsigned int exp)
{
	struct guild* g;
	int pos;
	int per;

	nullpo_ret(sd);

	if( exp == 0 )
		return 0;

	if( sd->status.guild_id == 0 )
		return 0;

	g = guild_search(sd->status.guild_id);
	if( g == NULL )
		return 0;

	pos = guild_getposition(g, sd);
	if( pos < 0 )
		return 0;

	per = g->position[pos].exp_mode;
	if( per <= 0 )
		return 0;

	if( per != 100 )
		exp = exp * per / 100;

	guild_addexp(sd->status.guild_id, sd->status.account_id, sd->status.char_id, exp);
	return exp;
}

// Zephyrus
int guild_score_saved(int guild_id, int index)
{
	return 0;
}

// スキルポイント割り振り
int guild_skillup(TBL_PC* sd, int skill_num)
{
	struct guild* g;
	int idx = skill_num - GD_SKILLBASE;
	int max = guild_skill_get_max(skill_num);

	nullpo_ret(sd);

	if( idx < 0 || idx >= MAX_GUILDSKILL || // not a guild skill
			sd->status.guild_id == 0 || (g=guild_search(sd->status.guild_id)) == NULL || // no guild
			strcmp(sd->status.name, g->master) ) // not the guild master
		return 0;

	if( g->skill_point > 0 &&
			g->skill[idx].id != 0 &&
			g->skill[idx].lv < max )
		intif_guild_skillup(g->guild_id, skill_num, sd->status.account_id, max);

	return 0;
}
// スキルポイント割り振り通知
int guild_skillupack(int guild_id,int skill_num,int account_id)
{
	struct map_session_data *sd=map_id2sd(account_id);
	struct guild *g=guild_search(guild_id);
	int i;
	if(g==NULL)
		return 0;
	if(sd!=NULL)
		clif_guild_skillup(sd,skill_num,g->skill[skill_num-GD_SKILLBASE].lv);
	// 全員に通知
	for(i=0;i<g->max_member;i++)
		if((sd=g->member[i].sd)!=NULL)
			clif_guild_skillinfo(sd);
	return 0;
}

// ギルド同盟数所得
int guild_get_alliance_count(struct guild *g,int flag)
{
	int i,c;

	nullpo_ret(g);

	for(i=c=0;i<MAX_GUILDALLIANCE;i++){
		if(	g->alliance[i].guild_id>0 &&
			g->alliance[i].opposition==flag )
			c++;
	}
	return c;
}

// Blocks all guild skills which have a common delay time.
int guild_block_skill_end(int tid, unsigned int tick, int id, intptr_t data)
{
	struct guild *g;
	char output[128];
	int idx = battle_config.guild_skills_separed_delay ? (int)data - GD_SKILLBASE : 0;

	if( (g = guild_search(id)) == NULL )
		return 1;

	if( idx < 0 || idx >= MAX_GUILDSKILL )
	{
		ShowError("guild_block_skill_end invalid skillnum %d.\n", (int)data);
		return 0;
	}

	if( tid != g->skill_block_timer[idx] )
	{
		ShowError("guild_block_skill_end %d != %d.\n", g->skill_block_timer[idx], tid);
		return 0;
	}

	sprintf(output, "%s : Guild Skill %s Ready!!", g->name, skill_get_desc((int)data));
	g->skill_block_timer[idx] = INVALID_TIMER;
	clif_guild_message(g, 0, output, strlen(output));

	return 1;
}

void guild_block_skill_status(struct guild *g, int skillnum)
{
	const struct TimerData * td;
	char output[128];
	int seconds, idx;
	
	idx = battle_config.guild_skills_separed_delay ? skillnum - GD_SKILLBASE : 0;
	if( g == NULL || idx < 0 || idx >= MAX_GUILDSKILL || g->skill_block_timer[idx] == INVALID_TIMER )
		return;

	if( (td = get_timer(g->skill_block_timer[idx])) == NULL )
		return;

	seconds = DIFF_TICK(td->tick,gettick())/1000;
	sprintf(output, "%s : Cannot use guild skill %s. %d seconds remaining...", g->name, skill_get_desc(skillnum), seconds);
	clif_guild_message(g, 0, output, strlen(output));
}

void guild_block_skill_start(struct guild *g, int skillnum, int time)
{
	int idx = battle_config.guild_skills_separed_delay ? skillnum - GD_SKILLBASE : 0;
	if( g == NULL || idx < 0 || idx >= MAX_GUILDSKILL )
		return;

	if( g->skill_block_timer[idx] != INVALID_TIMER )
		delete_timer(g->skill_block_timer[idx], guild_block_skill_end);
	
	g->skill_block_timer[idx] = add_timer(gettick() + time, guild_block_skill_end, g->guild_id, skillnum);
}

int guild_check_alliance(int guild_id1, int guild_id2, int flag)
{
	struct guild *g;
	int i;

	g = guild_search(guild_id1);
	if (g == NULL)
		return 0;

	ARR_FIND( 0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == guild_id2 && g->alliance[i].opposition == flag );
	return( i < MAX_GUILDALLIANCE ) ? 1 : 0;
}

// ギルド同盟要求
int guild_reqalliance(struct map_session_data *sd,struct map_session_data *tsd)
{
	struct guild *g[2];
	int i;

	if(agit_flag || agit2_flag)	{	// Disable alliance creation during woe [Valaris]
		clif_displaymessage(sd->fd,"Alliances cannot be made during Guild Wars!");
		return 0;
	}	// end addition [Valaris]


	nullpo_ret(sd);

	if(tsd==NULL || tsd->status.guild_id<=0)
		return 0;

	g[0]=guild_search(sd->status.guild_id);
	g[1]=guild_search(tsd->status.guild_id);

	if(g[0]==NULL || g[1]==NULL)
		return 0;

	// Prevent creation alliance with same guilds [LuzZza]
	if(sd->status.guild_id == tsd->status.guild_id)
		return 0;

	if( guild_get_alliance_count(g[0],0) >= battle_config.max_guild_alliance )	{
		clif_guild_allianceack(sd,4);
		return 0;
	}
	if( guild_get_alliance_count(g[1],0) >= battle_config.max_guild_alliance ) {
		clif_guild_allianceack(sd,3);
		return 0;
	}

	if( tsd->guild_alliance>0 ){
		clif_guild_allianceack(sd,1);
		return 0;
	}

	for(i=0;i<MAX_GUILDALLIANCE;i++){	// すでに同盟状態か確認
		if(	g[0]->alliance[i].guild_id==tsd->status.guild_id &&
			g[0]->alliance[i].opposition==0){
			clif_guild_allianceack(sd,0);
			return 0;
		}
	}

	tsd->guild_alliance=sd->status.guild_id;
	tsd->guild_alliance_account=sd->status.account_id;

	clif_guild_reqalliance(tsd,sd->status.account_id,g[0]->name);
	return 0;
}
// ギルド勧誘への返答
int guild_reply_reqalliance(struct map_session_data *sd,int account_id,int flag)
{
	struct map_session_data *tsd;

	nullpo_ret(sd);
	tsd= map_id2sd( account_id );
	if (!tsd) { //Character left? Cancel alliance.
		clif_guild_allianceack(sd,3);
		return 0;
	}

	if(sd->guild_alliance!=tsd->status.guild_id)	// 勧誘とギルドIDが違う
		return 0;

	if(flag==1){	// 承諾
		int i;

		struct guild *g,*tg;	// 同盟数再確認
		g=guild_search(sd->status.guild_id);
		tg=guild_search(tsd->status.guild_id);
		
		if(g==NULL || guild_get_alliance_count(g,0) >= battle_config.max_guild_alliance){
			clif_guild_allianceack(sd,4);
			clif_guild_allianceack(tsd,3);
			return 0;
		}
		if(tg==NULL || guild_get_alliance_count(tg,0) >= battle_config.max_guild_alliance){
			clif_guild_allianceack(sd,3);
			clif_guild_allianceack(tsd,4);
			return 0;
		}

		for(i=0;i<MAX_GUILDALLIANCE;i++){
			if(g->alliance[i].guild_id==tsd->status.guild_id &&
				g->alliance[i].opposition==1)
				intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
					sd->status.account_id,tsd->status.account_id,9 );
		}
		for(i=0;i<MAX_GUILDALLIANCE;i++){
			if(tg->alliance[i].guild_id==sd->status.guild_id &&
				tg->alliance[i].opposition==1)
				intif_guild_alliance( tsd->status.guild_id,sd->status.guild_id,
					tsd->status.account_id,sd->status.account_id,9 );
		}

		// inter鯖へ同盟要請
		intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
			sd->status.account_id,tsd->status.account_id,0 );
		return 0;
	}else{		// 拒否
		sd->guild_alliance=0;
		sd->guild_alliance_account=0;
		if(tsd!=NULL)
			clif_guild_allianceack(tsd,3);
	}
	return 0;
}

// ギルド関係解消
int guild_delalliance(struct map_session_data *sd,int guild_id,int flag)
{
	nullpo_ret(sd);

	if(agit_flag || agit2_flag)	{	// Disable alliance breaking during woe [Valaris]
		clif_displaymessage(sd->fd,"Alliances cannot be broken during Guild Wars!");
		return 0;
	}	// end addition [Valaris]

	if( flag && !guild_can_breakwar(sd->status.guild_id, guild_id) ) {
		clif_displaymessage(sd->fd,"Opposition cannot be broken at War. You need to wait 10 minutes of No Hostile activity.");
		return 0;
	}

	intif_guild_alliance( sd->status.guild_id,guild_id,sd->status.account_id,0,flag|8 );
	return 0;
}

// ギルド敵対
int guild_opposition(struct map_session_data *sd,struct map_session_data *tsd)
{
	struct guild *g;
	int i;

	nullpo_ret(sd);

	g=guild_search(sd->status.guild_id);
	if(g==NULL || tsd==NULL)
		return 0;

	// Prevent creation opposition with same guilds [LuzZza]
	if(sd->status.guild_id == tsd->status.guild_id)
		return 0;

	if( guild_get_alliance_count(g,1) >= battle_config.max_guild_opposition )	{
		clif_guild_oppositionack(sd,1);
		return 0;
	}

	for(i=0;i<MAX_GUILDALLIANCE;i++){	// すでに関係を持っているか確認
		if(g->alliance[i].guild_id==tsd->status.guild_id){
			if(g->alliance[i].opposition==1){	// すでに敵対
				clif_guild_oppositionack(sd,2);
				return 0;
			}
			if(agit_flag || agit2_flag) // Prevent the changing of alliances to oppositions during WoE.
				return 0;
			//Change alliance to opposition.
			intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
				sd->status.account_id,tsd->status.account_id,8 );
		}
	}

	// inter鯖に敵対要請
	intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
			sd->status.account_id,tsd->status.account_id,1 );
	return 0;
}

// ギルド同盟/敵対通知
int guild_allianceack(int guild_id1,int guild_id2,int account_id1,int account_id2,int flag,const char *name1,const char *name2)
{
	struct guild *g[2];
	int guild_id[2];
	const char *guild_name[2];
	struct map_session_data *sd[2];
	int j,i;

	guild_id[0] = guild_id1;
	guild_id[1] = guild_id2;
	guild_name[0] = name1;
	guild_name[1] = name2;
	sd[0] = map_id2sd(account_id1);
	sd[1] = map_id2sd(account_id2);

	g[0]=guild_search(guild_id1);
	g[1]=guild_search(guild_id2);

	if(sd[0]!=NULL && (flag&0x0f)==0){
		sd[0]->guild_alliance=0;
		sd[0]->guild_alliance_account=0;
	}

	if(flag&0x70){	// 失敗
		for(i=0;i<2-(flag&1);i++)
			if( sd[i]!=NULL )
				clif_guild_allianceack(sd[i],((flag>>4)==i+1)?3:4);
		return 0;
	}

	if(!(flag&0x08)){ // Creating Alliance|Opposition
		j = MAX_GUILDALLIANCE;
		for(i=0;i<2-(flag&1);i++)
		{
			if(g[i]!=NULL)
			{
				ARR_FIND( 0, MAX_GUILDALLIANCE, j, g[i]->alliance[j].guild_id == 0 );
				if( j < MAX_GUILDALLIANCE )
				{
					g[i]->alliance[j].guild_id=guild_id[1-i];
					memcpy(g[i]->alliance[j].name,guild_name[1-i],NAME_LENGTH);
					g[i]->alliance[j].opposition=flag&1;
				}
			}
		}

		// Guild Wars
		if( battle_config.guild_wars && (flag&1) && g[0] && g[1] && j < MAX_GUILDALLIANCE )
		{ // Opossition Created
			char output[256];

			ARR_FIND(0, MAX_GUILDALLIANCE, i, g[1]->alliance[i].guild_id == guild_id[0] && g[1]->alliance[i].opposition != 0);
			if( i < MAX_GUILDALLIANCE )
			{
				sprintf(output, "Guild [%s] and Guild [%s] are now at War!!", g[0]->name, g[1]->name);

				g[0]->alliance[j].war = true;
				g[1]->alliance[i].war = true;
				guild_war_init(g[0], g[1], true);
				guild_war_init(g[1], g[0], true);

				clif_broadcast(NULL, output, strlen(output) + 1, 0, ALL_CLIENT);
			}
		}
	}else{ // Delete Alliance|Opposition
		for(i=0;i<2-(flag&1);i++)
		{
			if(g[i]!=NULL)
			{
				ARR_FIND( 0, MAX_GUILDALLIANCE, j, g[i]->alliance[j].guild_id == guild_id[1-i] && g[i]->alliance[j].opposition == (flag&1) );
				if( j < MAX_GUILDALLIANCE )
				{
					if( battle_config.guild_wars && g[i]->alliance[j].war && (flag&1) )
					{
						guild_war_end(g[0], g[1], true);
						guild_war_end(g[1], g[0], false);
					}

					g[i]->alliance[j].guild_id = 0;
				}
			}
			if( sd[i]!=NULL )	// 解消通知
				clif_guild_delalliance(sd[i],guild_id[1-i],(flag&1));
		}
	}

	if((flag&0x0f)==0){			// 同盟通知
		if( sd[1]!=NULL )
			clif_guild_allianceack(sd[1],2);
	}else if((flag&0x0f)==1){	// 敵対通知
		if( sd[0]!=NULL )
			clif_guild_oppositionack(sd[0],0);
	}


	for(i=0;i<2-(flag&1);i++){	// 同盟/敵対リストの再送信
		struct map_session_data *sd;
		if(g[i]!=NULL)
			for(j=0;j<g[i]->max_member;j++)
				if((sd=g[i]->member[j].sd)!=NULL)
					clif_guild_allianceinfo(sd);
	}
	return 0;
}
// ギルド解散通知用
int guild_broken_sub(DBKey key,void *data,va_list ap)
{
	struct guild *g=(struct guild *)data;
	int guild_id=va_arg(ap,int);
	int i,j;
	struct map_session_data *sd=NULL;

	nullpo_ret(g);

	for(i=0;i<MAX_GUILDALLIANCE;i++){	// 関係を破棄
		if(g->alliance[i].guild_id==guild_id){
			for(j=0;j<g->max_member;j++)
				if( (sd=g->member[j].sd)!=NULL )
					clif_guild_delalliance(sd,guild_id,g->alliance[i].opposition);
			intif_guild_alliance(g->guild_id, guild_id,0,0,g->alliance[i].opposition|8);
			g->alliance[i].guild_id=0;
		}
	}
	return 0;
}


//Innvoked on /breakguild "Guild name"
int guild_broken(int guild_id, int flag)
{
	struct guild* g = guild_search(guild_id);
	struct map_session_data* sd;
	int i;

	if( g == NULL )
		return 0;

	if( flag != 0 )
		return 0;

	//we call castle_event::OnGuildBreak of all castles of the guild
	//you can set all castle_events in the castle_db.txt
	for( i = 0; i < MAX_GUILDCASTLE; ++i )
	{
		char name[EVENT_NAME_LENGTH];

		struct guild_castle* gc = guild_castle_search(i);
		if( gc == NULL || gc->guild_id != guild_id )
			continue;

		safestrncpy(name, gc->castle_event, ARRAYLENGTH(name));
		npc_event_do(strcat(name,"::OnGuildBreak"));
	}

	// Guild Skills Timers
	for( i = 0; i < MAX_GUILDSKILL; ++i )
	{
		if( g->skill_block_timer[i] == INVALID_TIMER )
			continue;
		delete_timer(g->skill_block_timer[i], guild_block_skill_end);
	}

	// Guild Master Cleanup
	if( (sd = map_nick2sd(g->master)) != NULL )
	{
		// Guild Aura Changes here ...
		sd->state.gmaster_flag = false;
	}

	// update members
	for( i = 0; i < g->max_member; ++i )
	{
		struct map_session_data* sd = g->member[i].sd;
		if( sd == NULL )
			continue;

		if( sd->state.storage_flag == 2 )
			storage_guild_storage_quit(sd,1);

		sd->state.gmaster_flag = false;
		sd->status.guild_id = 0;
		clif_guild_broken(g->member[i].sd,0);
		clif_charnameupdate(sd);
		status_calc_pc(sd,0); // Regional System
	}

	guild_db->foreach(guild_db,guild_broken_sub,guild_id);
	guild_castle_onguildbreak(guild_id);
	guild_storage_delete(guild_id);
	idb_remove(guild_db,guild_id);
	return 0;
}

//Changes the Guild Master to the specified player. [Skotlex]
int guild_gm_change(int guild_id, struct map_session_data *sd)
{
	struct guild *g;
	nullpo_ret(sd);

	if (sd->status.guild_id != guild_id)
		return 0;
	
	g=guild_search(guild_id);

	nullpo_ret(g);

	if (strcmp(g->master, sd->status.name) == 0) //Nothing to change.
		return 0;

	//Notify servers that master has changed.
	intif_guild_change_gm(guild_id, sd->status.name, strlen(sd->status.name)+1);
	return 1;
}

//Notification from Char server that a guild's master has changed. [Skotlex]
int guild_gm_changed(int guild_id, int account_id, int char_id)
{
	struct guild *g;
	struct guild_member gm;
	char output[128];
	int pos, i;

	g=guild_search(guild_id);

	if (!g)
		return 0;

	for(pos=0; pos<g->max_member && !(
		g->member[pos].account_id==account_id &&
		g->member[pos].char_id==char_id);
		pos++);

	if (pos == 0 || pos == g->max_member) return 0;

	memcpy(&gm, &g->member[pos], sizeof (struct guild_member));
	memcpy(&g->member[pos], &g->member[0], sizeof(struct guild_member));
	memcpy(&g->member[0], &gm, sizeof(struct guild_member));

	g->member[pos].position = g->member[0].position;
	g->member[0].position = 0; //Position 0: guild Master.
	strcpy(g->master, g->member[0].name);

	sprintf(output, "The Guild Master of [%s] has been changed to [%s]", g->name, g->master);
	clif_broadcast(NULL, output, strlen(output) + 1, 0, ALL_CLIENT);

	if (g->member[pos].sd && g->member[pos].sd->fd)
	{
		if( battle_config.at_changegm_cost && g->member[pos].sd->status.zeny >= battle_config.at_changegm_cost )
			pc_payzeny(g->member[pos].sd, battle_config.at_changegm_cost);

		clif_displaymessage(g->member[pos].sd->fd, "You no longer are the Guild Master.");
		g->member[pos].sd->state.gmaster_flag = false;
	}
	
	if (g->member[0].sd && g->member[0].sd->fd)
	{
		clif_displaymessage(g->member[0].sd->fd, "You have become the Guild Master!");
		g->member[0].sd->state.gmaster_flag = true;
	}

	// announce the change to all guild members
	for( i = 0; i < g->max_member; i++ )
	{
		if( g->member[i].sd && g->member[i].sd->fd && !(battle_config.bg_eAmod_mode && g->member[i].sd->bg_id) )
		{
			clif_guild_basicinfo(g->member[i].sd);
			clif_guild_memberlist(g->member[i].sd);
		}
	}

	return 1;
}

// ギルド解散
int guild_break(struct map_session_data *sd,char *name)
{
	struct guild *g;
	int i;

	nullpo_ret(sd);

	if( (g=guild_search(sd->status.guild_id))==NULL )
		return 0;
	if(strcmp(g->name,name)!=0)
		return 0;
	if(!sd->state.gmaster_flag)
		return 0;
	for(i=0;i<g->max_member;i++){
		if(	g->member[i].account_id>0 && (
			g->member[i].account_id!=sd->status.account_id ||
			g->member[i].char_id!=sd->status.char_id ))
			break;
	}
	if(i<g->max_member){
		clif_guild_broken(sd,2);
		return 0;
	}

	intif_guild_break(g->guild_id);
	return 0;
}


/*------------------------------------------
 * Guild Ranking System
 *------------------------------------------*/
int guild_ranking_save(int flag)
{
	struct guild_castle *gc;
	DBIterator* iter;
	struct guild *g;
	struct map_session_data *sd;
	int i, j, m, index, cc;

	for( i = 0; i < MAX_GUILDCASTLE; i++ )
	{ // Castle Score Update
		if( (gc = guild_castle_search(i)) == NULL || gc->guild_id == 0 )
			continue;
		
		if( woe_set && (m = map_mapindex2mapid(gc->mapindex)) >= 0 && map[m].flag.woe_set != woe_set )
			continue; // Not considered on this ranking

		index = gc->castle_id;

		if( (flag == 1 && index >= 24) || (flag == 2 && index < 24) )
			continue;

		if( (g = guild_search(gc->guild_id)) != NULL )
		{
			int addtime = DIFF_TICK(last_tick, gc->capture_tick),
				score = (addtime / 300) * (1 + (gc->economy / 25));

			g->castle[index].capture++;

			g->castle[index].posesion_time += addtime;
			g->castle[index].defensive_score += score;

			g->castle[index].changed = true;

			// Capture counter for members
			for( j = 0; j < MAX_GUILD; j++ )
			{
				if( (sd = g->member[j].sd) == NULL )
					continue;

				cc = pc_readaccountreg(sd,"#GC_CAPTURES");
				pc_setaccountreg(sd,"#GC_CAPTURES",++cc);
			}
		}
	}

	iter = guild_db->iterator(guild_db);
	for( g = (struct guild*)iter->first(iter,NULL); iter->exists(iter); g = (struct guild*)iter->next(iter,NULL) )
	{
		for( i = 0; i < MAX_GUILDCASTLE; i++ )
		{
			if( !g->castle[i].changed )
				continue;

			intif_guild_save_score(g->guild_id, i, &g->castle[i]);
			g->castle[i].changed = false;
		}
	}
	iter->destroy(iter);
	return 0;
}

int guild_agit_start(void)
{	// Run All NPC_Event[OnAgitStart]
	int c = npc_event_doall("OnAgitStart");
	struct guild_castle *gc;
	ShowStatus("NPC_Event:[OnAgitStart] Run (%d) Events by @AgitStart.\n",c);
	for( c = 0; c < MAX_GUILDCASTLE; c++ )
	{
		if( (gc = guild_castle_search(c)) == NULL || gc->guild_id == 0 || gc->castle_id >= 24 )
			continue;
		gc->capture_tick = last_tick;
	}
	return 0;
}

int guild_agit_end(void)
{	// Run All NPC_Event[OnAgitEnd]
	int c = npc_event_doall("OnAgitEnd");
	ShowStatus("NPC_Event:[OnAgitEnd] Run (%d) Events by @AgitEnd.\n",c);
	// Stop auto saving
	guild_ranking_save(1);
	return 0;
}

int guild_agit2_start(void)
{	// Run All NPC_Event[OnAgitStart2]
	int c = npc_event_doall("OnAgitStart2");
	struct guild_castle* gc;
	ShowStatus("NPC_Event:[OnAgitStart2] Run (%d) Events by @AgitStart2.\n",c);
	for( c = 0; c < MAX_GUILDCASTLE; c++ )
	{
		if( (gc = guild_castle_search(c)) == NULL || gc->guild_id == 0 || gc->castle_id < 24 )
			continue;
		gc->capture_tick = last_tick;
	}
	return 0;
}

int guild_agit2_end(void)
{	// Run All NPC_Event[OnAgitEnd2]
	int c = npc_event_doall("OnAgitEnd2");
	ShowStatus("NPC_Event:[OnAgitEnd2] Run (%d) Events by @AgitEnd2.\n",c);
	// Stop auto saving
	guild_ranking_save(2);
	return 0;
}

// Are these two guilds allied?
bool guild_isallied(int guild_id, int guild_id2)
{
	int i;
	struct guild* g = guild_search(guild_id);
	nullpo_ret(g);

	ARR_FIND( 0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == guild_id2 );
	return( i < MAX_GUILDALLIANCE && g->alliance[i].opposition == 0 );
}

static int guild_infoevent_db_final(DBKey key,void *data,va_list ap)
{
	aFree(data);
	return 0;
}


void do_init_guild(void)
{
	guild_db=idb_alloc(DB_OPT_RELEASE_DATA);
	guild_infoevent_db=idb_alloc(DB_OPT_BASE);

	memset(guild_skill_tree,0,sizeof(guild_skill_tree));
	sv_readdb(db_path, "guild_skill_tree.txt", ',', 2+MAX_GUILD_SKILL_REQUIRE*2, 2+MAX_GUILD_SKILL_REQUIRE*2, -1, &guild_read_guildskill_tree_db); //guild skill tree [Komurka]

	add_timer_func_list(guild_send_xy_timer, "guild_send_xy_timer");
	add_timer_interval(gettick()+GUILD_SEND_XY_INVERVAL,guild_send_xy_timer,0,0,GUILD_SEND_XY_INVERVAL);

	do_init_guild_castle();
	do_init_guild_expcache();
}

void do_final_guild(void)
{
	guild_db->destroy(guild_db,NULL);
	guild_infoevent_db->destroy(guild_infoevent_db,guild_infoevent_db_final);

	do_final_guild_castle();
	do_final_guild_expcache();
}
