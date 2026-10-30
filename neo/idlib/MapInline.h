#ifndef __MAPINLINE_H__
#define __MAPINLINE_H__

/*
===============================================================================

	The one rule that says dmap draws a func_static's or misc_model's model into the world (P2b.1.10b). dmap
	inlines the model into the areas when it holds (tools/compilers/dmap/usurface.cpp), idStaticEntity::Spawn then
	hides the entity, and MapStaticCanSkip adds its own tests before a spawn is skipped. world is the map's
	worldspawn, ent the entity's key/values: "inline" "1" on the entity, or "inlineAllStatics" "1" on the
	worldspawn for every static of a model format dmap reads that runs no GUI. Nothing else switches it: dmap's
	inlineAll option is refused, since the game would not see it and would draw the model a second time. reason
	names the failed test, or "inlined". idDict and idStr are supplied before inclusion.

===============================================================================
*/

ID_INLINE bool MapInlineEligible( const idDict& world, const idDict& ent, const char*& reason )
{
	const char* classname = ent.GetString( "classname" );
	if( idStr::Icmp( classname, "func_static" ) != 0 && idStr::Icmp( classname, "misc_model" ) != 0 )
	{
		reason = "not static";
		return false;
	}
	const bool inlineAll = world.GetBool( "inlineAllStatics" );
	if( !ent.GetBool( "inline" ) && !inlineAll )
	{
		reason = "not inlined";
		return false;
	}
	const idKeyValue* model = ent.FindKey( "model" );
	if( model == NULL || model->GetValue().IsEmpty() )
	{
		reason = "no model";
		return false;
	}
	if( inlineAll )
	{
		// the model formats dmap reads (the renderer's GLTF_EXT and GLTF_GLB_EXT, renderer/Model.h, are not visible
		// from idlib); the text after the last '.' matches one exactly when idStr::ExtractFileExtension's copy would
		static const char* const extensions[] = { "glb", "gltf", "lwo", "ase", "ma", "obj" };
		const int numExtensions = sizeof( extensions ) / sizeof( extensions[0] );
		const int dot = model->GetValue().Last( '.' );
		bool known = false;
		for( int i = 0; dot >= 0 && i < numExtensions; i++ )
		{
			known |= idStr::Icmp( model->GetValue().c_str() + dot + 1, extensions[i] ) == 0;
		}
		if( !known )
		{
			reason = "not inlined: unsupported extension";
			return false;
		}
		// an interactive GUI stays an entity
		if( ent.FindKey( "gui" ) != NULL || ent.FindKey( "gui2" ) != NULL || ent.FindKey( "gui3" ) != NULL )
		{
			reason = "not inlined: gui";
			return false;
		}
	}
	reason = "inlined";
	return true;
}

#endif /* !__MAPINLINE_H__ */
