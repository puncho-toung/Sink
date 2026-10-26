#!/usr/bin/env python3
"""make_fixtures.py: the fixture maps of the camera-relative verification sheet.

plans/camera-relative/00-PLAN.md WP-CRV and 01-VERIFY.md. Writes Doom 3 .map version 2 files (brushDef3, the
format idMapBrush::Write produces and TOOLS/aiharness/gen_map.py and gen_pvb.py write) into --out, by default
rbdoom-cr/base/maps/test beside this tree:

    cr_origin    two areas joined by a visportal; a spot light and a shadowed point light each with an occluder
                 between it and the start, a mirror, a fog light, an env_probe, a floor seam (two floor
                 materials meeting at y = 256) that runs through the visportal, an info_player_start on the seam
    cr_far       cr_origin moved by FAR_OFFSET = ( 122880, 122880, 0 )	# 120 x 1024: dmap splits on a 1024 block grid, and its st fix keeps a side's arithmetic identical when the move is a multiple of 8192 (impl/BUILD-AND-G0.md 11-12): the same description, the same entity
                 order, only every coordinate offset, so the two maps differ by the translation alone
    cr_subviews  a mirror, a direct-portal surface whose func_cameraview sits in a sealed remote room between two
                 marker plates 1 unit either side of its plane, a remoteRenderMap monitor with its own camera, a
                 plain func_fracture window and a func_smoke (an idSmokeParticles emitter, not a func_emitter)
    cr_sky       a portal sky whose info_portalSky is more than 131072 units from the player, plus a func_smoke
    cr_plain     one room, two shadowed lights, a few boxes: no sky, no subview
    rz_zfight    two func_static plates 0.5 units apart at 10000 and at 100000 units from the start, each pair
                 with a polygonOffset decal plate coplanar with the rear plate
    o1_sun       a parallel light 32000 units from a tall pole, light_radius 40000 on each axis
    o1_probe     an env_probe 500 units off its room's centre and a second room with no probe; no .lightgrid

Coordinates are Doom units, z up. A brush entity's brushes are written relative to its "origin", as the editor
does. The maps must be copied to NecroGameBuild/base/maps/test and dmapped (see README.md there); the direct
portal also needs base/materials/cr_verify.mtr. Output is deterministic.

Usage: python3 -I make_fixtures.py [--out DIR]
"""

import argparse
import os

FAR_OFFSET = ( 122880, 122880, 0 )	# 120 x 1024: dmap splits on a 1024 block grid, and its st fix keeps a side's arithmetic identical when the move is a multiple of 8192 (impl/BUILD-AND-G0.md 11-12)

WALL = "textures/base_wall/lfwall1"
WALL_RED = "textures/base_wall/lfwall5_red"
FLOOR_A = "textures/base_floor/conc_panel02"
FLOOR_B = "textures/base_floor/roundfloor1"
CAULK = "textures/common/caulk"
VISPORTAL = "textures/editor/visportal"
MIRROR = "textures/common/mirror"
DIRECT_PORTAL = "textures/cr_verify/direct_portal"		# base/materials/cr_verify.mtr
MONITOR = "shaderDemos/remoteCamera"					# remoteRenderMap
GLASS = "textures/glass/glass1"
PORTAL_SKY = "textures/smf/portal_sky"
DECAL = "textures/decals/schmutz"						# DECAL_MACRO: polygonOffset
SMOKE = "rockettrail.prt"								# a particle decl the projectiles emit through idSmokeParticles
FOG = "fogs/basicFog"
TEXMAT = "( ( 0.0078125 0 0 ) ( 0 0.0078125 0 ) )"
T = 16													# wall thickness

# ---------------------------------------------------------------------------
# writers (after gen_pvb.py, which follows idMapBrush::Write / idMapEntity::Write)
# ---------------------------------------------------------------------------

def fmt( v ):
	if float( v ).is_integer():
		return str( int( v ) + 0 )
	return "%.6f" % v

def vec( v ):
	return " ".join( fmt( c ) for c in v )

def add( a, b ):
	return tuple( a[i] + b[i] for i in range( 3 ) )

def side( plane, material ):
	return "( %s ) %s \"%s\" 0 0 0" % ( vec( plane ), TEXMAT, material )

