#!/usr/bin/env python3
"""parmsets.py: check that every render parm subset in renderer/RenderProgs.h lists its parms in the order the
matching shaders/renderParmSetN.inc.hlsl struct declares its members (01-VERIFY.md V0.6, GC9).

The C++ side is each `const idList<renderParm_t, ...> rp<Minimal|Nominal|Maximal>SetN = { RENDERPARM_..., ... };`.
The HLSL side is each `float4 rpName;` or `float4 rpName[count];` member of `struct renderParmSetN_t`, an array
expanding to one entry per float4. Names are compared after normalising both to lower case without underscores and
without the RENDERPARM_ / rp prefixes, with the few spellings that differ mapped explicitly (ALIASES and ARRAYS
below). Any set present on one side only, any length difference and the first position where the orders part are
reported. Exit 1 on any mismatch, 2 on a read error.

Usage: python3 -I parmsets.py [--progs neo/renderer/RenderProgs.h] [--shaders neo/shaders]
"""

import argparse
import glob
import os
import re
import sys

# C++ spelling (after normalising) -> HLSL spelling (after normalising), where the two differ
ALIASES = {
	"projmatrixx": "projectionmatrixx",
	"projmatrixy": "projectionmatrixy",
	"projmatrixz": "projectionmatrixz",
	"projmatrixw": "projectionmatrixw",
}

# HLSL array member (normalised) -> the normalised C++ name of element i
ARRAYS = {
	"shadowmatrices": lambda i: "shadowmatrix%d%s" % ( i // 4, "xyzw"[i % 4] ),
	"shadowatlasoffsets": lambda i: "shadowatlasoffset%d" % i,
}

def strip_comments( text ):
	text = re.sub( r"/\*.*?\*/", " ", text, flags = re.S )
	return re.sub( r"//[^\n]*", " ", text )

def norm( name ):
	return name.lower().replace( "_", "" )

def cpp_sets( path ):
	with open( path, encoding = "latin-1" ) as f:
		text = strip_comments( f.read() )
	sets = {}
	for m in re.finditer( r"idList\s*<\s*renderParm_t\s*,[^>]*>\s*(rp\w*?Set(\d+))\s*=\s*\{([^}]*)\}", text ):
		names = [ n.strip() for n in m.group( 3 ).split( "," ) if n.strip() ]
		entries = []
		for n in names:
			if not n.startswith( "RENDERPARM_" ):
				raise ValueError( "%s: %s holds '%s'" % ( path, m.group( 1 ), n ) )
			key = norm( n[len( "RENDERPARM_" ):] )
			entries.append( ( ALIASES.get( key, key ), n ) )
		sets[int( m.group( 2 ) )] = ( m.group( 1 ), entries )
	return sets

def eval_count( expr, where ):
	if not re.fullmatch( r"[0-9\s\*\+\-\(\)]+", expr ):
		raise ValueError( "%s: array size '%s' is not a plain integer expression" % ( where, expr ) )
	return int( eval( expr, { "__builtins__": {} } ) )

def hlsl_set( path, n ):
	with open( path, encoding = "latin-1" ) as f:
		text = strip_comments( f.read() )
	m = re.search( r"struct\s+renderParmSet%d_t\s*\{(.*?)\}\s*;" % n, text, flags = re.S )
	if m is None:
		raise ValueError( "%s: no struct renderParmSet%d_t" % ( path, n ) )
	entries = []
	for decl in m.group( 1 ).split( ";" ):
		decl = decl.strip()
		if not decl:
			continue
		d = re.fullmatch( r"(\w+)\s+(\w+)\s*(?:\[(.*)\])?", decl, flags = re.S )
		if d is None:
			raise ValueError( "%s: cannot read member '%s'" % ( path, decl ) )
		ctype, name, count = d.groups()
		if ctype != "float4":
			raise ValueError( "%s: member '%s' is %s, not float4" % ( path, name, ctype ) )
		key = norm( name[2:] if name.startswith( "rp" ) else name )
		if count is None:
			entries.append( ( key, name ) )
		else:
			k = eval_count( count, "%s %s" % ( path, name ) )
			element = ARRAYS.get( key, lambda i, key = key: "%s%d" % ( key, i ) )
			for i in range( k ):
				entries.append( ( element( i ), "%s[%d]" % ( name, i ) ) )
	return entries

def main():
	here = os.path.dirname( os.path.abspath( __file__ ) )
	neo = os.path.normpath( os.path.join( here, "..", ".." ) )
	p = argparse.ArgumentParser( description = __doc__.split( "\n" )[0] )
	p.add_argument( "--progs", default = os.path.join( neo, "renderer", "RenderProgs.h" ) )
	p.add_argument( "--shaders", default = os.path.join( neo, "shaders" ) )
	args = p.parse_args()

	try:
		cpp = cpp_sets( args.progs )
		hlsl_files = {}
		for path in glob.glob( os.path.join( args.shaders, "renderParmSet*.inc.hlsl" ) ):
			m = re.search( r"renderParmSet(\d+)\.inc\.hlsl$", path )
			if m:
				hlsl_files[int( m.group( 1 ) )] = path
		hlsl = { n: hlsl_set( path, n ) for n, path in hlsl_files.items() }
	except ( OSError, ValueError ) as e:
		print( "parmsets: %s" % e )
		return 2

	bad = 0
	for n in sorted( set( cpp ) | set( hlsl ) ):
		if n not in cpp:
			print( "set %d: %s has no C++ list" % ( n, hlsl_files[n] ) )
			bad += 1
			continue
		if n not in hlsl:
			print( "set %d: %s has no renderParmSet%d.inc.hlsl" % ( n, cpp[n][0], n ) )
			bad += 1
			continue
		listname, c = cpp[n]
		h = hlsl[n]
		mismatch = None
		for i in range( min( len( c ), len( h ) ) ):
			if c[i][0] != h[i][0]:
				mismatch = i
				break
		if mismatch is not None:
			print( "set %d: %s entry %d is %s, renderParmSet%d_t member %d is %s" % ( n, listname, mismatch, c[mismatch][1], n, mismatch, h[mismatch][1] ) )
			bad += 1
		elif len( c ) != len( h ):
			print( "set %d: %s has %d entries, renderParmSet%d_t has %d float4s" % ( n, listname, len( c ), n, len( h ) ) )
			bad += 1
		else:
			print( "set %d: %s matches renderParmSet%d_t, %d entries" % ( n, listname, n, len( c ) ) )

	print( "parmsets: %s" % ( "%d set(s) differ" % bad if bad else "clean" ) )
	return 1 if bad else 0

if __name__ == "__main__":
	sys.exit( main() )
