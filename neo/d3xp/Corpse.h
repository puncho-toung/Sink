#ifndef __GAME_CORPSE_H__
#define __GAME_CORPSE_H__

// A static server pose; the live AF solver and AI never survive the swap.
class idCorpse : public idAFEntity_Base
{
public:
	CLASS_PROTOTYPE( idCorpse );
	idCorpse();
	~idCorpse();
	void Spawn();
	void Save( idSaveGame* save ) const;
	void Restore( idRestoreGame* save );
	virtual void WriteToSnapshot( idBitMsg& msg ) const;
	virtual void ReadFromSnapshot( const idBitMsg& msg );
	bool Capture( idAI* actor );
	static bool WriteActorPose( idAI* actor, idFile_Memory& pose );
	bool Build( const idDeclEntityDef* source, idFile_Memory& pose );
	void WritePose( idFile* out ) const;
	const char* SourceName() const
	{
		return sourceName.c_str();
	}
	int64 DeathTime() const
	{
		return deathTime;
	}

private:
	void FreeHead();
	idStr sourceName;
	idList<byte, TAG_GAME> poseBytes;
	int64 deathTime;
	idAnimator headAnimator;
	renderEntity_t headRender;
	qhandle_t headHandle;
};
extern idCVar g_corpses;
#endif
