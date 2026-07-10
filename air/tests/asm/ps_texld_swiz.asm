; texture sample with swizzled coords
ps_3_0
dcl_texcoord0 v0
dcl_2d s0
mov r1, v0.yxzw
texld r0, r1, s0
mov oC0, r0
