/*
 * Background music handling for Quakespasm (reimplemented with FMOD)
 * Handles streaming music directly from the file system
 *
 * Copyright (C) 1999-2005 Id Software, Inc.
 * Copyright (C) 2010-2018 O.Sezer <sezero@users.sourceforge.net>
 * Copyright (C) 2021-2022 Nico de Poel <ndepoel@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "quakedef.h"
#include "snd_codec.h"
#include "bgmusic.h"

#if USE_FMOD
#include "fmod.h"
#include "fmod_errors.h"

#define MUSIC_DIRNAME	"music"

qboolean	bgmloop;
cvar_t		bgm_extmusic = {"bgm_extmusic", "1", CVAR_ARCHIVE};

static qboolean	no_extmusic = false;
static float	old_volume = -1.0f;

extern FMOD_SYSTEM *fmod_system;

static FMOD_CHANNELGROUP *bgm_channelGroup = NULL;
static FMOD_CHANNEL *bgm_channel = NULL;
static FMOD_SOUND *bgm_sound = NULL;

static const char *extensions[] =
{
	"wav", "flac", "ogg", "mp3"
};

static void BGM_Play_f (void)
{
	if (Cmd_Argc() == 2)
	{
		BGM_Play (Cmd_Argv(1));
	}
	else
	{
		Con_Printf ("music <musicfile>\n");
		return;
	}
}

static void BGM_Pause_f (void)
{
	BGM_Pause ();
}

static void BGM_Resume_f (void)
{
	BGM_Resume ();
}

static void BGM_Loop_f (void)
{
	if (Cmd_Argc() == 2)
	{
		if (q_strcasecmp(Cmd_Argv(1),  "0") == 0 ||
		    q_strcasecmp(Cmd_Argv(1),"off") == 0)
			bgmloop = false;
		else if (q_strcasecmp(Cmd_Argv(1), "1") == 0 ||
		         q_strcasecmp(Cmd_Argv(1),"on") == 0)
			bgmloop = true;
		else if (q_strcasecmp(Cmd_Argv(1),"toggle") == 0)
			bgmloop = !bgmloop;
	}

	if (bgmloop)
		Con_Printf("Music will be looped\n");
	else
		Con_Printf("Music will not be looped\n");
}

static void BGM_Stop_f (void)
{
	BGM_Stop();
}

qboolean BGM_Init (void)
{
	FMOD_RESULT result;

	Cvar_RegisterVariable(&bgm_extmusic);
	Cmd_AddCommand("music", BGM_Play_f);
	Cmd_AddCommand("music_pause", BGM_Pause_f);
	Cmd_AddCommand("music_resume", BGM_Resume_f);
	Cmd_AddCommand("music_loop", BGM_Loop_f);
	Cmd_AddCommand("music_stop", BGM_Stop_f);

	if (COM_CheckParm("-noextmusic") != 0)
		no_extmusic = true;

	bgmloop = true;

	if (!fmod_system)
	{
		Con_Printf("FMOD System not initialized! Cannot start FMOD music codec.\n");
		return false;
	}

	result = FMOD_System_CreateChannelGroup(fmod_system, "BGM", &bgm_channelGroup);
	if (result != FMOD_OK)
	{
		Con_Printf("Failed to create FMOD music channel group: %s\n", FMOD_ErrorString(result));
		return false;
	}

	return true;
}

void BGM_Shutdown (void)
{
	BGM_Stop();

	if (bgm_channelGroup)
	{
		FMOD_ChannelGroup_Release(bgm_channelGroup);
		bgm_channelGroup = NULL;
	}
}

static qboolean BGM_PlayStream(const char *filename)
{
	char netpath[MAX_OSPATH];
	FMOD_RESULT result;

	if (!fmod_system || !bgm_channelGroup)
	{
		Con_Printf("FMOD System not initialized, cannot play BGM\n");
		return false;
	}

	if (!COM_FullFilePath(filename, netpath, sizeof(netpath)))
	{
		Con_Printf("Could not open BGM file %s, file not found\n", filename);
		return false;
	}

	result = FMOD_System_CreateSound(fmod_system, netpath, FMOD_CREATESTREAM | FMOD_2D, NULL, &bgm_sound);
	if (result != FMOD_OK || !bgm_sound)
	{
		Con_Printf("Failed to create FMOD sound: %s\n", FMOD_ErrorString(result));
		BGM_Stop();
		return false;
	}

	Con_DPrintf("BGM_PlayStream: Successfully loaded %s\n", filename);

	result = FMOD_System_PlaySound(fmod_system, bgm_sound, bgm_channelGroup, false, &bgm_channel);
	if (result != FMOD_OK || !bgm_channel)
	{
		Con_Printf("Failed to play FMOD sound: %s\n", FMOD_ErrorString(result));
		BGM_Stop();
		return false;
	}

	if (bgmloop)
	{
		FMOD_Channel_SetMode(bgm_channel, FMOD_LOOP_NORMAL);
	}
	else
	{
		FMOD_Channel_SetMode(bgm_channel, FMOD_LOOP_OFF);
		FMOD_Channel_SetLoopCount(bgm_channel, 0);
	}

	return true;
}

static void BGM_Play_noext (const char *filename)
{
	char tmp[MAX_QPATH];

	for (int i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
	{
		q_snprintf(tmp, sizeof(tmp), "%s/%s.%s", MUSIC_DIRNAME, filename, extensions[i]);
		if (BGM_PlayStream(tmp))
			return;
	}

	Con_Printf("Couldn't handle music file %s\n", filename);
}

void BGM_Play (const char *filename)
{
	char tmp[MAX_QPATH];
	const char *ext;

	BGM_Stop();

	if (!filename || !*filename)
	{
		Con_DPrintf("null music file name\n");
		return;
	}

	ext = COM_FileGetExtension(filename);
	if (! *ext)	/* try all things */
	{
		BGM_Play_noext(filename);
		return;
	}

	q_snprintf(tmp, sizeof(tmp), "%s/%s", MUSIC_DIRNAME, filename);
	if (BGM_PlayStream(tmp))
		return;

	Con_Printf("Couldn't handle music file %s\n", filename);
}

