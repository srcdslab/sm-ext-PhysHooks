/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod PhysHooks Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"
#include "CDetour/detours.h"
#include <sourcehook.h>
#include <sh_memory.h>
#include <server_class.h>
#include <ispatialpartition.h>

#define SetBit(A,I)		((A)[(I) >> 5] |= (1 << ((I) & 31)))
#define ClearBit(A,I)	((A)[(I) >> 5] &= ~(1 << ((I) & 31)))
#define CheckBit(A,I)	!!((A)[(I) >> 5] & (1 << ((I) & 31)))

class CTriggerMoved : public IPartitionEnumerator
{
public:
	virtual IterationRetval_t EnumElement( IHandleEntity *pHandleEntity ) = 0;
};

class CTouchLinks : public IPartitionEnumerator
{
public:
	virtual IterationRetval_t EnumElement( IHandleEntity *pHandleEntity ) = 0;
};

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

PhysHooks g_Interface;		/**< Global singleton for extension's main interface */

SMEXT_LINK(&g_Interface);

CGlobalVars *gpGlobals = NULL;
IGameConfig *g_pGameConf = NULL;

CDetour *g_pDetour_RunThinkFunctions = NULL;
CDetour *g_pDetour_SimThink_ListCopy = NULL;

IForward *g_pOnRunThinkFunctions = NULL;
IForward *g_pOnPrePlayerThinkFunctions = NULL;
IForward *g_pOnPostPlayerThinkFunctions = NULL;
IForward *g_pOnRunThinkFunctionsPost = NULL;

int g_SH_TriggerMoved = 0;
int g_SH_TouchLinks = 0;
int g_iMaxPlayers = 0;

CTriggerMoved *g_CTriggerMoved = NULL;
CTouchLinks *g_CTouchLinks = NULL;


DETOUR_DECL_STATIC1(DETOUR_RunThinkFunctions, void, bool, simulating)
{
	if(g_pOnRunThinkFunctions->GetFunctionCount())
	{
		g_pOnRunThinkFunctions->PushCell(simulating);
		g_pOnRunThinkFunctions->Execute();
	}

	if(g_pOnPrePlayerThinkFunctions->GetFunctionCount())
	{
		g_pOnPrePlayerThinkFunctions->Execute();
	}

	DETOUR_STATIC_CALL(DETOUR_RunThinkFunctions)(simulating);

	if(g_pOnRunThinkFunctionsPost->GetFunctionCount())
	{
		g_pOnRunThinkFunctionsPost->PushCell(simulating);
		g_pOnRunThinkFunctionsPost->Execute();
	}
}

void (*g_pPhysics_SimulateEntity)(CBaseEntity *pEntity) = NULL;

void Physics_SimulateEntity_CustomLoop(CBaseEntity **ppList, int Count, float Startime)
{
	CUtlVectorFixed<CBaseEntity*, 65> apPlayers;

	// Remove players from list and put into apPlayers
	for(int i = 0; i < Count; i++)
	{
		CBaseEntity *pEntity = ppList[i];
		if(!pEntity)
			continue;

		edict_t *pEdict = gamehelpers->EdictOfIndex(gamehelpers->EntityToBCompatRef(pEntity));
		if(!pEdict)
			continue;

		int Entity = gamehelpers->IndexOfEdict(pEdict);
		if(Entity >= 1 && Entity <= g_iMaxPlayers)
		{
			apPlayers.AddToTail(pEntity);
			ppList[i] = NULL;
		}
	}

	// Shuffle players array
	int n = apPlayers.Count();
	for(int i = n-1; i > 0; i--)
	{
		int j = rand() % (i + 1);
		CBaseEntity *pTmp = apPlayers[j];
		apPlayers[j] = apPlayers[i];
		apPlayers[i] = pTmp;
	}

	// Simulate players first
	FOR_EACH_VEC(apPlayers, i)
	{
		gpGlobals->curtime = Startime;
		g_pPhysics_SimulateEntity(apPlayers[i]);
	}

	// Post Player simulation done
	if(g_pOnPostPlayerThinkFunctions->GetFunctionCount())
	{
		gpGlobals->curtime = Startime;
		g_pOnPostPlayerThinkFunctions->Execute();
	}

	// Now simulate the rest
	for(int i = 0; i < Count; i++)
	{
		CBaseEntity *pEntity = ppList[i];
		if(!pEntity)
			continue;

		gpGlobals->curtime = Startime;
		g_pPhysics_SimulateEntity(pEntity);
	}
}