def box_brush( mins, maxs, material, origin = ( 0, 0, 0 ) ):
	"""An axial box; material is one name or a dict of face ( '+x', '-x', ..., 'default' ) to name."""
	lo = [ mins[i] - origin[i] for i in range( 3 ) ]
	hi = [ maxs[i] - origin[i] for i in range( 3 ) ]
	if any( lo[i] >= hi[i] for i in range( 3 ) ):
		raise ValueError( "degenerate brush %s %s" % ( mins, maxs ) )
	if isinstance( material, str ):
		material = { "default": material }
	def m( face ):
		return material.get( face, material["default"] )
	# outward normal, d = -( n . p ) for a point p on the face
	planes = (
		( ( 0, 0, 1, -hi[2] ), "+z" ),
		( ( 0, 1, 0, -hi[1] ), "+y" ),
		( ( 1, 0, 0, -hi[0] ), "+x" ),
		( ( 0, 0, -1, lo[2] ), "-z" ),
		( ( 0, -1, 0, lo[1] ), "-y" ),
		( ( -1, 0, 0, lo[0] ), "-x" ),
	)
	return [ side( p, m( face ) ) for p, face in planes ]

def hull( mins, maxs, material ):
	"""Six brushes sealing the box interior mins..maxs; material as for box_brush, per hull piece:
	'floor', 'ceiling', '-x', '+x', '-y', '+y' or 'default'. Each piece shows only its inner face."""
	if isinstance( material, str ):
		material = { "default": material }
	def inner( piece, face ):
		return { "default": CAULK, face: material.get( piece, material["default"] ) }
	x0, y0, z0 = mins
	x1, y1, z1 = maxs
	return [
		box_brush( ( x0 - T, y0 - T, z0 - T ), ( x1 + T, y1 + T, z0 ), inner( "floor", "+z" ) ),
		box_brush( ( x0 - T, y0 - T, z1 ), ( x1 + T, y1 + T, z1 + T ), inner( "ceiling", "-z" ) ),
		box_brush( ( x0 - T, y0 - T, z0 ), ( x0, y1 + T, z1 ), inner( "-x", "+x" ) ),
		box_brush( ( x1, y0 - T, z0 ), ( x1 + T, y1 + T, z1 ), inner( "+x", "-x" ) ),
		box_brush( ( x0, y0 - T, z0 ), ( x1, y0, z1 ), inner( "-y", "+y" ) ),
		box_brush( ( x0, y1, z0 ), ( x1, y1 + T, z1 ), inner( "+y", "-y" ) ),
	]

class MapWriter:
	def __init__( self, name, comment ):
		self.out = [ "Version 2", "// %s: generated by rbdoom-cr/neo/tools/verify/make_fixtures.py; regenerate, do not edit" % name ]
		for line in comment.strip().split( "\n" ):
			self.out.append( "// " + line.strip() )
		self.num = 0

	def entity( self, keys, brushes = () ):
		out = self.out
		out.append( "// entity %d" % self.num )
		out.append( "{" )
		for k, v in keys:
			out.append( "\"%s\" \"%s\"" % ( k, v ) )
		for i, sides in enumerate( brushes ):
			out.append( "// primitive %d" % i )
			out.append( "{" )
			out.append( " brushDef3" )
			out.append( " {" )
			for s in sides:
				out.append( "  " + s )
			out.append( " }" )
			out.append( "}" )
		out.append( "}" )
		self.num += 1

	def brush_entity( self, classname, name, mins, maxs, material, extra = () ):
		"""A one-brush entity whose origin is the brush centre."""
		origin = tuple( ( mins[i] + maxs[i] ) / 2.0 for i in range( 3 ) )
		keys = [ ( "classname", classname ), ( "name", name ), ( "model", name ), ( "origin", vec( origin ) ) ] + list( extra )
		self.entity( keys, [ box_brush( mins, maxs, material, origin ) ] )

	def text( self ):
		return "\n".join( self.out ) + "\n"

def player_start( w, origin, yaw ):
	w.entity( [ ( "classname", "info_player_start" ), ( "name", "info_player_start_1" ), ( "origin", vec( origin ) ), ( "angle", fmt( yaw ) ) ] )

