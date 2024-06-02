//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Interface layer for ipion IVP physics.
//
// $Workfile:     $
// $Date:         $
// $NoKeywords: $
//=============================================================================//


#include "cbase.h"
#include "coordsize.h"
#include "entitylist.h"
#include "vcollide_parse.h"
#include "soundenvelope.h"
#include "game.h"
#include "utlvector.h"
#include "init_factory.h"
#include "igamesystem.h"
#include "hierarchy.h"
#include "IEffects.h"
#include "engine/IEngineSound.h"
#include "world.h"
#include "decals.h"
#include "physics_fx.h"
#include "vphysics_sound.h"
#include "movevars_shared.h"
#include "physics_saverestore.h"
#include "solidsetdefaults.h"
#include "tier0/vprof.h"
#include "engine/IStaticPropMgr.h"
#include "physics_prop_ragdoll.h"
#include "vphysics/object_hash.h"
#include "vphysics/collision_set.h"
#include "vphysics/friction.h"
#include "fmtstr.h"
#include "physics_npc_solver.h"
// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar phys_speeds( "phys_speeds", "0" );
extern ConVar phys_rolling_drag;

// defined in phys_constraint
extern IPhysicsConstraintEvent *g_pConstraintEvents;


// local variables
static CEntityList *g_pShadowEntities = NULL;
static float g_PhysAverageSimTime;


// local routines
static IPhysicsObject *PhysCreateWorld( CBaseEntity *pWorld );
static void PhysFrame( float deltaTime );

void TimescaleChanged( ConVar *var, char const *pOldString )
{
	if ( physenv )
	{
		physenv->ResetSimulationClock();
	}
}

ConVar phys_timescale( "phys_timescale", "1", 0, "Scale time for physics", TimescaleChanged );

struct damageevent_t
{
	CBaseEntity		*pEntity;
	IPhysicsObject	*pInflictorPhysics;
	CTakeDamageInfo	info;
	bool			bRestoreVelocity;
};

struct inflictorstate_t
{
	Vector			savedVelocity;
	AngularImpulse	savedAngularVelocity;
	IPhysicsObject	*pInflictorPhysics;
	float			otherMassMax;
	short			nextIndex;
	short			restored;
};

struct impulseevent_t
{
	IPhysicsObject	*pObject;
	Vector vecCenterForce;
	Vector vecCenterTorque;
};

struct velocityevent_t
{
	IPhysicsObject	*pObject;
	Vector vecVelocity;
};

enum
{
	COLLSTATE_ENABLED = 0,
	COLLSTATE_TRYDISABLE = 1,
	COLLSTATE_TRYNPCSOLVER = 2,
	COLLSTATE_DISABLED = 3
};

struct penetrateevent_t
{
	EHANDLE			hEntity0;
	EHANDLE			hEntity1;
	float			startTime;
	float			timeStamp;
	int				collisionState;
};

class CCollisionEvent : public IPhysicsCollisionEvent, public IPhysicsCollisionSolver, public IPhysicsObjectEvent
{
public:
	CCollisionEvent();
	friction_t *FindFriction( CBaseEntity *pObject );
	void ShutdownFriction( friction_t &friction );
	void FrameUpdate();
	void LevelShutdown( void );

	// IPhysicsCollisionEvent
	void PreCollision( vcollisionevent_t *pEvent );
	void PostCollision( vcollisionevent_t *pEvent );
	void Friction( IPhysicsObject *pObject, float energy, int surfaceProps, int surfacePropsHit, IPhysicsCollisionData *pData );
	void StartTouch( IPhysicsObject *pObject1, IPhysicsObject *pObject2, IPhysicsCollisionData *pTouchData );
	void EndTouch( IPhysicsObject *pObject1, IPhysicsObject *pObject2, IPhysicsCollisionData *pTouchData );
	void FluidStartTouch( IPhysicsObject *pObject, IPhysicsFluidController *pFluid );
	void FluidEndTouch( IPhysicsObject *pObject, IPhysicsFluidController *pFluid );
	void PostSimulationFrame();
	void ObjectEnterTrigger( IPhysicsObject *pTrigger, IPhysicsObject *pObject );
	void ObjectLeaveTrigger( IPhysicsObject *pTrigger, IPhysicsObject *pObject );
	
	void GetTriggerEvent( triggerevent_t *pEvent, CBaseEntity *pTriggerEntity );
	void BufferTouchEvents( bool enable ) { m_bBufferTouchEvents = enable; }
	void AddDamageEvent( CBaseEntity *pEntity, const CTakeDamageInfo &info, IPhysicsObject *pInflictorPhysics, bool bRestoreVelocity, const Vector &savedVel, const AngularImpulse &savedAngVel );
	void AddImpulseEvent( IPhysicsObject *pPhysicsObject, const Vector &vecCenterForce, const AngularImpulse &vecCenterTorque );
	void AddSetVelocityEvent( IPhysicsObject *pPhysicsObject, const Vector &vecVelocity );

	// IPhysicsCollisionSolver
	int		ShouldCollide( IPhysicsObject *pObj0, IPhysicsObject *pObj1, void *pGameData0, void *pGameData1 );
	int		ShouldSolvePenetration( IPhysicsObject *pObj0, IPhysicsObject *pObj1, void *pGameData0, void *pGameData1, float dt );
	bool	ShouldFreezeObject( IPhysicsObject *pObject ) { return true; }
	int		AdditionalCollisionChecksThisTick( int currentChecksDone ) 
	{
		//CallbackContext check(this);
		if ( currentChecksDone < 1200 )
		{
			DevMsg(1,"VPhysics Collision detection getting expensive, check for too many convex pieces!\n");
			return 1200 - currentChecksDone;
		}
		DevMsg(1,"VPhysics exceeded collision check limit (%d)!!!\nInterpenetration may result!\n", currentChecksDone );
		return 0; 
	}

	// IPhysicsObjectEvent
	// these can be used to optimize out queries on sleeping objects
	// Called when an object is woken after sleeping
	virtual void ObjectWake( IPhysicsObject *pObject ) {}
	// called when an object goes to sleep (no longer simulating)
	virtual void ObjectSleep( IPhysicsObject *pObject ) {}


	// locals
	bool GetInflictorVelocity( IPhysicsObject *pInflictor, Vector &velocity, AngularImpulse &angVelocity );

	void GetListOfPenetratingEntities( CBaseEntity *pSearch, CUtlVector<CBaseEntity *> &list );
	bool IsInCallback() { return m_inCallback > 0 ? true : false; }

private:
	void UpdateFrictionSounds();
	void UpdateTouchEvents();
	void UpdateDamageEvents();
	void UpdateImpulseEvents();
	void UpdateSetVelocityEvents();
	void UpdatePenetrateEvents( void );
	void UpdateFluidEvents();
	void AddTouchEvent( CBaseEntity *pEntity0, CBaseEntity *pEntity1, int touchType, const Vector &point, const Vector &normal );
	penetrateevent_t &FindOrAddPenetrateEvent( CBaseEntity *pEntity0, CBaseEntity *pEntity1 );
	float DeltaTimeSinceLastFluid( CBaseEntity *pEntity );

	void RestoreDamageInflictorState( IPhysicsObject *pInflictor );
	void RestoreDamageInflictorState( int inflictorStateIndex, float velocityBlend );
	int AddDamageInflictor( IPhysicsObject *pInflictorPhysics, float otherMass, const Vector &savedVel, const AngularImpulse &savedAngVel, bool addList );
	int	FindDamageInflictor( IPhysicsObject *pInflictorPhysics );

	// make the call into the entity system
	void DispatchStartTouch( CBaseEntity *pEntity0, CBaseEntity *pEntity1, const Vector &point, const Vector &normal );
	void DispatchEndTouch( CBaseEntity *pEntity0, CBaseEntity *pEntity1 );
	
	class CallbackContext
	{
	public:
		CallbackContext(CCollisionEvent *pOuter)
		{
			m_pOuter = pOuter;
			m_pOuter->m_inCallback++;
		}
		~CallbackContext()
		{
			m_pOuter->m_inCallback--;
		}
	private:
		CCollisionEvent *m_pOuter;
	};
	friend class CallbackContext;
	
	friction_t					m_current[8];
	gamevcollisionevent_t		m_gameEvent;
	triggerevent_t				m_triggerEvent;
	CUtlVector<touchevent_t>	m_touchEvents;
	CUtlVector<damageevent_t>	m_damageEvents;
	CUtlVector<inflictorstate_t>	m_damageInflictors;
	CUtlVector<penetrateevent_t> m_penetrateEvents;
	CUtlVector<fluidevent_t>	m_fluidEvents;
	CUtlVector<impulseevent_t>	m_impulseEvents;
	CUtlVector<velocityevent_t>	m_setVelocityEvents;
	int							m_inCallback;
	bool						m_bBufferTouchEvents;
};

CCollisionEvent g_Collisions;

class CPhysicsHook : public CBaseGameSystem
{
public:
	virtual bool Init();
	virtual void LevelInitPreEntity();
	virtual void LevelInitPostEntity();
	virtual void LevelShutdownPreEntity();
	virtual void LevelShutdownPostEntity();
	virtual void FrameUpdatePostEntityThink();
	virtual void PreClientUpdate();

	bool ShouldSimulate()
	{
		return (physenv && !m_bPaused) ? true : false;
	}

	physicssound::soundlist_t m_impactSounds;

	bool m_bPaused;
	bool m_isFinalTick;
	CUtlVector<masscenteroverride_t>	m_massCenterOverrides;
};


CPhysicsHook	g_PhysicsHook;


//-----------------------------------------------------------------------------
// Singleton access
//-----------------------------------------------------------------------------
IGameSystem* PhysicsGameSystem()
{
	return &g_PhysicsHook;
}


//-----------------------------------------------------------------------------
// Purpose: The physics hook callback implementations
//-----------------------------------------------------------------------------
bool CPhysicsHook::Init( void )
{
	factorylist_t factories;
	
	// Get the list of interface factories to extract the physics DLL's factory
	FactoryList_Retrieve( factories );

	if ( !factories.physicsFactory )
		return false;

	if ((physics = (IPhysics *)factories.physicsFactory( VPHYSICS_INTERFACE_VERSION, NULL )) == NULL ||
		(physcollision = (IPhysicsCollision *)factories.physicsFactory( VPHYSICS_COLLISION_INTERFACE_VERSION, NULL )) == NULL ||
		(physprops = (IPhysicsSurfaceProps *)factories.physicsFactory( VPHYSICS_SURFACEPROPS_INTERFACE_VERSION, NULL )) == NULL
		)
		return false;

	PhysParseSurfaceData( physprops, filesystem );

	m_isFinalTick = true;
	return true;
}


// a little debug wrapper to help fix bugs when entity pointers get trashed
#if 0
struct physcheck_t
{
	IPhysicsObject *pPhys;
	char			string[512];
};

CUtlVector< physcheck_t > physCheck;

void PhysCheckAdd( IPhysicsObject *pPhys, const char *pString )
{
	physcheck_t tmp;
	tmp.pPhys = pPhys;
	Q_strncpy( tmp.string, pString ,sizeof(tmp.string));
	physCheck.AddToTail( tmp );
}