DETOUR_DECL_STATIC2(DETOUR_SimThink_ListCopy, int, CBaseEntity **, ppList, int, listMax)
{
	int count = DETOUR_STATIC_CALL(DETOUR_SimThink_ListCopy)(ppList, listMax);
	Physics_SimulateEntity_CustomLoop(ppList, count, gpGlobals->curtime);
	return 0;
}

int g_TriggerEntityMoved;
int *g_pBlockTriggerTouchPlayers = NULL;
int *g_pBlockTriggerMoved = NULL;
// void IVEngineServer::TriggerMoved( edict_t *pTriggerEnt, bool testSurroundingBoundsOnly ) = 0;
SH_DECL_HOOK2_void(IVEngineServer, TriggerMoved, SH_NOATTRIB, 0, edict_t *, bool);
void TriggerMoved(edict_t *pTriggerEnt, bool testSurroundingBoundsOnly)
{
	g_TriggerEntityMoved = gamehelpers->IndexOfEdict(pTriggerEnt);

	// Block if bit is set
	if(g_pBlockTriggerMoved && CheckBit(g_pBlockTriggerMoved, g_TriggerEntityMoved))
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	// Decide per entity in TriggerMoved_EnumElement
	RETURN_META(MRES_IGNORED);
}

// IterationRetval_t CTriggerMoved::EnumElement( IHandleEntity *pHandleEntity ) = 0;
SH_DECL_HOOK1(CTriggerMoved, EnumElement, SH_NOATTRIB, 0, IterationRetval_t, IHandleEntity *);
IterationRetval_t TriggerMoved_EnumElement(IHandleEntity *pHandleEntity)
{
	if(!g_pBlockTriggerTouchPlayers)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	IServerUnknown *pUnk = static_cast< IServerUnknown* >( pHandleEntity );
	CBaseHandle hndl = pUnk->GetRefEHandle();
	int index = hndl.GetEntryIndex();

	// We only care about players
	if(index > g_iMaxPlayers)
	{
		RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
	}

	// block touching any clients here if bit is set
	if(CheckBit(g_pBlockTriggerTouchPlayers, g_TriggerEntityMoved))
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// allow touch
	RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
}

cell_t BlockTriggerMoved(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockTriggerMoved);
	else
		g_pBlockTriggerMoved = NULL;

	return 0;
}

cell_t BlockTriggerTouchPlayers(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockTriggerTouchPlayers);
	else
		g_pBlockTriggerTouchPlayers = NULL;

	return 0;
}

int g_SolidEntityMoved;
int *g_pBlockSolidMoved = NULL;
int *g_pBlockSolidTouchPlayers = NULL;
int *g_pFilterClientSolidTouch = NULL;
// void IVEngineServer::SolidMoved( edict_t *pSolidEnt, ICollideable *pSolidCollide, const Vector* pPrevAbsOrigin, bool testSurroundingBoundsOnly ) = 0;
SH_DECL_HOOK4_void(IVEngineServer, SolidMoved, SH_NOATTRIB, 0, edict_t *, ICollideable *, const Vector *, bool);
void SolidMoved(edict_t *pSolidEnt, ICollideable *pSolidCollide, const Vector *pPrevAbsOrigin, bool testSurroundingBoundsOnly)
{
	g_SolidEntityMoved = gamehelpers->IndexOfEdict(pSolidEnt);

	// Block if bit is set
	if(g_pBlockSolidMoved && CheckBit(g_pBlockSolidMoved, g_SolidEntityMoved))
	{
		RETURN_META(MRES_SUPERCEDE);
	}

	// Decide per entity in TouchLinks_EnumElement
	RETURN_META(MRES_IGNORED);
}

inline bool IsStaticProp_InLine( IHandleEntity *pHandleEntity ) const
{
	return (!pHandleEntity) || ( (pHandleEntity->GetRefEHandle().GetSerialNumber() == (0x80000000 >> NUM_ENT_ENTRY_BITS) ) != 0 );
}

