// snd_webaudio.c -- sound output in a browser page: none yet; the client plays
// silent (an AudioWorklet over the ring comes next)

#include "print.h"
#include "sound.h"

bool SNDDMA_Init (dma_t *dma)
{
	(void)dma;
	Con_Printf ("Sound: none in the browser yet\n");
	return false;
}

int SNDDMA_GetDMAPos (void)
{
	return 0;
}

void *SNDDMA_LockBuffer (void)
{
	return NULL;
}

void SNDDMA_UnlockBuffer (void *buffer)
{
	(void)buffer;
}

void SNDDMA_Submit (void)
{
}

void SNDDMA_Shutdown (void)
{
}

void SNDDMA_SetBlocked (bool blocked)
{
	(void)blocked;
}
