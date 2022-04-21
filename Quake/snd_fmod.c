/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2010-2011 O. Sezer <sezero@users.sourceforge.net>
Copyright (C) 2021-2022 Nico de Poel <ndepoel@gmail.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"

#ifdef USE_FMOD
#include "fmod.h"
#include "fmod_errors.h"

extern qboolean sound_started;	// in snd_dma.c
extern sfx_t *known_sfx;
extern int num_sfx;

FMOD_SYSTEM *fmod_system = NULL;

static int fmod_samplerate;
static float old_volume = -1.0f;

static FMOD_CHANNELGROUP *sfx_channelGroup = NULL;

vec3_t listener_origin;

static const char *FMOD_SpeakerModeString(FMOD_SPEAKERMODE speakermode);
static float F_CALL SND_FMOD_Attenuation(FMOD_CHANNELCONTROL *channelControl, float distance);
static void SND_StartAmbientSounds();

// Copy and convert coordinate system
#define FMOD_VectorCopy(a, b)	{(b).x=(a)[0];(b).y=(a)[2];(b).z=(a)[1];}
#define	sound_nominal_clip_dist	1000.0

typedef struct soundslot_s
{
	qboolean zone;
	FMOD_CHANNEL *channel;
	float dist_mult;
} soundslot_t;

typedef struct entsounds_s
{
	soundslot_t slots[8];
} entsounds_t;

static entsounds_t entsounds[MAX_CHANNELS];
static FMOD_CHANNEL *ambients[NUM_AMBIENTS];

// Keep track of all the sounds started each frame
static sfx_t *sfxThisFrame[16];
static int numSfxThisFrame;

void S_Startup(void)
{
	FMOD_RESULT result;
	FMOD_SPEAKERMODE speakermode;
	unsigned int version;
	int driver, numchannels;
	char name[1024];

	result = FMOD_System_Create(&fmod_system, FMOD_VERSION);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to create FMOD System: %s\n", FMOD_ErrorString(result));
		return;
	}

	result = FMOD_System_GetVersion(fmod_system, &version);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to retrieve FMOD version: %s\n", FMOD_ErrorString(result));
		return;
	}

	if (version < FMOD_VERSION)
	{
		Con_Printf("Incorrect FMOD library version, expected: 0x%x, found: 0x%x", FMOD_VERSION, version);
		return;
	}

	result = FMOD_System_SetSoftwareChannels(fmod_system, MAX_DYNAMIC_CHANNELS);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to set number of FMOD software channels: %s\n", FMOD_ErrorString(result));
		return;
	}

	result = FMOD_System_Init(fmod_system, MAX_CHANNELS, FMOD_INIT_VOL0_BECOMES_VIRTUAL, NULL);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to initialize FMOD System: %s\n", FMOD_ErrorString(result));
		return;
	}

	result = FMOD_System_GetDriver(fmod_system, &driver);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to retrieve selected FMOD driver: %s\n", FMOD_ErrorString(result));
		return;
	}

	result = FMOD_System_GetDriverInfo(fmod_system, driver, name, sizeof(name), NULL, &fmod_samplerate, &speakermode, &numchannels);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to retrieve FMOD driver info: %s\n", FMOD_ErrorString(result));
		return;
	}

	Con_Printf("FMOD version %01x.%02x.%02x, driver '%s', %s speaker mode, %d Hz, %d channels\n",
		(version >> 16) & 0xff, (version >> 8) & 0xff, version & 0xff, name, FMOD_SpeakerModeString(speakermode), fmod_samplerate, numchannels);

	result = FMOD_System_CreateChannelGroup(fmod_system, "SFX", &sfx_channelGroup);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to create FMOD SFX channel group: %s\n", FMOD_ErrorString(result));
		return;
	}

	// Set up custom distance attenuation system
	FMOD_ChannelGroup_Set3DMinMaxDistance(sfx_channelGroup, 0.0f, sound_nominal_clip_dist);
	FMOD_System_Set3DRolloffCallback(fmod_system, &SND_FMOD_Attenuation);

	memset(entsounds, 0, sizeof(entsounds));
	memset(sfxThisFrame, 0, sizeof(sfxThisFrame));
	numSfxThisFrame = 0;

	sound_started = true;
}

void S_Shutdown(void)
{
	sfx_t *sfx;
	int i;

	Con_DPrintf("[FMOD] Shutdown\n");

	S_StopAllSounds(false);

	// Release all sounds that were loaded and attached to sfx_t's
	for (i = 0; i < num_sfx; i++)
	{
		sfx = &known_sfx[i];

		if (sfx->sound)
		{
			FMOD_Sound_Release(sfx->sound);
			sfx->sound = NULL;
		}
	}

	if (sfx_channelGroup)
	{
		FMOD_ChannelGroup_Release(sfx_channelGroup);
		sfx_channelGroup = NULL;
	}

	FMOD_System_Close(fmod_system);
	fmod_system = NULL;
}

