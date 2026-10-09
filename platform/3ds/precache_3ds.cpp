// Monster frame precache (Subsystems sheet: precache; Menu sheet: cl_3dsprecache).
// ZDoom precaches only the sprite each actor shows at level start, so deaths, gibs and attacks load their
// frames on first use (the in-game freezes on the 3DS). Mods are only known at runtime, so instead of a
// generated table this marks every state sprite of each monster class present (and its parent classes),
// and the engine's own precache loop loads them with the level.
#include "doomtype.h"
#include "actor.h"
#include "info.h"
#include "c_cvars.h"
#include "r_state.h"
#include "i_3ds.h"

CVAR (Bool, cl_3dsprecache, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void I3DS_PrecacheSprites(unsigned char *spritelist)
{
	if (!cl_3dsprecache)
		return;
	TArray<const PClass *> done;
	AActor *actor;
	TThinkerIterator<AActor> it;
	while ((actor = it.Next()) != NULL)
	{
		if (!(actor->flags3 & MF3_ISMONSTER) && !(actor->flags & MF_COUNTKILL))
			continue;
		for (const PClass *cls = actor->GetClass(); cls != NULL && cls->ActorInfo != NULL; cls = cls->ParentClass)
		{
			if (done.Find(cls) < done.Size())
				break;	// this class and its parents are already marked
			done.Push(cls);
			const FActorInfo *info = cls->ActorInfo;
			for (int i = 0; i < info->NumOwnedStates; ++i)
				if ((unsigned)info->OwnedStates[i].sprite < sprites.Size())
					spritelist[info->OwnedStates[i].sprite] = 1;
		}
	}
}

void I3DS_PrecacheSounds()
{
	// sounds named by monster properties are already precached by the engine
}