const char *PhysCheck( IPhysicsObject *pPhys )
{
	for ( int i = 0; i < physCheck.Size(); i++ )
	{
		if ( physCheck[i].pPhys == pPhys )
			return physCheck[i].string;
	}

	return "unknown";
}
#endif

//-----------------------------------------------------------------------------
// Purpose: Precaches a surfaceproperties string name if it's set.
// Input  : idx - 
// Output : static void
//-----------------------------------------------------------------------------
static void PrecachePhysicsSoundByStringIndex( int idx )
{
	// Only precache if a value was set in the script file...
	if ( idx != 0 )
	{
		CBaseEntity::PrecacheScriptSound( physprops->GetString( idx ) );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Iterates all surfacedata sounds and precaches them
// Output : static void
//-----------------------------------------------------------------------------
static void PrecachePhysicsSounds()
{
	// precache the surface prop sounds
	for ( int i = 0; i < physprops->SurfacePropCount(); i++ )
	{
		surfacedata_t *pprop = physprops->GetSurfaceData( i );
		Assert( pprop );
		
		PrecachePhysicsSoundByStringIndex( pprop->sounds.stepleft );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.stepright );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.impactSoft );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.impactHard );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.scrapeSmooth );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.scrapeRough );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.bulletImpact );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.rolling );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.breakSound );
		PrecachePhysicsSoundByStringIndex( pprop->sounds.strainSound );
	}
}

void CPhysicsHook::LevelInitPreEntity() 
{
	physenv = physics->CreateEnvironment();
	g_EntityCollisionHash = physics->CreateObjectPairHash();
	factorylist_t factories;
	FactoryList_Retrieve( factories );
	physenv->SetDebugOverlay( factories.engineFactory );
	physenv->EnableDeleteQueue( true );

	physenv->SetCollisionSolver( &g_Collisions );
	physenv->SetCollisionEventHandler( &g_Collisions );
	physenv->SetConstraintEventHandler( g_pConstraintEvents );
	physenv->SetObjectEventHandler( &g_Collisions );
	
	physenv->SetSimulationTimestep( TICK_INTERVAL ); // 15 ms per tick
	// HL Game gravity, not real-world gravity
	physenv->SetGravity( Vector( 0, 0, -sv_gravity.GetFloat() ) );
	g_PhysAverageSimTime = 0;

	g_PhysWorldObject = PhysCreateWorld( GetWorldEntity() );

	g_pShadowEntities = new CEntityList;

	PrecachePhysicsSounds();

	m_bPaused = true;
}



void CPhysicsHook::LevelInitPostEntity() 
{
	m_bPaused = false;
}

void CPhysicsHook::LevelShutdownPreEntity() 
{
	if ( !physenv )
		return;
	physenv->SetQuickDelete( true );
}

void CPhysicsHook::LevelShutdownPostEntity() 
{
	if ( !physenv )
		return;

	g_pPhysSaveRestoreManager->ForgetAllModels();

	g_Collisions.LevelShutdown();

	physics->DestroyEnvironment( physenv );
	physenv = NULL;

	physics->DestroyObjectPairHash( g_EntityCollisionHash );
	g_EntityCollisionHash = NULL;

	physics->DestroyAllCollisionSets();

	g_PhysWorldObject = NULL;

	delete g_pShadowEntities;
	g_pShadowEntities = NULL;
	m_impactSounds.RemoveAll();
	m_massCenterOverrides.Purge();

}

// called after entities think
void CPhysicsHook::FrameUpdatePostEntityThink( ) 
{
	VPROF_BUDGET( "CPhysicsHook::FrameUpdatePostEntityThink", VPROF_BUDGETGROUP_PHYSICS );

	// Tracker 24846:  If game is paused, don't simulate vphysics
	float interval = ( gpGlobals->frametime > 0.0f ) ? TICK_INTERVAL : 0.0f;

	// update the physics simulation, not we don't use gpGlobals->frametime, since that can be 30 msec or 15 msec
	// depending on whether IsSimulatingOnAlternateTicks is true or not
	if ( CBaseEntity::IsSimulatingOnAlternateTicks() )
	{
		m_isFinalTick = false;
		PhysFrame( interval );
	}
	m_isFinalTick = true;
	PhysFrame( interval );
}

void CPhysicsHook::PreClientUpdate()
{
	physicssound::PlayImpactSounds( m_impactSounds );
}

bool PhysIsFinalTick()
{
	return g_PhysicsHook.m_isFinalTick;
}

IPhysicsObject *PhysCreateWorld( CBaseEntity *pWorld )
{
	staticpropmgr->CreateVPhysicsRepresentations( physenv, &g_SolidSetup, pWorld );
	return PhysCreateWorld_Shared( pWorld, modelinfo->GetVCollide(1), g_PhysDefaultObjectParams );
}


// vehicle wheels can only collide with things that can't get stuck in them during game physics
// because they aren't in the game physics world at present
static bool WheelCollidesWith( IPhysicsObject *pObj, CBaseEntity *pEntity )
{
#if defined( TF2_DLL )
	if ( pEntity->GetCollisionGroup() == TFCOLLISION_GROUP_OBJECT )
		return false;
#endif

	if ( pEntity->GetMoveType() == MOVETYPE_PUSH || pEntity->GetMoveType() == MOVETYPE_VPHYSICS || pObj->IsStatic() )
		return true;

	if ( pEntity->GetCollisionGroup() == COLLISION_GROUP_INTERACTIVE_DEBRIS )
		return false;

	return false;
}

CCollisionEvent::CCollisionEvent()
{
	m_inCallback = 0;
}

int CCollisionEvent::ShouldCollide( IPhysicsObject *pObj0, IPhysicsObject *pObj1, void *pGameData0, void *pGameData1 )
{
	CallbackContext check(this);

	CBaseEntity *pEntity0 = static_cast<CBaseEntity *>(pGameData0);
	CBaseEntity *pEntity1 = static_cast<CBaseEntity *>(pGameData1);

	if ( !pEntity0 || !pEntity1 )
		return 1;

	unsigned short gameFlags0 = pObj0->GetGameFlags();
	unsigned short gameFlags1 = pObj1->GetGameFlags();

	if ( pEntity0 == pEntity1 )
	{
		// allow all-or-nothing per-entity disable
		if ( (gameFlags0 | gameFlags1) & FVPHYSICS_NO_SELF_COLLISIONS )
			return 0;

		IPhysicsCollisionSet *pSet = physics->FindCollisionSet( pEntity0->GetModelIndex() );
		if ( pSet )
			return pSet->ShouldCollide( pObj0->GetGameIndex(), pObj1->GetGameIndex() );

		return 1;
	}

	// objects that are both constrained to the world don't collide with each other
	if ( (gameFlags0 & gameFlags1) & FVPHYSICS_CONSTRAINT_STATIC )
	{
		return 0;
	}

	// Special collision rules for vehicle wheels
	// Their entity collides with stuff using the normal rules, but they
	// have different rules than the vehicle body for various reasons.
	// sort of a hack because we don't have spheres to represent them in the game
	// world for speculative collisions.
	if ( pObj0->GetCallbackFlags() & CALLBACK_IS_VEHICLE_WHEEL )
	{
		if ( !WheelCollidesWith( pObj1, pEntity1 ) )
			return false;
	}
	if ( pObj1->GetCallbackFlags() & CALLBACK_IS_VEHICLE_WHEEL )
	{
		if ( !WheelCollidesWith( pObj0, pEntity0 ) )
			return false;
	}

	if ( pEntity0->ForceVPhysicsCollide( pEntity1 ) || pEntity1->ForceVPhysicsCollide( pEntity0 ) )
		return 1;

	if ( pEntity0->edict() && pEntity1->edict() )
	{
		// don't collide with your owner
		if ( pEntity0->GetOwnerEntity() == pEntity1 || pEntity1->GetOwnerEntity() == pEntity0 )
			return 0;
	}

	if ( pEntity0->GetMoveParent() || pEntity1->GetMoveParent() )
	{
		CBaseEntity *pParent0 = pEntity0->GetRootMoveParent();
		CBaseEntity *pParent1 = pEntity1->GetRootMoveParent();
		
		// NOTE: Don't let siblings/parents collide.  If you want this behavior, do it
		// with constraints, not hierarchy!
		if ( pParent0 == pParent1 )
			return 0;

		if ( g_EntityCollisionHash->IsObjectPairInHash( pParent0, pParent1 ) )
			return 0;

		IPhysicsObject *p0 = pParent0->VPhysicsGetObject();
		IPhysicsObject *p1 = pParent1->VPhysicsGetObject();
		if ( p0 && p1 )
		{
			if ( g_EntityCollisionHash->IsObjectPairInHash( p0, p1 ) )
				return 0;
		}
	}

	int solid0 = pEntity0->GetSolid();
	int solid1 = pEntity1->GetSolid();
	int nSolidFlags0 = pEntity0->GetSolidFlags();
	int nSolidFlags1 = pEntity1->GetSolidFlags();

	int movetype0 = pEntity0->GetMoveType();
	int movetype1 = pEntity1->GetMoveType();

	// entities with non-physical move parents or entities with MOVETYPE_PUSH
	// are considered as "AI movers".  They are unchanged by collision; they exert
	// physics forces on the rest of the system.
	bool aiMove0 = (movetype0==MOVETYPE_PUSH) ? true : false;
	bool aiMove1 = (movetype1==MOVETYPE_PUSH) ? true : false;

	if ( pEntity0->GetMoveParent() )
	{
		// if the object & its parent are both MOVETYPE_VPHYSICS, then this must be a special case
		// like a prop_ragdoll_attached
		if ( !(movetype0 == MOVETYPE_VPHYSICS && pEntity0->GetRootMoveParent()->GetMoveType() == MOVETYPE_VPHYSICS) )
		{
			aiMove0 = true;
		}
	}
	if ( pEntity1->GetMoveParent() )
	{
		// if the object & its parent are both MOVETYPE_VPHYSICS, then this must be a special case.
		if ( !(movetype1 == MOVETYPE_VPHYSICS && pEntity1->GetRootMoveParent()->GetMoveType() == MOVETYPE_VPHYSICS) )
		{
			aiMove1 = true;
		}
	}

	// AI movers don't collide with the world/static/pinned objects or other AI movers
	if ( (aiMove0 && !pObj1->IsMoveable()) ||
		(aiMove1 && !pObj0->IsMoveable()) ||
		(aiMove0 && aiMove1) )
		return 0;

	// BRJ 1/24/03
	// You can remove the assert if it's problematic; I *believe* this condition
	// should be met, but I'm not sure.
	//Assert ( (solid0 != SOLID_NONE) && (solid1 != SOLID_NONE) );
	if ( (solid0 == SOLID_NONE) || (solid1 == SOLID_NONE) )
		return 0;

	// not solid doesn't collide with anything
	if ( (nSolidFlags0|nSolidFlags1) & FSOLID_NOT_SOLID )
	{
		// might be a vphysics trigger, collide with everything but "not solid"
		if ( pObj0->IsTrigger() && !(nSolidFlags1 & FSOLID_NOT_SOLID) )
			return 1;
		if ( pObj1->IsTrigger() && !(nSolidFlags0 & FSOLID_NOT_SOLID) )
			return 1;

		return 0;
	}
	
	if ( (nSolidFlags0 & FSOLID_TRIGGER) && 
		!(solid1 == SOLID_VPHYSICS || solid1 == SOLID_BSP || movetype1 == MOVETYPE_VPHYSICS) )
		return 0;

	if ( (nSolidFlags1 & FSOLID_TRIGGER) && 
		!(solid0 == SOLID_VPHYSICS || solid0 == SOLID_BSP || movetype0 == MOVETYPE_VPHYSICS) )
		return 0;

	if ( !g_pGameRules->ShouldCollide( pEntity0->GetCollisionGroup(), pEntity1->GetCollisionGroup() ) )
		return 0;

	// check contents
	if ( !(pObj0->GetContents() & pEntity1->PhysicsSolidMaskForEntity()) || !(pObj1->GetContents() & pEntity0->PhysicsSolidMaskForEntity()) )
		return 0;

	if ( g_EntityCollisionHash->IsObjectPairInHash( pGameData0, pGameData1 ) )
		return 0;

	if ( g_EntityCollisionHash->IsObjectPairInHash( pObj0, pObj1 ) )
		return 0;

	return 1;
}