def point_light( w, name, origin, radius, shadows = True, color = ( 1, 1, 1 ) ):
	w.entity( [ ( "classname", "light" ), ( "name", name ), ( "origin", vec( origin ) ), ( "light_radius", vec( radius ) ),
		( "noshadows", "0" if shadows else "1" ), ( "_color", vec( color ) ) ] )

# ---------------------------------------------------------------------------
# cr_origin / cr_far: one description, an offset
# ---------------------------------------------------------------------------

CR_ROOM = ( 1024, 512, 256 )		# interior 0..X, 0..Y, 0..Z
CR_WALL_X = ( 504, 520 )			# partition wall between area A (x < 504) and area B (x > 520)
CR_DOOR_Y = ( 192, 320 )			# doorway; the floor seam at y = 256 runs through it
CR_DOOR_Z = 160
CR_SEAM_Y = 256
CR_START = ( 32, 256, 0 )			# on the seam, looking along it (+x) through the visportal

def cr_room( name, o ):
	"""cr_origin with o = ( 0, 0, 0 ), cr_far with o = FAR_OFFSET."""
	X, Y, Z = CR_ROOM
	wx0, wx1 = CR_WALL_X
	dy0, dy1 = CR_DOOR_Y
	P = lambda x, y, z: add( ( x, y, z ), o )
	w = MapWriter( name, """
		cr_origin's room at offset ( %s ): areas A ( x < 504 ) and B ( x > 520 ) joined by a visportal at x = 510..514.
		Start ( %s ) yaw 0 looks along the floor seam y = %d through the visportal: setviewpos %s 0
	""" % ( vec( o ), vec( P( *CR_START ) ), CR_SEAM_Y + o[1], vec( add( P( *CR_START ), ( 0, 0, 68 ) ) ) ) )

	world = [
		# two floor materials meet at y = 256, so dmap cannot merge the seam away
		box_brush( P( -T, -T, -T ), P( X + T, CR_SEAM_Y, 0 ), { "default": CAULK, "+z": FLOOR_A } ),
		box_brush( P( -T, CR_SEAM_Y, -T ), P( X + T, Y + T, 0 ), { "default": CAULK, "+z": FLOOR_B } ),
		box_brush( P( -T, -T, Z ), P( X + T, Y + T, Z + T ), { "default": CAULK, "-z": WALL } ),
		box_brush( P( -T, -T, 0 ), P( 0, Y + T, Z ), { "default": CAULK, "+x": WALL } ),
		box_brush( P( X, -T, 0 ), P( X + T, Y + T, Z ), { "default": CAULK, "-x": WALL } ),
		box_brush( P( 0, -T, 0 ), P( X, 0, Z ), { "default": CAULK, "+y": WALL } ),
		box_brush( P( 0, Y, 0 ), P( X, Y + T, Z ), { "default": CAULK, "-y": WALL } ),
		# partition with its doorway
		box_brush( P( wx0, 0, 0 ), P( wx1, dy0, Z ), WALL ),
		box_brush( P( wx0, dy1, 0 ), P( wx1, Y, Z ), WALL ),
		box_brush( P( wx0, dy0, CR_DOOR_Z ), P( wx1, dy1, Z ), WALL ),
		box_brush( P( 510, dy0, 0 ), P( 514, dy1, CR_DOOR_Z ), VISPORTAL ),
		# occluders between the start and each light of area A
		box_brush( P( 224, 32, 0 ), P( 256, 160, 192 ), WALL ),
		box_brush( P( 320, 352, 0 ), P( 352, 448, 192 ), WALL ),
	]
	w.entity( [ ( "classname", "worldspawn" ) ], world )

	player_start( w, P( *CR_START ), 0 )

	w.entity( [ ( "classname", "light" ), ( "name", "light_spot" ), ( "origin", vec( P( 288, 96, 240 ) ) ),
		( "light_target", "0 0 -240" ), ( "light_up", "0 -128 0" ), ( "light_right", "-128 0 0" ),
		( "noshadows", "0" ), ( "_color", "1 0.9 0.8" ) ] )
	point_light( w, "light_point", P( 384, 400, 160 ), ( 288, 288, 288 ), color = ( 0.8, 0.9, 1 ) )
	point_light( w, "light_b", P( 896, 128, 192 ), ( 384, 384, 256 ) )
	w.entity( [ ( "classname", "light" ), ( "name", "light_fog" ), ( "origin", vec( P( 768, 256, 96 ) ) ),
		( "light_radius", "224 224 96" ), ( "texture", FOG ), ( "noshadows", "1" ), ( "_color", "0.5 0.6 0.7" ) ] )
	w.entity( [ ( "classname", "env_probe" ), ( "name", "env_probe_1" ), ( "origin", vec( P( 768, 384, 128 ) ) ) ] )

	# a mirror on the +y wall of area A, seen at an angle from the start
	w.brush_entity( "func_static", "mirror_1", P( 64, Y - 4, 32 ), P( 224, Y, 192 ), { "default": CAULK, "-y": MIRROR } )
	return w.text()