static unsigned int SND_GetDelay(const sfx_t *sfx, float maxseconds)
{
	unsigned int delay;

	delay = maxseconds * 1000;
	if (delay > sfx->length)
		delay = sfx->length;
	if (delay > 0)
		delay = rand() % delay;

	return delay * fmod_samplerate / 1000;
}

/*
====================
SND_FMOD_Attenuation

Custom rolloff callback that mimics Quake's attenuation algorithm, 
whereby sounds can have varying degrees of distance rolloff.
====================
*/
static float F_CALL SND_FMOD_Attenuation(FMOD_CHANNELCONTROL *channelcontrol, float distance)
{
	FMOD_RESULT result;
	void *userdata;
	soundslot_t *soundslot;
	float scale;

	result = FMOD_Channel_GetUserData((FMOD_CHANNEL*)channelcontrol, &userdata);
	if (result != FMOD_OK || !userdata)
	{
		// Unknown type of channel or maybe it's a channel group, either way just return full volume in this case
		return 1.0f;
	}

	soundslot = (soundslot_t *)userdata;
	scale = 1.0f - (distance * soundslot->dist_mult);
	if (scale < 0.0f)
		scale = 0.0f;

	return scale;
}

/*
=================
SND_FMOD_Callback

Channel control callback that ensures a channel's associated userdata
is properly cleared when the channel stops playing.
=================
*/
static FMOD_RESULT F_CALL SND_FMOD_Callback(FMOD_CHANNELCONTROL *channelcontrol, FMOD_CHANNELCONTROL_TYPE controltype, FMOD_CHANNELCONTROL_CALLBACK_TYPE callbacktype, void *commanddata1, void *commanddata2)
{
	FMOD_RESULT result;
	void *userdata;
	soundslot_t *soundslot;

	// We're only interested in notifications for when a channel is done playing a sound
	if (controltype != FMOD_CHANNELCONTROL_CHANNEL || callbacktype != FMOD_CHANNELCONTROL_CALLBACK_END)
		return FMOD_OK;

	result = FMOD_Channel_GetUserData((FMOD_CHANNEL*)channelcontrol, &userdata);
	if (result != FMOD_OK || !userdata)
	{
		return FMOD_OK;
	}

	soundslot = (soundslot_t *)userdata;
	if (soundslot->zone)
	{
		Z_Free(soundslot);
	}

	return FMOD_OK;
}

static void SND_FMOD_SetChannelAttributes(FMOD_CHANNEL *channel, sfx_t *sfx, vec3_t origin, float vol)
{
	FMOD_VECTOR position;

	FMOD_VectorCopy(origin, position);
	FMOD_Channel_Set3DAttributes(channel, &position, NULL);
	FMOD_Channel_SetVolume(channel, vol);

	if (sfx->loopstart >= 0)
	{
		FMOD_Channel_SetMode(channel, FMOD_LOOP_NORMAL);
		FMOD_Channel_SetLoopPoints(channel, sfx->loopstart, FMOD_TIMEUNIT_MS, sfx->loopend, FMOD_TIMEUNIT_MS);
	}
	else
	{
		FMOD_Channel_SetMode(channel, FMOD_LOOP_OFF);
	}
}

static soundslot_t *SND_PickSoundSlot(int entnum, int entchannel)
{
	soundslot_t *slot;
	unsigned long long dspclock;

	if (entnum < 0 || entnum >= MAX_CHANNELS || entchannel == 0 || entchannel > 7)
	{
		// Just play on any free channel
		slot = (soundslot_t*)Z_Malloc(sizeof(soundslot_t));
		slot->zone = true;
		return slot;
	}

	// Local sound, use the first slot and override anything already playing
	if (entchannel < 0)
		entchannel = 0;

	slot = &entsounds[entnum].slots[entchannel];
	if (slot->channel)
	{
		// Stop any sound already playing on this slot
		FMOD_ChannelGroup_GetDSPClock(sfx_channelGroup, &dspclock, NULL);
		FMOD_Channel_SetFadePointRamp(slot->channel, dspclock + 64, 0.0f);
		FMOD_Channel_SetMode(slot->channel, FMOD_LOOP_OFF);
		slot->channel = NULL;
	}

	slot->zone = false;
	return slot;
}

