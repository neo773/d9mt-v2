; texkill combined with a sample
ps_3_0
dcl_texcoord0 v0
dcl_2d s0
texld r0, v0, s0
sub r1, r0, c0
texkill r1
mov oC0, r0