# ---------------------------------------------------------------------------
# cr_subviews
# ---------------------------------------------------------------------------

SUB_ROOM = ( 1024, 1024, 384 )
SUB_REMOTE = ( ( 3072, 0, 0 ), ( 6144, 1024, 512 ) )	# sealed room the direct portal looks into; deep enough behind the
													# camera to hold the portal view's eye, which sits behind it by the
													# player's distance to the portal surface (R_PortalViewBySurface)
SUB_PORTAL_CAM = ( 4608, 512, 192 )					# faces +x; the portal's oblique plane is x = 4608

def cr_subviews():
	X, Y, Z = SUB_ROOM
	cx, cy, cz = SUB_PORTAL_CAM
	w = MapWriter( "cr_subviews", """
		Mirror and direct portal on the +x wall, remoteRenderMap monitor on the +y wall, func_fracture pane, func_smoke.
		The direct portal looks through func_cameraview cam_portal ( %s ) yaw 0 in a sealed room; marker plates centred
		1 unit before ( x = %s ) and 1 unit beyond ( x = %s ) its plane, 0.5 units thick (V3.5a).
		Start ( 96 512 0 ) yaw 0: setviewpos 96 512 68 0
	""" % ( vec( SUB_PORTAL_CAM ), fmt( cx - 1 ), fmt( cx + 1 ) ) )

	world = hull( ( 0, 0, 0 ), ( X, Y, Z ), { "default": WALL, "floor": FLOOR_A } )
	world += hull( SUB_REMOTE[0], SUB_REMOTE[1], { "default": WALL_RED, "floor": FLOOR_B } )
	world.append( box_brush( ( 5120, 256, 0 ), ( 5376, 768, 256 ), WALL ) )		# something to see behind the plates
	w.entity( [ ( "classname", "worldspawn" ) ], world )

	player_start( w, ( 96, 512, 0 ), 0 )
	point_light( w, "light_1", ( 512, 512, 320 ), ( 640, 640, 384 ) )
	point_light( w, "light_2", ( 256, 832, 256 ), ( 384, 384, 384 ) )
	point_light( w, "light_remote", ( 5120, 512, 448 ), ( 1280, 768, 512 ) )

	w.brush_entity( "func_static", "mirror_1", ( X - 4, 576, 64 ), ( X, 896, 320 ), { "default": CAULK, "-x": MIRROR } )
	w.brush_entity( "func_static", "portal_1", ( X - 4, 128, 64 ), ( X, 448, 320 ), { "default": CAULK, "-x": DIRECT_PORTAL },
		[ ( "cameraTarget", "cam_portal" ) ] )
	w.entity( [ ( "classname", "func_cameraview" ), ( "name", "cam_portal" ), ( "origin", vec( SUB_PORTAL_CAM ) ), ( "angle", "0" ) ] )
	w.brush_entity( "func_static", "marker_before", ( cx - 1.25, cy - 256, cz - 128 ), ( cx - 0.75, cy, cz + 128 ), FLOOR_B )
	w.brush_entity( "func_static", "marker_beyond", ( cx + 0.75, cy, cz - 128 ), ( cx + 1.25, cy + 256, cz + 128 ), WALL )

	# the remote-render monitor (the "security camera" subview) and its camera in the main room
	w.brush_entity( "func_static", "monitor_1", ( 576, Y - 4, 64 ), ( 896, Y, 256 ), { "default": CAULK, "-y": MONITOR },
		[ ( "cameraTarget", "cam_monitor" ) ] )
	w.entity( [ ( "classname", "func_cameraview" ), ( "name", "cam_monitor" ), ( "origin", "128 128 320" ), ( "angle", "45" ) ] )

	# a plain window, not an x-ray surface (GD10)
	w.brush_entity( "func_fracture", "window_1", ( 640, 384, 0 ), ( 644, 640, 192 ), GLASS )
	w.entity( [ ( "classname", "func_smoke" ), ( "name", "smoke_1" ), ( "origin", "512 256 16" ), ( "smoke", SMOKE ) ] )
	return w.text()

