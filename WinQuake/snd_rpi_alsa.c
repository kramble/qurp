/*
Copyright (C) 1996-1997 Id Software, Inc.

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
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <stdio.h>
#include <alsa/asoundlib.h>
#include "quakedef.h"

int snd_inited;

static snd_pcm_t *alsa_handle;
static int alsa_bufsize;
static char *alsa_buffer;
static snd_pcm_uframes_t alsa_frames, alsa_remaining_frames;

static unsigned int ALSA_bufpos;	// Used in fill_audio
static unsigned int ALSA_buflen;

static void ALSA_fill_audio(void *udata, char *stream, int len)
{
	// Just repurpose SDL_callback_fill_audio for now (overkill)

	// shm-> buffer is filled via snd_dma.c S_Update() calls S_Update_()
	// calls snd_mix.c S_PaintChannels() then S_TransferPaintBuffer() then
	// S_TransferStereo16() then Snd_WriteLinearBlastStereo16() 

	if (!shm)	// This would be bad
	{
		// Con_Printf("\nAudio callback shm is NULL !!\n"); // DEBUG
		return;	
	}

#if 0
	// slow version
	unsigned int i;
	for (i=0; i<len; i++)
	{
		stream[i] = shm->buffer[ALSA_bufpos++];
		if (ALSA_bufpos >= ALSA_buflen)
			ALSA_bufpos = 0;
	}
#else
	// better version uses memcpy and updates ALSA_bufpos once only
	if (ALSA_bufpos + len > ALSA_buflen)
	{
		// This normally does not happen if shm->buffer is a
		// multiple of the SDL audio buffer, but we should cope anyway
		// Con_Printf("\nAudio callback buffer mis-aligned!!\n"); // DEBUG
		unsigned int start = ALSA_bufpos;
		unsigned int count = ALSA_buflen - ALSA_bufpos;
		memcpy(stream, shm->buffer+ALSA_bufpos, count);
		memcpy(stream + count, shm->buffer, len - count);
		ALSA_bufpos = len - count;
	}
	else
	{
		memcpy(stream, shm->buffer+ALSA_bufpos, len);
		if (ALSA_bufpos + len == ALSA_buflen)
			ALSA_bufpos = 0;
		else
			ALSA_bufpos += len;
	}
#endif
}

qboolean SNDDMA_Init(void)
{

	int rc;
	snd_pcm_hw_params_t *params;
	unsigned int rate;
	int dir;		// direction

	snd_inited = 0;

	Con_Printf("\nSNDDMA_Init Entered\n");

	// BUG FIXME This needs to be here else we get seg fault on init failure
	// ... bug is in snd.dma.c S_Init() which prints shm->speed regardless
	shm = &sn;		// See snd_dma.c, sn is global volatile dma_t
				// defined in sound.h

	rc = snd_pcm_open(&alsa_handle, "default",
                    SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (rc < 0)
	{
		perror("snd_pcm_open");
		Con_Printf("snd_pcm_open failed\n");
		return 0;
	}

	Con_Printf("\nSNDDMA_Init snd_pcm_open succeeded\n");

	snd_pcm_hw_params_alloca(&params);

	snd_pcm_hw_params_any(alsa_handle, params);

	snd_pcm_hw_params_set_access(alsa_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);

	snd_pcm_hw_params_set_format(alsa_handle, params, SND_PCM_FORMAT_S16_LE);

	snd_pcm_hw_params_set_channels(alsa_handle, params, 2);

	rate = 22050;
	snd_pcm_hw_params_set_rate_near(alsa_handle, params, &rate, &dir);

	alsa_frames = 1024;
	snd_pcm_hw_params_set_period_size_near(alsa_handle, params, &alsa_frames, &dir);

	alsa_remaining_frames = alsa_frames;

	printf("ALSA rate=%d, frames=%d\n", rate, (int)alsa_frames);

	// Reduce latency by setting internal buffer to a smaller value (the
	// default is a bit too long)
	snd_pcm_hw_params_set_buffer_size(alsa_handle, params, alsa_frames * 4);

	rc = snd_pcm_hw_params(alsa_handle, params);
	if (rc < 0)
	{
		perror("snd_pcm_hw_params");
		Con_Printf("snd_pcm_hw_params failed\n");
		return 0;
	}
	
	snd_pcm_hw_params_get_period_size(params, &alsa_frames, &dir);
	alsa_bufsize = alsa_frames * 2 * 2;	// 2 bytes per channel
	alsa_buffer = (char *) malloc (alsa_bufsize);

	shm->splitbuffer = 0;
	shm->samplebits = 16;
	shm->speed = 22050;
	shm->channels = 2;

	// Make the DMA buffer four times the size of the alsa buffer

	// samples is mono samples (see dma_t in sound.h), so times channels
	// shm->samples = 4 * wanted.samples * shm->channels;
	shm->samples = 4 * alsa_bufsize;
	ALSA_buflen = shm->samples * (shm->samplebits/8);
	shm->buffer = (unsigned char*) malloc(ALSA_buflen);

	shm->submission_chunk = 1;
	shm->samplepos = 0;

	snd_inited = 1;
	return 1;
}

int SNDDMA_GetDMAPos(void)
{
	int verbose = 0;	// For DEBUG

	int rc;
	int advance = 0;

	verbose && printf("SNDDMA_GetDMAPos\n");

	if (!snd_inited)
		return 0;

	// Attempt to write for every call from mixer (roughly every host frame)
	// FIXME would be better to check buffer status first, perhaps using
	// snd_pcm_avail(), or use a callback via snd_async_add_pcm_handler()
	// but this seems to work OK for now

	// NB NON-blocking write, so most calls return EAGAIN or short write

	if (alsa_remaining_frames <= 0 || alsa_remaining_frames > alsa_frames)
	{
		verbose && printf("ALSA FRAME ERROR remaining %d\n", alsa_remaining_frames);
		alsa_remaining_frames = alsa_frames;
	}

	// Adjust buffer for partial writes (4 bytes per frame)
	char *buf = alsa_buffer + 4 * (alsa_frames - alsa_remaining_frames);

	rc = snd_pcm_writei(alsa_handle, buf, alsa_remaining_frames);
	if (rc == -EAGAIN)
	{
		verbose && printf("ALSA not ready\n");
	}
	else if (rc == -EPIPE)
	{
		verbose && printf("ALSA underrun\n");
		snd_pcm_prepare(alsa_handle);
	}
	else if (rc < 0) // Other error
	{
		verbose && printf("ALSA writei error: %s\n", snd_strerror(rc));
	}
	else if (rc != (int)alsa_remaining_frames)
	{
		verbose && printf("ALSA short write, wrote %d frames\n", rc);

		// Attempt to write the remainder of buffer the next time
		alsa_remaining_frames -= rc;

		if (alsa_remaining_frames <= 0)
		{
			verbose && printf("ALSA FRAME ERROR remaining %d\n", alsa_remaining_frames);
			alsa_remaining_frames = alsa_frames;
			// Advance the buffer so we don't get stuck
			advance = 1;
		}
	}
	else
	{
		// We wrote successfuly, so advance the DMA buffer
		verbose && printf("ALSA full write, wrote %d frames\n", rc);
		alsa_remaining_frames = alsa_frames;
		advance = 1;
	}

	if (advance)
	{
		alsa_remaining_frames = alsa_frames;

		// Repurpose SDL_callback_fill_audio for now (overkill)
		ALSA_fill_audio(NULL, alsa_buffer, alsa_bufsize);
	}

	// samplepos counts mono samples (see dma_t in sound.h)
	shm->samplepos = ALSA_bufpos / (shm->samplebits / 8);
	return shm->samplepos;

}

void SNDDMA_Shutdown(void)
{
	Con_Printf("\nSNDDMA_Shutdown entered\n");	// DEBUG
	if (snd_inited)
	{
		Con_Printf("Close Audio\n");
		snd_pcm_drain(alsa_handle);
		snd_pcm_close(alsa_handle);
		snd_inited = 0;
	}
	Con_Printf("SNDDMA_Shutdown complete\n");
}

/*
==============
SNDDMA_Submit

Send sound to device if buffer isn't really the dma buffer
===============
*/
void SNDDMA_Submit(void)
{
}

