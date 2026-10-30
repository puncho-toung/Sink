#ifndef __GAME_WORLDSTORELOCAL_H__
#define __GAME_WORLDSTORELOCAL_H__

//puncho - P2b.1.1a: the record codec helpers that WorldStore.cpp (commit, apply, save, restore) and WorldStoreRecord.cpp
// (codec, aspects, created entities, binds) both call; MapLandmarks.cpp needs none of them
void RecordString( idFile& f, const char* value );
bool RecordReadString( idFile& f, idStr& value );
void Record64( idFile& f, uint64 value );
bool RecordRead64( idFile& f, uint64& value );
bool ValidateAspectBytes( unsigned short aspects, const char* data, int length );
unsigned short WriteAspects( idEntity* ent, idFile_Memory& payload, const idVec3& landmark, bool motion, uint64 bindParent );
bool ApplyAspects( idEntity* ent, unsigned short aspects, idFile_Memory& payload, const idMat3& rotation, const idVec3& landmark );
idEntity* SpawnCorpseRecord( const char* def, unsigned short aspects, idFile_Memory& payload );
#endif