// IterationRetval_t CTouchLinks::EnumElement( IHandleEntity *pHandleEntity ) = 0;
SH_DECL_HOOK1(CTouchLinks, EnumElement, SH_NOATTRIB, 0, IterationRetval_t, IHandleEntity *);
IterationRetval_t TouchLinks_EnumElement(IHandleEntity *pHandleEntity)
{
	// skip null handle entity or static props.
	if (!pHandleEntity || IsStaticProp_InLine(pHandleEntity))
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	IServerUnknown *pUnk = static_cast< IServerUnknown* >( pHandleEntity );
	CBaseHandle hndl = pUnk->GetRefEHandle();
	int index = hndl.GetEntryIndex();

	// Optimization: Players shouldn't touch other players
	if(g_SolidEntityMoved <= g_iMaxPlayers && index <= g_iMaxPlayers)
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// block solid from touching any clients here if bit is set
	if(g_pBlockSolidTouchPlayers && index <= g_iMaxPlayers && CheckBit(g_pBlockSolidTouchPlayers, g_SolidEntityMoved))
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// Block player from touching any filtered entity here if bit is set
	if(g_pFilterClientSolidTouch && index < 2048 && CheckBit(g_pFilterClientSolidTouch, g_SolidEntityMoved * 2048 + index))
	{
		RETURN_META_VALUE(MRES_SUPERCEDE, ITERATION_CONTINUE);
	}

	// Allow otherwise
	RETURN_META_VALUE(MRES_IGNORED, ITERATION_CONTINUE);
}

cell_t BlockSolidMoved(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockSolidMoved);
	else
		g_pBlockSolidMoved = NULL;

	return 0;
}

cell_t BlockSolidTouchPlayers(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pBlockSolidTouchPlayers);
	else
		g_pBlockSolidTouchPlayers = NULL;

	return 0;
}

cell_t FilterClientSolidTouch(IPluginContext *pContext, const cell_t *params)
{
	if(params[2])
		pContext->LocalToPhysAddr(params[1], &g_pFilterClientSolidTouch);
	else
		g_pFilterClientSolidTouch = NULL;

	return 0;
}

bool PhysHooks::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	srand((unsigned int)time(NULL));

	g_iMaxPlayers = playerhelpers->GetMaxClients();

	char conf_error[255] = "";
	if(!gameconfs->LoadGameConfigFile("PhysHooks.games", &g_pGameConf, conf_error, sizeof(conf_error)))
	{
		if(conf_error[0])
			snprintf(error, maxlength, "Could not read PhysHooks.games.txt: %s", conf_error);

		return false;
	}

	CDetourManager::Init(g_pSM->GetScriptingEngine(), g_pGameConf);

	g_pDetour_RunThinkFunctions = DETOUR_CREATE_STATIC(DETOUR_RunThinkFunctions, "Physics_RunThinkFunctions");
	if(g_pDetour_RunThinkFunctions == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for Physics_RunThinkFunctions");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_RunThinkFunctions->EnableDetour();

	g_pDetour_SimThink_ListCopy = DETOUR_CREATE_STATIC(DETOUR_SimThink_ListCopy, "SimThink_ListCopy");
	if (g_pDetour_SimThink_ListCopy == NULL)
	{
		snprintf(error, maxlength, "Could not create detour for SimThink_ListCopy");
		SDK_OnUnload();
		return false;
	}

	g_pDetour_SimThink_ListCopy->EnableDetour();

	// Find VTable for CTriggerMoved
	uintptr_t pCTriggerMoved;
	if(!g_pGameConf->GetMemSig("CTriggerMoved", (void **)(&pCTriggerMoved)) || !pCTriggerMoved)
	{
		snprintf(error, maxlength, "Failed to find CTriggerMoved.\n");
		SDK_OnUnload();
		return false;
	}

	// Find VTable for CTouchLinks
	uintptr_t pCTouchLinks;
	if(!g_pGameConf->GetMemSig("CTouchLinks", (void **)(&pCTouchLinks)) || !pCTouchLinks)
	{
		snprintf(error, maxlength, "Failed to find CTouchLinks.\n");
		SDK_OnUnload();
		return false;
	}

	// First function in VTable
	g_CTriggerMoved = (CTriggerMoved *)(pCTriggerMoved + 8);
	g_CTouchLinks = (CTouchLinks *)(pCTouchLinks + 8);

	g_SH_TriggerMoved = SH_ADD_DVPHOOK(CTriggerMoved, EnumElement, g_CTriggerMoved, SH_STATIC(TriggerMoved_EnumElement), false);
	g_SH_TouchLinks = SH_ADD_DVPHOOK(CTouchLinks, EnumElement, g_CTouchLinks, SH_STATIC(TouchLinks_EnumElement), false);

	SH_ADD_HOOK(IVEngineServer, TriggerMoved, engine, SH_STATIC(TriggerMoved), false);
	SH_ADD_HOOK(IVEngineServer, SolidMoved, engine, SH_STATIC(SolidMoved), false);

	if(!g_pGameConf->GetMemSig("Physics_SimulateEntity", (void **)(&g_pPhysics_SimulateEntity)) || !g_pPhysics_SimulateEntity)
	{
		snprintf(error, maxlength, "Failed to find Physics_SimulateEntity.\n");
		SDK_OnUnload();
		return false;
	}

	g_pOnRunThinkFunctions = forwards->CreateForward("OnRunThinkFunctions", ET_Ignore, 1, NULL, Param_Cell);
	g_pOnPrePlayerThinkFunctions = forwards->CreateForward("OnPrePlayerThinkFunctions", ET_Ignore, 0, NULL);
	g_pOnPostPlayerThinkFunctions = forwards->CreateForward("OnPostPlayerThinkFunctions", ET_Ignore, 0, NULL);
	g_pOnRunThinkFunctionsPost = forwards->CreateForward("OnRunThinkFunctionsPost", ET_Ignore, 1, NULL, Param_Cell);

	return true;
}