/*
==============
Ambient sounds

These are sounds that are always present and always playing. They are unaffected by distance or orientation.
Instead they are modulated in volume based on whether the player is inside a world volume that has ambient sound levels set.
==============
*/
static FMOD_CHANNEL *SND_StartAmbientSound(const char *samplename)
{
	sfx_t *sfx;
	FMOD_CHANNEL *channel;
	FMOD_RESULT result;

	sfx = S_PrecacheSound(samplename);
	if (!sfx)
		return NULL;

	S_LoadSound(sfx);
	if (!sfx->sound)
		return NULL;

	result = FMOD_System_PlaySound(fmod_system, sfx->sound, sfx_channelGroup, 1, &channel);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to play ambient FMOD sound: %s\n", FMOD_ErrorString(result));
		return NULL;
	}

	SND_FMOD_SetChannelAttributes(channel, sfx, vec3_origin, 0.0f);
	FMOD_Channel_Set3DLevel(channel, 0.0f);
	FMOD_Channel_SetPaused(channel, 0);

	return channel;
}

static void SND_StartAmbientSounds()
{
	memset(ambients, 0, sizeof(ambients));
	ambients[AMBIENT_WATER] = SND_StartAmbientSound("ambience/water1.wav");
	ambients[AMBIENT_SKY] = SND_StartAmbientSound("ambience/wind2.wav");
}

static void S_UpdateAmbientSounds()
{
	FMOD_CHANNEL *channel;
	mleaf_t *leaf;
	float vol, channel_vol;

	if (cls.state == ca_connected && cl.worldmodel && cl.worldmodel->nodes)
		leaf = Mod_PointInLeaf(listener_origin, cl.worldmodel);
	else
		leaf = NULL;

	for (int i = 0; i < NUM_AMBIENTS; i++)
	{
		channel = ambients[i];
		if (!channel)
			continue;

		if (!leaf || !ambient_level.value)
		{
			FMOD_Channel_SetVolume(channel, 0.0f);
			continue;
		}

		vol = (ambient_level.value * (float)leaf->ambient_sound_level[i]) / 255;
		if (vol < 0.03f)
			vol = 0.0f;

		// don't adjust volume too fast
		FMOD_Channel_GetVolume(channel, &channel_vol);
		if (channel_vol < vol)
		{
			channel_vol += host_frametime * (ambient_fade.value / 255);
			if (channel_vol > vol)
				channel_vol = vol;
		}
		else if (channel_vol > vol)
		{
			channel_vol -= host_frametime * (ambient_fade.value / 255);
			if (channel_vol < vol)
				channel_vol = vol;
		}

		FMOD_Channel_SetVolume(channel, channel_vol);
	}
}

/*
=============
Entity sounds

These are your typical 3D sounds generated by entities in the world, including the player.
Each entity has a number of preset voice channels, each of which can only play one sound at a time.
=============
*/
void S_StartSound(int entnum, int entchannel, sfx_t *sfx, vec3_t origin, float fvol, float attenuation)	// Note: volume and attenuation are properly normalized here
{
	int i;
	FMOD_CHANNEL *channel;
	FMOD_RESULT result;
	soundslot_t *slot;
	unsigned long long dspclock;

	if (!fmod_system || !sfx)
		return;

	if (nosound.value)
		return;

	S_LoadSound(sfx);
	if (!sfx->sound)
		return;

	// Choose a slot to play the sound on, and stop any conflicting sound on the same entchannel
	// Do this before playing the new sound, so that any previous sound will be stopped in time
	slot = SND_PickSoundSlot(entnum, entchannel);

	result = FMOD_System_PlaySound(fmod_system, sfx->sound, sfx_channelGroup, 1, &channel);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to play FMOD sound: %s\n", FMOD_ErrorString(result));
		return;
	}

	SND_FMOD_SetChannelAttributes(channel, sfx, origin, fvol);

	// Set up callback data for rolloff and cleanup
	slot->channel = channel;
	slot->dist_mult = attenuation / sound_nominal_clip_dist;
	FMOD_Channel_SetUserData(channel, slot);
	FMOD_Channel_SetCallback(channel, &SND_FMOD_Callback);

	// Anything coming from the view entity will always be full volume, and entchannel -1 is used for local sounds (e.g. menu sounds)
	if (entchannel < 0 || entnum == cl.viewentity)
	{
		FMOD_Channel_Set3DLevel(channel, 0.0f);
		FMOD_Channel_SetPriority(channel, 64);	// Ensure local sounds always get priority over other entities
	}
	
	for (i = 0; i < numSfxThisFrame; i++)
	{
		// if an identical sound has also been started this frame, offset the pos
		// a bit to keep it from just making the first one louder
		if (sfxThisFrame[i] == sfx)
		{
			FMOD_ChannelGroup_GetDSPClock(sfx_channelGroup, &dspclock, NULL);
			FMOD_Channel_SetDelay(channel, dspclock + SND_GetDelay(sfx, 0.1f), 0, 0);
			break;
		}
	}

	if (numSfxThisFrame < 16)
		sfxThisFrame[numSfxThisFrame++] = sfx;

	FMOD_Channel_SetPaused(channel, 0);
}

