#include "game.h"

namespace game
{
    bool intermission = false;
    int maptime = 0, maprealtime = 0, maplimit = -1;
    int respawnent = -1;
    int lasthit = 0, lastspawnattempt = 0;
	VAR(quasiradarhackenabled,0,0,1);
	VAR(quasiradarhackteam,0,0,1);
	VAR(quasiradarhackspawn,0,0,1);
	

    int following = -1, followdir = 0;

    fpsent *player1 = NULL, qplayer;         // our client
    vector<fpsent *> players;       // other clients
    int savedammo[NUMGUNS];

    bool clientoption(const char *arg) { return false; }

    void taunt()
    {
        if(player1->state!=CS_ALIVE || player1->physstate<PHYS_SLOPE) return;
        if(lastmillis-player1->lasttaunt<1000) return;
        player1->lasttaunt = lastmillis;
        addmsg(N_TAUNT, "rc", player1);
    }
    COMMAND(taunt, "");

    ICOMMAND(getfollow, "", (),
    {
        fpsent *f = followingplayer();
        intret(f ? f->clientnum : -1);
    });

	void follow(char *arg)
    {
        if(arg[0] ? player1->state==CS_SPECTATOR : following>=0)
        {
            following = arg[0] ? parseplayer(arg) : -1;
            if(following==player1->clientnum) following = -1;
            followdir = 0;
            conoutf("follow %s", following>=0 ? "on" : "off");
        }
	}
    COMMAND(follow, "s");

    void nextfollow(int dir)
    {
        if(player1->state!=CS_SPECTATOR || clients.empty())
        {
            stopfollowing();
            return;
        }
        int cur = following >= 0 ? following : (dir < 0 ? clients.length() - 1 : 0);
        loopv(clients)
        {
            cur = (cur + dir + clients.length()) % clients.length();
            if(clients[cur] && clients[cur]->state!=CS_SPECTATOR)
            {
                if(following<0) conoutf("follow on");
                following = cur;
                followdir = dir;
                return;
            }
        }
        stopfollowing();
    }
    ICOMMAND(nextfollow, "i", (int *dir), nextfollow(*dir < 0 ? -1 : 1));


    const char *getclientmap() { return clientmap; }

    void resetgamestate()
    {
        if(m_classicsp)
        {
            clearmovables();
            clearmonsters();                 // all monsters back at their spawns for editing
            entities::resettriggers();
        }
        clearprojectiles();
        clearbouncers();
    }

    fpsent *spawnstate(fpsent *d)              // reset player state not persistent accross spawns
    {
        d->respawn();
        d->spawnstate(gamemode);
        return d;
    }

    void respawnself()
    {
        if(paused || ispaused()) return;
        if(m_mp(gamemode))
        {
            if(player1->respawned!=player1->lifesequence)
            {
                addmsg(N_TRYSPAWN, "rc", player1);
                player1->respawned = player1->lifesequence;
            }
        }
        else
        {
            spawnplayer(player1);
            showscores(false);
            lasthit = 0;
            if(cmode) cmode->respawned(player1);
        }
    }

    fpsent *pointatplayer()
    {
        loopv(players) if(players[i] != player1 && intersect(players[i], player1->o, worldpos)) return players[i];
        return NULL;
    }

	//Find a player who is pointing at d.
	fpsent *pointingatplayer()
	{
		if(player1->state != CS_ALIVE) return NULL;
		int worldsize = getworldsize();
		vec etarg;
		loopv(players)
		{
			vecfromyawpitch(players[i]->yaw,players[i]->pitch,1,0,etarg);
			etarg = vec(etarg).mul(2*worldsize).add(players[i]->o);
			if(players[i] != player1 && players[i]->state == CS_ALIVE && !isteam(players[i]->team,player1->team) && qintersect(player1,players[i]->o,etarg)) return players[i];
		}
		return NULL;
	}

    void stopfollowing()
    {
        if(following<0) return;
        following = -1;
        followdir = 0;
        conoutf("follow off");
    }

    fpsent *followingplayer()
    {
        if(player1->state!=CS_SPECTATOR || following<0) return NULL;
        fpsent *target = getclient(following);
        if(target && target->state!=CS_SPECTATOR) return target;
        return NULL;
    }

    fpsent *hudplayer()
    {
        if(thirdperson) return player1;
        fpsent *target = followingplayer();
        return target ? target : player1;
    }

    void setupcamera()
    {
        fpsent *target = followingplayer();
        if(target)
        {
            player1->yaw = target->yaw;
            player1->pitch = target->state==CS_DEAD ? 0 : target->pitch;
            player1->o = target->o;
            player1->resetinterp();
        }
    }

    bool detachcamera()
    {
        fpsent *d = hudplayer();
        return d->state==CS_DEAD;
    }

    bool collidecamera()
    {
        switch(player1->state)
        {
            case CS_EDITING: return false;
            case CS_SPECTATOR: return followingplayer()!=NULL;
        }
        return true;
    }

    VARP(smoothmove, 0, 75, 100);
    VARP(smoothdist, 0, 32, 64);

    void predictplayer(fpsent *d, bool move)
    {
        d->o = d->newpos;
        d->yaw = d->newyaw;
        d->pitch = d->newpitch;
        if(move)
        {
            moveplayer(d, 1, false);
            d->newpos = d->o;
        }
        float k = 1.0f - float(lastmillis - d->smoothmillis)/smoothmove;
        if(k>0)
        {
            d->o.add(vec(d->deltapos).mul(k));
            d->yaw += d->deltayaw*k;
            if(d->yaw<0) d->yaw += 360;
            else if(d->yaw>=360) d->yaw -= 360;
            d->pitch += d->deltapitch*k;
        }
    }

    void otherplayers(int curtime)
    {
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == player1 || d->ai) continue;

            if(d->state==CS_ALIVE)
            {
                if(lastmillis - d->lastaction >= d->gunwait) d->gunwait = 0;
                if(d->quadmillis) entities::checkquad(curtime, d);
            }
            else if(d->state==CS_DEAD && d->ragdoll) moveragdoll(d);