bool PhysShouldCollide( IPhysicsObject *pObj0, IPhysicsObject *pObj1 )
{
	void *pGameData0 = pObj0->GetGameData();
	void *pGameData1 = pObj1->GetGameData();
	if ( !pGameData0 || !pGameData1 )
		return false;
	return g_Collisions.ShouldCollide( pObj0, pObj1, pGameData0, pGameData1 ) ? true : false;
}

bool PhysIsInCallback()
{
	if ( physenv->IsInSimulation() || g_Collisions.IsInCallback() )
		return true;

	return false;
}


static void ReportPenetration( CBaseEntity *pEntity, float duration )
{
	if ( pEntity->GetMoveType() == MOVETYPE_VPHYSICS )
	{
		if ( g_pDeveloper->GetInt() > 1 )
		{
			pEntity->m_debugOverlays |= OVERLAY_ABSBOX_BIT;
		}
		pEntity->AddTimedOverlay( "VPhysics Penetration Error!", duration );
	}
}

static void UpdateEntityPenetrationFlag( CBaseEntity *pEntity, bool isPenetrating )
{
	if ( !pEntity )
		return;
	IPhysicsObject *pList[VPHYSICS_MAX_OBJECT_LIST_COUNT];
	int count = pEntity->VPhysicsGetObjectList( pList, ARRAYSIZE(pList) );
	for ( int i = 0; i < count; i++ )
	{
		if ( !pList[i]->IsStatic() )
		{
			if ( isPenetrating )
			{
				PhysSetGameFlags( pList[i], FVPHYSICS_PENETRATING );
			}
			else
			{
				PhysClearGameFlags( pList[i], FVPHYSICS_PENETRATING );
			}
		}
	}
}

void CCollisionEvent::GetListOfPenetratingEntities( CBaseEntity *pSearch, CUtlVector<CBaseEntity *> &list )
{
	for ( int i = m_penetrateEvents.Count()-1; i >= 0; --i )
	{
		if ( m_penetrateEvents[i].hEntity0 == pSearch && m_penetrateEvents[i].hEntity1.Get() != NULL )
		{
			list.AddToTail( m_penetrateEvents[i].hEntity1 );
		}
		else if ( m_penetrateEvents[i].hEntity1 == pSearch && m_penetrateEvents[i].hEntity0.Get() != NULL )
		{
			list.AddToTail( m_penetrateEvents[i].hEntity0 );
		}
	}
}

void CCollisionEvent::UpdatePenetrateEvents( void )
{
	for ( int i = m_penetrateEvents.Count()-1; i >= 0; --i )
	{
		CBaseEntity *pEntity0 = m_penetrateEvents[i].hEntity0;
		CBaseEntity *pEntity1 = m_penetrateEvents[i].hEntity1;

		if ( m_penetrateEvents[i].collisionState == COLLSTATE_TRYDISABLE )
		{
			if ( pEntity0 && pEntity1 )
			{
				IPhysicsObject *pObj0 = pEntity0->VPhysicsGetObject();
				if ( pObj0 )
				{
					PhysForceEntityToSleep( pEntity0, pObj0 );
				}
				IPhysicsObject *pObj1 = pEntity1->VPhysicsGetObject();
				if ( pObj1 )
				{
					PhysForceEntityToSleep( pEntity1, pObj1 );
				}
				m_penetrateEvents[i].collisionState = COLLSTATE_DISABLED;
				continue;
			}
			// missing entity or object, clear event
		}
		else if ( m_penetrateEvents[i].collisionState == COLLSTATE_TRYNPCSOLVER )
		{
			if ( pEntity0 && pEntity1 )
			{
				CAI_BaseNPC *pNPC = pEntity0->MyNPCPointer();
				CBaseEntity *pBlocker = pEntity1;
				if ( !pNPC )
				{
					pNPC = pEntity1->MyNPCPointer();
					Assert(pNPC);
					pBlocker = pEntity0;
				}
				NPCPhysics_CreateSolver( pNPC, pBlocker, true, 1.0f );
			}
			// transferred to solver, clear event
		}
		else if ( gpGlobals->curtime - m_penetrateEvents[i].timeStamp > 1.0 )
		{
			if ( m_penetrateEvents[i].collisionState == COLLSTATE_DISABLED )
			{
				if ( pEntity0 && pEntity1 )
				{
					IPhysicsObject *pObj0 = pEntity0->VPhysicsGetObject();
					IPhysicsObject *pObj1 = pEntity1->VPhysicsGetObject();
					if ( pObj0 && pObj1 )
					{
						m_penetrateEvents[i].collisionState = COLLSTATE_ENABLED;
						continue;
					}
				}
			}
			// haven't penetrated for 1 second, so remove
		}
		else
		{
			// recent timestamp, don't remove the event yet
			continue;
		}
		// done, clear event
		m_penetrateEvents.FastRemove(i);
		UpdateEntityPenetrationFlag( pEntity0, false );
		UpdateEntityPenetrationFlag( pEntity1, false );
	}
}

penetrateevent_t &CCollisionEvent::FindOrAddPenetrateEvent( CBaseEntity *pEntity0, CBaseEntity *pEntity1 )
{
	int index = -1;
	for ( int i = m_penetrateEvents.Count()-1; i >= 0; --i )
	{
		if ( m_penetrateEvents[i].hEntity0.Get() == pEntity0 && m_penetrateEvents[i].hEntity1.Get() == pEntity1 )
		{
			index = i;
			break;
		}
	}
	if ( index < 0 )
	{
		index = m_penetrateEvents.AddToTail();
		penetrateevent_t &event = m_penetrateEvents[index];
		event.hEntity0 = pEntity0;
		event.hEntity1 = pEntity1;
		event.startTime = gpGlobals->curtime;
		event.collisionState = COLLSTATE_ENABLED;
		UpdateEntityPenetrationFlag( pEntity0, true );
		UpdateEntityPenetrationFlag( pEntity1, true );
	}
	penetrateevent_t &event = m_penetrateEvents[index];
	event.timeStamp = gpGlobals->curtime;
	return event;
}


//-----------------------------------------------------------------------------
// Impulse events
//-----------------------------------------------------------------------------
void CCollisionEvent::UpdateImpulseEvents()
{
	int nCount = m_impulseEvents.Count();
	for ( int i = 0; i < nCount; ++i )
	{
		m_impulseEvents[i].pObject->ApplyForceCenter( m_impulseEvents[i].vecCenterForce );
		m_impulseEvents[i].pObject->ApplyTorqueCenter( m_impulseEvents[i].vecCenterTorque );
	}
	m_impulseEvents.RemoveAll();
}

//-----------------------------------------------------------------------------
// Set velocity events
//-----------------------------------------------------------------------------
void CCollisionEvent::UpdateSetVelocityEvents()
{
	int nCount = m_setVelocityEvents.Count();
	for ( int i = 0; i < nCount; ++i )
	{
		m_setVelocityEvents[i].pObject->SetVelocity( &m_setVelocityEvents[i].vecVelocity, NULL );
	}
	m_setVelocityEvents.RemoveAll();
}


static ConVar phys_penetration_error_time( "phys_penetration_error_time", "10", 0, "Controls the duration of vphysics penetration error boxes." );


int CCollisionEvent::ShouldSolvePenetration( IPhysicsObject *pObj0, IPhysicsObject *pObj1, void *pGameData0, void *pGameData1, float dt )
{
	CallbackContext check(this);
	// this can get called as entities are being constructed on the other side of a game load or level transition
	// Some entities may not be fully constructed, so don't call into their code until the level is running
	if ( g_PhysicsHook.m_bPaused )
		return true;

	// solve it yourself here and return 0, or have the default implementation do it
	CBaseEntity *pEntity0 = static_cast<CBaseEntity *>(pGameData0);
	CBaseEntity *pEntity1 = static_cast<CBaseEntity *>(pGameData1);
	if ( pEntity0 > pEntity1 )
	{
		// swap sort
		CBaseEntity *pTmp = pEntity0;
		pEntity0 = pEntity1;
		pEntity1 = pTmp;
	}
	penetrateevent_t &event = FindOrAddPenetrateEvent( pEntity0, pEntity1 );
	float eventTime = gpGlobals->curtime - event.startTime;
	
	// NPC vs. vehicle.  Create a game DLL solver and remove this event
	if ( (pEntity0->MyNPCPointer() && pEntity1->GetServerVehicle()) ||
		(pEntity1->MyNPCPointer() && pEntity0->GetServerVehicle()) )
	{
		event.collisionState = COLLSTATE_TRYNPCSOLVER;
	}

#if _DEBUG
	const char *pName1 = STRING(pEntity0->GetModelName());
	const char *pName2 = STRING(pEntity1->GetModelName());
	if ( pEntity0 == pEntity1 )
	{
		int index0 = physcollision->CollideIndex( pObj0->GetCollide() );
		int index1 = physcollision->CollideIndex( pObj1->GetCollide() );
		DevMsg(2, "***Inter-penetration on %s (%d & %d) (%.0f, %.0f)\n", pName1?pName1:"(null)", index0, index1, gpGlobals->curtime, eventTime );
	}
	else
	{
		DevMsg(2, "***Inter-penetration between %s AND %s (%.0f, %.0f)\n", pName1?pName1:"(null)", pName2?pName2:"(null)", gpGlobals->curtime, eventTime );
	}

#endif

	if ( eventTime > 3 )
	{
		// don't put players or game physics controlled objects to sleep
		if ( !pEntity0->IsPlayer() && !pEntity1->IsPlayer() && !pObj0->GetShadowController() && !pObj1->GetShadowController() )
		{
			// two objects have been stuck for more than 3 seconds, try disabling simulation
			event.collisionState = COLLSTATE_TRYDISABLE;
		}
		if ( g_pDeveloper->GetInt() )
		{
			ReportPenetration( pEntity0, phys_penetration_error_time.GetFloat() );
			ReportPenetration( pEntity1, phys_penetration_error_time.GetFloat() );
		}
		event.startTime = gpGlobals->curtime;
	}


	return 1;
}


