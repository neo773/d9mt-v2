; screen-space derivatives
ps_3_0
dcl_texcoord0 v0
dsx r0, v0
dsy r1, v0
add r0, r0, r1
add r0, r0, v0
mov oC0, r0