const sp_nativeinfo_t MyNatives[] =
{
	{ "BlockTriggerMoved", BlockTriggerMoved },
	{ "BlockTriggerTouchPlayers", BlockTriggerTouchPlayers },
	{ "BlockSolidMoved", BlockSolidMoved },
	{ "BlockSolidTouchPlayers", BlockSolidTouchPlayers },
	{ "FilterClientSolidTouch", FilterClientSolidTouch },
	{ NULL, NULL }
};

void PhysHooks::SDK_OnAllLoaded()
{
	sharesys->AddNatives(myself, MyNatives);
	sharesys->RegisterLibrary(myself, "PhysHooks");
}
void PhysHooks::SDK_OnUnload()
{
	if(g_pDetour_RunThinkFunctions != NULL)
	{
		g_pDetour_RunThinkFunctions->Destroy();
		g_pDetour_RunThinkFunctions = NULL;
	}

	if (g_pDetour_SimThink_ListCopy != NULL)
	{
		g_pDetour_SimThink_ListCopy->Destroy();
		g_pDetour_SimThink_ListCopy = NULL;
	}

	if(g_pOnRunThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnRunThinkFunctions);
		g_pOnRunThinkFunctions = NULL;
	}

	if(g_pOnRunThinkFunctionsPost != NULL)
	{
		forwards->ReleaseForward(g_pOnRunThinkFunctionsPost);
		g_pOnRunThinkFunctionsPost = NULL;
	}

	if(g_pOnPrePlayerThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnPrePlayerThinkFunctions);
		g_pOnPrePlayerThinkFunctions = NULL;
	}

	if(g_pOnPostPlayerThinkFunctions != NULL)
	{
		forwards->ReleaseForward(g_pOnPostPlayerThinkFunctions);
		g_pOnPostPlayerThinkFunctions = NULL;
	}

	if(g_SH_TriggerMoved)
		SH_REMOVE_HOOK_ID(g_SH_TriggerMoved);

	if(g_SH_TouchLinks)
		SH_REMOVE_HOOK_ID(g_SH_TouchLinks);

	SH_REMOVE_HOOK(IVEngineServer, TriggerMoved, engine, SH_STATIC(TriggerMoved), false);
	SH_REMOVE_HOOK(IVEngineServer, SolidMoved, engine, SH_STATIC(SolidMoved), false);

	gameconfs->CloseGameConfigFile(g_pGameConf);
}

bool PhysHooks::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	gpGlobals = ismm->GetCGlobals();
	return true;
}