void CCollisionEvent::FluidStartTouch( IPhysicsObject *pObject, IPhysicsFluidController *pFluid ) 
{
	CallbackContext check(this);
	if ( ( pObject == NULL ) || ( pFluid == NULL ) )
		return;

	CBaseEntity *pEntity = static_cast<CBaseEntity *>(pObject->GetGameData());
	if ( !pEntity )
		return;

	pEntity->AddEFlags( EFL_TOUCHING_FLUID );
	pEntity->OnEntityEvent( ENTITY_EVENT_WATER_TOUCH, (void*)pFluid->GetContents() );

	float timeSinceLastCollision = DeltaTimeSinceLastFluid( pEntity );
	if ( timeSinceLastCollision < 0.5f )
		return;

	// UNDONE: Use this for splash logic instead?
	// UNDONE: Use angular term too - push splashes in rotAxs cross normal direction?
	Vector normal;
	float dist;
	pFluid->GetSurfacePlane( &normal, &dist );
	Vector vel;
	AngularImpulse angVel;
	pObject->GetVelocity( &vel, &angVel );
	Vector unitVel = vel;
	VectorNormalize( unitVel );
	
	// normal points out of the surface, we want the direction that points in
	float dragScale = pFluid->GetDensity() * physenv->GetSimulationTimestep();
	normal = -normal;
	float linearScale = 0.5f * DotProduct( unitVel, normal ) * pObject->CalculateLinearDrag( normal ) * dragScale;
	linearScale = clamp( linearScale, 0.0f, 1.0f );
	vel *= -linearScale;

	// UNDONE: Figure out how much of the surface area has crossed the water surface and scale angScale by that
	// For now assume 25%
	Vector rotAxis = angVel;
	VectorNormalize(rotAxis);
	float angScale = 0.25f * pObject->CalculateAngularDrag( angVel ) * dragScale;
	angScale = clamp( angScale, 0.0f, 1.0f );
	angVel *= -angScale;
	
	// compute the splash before we modify the velocity
	PhysicsSplash( pFluid, pObject, pEntity );

	// now damp out some motion toward the surface
	pObject->AddVelocity( &vel, &angVel );
}

void CCollisionEvent::FluidEndTouch( IPhysicsObject *pObject, IPhysicsFluidController *pFluid ) 
{
	CallbackContext check(this);
	if ( ( pObject == NULL ) || ( pFluid == NULL ) )
		return;

	CBaseEntity *pEntity = static_cast<CBaseEntity *>(pObject->GetGameData());
	if ( !pEntity )
		return;

	float timeSinceLastCollision = DeltaTimeSinceLastFluid( pEntity );
	if ( timeSinceLastCollision >= 0.5f )
	{
		PhysicsSplash( pFluid, pObject, pEntity );
	}

	pEntity->RemoveEFlags( EFL_TOUCHING_FLUID );
	pEntity->OnEntityEvent( ENTITY_EVENT_WATER_UNTOUCH, (void*)pFluid->GetContents() );
}

class CSkipKeys : public IVPhysicsKeyHandler
{
public:
	virtual void ParseKeyValue( void *pData, const char *pKey, const char *pValue ) {}
	virtual void SetDefaults( void *pData ) {}
};

void PhysSolidOverride( solid_t &solid, string_t overrideScript )
{
	if ( overrideScript != NULL_STRING)
	{
		// parser destroys this data
		bool collisions = solid.params.enableCollisions;

		char pTmpString[4096];

		// write a header for a solid_t
		Q_strncpy( pTmpString, "solid { ", sizeof(pTmpString) );

		// suck out the comma delimited tokens and turn them into quoted key/values
		char szToken[256];
		const char *pStr = nexttoken(szToken, STRING(overrideScript), ',');
		while ( szToken[0] != 0 )
		{
			Q_strncat( pTmpString, "\"", sizeof(pTmpString), COPY_ALL_CHARACTERS );
			Q_strncat( pTmpString, szToken, sizeof(pTmpString), COPY_ALL_CHARACTERS );
			Q_strncat( pTmpString, "\" ", sizeof(pTmpString), COPY_ALL_CHARACTERS );
			pStr = nexttoken(szToken, pStr, ',');
		}
		// terminate the script
		Q_strncat( pTmpString, "}", sizeof(pTmpString), COPY_ALL_CHARACTERS );

		// parse that sucker
		IVPhysicsKeyParser *pParse = physcollision->VPhysicsKeyParserCreate( pTmpString );
		CSkipKeys tmp;
		pParse->ParseSolid( &solid, &tmp );
		physcollision->VPhysicsKeyParserDestroy( pParse );

		// parser destroys this data
		solid.params.enableCollisions = collisions;
	}
}

void PhysSetMassCenterOverride( masscenteroverride_t &override )
{
	if ( override.entityName != NULL_STRING )
	{
		g_PhysicsHook.m_massCenterOverrides.AddToTail( override );
	}
}

// NOTE: This will remove the entry from the list as well
int PhysGetMassCenterOverrideIndex( string_t name )
{
	if ( name != NULL_STRING && g_PhysicsHook.m_massCenterOverrides.Count() )
	{
		for ( int i = 0; i < g_PhysicsHook.m_massCenterOverrides.Count(); i++ )
		{
			if ( g_PhysicsHook.m_massCenterOverrides[i].entityName == name )
			{
				return i;
			}
		}
	}
	return -1;
}

void PhysGetMassCenterOverride( CBaseEntity *pEntity, vcollide_t *pCollide, solid_t &solidOut )
{
	int index = PhysGetMassCenterOverrideIndex( pEntity->GetEntityName() );

	if ( index >= 0 )
	{
		masscenteroverride_t &override = g_PhysicsHook.m_massCenterOverrides[index];
		Vector massCenterWS = override.center;
		switch ( override.alignType )
		{
		case masscenteroverride_t::ALIGN_POINT:
			VectorITransform( massCenterWS, pEntity->EntityToWorldTransform(), solidOut.massCenterOverride );
			break;
		case masscenteroverride_t::ALIGN_AXIS:
			{
				Vector massCenterLocal, defaultMassCenterWS;
				physcollision->CollideGetMassCenter( pCollide->solids[solidOut.index], &massCenterLocal );
				VectorTransform( massCenterLocal, pEntity->EntityToWorldTransform(), defaultMassCenterWS );
				massCenterWS += override.axis * 
					( DotProduct(defaultMassCenterWS,override.axis) - DotProduct( override.axis, override.center ) );
				VectorITransform( massCenterWS, pEntity->EntityToWorldTransform(), solidOut.massCenterOverride );
			}
			break;
		}
		g_PhysicsHook.m_massCenterOverrides.FastRemove( index );

		if ( solidOut.massCenterOverride.Length() > DIST_EPSILON )
		{
			solidOut.params.massCenterOverride = &solidOut.massCenterOverride;
		}
	}
}

float PhysGetEntityMass( CBaseEntity *pEntity )
{
	IPhysicsObject *pList[VPHYSICS_MAX_OBJECT_LIST_COUNT];
	int physCount = pEntity->VPhysicsGetObjectList( pList, ARRAYSIZE(pList) );
	float otherMass = 0;
	for ( int i = 0; i < physCount; i++ )
	{
		otherMass += pList[i]->GetMass();
	}

	return otherMass;
}

typedef void (*EntityCallbackFunction) ( CBaseEntity *pEntity );

void IterateActivePhysicsEntities( EntityCallbackFunction func )
{
	int activeCount = physenv->GetActiveObjectCount();
	IPhysicsObject **pActiveList = NULL;
	if ( activeCount )
	{
		pActiveList = (IPhysicsObject **)stackalloc( sizeof(IPhysicsObject *)*activeCount );
		physenv->GetActiveObjects( pActiveList );
		for ( int i = 0; i < activeCount; i++ )
		{
			CBaseEntity *pEntity = reinterpret_cast<CBaseEntity *>(pActiveList[i]->GetGameData());
			if ( pEntity )
			{
				func( pEntity );
			}
		}
	}
}


static void CallbackHighlight( CBaseEntity *pEntity )
{
	pEntity->m_debugOverlays |= OVERLAY_ABSBOX_BIT | OVERLAY_PIVOT_BIT;
}

static void CallbackReport( CBaseEntity *pEntity )
{
	Msg( "%s\n", pEntity->GetClassname() );
}

CON_COMMAND(physics_highlight_active, "Turns on the absbox for all active physics objects")
{
	IterateActivePhysicsEntities( CallbackHighlight );
}

CON_COMMAND(physics_report_active, "Lists all active physics objects")
{
	IterateActivePhysicsEntities( CallbackReport );
}

CON_COMMAND_F(surfaceprop, "Reports the surface properties at the cursor", FCVAR_CHEAT )
{
	CBasePlayer *pPlayer = UTIL_GetCommandClient();

	trace_t tr;
	Vector forward;
	pPlayer->EyeVectors( &forward );
	UTIL_TraceLine(pPlayer->EyePosition(), pPlayer->EyePosition() + forward * MAX_COORD_RANGE,
		MASK_SHOT_HULL|CONTENTS_GRATE|CONTENTS_DEBRIS, pPlayer, COLLISION_GROUP_NONE, &tr );

	if ( tr.DidHit() )
	{
		const model_t *pModel = modelinfo->GetModel( tr.m_pEnt->GetModelIndex() );
		const char *pModelName = STRING(tr.m_pEnt->GetModelName());
		if ( tr.DidHitWorld() && tr.hitbox > 0 )
		{
			ICollideable *pCollide = staticpropmgr->GetStaticPropByIndex( tr.hitbox-1 );
			pModel = pCollide->GetCollisionModel();
			pModelName = modelinfo->GetModelName( pModel );
		}
		CFmtStr modelStuff;
		if ( pModel )
		{
			modelStuff.sprintf("%s.%s ", modelinfo->IsTranslucent( pModel ) ? "Translucent" : "Opaque", 
				modelinfo->IsTranslucentTwoPass( pModel ) ? "  Two-pass." : "" );
		}
		Msg("Hit surface \"%s\" (entity %s, model \"%s\" %s), texture \"%s\"\n", physprops->GetPropName( tr.surface.surfaceProps ), tr.m_pEnt->GetClassname(), pModelName, modelStuff.Access(), tr.surface.name );
	}
}

static void OutputVPhysicsDebugInfo( CBaseEntity *pEntity )
{
	if ( pEntity )
	{
		Msg("Entity %s (%s) %s\n", pEntity->GetClassname(), pEntity->GetDebugName(), pEntity->IsNavIgnored() ? "NAV IGNORE" : "" );
		IPhysicsObject *pPhysics = pEntity->VPhysicsGetObject();
		if ( pPhysics )
		{
			pPhysics->OutputDebugInfo();
		}
	}
}