/*
=============
Static sounds

These are sounds that have a fixed position in the world and loop continuously.
They typically start playing immediately on level load.
=============
*/
void S_StaticSound(sfx_t *sfx, vec3_t origin, float vol, float attenuation)	// Note: volume and attenuation are in 0-255 range here
{
	FMOD_CHANNEL *channel;
	FMOD_RESULT result;
	soundslot_t *slot;
	unsigned long long dspclock;

	if (!fmod_system || !sfx)
		return;

	S_LoadSound(sfx);
	if (!sfx->sound)
		return;

	if (sfx->loopstart < 0)
	{
		Con_Printf("Sound %s not looped\n", sfx->name);
		return;
	}

	result = FMOD_System_PlaySound(fmod_system, sfx->sound, sfx_channelGroup, 1, &channel);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to play static FMOD sound: %s\n", FMOD_ErrorString(result));
		return;
	}

	SND_FMOD_SetChannelAttributes(channel, sfx, origin, vol / 255);

	// Set up attenuation info for use by the rolloff callback
	slot = SND_PickSoundSlot(-1, -1);
	slot->channel = channel;
	slot->dist_mult = (attenuation / 64) / sound_nominal_clip_dist;
	FMOD_Channel_SetUserData(channel, slot);
	FMOD_Channel_SetCallback(channel, &SND_FMOD_Callback);

	// Add a random delay so that similar sounds don't phase together
	// Note: this isn't really authentic to the original Quake sound system, but it does improve the sense of directionality
	FMOD_ChannelGroup_GetDSPClock(sfx_channelGroup, &dspclock, NULL);
	FMOD_Channel_SetDelay(channel, dspclock + SND_GetDelay(sfx, 0.2f), 0, 0);

	FMOD_Channel_SetPaused(channel, 0);

	// Note: we can forget about the channel we just created, because all SFX channels will be stopped and released on level change through S_StopAllSounds
}

void S_StopSound(int entnum, int entchannel)
{
	soundslot_t *slot;
	unsigned long long dspclock;

	if (!fmod_system)
		return;

	if (entnum < 0 || entnum >= MAX_CHANNELS || entchannel < 0 || entchannel > 7)
		return;

	slot = &entsounds[entnum].slots[entchannel];
	if (slot->channel)
	{
		// Instead of immediately stopping the sound, we fade it down to 0 volume over several samples and let it play out
		// This prevents an annoying popping noise, which is especially noticeable on rapid-fire weapons
		FMOD_ChannelGroup_GetDSPClock(sfx_channelGroup, &dspclock, NULL);
		FMOD_Channel_SetFadePointRamp(slot->channel, dspclock + 64, 0.0f);
		FMOD_Channel_SetMode(slot->channel, FMOD_LOOP_OFF);
		slot->channel = NULL;
	}
}

void S_StopAllSounds(qboolean clear)
{
	if (!fmod_system)
		return;

	// Stopping all sounds also ensures that any associated zone memory is freed
	FMOD_ChannelGroup_Stop(sfx_channelGroup);

	if (clear)	// We're abusing the clear flag to also mean "keep ambients alive"
	{
		S_ClearBuffer();

		// Ambient sounds need to be restarted, as they are always "playing"
		SND_StartAmbientSounds();
	}
}

void S_ClearBuffer(void)
{
	// This is meant to prevent the same sound buffer playing over and over while the game is stalled
	// I don't think that's really an issue with FMOD
}

void S_Update(vec3_t origin, vec3_t forward, vec3_t right, vec3_t up)
{
	if (!fmod_system)
		return;

	if (old_volume != sfxvolume.value)
	{
		if (sfxvolume.value < 0)
			Cvar_SetQuick(&sfxvolume, "0");
		else if (sfxvolume.value > 1)
			Cvar_SetQuick(&sfxvolume, "1");

		old_volume = sfxvolume.value;
	}

	VectorCopy(origin, listener_origin);

	FMOD_VECTOR fmod_pos, fmod_forward, fmod_up;
	FMOD_VectorCopy(origin, fmod_pos);
	FMOD_VectorCopy(forward, fmod_forward);
	FMOD_VectorCopy(up, fmod_up);

	FMOD_System_Set3DListenerAttributes(fmod_system, 0, &fmod_pos, NULL, &fmod_forward, &fmod_up);

	FMOD_ChannelGroup_SetVolume(sfx_channelGroup, sfxvolume.value);

	S_UpdateAmbientSounds();

	FMOD_System_Update(fmod_system);

	// Reset sounds played for the next frame
	memset(sfxThisFrame, 0, sizeof(sfxThisFrame));
	numSfxThisFrame = 0;
}

