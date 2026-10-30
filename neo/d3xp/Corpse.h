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
	//puncho start - P2b.1.2: the map record. A corpse writes idEntity's section and the AI's (AI_RECORD_SCHEMA, whose
	// RF_ bits it numbers in its own scope), and is the class that applies and checks the AI's: the store builds a
	// corpse for an entry holding Corpse
	enum
	{
		AI_RECORD_SCHEMA( SCHEMA_RECORD_INDEX, _ )
		CORPSE_RECORD_FIELDS
	};
	compile_time_assert( ( int )CORPSE_RECORD_FIELDS == ( int )idAI::AI_RECORD_FIELDS );
	virtual void			WriteRecord( idRecordOut& rec, const recordFrame_t& frame ) const;
	virtual recordApply_t	ApplyRecord( idRecordIn& rec, const recordFrame_t& frame );
	static bool				CheckRecordSection( idRecordIn& rec );
	bool					RecordCorpseChanged( const idRecordOut& rec ) const
	{
		return true;
	}
	void					WriteRecordCorpse( idRecordOut& rec ) const;
	bool					ApplyRecordCorpse( idRecordIn& rec );
	static bool				CheckRecordCorpse( idRecordIn& rec );
	// a pose as Build reads it (its leading origin first), written as the Corpse field: no origin, frame-relative
	static void				WriteRecordPayload( idRecordOut& rec, const byte* pose, int length );
	//puncho end

private:
	void FreeHead();
	void EnforceLiveCap();
	idLinkList<idCorpse> corpseNode; //puncho - P2.4 (bug review 2026-10-28): the live corpses in creation order, for the live cap (P2b.1.10c: while the corpse class is off)
	idStr sourceName;
	idList<byte, TAG_GAME> poseBytes;
	int64 deathTime;
	idAnimator headAnimator;
	renderEntity_t headRender;
	qhandle_t headHandle;
};
extern idCVar g_corpses;
#endif
