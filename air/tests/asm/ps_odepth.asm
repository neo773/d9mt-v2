; oDepth: writes fragment depth from an interpolated value
ps_3_0
dcl_texcoord0 v0
def c30, 0.25, 0.5, 0.75, 1.0
mov oC0, c30
mul r0, v0, c30.yyyy
mov oDepth, r0.x