# ---------------------------------------------------------------------------
# cr_sky
# ---------------------------------------------------------------------------

SKY_PLAYER_ROOM = ( ( -64512, -512, 0 ), ( -63488, 512, 512 ) )
SKY_START = ( -64384, 0, 0 )
SKY_ROOM = ( ( 79488, -512, 0 ), ( 80512, 512, 1024 ) )
SKY_CAMERA = ( 80000, 0, 512 )

def cr_sky():
	d = ( ( SKY_CAMERA[0] - SKY_START[0] ) ** 2 + ( SKY_CAMERA[1] - SKY_START[1] ) ** 2 + ( SKY_CAMERA[2] - SKY_START[2] - 68 ) ** 2 ) ** 0.5
	assert d > 131072.0
	w = MapWriter( "cr_sky", """
		Player room ceiling and +x wall show the portal sky; info_portalSky ( %s ) is %d units from the start eye.
		Start ( %s ) yaw 0: setviewpos %s 0
	""" % ( vec( SKY_CAMERA ), int( d ), vec( SKY_START ), vec( add( SKY_START, ( 0, 0, 68 ) ) ) ) )

	world = hull( SKY_PLAYER_ROOM[0], SKY_PLAYER_ROOM[1], { "default": WALL, "floor": FLOOR_A, "ceiling": PORTAL_SKY, "+x": PORTAL_SKY } )
	world += hull( SKY_ROOM[0], SKY_ROOM[1], { "default": WALL_RED, "floor": FLOOR_B } )
	sx, sy, sz = SKY_CAMERA
	world.append( box_brush( ( sx + 256, sy - 384, 0 ), ( sx + 384, sy - 128, 640 ), WALL ) )
	world.append( box_brush( ( sx + 128, sy + 128, 0 ), ( sx + 320, sy + 320, 384 ), FLOOR_A ) )
	w.entity( [ ( "classname", "worldspawn" ) ], world )

	player_start( w, SKY_START, 0 )
	point_light( w, "light_1", ( -64000, 0, 384 ), ( 768, 768, 512 ) )
	point_light( w, "light_sky", ( sx, sy, 896 ), ( 1024, 1024, 1024 ) )
	w.entity( [ ( "classname", "info_portalSky" ), ( "name", "portalsky_1" ), ( "origin", vec( SKY_CAMERA ) ), ( "angle", "0" ) ] )
	w.entity( [ ( "classname", "func_smoke" ), ( "name", "smoke_1" ), ( "origin", "-63872 128 16" ), ( "smoke", SMOKE ) ] )
	return w.text()

# ---------------------------------------------------------------------------
# cr_plain
# ---------------------------------------------------------------------------

def cr_plain():
	w = MapWriter( "cr_plain", """
		One room, no sky, no subview (TAA and timing rows).
		Start ( 96 512 0 ) yaw 0: setviewpos 96 512 68 0
	""" )
	world = hull( ( 0, 0, 0 ), ( 1024, 1024, 384 ), { "default": WALL, "floor": FLOOR_A } )
	world.append( box_brush( ( 384, 256, 0 ), ( 512, 384, 128 ), WALL ) )
	world.append( box_brush( ( 640, 576, 0 ), ( 704, 832, 256 ), FLOOR_B ) )
	world.append( box_brush( ( 768, 192, 0 ), ( 896, 320, 64 ), FLOOR_B ) )
	w.entity( [ ( "classname", "worldspawn" ) ], world )
	player_start( w, ( 96, 512, 0 ), 0 )
	point_light( w, "light_1", ( 448, 512, 320 ), ( 640, 640, 384 ) )
	point_light( w, "light_2", ( 832, 768, 256 ), ( 384, 384, 384 ), color = ( 1, 0.85, 0.7 ) )
	return w.text()

