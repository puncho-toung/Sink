"""verify_image.py: image loading and pixel statistics shared by imgdiff.py, swim.py and seam.py.

The engine's `screenshot` writes 8-bit RGBA PNG through stb_image_write (R_WritePNG, Image_files.cpp), alpha
forced to 255. Pillow is used when it is installed (decoding and the statistics run in C); without it a
standard-library reader handles 8-bit non-interlaced PNG (grey, grey+alpha, RGB, RGBA) and uncompressed or RLE
true-colour TGA, and the statistics run in Python, which is slow on long series but gives the same numbers.

The scripts load this file by path, not by import, so they also work under `python3 -I`, which drops the script's
directory from sys.path.
"""

import struct
import zlib

try:
	from PIL import Image, ImageChops, ImageStat
	HAVE_PIL = True
except ImportError:
	HAVE_PIL = False

class RgbImage:
	"""width, height, rgb (bytes, 3 per pixel, rows top to bottom), pil (a Pillow RGB image or None)."""
	def __init__( self, width, height, rgb, pil = None ):
		self.width = width
		self.height = height
		self.rgb = rgb
		self.pil = pil

def _paeth( a, b, c ):
	p = a + b - c
	pa = abs( p - a )
	pb = abs( p - b )
	pc = abs( p - c )
	if pa <= pb and pa <= pc:
		return a
	if pb <= pc:
		return b
	return c

def _read_png( data, path ):
	if data[:8] != b"\x89PNG\r\n\x1a\n":
		raise ValueError( "%s: not a PNG" % path )
	pos = 8
	idat = []
	width = height = depth = ctype = interlace = None
	while pos < len( data ):
		length, kind = struct.unpack( ">I4s", data[pos:pos + 8] )
		chunk = data[pos + 8:pos + 8 + length]
		pos += 12 + length
		if kind == b"IHDR":
			width, height, depth, ctype, _, _, interlace = struct.unpack( ">IIBBBBB", chunk )
		elif kind == b"IDAT":
			idat.append( chunk )
		elif kind == b"IEND":
			break
	channels = { 0: 1, 2: 3, 4: 2, 6: 4 }.get( ctype )
	if depth != 8 or channels is None or interlace != 0:
		raise ValueError( "%s: only 8-bit non-interlaced grey/RGB(A) PNG is read without Pillow (depth %s, type %s)" % ( path, depth, ctype ) )
	raw = zlib.decompress( b"".join( idat ) )
	stride = width * channels
	out = bytearray( width * height * 3 )
	prev = bytearray( stride )
	src = 0
	for y in range( height ):
		ftype = raw[src]
		row = bytearray( raw[src + 1:src + 1 + stride] )
		src += 1 + stride
		if ftype == 1:
			for i in range( channels, stride ):
				row[i] = ( row[i] + row[i - channels] ) & 0xFF
		elif ftype == 2:
			for i in range( stride ):
				row[i] = ( row[i] + prev[i] ) & 0xFF
		elif ftype == 3:
			for i in range( stride ):
				left = row[i - channels] if i >= channels else 0
				row[i] = ( row[i] + ( ( left + prev[i] ) >> 1 ) ) & 0xFF
		elif ftype == 4:
			for i in range( stride ):
				left = row[i - channels] if i >= channels else 0
				upleft = prev[i - channels] if i >= channels else 0
				row[i] = ( row[i] + _paeth( left, prev[i], upleft ) ) & 0xFF
		elif ftype != 0:
			raise ValueError( "%s: bad PNG filter %d" % ( path, ftype ) )
		o = y * width * 3
		if channels >= 3:
			for x in range( width ):
				s = x * channels
				out[o:o + 3] = row[s:s + 3]
				o += 3
		else:
			for x in range( width ):
				g = row[x * channels]
				out[o] = out[o + 1] = out[o + 2] = g
				o += 3
		prev = row
	return RgbImage( width, height, bytes( out ) )

def _read_tga( data, path ):
	idlen, cmaptype, itype = data[0], data[1], data[2]
	width, height = struct.unpack( "<HH", data[12:16] )
	bpp, desc = data[16], data[17]
	if cmaptype != 0 or itype not in ( 2, 10 ) or bpp not in ( 24, 32 ):
		raise ValueError( "%s: only uncompressed or RLE true-colour TGA is read without Pillow" % path )
	n = bpp // 8
	pos = 18 + idlen
	count = width * height
	pix = bytearray( count * n )
	if itype == 2:
		pix[:] = data[pos:pos + count * n]
	else:
		i = 0
		while i < count:
			h = data[pos]
			pos += 1
			run = ( h & 0x7F ) + 1
			if h & 0x80:
				px = data[pos:pos + n]
				pos += n
				for _ in range( run ):
					pix[i * n:i * n + n] = px
					i += 1
			else:
				pix[i * n:( i + run ) * n] = data[pos:pos + run * n]
				pos += run * n
				i += run
	out = bytearray( count * 3 )
	top_down = bool( desc & 0x20 )
	for y in range( height ):
		sy = y if top_down else height - 1 - y
		for x in range( width ):
			s = ( sy * width + x ) * n
			o = ( y * width + x ) * 3
			out[o] = pix[s + 2]
			out[o + 1] = pix[s + 1]
			out[o + 2] = pix[s]
	return RgbImage( width, height, bytes( out ) )

def load( path ):
	if HAVE_PIL:
		im = Image.open( path ).convert( "RGB" )
		return RgbImage( im.width, im.height, None, im )
	with open( path, "rb" ) as f:
		data = f.read()
	if data[:8] == b"\x89PNG\r\n\x1a\n":
		return _read_png( data, path )
	return _read_tga( data, path )

def rgb_bytes( img ):
	if img.rgb is None:
		img.rgb = img.pil.tobytes()
	return img.rgb

def same_size( a, b ):
	return a.width == b.width and a.height == b.height

def diff_histogram( a, b ):
	"""256 bins: how many pixels have each value of max( |dR|, |dG|, |dB| )."""
	if a.pil is not None and b.pil is not None:
		d = ImageChops.difference( a.pil, b.pil )
		r, g, bl = d.split()
		return ImageChops.lighter( ImageChops.lighter( r, g ), bl ).histogram()
	hist = [ 0 ] * 256
	pa = rgb_bytes( a )
	pb = rgb_bytes( b )
	for i in range( 0, len( pa ), 3 ):
		dr = abs( pa[i] - pb[i] )
		dg = abs( pa[i + 1] - pb[i + 1] )
		db = abs( pa[i + 2] - pb[i + 2] )
		m = dr if dr > dg else dg
		hist[m if m > db else db] += 1
	return hist

def mean_abs_diff( a, b ):
	"""Mean of |a - b| over every channel of every pixel, in 0..255 units."""
	if a.pil is not None and b.pil is not None:
		means = ImageStat.Stat( ImageChops.difference( a.pil, b.pil ) ).mean
		return sum( means ) / len( means )
	pa = rgb_bytes( a )
	pb = rgb_bytes( b )
	return sum( abs( x - y ) for x, y in zip( pa, pb ) ) / float( len( pa ) )
