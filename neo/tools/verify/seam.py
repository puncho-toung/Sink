#!/usr/bin/env python3
"""seam.py: count the pixels of a clear colour inside a rectangle of a capture, for 01-VERIFY.md V1.7 (GB6).

Set a solid clear colour (for example r_clear "1 0 1"), look along the floor seam of maps/test/cr_far that crosses
the visportal, capture, and count the clear-colour pixels in a rectangle around the seam; cracks show the clear colour
through. Pixels match when every channel is within --tolerance of --color (0..255 units). The rectangle is
x0,y0,x1,y1 in pixels from the top-left corner, x1 and y1 exclusive.

With --against REFERENCE the reference capture is counted in the same rectangle and the exit code is 1 when the
count exceeds the reference's (the cr_far count must not exceed the cr_origin count). With --max N it is 1 when the
count exceeds N. Exit 2 on a usage or read error.

Usage: python3 -I seam.py CAPTURE.png --color 255,0,255 --rect x0,y0,x1,y1 [--tolerance 0] [--against REF.png] [--max N]
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

def ints( text, n, what ):
	parts = [ int( v ) for v in text.split( "," ) ]
	if len( parts ) != n:
		raise ValueError( "%s needs %d comma-separated integers" % ( what, n ) )
	return parts

def count( vi, path, color, rect, tolerance ):
	img = vi.load( path )
	x0, y0, x1, y1 = rect
	if not ( 0 <= x0 < x1 <= img.width and 0 <= y0 < y1 <= img.height ):
		raise ValueError( "%s: rectangle %s outside %dx%d" % ( path, rect, img.width, img.height ) )
	data = vi.rgb_bytes( img )
	r, g, b = color
	hits = 0
	for y in range( y0, y1 ):
		o = ( y * img.width + x0 ) * 3
		for _ in range( x1 - x0 ):
			if abs( data[o] - r ) <= tolerance and abs( data[o + 1] - g ) <= tolerance and abs( data[o + 2] - b ) <= tolerance:
				hits += 1
			o += 3
	return hits

def main():
	p = argparse.ArgumentParser( description = __doc__.split( "\n" )[0] )
	p.add_argument( "capture" )
	p.add_argument( "--color", required = True, help = "R,G,B in 0..255" )
	p.add_argument( "--rect", required = True, help = "x0,y0,x1,y1 in pixels, x1 and y1 exclusive" )
	p.add_argument( "--tolerance", type = int, default = 0 )
	p.add_argument( "--against", default = None )
	p.add_argument( "--max", type = int, default = None )
	args = p.parse_args()

	vi = load_helper()
	try:
		color = ints( args.color, 3, "--color" )
		rect = ints( args.rect, 4, "--rect" )
		hits = count( vi, args.capture, color, rect, args.tolerance )
		print( "seam %s: %d clear-colour pixels in %s" % ( args.capture, hits, args.rect ) )
		fail = False
		if args.against is not None:
			ref = count( vi, args.against, color, rect, args.tolerance )
			print( "seam %s: %d clear-colour pixels in %s (reference)" % ( args.against, ref, args.rect ) )
			fail = fail or hits > ref
		if args.max is not None:
			fail = fail or hits > args.max
	except ( OSError, ValueError ) as e:
		print( "seam: %s" % e )
		return 2

	if args.against is not None or args.max is not None:
		print( "seam: %s" % ( "FAIL" if fail else "pass" ) )
	return 1 if fail else 0

if __name__ == "__main__":
	sys.exit( main() )