# ---------------------------------------------------------------------------
# rz_zfight
# ---------------------------------------------------------------------------

RZ_START = ( -50048, 0, 0 )
RZ_HALL = ( ( -50176, -4608, 0 ), ( 50688, 4608, 4608 ) )

def rz_zfight():
	ex = RZ_START[0]
	w = MapWriter( "rz_zfight", """
		Rear plate A, front plate B 0.5 units nearer and overlapping it, decal plate C coplanar with A ( polygonOffset ),
		at 10000 ( left of the view ) and 100000 ( ahead ) units from the start along +x. The hall is caulk.
		Start ( %s ) yaw 0: setviewpos %s 0
	""" % ( vec( RZ_START ), vec( add( RZ_START, ( 0, 0, 68 ) ) ) ) )
	w.entity( [ ( "classname", "worldspawn" ) ], hull( RZ_HALL[0], RZ_HALL[1], CAULK ) )
	player_start( w, RZ_START, 0 )

	# ( distance, y0, y1, z1, label ): plate A covers y0..y1, z 0..z1
	for dist, y0, y1, z1, label in ( ( 10000, 1024, 2560, 1024, "10k" ), ( 100000, -3072, 1536, 3072, "100k" ) ):
		px = ex + dist					# A's front face
		thick = 16 if dist < 50000 else 64
		ym = ( y0 + y1 ) / 2.0
		h = y1 - y0
		w.brush_entity( "func_static", "plate_a_" + label, ( px, y0, 0 ), ( px + thick, y1, z1 ), WALL )
		w.brush_entity( "func_static", "plate_b_" + label, ( px - 0.5, ym - h / 4.0, z1 / 4.0 ), ( px + thick, y1 + h / 4.0, z1 * 0.75 ), FLOOR_B )
		w.brush_entity( "func_static", "decal_c_" + label, ( px, y0 + h / 16.0, z1 / 8.0 ), ( px + thick / 4.0, ym - h / 4.0 - h / 16.0, z1 / 2.0 ), DECAL )
		lx = px - ( 500 if dist < 50000 else 1000 )
		radius = ( 1024, h, z1 * 1.25 ) if dist < 50000 else ( 2048, h * 1.25, z1 * 1.25 )
		point_light( w, "light_" + label, ( lx, ym, z1 / 2.0 ), radius, shadows = False )
	return w.text()

# ---------------------------------------------------------------------------
# o1_sun
# ---------------------------------------------------------------------------

SUN_HULL = ( ( -36864, -36864, 0 ), ( 36864, 36864, 32768 ) )
SUN_START = ( -30000, 0, 0 )
SUN_POLE = ( ( -128, -128, 0 ), ( 128, 128, 8192 ) )
SUN_ORIGIN = ( 0, -22627, 22627 )			# 32000 from the pole's foot along the sun direction

def o1_sun():
	d = ( SUN_ORIGIN[0] ** 2 + SUN_ORIGIN[1] ** 2 + SUN_ORIGIN[2] ** 2 ) ** 0.5
	w = MapWriter( "o1_sun", """
		Parallel light ( %s ), %d units from the foot of the 8192-unit pole at the origin, light_center 0 -1 1
		( sun to the -y and up, so the pole's shadow falls toward +y ), light_radius 40000 on each axis.
		Floor only; walls and ceiling caulk. Casters at 5k, 15k and 25k from the start; the pole at 30k.
		Start ( %s ) yaw 0: setviewpos %s 0
	""" % ( vec( SUN_ORIGIN ), int( round( d ) ), vec( SUN_START ), vec( add( SUN_START, ( 0, 0, 68 ) ) ) ) )
	world = hull( SUN_HULL[0], SUN_HULL[1], { "default": CAULK, "floor": FLOOR_A } )
	world.append( box_brush( SUN_POLE[0], SUN_POLE[1], WALL ) )
	for x in ( -25000, -15000, -5000 ):
		world.append( box_brush( ( x - 256, 1792, 0 ), ( x + 256, 2304, 512 ), WALL ) )
	w.entity( [ ( "classname", "worldspawn" ) ], world )
	player_start( w, SUN_START, 0 )
	w.entity( [ ( "classname", "light" ), ( "name", "sun" ), ( "origin", vec( SUN_ORIGIN ) ), ( "parallel", "1" ),
		( "light_center", "0 -1 1" ), ( "light_radius", "40000 40000 40000" ), ( "noshadows", "0" ), ( "_color", "1 0.95 0.85" ) ] )
	return w.text()