void BGM_PlayCDtrack (byte track, qboolean looping)
{
/* instead of searching by the order of music_handlers, do so by
 * the order of searchpath priority: the file from the searchpath
 * with the highest path_id is most likely from our own gamedir
 * itself. This way, if a mod has track02 as a *.mp3 file, which
 * is below *.ogg in the music_handler order, the mp3 will still
 * have priority over track02.ogg from, say, id1.
 */
	char tmp[MAX_QPATH];
	const char *ext;
	unsigned int path_id, prev_id;

	BGM_Stop();
	if (CDAudio_Play(track, looping) == 0)
		return;			/* success */

	if (no_extmusic || !bgm_extmusic.value)
		return;

	prev_id = 0;
	ext  = NULL;
	
	for (int i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
	{
		q_snprintf(tmp, sizeof(tmp), "%s/track%02d.%s", MUSIC_DIRNAME, (int)track, extensions[i]);
		if (!COM_FileExists(tmp, &path_id))
			continue;

		if (path_id > prev_id)
		{
			prev_id = path_id;
			ext = extensions[i];
		}
	}

	if (ext == NULL)
	{
		Con_Printf("Couldn't find a cdrip for track %d\n", (int)track);
		return;
	}

	q_snprintf(tmp, sizeof(tmp), "%s/track%02d.%s", MUSIC_DIRNAME, (int)track, ext);
	if (!BGM_PlayStream(tmp))
		Con_Printf("Couldn't handle music file %s\n", tmp);
}

void BGM_Stop(void)
{
	if (bgm_channel)
	{
		FMOD_Channel_Stop(bgm_channel);
		bgm_channel = NULL;
	}

	if (bgm_sound)
	{
		FMOD_Sound_Release(bgm_sound);
		bgm_sound = NULL;
	}
}

void BGM_Pause (void)
{
	if (bgm_channel)
	{
		FMOD_Channel_SetPaused(bgm_channel, true);
	}
}

void BGM_Resume (void)
{
	if (bgm_channel)
	{
		FMOD_Channel_SetPaused(bgm_channel, false);
	}
}

void BGM_Update (void)
{
	if (old_volume != bgmvolume.value)
	{
		if (bgmvolume.value < 0)
			Cvar_SetQuick (&bgmvolume, "0");
		else if (bgmvolume.value > 1)
			Cvar_SetQuick (&bgmvolume, "1");

		old_volume = bgmvolume.value;
	}
	
	if (fmod_system && bgm_channelGroup)
	{
		FMOD_ChannelGroup_SetVolume(bgm_channelGroup, bgmvolume.value);
	}
}

#else

qboolean BGM_Init(void)
{
	Con_Printf("BGM disabled at compile time\n");
	return false;
}

void BGM_Shutdown(void)
{
}

void BGM_Play(const char *filename)
{
}

void BGM_Stop(void)
{
}

void BGM_Update(void)
{
}

void BGM_Pause(void)
{
}

void BGM_Resume(void)
{
}

void BGM_PlayCDtrack(byte track, qboolean looping)
{
	CDAudio_Play(track, looping);
}

#endif	// USE_FMOD