void S_ExtraUpdate(void)
{
	if (!fmod_system)
		return;

	FMOD_System_Update(fmod_system);
}

static void S_SetMasterMute(FMOD_BOOL mute)
{
	FMOD_CHANNELGROUP *master;

	if (!fmod_system)
		return;

	FMOD_System_GetMasterChannelGroup(fmod_system, &master);
	FMOD_ChannelGroup_SetMute(master, mute);
}

void S_BlockSound(void)
{
	S_SetMasterMute(1);
}

void S_UnblockSound(void)
{
	S_SetMasterMute(0);
}

sfxcache_t *S_LoadSound(sfx_t *s)
{
	char namebuffer[256];
	byte *data;
	byte stackbuf[1 * 1024]; // avoid dirtying the cache heap
	wavinfo_t info;
	FMOD_CREATESOUNDEXINFO exinfo;
	FMOD_RESULT result;
#if _DEBUG
	FMOD_SOUND_TYPE type;
	FMOD_SOUND_FORMAT format;
	int channels, bits;
#endif

	if (!fmod_system)
		return NULL;

	// Check if it's already loaded
	if (s->sound)
		return NULL;

	q_strlcpy(namebuffer, "sound/", sizeof(namebuffer));
	q_strlcat(namebuffer, s->name, sizeof(namebuffer));

	data = COM_LoadStackFile(namebuffer, stackbuf, sizeof(stackbuf), NULL);
	if (!data)
	{
		Con_Printf("Couldn't load %s\n", namebuffer);
		return NULL;
	}

	info = GetWavinfo(s->name, data, com_filesize);
	if (!info.channels)
	{
		Con_Printf("Invalid WAV file: %s\n", namebuffer);
		return NULL;
	}

	memset(&exinfo, 0, sizeof(FMOD_CREATESOUNDEXINFO));
	exinfo.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
	exinfo.length = com_filesize;

	// This will copy the sound data into FMOD's internal buffers, so there's no need to keep it around in hunk memory
	result = FMOD_System_CreateSound(fmod_system, (const char*)data, FMOD_3D | FMOD_OPENMEMORY | FMOD_CREATESAMPLE, &exinfo, &s->sound);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to create FMOD sound: %s\n", FMOD_ErrorString(result));
		return NULL;
	}

	// Collect data required for looping and delay
	if (info.loopstart >= 0)
	{
		s->loopstart = info.loopstart * 1000 / info.rate;
		s->loopend = info.samples * 1000 / info.rate;
	}
	else
	{
		s->loopstart = s->loopend = -1;
	}
	FMOD_Sound_GetLength(s->sound, &s->length, FMOD_TIMEUNIT_MS);

#if _DEBUG
	FMOD_Sound_GetFormat(s->sound, &type, &format, &channels, &bits);
	Con_DPrintf("[FMOD] Loaded sound '%s': type %d, format %d, %d channel(s), %d bits, %d ms, %d samples, loopstart = %d\n", s->name, type, format, channels, bits, s->length, info.samples, s->loopstart);
#endif

	return NULL;	// Return value is unused; FMOD has its own internal cache, we never need to use Quake's sfxcache_t
}

void S_TouchSound(const char *sample)
{
	// Move the sound data up into the CPU cache
	// Not really necessary here
}

static const char *FMOD_SpeakerModeString(FMOD_SPEAKERMODE speakermode)
{
	switch (speakermode)
	{
	case FMOD_SPEAKERMODE_MONO:
		return "Mono";
	case FMOD_SPEAKERMODE_STEREO:
		return "Stereo";
	case FMOD_SPEAKERMODE_QUAD:
		return "4.0 Quad";
	case FMOD_SPEAKERMODE_SURROUND:
		return "5.0 Surround";
	case FMOD_SPEAKERMODE_5POINT1:
		return "5.1 Surround";
	case FMOD_SPEAKERMODE_7POINT1:
		return "7.1 Surround";
	case FMOD_SPEAKERMODE_7POINT1POINT4:
		return "7.1.4 Surround";
	default:
		return "Unknown";
	}
}

#endif	// USE_FMOD
