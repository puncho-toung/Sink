#!/usr/bin/env python3
"""swim.py: mean absolute frame-to-frame difference of a numbered capture series, for 01-VERIFY.md V2.2, V2.4,
V2.10 and V3.3.

A series is a glob pattern (quote it) such as 'screenshots/cr_far_*.png', the names walkTics ... every <name> writes
(<name>_0001.png, <name>_0002.png, ...). Files are ordered by the last number in their name. For each consecutive
pair the mean of |a - b| over every channel of every pixel is taken (0..255 units); the series value is the mean of
those pair values. Prints every pair and the series value.

With --against REFERENCE the reference series is measured the same way and the exit code is 1 when the series value
exceeds the reference's (V2.2: cr_far at cvar 1 must not swim more than cr_origin). With --max N it is 1 when the
series value exceeds N. Exit 2 on a usage or read error.

Usage: python3 -I swim.py 'SERIES*.png' [--against 'REFERENCE*.png'] [--max N] [--quiet]
"""

import argparse
import glob
import importlib.util
import os
import re
import sys

def load_helper():
	path = os.path.join( os.path.dirname( os.path.abspath( __file__ ) ), "verify_image.py" )
	spec = importlib.util.spec_from_file_location( "verify_image", path )
	mod = importlib.util.module_from_spec( spec )
	spec.loader.exec_module( mod )
	return mod

def ordered( pattern ):
	files = glob.glob( pattern )
	def key( path ):
		nums = re.findall( r"\d+", os.path.basename( path ) )
		return ( int( nums[-1] ) if nums else -1, path )
	return sorted( files, key = key )

def series_value( vi, pattern, quiet ):
	files = ordered( pattern )
	if len( files ) < 2:
		raise ValueError( "%s: need at least two captures, found %d" % ( pattern, len( files ) ) )
	values = []
	prev = vi.load( files[0] )
	for path in files[1:]:
		cur = vi.load( path )
		if not vi.same_size( prev, cur ):
			raise ValueError( "%s: size differs from the previous capture" % path )
		v = vi.mean_abs_diff( prev, cur )
		values.append( v )
		if not quiet:
			print( "  %s -> %s: %.5f" % ( os.path.basename( files[len( values ) - 1] ), os.path.basename( path ), v ) )
		prev = cur
	return sum( values ) / len( values ), len( files )

def main():
	p = argparse.ArgumentParser( description = __doc__.split( "\n" )[0] )
	p.add_argument( "series" )
	p.add_argument( "--against", default = None )
	p.add_argument( "--max", type = float, default = None )
	p.add_argument( "--quiet", action = "store_true" )
	args = p.parse_args()

	vi = load_helper()
	try:
		value, n = series_value( vi, args.series, args.quiet )
		print( "swim %s: %d captures, mean frame-to-frame difference %.5f" % ( args.series, n, value ) )
		fail = False
		if args.against is not None:
			ref, rn = series_value( vi, args.against, args.quiet )
			print( "swim %s: %d captures, mean frame-to-frame difference %.5f (reference)" % ( args.against, rn, ref ) )
			fail = fail or value > ref
		if args.max is not None:
			fail = fail or value > args.max
	except ( OSError, ValueError ) as e:
		print( "swim: %s" % e )
		return 2

	if args.against is not None or args.max is not None:
		print( "swim: %s" % ( "FAIL" if fail else "pass" ) )
	return 1 if fail else 0

if __name__ == "__main__":
	sys.exit( main() )