static void MarkVPhysicsDebug( CBaseEntity *pEntity )
{
	if ( pEntity )
	{
		IPhysicsObject *pPhysics = pEntity->VPhysicsGetObject();
		if ( pPhysics )
		{
			unsigned short callbacks = pPhysics->GetCallbackFlags();
			callbacks ^= CALLBACK_MARKED_FOR_TEST;
			pPhysics->SetCallbackFlags( callbacks );
		}
	}
}

void PhysicsCommand( void (*func)( CBaseEntity *pEntity ) )
{
	if ( engine->Cmd_Argc() < 2 )
	{
		CBasePlayer *pPlayer = UTIL_GetCommandClient();

		trace_t tr;
		Vector forward;
		pPlayer->EyeVectors( &forward );
		UTIL_TraceLine(pPlayer->EyePosition(), pPlayer->EyePosition() + forward * MAX_COORD_RANGE,
			MASK_SHOT_HULL|CONTENTS_GRATE|CONTENTS_DEBRIS, pPlayer, COLLISION_GROUP_NONE, &tr );

		if ( tr.DidHit() )
		{
			func( tr.m_pEnt );
		}
	}
	else
	{
		CBaseEntity *pEnt = NULL;
		while ((pEnt = gEntList.FindEntityGeneric(pEnt, engine->Cmd_Argv(1))) != NULL)
		{
			func( pEnt );
		}
	}
}

CON_COMMAND(physics_debug_entity, "Dumps debug info for an entity")
{
	PhysicsCommand( OutputVPhysicsDebugInfo );
}

CON_COMMAND(physics_select, "Dumps debug info for an entity")
{
	PhysicsCommand( MarkVPhysicsDebug );
}

CON_COMMAND( physics_budget, "Times the cost of each active object" )
{
	int activeCount = physenv->GetActiveObjectCount();

	IPhysicsObject **pActiveList = NULL;
	CUtlVector<CBaseEntity *> ents;
	if ( activeCount )
	{
		int i;

		pActiveList = (IPhysicsObject **)stackalloc( sizeof(IPhysicsObject *)*activeCount );
		physenv->GetActiveObjects( pActiveList );
		for ( i = 0; i < activeCount; i++ )
		{
			CBaseEntity *pEntity = reinterpret_cast<CBaseEntity *>(pActiveList[i]->GetGameData());
			if ( pEntity )
			{
				int index = -1;
				for ( int j = 0; j < ents.Count(); j++ )
				{
					if ( pEntity == ents[j] )
					{
						index = j;
						break;
					}
				}
				if ( index >= 0 )
					continue;

				ents.AddToTail( pEntity );
			}
		}
		stackfree( pActiveList );

		if ( !ents.Count() )
			return;

		CUtlVector<float> times;
		float totalTime = 0.f;
		g_Collisions.BufferTouchEvents( true );
		float full = engine->Time();
		physenv->Simulate( DEFAULT_TICK_INTERVAL );
		full = engine->Time() - full;
		float lastTime = full;

		times.SetSize( ents.Count() );


		// NOTE: This is just a heuristic.  Attempt to estimate cost by putting each object to sleep in turn.
		//	note that simulation may wake the objects again and some costs scale with sets of objects/constraints/etc
		//	so these are only generally useful for broad questions, not real metrics!
		for ( i = 0; i < ents.Count(); i++ )
		{
			for ( int j = 0; j < i; j++ )
			{
				PhysForceEntityToSleep( ents[j], ents[j]->VPhysicsGetObject() );
			}
			float start = engine->Time();
			physenv->Simulate( DEFAULT_TICK_INTERVAL );
			float end = engine->Time();

			float elapsed = end - start;
			float avgTime = lastTime - elapsed;
			times[i] = clamp( avgTime, 0.00001f, 1.0f );
			totalTime += times[i];
			lastTime = elapsed;
 		}

		totalTime = max( totalTime, 0.001 );
		for ( i = 0; i < ents.Count(); i++ )
		{
			float fraction = times[i] / totalTime;
			Msg( "%s (%s): %.3fms (%.3f%%) @ %s\n", ents[i]->GetClassname(), ents[i]->GetDebugName(), fraction * totalTime * 1000.0f, fraction * 100.0f, VecToString(ents[i]->GetAbsOrigin()) );
		}
		g_Collisions.BufferTouchEvents( false );
	}

}

// Advance physics by time (in seconds)
void PhysFrame( float deltaTime )
{
	static int lastObjectCount = 0;
	entitem_t *pItem;

	if ( !g_PhysicsHook.ShouldSimulate() )
		return;

	// Trap interrupts and clock changes
	if ( deltaTime > 1.0f || deltaTime < 0.0f )
	{
		deltaTime = 0;
		Msg( "Reset physics clock\n" );
	}
	else if ( deltaTime > 0.1f )	// limit incoming time to 100ms
	{
		deltaTime = 0.1f;
	}
	float simRealTime = 0;

	deltaTime *= phys_timescale.GetFloat();
	// !!!HACKHACK -- hard limit scaled time to avoid spending too much time in here
	// Limit to 100 ms
	if ( deltaTime > 0.100f )
		deltaTime = 0.100f;

	bool bProfile = phys_speeds.GetBool();

	if ( bProfile )
	{
		simRealTime = engine->Time();
	}
	g_Collisions.BufferTouchEvents( true );
	physenv->Simulate( deltaTime );

	int activeCount = physenv->GetActiveObjectCount();
	IPhysicsObject **pActiveList = NULL;
	if ( activeCount )
	{
		pActiveList = (IPhysicsObject **)stackalloc( sizeof(IPhysicsObject *)*activeCount );
		physenv->GetActiveObjects( pActiveList );

		for ( int i = 0; i < activeCount; i++ )
		{
			CBaseEntity *pEntity = reinterpret_cast<CBaseEntity *>(pActiveList[i]->GetGameData());
			if ( pEntity )
			{
				if ( pEntity->CollisionProp()->DoesVPhysicsInvalidateSurroundingBox() )
				{
					pEntity->CollisionProp()->MarkSurroundingBoundsDirty();
				}
				pEntity->VPhysicsUpdate( pActiveList[i] );
			}
		}
		stackfree( pActiveList );
	}

	for ( pItem = g_pShadowEntities->m_pItemList; pItem; pItem = pItem->pNext )
	{
		CBaseEntity *pEntity = pItem->hEnt.Get();
		if ( !pEntity )
		{
			Msg( "Dangling pointer to physics entity!!!\n" );
			continue;
		}

		IPhysicsObject *pPhysics = pEntity->VPhysicsGetObject();
		// apply updates
		if ( pPhysics && !pPhysics->IsAsleep() )
		{
			pEntity->VPhysicsShadowUpdate( pPhysics );
		}
	}


	if ( bProfile )
	{
		simRealTime = engine->Time() - simRealTime;

		if ( simRealTime < 0 )
			simRealTime = 0;
		g_PhysAverageSimTime *= 0.8;
		g_PhysAverageSimTime += (simRealTime * 0.2);
		if ( lastObjectCount != 0 || activeCount != 0 )
		{
			Msg( "Physics: %3d objects, %4.1fms / AVG: %4.1fms\n", activeCount, simRealTime * 1000, g_PhysAverageSimTime * 1000 );
		}
		
		lastObjectCount = activeCount;
	}
	g_Collisions.BufferTouchEvents( false );
	g_Collisions.FrameUpdate();
}


void PhysAddShadow( CBaseEntity *pEntity )
{
	g_pShadowEntities->AddEntity( pEntity );
}

void PhysRemoveShadow( CBaseEntity *pEntity )
{
	g_pShadowEntities->DeleteEntity( pEntity );
}

void PhysEnableFloating( IPhysicsObject *pObject, bool bEnable )
{
	if ( pObject != NULL )
	{
		unsigned short flags = pObject->GetCallbackFlags();
		if ( bEnable )
		{
			flags |= CALLBACK_DO_FLUID_SIMULATION;
		}
		else
		{
			flags &= ~CALLBACK_DO_FLUID_SIMULATION;
		}
		pObject->SetCallbackFlags( flags );
	}
}


//-----------------------------------------------------------------------------
// CollisionEvent system 
//-----------------------------------------------------------------------------
// NOTE: PreCollision/PostCollision ALWAYS come in matched pairs!!!
void CCollisionEvent::PreCollision( vcollisionevent_t *pEvent )
{
	CallbackContext check(this);
	m_gameEvent.Init( pEvent );

	// gather the pre-collision data that the game needs to track
	for ( int i = 0; i < 2; i++ )
	{
		IPhysicsObject *pObject = pEvent->pObjects[i];
		if ( pObject )
		{
			if ( pObject->GetGameFlags() & FVPHYSICS_PLAYER_HELD )
			{
				CBaseEntity *pOtherEntity = reinterpret_cast<CBaseEntity *>(pEvent->pObjects[!i]->GetGameData());
				if ( pOtherEntity && !pOtherEntity->IsPlayer() )
				{
					Vector velocity;
					AngularImpulse angVel;
					// HACKHACK: If we totally clear this out, then Havok will think the objects
					// are penetrating and generate forces to separate them
					// so make it fairly small and have a tiny collision instead.
					pObject->GetVelocity( &velocity, &angVel );
					float len = VectorNormalize(velocity);
					len = max( len, 10 );
					velocity *= len;
					len = VectorNormalize(angVel);
					len = max( len, 1 );
					angVel *= len;
					pObject->SetVelocity( &velocity, &angVel );
				}
			}
			pObject->GetVelocity( &m_gameEvent.preVelocity[i], &m_gameEvent.preAngularVelocity[i] );
		}
	}
}

void CCollisionEvent::PostCollision( vcollisionevent_t *pEvent )
{
	CallbackContext check(this);
	bool isShadow[2] = {false,false};
	int i;

	for ( i = 0; i < 2; i++ )
	{
		IPhysicsObject *pObject = pEvent->pObjects[i];
		if ( pObject )
		{
			CBaseEntity *pEntity = reinterpret_cast<CBaseEntity *>(pObject->GetGameData());
			if ( !pEntity )
				return;

			// UNDONE: This is here to trap crashes due to NULLing out the game data on delete
			m_gameEvent.pEntities[i] = pEntity;
			unsigned int flags = pObject->GetCallbackFlags();
			pObject->GetVelocity( &m_gameEvent.postVelocity[i], NULL );
			if ( flags & CALLBACK_SHADOW_COLLISION )
			{
				isShadow[i] = true;
			}

			// Shouldn't get impacts with triggers
			Assert( !pObject->IsTrigger() );
		}
	}

	// copy off the post-collision variable data
	m_gameEvent.collisionSpeed = pEvent->collisionSpeed;
	m_gameEvent.pInternalData = pEvent->pInternalData;

	// special case for hitting self, only make one non-shadow call
	if ( m_gameEvent.pEntities[0] == m_gameEvent.pEntities[1] )
	{
		if ( pEvent->isCollision && m_gameEvent.pEntities[0] )
		{
			m_gameEvent.pEntities[0]->VPhysicsCollision( 0, &m_gameEvent );
		}
		return;
	}

	if ( isShadow[0] && isShadow[1] )
	{
		pEvent->isCollision = false;
	}

	for ( i = 0; i < 2; i++ )
	{
		if ( pEvent->isCollision )
		{
			m_gameEvent.pEntities[i]->VPhysicsCollision( i, &m_gameEvent );
		}
		if ( pEvent->isShadowCollision && isShadow[i] )
		{
			m_gameEvent.pEntities[i]->VPhysicsShadowCollision( i, &m_gameEvent );
		}
	}
}

