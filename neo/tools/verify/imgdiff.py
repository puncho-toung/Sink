#!/usr/bin/env python3
"""imgdiff.py: per-pixel difference of two captures (PNG or TGA), for 01-VERIFY.md V1.2b, V2.1 and the rows using
V2.1's threshold.

The per-pixel difference is max( |dR|, |dG|, |dB| ) in 0..255 units. Prints the maximum, the value at the given
percentile (default 99.9) and the count of differing pixels. Exits 1 when the percentile value exceeds --threshold
(default 2, the sheet's "max <= 2/255 on 99.9 percent of pixels"), when --max is given and the maximum exceeds it,
or when the sizes differ; 2 on a usage or read error.

Usage: python3 -I imgdiff.py A.png B.png [--threshold 2] [--percentile 99.9] [--max N]
"""

import argparse
import importlib.util
import os
import sys

def load_helper():
	path = os.path.join( os.path.dirname( os.path.abspath( __file__ ) ), "verify_image.py" )
	spec = importlib.util.spec_from_file_location( "verify_image", path )
	mod = importlib.util.module_from_spec( spec )
	spec.loader.exec_module( mod )
	return mod

def percentile_from_histogram( hist, pct ):
	"""The smallest value v such that at least pct percent of the pixels differ by v or less."""
	total = sum( hist )
	need = total * pct / 100.0
	running = 0
	for v, n in enumerate( hist ):
		running += n
		if running >= need:
			return v
	return len( hist ) - 1

def main():
	p = argparse.ArgumentParser( description = __doc__.split( "\n" )[0] )
	p.add_argument( "a" )
	p.add_argument( "b" )
	p.add_argument( "--threshold", type = int, default = 2, help = "largest allowed value at the percentile, 0..255" )
	p.add_argument( "--percentile", type = float, default = 99.9 )
	p.add_argument( "--max", type = int, default = None, help = "largest allowed maximum, 0..255 (off by default)" )
	args = p.parse_args()

	vi = load_helper()
	try:
		a = vi.load( args.a )
		b = vi.load( args.b )
	except ( OSError, ValueError ) as e:
		print( "imgdiff: %s" % e )
		return 2

	if not vi.same_size( a, b ):
		print( "imgdiff: size differs: %dx%d vs %dx%d" % ( a.width, a.height, b.width, b.height ) )
		return 1

	hist = vi.diff_histogram( a, b )
	total = sum( hist )
	peak = max( v for v, n in enumerate( hist ) if n ) if total else 0
	at_pct = percentile_from_histogram( hist, args.percentile )
	differing = total - hist[0]

	fail = at_pct > args.threshold or ( args.max is not None and peak > args.max )
	print( "imgdiff %s %s: %dx%d max %d p%g %d differing %d (%.4f%%) threshold %d%s -> %s" % (
		args.a, args.b, a.width, a.height, peak, args.percentile, at_pct, differing, 100.0 * differing / max( total, 1 ),
		args.threshold, "" if args.max is None else " max-limit %d" % args.max, "FAIL" if fail else "pass" ) )
	return 1 if fail else 0

if __name__ == "__main__":
	sys.exit( main() )