# ---------------------------------------------------------------------------
# o1_probe
# ---------------------------------------------------------------------------

PROBE_ROOM_A = ( ( 0, 0, 0 ), ( 2048, 1024, 512 ) )
PROBE_WALL_X = ( 2048, 2064 )
PROBE_ROOM_B_X1 = 3088
PROBE_DOOR_Y = ( 448, 576 )
PROBE_DOOR_Z = 192

def o1_probe():
	( ax0, ay0, az0 ), ( ax1, ay1, az1 ) = PROBE_ROOM_A
	centre = ( ( ax0 + ax1 ) / 2.0, ( ay0 + ay1 ) / 2.0, ( az0 + az1 ) / 2.0 )
	probe = add( centre, ( 500, 0, 0 ) )
	wx0, wx1 = PROBE_WALL_X
	dy0, dy1 = PROBE_DOOR_Y
	w = MapWriter( "o1_probe", """
		Room A ( x < 2048 ) holds env_probe_1 at ( %s ), 500 units off the room's centre ( the area bounds also take
		in the doorway up to the visportal, a few units ). Room B ( x > 2064 ) has no probe, so auto placement runs.
		No .lightgrid is generated by this script.
		Start ( 256 512 0 ) yaw 0: setviewpos 256 512 68 0
	""" % vec( probe ) )
	X1 = PROBE_ROOM_B_X1
	world = hull( ( ax0, ay0, az0 ), ( X1, ay1, az1 ), { "default": WALL, "floor": FLOOR_A } )
	world += [
		box_brush( ( wx0, ay0, 0 ), ( wx1, dy0, az1 ), WALL ),
		box_brush( ( wx0, dy1, 0 ), ( wx1, ay1, az1 ), WALL ),
		box_brush( ( wx0, dy0, PROBE_DOOR_Z ), ( wx1, dy1, az1 ), WALL ),
		box_brush( ( 2054, dy0, 0 ), ( 2058, dy1, PROBE_DOOR_Z ), VISPORTAL ),
		box_brush( ( 1280, 768, 0 ), ( 1536, 896, 320 ), WALL ),
	]
	w.entity( [ ( "classname", "worldspawn" ) ], world )
	player_start( w, ( 256, 512, 0 ), 0 )
	point_light( w, "light_a", ( 768, 512, 448 ), ( 1280, 640, 512 ) )
	point_light( w, "light_b", ( 2576, 512, 448 ), ( 640, 640, 512 ) )
	w.entity( [ ( "classname", "env_probe" ), ( "name", "env_probe_1" ), ( "origin", vec( probe ) ) ] )
	return w.text()

# ---------------------------------------------------------------------------

def write_file( path, text ):
	with open( path, "w", encoding = "ascii", newline = "\n" ) as f:
		f.write( text )

def main():
	here = os.path.dirname( os.path.abspath( __file__ ) )
	p = argparse.ArgumentParser( description = __doc__.split( "\n" )[0] )
	p.add_argument( "--out", default = os.path.normpath( os.path.join( here, "..", "..", "..", "base", "maps", "test" ) ) )
	args = p.parse_args()
	os.makedirs( args.out, exist_ok = True )

	maps = (
		( "cr_origin", cr_room( "cr_origin", ( 0, 0, 0 ) ) ),
		( "cr_far", cr_room( "cr_far", FAR_OFFSET ) ),
		( "cr_subviews", cr_subviews() ),
		( "cr_sky", cr_sky() ),
		( "cr_plain", cr_plain() ),
		( "rz_zfight", rz_zfight() ),
		( "o1_sun", o1_sun() ),
		( "o1_probe", o1_probe() ),
	)
	for name, text in maps:
		write_file( os.path.join( args.out, name + ".map" ), text )
		print( "%-12s %6d bytes" % ( name, len( text ) ) )

if __name__ == "__main__":
	main()