void PhysForceEntityToSleep( CBaseEntity *pEntity, IPhysicsObject *pObject )
{
	// UNDONE: Check to see if the object is touching the player first?
	// Might get the player stuck?
	if ( !pObject || !pObject->IsMoveable() )
		return;

	DevMsg(2, "Putting entity to sleep: %s\n", pEntity->GetClassname() );
	IPhysicsObject *pList[VPHYSICS_MAX_OBJECT_LIST_COUNT];
	int physCount = pEntity->VPhysicsGetObjectList( pList, ARRAYSIZE(pList) );
	for ( int i = 0; i < physCount; i++ )
	{
		PhysForceClearVelocity( pList[i] );
		pList[i]->Sleep();
	}
}

void CCollisionEvent::Friction( IPhysicsObject *pObject, float energy, int surfaceProps, int surfacePropsHit, IPhysicsCollisionData *pData )
{
	CallbackContext check(this);
	//Get our friction information
	Vector vecPos, vecVel;
	pData->GetContactPoint( vecPos );
	pObject->GetVelocityAtPoint( vecPos, vecVel );

	CBaseEntity *pEntity = reinterpret_cast<CBaseEntity *>(pObject->GetGameData());
		
	if ( pEntity  )
	{
		friction_t *pFriction = g_Collisions.FindFriction( pEntity );

		if ( pFriction && pFriction->pObject) 
		{
			// in MP mode play sound and effects once every 500 msecs,
			// no ongoing updates, takes too much bandwidth
			if ( (pFriction->flLastEffectTime + 0.5f) > gpGlobals->curtime)
			{
				pFriction->flLastUpdateTime = gpGlobals->curtime;
				return; 			
			}
		}

		pEntity->VPhysicsFriction( pObject, energy, surfaceProps, surfacePropsHit );
	}

	PhysFrictionEffect( vecPos, vecVel, energy, surfaceProps, surfacePropsHit );
}


friction_t *CCollisionEvent::FindFriction( CBaseEntity *pObject )
{
	friction_t *pFree = NULL;

	for ( int i = 0; i < ARRAYSIZE(m_current); i++ )
	{
		if ( !m_current[i].pObject && !pFree )
			pFree = &m_current[i];

		if ( m_current[i].pObject == pObject )
			return &m_current[i];
	}

	return pFree;
}

void CCollisionEvent::ShutdownFriction( friction_t &friction )
{
//	Msg( "Scrape Stop %s \n", STRING(friction.pObject->m_iClassname) );
	CSoundEnvelopeController::GetController().SoundDestroy( friction.patch );
	friction.patch = NULL;
	friction.pObject = NULL;
}

void CCollisionEvent::PostSimulationFrame()
{
	UpdateImpulseEvents();
	UpdateSetVelocityEvents();
	UpdateDamageEvents();
}

void CCollisionEvent::FrameUpdate( void )
{
	UpdateFrictionSounds();
	UpdateTouchEvents();
	UpdatePenetrateEvents();
	UpdateFluidEvents();
	UpdateImpulseEvents();
	UpdateSetVelocityEvents();
	UpdateDamageEvents(); // if there was no PSI in physics, we'll still need to do some of these because collisions are solved in between PSIs
}

void CCollisionEvent::UpdateFluidEvents( void )
{
	for ( int i = m_fluidEvents.Count()-1; i >= 0; --i )
	{
		if ( (gpGlobals->curtime - m_fluidEvents[i].impactTime) > FLUID_TIME_MAX )
		{
			m_fluidEvents.FastRemove(i);
		}
	}
}


float CCollisionEvent::DeltaTimeSinceLastFluid( CBaseEntity *pEntity )
{
	for ( int i = m_fluidEvents.Count()-1; i >= 0; --i )
	{
		if ( m_fluidEvents[i].pEntity == pEntity )
		{
			return gpGlobals->curtime - m_fluidEvents[i].impactTime;
		}
	}

	int index = m_fluidEvents.AddToTail();
	m_fluidEvents[index].pEntity = pEntity;
	m_fluidEvents[index].impactTime = gpGlobals->curtime;
	return FLUID_TIME_MAX;
}

void CCollisionEvent::UpdateFrictionSounds( void )
{
	for ( int i = 0; i < ARRAYSIZE(m_current); i++ )
	{
		if ( m_current[i].patch )
		{
			if ( m_current[i].flLastUpdateTime < (gpGlobals->curtime-0.1f) )
			{
				// friction wasn't updated the last 100msec, assume fiction finished
				ShutdownFriction( m_current[i] );
			}
		}
	}
}


void CCollisionEvent::DispatchStartTouch( CBaseEntity *pEntity0, CBaseEntity *pEntity1, const Vector &point, const Vector &normal )
{
	trace_t trace;
	memset( &trace, 0, sizeof(trace) );
	trace.endpos = point;
	trace.plane.dist = DotProduct( point, normal );
	trace.plane.normal = normal;

	// NOTE: This sets up the touch list for both entities, no call to pEntity1 is needed
	pEntity0->PhysicsMarkEntitiesAsTouchingEventDriven( pEntity1, trace );
}

void CCollisionEvent::DispatchEndTouch( CBaseEntity *pEntity0, CBaseEntity *pEntity1 )
{
	// frees the event-driven touchlinks
	pEntity0->PhysicsNotifyOtherOfUntouch( pEntity0, pEntity1 );
	pEntity1->PhysicsNotifyOtherOfUntouch( pEntity1, pEntity0 );
}

void CCollisionEvent::UpdateTouchEvents( void )
{
	for ( int i = 0; i < m_touchEvents.Count(); i++ )
	{
		const touchevent_t &event = m_touchEvents[i];
		if ( event.touchType == TOUCH_START )
		{
			DispatchStartTouch( event.pEntity0, event.pEntity1, event.endPoint, event.normal );
		}
		else
		{
			// TOUCH_END
			DispatchEndTouch( event.pEntity0, event.pEntity1 );
		}
	}
	m_touchEvents.RemoveAll();
}

void CCollisionEvent::UpdateDamageEvents( void )
{
	for ( int i = 0; i < m_damageEvents.Count(); i++ )
	{
		damageevent_t &event = m_damageEvents[i];

		// Track changes in the entity's life state
		int iEntBits = event.pEntity->IsAlive() ? 0x0001 : 0;
		iEntBits |= event.pEntity->IsMarkedForDeletion() ? 0x0002 : 0;
		iEntBits |= (event.pEntity->GetSolidFlags() & FSOLID_NOT_SOLID) ? 0x0004 : 0;
#if 0
		// Go ahead and compute the current static stress when hit by a large object (with a force high enough to do damage).  
		// That way you die from the impact rather than the stress of the object resting on you whenever possible. 
		// This makes the damage effects cleaner.
		if ( event.pInflictorPhysics && event.pInflictorPhysics->GetMass() > VPHYSICS_LARGE_OBJECT_MASS )
		{
			CBaseCombatCharacter *pCombat = event.pEntity->MyCombatCharacterPointer();
			if ( pCombat )
			{
				vphysics_objectstress_t stressOut;
				event.info.AddDamage( pCombat->CalculatePhysicsStressDamage( &stressOut, pCombat->VPhysicsGetObject() ) );
			}
		}
#endif

		event.pEntity->TakeDamage( event.info );
		int iEntBits2 = event.pEntity->IsAlive() ? 0x0001 : 0;
		iEntBits2 |= event.pEntity->IsMarkedForDeletion() ? 0x0002 : 0;
		iEntBits2 |= (event.pEntity->GetSolidFlags() & FSOLID_NOT_SOLID) ? 0x0004 : 0;

		if ( event.bRestoreVelocity && iEntBits != iEntBits2 )
		{
			// UNDONE: Use ratio of masses to blend in a little of the collision response?
			// UNDONE: Damage for future events is already computed - it would be nice to
			//			go back and recompute it now that the values have
			//			been adjusted
			RestoreDamageInflictorState( event.pInflictorPhysics );
		}
	}
	m_damageEvents.RemoveAll();
	m_damageInflictors.RemoveAll();
}

void CCollisionEvent::RestoreDamageInflictorState( int inflictorStateIndex, float velocityBlend )
{
	inflictorstate_t &state = m_damageInflictors[inflictorStateIndex];
	if ( state.restored )
		return;

	// so we only restore this guy once
	state.restored = true;

	if ( velocityBlend > 0 )
	{
		Vector velocity;
		AngularImpulse angVel;
		state.pInflictorPhysics->GetVelocity( &velocity, &angVel );
		state.savedVelocity = state.savedVelocity*velocityBlend + velocity*(1-velocityBlend);
		state.savedAngularVelocity = state.savedAngularVelocity*velocityBlend + angVel*(1-velocityBlend);
		state.pInflictorPhysics->SetVelocity( &state.savedVelocity, &state.savedAngularVelocity );
	}

	if ( state.nextIndex >= 0 )
	{
		RestoreDamageInflictorState( state.nextIndex, velocityBlend );
	}
}

void CCollisionEvent::RestoreDamageInflictorState( IPhysicsObject *pInflictor )
{
	if ( !pInflictor )
		return;

	int index = FindDamageInflictor( pInflictor );
	if ( index >= 0 )
	{
		inflictorstate_t &state = m_damageInflictors[index];
		if ( !state.restored )
		{
			float velocityBlend = 1.0;
			float inflictorMass = state.pInflictorPhysics->GetMass();
			if ( inflictorMass < VPHYSICS_LARGE_OBJECT_MASS && !(state.pInflictorPhysics->GetGameFlags() & FVPHYSICS_DMG_SLICE) )
			{
				float otherMass = state.otherMassMax > 0 ? state.otherMassMax : 1;
				float massRatio = inflictorMass / otherMass;
				massRatio = clamp( massRatio, 0.1f, 10.0f );
				if ( massRatio < 1 )
				{
					velocityBlend = RemapVal( massRatio, 0.1, 1, 0, 0.5 );
				}
				else
				{
					velocityBlend = RemapVal( massRatio, 1.0, 10, 0.5, 1 );
				}
			}
			RestoreDamageInflictorState( index, velocityBlend );
		}
	}
}

bool CCollisionEvent::GetInflictorVelocity( IPhysicsObject *pInflictor, Vector &velocity, AngularImpulse &angVelocity )
{
	int index = FindDamageInflictor( pInflictor );
	if ( index >= 0 )
	{
		inflictorstate_t &state = m_damageInflictors[index];
		velocity = state.savedVelocity;
		angVelocity = state.savedAngularVelocity;
		return true;
	}

	return false;
}

bool PhysGetDamageInflictorVelocityStartOfFrame( IPhysicsObject *pInflictor, Vector &velocity, AngularImpulse &angVelocity )
{
	return g_Collisions.GetInflictorVelocity( pInflictor, velocity, angVelocity );
}