            const int lagtime = lastmillis-d->lastupdate;
            if(!lagtime || intermission) continue;
            else if(lagtime>1000 && d->state==CS_ALIVE)
            {
                d->state = CS_LAGGED;
                continue;
            }
            if(d->state==CS_ALIVE || d->state==CS_EDITING)
            {
                if(smoothmove && d->smoothmillis>0) predictplayer(d, true);
                else moveplayer(d, 1, false);
            }
            else if(d->state==CS_DEAD && !d->ragdoll && lastmillis-d->lastpain<2000) moveplayer(d, 1, true);
        }
    }

    VARFP(slowmosp, 0, 0, 1,
    {
        if(m_sp && !slowmosp) setvar("gamespeed", 100);
    });

    void checkslowmo()
    {
        static int lastslowmohealth = 0;
        setvar("gamespeed", intermission ? 100 : clamp(player1->health, 25, 200), true, false);
        if(player1->health<player1->maxhealth && lastmillis-max(maptime, lastslowmohealth)>player1->health*player1->health/2)
        {
            lastslowmohealth = lastmillis;
            player1->health++;
        }
    }
    VAR(quasiantiintermission,0,0,1);
    void updateworld()        // main game update loop
    {
        if(!maptime) { maptime = lastmillis; maprealtime = totalmillis; return; }
        if(!curtime) { gets2c(); if(player1->clientnum>=0) c2sinfo(); return; }

        physicsframe();
        ai::navigate();
        entities::checkquad(curtime, player1);
        updateweapons(curtime);
        otherplayers(curtime);
        ai::update();
        moveragdolls();
        gets2c();
        updatemovables(curtime);
        updatemonsters(curtime);
        if(player1->state==CS_DEAD)
        {
            if(player1->ragdoll) moveragdoll(player1);
            else if(lastmillis-player1->lastpain<2000)
            {
                player1->move = player1->strafe = 0;
                moveplayer(player1, 10, false);
            }
        }
        else if(!intermission || quasiantiintermission)
        {
            if(player1->ragdoll) cleanragdoll(player1);
            moveplayer(player1, 10, true);
            swayhudgun(curtime);
            entities::checkitems(player1);
            if(m_sp)
            {
                if(slowmosp) checkslowmo();
                if(m_classicsp) entities::checktriggers();
            }
            else if(cmode) cmode->checkitems(player1);
        }
        if(player1->clientnum>=0) c2sinfo();   // do this last, to reduce the effective frame lag
    }

    void spawnplayer(fpsent *d)   // place at random spawn
    {
        if(cmode) cmode->pickspawn(d);
        else findplayerspawn(d, d==player1 && respawnent>=0 ? respawnent : -1);
        spawnstate(d);
        if(d==player1)
        {
            if(editmode) d->state = CS_EDITING;
            else if(d->state != CS_SPECTATOR) d->state = CS_ALIVE;
        }
        else d->state = CS_ALIVE;
    }

    VARP(spawnwait, 0, 0, 1000);

	VAR(quasirespawnenabled, 0, 0, 1);
	VARR(qrespawn, 0 , 0, 1);
    void respawn()
    {
        if(player1->state==CS_DEAD)
        {
            player1->attacking = false;
            int wait = cmode ? cmode->respawnwait(player1) : 0;
            if(wait>0)
            {
				if(qrespawn == 0 || lastmillis - lastspawnattempt >= 500) {
					if(quasirespawnenabled == 1) qrespawn = 1;
					lastspawnattempt = lastmillis;
					//conoutf(CON_GAMEINFO, "\f2you must wait %d second%s before respawn!", wait, wait!=1 ? "s" : "");
					return;
				}
            }
			qrespawn = 0;
            if(lastmillis < player1->lastpain + spawnwait) return;
            if(m_dmsp) { changemap(clientmap, gamemode); return; }    // if we die in SP we try the same map again
            respawnself();
            if(m_classicsp)
            {
                conoutf(CON_GAMEINFO, "\f2You wasted another life! The monsters stole your armour and some ammo...");
                loopi(NUMGUNS) if(i!=GUN_PISTOL && (player1->ammo[i] = savedammo[i]) > 5) player1->ammo[i] = max(player1->ammo[i]/3, 5);
            }
        }
    }

    // inputs

    void doattack(bool on)
    {
        if(intermission && !quasiantiintermission) return;
        if((player1->attacking = on)) respawn();
    }

	void doquasiattack(bool on)
    {
        if(intermission  && !quasiantiintermission) return;
        if((player1->qattackbot = on)) respawn();
    }
	void doquasikill(char * arg) {
		int cn = parseplayer(arg);
		if(cn == -1) return;
		fpsent *t;
		loopv(players) { if(players[i]->clientnum == cn) t = players[i]; }
		conoutf(CON_GAMEINFO,"Shoot %s",t->name);
		player1->attacking = true;
		shoot(player1, t->o, t->feetpos());
		player1->attacking = false;
	}
	ICOMMAND(quasikill, "s", (char * arg), { doquasikill(arg); });
	VAR(quasileapenabled,0,0,1);
	VAR(quasileapautorestore,0,0,1);
	void doquasirestore(int q = 0)
	{
		if(player1->state == CS_QLEAP || player1->state == CS_SPECTATOR)
		{
			if(q == 0) {//restore position only 
				player1->o = qplayer.o; //recover only the position.
			} else {
				*player1 = qplayer; //recover everything.
			}
			player1->reset();
		}
	}
	void doquasileap()
	{
		if(player1->state == CS_ALIVE && quasileapenabled == 1) //Only When Alive
		{
			qplayer = *player1; //Save
			player1->state = CS_QLEAP;
		}
		else if(player1->state == CS_QLEAP)
		{
			if(quasileapautorestore == 1) doquasirestore(1); //restore pos
			else player1->state = qplayer.state; //Recover State
		}
	}
	ICOMMAND(quasileap, "", (), { doquasileap(); });
	ICOMMAND(quasirestore, "i", (int q), { doquasirestore(q); });
	VARR(qspec,0,0,1);
	void doquasispec()
	{
		if(player1->state == CS_ALIVE || player1->state == CS_DEAD || player1->state == CS_EDITING || (player1->state == CS_SPECTATOR && qspec == 1))
		{
			if(qspec == 0)
			{
				qplayer = *player1; //Save
				player1->state = CS_SPECTATOR;
				qspec = 1;
			}
			else
			{
				doquasirestore(1);
				qspec = 0;
			}
		}
	}
	ICOMMAND(quasispec,"",(),{ doquasispec(); });
	vec qteleo;
	float qtelepitch,qteleyaw;
	void quasitelesetdest()
	{
		qteleo = player1->o;
		qteleyaw = player1->yaw;
		qtelepitch = player1->pitch;
		conoutf(CON_GAMEINFO,"Set Destination: %f %f %f",qteleo.x,qteleo.y,qteleo.z);
	}
	void quasitelegotodest()
	{
		conoutf(CON_GAMEINFO,"Set Destination: %f %f %f",qteleo.x,qteleo.y,qteleo.z);
		player1->o = qteleo;
		player1->yaw = qteleyaw;
		player1->pitch = qtelepitch;
		entinmap(player1);
        updatedynentcache(player1);
	}
	ICOMMAND(quasiset,"",(), { quasitelesetdest(); });
	ICOMMAND(quasigoto,"",(), { quasitelegotodest(); });
	VAR(qrageon,0,0,1);
	VAR(qrage,0,0,1000);
	void doquasirage() {
		qrageon = 1;
		qrage = 0;
	}
	//ICOMMAND(quasirage,"",(), { doquasirage(); });
	void quasiping(char *text) { 
		//char prefix[1];
		//prefix[0] = char(27);
		//char * newstr = strcat(prefix,text);
		conoutf(CON_CHAT, "\f4QuasiPing: %s", text);
		addmsg(N_SAYTEAM, "rcs", player1, text); 
	}
    COMMAND(quasiping, "C");

    void quasictfcap(int * ct) {
        if(!cmode) {
            conoutf("\f3Unable to capture flag!");
            return;
        }
        setvar("quasictfcapct",*ct < 1 ? 1 : *ct);
    }
    COMMAND(quasictfcap,"i");
    bool canjump()
    {
        if(!intermission || quasiantiintermission) respawn();
        return player1->state!=CS_DEAD && (!intermission || quasiantiintermission);
    }

    bool allowmove(physent *d)
    {
        if(d->type!=ENT_PLAYER) return true;
        return !((fpsent *)d)->lasttaunt || lastmillis-((fpsent *)d)->lasttaunt>=1000;
    }

    VARP(hitsound, 0, 0, 1);

    void damaged(int damage, fpsent *d, fpsent *actor, bool local)
    {
        if(d->state!=CS_ALIVE || (intermission && !quasiantiintermission)) return;

        if(local) damage = d->dodamage(damage);
        else if(actor==player1) return;

        fpsent *h = hudplayer();
        if(h!=player1 && actor==h && d!=actor)
        {
            if(hitsound && lasthit != lastmillis) playsound(S_HIT);
            lasthit = lastmillis;
        }
        if(d==h)
        {
            damageblend(damage);
            damagecompass(damage, actor->o);
        }
        damageeffect(damage, d, d!=h);

		ai::damaged(d, actor);

        if(m_sp && slowmosp && d==player1 && d->health < 1) d->health = 1;

        if(d->health<=0) { if(local) killed(d, actor); }
        else if(d==h) playsound(S_PAIN6);
        else playsound(S_PAIN1+rnd(5), &d->o);
    }

    VARP(deathscore, 0, 1, 1);

    void deathstate(fpsent *d, bool restore)
    {
        d->state = CS_DEAD;
        d->lastpain = lastmillis;
        if(!restore) gibeffect(max(-d->health, 0), d->vel, d);
        if(d==player1)
        {
            if(deathscore) showscores(true);
            disablezoom();
            if(!restore) loopi(NUMGUNS) savedammo[i] = player1->ammo[i];
            d->attacking = false;
            if(!restore) d->deaths++;
            //d->pitch = 0;
            d->roll = 0;
            playsound(S_DIE1+rnd(2));
        }
        else
        {
            d->move = d->strafe = 0;
            d->resetinterp();
            d->smoothmillis = 0;
            playsound(S_DIE1+rnd(2), &d->o);
        }
    }

    void killed(fpsent *d, fpsent *actor)
    {
        if(d->state==CS_EDITING)
        {
            d->editstate = CS_DEAD;
            if(d==player1) d->deaths++;
            else d->resetinterp();
            return;
        }
		else if((d->state!=CS_ALIVE && d->state != CS_QLEAP && getvar("qspec") != 1)|| (intermission && !quasiantiintermission)) return;
		if(d == player1 && getvar("qspec") == 1) d = &qplayer; //make the changes to our save. Dont drop out of spectator though.
        fpsent *h = followingplayer();
        if(!h) h = player1;
        int contype = d==h || actor==h ? CON_FRAG_SELF : CON_FRAG_OTHER;
        string dname, aname;
        copystring(dname, d==player1 ? "you" : colorname(d));
        copystring(aname, actor==player1 ? "you" : colorname(actor));
        if(actor->type==ENT_AI)
            conoutf(contype, "\f2%s got killed by %s!", dname, aname);
        else if(d==actor || actor->type==ENT_INANIMATE)
            conoutf(contype, "\f2%s suicided%s", dname, d==player1 ? "!" : "");
        else if(isteam(d->team, actor->team))
        {
            contype |= CON_TEAMKILL;
            if(actor==player1) conoutf(contype, "\f3you fragged a teammate (%s)", dname);
            else if(d==player1) conoutf(contype, "\f3you got fragged by a teammate (%s)", aname);
            else conoutf(contype, "\f2%s fragged a teammate (%s)", aname, dname);
        }
        else
        {
            if(d==player1) conoutf(contype, "\f2you got fragged by %s", aname);
            else conoutf(contype, "\f2%s fragged %s", aname, dname);
        }
        deathstate(d);
		ai::killed(d, actor);
    }

    void timeupdate(int secs)
    {
        if(secs > 0)
        {
            maplimit = lastmillis + secs*1000;
        }
        else
        {
            intermission = true;
            player1->attacking = false;
            if(cmode) cmode->gameover();
            conoutf(CON_GAMEINFO, "\f2intermission:");
            conoutf(CON_GAMEINFO, "\f2game has ended!");
            if(m_ctf) conoutf(CON_GAMEINFO, "\f2player frags: %d, flags: %d, deaths: %d", player1->frags, player1->flags, player1->deaths);
            else conoutf(CON_GAMEINFO, "\f2player frags: %d, deaths: %d", player1->frags, player1->deaths);
            int accuracy = (player1->totaldamage*100)/max(player1->totalshots, 1);
            conoutf(CON_GAMEINFO, "\f2player total damage dealt: %d, damage wasted: %d, accuracy(%%): %d", player1->totaldamage, player1->totalshots-player1->totaldamage, accuracy);
            if(m_sp) spsummary(accuracy);

            showscores(true);
            disablezoom();
            
            if(identexists("intermission")) execute("intermission");
        }
    }

    ICOMMAND(getfrags, "", (), intret(player1->frags));
    ICOMMAND(getflags, "", (), intret(player1->flags));
    ICOMMAND(getdeaths, "", (), intret(player1->deaths));
    ICOMMAND(getaccuracy, "", (), intret((player1->totaldamage*100)/max(player1->totalshots, 1)));
    ICOMMAND(gettotaldamage, "", (), intret(player1->totaldamage));
    ICOMMAND(gettotalshots, "", (), intret(player1->totalshots));

    vector<fpsent *> clients;

    fpsent *newclient(int cn)   // ensure valid entity
    {
        if(cn < 0 || cn > max(0xFF, MAXCLIENTS + MAXBOTS))
        {
            neterr("clientnum", false);
            return NULL;
        }

        if(cn == player1->clientnum) return player1;

        while(cn >= clients.length()) clients.add(NULL);
        if(!clients[cn])
        {
            fpsent *d = new fpsent;
            d->clientnum = cn;
            clients[cn] = d;
            players.add(d);
        }
        return clients[cn];
    }

    fpsent *getclient(int cn)   // ensure valid entity
    {
        if(cn == player1->clientnum) return player1;
        return clients.inrange(cn) ? clients[cn] : NULL;
    }

    void clientdisconnected(int cn, bool notify)
    {
        if(!clients.inrange(cn)) return;
        if(following==cn)
        {
            if(followdir) nextfollow(followdir);
            else stopfollowing();
        }
        fpsent *d = clients[cn];
        if(!d) return;
        if(notify && d->name[0]) conoutf("player %s disconnected", colorname(d));
        removeweapons(d);
        removetrackedparticles(d);
        removetrackeddynlights(d);
        if(cmode) cmode->removeplayer(d);
        players.removeobj(d);
        DELETEP(clients[cn]);
        cleardynentcache();
    }

    void clearclients(bool notify)
    {
        loopv(clients) if(clients[i]) clientdisconnected(i, notify);
    }

    void initclient()
    {
        player1 = spawnstate(new fpsent);
        players.add(player1);
    }

    VARP(showmodeinfo, 0, 1, 1);

    void startgame()
    {
        clearmovables();
        clearmonsters();

        clearprojectiles();
        clearbouncers();
        clearragdolls();

        // reset perma-state
        loopv(players)
        {
            fpsent *d = players[i];
            d->frags = d->flags = 0;
            d->deaths = 0;
            d->totaldamage = 0;
            d->totalshots = 0;
            d->maxhealth = 100;
            d->lifesequence = -1;
            d->respawned = d->suicided = -2;
        }

        setclientmode();

        intermission = false;
        maptime = maprealtime = 0;
        maplimit = -1;

        if(cmode)
        {
            cmode->preload();
            cmode->setup();
        }

        conoutf(CON_GAMEINFO, "\f2game mode is %s", server::modename(gamemode));

        if(m_sp)
        {
            defformatstring(scorename)("bestscore_%s", getclientmap());
            const char *best = getalias(scorename);
            if(*best) conoutf(CON_GAMEINFO, "\f2try to beat your best score so far: %s", best);
        }
        else
        {
            const char *info = m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
            if(showmodeinfo && info) conoutf(CON_GAMEINFO, "\f0%s", info);
        }

        if(player1->playermodel != playermodel) switchplayermodel(playermodel);

        showscores(false);
        disablezoom();
        lasthit = 0;

        if(identexists("mapstart")) execute("mapstart");
    }

    void startmap(const char *name)   // called just after a map load
    {
        ai::savewaypoints();
        ai::clearwaypoints(true);

        respawnent = -1; // so we don't respawn at an old spot
        if(!m_mp(gamemode)) spawnplayer(player1);
        else findplayerspawn(player1, -1);
        entities::resetspawns();
        copystring(clientmap, name ? name : "");
        
        sendmapinfo();
    }

    const char *getmapinfo()
    {
        return showmodeinfo && m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material)
    {
        if(d->type==ENT_INANIMATE) return;
        if     (waterlevel>0) { if(material!=MAT_LAVA) playsound(S_SPLASH1, d==player1 ? NULL : &d->o); }
        else if(waterlevel<0) playsound(material==MAT_LAVA ? S_BURN : S_SPLASH2, d==player1 ? NULL : &d->o);
        if     (floorlevel>0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_JUMP, d); }
        else if(floorlevel<0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_LAND, d); }
    }

    void dynentcollide(physent *d, physent *o, const vec &dir)
    {
        switch(d->type)
        {
            case ENT_AI: if(dir.z > 0) stackmonster((monster *)d, o); break;
            case ENT_INANIMATE: if(dir.z > 0) stackmovable((movable *)d, o); break;
        }
    }

    void msgsound(int n, physent *d)
    {
        if(!d || d==player1)
        {
            addmsg(N_SOUND, "ci", d, n);
            playsound(n);
        }
        else
        {
            if(d->type==ENT_PLAYER && ((fpsent *)d)->ai)
                addmsg(N_SOUND, "ci", d, n);
            playsound(n, &d->o);
        }
    }

    int numdynents() { return players.length()+monsters.length()+movables.length(); }

    dynent *iterdynents(int i)
    {
        if(i<players.length()) return players[i];
        i -= players.length();
        if(i<monsters.length()) return (dynent *)monsters[i];
        i -= monsters.length();
        if(i<movables.length()) return (dynent *)movables[i];
        return NULL;
    }

    bool duplicatename(fpsent *d, const char *name = NULL)
    {
        if(!name) name = d->name;
        loopv(players) if(d!=players[i] && !strcmp(name, players[i]->name)) return true;
        return false;
    }

    const char *colorname(fpsent *d, const char *name, const char *prefix)
    {
        if(!name) name = d->name;
        if(name[0] && !duplicatename(d, name) && d->aitype == AI_NONE) return name;
        static string cname[3];
        static int cidx = 0;
        cidx = (cidx+1)%3;
        formatstring(cname[cidx])(d->aitype == AI_NONE ? "%s%s \fs\f5(%d)\fr" : "%s%s \fs\f5[%d]\fr", prefix, name, d->clientnum);
        return cname[cidx];
    }

    void suicide(physent *d)
    {
        if(d==player1 || (d->type==ENT_PLAYER && ((fpsent *)d)->ai))
        {
            if(d->state!=CS_ALIVE) return;
            fpsent *pl = (fpsent *)d;
            if(!m_mp(gamemode)) killed(pl, pl);
            else if(pl->suicided!=pl->lifesequence)
            {
                addmsg(N_SUICIDE, "rc", pl);
                pl->suicided = pl->lifesequence;
            }
        }
        else if(d->type==ENT_AI) suicidemonster((monster *)d);
        else if(d->type==ENT_INANIMATE) suicidemovable((movable *)d);
    }
    ICOMMAND(kill, "", (), suicide(player1));

    bool needminimap() { return m_ctf || m_protect || m_hold || m_capture || true; }

    void drawicon(int icon, float x, float y, float sz)
    {
        settexture("packages/hud/items.png");
        glBegin(GL_TRIANGLE_STRIP);
        float tsz = 0.25f, tx = tsz*(icon%4), ty = tsz*(icon/4);
        glTexCoord2f(tx,     ty);     glVertex2f(x,    y);
        glTexCoord2f(tx+tsz, ty);     glVertex2f(x+sz, y);
        glTexCoord2f(tx,     ty+tsz); glVertex2f(x,    y+sz);
        glTexCoord2f(tx+tsz, ty+tsz); glVertex2f(x+sz, y+sz);
        glEnd();
    }

    float abovegameplayhud(int w, int h)
    {
        switch(hudplayer()->state)
        {
            case CS_EDITING:
            case CS_SPECTATOR:
                return 1;
            default:
                return 1650.0f/1800.0f;
        }
    }

    int ammohudup[3] = { GUN_CG, GUN_RL, GUN_GL },
        ammohuddown[3] = { GUN_RIFLE, GUN_SG, GUN_PISTOL },
        ammohudcycle[7] = { -1, -1, -1, -1, -1, -1, -1 };

    ICOMMAND(ammohudup, "sss", (char *w1, char *w2, char *w3),
    {
        int i = 0;
        if(w1[0]) ammohudup[i++] = parseint(w1);
        if(w2[0]) ammohudup[i++] = parseint(w2);
        if(w3[0]) ammohudup[i++] = parseint(w3);
        while(i < 3) ammohudup[i++] = -1;
    });

    ICOMMAND(ammohuddown, "sss", (char *w1, char *w2, char *w3),
    {
        int i = 0;
        if(w1[0]) ammohuddown[i++] = parseint(w1);
        if(w2[0]) ammohuddown[i++] = parseint(w2);
        if(w3[0]) ammohuddown[i++] = parseint(w3);
        while(i < 3) ammohuddown[i++] = -1;
    });

    ICOMMAND(ammohudcycle, "sssssss", (char *w1, char *w2, char *w3, char *w4, char *w5, char *w6, char *w7),
    {
        int i = 0;
        if(w1[0]) ammohudcycle[i++] = parseint(w1);
        if(w2[0]) ammohudcycle[i++] = parseint(w2);
        if(w3[0]) ammohudcycle[i++] = parseint(w3);
        if(w4[0]) ammohudcycle[i++] = parseint(w4);
        if(w5[0]) ammohudcycle[i++] = parseint(w5);
        if(w6[0]) ammohudcycle[i++] = parseint(w6);
        if(w7[0]) ammohudcycle[i++] = parseint(w7);
        while(i < 7) ammohudcycle[i++] = -1;
    });

    VARP(ammohud, 0, 1, 1);

    void drawammohud(fpsent *d)
    {
        float x = HICON_X + 2*HICON_STEP, y = HICON_Y, sz = HICON_SIZE;
        glPushMatrix();
        glScalef(1/3.2f, 1/3.2f, 1);
        float xup = (x+sz)*3.2f, yup = y*3.2f + 0.1f*sz;
        loopi(3)
        {
            int gun = ammohudup[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            drawicon(HICON_FIST+gun, xup, yup, sz);
            yup += sz;
        }
        float xdown = x*3.2f - sz, ydown = (y+sz)*3.2f - 0.1f*sz;
        loopi(3)
        {
            int gun = ammohuddown[3-i-1];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            ydown -= sz;
            drawicon(HICON_FIST+gun, xdown, ydown, sz);
        }
        int offset = 0, num = 0;
        loopi(7)
        {
            int gun = ammohudcycle[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL) continue;
            if(gun == d->gunselect) offset = i + 1;
            else if(d->ammo[gun]) num++;
        }
        float xcycle = (x+sz/2)*3.2f + 0.5f*num*sz, ycycle = y*3.2f-sz;
        loopi(7)
        {
            int gun = ammohudcycle[(i + offset)%7];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            xcycle -= sz;
            drawicon(HICON_FIST+gun, xcycle, ycycle, sz);
        }
        glPopMatrix();
    }

    void drawhudicons(fpsent *d)
    {
        glPushMatrix();
        glScalef(2, 2, 1);

        draw_textf("%d", (HICON_X + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->state==CS_DEAD ? 0 : d->health);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) draw_textf("%d", (HICON_X + HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->armour);
            draw_textf("%d", (HICON_X + 2*HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->ammo[d->gunselect]);
        }

        glPopMatrix();

        drawicon(HICON_HEALTH, HICON_X, HICON_Y);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) drawicon(HICON_BLUE_ARMOUR+d->armourtype, HICON_X + HICON_STEP, HICON_Y);
            drawicon(HICON_FIST+d->gunselect, HICON_X + 2*HICON_STEP, HICON_Y);
            if(d->quadmillis) drawicon(HICON_QUAD, HICON_X + 3*HICON_STEP, HICON_Y);
            if(ammohud) drawammohud(d);
        }
    }

	void qdrawradar(float x, float y, float s)
    {
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x,   y);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x+s, y);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x,   y+s);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x+s, y+s);
        glEnd();
    }

	float qcalcradarscale()
    {
        //return radarscale<=0 || radarscale>maxradarscale ? maxradarscale : max(radarscale, float(minradarscale));
        return clamp(max(minimapradius.x, minimapradius.y)/3, float(getvar("minradarscale")), float(getvar("maxradarscale")));
    }

	void qdrawblip(fpsent *d, float x, float y, float s, const vec &pos, bool flagblip, bool cutoff = false)
    {
        float scale = qcalcradarscale();
        vec dir = d->o;
        dir.sub(pos).div(scale);
        float size = flagblip ? 0.1f : 0.05f,
              xoffset = flagblip ? -2*(3/32.0f)*size : -size,
              yoffset = flagblip ? -2*(1 - 3/32.0f)*size : -size,
              dist = dir.magnitude2(), maxdist = 1 - 0.05f - 0.05f;
        if(dist >= maxdist) dir.mul(maxdist/dist);
        dir.rotate_around_z(-camera1->yaw*RAD);
        if(dist < maxdist || (dist >= maxdist && cutoff == false)) qdrawradar(x + s*0.5f*(1.0f + dir.x + xoffset), y + s*0.5f*(1.0f + dir.y + yoffset), size*s);
    }

    void qdrawminimap(fpsent *d, float x, float y, float s)
    {
        vec pos = vec(d->o).sub(minimapcenter).mul(minimapscale).add(0.5f), dir;
        vecfromyawpitch(camera1->yaw, 0, 1, 0, dir);
        float scale = qcalcradarscale();
        glBegin(GL_TRIANGLE_FAN);
        loopi(16)
        {
            vec tc = vec(dir).rotate_around_z(i/16.0f*2*M_PI);
            glTexCoord2f(pos.x + tc.x*scale*minimapscale.x, pos.y + tc.y*scale*minimapscale.y);
            vec v = vec(0, -1, 0).rotate_around_z(i/16.0f*2*M_PI);
            glVertex2f(x + 0.5f*s*(1.0f + v.x), y + 0.5f*s*(1.0f + v.y));
        }
        glEnd();
    }

	void qdrawotherhud(fpsent *d, int w, int h)
    {

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        int s = 1800/4, x = 1800*w/h - s - s/10, y = s/10;
        glColor4f(1, 1, 1, 1/*minimapalpha*/);
        if(1 /*minimapalpha*/>= 1) glDisable(GL_BLEND);
        bindminimap();
        qdrawminimap(d, x, y, s);
        if(1 /*minimapalpha*/ >= 1) glEnable(GL_BLEND);
        glColor3f(1, 1, 1);
        float margin = 0.04f, roffset = s*margin, rsize = s + 2*roffset;
        settexture("packages/hud/radar.png", 3);
        qdrawradar(x - roffset, y - roffset, rsize);
		if(quasiradarhackenabled == 1)
		{
			if(quasiradarhackspawn == 1)
			{
				const vector<extentity *> &qents = entities::getents();
				loopv(qents)
				{
					if(qents[i]->type == ET_PLAYERSTART &&  qents[i]->attr2 == 0) { //Make sure it's a player spawn, and the correct mode.
						settexture("quasimodo/sdot_blue.png", 1);
						qdrawblip(d, x, y, s, qents[i]->o, false, true);
					}
				}
			}
			loopv(players)
			{
				if(players[i] == player1 || (isteam(players[i]->team,player1->team) && quasiradarhackteam == 0) || (players[i]->state != CS_ALIVE && players[i]->state != CS_EDITING)) continue;
				if(isteam(players[i]->team,player1->team)) settexture("quasimodo/dot_blue.png", 2);
				else settexture("quasimodo/dot_red.png", 2);
				qdrawblip(d, x, y, s, players[i]->o, false);
			}
		}
    }

	VARP(quasinameplayers,0,0,1);
	fpsent qnameplayer;


    void gameplayhud(int w, int h)
    {
        glPushMatrix();
        glScalef(h/1800.0f, h/1800.0f, 1);

		int pw,ph;
		text_bounds("  ", pw, ph);
		if(player1->state==CS_SPECTATOR)
        {
            int tw, th, fw, fh;
            text_bounds("SPECTATOR", tw, th);
            th = max(th, ph);
            fpsent *f = followingplayer();
            text_bounds(f ? colorname(f) : " ", fw, fh);
            fh = max(fh, ph);
            draw_text("SPECTATOR", w*1800/h - tw - pw, 1650 - th - fh);
            if(f) draw_text(colorname(f), w*1800/h - fw - pw, 1650 - fh);
        }
		else if(player1->state == CS_QLEAP)
		{
			int tw, th, fw, fh;
            text_bounds("QUASIMODO LEAP", tw, th);
            th = max(th, ph);
            fpsent *f = followingplayer();
            text_bounds(f ? colorname(f) : " ", fw, fh);
            fh = max(fh, ph);
            draw_text("QUASIMODO LEAP", w*1800/h - tw - pw, 1650 - th - fh);
            if(f) draw_text(colorname(f), w*1800/h - fw - pw, 1650 - fh);
		}

		if(quasinameplayers == 1) {
			fpsent *t = pointatplayer();
			if(t) qnameplayer = *t;
			if(qnameplayer.name != "") draw_text(qnameplayer.name, 0, 900-ph);
		}
		draw_textf("\f6Frags: \f3%d \f6Deaths: \f3%d \f6Acc: \f3%d%%\n\f1TotalDamage: \f3%d \f1TotalShots: \f3%d", 1000, 50, player1->frags, player1->deaths, (player1->totaldamage*100)/max(player1->totalshots, 1), player1->totaldamage, player1->totalshots);
        fpsent *d = hudplayer();
        if(d->state!=CS_EDITING)
        {
            if(d->state!=CS_SPECTATOR) drawhudicons(d);
            if(cmode) cmode->drawhud(d, w, h);
			else if(quasiradarhackenabled == 1) qdrawotherhud(d, w, h);
        }

        glPopMatrix();
    }

    int clipconsole(int w, int h)
    {
        if(cmode) return cmode->clipconsole(w, h);
        return 0;
    }

    VARP(teamcrosshair, 0, 1, 1);
    VARP(hitcrosshair, 0, 425, 1000);

    const char *defaultcrosshair(int index)
    {
        switch(index)
        {
            case 2: return "data/hit.png";
            case 1: return "data/teammate.png";
            default: return "data/crosshair.png";
        }
    }

    int selectcrosshair(float &r, float &g, float &b)
    {
        fpsent *d = hudplayer();
        if(d->state==CS_SPECTATOR || d->state==CS_DEAD) return -1;

        if(d->state!=CS_ALIVE) return 0;

        int crosshair = 0;
        if(lasthit && lastmillis - lasthit < hitcrosshair) crosshair = 2;
        else if(teamcrosshair)
        {
            dynent *o = intersectclosest(d->o, worldpos, d);
            if(o && o->type==ENT_PLAYER && isteam(((fpsent *)o)->team, d->team))
            {
                crosshair = 1;
                r = g = 0;
            }
        }

        if(crosshair!=1 && !editmode && !m_insta)
        {
            if(d->health<=25) { r = 1.0f; g = b = 0; }
            else if(d->health<=50) { r = 1.0f; g = 0.5f; b = 0; }
        }
        if(d->gunwait) { r *= 0.5f; g *= 0.5f; b *= 0.5f; }
        return crosshair;
    }

    void lighteffects(dynent *e, vec &color, vec &dir)
    {
#if 0
        fpsent *d = (fpsent *)e;
        if(d->state!=CS_DEAD && d->quadmillis)
        {
            float t = 0.5f + 0.5f*sinf(2*M_PI*lastmillis/1000.0f);
            color.y = color.y*(1-t) + t;
        }
#endif
    }

    bool serverinfostartcolumn(g3d_gui *g, int i)
    {
        static const char *names[] = { "ping ", "players ", "map ", "mode ", "master ", "host ", "port ", "description " };
        static const int struts[] =  { 0,       0,          12,     12,      8,         13,      6,       24 };
        if(size_t(i) >= sizeof(names)/sizeof(names[0])) return false;
        g->pushlist();
        g->text(names[i], 0xFFFF80, !i ? " " : NULL);
        if(struts[i]) g->strut(struts[i]);
        g->mergehits(true);
        return true;
    }

    void serverinfoendcolumn(g3d_gui *g, int i)
    {
        g->mergehits(false);
        g->poplist();
    }

    const char *mastermodecolor(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodecolors)/sizeof(mastermodecolors[0])) ? mastermodecolors[n-MM_START] : unknown;
    }

    const char *mastermodeicon(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodeicons)/sizeof(mastermodeicons[0])) ? mastermodeicons[n-MM_START] : unknown;
    }

    bool serverinfoentry(g3d_gui *g, int i, const char *name, int port, const char *sdesc, const char *map, int ping, const vector<int> &attr, int np)
    {
        if(ping < 0 || attr.empty() || attr[0]!=PROTOCOL_VERSION)
        {
            switch(i)
            {
                case 0:
                    if(g->button(" ", 0xFFFFDD, "serverunk")&G3D_UP) return true;
                    break;

                case 1:
                case 2:
                case 3:
                case 4:
                    if(g->button(" ", 0xFFFFDD)&G3D_UP) return true;
                    break;

                case 5:
                    if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                    break;

                case 6:
                    if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                    break;

                case 7:
                    if(ping < 0)
                    {
                        if(g->button(sdesc, 0xFFFFDD)&G3D_UP) return true;
                    }
                    else if(g->buttonf("[%s protocol] ", 0xFFFFDD, NULL, attr.empty() ? "unknown" : (attr[0] < PROTOCOL_VERSION ? "older" : "newer"))&G3D_UP) return true;
                    break;
            }
            return false;
        }

        switch(i)
        {
            case 0:
            {
                const char *icon = attr.inrange(3) && np >= attr[3] ? "serverfull" : (attr.inrange(4) ? mastermodeicon(attr[4], "serverunk") : "serverunk");
                if(g->buttonf("%d ", 0xFFFFDD, icon, ping)&G3D_UP) return true;
                break;
            }

            case 1:
                if(attr.length()>=4)
                {
                    if(g->buttonf(np >= attr[3] ? "\f3%d/%d " : "%d/%d ", 0xFFFFDD, NULL, np, attr[3])&G3D_UP) return true;
                }
                else if(g->buttonf("%d ", 0xFFFFDD, NULL, np)&G3D_UP) return true;
                break;

            case 2:
                if(g->buttonf("%.25s ", 0xFFFFDD, NULL, map)&G3D_UP) return true;
                break;

            case 3:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, attr.length()>=2 ? server::modename(attr[1], "") : "")&G3D_UP) return true;
                break;

            case 4:
                if(g->buttonf("%s%s ", 0xFFFFDD, NULL, attr.length()>=5 ? mastermodecolor(attr[4], "") : "", attr.length()>=5 ? server::mastermodename(attr[4], "") : "")&G3D_UP) return true;
                break;

            case 5:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                break;

            case 6:
                if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                break;

            case 7:
                if(g->buttonf("%.25s", 0xFFFFDD, NULL, sdesc)&G3D_UP) return true;
                break;
        }
        return false;
    }

    struct playerinfo {
        string name, team;
        int cn,ping,frags,flags,deaths,teamkills,accuracy,privilege,state;
        uint ip;
        playerinfo() : cn(-1),ping(0),frags(0),flags(0),deaths(0),teamkills(0),accuracy(0),privilege(0),state(0),ip(0) {}
    };
    struct teaminfo {
        string name;
        int score;
        int numbases;
        vector<int> bases; //deemed useless information in relation to a server browser, however it shall be stored for future use TODO: is this really needed?
        vector<playerinfo *> players;
        teaminfo() : score(0), numbases(-1) {}
    };
    struct extinfo {
        int gametime;
        int teamping,playerping,playerids;
        bool teamresp,playerresp;
        int gamemode;
        bool teammode, ignore;
        vector<teaminfo *> teams;
        vector<playerinfo *> players,spectators;
        extinfo() : gametime(0),teamping(0),playerping(0),playerids(0),teamresp(true),playerresp(true),gamemode(-3),teammode(false), ignore(false) {}
    };

    static int sortplayers(playerinfo ** a, playerinfo ** b) {
        if((*a)->flags > (*b)->flags) return -1;
        if((*a)->flags < (*b)->flags) return 1;
        if((*a)->frags > (*b)->frags) return -1;
        if((*a)->frags < (*b)->frags) return 1;
        return strcmp((*a)->name, (*b)->name);
        
    }
    static int sortteams(teaminfo ** a, teaminfo ** b)
    {
        if(!(*a)->name[0])
        {
            if((*b)->name[0]) return 1;
        }
        else if(!(*b)->name[0]) return -1;
        if((*a)->score > (*b)->score) return -1;
        if((*a)->score < (*b)->score) return 1;
        if((*a)->players.length() > (*b)->players.length()) return -1;
        if((*a)->players.length() < (*b)->players.length()) return 1;
        return (*a)->name[0] && (*b)->name[0] ? strcmp((*a)->name, (*b)->name) : 0;
    }
    void assignplayers(serverinfo * si)
    {
        extinfo * x = (extinfo *)si->extinfo;
        if(x->teammode) {
            loopv(x->teams) x->teams[i]->players.shrink(0);
            loopv(x->players) {
                playerinfo * pl = x->players[i];
                loopv(x->teams) if(!strncmp(pl->team,x->teams[i]->name,260) && pl->state != CS_SPECTATOR) x->teams[i]->players.add(pl);
            }
        }
        else
        {
            teaminfo * tm = NULL;
            if(x->teams.length() == 0) {
                tm= new teaminfo;
                x->teams.add(tm);
                strcpy(tm->name, "");
                tm->score = 0;
            }
            else tm = x->teams[0];
            tm->players = x->players;
            loopv(tm->players) if(tm->players[i]->state == CS_SPECTATOR) tm->players.remove(i);
        }
        x->spectators.shrink(0);
        loopv(x->players) {
            if(x->players[i]->state == CS_SPECTATOR) x->spectators.add(x->players[i]);
        }
        loopv(x->teams) x->teams[i]->players.sort(sortplayers);
        x->teams.sort(sortteams);
    }

    bool extinfoparse(ucharbuf & p, serverinfo * si) {
            if(getint(p) != 0) return false;
            playerinfo * pl = NULL;
            teaminfo * tm = NULL;
            extinfo * x = NULL;
            if(si->extinfo == NULL) {
                x = new extinfo;
                si->extinfo = x;
            } else x = (extinfo *)si->extinfo;

            int op = getint(p);
            if(op == 1) { //playerinfo
                //Failsafe sanity check to prevent overflows
                if(x->ignore) return true;
                if(x->players.length() > MAXCLIENTS || x->teams.length() > MAXCLIENTS) {
                    x->ignore = true;
                    loopv(x->players) {
                        pl = x->players[i];
                        x->players.remove(i);
                        delete pl;
                    }
                    loopv(x->teams) {
                        tm = x->teams[i];
                        x->teams.remove(i);
                        delete tm;
                    }
                }



                char text[MAXTRANS];
                uint ip = 0;
                p.len = 6;
                op = getint(p);
                if(op == -10) {
                    //check id list agains currenyly know list of players and remove missing
                    x->playerping = lastmillis;
                    vector <int> cnlist;
                    loopi(si->numplayers) {
                        int cn = getint(p);
                        cnlist.add(cn);
                    }
                    loopv(x->players) if(cnlist.find(x->players[i]->cn) == -1) {
                        delete x->players[i];
                        x->players.remove(i);
                    }
                    x->playerids = cnlist.length();
                }
                else if(op == -11) {
                    x->playerping = lastmillis;
                    int cn = getint(p);
                    //get old, or create new player
                    loopv(x->players) if(x->players[i]->cn == cn) pl = x->players[i];
                    if(!pl) {
                        pl = new playerinfo;
                        pl->cn = cn;
                        x->players.add(pl);
                    }

                    pl->ping = getint(p);
                    getstring(text,p);//name
                    strncpy(pl->name,text,260);
                    getstring(text,p);//team
                    strncpy(pl->team,text,260);
                    pl->frags = getint(p);
                    pl->flags = getint(p);
                    pl->deaths = getint(p);
                    pl->teamkills = getint(p);
                    pl->accuracy = getint(p);
                    p.len +=3;
                    pl->privilege = getint(p);
                    pl->state = getint(p);
                    p.get((uchar*)&ip,3); //ip
                    pl->ip = ip;
                    if(x->playerids == x->players.length()) x->playerresp = true;
                }
            }
            else if(op == 2) { //teaminfo
                x->teamping = lastmillis;
                x->teamresp = true;
                char text[MAXTRANS];
                vector<teaminfo *> tmlist;
                vector<int> bases;
                p.len = 5;
                int teammode = getint(p);
                x->teammode = teammode != 0 ? false : true;
                x->gamemode = getint(p);
                x->gametime = getint(p);

                if(teammode != 0) {
                    return true; //no teams
                }
                while(p.remaining()) {
                    tm = NULL;
                    getstring(text,p);
                    int score = getint(p);
                    int numbases = getint(p);
                    if(numbases > -1) {
                        if(numbases > MAXCLIENTS) return true; //sanity check
                        loopi(numbases) bases.add(getint(p));
                    }
                    
                    loopv(x->teams) if(!strncmp(x->teams[i]->name,text,260)) {
                        tm = x->teams[i];
                    }
                    if(!tm) {
                        tm = new teaminfo();
                        strncpy(tm->name,text,260);
                        x->teams.add(tm);
                    }
                    tmlist.add(tm);
                    tm->score = score;
                    tm->numbases = numbases;
                    tm->bases = bases;
                }
                //delete missing teams
                loopv(x->teams) if(tmlist.find(x->teams[i])==-1) {
                    delete x->teams[i];
                    x->teams.remove(i);
                }
            }
            return true;
    }

    void extinforequest(ENetSocket &pingsock, ENetAddress & address,serverinfo * si) {
        extinfo * x = NULL;
        if(si != NULL)x = (extinfo *)si->extinfo;
        if(x != NULL && x->ignore) return;
        ENetBuffer playerbuf,teambuf;
        uchar player[MAXTRANS], team[MAXTRANS];
        ucharbuf pl(player, sizeof(player));
        ucharbuf tm(team, sizeof(team));
        putint(pl, 0);
        putint(pl, 1);
        putint(pl, -1);
        putint(tm, 0);
        putint(tm, 2);
        putint(tm, -1);
        if(x == NULL || (lastmillis - x->playerping > 1000 && x->playerresp) || lastmillis - x->playerping > 10000) {//only needed every second and wait for response before requesting again in case of errors, re-request every 10 seconds.
            if(x != NULL) {
                x->playerping = 0;
                x->playerresp = false;
            }
            playerbuf.data = player;
            playerbuf.dataLength = pl.length();
            enet_socket_send(pingsock, &address, &playerbuf, 1);
        }

        if(x == NULL || (lastmillis - x->teamping > 2000 && x->teamresp)) {//only needed every two second and wait for response before requesting again
            if(x != NULL) {
                x->teamping = 0;
                x->teamresp = false;
            }
            teambuf.data = team;
            teambuf.dataLength = tm.length();
            enet_socket_send(pingsock, &address, &teambuf, 1);
        }
    }

    VARP(extinfoshow,0,1,1);
    VARP(extinfoshowfrags,0,1,1);
    VARP(extinfoshowdeaths,0,1,1);
    VARP(extinfoshowteamkills,0,1,1);
    VARP(extinfoshowaccuracy,0,1,1);
    VARP(extinfoshowflags,0,1,1);
    VARP(extinfoshowping,0,0,1);
    VARP(extinfoshowclientnum,0,0,1);
    void serverextinfo(g3d_gui *cgui, serverinfo * si) {
        cgui->titlef("%.25s", 0xFFFF80, NULL, si->sdesc);
        cgui->titlef("%s:%d", 0xFFFF80, NULL, si->name, si->port);
        if(extinfoshow && si->extinfo != NULL) {
            extinfo * x = (extinfo *)si->extinfo;
            assignplayers(si);
            cgui->pushlist(0);
            cgui->text(server::modename(x->gamemode), 0xFFFF80);
            cgui->separator();
            const char *mname = si->map;
            cgui->text(mname[0] ? mname : "[new map]", 0xFFFF80);
            /*
            if(game::m_timed && mname[0] && (maplimit >= 0 || intermission))
            {
                g.separator();
                if(intermission) g.text("intermission", 0xFFFF80);
                else 
                {
                    int secs = max(maplimit-lastmillis, 0)/1000, mins = secs/60;
                    secs %= 60;
                    g.pushlist();
                    g.strut(mins >= 10 ? 4.5f : 3.5f);
                    g.textf("%d:%02d", 0xFFFF80, NULL, mins, secs);
                    g.poplist();
                }
            }*/
            cgui->separator();
            if(x->gametime == 0 || !x->players.length()) cgui->text("intermission", 0xFFFF80);
            else
            {
                int subsecs = (lastmillis-x->teamping)/1000, secs = x->gametime-subsecs, mins = secs/60;
                secs %= 60;
                cgui->pushlist();
                cgui->strut(mins >= 10 ? 4.5f : 3.5f);
                cgui->textf("%d:%02d", 0xFFFF80, NULL, mins, secs);
                cgui->poplist();
            }
            //if(paused || ispaused()) { g.separator(); g.text("paused", 0xFFFF80); } //no pause/unpasue sent in extinfo
            cgui->poplist();

            cgui->separator();
 
            //int numgroups = groupplayers();
            loopk(x->teammode ? x->teams.length() : 1)
            {
                if((k%2)==0) cgui->pushlist(); // horizontal
            
                //scoregroup &sg = *groups[k];
                teaminfo * tm = x->teams[k];
                int bgcolor = 0x3030C0, fgcolor = 0xFFFF80;

                cgui->pushlist(); // vertical
                cgui->pushlist(); // horizontal

                #define loopscoregroup(o, b) \
                    loopv(tm->players) \
                    { \
                        playerinfo * o = tm->players[i]; \
                        b; \
                    }    

                cgui->pushlist();
                if(tm->name[0] && x->teammode)
                {
                    cgui->pushlist();
                    cgui->background(bgcolor, x->teams.length()>1 ? 3 : 5);
                    cgui->strut(1);
                    cgui->poplist();
                }
                cgui->text("", 0, " ");
                loopscoregroup(o,
                {
                    if(o->flags > 0) { cgui->pushlist(); cgui->background(tm->name[0] && x->teammode ? 0x505050 : 0, x->teams.length()>1 ? 3 : 5); }
                    const char *icon = o->state == CS_ALIVE ? "radio_on" : "radio_off";
                    cgui->text("", 0, icon);
                    if(o->flags > 0) cgui->poplist();
                });
                cgui->poplist();

                if(tm->name[0] && x->teammode)
                {
                    cgui->pushlist(); // vertical

                    if(tm->score>=10000) cgui->textf("%s: WIN", fgcolor, NULL, tm->name);
                    else cgui->textf("%s: %d", fgcolor, NULL, tm->name, tm->score);

                    cgui->pushlist(); // horizontal
                }

                if(extinfoshowfrags)
                { 
                    cgui->pushlist();
                    cgui->strut(7);
                    cgui->text("frags", fgcolor);
                    loopscoregroup(o, cgui->textf("%d", 0xFFFFDD, NULL, o->frags));
                    cgui->poplist();
                }

                if(extinfoshowdeaths)
                { 
                    cgui->pushlist();
                    cgui->strut(7);
                    cgui->text("deaths", fgcolor);
                    loopscoregroup(o, cgui->textf("%d", 0xFFFFDD, NULL, o->deaths));
                    cgui->poplist();
                }

                if(extinfoshowteamkills)
                { 
                    cgui->pushlist();
                    cgui->strut(7);
                    cgui->text("teamkills", fgcolor);
                    loopscoregroup(o, cgui->textf("%d", o->teamkills >= 5 ? 0xFF0000 : 0xFFFFDD, NULL, o->teamkills));
                    cgui->poplist();
                }

                if(extinfoshowaccuracy)
                { 
                    cgui->pushlist();
                    cgui->strut(7);
                    cgui->text("%", fgcolor);
                    loopscoregroup(o, cgui->textf("%d%%", 0xFFFFDD, NULL, o->accuracy));
                    cgui->poplist();
                }

                if(extinfoshowflags && (m_check(x->gamemode,M_CTF) || m_check(x->gamemode,M_HOLD)))
                { 
                    cgui->pushlist();
                    cgui->strut(7);
                    cgui->text("flags", fgcolor);
                    loopscoregroup(o, cgui->textf("%d", 0xFFFFDD, NULL, o->flags));
                    cgui->poplist();
                }

                if(extinfoshowping)
                {
                    cgui->pushlist();
                    cgui->text("ping", fgcolor);
                    cgui->strut(6);
                    loopscoregroup(o, 
                    {
                        if(o->state==CS_LAGGED) cgui->text("LAG", 0xFFFFDD);
                        else cgui->textf("%d", 0xFFFFDD, NULL, o->ping);
                    });
                    cgui->poplist();
                }

                cgui->pushlist();
                cgui->text("name", fgcolor);
                cgui->strut(10);
                loopscoregroup(o, 
                {
                    int status = o->state!=CS_DEAD ? 0xFFFFDD : 0x606060;
                    if(o->privilege)
                    {
                        status = o->privilege>=2 ? 0xFF8000 : 0x40FF80;
                        if(o->state==CS_DEAD) status = (status>>1)&0x7F7F7F;
                    }
                    if(o->cn < MAXCLIENTS) cgui->text(o->name, status);
                    else cgui->textf("%s \f5[%i]", status, NULL,o->name,o->cn);
                });
                cgui->poplist();

                if(extinfoshowclientnum)
                {
                    cgui->space(1);
                    cgui->pushlist();
                    cgui->text("cn", fgcolor);
                    loopscoregroup(o, cgui->textf("%d", 0xFFFFDD, NULL, o->cn));
                    cgui->poplist();
                }
            
                if(tm->name[0] && x->teammode)
                {
                    cgui->poplist(); // horizontal
                    cgui->poplist(); // vertical
                }

                cgui->poplist(); // horizontal
                cgui->poplist(); // vertical

                if(k+1<x->teams.length() && (k+1)%2) cgui->space(2);
                else cgui->poplist(); // horizontal
            }
        
            if(x->spectators.length() > 0)
            {

                cgui->pushlist();

                cgui->pushlist();
                cgui->text("spectator", 0xFFFF80, " ");
                loopv(x->spectators) 
                {
                    playerinfo *pl = x->spectators[i];
                    int status = 0xFFFFDD;
                    if(pl->privilege) status = pl->privilege>=2 ? 0xFF8000 : 0x40FF80;
                    cgui->text(pl->name, status, "spectator");
                }
                cgui->poplist();

                if(extinfoshowclientnum) {
                    cgui->space(1);
                    cgui->pushlist();
                    cgui->text("cn", 0xFFFF80);
                    loopv(x->spectators) cgui->textf("%d", 0xFFFFDD, NULL, x->spectators[i]->cn);
                    cgui->poplist();
                }

                if(extinfoshowping) {
                    cgui->space(1);
                    cgui->pushlist();
                    cgui->text("ping", 0xFFFF80);
                    loopv(x->spectators) cgui->textf("%d", 0xFFFFDD, NULL, x->spectators[i]->ping);
                    cgui->poplist();
                }

                cgui->poplist();
            }


        } //end scoreboard
    }

    bool matchstr(char * haystack, const char * straw) {
        char * copy_haystack = new char[strlen(haystack)];
        char * copy_straw = new char[strlen(straw)];
        char * r = NULL;
        for(unsigned int it = 0; it < strlen(haystack); it++) {
            copy_haystack[it] = tolower(haystack[it]);
        }

        for(unsigned int it = 0; it < strlen(straw); it++) {
            copy_straw[it] = tolower(straw[it]);
        }
        r = strstr(copy_haystack,copy_straw);
        return r && r != NULL;

    }
    SVAR(filter,"");
    VAR(filterplayernames,0,1,1);
    VAR(filterservernames,0,1,1);
	VAR(filtercase,0,1,1);
    int serverfilter(serverinfo * si) {
        extinfo * x = (extinfo *)si->extinfo;
        if(!filter[0]) return true;
        if(x == NULL) return false;
        int is = 0;
        if(filterplayernames == 1) {
            loopv(x->players) {
                if(!filtercase) {
                    if(matchstr(x->players[i]->name,filter)) is++;
                }
                else {
                    if(strstr(x->players[i]->name,filter)) is++;
                }
            }
        }
        if(filterservernames == 1) {
            if(!filtercase) {
                if(matchstr(si->sdesc,filter)) is++;
            }
            else {
                if(strstr(si->sdesc,filter)) is++;
            }
        }
        return is;
    }


    // any data written into this vector will get saved with the map data. Must take care to do own versioning, and endianess if applicable. Will not get called when loading maps from other games, so provide defaults.
    void writegamedata(vector<char> &extras) {}
    void readgamedata(vector<char> &extras) {}

    const char *gameident() { return "fps"; }
    const char *savedconfig() { return "config.cfg"; }
    const char *restoreconfig() { return "restore.cfg"; }
    const char *defaultconfig() { return "data/defaults.cfg"; }
    const char *autoexec() { return "autoexec.cfg"; }
    const char *savedservers() { return "servers.cfg"; }

    void loadconfigs()
    {
        execfile("auth.cfg", false);
    }
}