void CCollisionEvent::AddTouchEvent( CBaseEntity *pEntity0, CBaseEntity *pEntity1, int touchType, const Vector &point, const Vector &normal )
{
	if ( !pEntity0 || !pEntity1 )
		return;

	for ( int i = m_touchEvents.Count()-1; i >= 0; --i )
	{
		// event already in list, skip
		if ( m_touchEvents[i].pEntity0 == pEntity0 && 
			m_touchEvents[i].pEntity1 == pEntity1 && 
			m_touchEvents[i].touchType == touchType )
			return;
	}
	int index = m_touchEvents.AddToTail();
	touchevent_t &event = m_touchEvents[index];
	event.pEntity0 = pEntity0;
	event.pEntity1 = pEntity1;
	event.touchType = touchType;
	event.endPoint = point;
	event.normal = normal;
}

void CCollisionEvent::AddDamageEvent( CBaseEntity *pEntity, const CTakeDamageInfo &info, IPhysicsObject *pInflictorPhysics, bool bRestoreVelocity, const Vector &savedVel, const AngularImpulse &savedAngVel )
{
	if ( pEntity->IsMarkedForDeletion() )
		return;

	if ( !( info.GetDamageType() & (DMG_BURN | DMG_DROWN | DMG_TIMEBASED | DMG_PREVENT_PHYSICS_FORCE) ) )
	{
		Assert( info.GetDamageForce() != vec3_origin && info.GetDamagePosition() != vec3_origin );
	}

	int index = m_damageEvents.AddToTail();
	damageevent_t &event = m_damageEvents[index];
	event.pEntity = pEntity;
	event.info = info;
	event.pInflictorPhysics = pInflictorPhysics;
	event.bRestoreVelocity = bRestoreVelocity;
	if ( !pInflictorPhysics || !pInflictorPhysics->IsMoveable() )
	{
		event.bRestoreVelocity = false;
	}

	if ( event.bRestoreVelocity )
	{
		float otherMass = pEntity->VPhysicsGetObject()->GetMass();
		int inflictorIndex = FindDamageInflictor(pInflictorPhysics);
		if ( inflictorIndex >= 0 )
		{
			// if this is a bigger mass, save that info
			inflictorstate_t &state = m_damageInflictors[inflictorIndex];
			if ( otherMass > state.otherMassMax )
			{
				state.otherMassMax = otherMass;
			}

		}
		else
		{
			AddDamageInflictor( pInflictorPhysics, otherMass, savedVel, savedAngVel, true );
		}
	}

}

void CCollisionEvent::AddImpulseEvent( IPhysicsObject *pPhysicsObject, const Vector &vecCenterForce, const AngularImpulse &vecCenterTorque )
{
	int index = m_impulseEvents.AddToTail();
	impulseevent_t &event = m_impulseEvents[index];
	event.pObject = pPhysicsObject;
	event.vecCenterForce = vecCenterForce;
	event.vecCenterTorque = vecCenterTorque;
}

void CCollisionEvent::AddSetVelocityEvent( IPhysicsObject *pPhysicsObject, const Vector &vecVelocity )
{
	int index = m_setVelocityEvents.AddToTail();
	velocityevent_t &event = m_setVelocityEvents[index];
	event.pObject = pPhysicsObject;
	event.vecVelocity = vecVelocity;
}

int CCollisionEvent::FindDamageInflictor( IPhysicsObject *pInflictorPhysics )
{
	// UNDONE: Linear search?  Probably ok with a low count here
	for ( int i = m_damageInflictors.Count()-1; i >= 0; --i )
	{
		const inflictorstate_t &state = m_damageInflictors[i];
		if ( state.pInflictorPhysics == pInflictorPhysics )
			return i;
	}

	return -1;
}


int CCollisionEvent::AddDamageInflictor( IPhysicsObject *pInflictorPhysics, float otherMass, const Vector &savedVel, const AngularImpulse &savedAngVel, bool addList )
{
	// NOTE: Save off the state of the object before collision
	// restore if the impact is a kill
	// UNDONE: Should we absorb some energy here?
	// NOTE: we can't save a delta because there could be subsequent post-fatal collisions

	int addIndex = m_damageInflictors.AddToTail();
	{
		inflictorstate_t &state = m_damageInflictors[addIndex];
		state.pInflictorPhysics = pInflictorPhysics;
		state.savedVelocity = savedVel;
		state.savedAngularVelocity = savedAngVel;
		state.otherMassMax = otherMass;
		state.restored = false;
		state.nextIndex = -1;
	}

	if ( addList )
	{
		CBaseEntity *pEntity = static_cast<CBaseEntity *>(pInflictorPhysics->GetGameData());
		if ( pEntity )
		{
			IPhysicsObject *pList[VPHYSICS_MAX_OBJECT_LIST_COUNT];
			int physCount = pEntity->VPhysicsGetObjectList( pList, ARRAYSIZE(pList) );
			if ( physCount > 1 )
			{
				int currentIndex = addIndex;
				for ( int i = 0; i < physCount; i++ )
				{
					if ( pList[i] != pInflictorPhysics )
					{
						Vector vel;
						AngularImpulse angVel;
						pList[i]->GetVelocity( &vel, &angVel );
						int next = AddDamageInflictor( pList[i], otherMass, vel, angVel, false );
						m_damageInflictors[currentIndex].nextIndex = next;
						currentIndex = next;
					}
				}
			}
		}
	}
	return addIndex;
}


void CCollisionEvent::LevelShutdown( void )
{
	for ( int i = 0; i < ARRAYSIZE(m_current); i++ )
	{
		if ( m_current[i].patch )
		{
			ShutdownFriction( m_current[i] );
		}
	}
}


void CCollisionEvent::StartTouch( IPhysicsObject *pObject1, IPhysicsObject *pObject2, IPhysicsCollisionData *pTouchData )
{
	CallbackContext check(this);
	CBaseEntity *pEntity1 = static_cast<CBaseEntity *>(pObject1->GetGameData());
	CBaseEntity *pEntity2 = static_cast<CBaseEntity *>(pObject2->GetGameData());

	if ( !pEntity1 || !pEntity2 )
		return;

	Vector endPoint, normal;
	pTouchData->GetContactPoint( endPoint );
	pTouchData->GetSurfaceNormal( normal );
	if ( !m_bBufferTouchEvents )
	{
		DispatchStartTouch( pEntity1, pEntity2, endPoint, normal );
	}
	else
	{
		AddTouchEvent( pEntity1, pEntity2, TOUCH_START, endPoint, normal );
	}
}

static int CountPhysicsObjectEntityContacts( IPhysicsObject *pObject, CBaseEntity *pEntity )
{
	IPhysicsFrictionSnapshot *pSnapshot = pObject->CreateFrictionSnapshot();
	int count = 0;
	while ( pSnapshot->IsValid() )
	{
		IPhysicsObject *pOther = pSnapshot->GetObject(1);
		CBaseEntity *pOtherEntity = static_cast<CBaseEntity *>(pOther->GetGameData());
		if ( pOtherEntity == pEntity )
			count++;
		pSnapshot->NextFrictionData();
	}
	pObject->DestroyFrictionSnapshot( pSnapshot );
	return count;
}

void CCollisionEvent::EndTouch( IPhysicsObject *pObject1, IPhysicsObject *pObject2, IPhysicsCollisionData *pTouchData )
{
	CallbackContext check(this);
	CBaseEntity *pEntity1 = static_cast<CBaseEntity *>(pObject1->GetGameData());
	CBaseEntity *pEntity2 = static_cast<CBaseEntity *>(pObject2->GetGameData());

	if ( !pEntity1 || !pEntity2 )
		return;

	// contact point deleted, but entities are still touching?
	IPhysicsObject *list[VPHYSICS_MAX_OBJECT_LIST_COUNT];
	int count = pEntity1->VPhysicsGetObjectList( list, ARRAYSIZE(list) );

	int contactCount = 0;
	for ( int i = 0; i < count; i++ )
	{
		contactCount += CountPhysicsObjectEntityContacts( list[i], pEntity2 );
		
		// still touching
		if ( contactCount > 1 )
			return;
	}

	// should have exactly one contact point (the one getting deleted here)
	//Assert( contactCount == 1 );

	Vector endPoint, normal;
	pTouchData->GetContactPoint( endPoint );
	pTouchData->GetSurfaceNormal( normal );

	if ( !m_bBufferTouchEvents )
	{
		DispatchEndTouch( pEntity1, pEntity2 );
	}
	else
	{
		AddTouchEvent( pEntity1, pEntity2, TOUCH_END, vec3_origin, vec3_origin );
	}
}


// UNDONE: This is functional, but minimally.
void CCollisionEvent::ObjectEnterTrigger( IPhysicsObject *pTrigger, IPhysicsObject *pObject )
{
	CallbackContext check(this);
	CBaseEntity *pTriggerEntity = static_cast<CBaseEntity *>(pTrigger->GetGameData());
	CBaseEntity *pEntity = static_cast<CBaseEntity *>(pObject->GetGameData());
	if ( pTriggerEntity && pEntity )
	{
		m_triggerEvent.Init( pTriggerEntity, pTrigger, pObject ); 
		pTriggerEntity->StartTouch( pEntity );
		m_triggerEvent.Clear();
	}
}

void CCollisionEvent::ObjectLeaveTrigger( IPhysicsObject *pTrigger, IPhysicsObject *pObject )
{
	CallbackContext check(this);
	CBaseEntity *pTriggerEntity = static_cast<CBaseEntity *>(pTrigger->GetGameData());
	CBaseEntity *pEntity = static_cast<CBaseEntity *>(pObject->GetGameData());
	if ( pTriggerEntity && pEntity )
	{
		m_triggerEvent.Init( pTriggerEntity, pTrigger, pObject ); 
		pTriggerEntity->EndTouch( pEntity );
		m_triggerEvent.Clear();
	}
}

void CCollisionEvent::GetTriggerEvent( triggerevent_t *pEvent, CBaseEntity *pTriggerEntity )
{
	if ( pEvent && pTriggerEntity == m_triggerEvent.pTriggerEntity )
	{
		*pEvent = m_triggerEvent;
	}
}

void PhysGetListOfPenetratingEntities( CBaseEntity *pSearch, CUtlVector<CBaseEntity *> &list )
{
	g_Collisions.GetListOfPenetratingEntities( pSearch, list );
}

void PhysGetTriggerEvent( triggerevent_t *pEvent, CBaseEntity *pTriggerEntity )
{
	g_Collisions.GetTriggerEvent( pEvent, pTriggerEntity );
}
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// External interface to collision sounds
//-----------------------------------------------------------------------------

void PhysicsImpactSound( CBaseEntity *pEntity, IPhysicsObject *pPhysObject, int channel, int surfaceProps, int surfacePropsHit, float volume, float impactSpeed )
{
	physicssound::AddImpactSound( g_PhysicsHook.m_impactSounds, pEntity, pEntity->entindex(), channel, pPhysObject, surfaceProps, surfacePropsHit, volume, impactSpeed );
}

void PhysCollisionSound( CBaseEntity *pEntity, IPhysicsObject *pPhysObject, int channel, int surfaceProps, int surfacePropsHit, float deltaTime, float speed )
{
	if ( deltaTime < 0.05f || speed < 70.0f )
		return;

	float volume = speed * speed * (1.0f/(320.0f*320.0f));	// max volume at 320 in/s
	if ( volume > 1.0f )
		volume = 1.0f;

	PhysicsImpactSound( pEntity, pPhysObject, channel, surfaceProps, surfacePropsHit, volume, speed );
}

void PhysBreakSound( CBaseEntity *pEntity, IPhysicsObject *pPhysObject, Vector vecOrigin )
{
	if ( !pPhysObject)
		return;

	surfacedata_t *psurf = physprops->GetSurfaceData( pPhysObject->GetMaterialIndex() );
	if ( !psurf->sounds.breakSound )
		return;

	const char *pSound = physprops->GetString( psurf->sounds.breakSound );
	CSoundParameters params;
	if ( !CBaseEntity::GetParametersForSound( pSound, params, NULL ) )
		return;

	// Play from the world, because the entity is breaking, so it'll be destroyed soon
	CPASAttenuationFilter filter( vecOrigin, params.soundlevel );
	EmitSound_t ep;
	ep.m_nChannel = CHAN_STATIC;
	ep.m_pSoundName = params.soundname;
	ep.m_flVolume = params.volume;
	ep.m_SoundLevel = params.soundlevel;
	ep.m_nPitch = params.pitch;
	ep.m_pOrigin = &vecOrigin;
	CBaseEntity::EmitSound( filter, 0 /*sound.entityIndex*/, ep );
}

ConVar collision_shake_amp("collision_shake_amp", "0.2");
ConVar collision_shake_freq("collision_shake_freq", "0.5");
ConVar collision_shake_time("collision_shake_time", "0.5");
				 
void PhysCollisionScreenShake( gamevcollisionevent_t *pEvent, int index )
{
	int otherIndex = !index;
	float mass = pEvent->pObjects[index]->GetMass();
	if ( mass >= VPHYSICS_LARGE_OBJECT_MASS && pEvent->pObjects[otherIndex]->IsStatic() && 
		!(pEvent->pObjects[index]->GetGameFlags() & FVPHYSICS_PENETRATING) )
	{
		mass = clamp(mass, VPHYSICS_LARGE_OBJECT_MASS, 2000);
		if ( pEvent->collisionSpeed > 30 && pEvent->deltaCollisionTime > 0.25f )
		{
			Vector vecPos;
			pEvent->pInternalData->GetContactPoint( vecPos );
			float impulse = pEvent->collisionSpeed * mass;
			float amplitude = impulse * (collision_shake_amp.GetFloat() / (30.0f * VPHYSICS_LARGE_OBJECT_MASS));
			UTIL_ScreenShake( vecPos, amplitude, collision_shake_freq.GetFloat(), collision_shake_time.GetFloat(), amplitude * 60, SHAKE_START );
		}
	}
}

void PhysCollisionDust( gamevcollisionevent_t *pEvent, surfacedata_t *phit )
{

	switch ( phit->game.material )
	{
	case CHAR_TEX_SAND:
	case CHAR_TEX_DIRT:

		if ( pEvent->collisionSpeed < 200.0f )
			return;
		
		break;

	case CHAR_TEX_CONCRETE:

		if ( pEvent->collisionSpeed < 340.0f )
			return;

		break;

	default:
		return;
	}

	//Kick up dust
	Vector	vecPos, vecVel;

	pEvent->pInternalData->GetContactPoint( vecPos );

	vecVel.Random( -1.0f, 1.0f );
	vecVel.z = random->RandomFloat( 0.3f, 1.0f );
	VectorNormalize( vecVel );
	g_pEffects->Dust( vecPos, vecVel, 8.0f, pEvent->collisionSpeed );
}

void PhysFrictionSound( CBaseEntity *pEntity, IPhysicsObject *pObject, const char *pSoundName, float flVolume )
{
	if ( !pEntity )
		return;
	
	// cut out the quiet sounds
	// UNDONE: Separate threshold for starting a sound vs. continuing?
	flVolume = clamp( flVolume, 0.0f, 1.0f );
	if ( flVolume > (1.0f/128.0f) )
	{
		friction_t *pFriction = g_Collisions.FindFriction( pEntity );
		if ( !pFriction )
			return;

		CSoundParameters params;
		if ( !CBaseEntity::GetParametersForSound( pSoundName, params, NULL ) )
			return;

		if ( !pFriction->pObject )
		{
			// don't create really quiet scrapes
			if ( params.volume * flVolume <= 0.1f )
				return;

			pFriction->pObject = pEntity;
			CPASAttenuationFilter filter( pEntity, params.soundlevel );
			pFriction->patch = CSoundEnvelopeController::GetController().SoundCreate( 
				filter, pEntity->entindex(), CHAN_BODY, pSoundName, params.soundlevel );
			CSoundEnvelopeController::GetController().Play( pFriction->patch, params.volume * flVolume, params.pitch );
		}
		else
		{
			float pitch = (flVolume * (params.pitchhigh - params.pitchlow)) + params.pitchlow;
			CSoundEnvelopeController::GetController().SoundChangeVolume( pFriction->patch, params.volume * flVolume, 0.1f );
			CSoundEnvelopeController::GetController().SoundChangePitch( pFriction->patch, pitch, 0.1f );
		}

		pFriction->flLastUpdateTime = gpGlobals->curtime;
		pFriction->flLastEffectTime = gpGlobals->curtime;
	}
}

void PhysCleanupFrictionSounds( CBaseEntity *pEntity )
{
	friction_t *pFriction = g_Collisions.FindFriction( pEntity );
	if ( pFriction && pFriction->patch )
	{
		g_Collisions.ShutdownFriction( *pFriction );
	}
}


//-----------------------------------------------------------------------------
// Applies force impulses at a later time
//-----------------------------------------------------------------------------
void PhysCallbackImpulse( IPhysicsObject *pPhysicsObject, const Vector &vecCenterForce, const AngularImpulse &vecCenterTorque )
{
	Assert( physenv->IsInSimulation() );
	g_Collisions.AddImpulseEvent( pPhysicsObject, vecCenterForce, vecCenterTorque );
}

void PhysCallbackSetVelocity( IPhysicsObject *pPhysicsObject, const Vector &vecVelocity )
{
	Assert( physenv->IsInSimulation() );
	g_Collisions.AddSetVelocityEvent( pPhysicsObject, vecVelocity );
}

void PhysCallbackDamage( CBaseEntity *pEntity, const CTakeDamageInfo &info, gamevcollisionevent_t &event, int hurtIndex )
{
	Assert( physenv->IsInSimulation() );
	int otherIndex = !hurtIndex;
	g_Collisions.AddDamageEvent( pEntity, info, event.pObjects[otherIndex], true, event.preVelocity[otherIndex], event.preAngularVelocity[otherIndex] );
}

void PhysCallbackDamage( CBaseEntity *pEntity, const CTakeDamageInfo &info )
{
	if ( PhysIsInCallback() )
	{
		CBaseEntity *pInflictor = info.GetInflictor();
		IPhysicsObject *pInflictorPhysics = (pInflictor) ? pInflictor->VPhysicsGetObject() : NULL;
		g_Collisions.AddDamageEvent( pEntity, info, pInflictorPhysics, false, vec3_origin, vec3_origin );
		if ( pEntity && info.GetInflictor() )
		{
			DevMsg( 2, "Warning: Physics damage event with no recovery info!\nObjects: %s, %s\n", pEntity->GetClassname(), info.GetInflictor()->GetClassname() );
		}
	}
	else
	{
		pEntity->TakeDamage( info );
	}
}


IPhysicsObject *FindPhysicsObjectByName( const char *pName )
{
	if ( !pName || !strlen(pName) )
		return NULL;

	CBaseEntity *pEntity = NULL;
	IPhysicsObject *pBestObject = NULL;
	while (1)
	{
		pEntity = gEntList.FindEntityByName( pEntity, pName, NULL );
		if ( !pEntity )
			break;
		if ( pEntity->VPhysicsGetObject() )
		{
			if ( pBestObject )
			{
				DevWarning("Physics entity/constraint attached to more than one entity with the name %s!!!\n", pName );
				while ( ( pEntity = gEntList.FindEntityByName( pEntity, pName, NULL ) ) != NULL )
				{
					DevWarning("Found %s\n", pEntity->GetClassname() );
				}
				break;

			}
			pBestObject = pEntity->VPhysicsGetObject();
		}
	}
	return pBestObject;
}

void CC_AirDensity( void )
{
	if ( !physenv )
		return;

	if ( engine->Cmd_Argc() < 2 )
	{
		Msg( "air_density <value>\nCurrent air density is %.2f\n", physenv->GetAirDensity() );
	}
	else
	{
		float density = atof(engine->Cmd_Argv(1));
		physenv->SetAirDensity( density );
	}
}
static ConCommand air_density("air_density", CC_AirDensity, "Changes the density of air for drag computations.", FCVAR_CHEAT);


#if 0

#include "filesystem.h"
//-----------------------------------------------------------------------------
// Purpose: This will append a collide to a glview file.  Then you can view the 
//			collisionmodels with glview.
// Input  : *pCollide - collision model
//			&origin - position of the instance of this model
//			&angles - orientation of instance
//			*pFilename - output text file
//-----------------------------------------------------------------------------
// examples:
// world:
//	DumpCollideToGlView( pWorldCollide->solids[0], vec3_origin, vec3_origin, "jaycollide.txt" );
// static_prop:
//	DumpCollideToGlView( info.m_pCollide->solids[0], info.m_Origin, info.m_Angles, "jaycollide.txt" );
//
//-----------------------------------------------------------------------------
void DumpCollideToGlView( CPhysCollide *pCollide, const Vector &origin, const QAngle &angles, const char *pFilename )
{
	if ( !pCollide )
		return;

	printf("Writing %s...\n", pFilename );
	Vector *outVerts;
	int vertCount = physcollision->CreateDebugMesh( pCollide, &outVerts );
	FileHandle_t fp = filesystem->Open( pFilename, "ab" );
	int triCount = vertCount / 3;
	int vert = 0;
	VMatrix tmp = SetupMatrixOrgAngles( origin, angles );
	int i;
	for ( i = 0; i < vertCount; i++ )
	{
		outVerts[i] = tmp.VMul4x3( outVerts[i] );
	}
	for ( i = 0; i < triCount; i++ )
	{
		filesystem->FPrintf( fp, "3\n" );
		filesystem->FPrintf( fp, "%6.3f %6.3f %6.3f 1 0 0\n", outVerts[vert].x, outVerts[vert].y, outVerts[vert].z );
		vert++;
		filesystem->FPrintf( fp, "%6.3f %6.3f %6.3f 0 1 0\n", outVerts[vert].x, outVerts[vert].y, outVerts[vert].z );
		vert++;
		filesystem->FPrintf( fp, "%6.3f %6.3f %6.3f 0 0 1\n", outVerts[vert].x, outVerts[vert].y, outVerts[vert].z );
		vert++;
	}
	filesystem->Close( fp );
	physcollision->DestroyDebugMesh( vertCount, outVerts );
}
#endif

